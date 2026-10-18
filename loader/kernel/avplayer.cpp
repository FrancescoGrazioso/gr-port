// libSceAvPlayer, phase 1: every movie plays as a few black frames and then ends.
//
// Behaves like a real (very short) stream: READY on AddSource, PLAY on Start, a handful of decoded
// NV12 frames through sceAvPlayerGetVideoDataEx (allocated with the game's own allocateTexture
// callback, as the real library does), then STOP and sceAvPlayerIsActive == false. Games create
// their video textures on the first frame and release them on finalize, so at least one frame must
// be delivered. Real H.264/AAC decoding through Media Foundation is phase 2.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

enum Event : int32_t { kStateStop = 1, kStateReady = 2, kStatePlay = 3 };

constexpr uint32_t kWidth = 1920, kHeight = 1080, kFrames = 3;

struct Player {
    // SceAvPlayerMemAllocator
    uint64_t mem_obj = 0, alloc_tex = 0, free_tex = 0;
    // SceAvPlayerEventReplacement
    uint64_t event_obj = 0, event_cb = 0;
    bool auto_start = false;
    bool started = false, ended = false;
    uint32_t frames_left = 0;
    uint32_t frames_delivered = 0;
    uint64_t frame = 0; // NV12 buffer
    std::string source;
};

void Fire(Player* p, int32_t ev) {
    if (p->event_cb) CallGuest(p->event_cb, {p->event_obj, uint64_t(uint32_t(ev)), 0, 0});
}

void Begin(Player* p) {
    if (p->started) return;
    p->started = true;
    if (!p->frame && p->alloc_tex) {
        const uint64_t size = uint64_t(kWidth) * kHeight * 3 / 2;
        p->frame = CallGuest(p->alloc_tex, {p->mem_obj, 0x100, size});
        if (p->frame) {
            std::memset(Ptr<void>(p->frame), 16, uint64_t(kWidth) * kHeight);                    // Y: black
            std::memset(Ptr<void>(p->frame + uint64_t(kWidth) * kHeight), 128, size / 3);        // UV: neutral
        }
    }
    p->frames_left = p->frame ? kFrames : 0;
    Fire(p, kStatePlay);
    if (!p->frames_left) {
        p->ended = true;
        Fire(p, kStateStop);
    }
}

} // namespace

void RegisterAvPlayer(Hle& h) {
    // SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData*)
    //   +0 memoryReplacement {objectPointer, allocate, deallocate, allocateTexture, deallocateTexture}
    //   +80 eventReplacement {objectPointer, eventCallback}, +108 bool autoStart
    h.Register("sceAvPlayerInit", [](const uint64_t* a) -> uint64_t {
        auto* p = new Player;
        if (a[0]) {
            p->mem_obj = Load<uint64_t>(a[0]);
            p->alloc_tex = Load<uint64_t>(a[0] + 24);
            p->free_tex = Load<uint64_t>(a[0] + 32);
            p->event_obj = Load<uint64_t>(a[0] + 80);
            p->event_cb = Load<uint64_t>(a[0] + 88);
            p->auto_start = Load<uint8_t>(a[0] + 108) != 0;
        }
        return reinterpret_cast<uint64_t>(p);
    });
    h.Register("sceAvPlayerPostInit", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceAvPlayerAddSource", [](const uint64_t* a) -> uint64_t { // (handle, const char* path)
        auto* p = Ptr<Player>(a[0]);
        if (!p) return 0x806A0001;
        p->source = Str(a[1]);
        TraceNote("avplayer: movie '%s' (decoding not implemented yet: %u black frames)", p->source.c_str(), kFrames);
        // Like the real library, report READY asynchronously from a player thread once the game has
        // returned from AddSource and finished setting up its own movie object.
        StartGuestThread(
            [p] {
                Sleep(30);
                Fire(p, kStateReady);
                if (p->auto_start) Begin(p);
            },
            1u << 20, nullptr, "AvPlayerEvents");
        return 0;
    });
    h.Register("sceAvPlayerStart", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        if (!p) return 0x806A0001;
        Begin(p);
        return 0;
    });
    h.Register("sceAvPlayerStop", [](const uint64_t* a) -> uint64_t {
        if (auto* p = Ptr<Player>(a[0]); p && !p->ended) {
            p->ended = true;
            p->frames_left = 0;
            Fire(p, kStateStop);
        }
        return 0;
    });
    h.Register("sceAvPlayerIsActive", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        return p && p->started && !p->ended;
    });
    // bool sceAvPlayerGetVideoDataEx(handle, SceAvPlayerFrameInfoEx* info)
    //   +0 pData, +16 timeStamp, +24 details.video {width, height, float aspect, lang[4], framerate,
    //   crop l/r/t/b, pitch, u8 lumaBitDepth, u8 chromaBitDepth, bool fullRange}
    auto video = [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        if (!p || !p->started || p->ended || !p->frames_left || !a[1]) return 0;
        uint8_t info[80] = {};
        const uint64_t ts = uint64_t(p->frames_delivered++) * 33;
        const uint32_t w = kWidth, hgt = kHeight, fps = 30, pitch = kWidth;
        const float aspect = 16.f / 9.f;
        std::memcpy(info + 0, &p->frame, 8);
        std::memcpy(info + 16, &ts, 8);
        std::memcpy(info + 24, &w, 4);
        std::memcpy(info + 28, &hgt, 4);
        std::memcpy(info + 32, &aspect, 4);
        std::memcpy(info + 40, &fps, 4);
        std::memcpy(info + 60, &pitch, 4);
        info[64] = 8;
        info[65] = 8;
        std::memcpy(Ptr<void>(a[1]), info, sizeof info);
        if (--p->frames_left == 0) {
            p->ended = true;
            Fire(p, kStateStop);
        }
        return 1;
    };
    h.Register("sceAvPlayerGetVideoDataEx", video);
    h.Register("sceAvPlayerGetVideoData", video);
    h.Register("sceAvPlayerGetAudioData", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceAvPlayerStreamCount", [](const uint64_t*) -> uint64_t { return 1; });
    // SceAvPlayerStreamInfo (40 bytes) { u32 type (0 = video); u8 reserved[4];
    //   details.video { u32 width, height; float aspect; u8 lang[4]; } u64 duration; u64 startTime; }
    h.Register("sceAvPlayerGetStreamInfo", [](const uint64_t* a) -> uint64_t {
        if (!a[2]) return 0x806A0001;
        uint8_t s[40] = {};
        const uint32_t w = kWidth, hgt = kHeight;
        const float aspect = 16.f / 9.f;
        const uint64_t duration = uint64_t(kFrames) * 33;
        std::memcpy(s + 8, &w, 4);
        std::memcpy(s + 12, &hgt, 4);
        std::memcpy(s + 16, &aspect, 4);
        std::memcpy(s + 24, &duration, 8);
        std::memcpy(Ptr<void>(a[2]), s, sizeof s);
        return 0;
    });
    h.Register("sceAvPlayerClose", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        if (!p) return 0;
        if (p->frame && p->free_tex) CallGuest(p->free_tex, {p->mem_obj, p->frame});
        delete p;
        return 0;
    });
    for (const char* n : {"sceAvPlayerPause", "sceAvPlayerResume", "sceAvPlayerSetLooping", "sceAvPlayerEnableStream",
                          "sceAvPlayerDisableStream", "sceAvPlayerJumpToTime", "sceAvPlayerSetLogCallback",
                          "sceAvPlayerSetAvSyncMode", "sceAvPlayerSetTrickSpeed"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceAvPlayerCurrentTime", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        return p ? uint64_t(p->frames_delivered) * 33 : 0;
    });
}

} // namespace gr::kernel
