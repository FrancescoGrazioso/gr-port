// libSceFont / libSceFontFt on top of stb_truetype (TrueType and OpenType/CFF outlines).
// The game loads its own font files (uroot/arc/.../font/*.otf|ttf) and hands the bytes to
// sceFontOpenFontMemory; glyphs are rasterised straight into the game's render surfaces.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

constexpr uint64_t kErrInvalidFont = 0x80460003, kErrInvalidParam = 0x80460002;

struct Font {
    stbtt_fontinfo info{};
    float scale_w = 16.f, scale_h = 16.f;   // sceFontSetScalePixel
    float render_w = 0.f, render_h = 0.f;   // sceFontSetupRenderScalePixel (0 = use scale_*)
    uint64_t renderer = 0;
};

struct Handle {
    uint64_t magic = 0x464F4E54; // dummy library/renderer objects
};

Font* F(uint64_t h) { return h ? Ptr<Font>(h) : nullptr; }

float PixelScale(const Font* f, bool render) {
    const float px = render && f->render_h > 0 ? f->render_h : f->scale_h;
    return stbtt_ScaleForMappingEmToPixels(&f->info, px);
}

// SceFontGlyphMetrics { float width, height; struct { float bearingX, bearingY, advance; } h, v; }
void Metrics(const Font* f, uint32_t code, bool render, float out[8]) {
    const float s = PixelScale(f, render);
    const int g = stbtt_FindGlyphIndex(&f->info, int(code));
    int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
    stbtt_GetGlyphBox(&f->info, g, &x0, &y0, &x1, &y1);
    const float w = float(x1 - x0) * s, hgt = float(y1 - y0) * s;
    const float em = render && f->render_h > 0 ? f->render_h : f->scale_h;
    out[0] = w;
    out[1] = hgt;
    out[2] = float(x0) * s;        // horizontal bearing X
    out[3] = float(y1) * s;        // horizontal bearing Y (top above baseline)
    out[4] = float(adv) * s;       // horizontal advance
    out[5] = -w * 0.5f;            // vertical bearing X
    out[6] = em * 0.1f;            // vertical bearing Y
    out[7] = em;                   // vertical advance
}

// SceFontRenderSurface { void* buffer; i32 widthByte; i8 pixelSizeByte; u8 pad[3]; i32 width, height;
//                        struct { u32 x0, y0, x1, y1; } scissor; ... }
struct Surface {
    uint8_t* buffer;
    int32_t width_byte;
    int8_t pixel_size;
    int32_t width, height;
    uint32_t sx0, sy0, sx1, sy1;
};
Surface LoadSurface(uint64_t s) {
    Surface r{};
    r.buffer = Ptr<uint8_t>(Load<uint64_t>(s));
    r.width_byte = Load<int32_t>(s + 8);
    r.pixel_size = Load<int8_t>(s + 12);
    r.width = Load<int32_t>(s + 16);
    r.height = Load<int32_t>(s + 20);
    r.sx0 = Load<uint32_t>(s + 24);
    r.sy0 = Load<uint32_t>(s + 28);
    r.sx1 = Load<uint32_t>(s + 32);
    r.sy1 = Load<uint32_t>(s + 36);
    return r;
}

} // namespace

void RegisterFont(Hle& h) {
    static Handle s_selection;
    h.Register("sceFontSelectLibraryFt", [](const uint64_t*) -> uint64_t { return reinterpret_cast<uint64_t>(&s_selection); });
    h.Register("sceFontSelectRendererFt", [](const uint64_t*) -> uint64_t { return reinterpret_cast<uint64_t>(&s_selection); });
    for (const char* n : {"sceFontMemoryInit", "sceFontMemoryTerm", "sceFontSupportSystemFonts", "sceFontSupportExternalFonts",
                          "sceFontAttachDeviceCacheBuffer", "sceFontDestroyRenderer", "sceFontDestroyLibrary",
                          "sceFontFtSupportOpenType", "sceFontFtSupportTrueType"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });
    // (const SceFontMem*, SceFontLibrarySelection, u64 edition, SceFontLibrary* out)
    h.Register("sceFontCreateLibraryWithEdition", [](const uint64_t* a) -> uint64_t {
        Store<uint64_t>(a[3], reinterpret_cast<uint64_t>(new Handle));
        return 0;
    });
    // (const SceFontMem*, SceFontRendererSelection, u64 edition, SceFontRenderer* out)
    h.Register("sceFontCreateRendererWithEdition", [](const uint64_t* a) -> uint64_t {
        Store<uint64_t>(a[3], reinterpret_cast<uint64_t>(new Handle));
        return 0;
    });
    // (library, const void* fontAddress, u32 fontSize, const SceFontOpenDetail*, SceFontHandle* out)
    h.Register("sceFontOpenFontMemory", [](const uint64_t* a) -> uint64_t {
        if (!a[1] || !a[2] || !a[4]) return kErrInvalidParam;
        auto* f = new Font;
        const auto* data = Ptr<const uint8_t>(a[1]);
        const int off = stbtt_GetFontOffsetForIndex(data, 0);
        if (off < 0 || !stbtt_InitFont(&f->info, data, off)) {
            TraceNote("font: unsupported font data (%u bytes)", unsigned(a[2]));
            delete f;
            return kErrInvalidFont;
        }
        TraceNote("font: opened %u-byte font, %d glyphs", unsigned(a[2]), f->info.numGlyphs);
        Store<uint64_t>(a[4], reinterpret_cast<uint64_t>(f));
        return 0;
    });
    h.Register("sceFontCloseFont", [](const uint64_t* a) -> uint64_t {
        delete F(a[0]);
        return 0;
    });
    h.Register("sceFontBindRenderer", [](const uint64_t* a) -> uint64_t {
        if (Font* f = F(a[0])) f->renderer = a[1];
        return 0;
    });
    h.Register("sceFontUnbindRenderer", [](const uint64_t* a) -> uint64_t {
        if (Font* f = F(a[0])) f->renderer = 0;
        return 0;
    });
    // (font, float w, float h)
    h.Register("sceFontSetScalePixel", [](const uint64_t* a) -> uint64_t {
        Font* f = F(a[0]);
        if (!f) return kErrInvalidFont;
        f->scale_w = ArgF(a, 0);
        f->scale_h = ArgF(a, 1);
        return 0;
    });
    h.Register("sceFontSetupRenderScalePixel", [](const uint64_t* a) -> uint64_t {
        Font* f = F(a[0]);
        if (!f) return kErrInvalidFont;
        f->render_w = ArgF(a, 0);
        f->render_h = ArgF(a, 1);
        return 0;
    });
    // (font, float* w, float* h)
    h.Register("sceFontGetScalePixel", [](const uint64_t* a) -> uint64_t {
        Font* f = F(a[0]);
        if (!f) return kErrInvalidFont;
        Store<float>(a[1], f->scale_w);
        Store<float>(a[2], f->scale_h);
        return 0;
    });
    // (font, u32 code, SceFontGlyphMetrics* out)
    h.Register("sceFontGetCharGlyphMetrics", [](const uint64_t* a) -> uint64_t {
        Font* f = F(a[0]);
        if (!f || !a[2]) return kErrInvalidFont;
        float m[8];
        Metrics(f, uint32_t(a[1]), false, m);
        std::memcpy(Ptr<void>(a[2]), m, sizeof m);
        return 0;
    });
    // (font, u32 preCode, u32 code, SceFontKerning* {float dx, dy, px, py})
    h.Register("sceFontGetKerning", [](const uint64_t* a) -> uint64_t {
        Font* f = F(a[0]);
        if (!f || !a[3]) return kErrInvalidFont;
        const int g1 = stbtt_FindGlyphIndex(&f->info, int(a[1])), g2 = stbtt_FindGlyphIndex(&f->info, int(a[2]));
        const float k[4] = {float(stbtt_GetGlyphKernAdvance(&f->info, g1, g2)) * PixelScale(f, false), 0, 0, 0};
        std::memcpy(Ptr<void>(a[3]), k, sizeof k);
        return 0;
    });
    // (SceFontRenderSurface*, void* buffer, int bufWidthByte, int pixelSizeByte, int width, int height)
    h.Register("sceFontRenderSurfaceInit", [](const uint64_t* a) -> uint64_t {
        if (!a[0]) return kErrInvalidParam;
        uint8_t s[40] = {};
        const uint64_t buf = a[1];
        const int32_t wb = int32_t(a[2]), w = int32_t(a[4]), hgt = int32_t(a[5]);
        const int8_t ps = int8_t(a[3]);
        const uint32_t scissor[4] = {0, 0, uint32_t(w), uint32_t(hgt)};
        std::memcpy(s, &buf, 8);
        std::memcpy(s + 8, &wb, 4);
        std::memcpy(s + 12, &ps, 1);
        std::memcpy(s + 16, &w, 4);
        std::memcpy(s + 20, &hgt, 4);
        std::memcpy(s + 24, scissor, 16);
        std::memcpy(Ptr<void>(a[0]), s, sizeof s);
        return 0;
    });
    // (font, u32 code, SceFontRenderSurface*, float x, float y, SceFontGlyphMetrics* m, SceFontRenderOutput* out)
    h.Register("sceFontRenderCharGlyphImageHorizontal", [](const uint64_t* a) -> uint64_t {
        Font* f = F(a[0]);
        if (!f || !a[2]) return kErrInvalidFont;
        const uint32_t code = uint32_t(a[1]);
        const float pen_x = ArgF(a, 0), pen_y = ArgF(a, 1);
        float m[8];
        Metrics(f, code, true, m);
        if (a[3]) std::memcpy(Ptr<void>(a[3]), m, sizeof m);

        const Surface s = LoadSurface(a[2]);
        const float sc = PixelScale(f, true);
        const int g = stbtt_FindGlyphIndex(&f->info, int(code));
        int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        stbtt_GetGlyphBitmapBox(&f->info, g, sc, sc, &bx0, &by0, &bx1, &by1);
        const int gw = bx1 - bx0, gh = by1 - by0;
        const int ox = int(std::floor(pen_x)) + bx0, oy = int(std::floor(pen_y)) + by0;
        if (gw > 0 && gh > 0 && s.buffer && s.pixel_size > 0) {
            std::vector<uint8_t> bmp(size_t(gw) * gh);
            stbtt_MakeGlyphBitmap(&f->info, bmp.data(), gw, gh, gw, sc, sc, g);
            const int cx0 = std::max<int>(int(s.sx0), 0), cy0 = std::max<int>(int(s.sy0), 0);
            const int cx1 = std::min<int>(int(s.sx1 ? s.sx1 : s.width), s.width);
            const int cy1 = std::min<int>(int(s.sy1 ? s.sy1 : s.height), s.height);
            for (int y = 0; y < gh; ++y) {
                const int dy = oy + y;
                if (dy < cy0 || dy >= cy1) continue;
                uint8_t* row = s.buffer + size_t(dy) * s.width_byte;
                for (int x = 0; x < gw; ++x) {
                    const int dx = ox + x;
                    if (dx < cx0 || dx >= cx1) continue;
                    const uint8_t v = bmp[size_t(y) * gw + x];
                    if (!v) continue;
                    uint8_t* px = row + size_t(dx) * s.pixel_size;
                    if (s.pixel_size == 1) {
                        px[0] = std::max(px[0], v);
                    } else {
                        for (int c = 0; c < s.pixel_size - 1 && c < 3; ++c) px[c] = 0xFF;
                        px[s.pixel_size - 1] = std::max(px[s.pixel_size - 1], v);
                    }
                }
            }
        }
        // SceFontRenderOutput (40 bytes, layout confirmed from the game's use of it):
        //   +0x00 const SceFontGlyphMetrics* metrics
        //   +0x08 float bearingX, bearingY, dv, stride
        //   +0x18 i32 x, y      image top-left inside the surface
        //   +0x20 u32 width, height
        if (a[4]) {
            uint8_t out[40] = {};
            const uint64_t mp = a[3];
            const float bx = float(bx0), by = float(-by0), dv = 0.f, stride = float(s.width_byte);
            const int32_t ix = gw > 0 ? ox : 0, iy = gh > 0 ? oy : 0;
            const uint32_t w = uint32_t(std::max(gw, 0)), hh = uint32_t(std::max(gh, 0));
            std::memcpy(out, &mp, 8);
            std::memcpy(out + 8, &bx, 4);
            std::memcpy(out + 12, &by, 4);
            std::memcpy(out + 16, &dv, 4);
            std::memcpy(out + 20, &stride, 4);
            std::memcpy(out + 24, &ix, 4);
            std::memcpy(out + 28, &iy, 4);
            std::memcpy(out + 32, &w, 4);
            std::memcpy(out + 36, &hh, 4);
            std::memcpy(Ptr<void>(a[4]), out, sizeof out);
        }
        return 0;
    });
}

} // namespace gr::kernel
