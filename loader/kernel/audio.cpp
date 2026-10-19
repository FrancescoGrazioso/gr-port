// libSceAudioOut. Ports play through WASAPI (audio_out.cpp); libSceNgs2, the game's mixer that
// feeds them, is in ngs2.cpp.
// sceAudioOutOutput blocks for one grain like the real hardware does, which is what paces the
// game's sound threads.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <vector>

#include "guest.h"
#include "kernel.h"
#include "audio_out.h"

namespace gr::kernel {

void RegisterNgs2(Hle& h); // ngs2.cpp
namespace {

struct Port {
    bool used = false;
    uint32_t grain = 256, freq = 48000, format = 0;
    uint64_t next_us = 0;
};
std::mutex g_audio;
Port g_ports[32];

uint64_t NowUs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(double(c.QuadPart) * 1e6 / double(f.QuadPart));
}

// Sleeps until the port's next grain boundary.
void PaceOutput(int handle) {
    uint64_t target;
    {
        std::lock_guard<std::mutex> lk(g_audio);
        Port& p = g_ports[handle];
        const uint64_t period = uint64_t(p.grain) * 1000000 / (p.freq ? p.freq : 48000);
        const uint64_t now = NowUs();
        if (p.next_us < now || p.next_us > now + 4 * period) p.next_us = now;
        p.next_us += period;
        target = p.next_us;
    }
    for (uint64_t now = NowUs(); now < target; now = NowUs()) {
        const uint64_t left = target - now;
        if (left > 2000) Sleep(DWORD(left / 1000) - 1);
        else SwitchToThread();
    }
}

bool ValidPort(uint64_t h) { return h >= 1 && h < 32 && g_ports[h].used; }

} // namespace

void RegisterAudio(Hle& h) {
    h.Register("sceAudioOutInit", [](const uint64_t*) -> uint64_t { return 0; });
    // (userId, type, index, u32 len, u32 freq, u32 param)
    h.Register("sceAudioOutOpen", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_audio);
        for (int i = 1; i < 32; ++i) {
            if (g_ports[i].used) continue;
            g_ports[i] = {true, uint32_t(a[3]), uint32_t(a[4]), uint32_t(a[5]), 0};
            TraceNote("audio: port %d opened (type %d, %u samples @ %u Hz, format 0x%x)", i, int(a[1]),
                      unsigned(a[3]), unsigned(a[4]), unsigned(a[5]));
            AudioOutOpen(i);
            return uint64_t(i);
        }
        return 0x80260005; // port full
    });
    h.Register("sceAudioOutClose", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_audio);
        if (a[0] < 32) g_ports[a[0]].used = false;
        AudioOutClose(int(a[0]));
        return 0;
    });
    h.Register("sceAudioOutOutput", [](const uint64_t* a) -> uint64_t {
        if (!ValidPort(a[0])) return 0x80260004;
        if (a[1]) { // a null pointer only flushes
            const Port& p = g_ports[a[0]];
            AudioOutPush(int(a[0]), Ptr<void>(a[1]), p.grain, p.format & 0xFF, p.freq);
            PaceOutput(int(a[0]));
        }
        return g_ports[a[0]].grain;
    });
    // (const SceAudioOutOutputParam* {i32 handle; pad; const void* ptr;}, u32 num)
    h.Register("sceAudioOutOutputs", [](const uint64_t* a) -> uint64_t {
        if (!a[0] || !a[1]) return 0x80260004;
        const int32_t handle = Load<int32_t>(a[0]);
        if (!ValidPort(uint64_t(handle))) return 0x80260004;
        for (uint64_t i = 0; i < a[1]; ++i) {
            const int32_t ph = Load<int32_t>(a[0] + i * 16);
            const uint64_t ptr = Load<uint64_t>(a[0] + i * 16 + 8);
            if (ValidPort(uint64_t(ph)) && ptr) {
                const Port& p = g_ports[ph];
                AudioOutPush(ph, Ptr<void>(ptr), p.grain, p.format & 0xFF, p.freq);
            }
        }
        PaceOutput(handle);
        return g_ports[handle].grain;
    });
    // SceAudioOutPortState { u16 output; u8 channel; u8 reserved; i16 volume; u16 rerouteCounter; u64 flag; u64 r[2]; }
    h.Register("sceAudioOutGetPortState", [](const uint64_t* a) -> uint64_t {
        uint8_t s[32] = {};
        const uint16_t output = 1;  // main (HDMI)
        const int16_t volume = 127;
        s[2] = 2;                   // stereo
        std::memcpy(s, &output, 2);
        std::memcpy(s + 4, &volume, 2);
        std::memcpy(Ptr<void>(a[1]), s, sizeof s);
        return 0;
    });
    // (handle, u32 flag (channel bits), const i32* vol), 32768 = 0 dB
    h.Register("sceAudioOutSetVolume", [](const uint64_t* a) -> uint64_t {
        if (!ValidPort(a[0]) || !a[2]) return 0;
        const float l = (a[1] & 1) ? float(Load<int32_t>(a[2])) / 32768.f : 1.f;
        const float r = (a[1] & 2) ? float(Load<int32_t>(a[2] + 4)) / 32768.f : l;
        AudioOutSetVolume(int(a[0]), l, r);
        return 0;
    });
    h.Register("sceAudioOutSetMixLevelPadSpk", [](const uint64_t*) -> uint64_t { return 0; });
    RegisterNgs2(h);
}

} // namespace gr::kernel
