#include "gpu/videoout.h"

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu/gpu.h"
#include "guest.h"
#include "kernel/equeue.h"
#include "kernel/kernel.h"

namespace gr::videoout {
namespace {

using kernel::EventQueue;
using kernel::Load;
using kernel::Ptr;
using kernel::Store;

constexpr uint64_t kEventFlip = 0, kEventVblank = 1;
constexpr int kMaxBuffers = 16;

// SceVideoOutBufferAttribute (40 bytes)
struct BufferAttribute {
    uint32_t pixel_format, tiling_mode, aspect_ratio, width, height, pitch_in_pixel, option, reserved0;
    uint64_t reserved1;
};

struct Pending {
    int32_t index;
    int64_t arg;
};

struct Port {
    std::mutex m;
    std::condition_variable vblank_cv;
    uint64_t buffers[kMaxBuffers] = {};
    BufferAttribute attr[kMaxBuffers] = {};
    std::vector<std::pair<EventQueue*, uint64_t>> flip_events, vblank_events;
    std::deque<Pending> pending;
    int32_t current = -1;
    uint64_t flip_count = 0, vblank_count = 0;
    int64_t last_flip_arg = 0;
    uint64_t last_flip_time = 0;
    uint32_t flip_rate = 0; // 0 = 60 Hz, 1 = 30 Hz, 2 = 20 Hz
    bool open = false;
} g_port;

std::once_flag g_vblank_started;

uint64_t NowUs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(double(c.QuadPart) * 1e6 / double(f.QuadPart));
}

void VblankThread() {
    SetThreadDescription(GetCurrentThread(), L"gr-vblank");
    timeBeginPeriod(1);
    const uint64_t period = 16667;
    uint64_t next = NowUs() + period;
    for (;;) {
        const uint64_t now = NowUs();
        if (now < next) Sleep(DWORD((next - now) / 1000));
        while (NowUs() < next) SwitchToThread();
        next += period;
        std::vector<std::pair<EventQueue*, uint64_t>> flips, vblanks;
        Pending done{};
        bool flipped = false;
        {
            std::lock_guard<std::mutex> lk(g_port.m);
            g_port.vblank_count++;
            const uint32_t divider = g_port.flip_rate + 1;
            if (!g_port.pending.empty() && (g_port.vblank_count % divider) == 0) {
                done = g_port.pending.front();
                g_port.pending.pop_front();
                g_port.current = done.index;
                g_port.flip_count++;
                g_port.last_flip_arg = done.arg;
                g_port.last_flip_time = NowUs();
                flipped = true;
                flips = g_port.flip_events;
            }
            vblanks = g_port.vblank_events;
            g_port.vblank_cv.notify_all();
        }
        for (auto& [eq, udata] : vblanks) eq->Trigger(kEventVblank, kernel::kFilterVideoOut, int64_t(g_port.vblank_count));
        if (flipped)
            for (auto& [eq, udata] : flips) eq->Trigger(kEventFlip, kernel::kFilterVideoOut, done.arg);
    }
}

void StartVblank() {
    std::call_once(g_vblank_started, [] { std::thread(VblankThread).detach(); });
}

} // namespace

void Flip(int32_t buffer_index, int64_t flip_arg) {
    StartVblank();
    std::lock_guard<std::mutex> lk(g_port.m);
    g_port.pending.push_back({buffer_index, flip_arg});
}

FrameInfo Current() {
    std::lock_guard<std::mutex> lk(g_port.m);
    FrameInfo fi;
    fi.flip_count = g_port.flip_count;
    fi.vblank_count = g_port.vblank_count;
    if (g_port.current >= 0 && g_port.current < kMaxBuffers) {
        const auto& a = g_port.attr[g_port.current];
        fi.addr = g_port.buffers[g_port.current];
        fi.width = a.width;
        fi.height = a.height;
        fi.pitch = a.pitch_in_pixel;
        fi.pixel_format = a.pixel_format;
        fi.tiling = a.tiling_mode;
    }
    return fi;
}

FrameInfo Buffer(int32_t index) {
    std::lock_guard<std::mutex> lk(g_port.m);
    FrameInfo fi;
    fi.flip_count = g_port.flip_count;
    fi.vblank_count = g_port.vblank_count;
    if (index >= 0 && index < kMaxBuffers) {
        const auto& a = g_port.attr[index];
        fi.addr = g_port.buffers[index];
        fi.width = a.width;
        fi.height = a.height;
        fi.pitch = a.pitch_in_pixel;
        fi.pixel_format = a.pixel_format;
        fi.tiling = a.tiling_mode;
    }
    return fi;
}

void Register(Hle& h) {
    // int sceVideoOutOpen(SceUserServiceUserId, int busType, int index, const void* param)
    h.Register("sceVideoOutOpen", [](const uint64_t*) -> uint64_t {
        g_port.open = true;
        StartVblank();
        gpu::Start();
        return 1;
    });
    h.Register("sceVideoOutClose", [](const uint64_t*) -> uint64_t { return 0; });
    // void sceVideoOutSetBufferAttribute(attr*, pixelFormat, tilingMode, aspectRatio, width, height, pitchInPixel)
    h.Register("sceVideoOutSetBufferAttribute", [](const uint64_t* a) -> uint64_t {
        BufferAttribute at{uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]), uint32_t(a[4]), uint32_t(a[5]), uint32_t(a[6]),
                           0, 0, 0};
        Store(a[0], at);
        return 0;
    });
    // int sceVideoOutRegisterBuffers(handle, int startIndex, void* const* addresses, int num, const attr*)
    h.Register("sceVideoOutRegisterBuffers", [](const uint64_t* a) -> uint64_t {
        const int start = int(a[1]), num = int(a[3]);
        if (start < 0 || num <= 0 || start + num > kMaxBuffers) return kernel::SceError(kernel::kEINVAL);
        const auto at = Load<BufferAttribute>(a[4]);
        std::lock_guard<std::mutex> lk(g_port.m);
        for (int i = 0; i < num; ++i) {
            g_port.buffers[start + i] = Load<uint64_t>(a[2] + 8ull * i);
            g_port.attr[start + i] = at;
        }
        TraceNote("videoout: %d buffers registered from index %d: %ux%u pitch %u format 0x%x tiling %u", num, start,
                  at.width, at.height, at.pitch_in_pixel, at.pixel_format, at.tiling_mode);
        return 0;
    });
    h.Register("sceVideoOutUnregisterBuffers", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceVideoOutSetFlipRate", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_port.m);
        g_port.flip_rate = uint32_t(a[1]) > 2 ? 2 : uint32_t(a[1]);
        return 0;
    });
    // int sceVideoOutAddFlipEvent(SceKernelEqueue eq, int handle, void* udata)
    h.Register("sceVideoOutAddFlipEvent", [](const uint64_t* a) -> uint64_t {
        auto* eq = Ptr<EventQueue>(a[0]);
        if (!eq) return kernel::SceError(kernel::kEBADF);
        eq->Add(kEventFlip, kernel::kFilterVideoOut, a[2], true);
        std::lock_guard<std::mutex> lk(g_port.m);
        g_port.flip_events.push_back({eq, a[2]});
        return 0;
    });
    h.Register("sceVideoOutAddVblankEvent", [](const uint64_t* a) -> uint64_t {
        auto* eq = Ptr<EventQueue>(a[0]);
        if (!eq) return kernel::SceError(kernel::kEBADF);
        eq->Add(kEventVblank, kernel::kFilterVideoOut, a[2], true);
        std::lock_guard<std::mutex> lk(g_port.m);
        g_port.vblank_events.push_back({eq, a[2]});
        return 0;
    });
    // int sceVideoOutSubmitFlip(handle, int bufferIndex, int flipMode, int64 flipArg)
    h.Register("sceVideoOutSubmitFlip", [](const uint64_t* a) -> uint64_t {
        gpu::SubmitFlip(int32_t(a[1]), int64_t(a[3]));
        return 0;
    });
    // SceVideoOutFlipStatus { u64 count, processTime, tsc; i64 flipArg; u64 submitTsc, reserved0;
    //                         i32 gcQueueNum, flipPendingNum, currentBuffer; u32 reserved1; }
    h.Register("sceVideoOutGetFlipStatus", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_port.m);
        uint8_t s[64] = {};
        const uint64_t tsc = gpu::GpuTimestamp();
        const int32_t pending = int32_t(g_port.pending.size()), cur = g_port.current;
        std::memcpy(s + 0, &g_port.flip_count, 8);
        std::memcpy(s + 8, &g_port.last_flip_time, 8);
        std::memcpy(s + 16, &tsc, 8);
        std::memcpy(s + 24, &g_port.last_flip_arg, 8);
        std::memcpy(s + 52, &pending, 4);
        std::memcpy(s + 56, &cur, 4);
        std::memcpy(Ptr<void>(a[1]), s, sizeof s);
        return 0;
    });
    h.Register("sceVideoOutIsFlipPending", [](const uint64_t*) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_port.m);
        return g_port.pending.size();
    });
    h.Register("sceVideoOutWaitVblank", [](const uint64_t*) -> uint64_t {
        std::unique_lock<std::mutex> lk(g_port.m);
        const uint64_t v = g_port.vblank_count;
        g_port.vblank_cv.wait(lk, [&] { return g_port.vblank_count != v; });
        return 0;
    });
    // SceVideoOutVblankStatus { u64 count; u64 processTime; u64 tsc; u64 reserved; u8 flags; }
    h.Register("sceVideoOutGetVblankStatus", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_port.m);
        uint8_t s[40] = {};
        const uint64_t now = NowUs(), tsc = gpu::GpuTimestamp();
        std::memcpy(s, &g_port.vblank_count, 8);
        std::memcpy(s + 8, &now, 8);
        std::memcpy(s + 16, &tsc, 8);
        std::memcpy(Ptr<void>(a[1]), s, sizeof s);
        return 0;
    });
    // SceVideoOutResolutionStatus { i32 fullWidth, fullHeight, paneWidth, paneHeight; u64 refreshRate;
    //                               float screenSizeInInch; u16 flags; ... }
    h.Register("sceVideoOutGetResolutionStatus", [](const uint64_t* a) -> uint64_t {
        uint8_t s[48] = {};
        const int32_t w = 1920, hgt = 1080;
        const uint64_t rate = 3; // SCE_VIDEO_OUT_REFRESH_RATE_59_94HZ
        const float inch = 50.f;
        std::memcpy(s + 0, &w, 4);
        std::memcpy(s + 4, &hgt, 4);
        std::memcpy(s + 8, &w, 4);
        std::memcpy(s + 12, &hgt, 4);
        std::memcpy(s + 16, &rate, 8);
        std::memcpy(s + 24, &inch, 4);
        std::memcpy(Ptr<void>(a[1]), s, sizeof s);
        return 0;
    });
}

} // namespace gr::videoout
