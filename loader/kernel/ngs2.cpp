// libSceNgs2: Sony's software audio graph, which the game's sound engine ("sndx") drives directly.
//
// Objects: a system owns racks; a rack (sampler 0x1000, submixer 0x2000, reverb 0x2001, mastering
// 0x3000) owns voices. Voices are configured with parameter lists (sceNgs2VoiceControl):
//   common   0x1 matrix levels, 0x2 port volume, 0x3 port matrix, 0x4 port delay, 0x5 patch,
//            0x6 event (play, stop, kill, pause, resume), 0x7 callback
//   sampler  0x10000000 setup (waveform format), 0x10000001 waveform blocks (queued, so the game
//            can stream), 0x10000005 pitch, 0x10000006 envelope, 0x1000000a filter, ...
// A sampler voice decodes its blocks (PCM 16/24-bit, float, or ATRAC9 through LibAtrac9) and sends
// each output port, through its matrix and volume, to the input of the patched voice. Submixers sum
// their inputs and forward them the same way; mastering voices write the render buffers handed to
// sceNgs2SystemRender (mastering voice i → buffer i), which the game then plays with sceAudioOut.
// Not modelled: envelopes and filters (unit gain), reverb (silent), port delays, voice callbacks.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "guest.h"
#include "kernel.h"

#include "libatrac9.h"

namespace gr::kernel {
namespace {

constexpr uint32_t kRackSampler = 0x1000, kRackSubmixer = 0x2000, kRackReverb = 0x2001, kRackMastering = 0x3000;
constexpr uint32_t kMaxPorts = 8, kMaxMatrices = 4, kMixChannels = 8;
// SceNgs2VoiceState flags and events
constexpr uint32_t kStateInUse = 0x1, kStatePlaying = 0x2, kStatePaused = 0x4, kStateStopped = 0x8;
enum : uint32_t { kEventPlay = 0, kEventStop = 1, kEventStopImm = 2, kEventKill = 3, kEventPause = 4, kEventResume = 5 };
// waveform types
constexpr uint32_t kPcmI16L = 0x12, kPcmI24L = 0x14, kPcmF32L = 0x18, kAtrac9 = 0x40;

struct Block {
    uint64_t data = 0;
    uint32_t size = 0, repeats = 0, skip = 0, samples = 0;
    uint64_t user = 0;
};

struct Voice;

struct Port {
    Voice* dest = nullptr;
    uint32_t input = 0;
    float volume = 1.f;
    int32_t matrix = 0;
};

struct Rack {
    uint64_t magic = 0x4E475352; // 'NGSR'
    uint32_t id = 0;
    std::map<uint32_t, Voice*> voices;
};

struct Voice {
    uint64_t magic = 0x4E475356; // 'NGSV'
    Rack* rack = nullptr;
    uint32_t index = 0;
    Port ports[kMaxPorts];
    std::vector<float> matrix[kMaxMatrices];
    uint32_t matrix_levels[kMaxMatrices] = {};
    std::vector<float> input; // kMixChannels x frames (submixer / mastering / reverb)
    uint32_t flags = 0;

    // sampler
    uint32_t wtype = 0, channels = 1, rate = 48000, config = 0;
    float pitch = 1.f;
    std::deque<Block> blocks;
    bool have_block = false;
    Block cur;
    uint32_t cur_repeat = 0;
    uint64_t cur_byte = 0;     // decode position in the current block
    uint32_t cur_sample = 0;   // samples output from the current pass of the block
    uint32_t cur_skipped = 0;
    double phase = 0;          // resampler position between prev and next frame
    float prev[2] = {}, next[2] = {};
    bool primed = false;
    uint64_t decoded_samples = 0, decoded_bytes = 0;
    uint64_t waveform_data = 0;
    uint32_t starved = 0;      // consecutive renders without data
    // ATRAC9
    void* at9 = nullptr;
    Atrac9CodecInfo at9_info{};
    std::vector<int16_t> at9_pcm;
    size_t at9_pos = 0;
    uint32_t at9_frame_in_sf = 0;
};

std::mutex g_m;
float g_peak_sampler = 0, g_peak_mixer = 0, g_peak_master = 0; // GR_NGS2_LOG statistics
std::vector<Rack*> g_racks;

Voice* GetVoice(Rack* r, uint32_t index) {
    Voice*& v = r->voices[index];
    if (!v) {
        v = new Voice;
        v->rack = r;
        v->index = index;
        if (r->id != kRackSampler) v->flags = kStateInUse;
    }
    return v;
}

// A patch destination is a voice handle or, as the game also does, a rack handle (its voice 0).
Voice* AsVoice(uint64_t h) {
    if (!h) return nullptr;
    for (Rack* r : g_racks) {
        if (reinterpret_cast<uint64_t>(r) == h) return GetVoice(r, 0);
        for (auto& [i, v] : r->voices)
            if (reinterpret_cast<uint64_t>(v) == h) return v;
    }
    return nullptr;
}

void ResetStream(Voice* v) {
    v->blocks.clear();
    v->have_block = false;
    v->cur_repeat = v->cur_sample = v->cur_skipped = 0;
    v->cur_byte = 0;
    v->phase = 0;
    v->primed = false;
    v->prev[0] = v->prev[1] = v->next[0] = v->next[1] = 0;
    v->at9_pcm.clear();
    v->at9_pos = 0;
    v->at9_frame_in_sf = 0;
}

void SetupAtrac9(Voice* v) {
    if (!v->at9) v->at9 = Atrac9GetHandle();
    // the config word holds the 4 ATRAC9 config bytes, first byte 0xFE
    uint8_t cfg[4];
    const uint32_t c = v->config;
    if ((c & 0xFF) == 0xFE) {
        cfg[0] = uint8_t(c); cfg[1] = uint8_t(c >> 8); cfg[2] = uint8_t(c >> 16); cfg[3] = uint8_t(c >> 24);
    } else {
        cfg[0] = uint8_t(c >> 24); cfg[1] = uint8_t(c >> 16); cfg[2] = uint8_t(c >> 8); cfg[3] = uint8_t(c);
    }
    if (Atrac9InitDecoder(v->at9, cfg) == 0) Atrac9GetCodecInfo(v->at9, &v->at9_info);
    else v->at9_info = {};
}

bool NextBlock(Voice* v) {
    if (v->have_block) {
        // a block plays 1 + numRepeats times
        if (v->cur.repeats == 0xFFFFFFFFu || v->cur_repeat < v->cur.repeats) {
            ++v->cur_repeat;
            v->cur_byte = 0;
            v->cur_sample = 0;
            v->cur_skipped = 0;
            v->at9_pcm.clear();
            v->at9_pos = 0;
            v->at9_frame_in_sf = 0;
            return true;
        }
        v->have_block = false;
    }
    if (v->blocks.empty()) return false;
    v->cur = v->blocks.front();
    v->blocks.pop_front();
    v->have_block = true;
    v->cur_repeat = 0;
    v->cur_byte = 0;
    v->cur_sample = 0;
    v->cur_skipped = 0;
    v->at9_pcm.clear();
    v->at9_pos = 0;
    v->at9_frame_in_sf = 0;
    v->waveform_data = v->cur.data;
    return true;
}

// Decodes one source frame (up to 2 channels kept) of the current block; false at the end of data.
bool DecodeFrame(Voice* v, float out[2]) {
    while (true) {
        if (!v->have_block && !NextBlock(v)) return false;
        const Block& b = v->cur;
        const uint32_t ch = std::max<uint32_t>(1, v->channels);
        if (b.samples && v->cur_sample >= b.samples) {
            if (!NextBlock(v)) return false;
            continue;
        }
        float s[2] = {0, 0};
        bool got = false;
        if (v->wtype == kAtrac9) {
            if (!v->at9 || !v->at9_info.frameSamples) return false;
            const uint32_t ach = uint32_t(std::max(1, v->at9_info.channels));
            while (v->at9_pos >= v->at9_pcm.size()) {
                if (v->cur_byte >= b.size) break;
                // frames are packed in fixed-size superframes; the padding after the last frame of a
                // superframe is skipped
                const uint64_t sf = uint64_t(std::max(1, v->at9_info.superframeSize));
                const uint32_t fps = uint32_t(std::max(1, v->at9_info.framesInSuperframe));
                if (v->at9_frame_in_sf == 0 && v->cur_byte + sf > b.size) { v->cur_byte = b.size; break; }
                v->at9_pcm.assign(size_t(v->at9_info.frameSamples) * ach, 0);
                int used = 0;
                if (Atrac9Decode(v->at9, Ptr<uint8_t>(b.data + v->cur_byte), v->at9_pcm.data(), &used) != 0 || used <= 0) {
                    v->cur_byte = b.size; // corrupt or truncated: drop the rest of the block
                    v->at9_pcm.clear();
                    break;
                }
                const uint64_t before = v->cur_byte;
                v->cur_byte += uint64_t(used);
                if (++v->at9_frame_in_sf == fps) {
                    v->at9_frame_in_sf = 0;
                    v->cur_byte = (v->cur_byte + sf - 1) / sf * sf;
                }
                v->decoded_bytes += v->cur_byte - before;
                v->at9_pos = 0;
            }
            if (v->at9_pos < v->at9_pcm.size()) {
                s[0] = float(v->at9_pcm[v->at9_pos]) / 32768.f;
                s[1] = ach > 1 ? float(v->at9_pcm[v->at9_pos + 1]) / 32768.f : s[0];
                v->at9_pos += ach;
                got = true;
            }
        } else {
            const uint32_t bps = v->wtype == kPcmI24L ? 3 : v->wtype == kPcmF32L ? 4 : 2;
            if (v->cur_byte + uint64_t(bps) * ch <= b.size) {
                const uint8_t* p = Ptr<uint8_t>(b.data + v->cur_byte);
                for (uint32_t c = 0; c < std::min<uint32_t>(ch, 2); ++c) {
                    const uint8_t* q = p + c * bps;
                    if (bps == 3) s[c] = float(int32_t(uint32_t(q[0]) << 8 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 24) >> 8) / 8388608.f;
                    else if (bps == 4) std::memcpy(&s[c], q, 4);
                    else s[c] = float(int16_t(uint16_t(q[0] | q[1] << 8))) / 32768.f;
                }
                if (ch == 1) s[1] = s[0];
                v->cur_byte += uint64_t(bps) * ch;
                v->decoded_bytes += uint64_t(bps) * ch;
                got = true;
            }
        }
        if (!got) {
            if (!NextBlock(v)) return false;
            continue;
        }
        if (v->cur_skipped < b.skip) { // leading samples to drop (codec delay)
            ++v->cur_skipped;
            continue;
        }
        ++v->cur_sample;
        ++v->decoded_samples;
        out[0] = s[0];
        out[1] = s[1];
        return true;
    }
}

// Renders `frames` stereo frames of a sampler voice at 48 kHz; returns false once it ran out of data.
bool RenderSampler(Voice* v, float* out, uint32_t frames) {
    const double step = double(v->pitch > 0 ? v->pitch : 1.f) * double(v->rate ? v->rate : 48000) / 48000.0;
    if (!v->primed) {
        if (!DecodeFrame(v, v->next)) return false;
        v->prev[0] = v->next[0];
        v->prev[1] = v->next[1];
        v->primed = true;
        v->phase = 0;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const float t = float(v->phase);
        out[i * 2] = v->prev[0] + (v->next[0] - v->prev[0]) * t;
        out[i * 2 + 1] = v->prev[1] + (v->next[1] - v->prev[1]) * t;
        v->phase += step;
        while (v->phase >= 1.0) {
            v->phase -= 1.0;
            v->prev[0] = v->next[0];
            v->prev[1] = v->next[1];
            if (!DecodeFrame(v, v->next)) {
                for (uint32_t k = i + 1; k < frames; ++k) out[k * 2] = out[k * 2 + 1] = 0;
                return false;
            }
        }
    }
    return true;
}

// Sends a voice's output (src_ch channels, frames) through its ports to the patched inputs.
void Route(Voice* v, const float* src, uint32_t src_ch, uint32_t frames) {
    for (const Port& p : v->ports) {
        Voice* d = p.dest;
        if (!d || d->input.empty() || p.volume == 0.f) continue;
        float* dst = d->input.data();
        const int32_t m = p.matrix;
        const std::vector<float>* lv = (m >= 0 && m < int32_t(kMaxMatrices) && !v->matrix[m].empty()) ? &v->matrix[m] : nullptr;
        if (lv) {
            // levels[out * src_ch + in]
            const uint32_t outs = std::min<uint32_t>(kMixChannels, uint32_t(lv->size()) / src_ch);
            for (uint32_t o = 0; o < outs; ++o)
                for (uint32_t c = 0; c < src_ch; ++c) {
                    const float g = (*lv)[o * src_ch + c] * p.volume;
                    if (g == 0.f) continue;
                    for (uint32_t i = 0; i < frames; ++i) dst[i * kMixChannels + o] += src[i * src_ch + c] * g;
                }
        } else {
            for (uint32_t i = 0; i < frames; ++i)
                for (uint32_t c = 0; c < std::min(src_ch, kMixChannels); ++c)
                    dst[i * kMixChannels + c] += src[i * src_ch + c] * p.volume;
        }
    }
}

int Depth(Voice* v) { // patch hops to a mastering voice (submixers feeding submixers render first)
    int d = 0;
    for (Voice* c = v; c && d < 16; ++d) {
        Voice* n = nullptr;
        for (const Port& p : c->ports)
            if (p.dest) { n = p.dest; break; }
        if (!n || (n->rack && n->rack->id == kRackMastering)) break;
        c = n;
    }
    return d;
}

uint64_t NewRack(uint32_t id) {
    auto* r = new Rack;
    r->id = id;
    std::lock_guard<std::mutex> lk(g_m);
    g_racks.push_back(r);
    if (std::getenv("GR_NGS2_LOG")) TraceNote("ngs2: rack 0x%x created", id);
    return reinterpret_cast<uint64_t>(r);
}

void ApplyParams(Voice* v, uint64_t p) {
    static const bool log = std::getenv("GR_NGS2_LOG") != nullptr;
    for (int guard = 0; guard < 64 && p; ++guard) {
        const uint16_t size = Load<uint16_t>(p);
        const int16_t next = Load<int16_t>(p + 2);
        const uint32_t id = Load<uint32_t>(p + 4);
        switch (id) {
        case 0x1: { // matrix levels {matrixId, numLevels, const float* aLevel}
            const uint32_t mid = Load<uint32_t>(p + 8), n = Load<uint32_t>(p + 12);
            const uint64_t lv = Load<uint64_t>(p + 16);
            if (mid < kMaxMatrices && lv && n <= 64) {
                v->matrix[mid].resize(n);
                for (uint32_t i = 0; i < n; ++i) v->matrix[mid][i] = Load<float>(lv + i * 4);
                v->matrix_levels[mid] = n;
                if (log) {
                    char line[256];
                    int k = std::snprintf(line, sizeof line, "ngs2: voice %x.%u matrix %u:", v->rack ? v->rack->id : 0, v->index, mid);
                    for (uint32_t i = 0; i < n && k < 230; ++i) k += std::snprintf(line + k, sizeof line - k, " %.2f", v->matrix[mid][i]);
                    TraceNote("%s", line);
                }
            }
            break;
        }
        case 0x2: { // port volume {port, level}
            const uint32_t port = Load<uint32_t>(p + 8);
            if (port < kMaxPorts) v->ports[port].volume = Load<float>(p + 12);
            break;
        }
        case 0x3: { // port matrix {port, matrixId}
            const uint32_t port = Load<uint32_t>(p + 8);
            if (port < kMaxPorts) v->ports[port].matrix = Load<int32_t>(p + 12);
            break;
        }
        case 0x5: { // patch {port, destInputId, destHandle}
            const uint32_t port = Load<uint32_t>(p + 8);
            if (port < kMaxPorts) {
                v->ports[port].dest = AsVoice(Load<uint64_t>(p + 16));
                v->ports[port].input = Load<uint32_t>(p + 12);
                if (log && !v->ports[port].dest)
                    TraceNote("ngs2: patch %x.%u port %u -> unknown handle %llx (input %u)", v->rack ? v->rack->id : 0,
                              v->index, port, (unsigned long long)Load<uint64_t>(p + 16), v->ports[port].input);
                else if (log)
                    TraceNote("ngs2: patch %x.%u port %u -> %x.%u input %u", v->rack ? v->rack->id : 0, v->index, port,
                              v->ports[port].dest->rack ? v->ports[port].dest->rack->id : 0, v->ports[port].dest->index,
                              v->ports[port].input);
            }
            break;
        }
        case 0x6: { // event
            if (log)
                TraceNote("ngs2: voice %x.%u event %u (queued blocks %zu, decoded %llu)", v->rack ? v->rack->id : 0,
                          v->index, Load<uint32_t>(p + 8), v->blocks.size(), (unsigned long long)v->decoded_samples);
            switch (Load<uint32_t>(p + 8)) {
            case kEventPlay:
                v->flags = kStateInUse | kStatePlaying;
                v->decoded_samples = v->decoded_bytes = 0;
                break;
            case kEventResume:
                if (v->flags & kStateInUse) v->flags = kStateInUse | kStatePlaying;
                break;
            case kEventPause:
                if (v->flags & kStateInUse) v->flags = kStateInUse | kStatePaused;
                break;
            case kEventStop: case kEventStopImm: case kEventKill:
                v->flags = 0;
                ResetStream(v);
                break;
            default: break;
            }
            break;
        }
        case 0x10000000: { // sampler setup {waveformType, numChannels, sampleRate, configData, ...}
            v->wtype = Load<uint32_t>(p + 8);
            v->channels = std::clamp<uint32_t>(Load<uint32_t>(p + 12), 1, 8);
            v->rate = std::max<uint32_t>(1, Load<uint32_t>(p + 16));
            v->config = Load<uint32_t>(p + 20);
            v->pitch = 1.f;
            ResetStream(v);
            if (v->wtype == kAtrac9) SetupAtrac9(v);
            if (log) TraceNote("ngs2: voice %x.%u setup type 0x%x ch %u rate %u config %08x (at9 %d ch, %d samples/frame)",
                               v->rack ? v->rack->id : 0, v->index, v->wtype, v->channels, v->rate, v->config,
                               v->at9_info.channels, v->at9_info.frameSamples);
            break;
        }
        case 0x10000001: { // waveform blocks {const void* data, flags, numBlocks, const Block* aBlock}
            const uint64_t data = Load<uint64_t>(p + 8), blk = Load<uint64_t>(p + 24);
            const uint32_t num = Load<uint32_t>(p + 20);
            for (uint32_t b = 0; b < num && blk && b < 64; ++b) {
                const uint64_t q = blk + uint64_t(b) * 32;
                Block k;
                k.data = data + Load<uint32_t>(q);
                k.size = Load<uint32_t>(q + 4);
                k.repeats = Load<uint32_t>(q + 8);
                k.skip = Load<uint32_t>(q + 12);
                k.samples = Load<uint32_t>(q + 16);
                k.user = Load<uint64_t>(q + 24);
                v->blocks.push_back(k);
            }
            break;
        }
        case 0x10000005: // pitch {ratio}
            v->pitch = Load<float>(p + 8);
            break;
        default: // envelopes, filters, peak meters, mastering/submixer setup: not modelled
            if (log && (!v->rack || v->rack->id != kRackSampler)) {
                char line[256];
                int k = std::snprintf(line, sizeof line, "ngs2: voice %x.%u param 0x%x:", v->rack ? v->rack->id : 0, v->index, id);
                for (uint32_t off = 8; off + 4 <= size && off < 96 && k < 230; off += 4)
                    k += std::snprintf(line + k, sizeof line - k, " %08x", Load<uint32_t>(p + off));
                TraceNote("%s", line);
            }
            break;
        }
        if (next == 0 || size == 0) break;
        p += int64_t(next);
    }
}

} // namespace

void RegisterNgs2(Hle& h) {
    h.Register("sceNgs2SystemCreate", [](const uint64_t* a) -> uint64_t { // (option, bufferInfo, out)
        Store<uint64_t>(a[2], reinterpret_cast<uint64_t>(new Rack));
        return 0;
    });
    h.Register("sceNgs2SystemCreateWithAllocator", [](const uint64_t* a) -> uint64_t { // (option, allocator, out)
        Store<uint64_t>(a[2], reinterpret_cast<uint64_t>(new Rack));
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
        Store<uint64_t>(a[4], NewRack(uint32_t(a[1])));
        return 0;
    });
    h.Register("sceNgs2RackCreateWithAllocator", [](const uint64_t* a) -> uint64_t { // (system, rackId, option, alloc, out)
        Store<uint64_t>(a[4], NewRack(uint32_t(a[1])));
        return 0;
    });
    h.Register("sceNgs2RackDestroy", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_m);
        auto* r = reinterpret_cast<Rack*>(a[0]);
        g_racks.erase(std::remove(g_racks.begin(), g_racks.end(), r), g_racks.end());
        // voices stay allocated: the guest may still hold handles while tearing down
        if (r) {
            for (auto& [i, v] : r->voices) { v->flags = 0; ResetStream(v); v->rack = nullptr; }
            r->voices.clear();
        }
        return 0;
    });
    // (rack, u32 voiceIndex, SceNgs2Handle* out): one stable voice per (rack, index)
    h.Register("sceNgs2RackGetVoiceHandle", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_m);
        auto* r = reinterpret_cast<Rack*>(a[0]);
        if (!r || r->magic != 0x4E475352) return 0x804A0002;
        Store<uint64_t>(a[2], reinterpret_cast<uint64_t>(GetVoice(r, uint32_t(a[1]))));
        return 0;
    });
    // (voice, const SceNgs2VoiceParamHead* list)  head = { u16 size; i16 next; u32 id; }
    h.Register("sceNgs2VoiceControl", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_m);
        if (Voice* v = AsVoice(a[0]); v && a[1]) ApplyParams(v, a[1]);
        return 0;
    });
    // (voice, state*, size_t size). Samplers: SceNgs2SamplerVoiceState (48 bytes) { u32 stateFlags;
    //   f32 envelopeHeight, peakHeight; u32 reserved; u64 numDecodedSamples; u64 decodedDataSize;
    //   u64 userData; const void* waveformData }
    h.Register("sceNgs2VoiceGetState", [](const uint64_t* a) -> uint64_t {
        if (!a[1] || !a[2]) return 0;
        uint8_t s[48] = {};
        {
            std::lock_guard<std::mutex> lk(g_m);
            if (Voice* v = AsVoice(a[0])) {
                const float env = (v->flags & kStatePlaying) ? 1.0f : 0.0f;
                std::memcpy(s, &v->flags, 4);
                std::memcpy(s + 4, &env, 4);
                std::memcpy(s + 16, &v->decoded_samples, 8);
                std::memcpy(s + 24, &v->decoded_bytes, 8);
                std::memcpy(s + 32, &v->cur.user, 8);
                std::memcpy(s + 40, &v->waveform_data, 8);
                static int logged = 0;
                if (std::getenv("GR_NGS2_LOG") && v->wtype == kAtrac9 && (v->flags & kStatePlaying) && logged < 300) {
                    ++logged;
                    TraceNote("ngs2: getstate %x.%u flags %x samples %llu bytes %llu data %llx (cur block %llx size %u, queued %zu) size %llu <- %llx",
                              v->rack ? v->rack->id : 0, v->index, v->flags, (unsigned long long)v->decoded_samples,
                              (unsigned long long)v->decoded_bytes, (unsigned long long)v->waveform_data,
                              (unsigned long long)v->cur.data, v->cur.size, v->blocks.size(), (unsigned long long)a[2],
                              (unsigned long long)a[8]);
                }
            }
        }
        std::memcpy(Ptr<void>(a[1]), s, std::min<uint64_t>(a[2], sizeof s));
        return 0;
    });
    // (system, SceNgs2RenderBufferInfo* {void* buffer; size_t size; u32 waveformType; u32 channels}, u32 n)
    h.Register("sceNgs2SystemRender", [](const uint64_t* a) -> uint64_t {
        const uint64_t nbuf = a[2];
        if (!a[1] || !nbuf) return 0;
        const uint32_t ch0 = std::max<uint32_t>(1, Load<uint32_t>(a[1] + 20));
        const uint32_t frames = uint32_t(Load<uint64_t>(a[1] + 8) / (uint64_t(ch0) * 4));
        if (!frames || frames > 4096) return 0;
        static int logged = 0;
        if (std::getenv("GR_NGS2_LOG") && logged++ < 2)
            for (uint64_t i = 0; i < nbuf; ++i)
                TraceNote("ngs2: render buffer %llu: size %llu type 0x%x ch %u", (unsigned long long)i,
                          (unsigned long long)Load<uint64_t>(a[1] + i * 24 + 8), Load<uint32_t>(a[1] + i * 24 + 16),
                          Load<uint32_t>(a[1] + i * 24 + 20));
        std::lock_guard<std::mutex> lk(g_m);
        std::vector<Voice*> samplers, mixers, masters;
        for (Rack* r : g_racks)
            for (auto& [i, v] : r->voices) {
                if (r->id == kRackSampler) samplers.push_back(v);
                else if (r->id == kRackMastering) masters.push_back(v);
                else mixers.push_back(v);
                if (r->id != kRackSampler) v->input.assign(size_t(frames) * kMixChannels, 0.f);
            }
        std::vector<float> tmp(size_t(frames) * 2);
        for (Voice* v : samplers) {
            if ((v->flags & kStatePaused) && !v->primed && DecodeFrame(v, v->next)) {
                // a paused voice still fills its codec cache: streams wait for this before resuming
                v->prev[0] = v->next[0];
                v->prev[1] = v->next[1];
                v->primed = true;
                v->phase = 0;
            }
            if ((v->flags & (kStateInUse | kStatePlaying)) != (kStateInUse | kStatePlaying)) continue;
            const bool more = RenderSampler(v, tmp.data(), frames);
            for (float x : tmp) g_peak_sampler = std::max(g_peak_sampler, std::fabs(x));
            if (v->channels == 1) { // the matrices of mono voices have one input column
                for (uint32_t i = 0; i < frames; ++i) tmp[i] = tmp[i * 2];
                Route(v, tmp.data(), 1, frames);
            } else {
                Route(v, tmp.data(), 2, frames);
            }
            // Out of waveform data: streams refill the queue in separate calls, so a voice only frees
            // itself after ~60 ms without data (~1 s if it never received any).
            if (more) {
                v->starved = 0;
            } else if (++v->starved > (v->decoded_samples ? 12u : 190u)) {
                v->flags = 0;
                v->starved = 0;
            }
        }
        std::sort(mixers.begin(), mixers.end(), [](Voice* x, Voice* y) { return Depth(x) > Depth(y); });
        for (Voice* v : mixers) {
            if (v->rack->id == kRackReverb) continue; // reverb is not modelled (silent)
            for (float x : v->input) g_peak_mixer = std::max(g_peak_mixer, std::fabs(x));
            Route(v, v->input.data(), kMixChannels, frames);
        }
        std::sort(masters.begin(), masters.end(), [](Voice* x, Voice* y) { return x->index < y->index; });
        static const bool stats = std::getenv("GR_NGS2_LOG") != nullptr;
        static uint32_t renders = 0;
        if (stats)
            for (Voice* m : masters)
                for (float x : m->input) g_peak_master = std::max(g_peak_master, std::fabs(x));
        if (stats && ++renders % 375 == 0) { // ~2 s
            int active = 0, at9 = 0;
            for (Voice* v : samplers)
                if (v->flags & kStatePlaying) { ++active; at9 += v->wtype == kAtrac9; }
            TraceNote("ngs2: render %u: %zu samplers, %d playing (%d atrac9), %zu masters; 2 s peaks: samplers %.3f, mixer inputs %.3f, master %.3f",
                      renders, samplers.size(), active, at9, masters.size(), g_peak_sampler, g_peak_mixer, g_peak_master);
            g_peak_sampler = g_peak_mixer = g_peak_master = 0;
        }
        for (uint64_t b = 0; b < nbuf; ++b) {
            const uint64_t info = a[1] + b * 24;
            const uint64_t buf = Load<uint64_t>(info), size = Load<uint64_t>(info + 8);
            const uint32_t type = Load<uint32_t>(info + 16), ch = std::max<uint32_t>(1, Load<uint32_t>(info + 20));
            if (!buf || !size) continue;
            std::memset(Ptr<void>(buf), 0, size);
            if (b >= masters.size() || type != kPcmF32L) continue;
            const float* src = masters[b]->input.data();
            float* dst = Ptr<float>(buf);
            const uint32_t n = uint32_t(std::min<uint64_t>(frames, size / (uint64_t(ch) * 4)));
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t c = 0; c < ch && c < kMixChannels; ++c) dst[i * ch + c] = src[i * kMixChannels + c];
        }
        return 0;
    });
}

} // namespace gr::kernel
