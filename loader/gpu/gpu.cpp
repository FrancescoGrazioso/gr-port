#include "gpu.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "guest.h"
#include "gpu/capture.h"
#include "gpu/renderer.h"
#include "gpu/pm4.h"
#include "gpu/videoout.h"
#include "kernel/equeue.h"

namespace gr::gpu {
namespace {

constexpr int16_t kEopFilter = kernel::kFilterGraphics;

struct Work {
    enum Kind { kGraphics, kCompute } kind = kGraphics;
    Submission sub;              // kGraphics
    uint64_t ring = 0;           // kCompute: ring base
    uint32_t from = 0, to = 0, ring_dw = 0;
    uint64_t read_ptr_addr = 0;
};

struct Queue {
    std::mutex m;
    std::condition_variable cv, idle_cv;
    std::deque<Work> work;
    bool busy = false;
    std::thread thread;
};

Queue g_gfx, g_compute;
std::once_flag g_started;

struct EopListener {
    kernel::EventQueue* eq;
    uint64_t id;
    uint64_t udata;
};
std::mutex g_eop_m;
std::vector<EopListener> g_eop;

struct ComputeRing {
    uint64_t base = 0;
    uint32_t size_dw = 0;
    uint64_t read_ptr_addr = 0;
    uint32_t last = 0;
};
std::mutex g_ring_m;
std::vector<ComputeRing> g_rings;

// statistics
std::array<std::atomic<uint64_t>, 256> g_op_count{};
std::array<std::atomic<uint64_t>, 64> g_event_types{};
std::atomic<uint64_t> g_submits{0}, g_flips{0}, g_eops{0}, g_wait_timeouts{0}, g_bad_packets{0}, g_faults{0},
    g_draws{0}, g_dispatches{0}, g_compute_ring_dispatches{0};
thread_local bool t_gfx_thread = false; // the renderer only runs on the graphics ring's thread

// Shadow register files (filled by SET_*_REG; consumed by the draw translator in phase 1.4).
uint32_t g_ctx_regs[0x400], g_sh_regs[0x400], g_uconfig_regs[0x400];
uint64_t g_index_base = 0;
uint32_t g_index_type = 0, g_num_instances = 1;

void SignalEop() {
    g_eops++;
    std::lock_guard<std::mutex> lk(g_eop_m);
    for (auto& l : g_eop) l.eq->Trigger(l.id, kEopFilter, 0);
}

uint64_t Addr48(uint32_t lo, uint32_t hi) { return uint64_t(lo) | (uint64_t(hi & 0xFFFF) << 32); }

void WriteData(uint64_t addr, uint32_t data_sel, uint32_t lo, uint32_t hi) {
    if (!addr) return;
    switch (data_sel) {
    case 1: *reinterpret_cast<uint32_t*>(addr) = lo; break;
    case 2: *reinterpret_cast<uint64_t*>(addr) = uint64_t(lo) | (uint64_t(hi) << 32); break;
    case 3:
    case 4: *reinterpret_cast<uint64_t*>(addr) = GpuTimestamp(); break;
    default: break;
    }
}

// Constant engine (CE): runs the constant command buffer (CCB) that goes with each draw command
// buffer. It fills a 48 KiB constant RAM (WRITE_CONST_RAM / LOAD_CONST_RAM) and copies pieces of it
// to memory (DUMP_CONST_RAM): that is where many descriptor tables the shaders read come from.
// CE and DE (draw engine) stay in step through counters: the DE waits with WAIT_ON_CE_COUNTER until
// the CE has produced the next dump, the CE waits with WAIT_ON_DE_COUNTER_DIFF until the DE caught
// up. Same scheduling as shadPS4: the CE runs until it blocks; the DE resumes it when it waits.
struct ConstantEngine {
    std::array<uint8_t, 48 * 1024> ram{};
    uint32_t ce_count = 0, de_count = 0;
    struct Segment { const uint32_t* p; const uint32_t* end; };
    std::vector<Segment> stack; // CCB plus nested IB_CONST
    uint64_t packets = 0, dumps = 0;

    bool Done() const { return stack.empty(); }
    void Start(const uint32_t* ccb, uint32_t n_dw) {
        ce_count = de_count = 0;
        stack.clear();
        if (ccb && n_dw) stack.push_back({ccb, ccb + n_dw});
    }
    // Runs until the CE blocks on the DE or runs out of commands. Returns false if it made no progress.
    bool Run() {
        bool progress = false;
        while (!stack.empty()) {
            Segment& s = stack.back();
            if (s.p >= s.end) { stack.pop_back(); continue; }
            const uint32_t h = *s.p;
            if (Pm4Type(h) == 2) { ++s.p; continue; }
            if (Pm4Type(h) != 3) { stack.clear(); break; }
            const uint32_t body = Pm4Count(h) + 1;
            const uint32_t* b = s.p + 1;
            if (b + body > s.end) { stack.clear(); break; }
            const uint32_t op = Pm4Opcode(h);
            if (op == kWaitOnDeCounterDiff && (de_count - ce_count) >= b[0]) break; // blocked
            s.p = b + body; // consume before a nested buffer is pushed
            ++packets;
            progress = true;
            switch (op) {
            case kWriteConstRam: { // b[0] = byte offset, b[1..] = data
                const uint32_t off = b[0] & 0xFFFF, bytes = (body - 1) * 4;
                if (off + bytes <= ram.size()) std::memcpy(ram.data() + off, b + 1, bytes);
                break;
            }
            case kLoadConstRam: { // b[0..1] = address, b[2] = dwords, b[3] = byte offset
                const auto* src = reinterpret_cast<const uint8_t*>(uint64_t(b[0]) | (uint64_t(b[1] & 0xFFFF) << 32));
                const uint32_t bytes = (b[2] & 0x7FFF) * 4, off = b[3] & 0xFFFF;
                if (src && off + bytes <= ram.size()) std::memcpy(ram.data() + off, src, bytes);
                break;
            }
            case kDumpConstRam: { // b[0] = byte offset, b[1] = dwords, b[2..3] = address
                auto* dst = reinterpret_cast<uint8_t*>(uint64_t(b[2]) | (uint64_t(b[3]) << 32));
                const uint32_t off = b[0] & 0xFFFF, bytes = (b[1] & 0x7FFF) * 4;
                if (dst && off + bytes <= ram.size()) std::memcpy(dst, ram.data() + off, bytes);
                ++dumps;
                break;
            }
            case kIncrementCeCounter: ++ce_count; break;
            case kIndirectBufferConst: {
                const auto* ib = reinterpret_cast<const uint32_t*>(uint64_t(b[0] & ~3u) | (uint64_t(b[1] & 0xFFFF) << 32));
                if (ib && stack.size() < 8) stack.push_back({ib, ib + (b[2] & 0xFFFFF)});
                break;
            }
            default: break;
            }
        }
        return progress;
    }
};
ConstantEngine g_ce; // graphics thread only

bool Compare(uint32_t func, uint32_t v, uint32_t ref) {
    switch (func) {
    case 0: return true;
    case 1: return v < ref;
    case 2: return v <= ref;
    case 3: return v == ref;
    case 4: return v != ref;
    case 5: return v >= ref;
    case 6: return v > ref;
    default: return true;
    }
}

void SetRegs(uint32_t* file, uint32_t base, const uint32_t* body, uint32_t n) {
    // body[0] = register offset relative to the file base, body[1..] = values
    const uint32_t off = body[0] & 0xFFFF;
    for (uint32_t i = 1; i < n; ++i) {
        const uint32_t r = off + i - 1;
        if (r < 0x400) file[r] = body[i];
    }
    (void)base;
}

void ProcessBuffer(const uint32_t* cmd, uint32_t n_dw, int depth);

void Draw(const DrawInfo& d) {
    CaptureDraw(d, g_ctx_regs, g_sh_regs, g_uconfig_regs);
    RendererDraw(d, g_ctx_regs, g_sh_regs, g_uconfig_regs);
}

void ExecuteType3(uint32_t op, const uint32_t* b, uint32_t n, int depth) {
    switch (op) {
    case kNop:
        // debug markers (sceGnmInsertPush/Set/PopMarker) are logged in the traced submission
        if (n >= 3 && b[0] == kMarkerTag && b[1] == uint32_t(Marker::kDebugMarker) && t_gfx_thread)
            RendererMarker(b[2], n >= 4 ? reinterpret_cast<const char*>(b + 3) : "");
        break;
    case kEventWrite: { // b[0] = event type | index << 8, optional b[1..2] = address
        const uint32_t type = b[0] & 0x3F;
        g_event_types[type]++;
        static std::atomic<int> logged{0};
        if (n >= 3 && logged++ < 8)
            TraceNote("gpu: EVENT_WRITE type 0x%x index %u addr 0x%llx", type, (b[0] >> 8) & 0xF,
                      (unsigned long long)(uint64_t(b[1]) | (uint64_t(b[2] & 0xFFFF) << 32)));
        break;
    }
    case kEventWriteEop: { // b[0]=event_cntl b[1]=addr_lo b[2]=addr_hi|int_sel|data_sel b[3..4]=data
        if (n < 5) break;
        const uint32_t data_sel = b[2] >> 29, int_sel = (b[2] >> 24) & 3;
        WriteData(Addr48(b[1], b[2]), data_sel, b[3], b[4]);
        if (int_sel) SignalEop();
        break;
    }
    case kReleaseMem: { // b[0]=event_cntl b[1]=sel bits b[2]=addr_lo b[3]=addr_hi b[4..5]=data
        if (n < 6) break;
        const uint32_t data_sel = b[1] >> 29, int_sel = (b[1] >> 24) & 3;
        WriteData(uint64_t(b[2]) | (uint64_t(b[3]) << 32), data_sel, b[4], b[5]);
        if (int_sel) SignalEop();
        break;
    }
    case kEventWriteEos: { // b[0]=event_cntl b[1]=addr_lo b[2]=addr_hi|command b[3]=data
        if (n < 4) break;
        if ((b[2] >> 29) == 2) {
            const uint64_t a = Addr48(b[1], b[2]);
            if (a) *reinterpret_cast<uint32_t*>(a) = b[3];
        }
        break;
    }
    case kWriteData: { // b[0]=control b[1]=addr_lo b[2]=addr_hi b[3..]=data
        if (n < 4) break;
        const uint32_t dst_sel = (b[0] >> 8) & 0xF;
        const bool one_addr = b[0] & (1u << 16);
        if (dst_sel == 0) break; // register write
        auto* dst = reinterpret_cast<uint32_t*>(uint64_t(b[1]) | (uint64_t(b[2]) << 32));
        if (!dst) break;
        for (uint32_t i = 3; i < n; ++i) {
            *dst = b[i];
            if (!one_addr) ++dst;
        }
        break;
    }
    case kWaitRegMem: { // b[0]=func|mem_space b[1]=addr_lo b[2]=addr_hi b[3]=ref b[4]=mask b[5]=interval
        if (n < 6) break;
        if (!(b[0] & 0x10)) break; // register poll: nothing to wait for on the host
        const auto* p = reinterpret_cast<volatile uint32_t*>(uint64_t(b[1] & ~3u) | (uint64_t(b[2] & 0xFFFF) << 32));
        if (!p) break;
        const uint32_t func = b[0] & 7, ref = b[3], mask = b[4];
        const ULONGLONG start = GetTickCount64();
        while (!Compare(func, *p & mask, ref)) {
            if (GetTickCount64() - start > 5000) {
                if (g_wait_timeouts++ < 8)
                    TraceNote("gpu: WAIT_REG_MEM timeout at 0x%llx (value 0x%x mask 0x%x func %u ref 0x%x)",
                              (unsigned long long)uint64_t(reinterpret_cast<uintptr_t>(p)), *p, mask, func, ref);
                break;
            }
            SwitchToThread();
        }
        break;
    }
    case kDmaData: { // b[0]=control b[1]=src_lo b[2]=src_hi b[3]=dst_lo b[4]=dst_hi b[5]=byte_count|flags
        if (n < 6) break;
        const uint32_t src_sel = (b[0] >> 29) & 3, dst_sel = (b[0] >> 20) & 3;
        const uint32_t bytes = b[5] & 0x1FFFFF;
        auto* dst = reinterpret_cast<uint8_t*>(uint64_t(b[3]) | (uint64_t(b[4]) << 32));
        { // Debug (GR_DMA_WATCH=<hex addr>): DMA_DATA packets whose destination covers that address
            static const uint64_t dma_watch = [] { const char* e = std::getenv("GR_DMA_WATCH"); return e ? std::strtoull(e, nullptr, 16) : 0ull; }();
            static std::atomic<int> logged{0};
            const uint64_t d0 = reinterpret_cast<uint64_t>(dst);
            if (dma_watch && d0 + bytes > dma_watch && d0 < dma_watch + 0x400000 && logged++ < 40) {
                const uint64_t s0 = uint64_t(b[1]) | (uint64_t(b[2]) << 32);
                uint32_t nz = 0;
                if (src_sel == 0 && s0)
                    for (uint32_t i = 0; i + 4 <= bytes && i < (1u << 22); i += 4)
                        nz += reinterpret_cast<const uint32_t*>(s0)[i / 4] != 0;
                TraceNote("dma: ctl %08x src_sel %u dst_sel %u src 0x%llx dst 0x%llx bytes 0x%x flags %08x (source nonzero dwords %u)",
                          b[0], src_sel, dst_sel, (unsigned long long)s0, (unsigned long long)d0, bytes, b[5], nz);
            }
        }
        if (dst_sel != 0 || !dst || !bytes) break;
        if (src_sel == 0) {
            const auto* src = reinterpret_cast<const uint8_t*>(uint64_t(b[1]) | (uint64_t(b[2]) << 32));
            if (src) std::memmove(dst, src, bytes);
        } else if (src_sel == 2) {
            for (uint32_t i = 0; i + 4 <= bytes; i += 4) std::memcpy(dst + i, &b[1], 4);
        }
        break;
    }
    case kIndirectBuffer:
    case kIndirectBufferConst: {
        if (n < 3 || depth > 4) break;
        const auto* ib = reinterpret_cast<const uint32_t*>(uint64_t(b[0] & ~3u) | (uint64_t(b[1] & 0xFFFF) << 32));
        if (op == kIndirectBuffer && ib) ProcessBuffer(ib, b[2] & 0xFFFFF, depth + 1);
        break;
    }
    case kSetContextReg: SetRegs(g_ctx_regs, kContextRegBase, b, n); break;
    case kSetShReg: SetRegs(g_sh_regs, kShRegBase, b, n); break;
    case kSetUconfigReg: SetRegs(g_uconfig_regs, kUconfigRegBase, b, n); break;
    case kIndexBase: g_index_base = uint64_t(b[0]) | (uint64_t(b[1] & 0xFFFF) << 32); break;
    case kIndexType: g_index_type = b[0] & 3; break;
    case kNumInstances: g_num_instances = b[0] ? b[0] : 1; break;
    case kDrawIndexAuto: { // b[0] = vertex count, b[1] = draw initiator
        g_draws++;
        Draw({op, b[0], 0, 0, g_num_instances});
        break;
    }
    case kDrawIndex2: { // b[0] max size, b[1..2] index base, b[3] count, b[4] initiator
        g_draws++;
        if (n >= 4) Draw({op, b[3], uint64_t(b[1]) | (uint64_t(b[2] & 0xFFFF) << 32), g_index_type, g_num_instances});
        break;
    }
    case kDrawIndexOffset2: { // b[0] max size, b[1] index offset, b[2] count, b[3] initiator
        g_draws++;
        if (n >= 3) {
            const uint64_t elem = g_index_type == 1 ? 4 : 2;
            Draw({op, b[2], g_index_base + uint64_t(b[1]) * elem, g_index_type, g_num_instances});
        }
        break;
    }
    case kDrawIndirect:
    case kDrawIndexIndirect:
    case kDrawIndexMultiAuto:
    case kDrawIndirectMulti:
    case kDrawIndexIndirectMulti: g_draws++; break;
    case kDispatchDirect:
        g_dispatches++;
        if (n >= 3) {
            CaptureDispatch(b[0], b[1], b[2], g_sh_regs);
            if (t_gfx_thread) RendererDispatch(b[0], b[1], b[2], g_ctx_regs, g_sh_regs, g_uconfig_regs);
            else g_compute_ring_dispatches++;
        }
        break;
    case kDispatchIndirect: g_dispatches++; break;
    case kIncrementDeCounter:
        if (t_gfx_thread) g_ce.de_count++;
        break;
    case kWaitOnCeCounter: // the DE needs the CE's next dump: let the CE run until it produced it
        if (t_gfx_thread)
            while (g_ce.ce_count <= g_ce.de_count && !g_ce.Done())
                if (!g_ce.Run()) break;
        break;
    default: break;
    }
}

void ProcessBuffer(const uint32_t* cmd, uint32_t n_dw, int depth) {
    uint32_t i = 0;
    while (i < n_dw) {
        const uint32_t h = cmd[i];
        switch (Pm4Type(h)) {
        case 3: {
            const uint32_t body = Pm4Count(h) + 1;
            if (i + 1 + body > n_dw) { g_bad_packets++; return; }
            const uint32_t op = Pm4Opcode(h);
            g_op_count[op]++;
            ExecuteType3(op, cmd + i + 1, body, depth);
            i += 1 + body;
            break;
        }
        case 2: i += 1; break; // filler
        case 0: i += 2 + Pm4Count(h); break;
        default:
            if (g_bad_packets++ < 8)
                TraceNote("gpu: invalid PM4 header 0x%08x at dword %u of %u", h, i, n_dw);
            return;
        }
    }
}

// SEH wrapper: a bad pointer inside a command buffer must not take the whole process down.
void SafeProcess(const uint32_t* cmd, uint32_t n_dw) {
    SetCrashHandlerBypass(true);
    __try {
        ProcessBuffer(cmd, n_dw, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_faults++ < 8) TraceNote("gpu: fault while processing command buffer at 0x%p", cmd);
    }
    SetCrashHandlerBypass(false);
}

template <typename F>
void SafeRun(F f) {
    SetCrashHandlerBypass(true);
    __try {
        f();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_faults++ < 8) TraceNote("gpu: fault while processing a constant command buffer");
        g_ce.stack.clear();
    }
    SetCrashHandlerBypass(false);
}

void ProcessWork(Work& w) {
    if (w.kind == Work::kGraphics) {
        const uint64_t index = g_submits++;
        CaptureBeginSubmission(index);
        RendererBeginSubmission(index);
        for (size_t i = 0; i < w.sub.dcbs.size(); ++i) {
            const auto& cb = w.sub.dcbs[i];
            // the constant command buffer paired with this draw command buffer runs on the CE
            const CommandBuffer ccb = i < w.sub.ccbs.size() ? w.sub.ccbs[i] : CommandBuffer{0, 0};
            g_ce.Start(reinterpret_cast<const uint32_t*>(ccb.addr), ccb.addr ? ccb.size_bytes / 4 : 0);
            SafeRun([] { g_ce.Run(); });
            if (cb.addr && cb.size_bytes) SafeProcess(reinterpret_cast<const uint32_t*>(cb.addr), cb.size_bytes / 4);
            SafeRun([] { while (!g_ce.Done() && g_ce.Run()) {} }); // leftover CE work
        }
        RendererEndSubmission();
        if (w.sub.flip) {
            g_flips++;
            RendererFlip(int32_t(w.sub.buffer_index));
            videoout::Flip(w.sub.buffer_index, w.sub.flip_arg);
        }
    } else {
        const auto* ring = reinterpret_cast<const uint32_t*>(w.ring);
        if (w.to >= w.from) {
            SafeProcess(ring + w.from, w.to - w.from);
        } else {
            SafeProcess(ring + w.from, w.ring_dw - w.from);
            SafeProcess(ring, w.to);
        }
        if (w.read_ptr_addr) *reinterpret_cast<volatile uint32_t*>(w.read_ptr_addr) = w.to;
    }
}

void Worker(Queue* q, const char* name) {
    SetThreadDescription(GetCurrentThread(), name[0] == 'g' ? L"gr-gpu-gfx" : L"gr-gpu-compute");
    t_gfx_thread = name[0] == 'g';
    for (;;) {
        Work w;
        {
            std::unique_lock<std::mutex> lk(q->m);
            q->busy = false;
            q->idle_cv.notify_all();
            q->cv.wait(lk, [&] { return !q->work.empty(); });
            w = std::move(q->work.front());
            q->work.pop_front();
            q->busy = true;
        }
        ProcessWork(w);
    }
}

void Push(Queue& q, Work w) {
    {
        std::lock_guard<std::mutex> lk(q.m);
        q.work.push_back(std::move(w));
    }
    q.cv.notify_one();
}

} // namespace

void Start() {
    std::call_once(g_started, [] {
        g_gfx.thread = std::thread(Worker, &g_gfx, "gfx");
        g_compute.thread = std::thread(Worker, &g_compute, "compute");
        g_gfx.thread.detach();
        g_compute.thread.detach();
    });
}

void Submit(Submission s) {
    Start();
    Work w;
    w.sub = std::move(s);
    Push(g_gfx, std::move(w));
}

void SubmitFlip(int32_t buffer_index, int64_t flip_arg) {
    Submission s;
    s.flip = true;
    s.buffer_index = uint32_t(buffer_index);
    s.flip_arg = flip_arg;
    Submit(std::move(s));
}

void WaitIdle() {
    std::unique_lock<std::mutex> lk(g_gfx.m);
    g_gfx.idle_cv.wait(lk, [] { return g_gfx.work.empty() && !g_gfx.busy; });
}

void AddEopEvent(kernel::EventQueue* eq, uint64_t id, uint64_t udata) {
    eq->Add(id, kEopFilter, udata, true);
    std::lock_guard<std::mutex> lk(g_eop_m);
    g_eop.push_back({eq, id, udata});
}

void RemoveEopEvent(kernel::EventQueue* eq, uint64_t id) {
    eq->Remove(id, kEopFilter);
    std::lock_guard<std::mutex> lk(g_eop_m);
    g_eop.erase(std::remove_if(g_eop.begin(), g_eop.end(), [&](const EopListener& l) { return l.eq == eq && l.id == id; }),
                g_eop.end());
}

uint32_t MapComputeQueue(uint32_t pipe, uint32_t queue, uint64_t ring_base, uint32_t ring_size_dw,
                         uint64_t read_ptr_addr) {
    std::lock_guard<std::mutex> lk(g_ring_m);
    g_rings.push_back({ring_base, ring_size_dw, read_ptr_addr, 0});
    TraceNote("gpu: compute queue %zu mapped (pipe %u queue %u ring 0x%llx, %u dwords)", g_rings.size(), pipe, queue,
              (unsigned long long)ring_base, ring_size_dw);
    return uint32_t(g_rings.size()); // vqid, 1-based
}

void DingDong(uint32_t vqid, uint32_t next_offset_dw) {
    Start();
    Work w;
    w.kind = Work::kCompute;
    {
        std::lock_guard<std::mutex> lk(g_ring_m);
        if (vqid == 0 || vqid > g_rings.size()) return;
        ComputeRing& r = g_rings[vqid - 1];
        w.ring = r.base;
        w.ring_dw = r.size_dw;
        w.from = r.last;
        w.to = next_offset_dw % (r.size_dw ? r.size_dw : 1);
        w.read_ptr_addr = r.read_ptr_addr;
        r.last = w.to;
    }
    Push(g_compute, std::move(w));
}

uint64_t GpuTimestamp() {
    static const double scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 100e6 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(double(c.QuadPart) * scale);
}

std::string StatsSummary() {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "gpu: %llu submits, %llu flips, %llu EOP interrupts, %llu draws, %llu dispatches, "
                  "%llu wait timeouts, %llu bad packets, %llu faults",
                  (unsigned long long)g_submits.load(), (unsigned long long)g_flips.load(),
                  (unsigned long long)g_eops.load(), (unsigned long long)g_draws.load(),
                  (unsigned long long)g_dispatches.load(), (unsigned long long)g_wait_timeouts.load(),
                  (unsigned long long)g_bad_packets.load(), (unsigned long long)g_faults.load());
    std::string s = buf;
    std::snprintf(buf, sizeof buf, ", %llu dispatches on compute rings (not rendered)",
                  (unsigned long long)g_compute_ring_dispatches.load());
    s += buf;
    std::vector<std::pair<uint64_t, uint32_t>> ops;
    for (uint32_t i = 0; i < 256; ++i)
        if (g_op_count[i]) ops.push_back({g_op_count[i].load(), i});
    std::sort(ops.rbegin(), ops.rend());
    std::snprintf(buf, sizeof buf, "\n  constant engine: %llu packets, %llu dumps", (unsigned long long)g_ce.packets,
                  (unsigned long long)g_ce.dumps);
    s += buf;
    s += "\n  EVENT_WRITE types:";
    for (uint32_t i = 0; i < 64; ++i)
        if (g_event_types[i]) {
            std::snprintf(buf, sizeof buf, " 0x%02x:%llu", i, (unsigned long long)g_event_types[i].load());
            s += buf;
        }
    s += "\n  top PM4 opcodes:";
    for (size_t i = 0; i < ops.size() && i < 40; ++i) {
        std::snprintf(buf, sizeof buf, " 0x%02x:%llu", ops[i].second, (unsigned long long)ops[i].first);
        s += buf;
    }
    return s;
}

} // namespace gr::gpu
