// libSceAvPlayer over Media Foundation.
//
// The game hands the player a path inside its own archives ("mp4/ep00-01c.mp4") and a set of file
// callbacks (SceAvPlayerFileReplacement) that read it. On AddSource a player thread reads the whole
// file through those callbacks, opens it with an IMFSourceReader (H.264 → NV12, AAC → 16-bit PCM)
// and reports READY. A host decoder thread then keeps a few frames ahead; sceAvPlayerGetVideoDataEx
// hands out the next frame once the playback clock reaches its timestamp, in buffers allocated with
// the game's own allocateTexture callback (NV12, pitch aligned to 64, height to 16, crop offsets for
// the padding), as the real library does. Audio is decoded too and handed out by GetAudioData in
// chunks of at most 1024 samples.
//
// If Media Foundation cannot open a movie, the player falls back to a few black frames and ends, so
// the game never waits forever.
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

enum Event : int32_t { kStateStop = 1, kStateReady = 2, kStatePlay = 3 };

constexpr uint32_t kVideoBuffers = 4;    // guest NV12 buffers (decoded queue + the frame on screen)
constexpr size_t kVideoQueue = 2;        // decoded frames waiting for their time
constexpr size_t kAudioQueue = 32;       // decoded audio chunks (oldest dropped when full)
constexpr uint32_t kAudioChunk = 1024;   // samples per GetAudioData
constexpr uint32_t kFallbackFrames = 3;  // black frames when decoding is not possible

struct VideoFrame {
    uint64_t ts_ms = 0;
    uint32_t buffer = 0; // index into Player::vbuf
};

struct AudioFrame {
    uint64_t ts_ms = 0;
    std::vector<int16_t> pcm; // interleaved
};

struct Player {
    // SceAvPlayerMemAllocator
    uint64_t mem_obj = 0, alloc = 0, dealloc = 0, alloc_tex = 0, free_tex = 0;
    // SceAvPlayerFileReplacement
    uint64_t file_obj = 0, f_open = 0, f_close = 0, f_read = 0, f_size = 0;
    // SceAvPlayerEventReplacement
    uint64_t event_obj = 0, event_cb = 0;
    bool auto_start = false;
    std::string source;

    // stream description (valid once ready)
    uint32_t width = 1920, height = 1080;      // display size
    uint32_t pitch = 1920, aligned_h = 1088;   // guest buffer layout
    uint32_t fps = 30;
    uint32_t channels = 2, sample_rate = 48000;
    uint64_t duration_ms = 0;
    bool has_audio = false;
    bool decoding = false; // Media Foundation reader is open

    // guest buffers
    uint64_t vbuf[kVideoBuffers] = {};
    uint64_t abuf = 0; // one audio chunk buffer (the game consumes it before the next call)

    // playback state
    std::atomic<bool> started{false}, ended{false}, ready{false};
    std::chrono::steady_clock::time_point t0;
    uint64_t last_ts = 0;
    int32_t current = -1; // vbuf index on screen
    uint32_t fallback_left = 0;

    // decoder
    IMFSourceReader* reader = nullptr;
    std::thread decoder;
    std::mutex m;
    std::condition_variable cv;
    std::deque<VideoFrame> video;
    std::deque<AudioFrame> audio;
    std::vector<uint32_t> free_bufs;
    bool eof = false, quit = false;
    std::vector<uint8_t> file; // the whole movie
};

void Fire(Player* p, int32_t ev) {
    if (p->event_cb) CallGuest(p->event_cb, {p->event_obj, uint64_t(uint32_t(ev)), 0, 0});
}

uint64_t ElapsedMs(const Player* p) {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - p->t0).count());
}

// Reads the movie through the game's file callbacks (guest context).
bool ReadMovie(Player* p) {
    if (!p->f_open || !p->f_read || !p->f_size) return false;
    const uint64_t path = p->alloc ? CallGuest(p->alloc, {p->mem_obj, 16, uint64_t(p->source.size() + 1)}) : 0;
    if (!path) return false;
    std::memcpy(Ptr<void>(path), p->source.c_str(), p->source.size() + 1);
    const int32_t fd = int32_t(CallGuest(p->f_open, {p->file_obj, path}));
    bool ok = false;
    if (fd >= 0) {
        const uint64_t size = CallGuest(p->f_size, {p->file_obj});
        constexpr uint32_t kChunk = 1u << 20;
        const uint64_t buf = size ? CallGuest(p->alloc, {p->mem_obj, 64, kChunk}) : 0;
        if (buf && size < (1ull << 31)) {
            p->file.resize(size_t(size));
            uint64_t pos = 0;
            while (pos < size) {
                const uint32_t len = uint32_t(std::min<uint64_t>(kChunk, size - pos));
                const int32_t got = int32_t(CallGuest(p->f_read, {p->file_obj, buf, pos, len}));
                if (got <= 0) break;
                std::memcpy(p->file.data() + pos, Ptr<void>(buf), size_t(got));
                pos += uint64_t(got);
            }
            ok = pos == size;
        }
        if (buf && p->dealloc) CallGuest(p->dealloc, {p->mem_obj, buf});
        if (p->f_close) CallGuest(p->f_close, {p->file_obj});
    }
    if (p->dealloc) CallGuest(p->dealloc, {p->mem_obj, path});
    return ok;
}

bool MfStartup() {
    static const bool ok = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    }();
    return ok;
}

// Opens the in-memory movie and selects NV12 video and 16-bit PCM audio outputs.
bool OpenReader(Player* p) {
    if (!MfStartup() || p->file.empty()) return false;
    IStream* stream = SHCreateMemStream(p->file.data(), UINT(p->file.size()));
    if (!stream) return false;
    IMFByteStream* bs = nullptr;
    HRESULT hr = MFCreateMFByteStreamOnStream(stream, &bs);
    stream->Release();
    if (FAILED(hr)) return false;
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
    hr = MFCreateSourceReaderFromByteStream(bs, attr, &p->reader);
    attr->Release();
    bs->Release();
    if (FAILED(hr)) return false;

    IMFSourceReader* r = p->reader;
    r->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    r->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    IMFMediaType* vt = nullptr;
    MFCreateMediaType(&vt);
    vt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, vt);
    vt->Release();
    if (FAILED(hr)) return false;
    IMFMediaType* cur = nullptr;
    if (SUCCEEDED(r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur))) {
        UINT32 w = 0, h = 0, num = 0, den = 0;
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        MFVideoArea area{};
        if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            w = UINT32(area.Area.cx);
            h = UINT32(area.Area.cy);
        }
        if (SUCCEEDED(MFGetAttributeRatio(cur, MF_MT_FRAME_RATE, &num, &den)) && den) p->fps = (num + den / 2) / den;
        if (w && h) { p->width = w; p->height = h; }
        cur->Release();
    }
    p->pitch = (p->width + 63) & ~63u;
    p->aligned_h = (p->height + 15) & ~15u;

    if (SUCCEEDED(r->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
        IMFMediaType* at = nullptr;
        MFCreateMediaType(&at);
        at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        at->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, at))) {
            IMFMediaType* ac = nullptr;
            if (SUCCEEDED(r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &ac))) {
                p->channels = MFGetAttributeUINT32(ac, MF_MT_AUDIO_NUM_CHANNELS, 2);
                p->sample_rate = MFGetAttributeUINT32(ac, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                p->has_audio = p->channels > 0 && p->channels <= 8;
                ac->Release();
            }
        } else {
            r->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), FALSE);
        }
        at->Release();
    }
    PROPVARIANT dur;
    PropVariantInit(&dur);
    if (SUCCEEDED(r->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &dur)) &&
        dur.vt == VT_UI8)
        p->duration_ms = dur.uhVal.QuadPart / 10000;
    PropVariantClear(&dur);
    return true;
}

// Copies an NV12 sample into a guest buffer laid out as pitch x aligned_h (Y) + pitch x aligned_h/2 (UV).
void CopyNv12(Player* p, IMFSample* s, uint8_t* dst) {
    IMFMediaBuffer* mb = nullptr;
    if (FAILED(s->ConvertToContiguousBuffer(&mb))) return;
    BYTE* src = nullptr;
    LONG stride = 0;
    IMF2DBuffer* b2 = nullptr;
    DWORD len = 0;
    bool locked2d = false;
    if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&src, &stride))) {
        locked2d = true;
    } else if (SUCCEEDED(mb->Lock(&src, nullptr, &len))) {
        stride = LONG(p->width);
    }
    if (src && stride > 0) {
        // the decoder pads the luma plane to a multiple of 16 rows; the chroma plane follows it
        DWORD cur_len = 0;
        mb->GetCurrentLength(&cur_len);
        uint32_t plane_rows = p->aligned_h;
        if (cur_len && uint64_t(stride) * 3 / 2 > 0) {
            const uint64_t rows = uint64_t(cur_len) * 2 / 3 / uint64_t(stride);
            if (rows >= p->height && rows <= p->height + 64) plane_rows = uint32_t(rows);
        }
        const uint32_t row = std::min<uint32_t>(p->width, uint32_t(stride));
        for (uint32_t y = 0; y < p->height; ++y)
            std::memcpy(dst + uint64_t(y) * p->pitch, src + uint64_t(y) * stride, row);
        const uint8_t* uv = src + uint64_t(plane_rows) * stride;
        uint8_t* duv = dst + uint64_t(p->pitch) * p->aligned_h;
        for (uint32_t y = 0; y < p->height / 2; ++y)
            std::memcpy(duv + uint64_t(y) * p->pitch, uv + uint64_t(y) * stride, row);
    }
    if (locked2d) b2->Unlock2D();
    else if (src) mb->Unlock();
    if (b2) b2->Release();
    mb->Release();
}

void DecoderThread(Player* p) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (true) {
        {
            std::unique_lock<std::mutex> lk(p->m);
            p->cv.wait(lk, [p] { return p->quit || (p->video.size() < kVideoQueue && !p->free_bufs.empty()); });
            if (p->quit) break;
        }
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        const HRESULT hr = p->reader->ReadSample(DWORD(MF_SOURCE_READER_ANY_STREAM), 0, &stream, &flags, &ts, &s);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) && !s) {
            // one stream ended; the movie ends with the video stream
            IMFMediaType* t = nullptr;
            bool video_end = FAILED(hr);
            if (!video_end && SUCCEEDED(p->reader->GetCurrentMediaType(stream, &t))) {
                GUID major{};
                t->GetGUID(MF_MT_MAJOR_TYPE, &major);
                video_end = major == MFMediaType_Video;
                t->Release();
            }
            if (video_end) {
                std::lock_guard<std::mutex> lk(p->m);
                p->eof = true;
                break;
            }
            continue;
        }
        if (!s) continue;
        IMFMediaType* t = nullptr;
        GUID major{};
        if (SUCCEEDED(p->reader->GetCurrentMediaType(stream, &t))) {
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            t->Release();
        }
        const uint64_t ts_ms = uint64_t(std::max<LONGLONG>(ts, 0)) / 10000;
        if (major == MFMediaType_Video) {
            uint32_t idx;
            {
                std::lock_guard<std::mutex> lk(p->m);
                idx = p->free_bufs.back();
                p->free_bufs.pop_back();
            }
            CopyNv12(p, s, Ptr<uint8_t>(p->vbuf[idx]));
            std::lock_guard<std::mutex> lk(p->m);
            p->video.push_back({ts_ms, idx});
        } else if (major == MFMediaType_Audio && p->has_audio) {
            IMFMediaBuffer* mb = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&mb))) {
                BYTE* src = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(mb->Lock(&src, nullptr, &len))) {
                    const uint32_t frame_bytes = 2 * p->channels;
                    const uint32_t total = len / frame_bytes;
                    std::lock_guard<std::mutex> lk(p->m);
                    for (uint32_t off = 0; off < total; off += kAudioChunk) {
                        const uint32_t n = std::min(kAudioChunk, total - off);
                        AudioFrame f;
                        f.ts_ms = ts_ms + uint64_t(off) * 1000 / p->sample_rate;
                        f.pcm.assign(reinterpret_cast<int16_t*>(src + uint64_t(off) * frame_bytes),
                                     reinterpret_cast<int16_t*>(src + uint64_t(off + n) * frame_bytes));
                        if (p->audio.size() >= kAudioQueue) p->audio.pop_front();
                        p->audio.push_back(std::move(f));
                    }
                    mb->Unlock();
                }
                mb->Release();
            }
        }
        s->Release();
    }
    CoUninitialize();
}

// Guest context: allocates the output buffers and starts decoding.
void Prepare(Player* p) {
    if (ReadMovie(p) && OpenReader(p) && p->alloc_tex) {
        const uint64_t size = uint64_t(p->pitch) * p->aligned_h * 3 / 2;
        bool ok = true;
        for (uint32_t i = 0; i < kVideoBuffers && ok; ++i) {
            p->vbuf[i] = CallGuest(p->alloc_tex, {p->mem_obj, 0x100, size});
            ok = p->vbuf[i] != 0;
            if (ok) {
                std::memset(Ptr<void>(p->vbuf[i]), 16, uint64_t(p->pitch) * p->aligned_h);
                std::memset(Ptr<void>(p->vbuf[i] + uint64_t(p->pitch) * p->aligned_h), 128, size / 3);
                p->free_bufs.push_back(i);
            }
        }
        if (ok && p->has_audio && p->alloc) p->abuf = CallGuest(p->alloc, {p->mem_obj, 64, kAudioChunk * 2 * p->channels});
        if (ok) {
            p->decoding = true;
            p->file.shrink_to_fit();
            p->decoder = std::thread(DecoderThread, p);
            TraceNote("avplayer: '%s' %ux%u %u fps, %llu ms%s", p->source.c_str(), p->width, p->height, p->fps,
                      (unsigned long long)p->duration_ms, p->has_audio ? ", audio" : "");
            return;
        }
    }
    // fallback: a few black frames
    TraceNote("avplayer: '%s' could not be decoded, playing %u black frames", p->source.c_str(), kFallbackFrames);
    p->pitch = 1920;
    p->width = 1920;
    p->height = p->aligned_h = 1080;
    if (!p->vbuf[0] && p->alloc_tex) {
        const uint64_t size = uint64_t(p->pitch) * p->aligned_h * 3 / 2;
        p->vbuf[0] = CallGuest(p->alloc_tex, {p->mem_obj, 0x100, size});
        if (p->vbuf[0]) {
            std::memset(Ptr<void>(p->vbuf[0]), 16, uint64_t(p->pitch) * p->aligned_h);
            std::memset(Ptr<void>(p->vbuf[0] + uint64_t(p->pitch) * p->aligned_h), 128, size / 3);
        }
    }
    p->duration_ms = uint64_t(kFallbackFrames) * 33;
}

void End(Player* p) {
    if (p->ended.exchange(true)) return;
    Fire(p, kStateStop);
}

void Begin(Player* p) {
    if (p->started.exchange(true)) return;
    p->t0 = std::chrono::steady_clock::now();
    p->fallback_left = p->decoding ? 0 : (p->vbuf[0] ? kFallbackFrames : 0);
    Fire(p, kStatePlay);
    if (!p->decoding && !p->fallback_left) End(p);
}

void StopDecoder(Player* p) {
    {
        std::lock_guard<std::mutex> lk(p->m);
        p->quit = true;
    }
    p->cv.notify_all();
    if (p->decoder.joinable()) p->decoder.join();
    if (p->reader) { p->reader->Release(); p->reader = nullptr; }
}

void FillVideoInfo(Player* p, uint64_t dst, uint64_t data, uint64_t ts, bool ex) {
    // SceAvPlayerFrameInfoEx: +0 pData, +16 timeStamp, +24 details.video {width, height, float aspect,
    // lang[4], framerate, crop l/r/t/b, pitch, u8 lumaBitDepth, u8 chromaBitDepth, bool fullRange}
    // SceAvPlayerFrameInfo: +0 pData, +16 timeStamp, +24 details.video {width, height, float aspect, lang[4]}
    uint8_t info[104] = {};
    const uint32_t w = (p->width + 15) & ~15u, hgt = p->aligned_h, fps = p->fps;
    const uint32_t crop_r = p->pitch - p->width, crop_b = p->aligned_h - p->height;
    const float aspect = float(p->width) / float(p->height);
    std::memcpy(info + 0, &data, 8);
    std::memcpy(info + 16, &ts, 8);
    std::memcpy(info + 24, &w, 4);
    std::memcpy(info + 28, &hgt, 4);
    std::memcpy(info + 32, &aspect, 4);
    if (ex) {
        std::memcpy(info + 40, &fps, 4);
        std::memcpy(info + 48, &crop_r, 4);
        std::memcpy(info + 56, &crop_b, 4);
        std::memcpy(info + 60, &p->pitch, 4);
        info[64] = 8;
        info[65] = 8;
    }
    std::memcpy(Ptr<void>(dst), info, ex ? 104 : 40);
}

template <bool kEx>
uint64_t GetVideo(const uint64_t* a) {
    auto* p = Ptr<Player>(a[0]);
    if (!p || !p->started || p->ended || !a[1]) return 0;
    if (!p->decoding) {
        if (!p->fallback_left) return 0;
        FillVideoInfo(p, a[1], p->vbuf[0], p->last_ts, kEx);
        p->last_ts += 33;
        if (--p->fallback_left == 0) End(p);
        return 1;
    }
    VideoFrame f;
    {
        std::lock_guard<std::mutex> lk(p->m);
        if (p->video.empty()) {
            if (p->eof) {
                // movie over (the game stops on the event)
                p->m.unlock();
                End(p);
                p->m.lock();
            }
            return 0;
        }
        if (p->video.front().ts_ms > ElapsedMs(p)) return 0;
        f = p->video.front();
        p->video.pop_front();
        if (p->current >= 0) p->free_bufs.push_back(uint32_t(p->current));
        p->current = int32_t(f.buffer);
    }
    p->cv.notify_all();
    p->last_ts = f.ts_ms;
    FillVideoInfo(p, a[1], p->vbuf[f.buffer], f.ts_ms, kEx);
    return 1;
}

} // namespace

void RegisterAvPlayer(Hle& h) {
    // SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData*)
    //   +0 memoryReplacement {objectPointer, allocate, deallocate, allocateTexture, deallocateTexture}
    //   +40 fileReplacement {objectPointer, open, close, readOffset, size}
    //   +80 eventReplacement {objectPointer, eventCallback}, +108 bool autoStart
    h.Register("sceAvPlayerInit", [](const uint64_t* a) -> uint64_t {
        auto* p = new Player;
        if (a[0]) {
            p->mem_obj = Load<uint64_t>(a[0]);
            p->alloc = Load<uint64_t>(a[0] + 8);
            p->dealloc = Load<uint64_t>(a[0] + 16);
            p->alloc_tex = Load<uint64_t>(a[0] + 24);
            p->free_tex = Load<uint64_t>(a[0] + 32);
            p->file_obj = Load<uint64_t>(a[0] + 40);
            p->f_open = Load<uint64_t>(a[0] + 48);
            p->f_close = Load<uint64_t>(a[0] + 56);
            p->f_read = Load<uint64_t>(a[0] + 64);
            p->f_size = Load<uint64_t>(a[0] + 72);
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
        // Like the real library, open the source on a player thread and report READY asynchronously,
        // once the game has returned from AddSource and finished setting up its own movie object.
        StartGuestThread(
            [p] {
                Sleep(10);
                Prepare(p);
                p->ready = true;
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
        if (auto* p = Ptr<Player>(a[0])) End(p);
        return 0;
    });
    h.Register("sceAvPlayerIsActive", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        return p && p->started && !p->ended;
    });
    h.Register("sceAvPlayerGetVideoDataEx", GetVideo<true>);
    h.Register("sceAvPlayerGetVideoData", GetVideo<false>);
    // bool sceAvPlayerGetAudioData(handle, SceAvPlayerFrameInfo* info): +0 pData, +16 timeStamp,
    // +24 details.audio {u16 channelCount, u8 reserved[2], u32 sampleRate, u32 size, lang[4]}
    h.Register("sceAvPlayerGetAudioData", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        if (!p || !p->started || p->ended || !p->decoding || !p->abuf || !a[1]) return 0;
        AudioFrame f;
        {
            std::lock_guard<std::mutex> lk(p->m);
            if (p->audio.empty()) return 0;
            f = std::move(p->audio.front());
            p->audio.pop_front();
        }
        const uint32_t size = uint32_t(f.pcm.size() * 2);
        std::memcpy(Ptr<void>(p->abuf), f.pcm.data(), size);
        uint8_t info[40] = {};
        const uint16_t ch = uint16_t(p->channels);
        std::memcpy(info + 0, &p->abuf, 8);
        std::memcpy(info + 16, &f.ts_ms, 8);
        std::memcpy(info + 24, &ch, 2);
        std::memcpy(info + 28, &p->sample_rate, 4);
        std::memcpy(info + 32, &size, 4);
        std::memcpy(Ptr<void>(a[1]), info, sizeof info);
        return 1;
    });
    h.Register("sceAvPlayerStreamCount", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        return p && p->has_audio ? 2 : 1;
    });
    // SceAvPlayerStreamInfo (40 bytes) { u32 type (0 video, 1 audio); u8 reserved[4];
    //   details { video: u32 width, height; float aspect; u8 lang[4] | audio: u16 channels; u8 r[2];
    //   u32 sampleRate; u32 size; u8 lang[4] } u64 duration; u64 startTime; }
    h.Register("sceAvPlayerGetStreamInfo", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        if (!p || !a[2]) return 0x806A0001;
        uint8_t s[40] = {};
        if (a[1] == 1 && p->has_audio) {
            const uint32_t type = 1;
            const uint16_t ch = uint16_t(p->channels);
            std::memcpy(s, &type, 4);
            std::memcpy(s + 8, &ch, 2);
            std::memcpy(s + 12, &p->sample_rate, 4);
        } else {
            const uint32_t w = p->width, hgt = p->height;
            const float aspect = float(p->width) / float(p->height);
            std::memcpy(s + 8, &w, 4);
            std::memcpy(s + 12, &hgt, 4);
            std::memcpy(s + 16, &aspect, 4);
        }
        std::memcpy(s + 24, &p->duration_ms, 8);
        std::memcpy(Ptr<void>(a[2]), s, sizeof s);
        return 0;
    });
    h.Register("sceAvPlayerClose", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        if (!p) return 0;
        StopDecoder(p);
        for (uint64_t b : p->vbuf)
            if (b && p->free_tex) CallGuest(p->free_tex, {p->mem_obj, b});
        if (p->abuf && p->dealloc) CallGuest(p->dealloc, {p->mem_obj, p->abuf});
        delete p;
        return 0;
    });
    for (const char* n : {"sceAvPlayerPause", "sceAvPlayerResume", "sceAvPlayerSetLooping", "sceAvPlayerEnableStream",
                          "sceAvPlayerDisableStream", "sceAvPlayerJumpToTime", "sceAvPlayerSetLogCallback",
                          "sceAvPlayerSetAvSyncMode", "sceAvPlayerSetTrickSpeed"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceAvPlayerCurrentTime", [](const uint64_t* a) -> uint64_t {
        auto* p = Ptr<Player>(a[0]);
        return p ? p->last_ts : 0;
    });
}

} // namespace gr::kernel
