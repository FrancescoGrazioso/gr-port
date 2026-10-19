// libSceAudioOut and libSceNgs2. AudioOut ports play through WASAPI (audio_out.cpp); Ngs2 is still
// a stub that renders silence.
// sceAudioOutOutput blocks for one grain like the real hardware does, which is what paces the
// game's sound threads.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "guest.h"
#include "kernel.h"
#include "audio_out.h"

namespace gr::kernel {
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

// Ngs2 handles: small heap objects so the guest gets unique non-null values. Voices also carry a
// playback state machine so the game's sound engine sees streams start, advance and stop.
struct Ngs2Object {
    uint64_t magic = 0x4E475332;
    // voice state
    std::mutex m;
    uint32_t flags = 0;          // SCE_NGS2_VOICE_STATE_FLAG_*
    bool playing = false;
    uint64_t start_us = 0;       // when PLAY/RESUME happened
    uint64_t done_samples = 0;   // samples accumulated before the last pause
    uint32_t rate = 48000, channels = 2;
};

// Voice state flags and events (sceNgs2VoiceControl, param id 6).
constexpr uint32_t kStateInUse = 0x1, kStatePlaying = 0x2, kStatePaused = 0x4;
enum : uint32_t { kEventPlay = 0, kEventStop = 1, kEventStopImm = 2, kEventKill = 3, kEventPause = 4, kEventResume = 5 };
constexpr uint32_t kParamEvent = 0x6, kParamSamplerSetup = 0x10000000;

uint64_t Samples(Ngs2Object* v) {
    uint64_t s = v->done_samples;
    if (v->playing) s += (NowUs() - v->start_us) * v->rate / 1000000;
    return s;
}

void VoiceEvent(Ngs2Object* v, uint32_t ev) {
    std::lock_guard<std::mutex> lk(v->m);
    switch (ev) {
    case kEventPlay:
        v->flags = kStateInUse | kStatePlaying;
        v->playing = true;
        v->done_samples = 0;
        v->start_us = NowUs();
        break;
    case kEventResume:
        if (!v->playing && (v->flags & kStateInUse)) {
            v->playing = true;
            v->start_us = NowUs();
            v->flags = kStateInUse | kStatePlaying;
        }
        break;
    case kEventPause:
        if (v->playing) {
            v->done_samples = Samples(v);
            v->playing = false;
        }
        v->flags = kStateInUse | kStatePaused;
        break;
    case kEventStop:
    case kEventStopImm:
    case kEventKill:
        v->done_samples = Samples(v);
        v->playing = false;
        v->flags = 0;
        break;
    default: break;
    }
}

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

    // ---- Ngs2: create objects, render silence
    h.Register("sceNgs2SystemCreate", [](const uint64_t* a) -> uint64_t {        // (option, bufferInfo, out)
        Store<uint64_t>(a[2], reinterpret_cast<uint64_t>(new Ngs2Object));
        return 0;
    });
    h.Register("sceNgs2SystemCreateWithAllocator", [](const uint64_t* a) -> uint64_t { // (option, allocator, out)
        Store<uint64_t>(a[2], reinterpret_cast<uint64_t>(new Ngs2Object));
        return 0;
    });
    h.Register("sceNgs2SystemDestroy", [](const uint64_t*) -> uint64_t { return 0; });
    // (u32 rackId, const option*, SceNgs2ContextBufferInfo* {void* hostBuffer; size_t hostBufferSize; ...})
    h.Register("sceNgs2RackQueryBufferSize", [](const uint64_t* a) -> uint64_t {
        Store<uint64_t>(a[2] + 8, 0x10000);
        return 0;
    });
    h.Register("sceNgs2SystemQueryBufferSize", [](const uint64_t* a) -> uint64_t {
        Store<uint64_t>(a[1] + 8, 0x10000);
        return 0;
    });
    h.Register("sceNgs2RackCreate", [](const uint64_t* a) -> uint64_t { // (system, rackId, option, bufferInfo, out)
        Store<uint64_t>(a[4], reinterpret_cast<uint64_t>(new Ngs2Object));
        return 0;
    });
    h.Register("sceNgs2RackCreateWithAllocator", [](const uint64_t* a) -> uint64_t { // (system, rackId, option, alloc, out)
        Store<uint64_t>(a[4], reinterpret_cast<uint64_t>(new Ngs2Object));
        return 0;
    });
    h.Register("sceNgs2RackDestroy", [](const uint64_t*) -> uint64_t { return 0; });
    // (rack, u32 voiceIndex, SceNgs2Handle* out): one stable object per (rack, index)
    h.Register("sceNgs2RackGetVoiceHandle", [](const uint64_t* a) -> uint64_t {
        static std::mutex m;
        static std::vector<std::pair<std::pair<uint64_t, uint32_t>, Ngs2Object*>> voices;
        std::lock_guard<std::mutex> lk(m);
        for (auto& [k, v] : voices)
            if (k.first == a[0] && k.second == uint32_t(a[1])) {
                Store<uint64_t>(a[2], reinterpret_cast<uint64_t>(v));
                return 0;
            }
        auto* v = new Ngs2Object;
        voices.push_back({{a[0], uint32_t(a[1])}, v});
        Store<uint64_t>(a[2], reinterpret_cast<uint64_t>(v));
        return 0;
    });
    // (voice, const SceNgs2VoiceParamHead* list)  head = { u16 size; i16 next; u32 id; }
    h.Register("sceNgs2VoiceControl", [](const uint64_t* a) -> uint64_t {
        if (a[1]) {
            char line[1024];
            int n = std::snprintf(line, sizeof line, "ngs2: VoiceControl voice=%llx", (unsigned long long)a[0]);
            uint64_t p = a[1];
            auto* voice = Ptr<Ngs2Object>(a[0]);
            for (int i = 0; i < 16 && p && n < 800; ++i) {
                const uint16_t size = Load<uint16_t>(p);
                const int16_t next = Load<int16_t>(p + 2);
                const uint32_t id = Load<uint32_t>(p + 4);
                if (voice && id == kParamEvent && size >= 12) VoiceEvent(voice, Load<uint32_t>(p + 8));
                if (voice && id == kParamSamplerSetup && size >= 20) { // {format, channels, sampleRate, ...}
                    std::lock_guard<std::mutex> lk(voice->m);
                    voice->channels = std::max<uint32_t>(1, Load<uint32_t>(p + 12));
                    voice->rate = std::max<uint32_t>(1, Load<uint32_t>(p + 16));
                }
                n += std::snprintf(line + n, sizeof line - n, " [id=0x%x size=%u:", id, size);
                for (uint32_t off = 8; off + 4 <= size && off < 40 && n < 900; off += 4)
                    n += std::snprintf(line + n, sizeof line - n, " %x", Load<uint32_t>(p + off));
                n += std::snprintf(line + n, sizeof line - n, "]");
                if (next == 0) break;
                p += int64_t(next);
            }
            TraceLine(line, a[8]);
        }
        return 0;
    });
    // (voice, state*, size_t size). For samplers the state is SceNgs2SamplerVoiceState (48 bytes):
    //   u32 stateFlags; f32 envelopeHeight, peakHeight; u32 reserved; u64 numDecodedSamples;
    //   u64 decodedDataSize; u64 userData; const void* waveformData
    h.Register("sceNgs2VoiceGetState", [](const uint64_t* a) -> uint64_t {
        if (!a[1] || !a[2]) return 0;
        uint8_t s[48] = {};
        if (auto* v = Ptr<Ngs2Object>(a[0])) {
            std::lock_guard<std::mutex> lk(v->m);
            const uint64_t samples = Samples(v), bytes = samples * v->channels * 2;
            const float env = v->playing ? 1.0f : 0.0f;
            std::memcpy(s, &v->flags, 4);
            std::memcpy(s + 4, &env, 4);
            std::memcpy(s + 16, &samples, 8);
            std::memcpy(s + 24, &bytes, 8);
        }
        std::memcpy(Ptr<void>(a[1]), s, std::min<uint64_t>(a[2], sizeof s));
        return 0;
    });
    // (system, SceNgs2RenderBufferInfo* {void* buffer; size_t size; u32 waveformType; u32 channels}, u32 n)
    h.Register("sceNgs2SystemRender", [](const uint64_t* a) -> uint64_t {
        for (uint64_t i = 0; i < a[2]; ++i) {
            const uint64_t info = a[1] + i * 24;
            const uint64_t buf = Load<uint64_t>(info), size = Load<uint64_t>(info + 8);
            if (buf && size) std::memset(Ptr<void>(buf), 0, size);
        }
        return 0;
    });
}

} // namespace gr::kernel
