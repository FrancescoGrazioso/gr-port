// Host audio output for libSceAudioOut: every guest port feeds a ring buffer of 48 kHz float stereo
// frames; a mixer thread sums the ports into one shared-mode WASAPI stream (Windows converts to the
// device format). The guest threads keep their own pacing (sceAudioOutOutput blocks for one grain,
// see audio.cpp), so the rings stay short; a port that runs ahead by more than kMaxQueue drops its
// oldest frames instead of drifting.
#include "audio_out.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "guest.h"

namespace gr::kernel {
namespace {

constexpr uint32_t kRate = 48000;
constexpr size_t kMaxQueue = kRate / 5;   // 200 ms per port
constexpr size_t kMinStart = kRate / 50;  // 20 ms buffered before a port starts playing

struct PortRing {
    bool open = false;
    bool primed = false;
    float vol_l = 1.f, vol_r = 1.f;
    double src_pos = 0;          // resampler phase
    float last_l = 0, last_r = 0;
    std::deque<float> frames;    // interleaved L R
};

std::mutex g_m;
PortRing g_rings[32];
std::atomic<bool> g_started{false}, g_muted{false};
std::thread g_mixer;

// Debug (GR_AUDIO_DUMP=<file.wav>): records the mixed output, so tests can check it is not silent.
struct WavDump {
    FILE* f = nullptr;
    uint32_t bytes = 0;
    void Open(const char* path) {
        f = std::fopen(path, "wb");
        if (f) { uint8_t h[44] = {}; std::fwrite(h, 1, 44, f); }
    }
    void Write(const float* s, size_t n) {
        if (!f) return;
        for (size_t i = 0; i < n; ++i) {
            const int16_t v = int16_t(std::clamp(s[i], -1.f, 1.f) * 32767.f);
            std::fwrite(&v, 2, 1, f);
        }
        bytes += uint32_t(n * 2);
        Finish();
    }
    void Finish() { // keeps the header valid at every write (the process may be terminated)
        const long pos = std::ftell(f);
        auto put32 = [&](long off, uint32_t v) { std::fseek(f, off, SEEK_SET); std::fwrite(&v, 4, 1, f); };
        auto put16 = [&](long off, uint16_t v) { std::fseek(f, off, SEEK_SET); std::fwrite(&v, 2, 1, f); };
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f); put32(4, 36 + bytes);
        std::fseek(f, 8, SEEK_SET); std::fwrite("WAVEfmt ", 1, 8, f);
        put32(16, 16); put16(20, 1); put16(22, 2); put32(24, kRate); put32(28, kRate * 4); put16(32, 4); put16(34, 16);
        std::fseek(f, 36, SEEK_SET); std::fwrite("data", 1, 4, f); put32(40, bytes);
        std::fseek(f, pos, SEEK_SET);
        std::fflush(f);
    }
} g_dump;

void Mixer() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
        TraceNote("audio: no output device, sound disabled");
        return;
    }
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = kRate * 8;
    const REFERENCE_TIME buffer_time = 1000000; // 100 ms
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  buffer_time, 0, &wf, nullptr)) ||
        FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
        TraceNote("audio: WASAPI initialisation failed, sound disabled");
        return;
    }
    UINT32 total = 0;
    client->GetBufferSize(&total);
    client->Start();
    TraceNote("audio: WASAPI output started (48 kHz stereo float, %u-frame buffer)", total);
    std::vector<float> mix;
    if (const char* d = std::getenv("GR_AUDIO_DUMP")) g_dump.Open(d);
    while (true) {
        Sleep(5);
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        // keep ~40 ms queued in the device
        const UINT32 target = std::min<UINT32>(total, kRate / 25);
        if (padding >= target) continue;
        const UINT32 n = target - padding;
        mix.assign(size_t(n) * 2, 0.f);
        {
            std::lock_guard<std::mutex> lk(g_m);
            for (PortRing& r : g_rings) {
                if (!r.open) continue;
                if (!r.primed) {
                    if (r.frames.size() / 2 < kMinStart) continue;
                    r.primed = true;
                }
                const size_t avail = std::min<size_t>(n, r.frames.size() / 2);
                for (size_t i = 0; i < avail; ++i) {
                    mix[i * 2] += r.frames[i * 2] * r.vol_l;
                    mix[i * 2 + 1] += r.frames[i * 2 + 1] * r.vol_r;
                }
                r.frames.erase(r.frames.begin(), r.frames.begin() + avail * 2);
                if (avail < n) r.primed = false; // underrun: wait for a little data again
            }
        }
        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(n, &out))) break;
        float* f = reinterpret_cast<float*>(out);
        const bool mute = g_muted.load();
        for (size_t i = 0; i < size_t(n) * 2; ++i) f[i] = mute ? 0.f : std::clamp(mix[i], -1.f, 1.f);
        g_dump.Write(mix.data(), mix.size());
        render->ReleaseBuffer(n, 0);
    }
    client->Stop();
}

// Converts one guest frame to stereo float.
void ToStereo(const uint8_t* src, uint32_t fmt, float& l, float& r) {
    auto s16 = [&](int i) { return float(reinterpret_cast<const int16_t*>(src)[i]) / 32768.f; };
    auto f32 = [&](int i) { return reinterpret_cast<const float*>(src)[i]; };
    switch (fmt) {
    case 0: l = r = s16(0); break;
    case 1: l = s16(0); r = s16(1); break;
    case 3: l = r = f32(0); break;
    case 4: l = f32(0); r = f32(1); break;
    case 2: case 6: case 5: case 7: {
        // 8 channels: FL FR C LFE, then the surround pairs; simple downmix
        const bool fl = fmt == 5 || fmt == 7;
        float c[8];
        for (int i = 0; i < 8; ++i) c[i] = fl ? f32(i) : s16(i);
        l = 0.5f * (c[0] + 0.707f * c[2] + 0.5f * (c[4] + c[6]));
        r = 0.5f * (c[1] + 0.707f * c[2] + 0.5f * (c[5] + c[7]));
        break;
    }
    default: l = r = 0; break;
    }
}

uint32_t FrameBytes(uint32_t fmt) {
    switch (fmt) {
    case 0: return 2;
    case 1: return 4;
    case 3: return 4;
    case 4: return 8;
    case 2: case 6: return 16;
    case 5: case 7: return 32;
    default: return 0;
    }
}

} // namespace

void AudioOutSetMuted(bool m) { g_muted = m; }

void AudioOutOpen(int port) {
    if (port < 0 || port >= 32) return;
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_rings[port] = PortRing{};
        g_rings[port].open = true;
    }
    if (!g_started.exchange(true)) {
        g_mixer = std::thread(Mixer);
        g_mixer.detach();
    }
}

void AudioOutClose(int port) {
    if (port < 0 || port >= 32) return;
    std::lock_guard<std::mutex> lk(g_m);
    g_rings[port] = PortRing{};
}

void AudioOutSetVolume(int port, float l, float r) {
    if (port < 0 || port >= 32) return;
    std::lock_guard<std::mutex> lk(g_m);
    g_rings[port].vol_l = l;
    g_rings[port].vol_r = r;
}

void AudioOutPush(int port, const void* data, uint32_t frames, uint32_t format, uint32_t freq) {
    const uint32_t fb = FrameBytes(format);
    if (port < 0 || port >= 32 || !data || !fb || !frames) return;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    std::lock_guard<std::mutex> lk(g_m);
    PortRing& r = g_rings[port];
    if (!r.open) return;
    // linear resampling to 48 kHz
    const double step = double(freq ? freq : kRate) / kRate;
    float pl = r.last_l, pr = r.last_r;
    for (uint32_t i = 0; i < frames; ++i) {
        float cl, cr;
        ToStereo(src + size_t(i) * fb, format, cl, cr);
        if (step == 1.0) {
            r.frames.push_back(cl);
            r.frames.push_back(cr);
        } else {
            while (r.src_pos < 1.0) {
                const float t = float(r.src_pos);
                r.frames.push_back(pl + (cl - pl) * t);
                r.frames.push_back(pr + (cr - pr) * t);
                r.src_pos += step;
            }
            r.src_pos -= 1.0;
        }
        pl = cl;
        pr = cr;
    }
    r.last_l = pl;
    r.last_r = pr;
    if (r.frames.size() > kMaxQueue * 2) r.frames.erase(r.frames.begin(), r.frames.end() - kMaxQueue * 2);
}

} // namespace gr::kernel
