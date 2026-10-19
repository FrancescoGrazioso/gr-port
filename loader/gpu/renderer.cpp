#include "gpu/renderer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "gpu/pm4.h"
#include "gpu/renderdoc.h"
#include "guest_bp.h"
#include "gpu/shader_recompiler.h"
#include "gpu/videoout.h"
#include "guest.h"

namespace fs = std::filesystem;

namespace gr::gpu {
namespace {

// ---------------------------------------------------------------------------------------------------------
// Register helpers

constexpr uint32_t kCbBase = 0xA318, kCbStride = 15;

struct Regs {
    const uint32_t* ctx;
    const uint32_t* sh;
    const uint32_t* uc;
    uint32_t C(uint32_t r) const { return ctx[r - kContextRegBase]; }
    uint32_t S(uint32_t r) const { return sh[r - kShRegBase]; }
    float Cf(uint32_t r) const { float f; uint32_t v = C(r); std::memcpy(&f, &v, 4); return f; }
};

uint64_t PgmAddr(const Regs& r, uint32_t lo_reg) {
    return (uint64_t(r.S(lo_reg)) << 8) | (uint64_t(r.S(lo_reg + 1) & 0xFF) << 40);
}

uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

double NowMs() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * inv;
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

bool SafeWrite(uint64_t dst, const void* src, size_t n) {
    if (dst < 0x10000) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Cheap change detector for guest-memory textures: 64-bit words, the whole range up to 256 KiB,
// beyond that 2 KiB out of every 32 KiB plus the tail.
uint64_t SampleHash(uint64_t addr, uint64_t bytes) {
    const auto* p = reinterpret_cast<const uint64_t*>(addr);
    const uint64_t words = bytes / 8;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; };
    if (bytes <= (256u << 10)) {
        for (uint64_t i = 0; i < words; ++i) mix(p[i]);
    } else {
        for (uint64_t chunk = 0; chunk < words; chunk += 4096)
            for (uint64_t i = chunk; i < std::min(words, chunk + 256); ++i) mix(p[i]);
        for (uint64_t i = words > 256 ? words - 256 : 0; i < words; ++i) mix(p[i]);
    }
    return h;
}

// VirtualQuery is very slow in this process (thousands of guest regions), so committed ranges that
// were already seen are cached. Reads that could still fault go through SafeCopy / __try.
bool Readable(uint64_t addr, size_t n) {
    struct Range { uint64_t lo, hi; };
    static Range cache[64];
    static unsigned next = 0;
    if (addr < 0x10000) return false;
    for (const Range& r : cache)
        if (addr >= r.lo && addr + n <= r.hi) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    const uint64_t lo = reinterpret_cast<uint64_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    cache[next++ % 64] = {lo, hi};
    return addr + n <= hi;
}

bool SafeCopy(void* dst, uint64_t src, size_t n) {
    if (src < 0x10000) { std::memset(dst, 0, n); return false; }
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(dst, 0, n);
        return false;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Tiling

// Element index inside an 8x8 micro tile. Non-displayable / depth thin tiles interleave x and y bits
// (x0 y0 x1 y1 x2 y2); displayable tiles use a per-element-size order (AddrLib ComputePixelIndexWithinMicroTile).
uint32_t MicroIndex(uint32_t x, uint32_t y, uint32_t bpp, bool display) {
    auto b = [](uint32_t v, int i) { return (v >> i) & 1u; };
    uint32_t bits[6];
    if (!display) {
        const uint32_t t[6] = {b(x, 0), b(y, 0), b(x, 1), b(y, 1), b(x, 2), b(y, 2)};
        std::memcpy(bits, t, sizeof t);
    } else {
        switch (bpp) {
        case 1: { const uint32_t t[6] = {b(x, 0), b(x, 1), b(x, 2), b(y, 1), b(y, 0), b(y, 2)}; std::memcpy(bits, t, sizeof t); break; }
        case 2: { const uint32_t t[6] = {b(x, 0), b(x, 1), b(x, 2), b(y, 0), b(y, 1), b(y, 2)}; std::memcpy(bits, t, sizeof t); break; }
        case 4: { const uint32_t t[6] = {b(x, 0), b(x, 1), b(y, 0), b(x, 2), b(y, 1), b(y, 2)}; std::memcpy(bits, t, sizeof t); break; }
        case 8: { const uint32_t t[6] = {b(x, 0), b(y, 0), b(x, 1), b(x, 2), b(y, 1), b(y, 2)}; std::memcpy(bits, t, sizeof t); break; }
        default: { const uint32_t t[6] = {b(y, 0), b(x, 0), b(x, 1), b(x, 2), b(y, 1), b(y, 2)}; std::memcpy(bits, t, sizeof t); break; }
        }
    }
    uint32_t idx = 0;
    for (int i = 0; i < 6; ++i) idx |= bits[i] << i;
    return idx;
}

// 1D (micro-)tiled surface → linear rows of `w` elements. Returns false on a fault.
bool Detile1D(uint8_t* dst, uint64_t src, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp, bool display,
              uint32_t slices, uint64_t slice_stride = 0) {
    const uint32_t pitch_al = (pitch + 7) & ~7u, h_al = (h + 7) & ~7u;
    const uint64_t slice_bytes = slice_stride ? slice_stride : uint64_t(pitch_al) * h_al * bpp;
    uint32_t lut[64];
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) lut[y * 8 + x] = MicroIndex(x, y, bpp, display);
    __try {
        for (uint32_t s = 0; s < slices; ++s) {
            const uint8_t* base = reinterpret_cast<const uint8_t*>(src + s * slice_bytes);
            uint8_t* out = dst + uint64_t(s) * w * h * bpp;
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t ty = y >> 3, iy = y & 7;
                for (uint32_t x = 0; x < w; ++x) {
                    const uint64_t tile = uint64_t(ty) * (pitch_al >> 3) + (x >> 3);
                    const uint64_t off = (tile * 64 + lut[iy * 8 + (x & 7)]) * bpp;
                    std::memcpy(out + (uint64_t(y) * w + x) * bpp, base + off, bpp);
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Mip chain of a 1D-thin (micro-tiled) texture as the GPU lays it out (shadPS4 ComputeImageSize): level i
// is `pitch >> i` wide (rounded up to a power of two with pow2pad, in blocks for BC formats), padded to
// 8x8 elements and widened until a slice is a multiple of 256 bytes; levels follow each other, each one
// holding every slice.
struct MipLevel {
    uint64_t offset = 0, slice_bytes = 0;
    uint32_t pitch = 0;      // elements (blocks), multiple of 8
    uint32_t w = 0, h = 0;   // texels of this level
    uint32_t bw = 0, bh = 0; // elements to detile
};

uint32_t CeilPow2(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

uint64_t MicroMipChain(uint32_t w, uint32_t h, uint32_t pitch, uint32_t levels, uint32_t slices, uint32_t bpp,
                       bool block, bool pow2, MipLevel* out) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < levels; ++i) {
        MipLevel& m = out[i];
        m.w = std::max(w >> i, 1u);
        m.h = std::max(h >> i, 1u);
        m.bw = block ? (m.w + 3) / 4 : m.w;
        m.bh = block ? (m.h + 3) / 4 : m.h;
        uint32_t pw = std::max(pitch >> i, 1u), ph = m.h;
        if (block) { pw = (pw + 3) / 4; ph = (ph + 3) / 4; }
        pw = std::max(pw, 1u);
        ph = std::max(ph, 1u);
        if (pow2) { pw = CeilPow2(pw); ph = CeilPow2(ph); }
        uint32_t pal = (pw + 7) & ~7u;
        const uint32_t hal = (ph + 7) & ~7u;
        while ((uint64_t(pal) * hal * bpp) % 256) pal += 8;
        m.pitch = pal;
        m.slice_bytes = uint64_t(pal) * hal * bpp;
        m.offset = total;
        total += m.slice_bytes * slices;
    }
    return total;
}

// ---------------------------------------------------------------------------------------------------------
// Formats

// CB_COLORn_INFO.FORMAT → component count
uint32_t CbComponents(uint32_t fmt) {
    switch (fmt) {
    case 1: case 2: case 4: return 1;
    case 3: case 5: case 11: return 2;
    case 6: case 7: case 16: return 3;
    default: return 4;
    }
}

VkFormat CbFormat(uint32_t fmt, uint32_t num) {
    // num: 0 unorm, 1 snorm, 4 uint, 5 sint, 6 srgb, 7 float
    auto pick = [&](VkFormat un, VkFormat sn, VkFormat ui, VkFormat si, VkFormat srgb, VkFormat fl) {
        switch (num) {
        case 0: return un;
        case 1: return sn;
        case 4: return ui;
        case 5: return si;
        case 6: return srgb != VK_FORMAT_UNDEFINED ? srgb : un;
        case 7: return fl != VK_FORMAT_UNDEFINED ? fl : un;
        default: return un;
        }
    };
    const VkFormat U = VK_FORMAT_UNDEFINED;
    switch (fmt) {
    case 1: return pick(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8_SRGB, U);
    case 2: return pick(VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, U, VK_FORMAT_R16_SFLOAT);
    case 3: return pick(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SRGB, U);
    case 4: return pick(VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, U, VK_FORMAT_R32_SFLOAT);
    case 5: return pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, U, VK_FORMAT_R16G16_SFLOAT);
    case 6: case 7: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 8: case 9: return pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, U, U);
    case 10: return pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
                         VK_FORMAT_R8G8B8A8_SRGB, U);
    case 11: return pick(VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, U, VK_FORMAT_R32G32_SFLOAT);
    case 12: return pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
                         VK_FORMAT_R16G16B16A16_SINT, U, VK_FORMAT_R16G16B16A16_SFLOAT);
    case 14: return pick(VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT,
                         VK_FORMAT_R32G32B32A32_SINT, U, VK_FORMAT_R32G32B32A32_SFLOAT);
    case 16: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 17: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 18: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 19: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// Output swizzle (memory component ← shader component), 2 bits per destination component.
uint8_t CbSwizzle(uint32_t comps, uint32_t swap) {
    auto enc = [](int x, int y, int z, int w) { return uint8_t(x | (y << 2) | (z << 4) | (w << 6)); };
    switch (comps) {
    case 1: return enc(int(swap), 1, 2, 3);
    case 2: {
        static const int t[4][2] = {{0, 1}, {0, 3}, {1, 0}, {3, 0}};
        return enc(t[swap][0], t[swap][1], 2, 3);
    }
    case 3: {
        static const int t[4][3] = {{0, 1, 2}, {0, 1, 3}, {2, 1, 0}, {3, 1, 0}};
        return enc(t[swap][0], t[swap][1], t[swap][2], 3);
    }
    default: {
        static const int t[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
        return enc(t[swap][0], t[swap][1], t[swap][2], t[swap][3]);
    }
    }
}

// Debug texture dumps: one BC1/BC2/BC3 block (dfmt 35/36/37) → 16 RGBA8 texels, row-major.
void DecodeBcBlock(uint32_t dfmt, const uint8_t* b, uint8_t out[16][4]) {
    const uint8_t* c = dfmt == 35 ? b : b + 8;
    const uint16_t c0 = uint16_t(c[0] | (c[1] << 8)), c1 = uint16_t(c[2] | (c[3] << 8));
    uint8_t pal[4][4];
    auto expand = [](uint16_t v, uint8_t* o) {
        o[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
        o[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
        o[2] = uint8_t((v & 31) * 255 / 31);
        o[3] = 255;
    };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1 || dfmt != 35) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (c0 > c1 || dfmt != 35) ? 255 : 0;
    const uint32_t bits = uint32_t(c[4]) | (uint32_t(c[5]) << 8) | (uint32_t(c[6]) << 16) | (uint32_t(c[7]) << 24);
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
    if (dfmt == 36) { // explicit 4-bit alpha
        for (int i = 0; i < 16; ++i) out[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
    } else if (dfmt == 37) { // interpolated alpha
        uint8_t a[8] = {b[0], b[1]};
        if (a[0] > a[1])
            for (int k = 1; k < 7; ++k) a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
        else {
            for (int k = 1; k < 5; ++k) a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5);
            a[6] = 0;
            a[7] = 255;
        }
        uint64_t ab = 0;
        for (int k = 0; k < 6; ++k) ab |= uint64_t(b[2 + k]) << (8 * k);
        for (int i = 0; i < 16; ++i) out[i][3] = a[(ab >> (3 * i)) & 7];
    }
}

struct TexFormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t bytes = 0; // per texel, or per 4x4 block when block = true
    bool block = false;
    uint32_t kind = 0;  // sampled type: 0 float, 1 uint, 2 sint
};

TexFormatInfo TexFormat(uint32_t dfmt, uint32_t nfmt) {
    // nfmt: 0 unorm 1 snorm 2 uscaled 3 sscaled 4 uint 5 sint 6 snorm_nz 7 float 9 srgb
    TexFormatInfo i;
    const bool srgb = nfmt == 9, sn = nfmt == 1 || nfmt == 6, ui = nfmt == 4, si = nfmt == 5, fl = nfmt == 7;
    i.kind = ui ? 1 : si ? 2 : 0;
    auto p = [&](VkFormat un, VkFormat snf, VkFormat uif, VkFormat sif, VkFormat srgbf, VkFormat flf) {
        if (srgb && srgbf) return srgbf;
        if (sn && snf) return snf;
        if (ui && uif) return uif;
        if (si && sif) return sif;
        if (fl && flf) return flf;
        return un;
    };
    const VkFormat U = VK_FORMAT_UNDEFINED;
    switch (dfmt) {
    case 1: i.format = p(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8_SRGB, U); i.bytes = 1; break;
    case 2: i.format = p(VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, U, VK_FORMAT_R16_SFLOAT); i.bytes = 2; break;
    case 3: i.format = p(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SRGB, U); i.bytes = 2; break;
    case 4: i.format = p(fl ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32_UINT, U, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, U, VK_FORMAT_R32_SFLOAT);
        if (!ui && !si) { i.format = VK_FORMAT_R32_SFLOAT; i.kind = 0; }
        i.bytes = 4; break;
    case 5: i.format = p(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, U, VK_FORMAT_R16G16_SFLOAT); i.bytes = 4; break;
    case 6: case 7: i.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32; i.bytes = 4; i.kind = 0; break;
    case 8: case 9: i.format = p(VK_FORMAT_A2B10G10R10_UNORM_PACK32, U, VK_FORMAT_A2B10G10R10_UINT_PACK32, U, U, U); i.bytes = 4; break;
    case 10: i.format = p(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SRGB, U); i.bytes = 4; break;
    case 11: i.format = p(VK_FORMAT_R32G32_SFLOAT, U, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, U, VK_FORMAT_R32G32_SFLOAT); i.bytes = 8; break;
    case 12: i.format = p(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT, U, VK_FORMAT_R16G16B16A16_SFLOAT); i.bytes = 8; break;
    case 13: i.format = p(VK_FORMAT_R32G32B32_SFLOAT, U, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT, U, VK_FORMAT_R32G32B32_SFLOAT); i.bytes = 12; break;
    case 14: i.format = p(VK_FORMAT_R32G32B32A32_SFLOAT, U, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, U, VK_FORMAT_R32G32B32A32_SFLOAT); i.bytes = 16; break;
    case 16: i.format = VK_FORMAT_R5G6B5_UNORM_PACK16; i.bytes = 2; break;
    case 17: i.format = VK_FORMAT_A1R5G5B5_UNORM_PACK16; i.bytes = 2; break;
    case 18: i.format = VK_FORMAT_R5G5B5A1_UNORM_PACK16; i.bytes = 2; break;
    case 19: i.format = VK_FORMAT_R4G4B4A4_UNORM_PACK16; i.bytes = 2; break;
    case 35: i.format = srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK; i.bytes = 8; i.block = true; break;
    case 36: i.format = srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK; i.bytes = 16; i.block = true; break;
    case 37: i.format = srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK; i.bytes = 16; i.block = true; break;
    case 38: i.format = sn ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK; i.bytes = 8; i.block = true; break;
    case 39: i.format = sn ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK; i.bytes = 16; i.block = true; break;
    case 40: i.format = sn ? VK_FORMAT_BC6H_SFLOAT_BLOCK : VK_FORMAT_BC6H_UFLOAT_BLOCK; i.bytes = 16; i.block = true; break;
    case 41: i.format = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK; i.bytes = 16; i.block = true; break;
    default: break;
    }
    if (i.block) i.kind = 0;
    return i;
}

uint32_t FormatBytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB: return 1;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return 2;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return 8;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 16;
    default: return 4;
    }
}

VkComponentSwizzle DstSel(uint32_t s) {
    switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 1: return VK_COMPONENT_SWIZZLE_ONE;
    case 4: return VK_COMPONENT_SWIZZLE_R;
    case 5: return VK_COMPONENT_SWIZZLE_G;
    case 6: return VK_COMPONENT_SWIZZLE_B;
    case 7: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

VkBlendFactor BlendFactor(uint32_t f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_DST_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 16: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 18: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t f) {
    switch (f) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

VkStencilOp StencilOp(uint32_t o) {
    switch (o) {
    case 1: return VK_STENCIL_OP_ZERO;
    case 2: case 3: case 4: return VK_STENCIL_OP_REPLACE;
    case 5: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 7: return VK_STENCIL_OP_INVERT;
    case 8: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 9: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode AddressMode(uint32_t c) {
    switch (c) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3: case 5: case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Renderer state

struct RenderTarget {
    vk::Image img;
    uint64_t gen = 0;   // bumped whenever the target is rendered to / cleared
    uint64_t readback_gen = ~0ull; // generation last written back to guest memory (tiny targets)
    uint32_t info = 0;  // CB_COLOR_INFO (format identity)
    bool depth = false;
};

struct Texture {
    vk::Image img;
    VkImageView view = VK_NULL_HANDLE; // swizzled view used for sampling
    uint64_t src_gen = ~0ull;          // render-target generation last copied
    uint64_t mem_hash = 0;             // guest-memory content hash (uploaded textures)
    uint32_t layers = 1;
    uint32_t levels = 1;               // mip levels uploaded from guest memory
    uint64_t last_check_submit = ~0ull;
};

struct ShaderEntry {
    std::unique_ptr<TranslatedShader> t;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    bool ok = false;
};

struct PipeKey {
    uint64_t vs = 0, ps = 0;
    VkFormat color[8];
    uint32_t ncolor;
    VkFormat depth;
    uint32_t blend[8];
    uint32_t mask[8];
    uint32_t topology;
    uint32_t pad;
};

struct Pipeline {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct Stats {
    uint64_t draws = 0, drawn = 0, skipped = 0, shaders = 0, shader_fail = 0, pipelines = 0, textures = 0,
             tex_uploads = 0, tex_rt_copies = 0, rts = 0, flips = 0, flips_out = 0, clears = 0, dumped = 0,
             dispatches = 0, dispatched = 0, writebacks = 0, tiny_readbacks = 0, gs_draws = 0;
    double draw_ms = 0, wait_ms = 0, flip_ms = 0; // host time spent recording draws / waiting for the GPU / in flips
    std::map<std::string, uint64_t> skip_reasons;
};

class Renderer {
public:
    Renderer() : dev_(vk::Device::Get()) {}

    bool Init();
    // Debug (GR_DRAW_TRACE): occlusion query per draw of the traced submission (samples passed)
    static constexpr uint32_t kMaxQueries = 8192;
    VkQueryPool qpool_ = VK_NULL_HANDLE;
    bool qtrace_ = false;
    uint32_t qcount_ = 0;
    int32_t last_query_ = -1;
    void BeginSubmission(uint64_t index) {
        submit_index_ = index;
        static const int64_t trace_at = [] { const char* e = std::getenv("GR_DRAW_TRACE"); return e ? std::atoll(e) : -1; }();
        if (int64_t(index) == RenderDocTargetSubmission() && Init()) {
            Flush(); // earlier work stays out of the capture
            RenderDocStartCapture();
        }
        qtrace_ = int64_t(index) == trace_at && Init();
        if (qtrace_) {
            if (!qpool_) {
                VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_OCCLUSION;
                qi.queryCount = kMaxQueries;
                vkCreateQueryPool(dev_.device(), &qi, nullptr, &qpool_);
            }
            if (qpool_) vkCmdResetQueryPool(Cmd(), qpool_, 0, kMaxQueries);
            qcount_ = 0;
        }
    }
    void Draw(const DrawInfo& d, const Regs& r);
    void Dispatch(uint32_t gx, uint32_t gy, uint32_t gz, const Regs& r);
    // Tiny colour targets (exposure / luminance results, up to 8x8) are read back into guest memory:
    // the game reads them on the CPU (e.g. eboot+0x27358f reads the 1x1 average luminance).
    void ReadbackTinyTargets() {
        constexpr uint64_t kBase = 32ull << 20; // past the area used for CPU frame copies
        struct Pending { uint64_t addr, off, bytes; };
        std::vector<Pending> list;
        uint64_t off = kBase;
        for (auto& [addr, rt] : rts_) {
            const uint64_t px = uint64_t(rt.img.width) * rt.img.height;
            if (rt.depth || px > 64 || rt.gen == rt.readback_gen) continue;
            const uint64_t bytes = px * std::max<uint32_t>(FormatBytes(rt.img.format), 1);
            if (off + bytes > readback_.size) break;
            VkCommandBuffer c = Cmd();
            Barrier(c, rt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy bc{};
            bc.bufferOffset = off;
            bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bc.imageExtent = {rt.img.width, rt.img.height, 1};
            vkCmdCopyImageToBuffer(c, rt.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &bc);
            rt.readback_gen = rt.gen;
            list.push_back({addr, off, bytes});
            off = (off + bytes + 255) & ~255ull;
        }
        if (list.empty()) return;
        Flush();
        for (const auto& p : list)
            SafeWrite(p.addr, static_cast<const uint8_t*>(readback_.mapped) + p.off, size_t(p.bytes));
        stats_.tiny_readbacks += list.size();
    }
    void EndSubmission() {
        Flush();
        ReadbackTinyTargets();
        if (int64_t(submit_index_) == RenderDocTargetSubmission()) RenderDocEndCapture();
        if (qtrace_ && qpool_ && qcount_) {
            std::vector<uint64_t> res(qcount_);
            vkGetQueryPoolResults(dev_.device(), qpool_, 0, qcount_, qcount_ * 8, res.data(), 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            for (uint32_t i = 0; i < qcount_; ++i) TraceNote("drawquery %u: %llu samples", i, (unsigned long long)res[i]);
        }
        qtrace_ = false;
        static const int64_t dump_at = [] { const char* e = std::getenv("GR_DUMP_RTS"); return e ? std::atoll(e) : -1; }();
        if (int64_t(submit_index_) == dump_at) DumpRenderTargets();
    }
    void DumpRenderTargets();
    void Flip(int32_t index);
    std::string StatsText();

    bool Acquire(OutputFrame* f) {
        out_m_.lock();
        if (!out_ready_) return false;
        f->image = out_.image;
        f->width = out_w_;
        f->height = out_h_;
        return true;
    }
    void Release() { out_m_.unlock(); }

    std::vector<uint32_t> index_tmp_;
    // Vulkan pipeline cache persisted in <exe>/cache/pipelines.bin (driver-compiled pipelines)
    VkPipelineCache pcache_ = VK_NULL_HANDLE;
    uint64_t saved_pipelines_ = 0;
    void LoadPipelineCache() {
        std::vector<char> data;
        std::ifstream f(ExeDir() / "cache" / "pipelines.bin", std::ios::binary);
        if (f) data.assign(std::istreambuf_iterator<char>(f), {});
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(dev_.device(), &ci, nullptr, &pcache_) != VK_SUCCESS) {
            ci.initialDataSize = 0; // stale/incompatible data
            ci.pInitialData = nullptr;
            vkCreatePipelineCache(dev_.device(), &ci, nullptr, &pcache_);
        }
    }
    void SavePipelineCache() {
        if (!pcache_) return;
        size_t n = 0;
        vkGetPipelineCacheData(dev_.device(), pcache_, &n, nullptr);
        std::vector<char> data(n);
        if (vkGetPipelineCacheData(dev_.device(), pcache_, &n, data.data()) != VK_SUCCESS) return;
        std::error_code ec;
        fs::create_directories(ExeDir() / "cache", ec);
        std::ofstream(ExeDir() / "cache" / "pipelines.bin", std::ios::binary).write(data.data(), std::streamsize(n));
        saved_pipelines_ = stats_.pipelines;
    }
    // filled by BindShaderResources for the current dispatch
    struct Writeback { uint64_t guest, ring_off, size; };
    std::vector<RenderTarget*> written_images_;
    std::vector<Writeback> writebacks_;
    // per-section host time inside Draw (profiling)
    double sec_[8] = {}, mark_last_ = 0;
    void Mark(int i) {
        const double n = NowMs();
        sec_[i] += n - mark_last_;
        mark_last_ = n;
    }

    std::atomic<bool> cpu_frames_{false};
    std::mutex cpu_m_;
    CpuFrame cpu_frame_;

    RendererConfig cfg;

private:
    VkCommandBuffer Cmd();
    void Flush();
public:
    const char* last_skip = nullptr;
    uint64_t SubmitIndex() const { return submit_index_; }
    uint64_t DrawnCount() const { return stats_.drawn + stats_.clears; }
private:
    void Skip(const char* why) {
        last_skip = why;
        stats_.skipped++;
        stats_.skip_reasons[why]++;
    }
    // ring allocation from the upload buffer; returns offset or UINT64_MAX
    uint64_t Alloc(uint64_t size, uint64_t align = 256) {
        uint64_t off = (ring_off_ + align - 1) & ~(align - 1);
        if (off + size > ring_.size) return UINT64_MAX;
        ring_off_ = off + size;
        return off;
    }
    uint8_t* RingPtr(uint64_t off) { return static_cast<uint8_t*>(ring_.mapped) + off; }

    void Barrier(VkCommandBuffer c, vk::Image& img, VkImageLayout to, uint32_t layers = VK_REMAINING_ARRAY_LAYERS);
    void FullBarrier(VkCommandBuffer c);
    bool FormatSupports(VkFormat f, VkFormatFeatureFlags feat);

    RenderTarget* GetColorTarget(uint64_t addr, VkFormat fmt, uint32_t w, uint32_t h, uint32_t info);
    RenderTarget* GetDepthTarget(uint64_t addr, uint32_t w, uint32_t h);
    ShaderEntry* GetShader(const ShaderKey& key);
    Pipeline* GetPipeline(const PipeKey& k, ShaderEntry* vs, ShaderEntry* ps);
    Pipeline* GetComputePipeline(ShaderEntry* e);
    bool PrepareShader(ShaderEntry*& e, ShaderKey& key, const uint32_t* user, std::vector<uint32_t>& sd,
                       ResourceDescriptors& desc);
    RenderTarget* GetStorageImage(const uint32_t* t);
    VkSampler GetSampler(const uint32_t* s);
    Texture* GetTexture(const uint32_t* t, uint32_t& kind);
    VkDescriptorSetLayout EmptyLayout();
    bool BindShaderResources(ShaderEntry* e, const std::vector<uint32_t>& sdata, const ResourceDescriptors& desc,
                             VkDescriptorSet& set, uint32_t max_records);

    // Legacy geometry pipeline (ES → GS → copy VS). The ES and the GS run as compute passes that write the
    // ESGS / GSVS rings; those rings live in two GPU buffers (only shaders read them) sized for a whole draw:
    // the hardware streams waves through 4 MB rings, we shade every wave of the draw at once.
    struct GsDraw {
        bool active = false;
        uint64_t aux_off = 0, aux_size = 0; // GS emit info (read by the copy VS)
        uint32_t nprim = 0, verts = 0;      // copy draw: instances × vertices
        uint32_t params[4] = {};            // ShaderKey::gs_params for the copy VS
    } gs_;
    static constexpr uint64_t kGpuRingSize = 64ull << 20;
    vk::Buffer ring_buf_[2];                // ESGS, GSVS
    uint64_t ring_base_[2] = {};            // guest V# base of each ring for the current draw
    uint64_t aux_off_ = 0, aux_size_ = 0;   // aux binding of the next BindShaderResources (gs_mode shaders)
    bool RunEsGs(const DrawInfo& d, const Regs& r);
    bool RunGsPass(ShaderKey key, const uint32_t* user, uint64_t aux_off, uint64_t aux_size, uint32_t groups);

    vk::Device& dev_;
    bool init_ = false, init_ok_ = false;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool recording_ = false;
    vk::Buffer ring_;
    uint64_t ring_off_ = 0;
    vk::Buffer scratch_; // device-local, depth → colour copies
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    uint32_t sets_used_ = 0;
    VkDescriptorSetLayout empty_layout_ = VK_NULL_HANDLE;
    uint64_t submit_index_ = 0;

    std::unordered_map<uint64_t, RenderTarget> rts_, depths_;
    std::unordered_map<uint64_t, uint64_t> stencil_to_depth_; // stencil plane address -> depth target address
    struct HtileOwner { uint64_t depth; uint32_t w, h; };
    std::unordered_map<uint64_t, HtileOwner> htile_to_depth_; // HTILE buffer address -> depth target
    std::unordered_map<uint64_t, Texture> textures_;
    std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> shaders_;
    std::unordered_map<uint64_t, Pipeline> pipelines_;
    std::unordered_map<uint64_t, VkPipelineLayout> playouts_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<VkFormat, VkFormatFeatureFlags> fmt_feats_;
    std::vector<vk::Image> garbage_; // destroyed after the next fence
    std::vector<VkImageView> garbage_views_;

    std::mutex out_m_;
    vk::Image out_;
    uint32_t out_w_ = 0, out_h_ = 0;
    VkFormat out_fmt_ = VK_FORMAT_UNDEFINED;
    bool out_ready_ = false;
    vk::Buffer readback_;

    Stats stats_;
    uint32_t errors_logged_ = 0;
};

Renderer& R() {
    static Renderer r;
    return r;
}

bool Renderer::Init() {
    if (init_) return init_ok_;
    init_ = true;
    if (!dev_.ok()) {
        TraceNote("renderer: Vulkan unavailable (%s); rendering disabled", dev_.Error().c_str());
        return false;
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev_.queue_family();
    vkCreateCommandPool(dev_.device(), &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev_.device(), &ai, &cmd_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev_.device(), &fci, nullptr, &fence_);
    ring_ = dev_.CreateBuffer(256ull << 20,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              true);
    scratch_ = dev_.CreateBuffer(64ull << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    readback_ = dev_.CreateBuffer(4096ull * 2304 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 65536},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 65536},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 65536},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}};
    VkDescriptorPoolCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dci.maxSets = 16384;
    dci.poolSizeCount = 4;
    dci.pPoolSizes = sizes;
    vkCreateDescriptorPool(dev_.device(), &dci, nullptr, &dpool_);
    if (!ring_.mapped || !dpool_ || !cmd_) {
        TraceNote("renderer: failed to create Vulkan resources");
        return false;
    }
    TraceNote("renderer: using %s (thread %lu)", dev_.gpu_name().c_str(), GetCurrentThreadId());
    LoadPipelineCache();
    init_ok_ = true;
    return true;
}

VkCommandBuffer Renderer::Cmd() {
    if (!recording_) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd_, 0);
        vkBeginCommandBuffer(cmd_, &bi);
        recording_ = true;
    }
    return cmd_;
}

void Renderer::Flush() {
    if (!recording_) return;
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    {
        std::lock_guard<std::mutex> lk(dev_.queue_mutex());
        if (VkResult r = vkQueueSubmit(dev_.queue(), 1, &si, fence_); r != VK_SUCCESS)
            TraceNote("renderer: vkQueueSubmit failed (%d)", int(r));
    }
    const double wait_t0 = NowMs();
    if (VkResult r = vkWaitForFences(dev_.device(), 1, &fence_, VK_TRUE, 5'000'000'000ull); r != VK_SUCCESS)
        TraceNote("renderer: fence wait failed (%d) - device lost?", int(r));
    stats_.wait_ms += NowMs() - wait_t0;
    vkResetFences(dev_.device(), 1, &fence_);
    recording_ = false;
    ring_off_ = 0;
    vkResetDescriptorPool(dev_.device(), dpool_, 0);
    sets_used_ = 0;
    for (auto& g : garbage_) dev_.DestroyImage(g);
    garbage_.clear();
    for (auto v : garbage_views_) vkDestroyImageView(dev_.device(), v, nullptr);
    garbage_views_.clear();
}

void Renderer::FullBarrier(VkCommandBuffer c) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(c, &dep);
}

void Renderer::Barrier(VkCommandBuffer c, vk::Image& img, VkImageLayout to, uint32_t layers) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c, &dep);
    img.layout = to;
}

bool Renderer::FormatSupports(VkFormat f, VkFormatFeatureFlags feat) {
    auto it = fmt_feats_.find(f);
    if (it == fmt_feats_.end()) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(dev_.gpu(), f, &p);
        it = fmt_feats_.emplace(f, p.optimalTilingFeatures).first;
    }
    return (it->second & feat) == feat;
}

RenderTarget* Renderer::GetColorTarget(uint64_t addr, VkFormat fmt, uint32_t w, uint32_t h, uint32_t info) {
    auto it = rts_.find(addr);
    if (it != rts_.end()) {
        RenderTarget& rt = it->second;
        if (rt.img.format == fmt && rt.img.width == w && rt.img.height == h) {
            rt.info = info;
            return &rt;
        }
        garbage_.push_back(rt.img);
        rts_.erase(it);
    }
    if (!FormatSupports(fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return nullptr;
    RenderTarget rt;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (FormatSupports(fmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) usage |= VK_IMAGE_USAGE_STORAGE_BIT; // compute writes
    rt.img = dev_.CreateImage(fmt, w, h, 1, usage);
    if (!rt.img.image) return nullptr;
    rt.info = info;
    VkCommandBuffer c = Cmd();
    Barrier(c, rt.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, rt.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    stats_.rts++;
    TraceNote("renderer: render target 0x%llx %ux%u format %d (CB info 0x%x)", (unsigned long long)addr, w, h, int(fmt), info);
    {
        char label[64];
        std::snprintf(label, sizeof label, "rt 0x%llx %ux%u", (unsigned long long)addr, w, h);
        WatchGuestRange(addr, uint64_t(w) * h * std::max<uint32_t>(FormatBytes(fmt), 1), label);
    }
    return &(rts_[addr] = rt);
}

RenderTarget* Renderer::GetDepthTarget(uint64_t addr, uint32_t w, uint32_t h) {
    auto it = depths_.find(addr);
    if (it != depths_.end()) {
        if (it->second.img.width == w && it->second.img.height == h) return &it->second;
        garbage_.push_back(it->second.img);
        depths_.erase(it);
    }
    RenderTarget rt;
    rt.depth = true;
    rt.img = dev_.CreateImage(VK_FORMAT_D32_SFLOAT_S8_UINT, w, h, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (!rt.img.image) return nullptr;
    VkCommandBuffer c = Cmd();
    Barrier(c, rt.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue cv{1.0f, 0};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(c, rt.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
    TraceNote("renderer: depth target 0x%llx %ux%u", (unsigned long long)addr, w, h);
    {
        char label[64];
        std::snprintf(label, sizeof label, "depth 0x%llx %ux%u", (unsigned long long)addr, w, h);
        WatchGuestRange(addr, uint64_t(w) * h * 4, label);
    }
    return &(depths_[addr] = rt);
}

ShaderEntry* Renderer::GetShader(const ShaderKey& key) {
    const uint64_t h = HashKey(key);
    auto it = shaders_.find(h);
    if (it != shaders_.end()) return it->second.get();
    auto e = std::make_unique<ShaderEntry>();
    const uint32_t* code = reinterpret_cast<const uint32_t*>(key.code);
    const uint32_t* fetch = key.fetch ? reinterpret_cast<const uint32_t*>(key.fetch) : nullptr;
    e->t = Translate(key, code, fetch);
    stats_.shaders++;
    const char* stage = key.stage == Stage::Vertex ? "vs" : "ps";
    // Debug (GR_DUMP_GLSL=1 or <hex address>): writes the generated GLSL to <exe>/glsl
    static const char* dump_glsl = std::getenv("GR_DUMP_GLSL");
    if (dump_glsl && (std::strcmp(dump_glsl, "1") == 0 || std::strtoull(dump_glsl, nullptr, 16) == key.code)) {
        std::error_code ec;
        fs::create_directories(ExeDir() / "glsl", ec);
        char name[64];
        std::snprintf(name, sizeof name, "%s_%010llx_%08x.glsl", stage, (unsigned long long)key.code, uint32_t(h));
        std::ofstream(ExeDir() / "glsl" / name) << e->t->glsl;
    }
    if (!e->t->error.empty()) {
        stats_.shader_fail++;
        if (errors_logged_++ < 32) {
            TraceNote("renderer: %s 0x%llx translation failed: %.400s", stage, (unsigned long long)key.code,
                      e->t->error.c_str());
            std::error_code ec;
            fs::create_directories(ExeDir() / "shader_errors", ec);
            char name[64];
            std::snprintf(name, sizeof name, "%s_%010llx.glsl", stage, (unsigned long long)key.code);
            std::ofstream(ExeDir() / "shader_errors" / name) << e->t->glsl << "\n/*\n" << e->t->error << "\n*/\n";
        }
        return (shaders_[h] = std::move(e)).get();
    }
    if (!e->t->warnings.empty() && errors_logged_++ < 64)
        TraceNote("renderer: %s 0x%llx has unsupported instructions:\n%s", stage, (unsigned long long)key.code,
                  e->t->warnings.c_str());
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = e->t->spirv.size() * 4;
    mci.pCode = e->t->spirv.data();
    vkCreateShaderModule(dev_.device(), &mci, nullptr, &e->module);
    std::vector<VkDescriptorSetLayoutBinding> b;
    const VkShaderStageFlags st = key.stage == Stage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : key.stage == Stage::Compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    b.push_back({0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr});
    for (const auto& r : e->t->resources) {
        VkDescriptorType ty = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (r.kind == ResourceSlot::kTexture) ty = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (r.kind == ResourceSlot::kStorageImage) ty = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        else if (r.kind == ResourceSlot::kSampler) ty = VK_DESCRIPTOR_TYPE_SAMPLER;
        b.push_back({r.binding, ty, 1, st, nullptr});
    }
    if (key.gs_mode != ShaderKey::kGsNone) b.push_back({kAuxBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st, nullptr});
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = uint32_t(b.size());
    lci.pBindings = b.data();
    vkCreateDescriptorSetLayout(dev_.device(), &lci, nullptr, &e->layout);
    e->ok = e->module && e->layout;
    return (shaders_[h] = std::move(e)).get();
}

VkDescriptorSetLayout Renderer::EmptyLayout() {
    if (!empty_layout_) {
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        vkCreateDescriptorSetLayout(dev_.device(), &lci, nullptr, &empty_layout_);
    }
    return empty_layout_;
}

Pipeline* Renderer::GetPipeline(const PipeKey& k, ShaderEntry* vs, ShaderEntry* ps) {
    const uint64_t h = Fnv(&k, sizeof k);
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return &it->second;

    Pipeline p;
    const uint64_t lh = Fnv(&vs->layout, sizeof(VkDescriptorSetLayout),
                            Fnv(ps ? &ps->layout : &empty_layout_, sizeof(VkDescriptorSetLayout)));
    auto lit = playouts_.find(lh);
    if (lit == playouts_.end()) {
        VkDescriptorSetLayout sets[2] = {vs->layout, ps ? ps->layout : EmptyLayout()};
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 2;
        plci.pSetLayouts = sets;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev_.device(), &plci, nullptr, &pl);
        lit = playouts_.emplace(lh, pl).first;
    }
    p.layout = lit->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs->module, "main", nullptr};
    uint32_t nstages = 1;
    if (ps) {
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps->module, "main", nullptr};
        nstages = 2;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VkPrimitiveTopology(k.topology);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[8]{};
    for (uint32_t i = 0; i < k.ncolor; ++i) {
        const uint32_t b = k.blend[i];
        att[i].colorWriteMask = k.mask[i];
        att[i].blendEnable = (b >> 30) & 1;
        att[i].srcColorBlendFactor = BlendFactor(b & 0x1F);
        att[i].colorBlendOp = BlendOp((b >> 5) & 7);
        att[i].dstColorBlendFactor = BlendFactor((b >> 8) & 0x1F);
        if ((b >> 29) & 1) {
            att[i].srcAlphaBlendFactor = BlendFactor((b >> 16) & 0x1F);
            att[i].alphaBlendOp = BlendOp((b >> 21) & 7);
            att[i].dstAlphaBlendFactor = BlendFactor((b >> 24) & 0x1F);
        } else {
            att[i].srcAlphaBlendFactor = att[i].srcColorBlendFactor;
            att[i].alphaBlendOp = att[i].colorBlendOp;
            att[i].dstAlphaBlendFactor = att[i].dstColorBlendFactor;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.ncolor;
    cb.pAttachments = att;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rinfo.colorAttachmentCount = k.ncolor;
    rinfo.pColorAttachmentFormats = k.color;
    rinfo.depthAttachmentFormat = k.depth;
    rinfo.stencilAttachmentFormat = k.depth;
    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.pNext = &rinfo;
    gci.stageCount = nstages;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy;
    gci.layout = p.layout;
    if (VkResult r = vkCreateGraphicsPipelines(dev_.device(), pcache_, 1, &gci, nullptr, &p.pipe); r != VK_SUCCESS) {
        TraceNote("renderer: vkCreateGraphicsPipelines failed (%d)", int(r));
        p.pipe = VK_NULL_HANDLE;
    }
    stats_.pipelines++;
    return &(pipelines_[h] = p);
}

VkSampler Renderer::GetSampler(const uint32_t* s) {
    const uint64_t h = Fnv(s, 16);
    auto it = samplers_.find(h);
    if (it != samplers_.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.addressModeU = AddressMode(s[0] & 7);
    ci.addressModeV = AddressMode((s[0] >> 3) & 7);
    ci.addressModeW = AddressMode((s[0] >> 6) & 7);
    const uint32_t mag = (s[2] >> 20) & 3, min = (s[2] >> 22) & 3, mip = (s[2] >> 26) & 3;
    ci.magFilter = (mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.minLod = float(s[1] & 0xFFF) / 256.0f;
    ci.maxLod = mip == 0 ? 0.25f : float((s[1] >> 12) & 0xFFF) / 256.0f;
    const int32_t bias = int32_t(s[2] << 18) >> 18;
    ci.mipLodBias = float(bias) / 256.0f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const uint32_t bct = s[3] >> 30;
    if (bct == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (bct == 2) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VkSampler smp = VK_NULL_HANDLE;
    vkCreateSampler(dev_.device(), &ci, nullptr, &smp);
    samplers_[h] = smp;
    return smp;
}

// Resolves a T# to a host texture: a copy of a render target when one lives at the address, an upload
// from guest memory otherwise.
Texture* Renderer::GetTexture(const uint32_t* t, uint32_t& kind) {
    const uint64_t addr = ((uint64_t(t[0]) | (uint64_t(t[1] & 0x3F) << 32)) << 8) /* 38-bit base; bits 38-39 are MTYPE_L2 */;
    const uint32_t dfmt = (t[1] >> 20) & 0x3F, nfmt = (t[1] >> 26) & 0xF;
    const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
    const uint32_t type = t[3] >> 28;
    const uint32_t tiling = (t[3] >> 20) & 0x1F;
    const uint32_t depth = (t[4] & 0x1FFF) + 1, pitch = ((t[4] >> 13) & 0x3FFF) + 1;
    const uint32_t last_array = (t[5] >> 13) & 0x1FFF;
    TexFormatInfo fi = TexFormat(dfmt, nfmt);
    kind = fi.kind;
    if (!addr || fi.format == VK_FORMAT_UNDEFINED || w > 16384 || h > 16384) return nullptr;
    auto rt_it = rts_.find(addr);
    auto dt_it = depths_.find(addr);
    RenderTarget* src_rt = rt_it != rts_.end() ? &rt_it->second : dt_it != depths_.end() ? &dt_it->second : nullptr;
    // The stencil plane of a depth buffer lives at its own address (DB_STENCIL_*_BASE).
    bool from_stencil = false;
    if (!src_rt) {
        auto st = stencil_to_depth_.find(addr);
        if (st != stencil_to_depth_.end()) {
            auto d = depths_.find(st->second);
            if (d != depths_.end()) { src_rt = &d->second; from_stencil = true; }
        }
    }
    if (from_stencil) { fi.format = VK_FORMAT_R8_UNORM; fi.bytes = 1; fi.block = false; kind = 0; }
    else if (src_rt && src_rt->depth) { fi.format = VK_FORMAT_R32_SFLOAT; fi.bytes = 4; fi.block = false; kind = 0; }

    uint32_t key_words[8];
    std::memcpy(key_words, t, 32);
    key_words[6] = key_words[7] = 0;
    const uint64_t key = Fnv(key_words, sizeof key_words, from_stencil ? 0x5354 : src_rt ? 0x5254 : 0x4D45);
    Texture* tex;
    auto it = textures_.find(key);
    const bool is_new = it == textures_.end();
    VkCommandBuffer c = Cmd();
    if (is_new) {
        Texture nt;
        VkImageType itype = VK_IMAGE_TYPE_2D;
        VkImageViewType vtype = VK_IMAGE_VIEW_TYPE_2D;
        uint32_t layers = 1, d3 = 1;
        VkImageCreateFlags flags = 0;
        if (!src_rt) {
            if (type == 10) { itype = VK_IMAGE_TYPE_3D; vtype = VK_IMAGE_VIEW_TYPE_3D; d3 = depth; }
            else if (type == 11) { vtype = VK_IMAGE_VIEW_TYPE_CUBE; layers = 6; flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; }
            else if (type == 13) { vtype = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = last_array + 1; }
        }
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = flags;
        ci.imageType = itype;
        ci.format = fi.format;
        ci.extent = {w, h, d3};
        // Mip chains are uploaded for micro-tiled 2D / cube / array textures (the T# last_level).
        uint32_t levels = 1;
        if (!src_rt && type != 10 && (tiling == 13 || tiling == 9 || tiling == 5)) {
            const uint32_t last = (t[3] >> 16) & 0xF;
            uint32_t maxl = 1;
            while ((std::max(w, h) >> maxl) > 0) ++maxl;
            levels = std::min(last + 1, maxl);
        }
        nt.levels = levels;
        ci.mipLevels = levels;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(dev_.device(), &ci, nullptr, &nt.img.image) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev_.device(), nt.img.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = dev_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev_.device(), &ai, nullptr, &nt.img.memory);
        vkBindImageMemory(dev_.device(), nt.img.image, nt.img.memory, 0);
        nt.img.format = fi.format;
        nt.img.width = w;
        nt.img.height = h;
        nt.img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        nt.layers = layers;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = nt.img.image;
        vci.viewType = vtype;
        vci.format = fi.format;
        vci.components = {DstSel(t[3] & 7), DstSel((t[3] >> 3) & 7), DstSel((t[3] >> 6) & 7), DstSel((t[3] >> 9) & 7)};
        if (src_rt && src_rt->depth && !from_stencil) vci.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        vkCreateImageView(dev_.device(), &vci, nullptr, &nt.view);
        Barrier(c, nt.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        if (!fi.block) vkCmdClearColorImage(c, nt.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        tex = &(textures_[key] = nt);
        stats_.textures++;
    } else {
        tex = &it->second;
    }

    if (src_rt) {
        if (tex->src_gen != src_rt->gen) {
            tex->src_gen = src_rt->gen;
            const uint32_t cw = std::min(w, src_rt->img.width), ch = std::min(h, src_rt->img.height);
            Barrier(c, src_rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Barrier(c, tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (src_rt->depth) {
                if (uint64_t(cw) * ch * 4 <= scratch_.size) {
                    VkBufferImageCopy bc{};
                    bc.imageSubresource = {VkImageAspectFlags(from_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT), 0, 0, 1};
                    bc.imageExtent = {cw, ch, 1};
                    vkCmdCopyImageToBuffer(c, src_rt->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch_.buffer, 1, &bc);
                    FullBarrier(c);
                    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    vkCmdCopyBufferToImage(c, scratch_.buffer, tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
                }
            } else if (FormatBytes(src_rt->img.format) == FormatBytes(tex->img.format) && !fi.block) {
                VkImageCopy ic{};
                ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                ic.extent = {cw, ch, 1};
                vkCmdCopyImage(c, src_rt->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex->img.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
            } else {
                Skip("texture/render-target format size mismatch");
            }
            stats_.tex_rt_copies++;
        }
    } else if (is_new || tex->last_check_submit != submit_index_) {
        // Guest-memory texture: (re)upload when the content hash changes (checked once per submission).
        tex->last_check_submit = submit_index_;
        const uint32_t bw = fi.block ? (w + 3) / 4 : w, bh = fi.block ? (h + 3) / 4 : h;
        const uint32_t bpitch = fi.block ? (pitch + 3) / 4 : pitch;
        const uint32_t layers = tex->layers;
        const uint32_t d3 = type == 10 ? depth : 1;
        const bool micro = tiling == 13 || tiling == 9 || tiling == 5;
        MipLevel mips[16];
        const uint32_t nlev = micro ? std::min(tex->levels, 16u) : 1;
        const bool pow2 = micro && ((t[3] >> 25) & 1) != 0;
        uint64_t src_bytes = micro ? MicroMipChain(w, h, pitch, nlev, layers * d3, fi.bytes, fi.block, pow2, mips)
                                   : uint64_t(std::max(bpitch, bw)) * bh * fi.bytes * layers * d3;
        uint64_t bytes = src_bytes;
        if (micro) {
            bytes = 0;
            for (uint32_t i = 0; i < nlev; ++i) bytes += uint64_t(mips[i].bw) * mips[i].bh * fi.bytes * layers * d3;
        }
        if (bytes == 0 || src_bytes > (96ull << 20)) {
            Skip("texture too large");
        } else if (Readable(addr, size_t(src_bytes))) {
            const uint64_t hsh = SampleHash(addr, src_bytes);
            if (is_new || hsh != tex->mem_hash) {
                tex->mem_hash = hsh;
                const uint64_t off = Alloc(bytes + 16ull * nlev, 16);
                if (off != UINT64_MAX) {
                    Barrier(c, tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    if (micro) {
                        VkBufferImageCopy bcs[16]{};
                        uint64_t dst = off;
                        for (uint32_t i = 0; i < nlev; ++i) {
                            const MipLevel& m = mips[i];
                            Detile1D(RingPtr(dst), addr + m.offset, m.bw, m.bh, m.pitch, fi.bytes, tiling == 9,
                                     layers * d3, m.slice_bytes);
                            VkBufferImageCopy& bc = bcs[i];
                            bc.bufferOffset = dst;
                            bc.bufferRowLength = m.bw * (fi.block ? 4 : 1);
                            bc.bufferImageHeight = m.bh * (fi.block ? 4 : 1);
                            bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, layers};
                            bc.imageExtent = {m.w, m.h, d3};
                            dst += uint64_t(m.bw) * m.bh * fi.bytes * layers * d3;
                            dst = (dst + 15) & ~15ull;
                        }
                        vkCmdCopyBufferToImage(c, ring_.buffer, tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nlev, bcs);
                    } else {
                        SafeCopy(RingPtr(off), addr, size_t(bytes));
                        VkBufferImageCopy bc{};
                        bc.bufferOffset = off;
                        bc.bufferRowLength = std::max(pitch, w);
                        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
                        bc.imageExtent = {w, h, d3};
                        vkCmdCopyBufferToImage(c, ring_.buffer, tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
                    }
                    stats_.tex_uploads++;
                    if (tiling != 8 && tiling != 31 && !micro) {
                        stats_.skip_reasons["texture uploaded without detiling"]++;
                        TraceNote("renderer: texture 0x%llx %ux%u fmt %u/%u tiling %u type %u uploaded without detiling", (unsigned long long)addr, w, h, dfmt, nfmt, tiling, type);
                    }
                    static const bool dump_tex = std::getenv("GR_DUMP_TEX") != nullptr;
                    if (dump_tex && fi.block && (dfmt >= 35 && dfmt <= 37) && layers * d3 == 1) {
                        // BC1/BC2/BC3 → RGBA8 for inspection
                        std::vector<uint8_t> px(size_t(w) * h * 4);
                        const uint8_t* srcp = RingPtr(off);
                        const uint32_t row_blocks = micro ? bw : std::max(bpitch, bw);
                        for (uint32_t by = 0; by < bh; ++by)
                            for (uint32_t bx = 0; bx < bw; ++bx) {
                                const uint8_t* blk = srcp + (size_t(by) * row_blocks + bx) * fi.bytes;
                                uint8_t rgba[16][4];
                                DecodeBcBlock(dfmt, blk, rgba);
                                for (uint32_t k = 0; k < 16; ++k) {
                                    const uint32_t x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
                                    if (x < w && y < h) std::memcpy(&px[(size_t(y) * w + x) * 4], rgba[k], 4);
                                }
                            }
                        std::error_code ec;
                        fs::create_directories(ExeDir() / "textures", ec);
                        char name[96];
                        std::snprintf(name, sizeof name, "tex_%010llx_%llu_bc%u.png", (unsigned long long)addr,
                                      (unsigned long long)submit_index_, dfmt - 34);
                        stbi_write_png((ExeDir() / "textures" / name).string().c_str(), int(w), int(h), 4, px.data(), int(w * 4));
                    }
                    if (dump_tex && !fi.block && (fi.bytes == 1 || fi.bytes == 4) && layers * d3 == 1) {
                        std::vector<uint8_t> px(size_t(w) * h * 4);
                        const uint8_t* srcp = RingPtr(off);
                        const uint32_t row = micro ? bw : std::max(pitch, w);
                        for (uint32_t y = 0; y < h; ++y)
                            for (uint32_t x = 0; x < w; ++x) {
                                uint8_t* o = &px[(size_t(y) * w + x) * 4];
                                if (fi.bytes == 1) { o[0] = o[1] = o[2] = srcp[size_t(y) * row + x]; o[3] = 255; }
                                else std::memcpy(o, srcp + (size_t(y) * row + x) * 4, 4);
                            }
                        std::error_code ec;
                        fs::create_directories(ExeDir() / "textures", ec);
                        char name[96];
                        std::snprintf(name, sizeof name, "tex_%010llx_%llu.png", (unsigned long long)addr,
                                      (unsigned long long)submit_index_);
                        stbi_write_png((ExeDir() / "textures" / name).string().c_str(), int(w), int(h), 4, px.data(), int(w * 4));
                    }
                    if (stats_.tex_uploads <= 64)
                        TraceNote("renderer: texture 0x%llx %ux%u pitch %u fmt %u/%u tiling %u type %u layers %u", (unsigned long long)addr,
                                  w, h, pitch, dfmt, nfmt, tiling, type, layers);
                }
            }
        } else {
            Skip("texture memory unreadable");
            static const bool dbg_one = std::getenv("GR_DEBUG_UNREADABLE_ONE") != nullptr;
            if (dbg_one && is_new && !fi.block) {
                VkClearColorValue one{};
                one.float32[0] = one.float32[1] = one.float32[2] = one.float32[3] = 1.0f;
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, tex->layers};
                Barrier(c, tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdClearColorImage(c, tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
            }
            static int logged = 0;
            if (logged++ < 24 || qtrace_)
                TraceNote("renderer: unreadable texture 0x%llx %ux%u fmt %u tiling %u bytes %llu (submission %llu) T# %08x %08x %08x %08x %08x %08x",
                          (unsigned long long)addr, w, h, dfmt, tiling, (unsigned long long)src_bytes,
                          (unsigned long long)submit_index_, t[0], t[1], t[2], t[3], t[4], t[5]);
        }
    }
    Barrier(c, tex->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return tex;
}

bool Renderer::BindShaderResources(ShaderEntry* e, const std::vector<uint32_t>& sdata, const ResourceDescriptors& desc,
                                   VkDescriptorSet& set, uint32_t max_records) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = dpool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &e->layout;
    if (vkAllocateDescriptorSets(dev_.device(), &ai, &set) != VK_SUCCESS) return false;
    sets_used_++;
    const auto& t = *e->t;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufs(t.resources.size() + 1);
    std::vector<VkDescriptorImageInfo> imgs(t.resources.size());
    writes.reserve(t.resources.size() + 1);

    const uint64_t sd_bytes = std::max<uint64_t>(sdata.size() * 4, 16);
    const uint64_t sd_off = Alloc(sd_bytes);
    if (sd_off == UINT64_MAX) return false;
    std::memcpy(RingPtr(sd_off), sdata.data(), sdata.size() * 4);
    bufs[0] = {ring_.buffer, sd_off, sd_bytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bufs[0];
    writes.push_back(w);
    VkDescriptorBufferInfo aux_info{ring_.buffer, aux_off_, std::max<uint64_t>(aux_size_, 16)};
    if (t.key.gs_mode != ShaderKey::kGsNone) {
        VkWriteDescriptorSet wa = w;
        wa.dstBinding = kAuxBinding;
        wa.pBufferInfo = &aux_info;
        writes.push_back(wa);
    }

    for (size_t i = 0; i < t.resources.size(); ++i) {
        const auto& r = t.resources[i];
        const uint32_t* d = desc.values[i].data();
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set;
        wr.dstBinding = r.binding;
        wr.descriptorCount = 1;
        if (r.kind == ResourceSlot::kTexture) {
            uint32_t kind = 0;
            Texture* tx = nullptr;
            // Debug (GR_DEBUG_BLACK_TEX=<hex shader>:<resource>): replaces one texture of one shader with black
            static const std::pair<uint64_t, size_t> black_tex = [] {
                const char* e = std::getenv("GR_DEBUG_BLACK_TEX");
                if (!e) return std::pair<uint64_t, size_t>{0, 0};
                char* end = nullptr;
                const uint64_t sh = std::strtoull(e, &end, 16);
                return std::pair<uint64_t, size_t>{sh, end && *end == ':' ? size_t(std::strtoul(end + 1, nullptr, 10)) : 0};
            }();
            const bool force_black = black_tex.first && black_tex.first == e->t->key.code && black_tex.second == i;
            if (force_black || !(d[0] | d[1] | d[2] | d[3])) {
                // null T#: GCN returns zeros, so bind a 1x1 black texture
                {
                    static std::set<std::pair<uint64_t, size_t>> seen;
                    if (seen.size() < 64 && seen.insert({e->t->key.code, i}).second)
                        TraceNote("renderer: null T# for shader 0x%llx resource %zu (instr %u, s%u, submission %llu)",
                                  (unsigned long long)e->t->key.code, i, r.instr, unsigned(r.sgpr),
                                  (unsigned long long)submit_index_);
                }
                static const uint32_t null_t[8] = {0xFFFFFF00u, 0x00A00000u /* 8888 unorm */, 0, 0x00000FACu | (9u << 28), 0, 0, 0, 0};
                tx = GetTexture(null_t, kind);
            } else {
                tx = GetTexture(d, kind);
            }
            if (!tx) {
                static int logged = 0;
                if (logged++ < 16)
                    TraceNote("renderer: unresolved T# %08x %08x %08x %08x %08x %08x %08x %08x (shader 0x%llx)", d[0], d[1],
                              d[2], d[3], d[4], d[5], d[6], d[7], (unsigned long long)e->t->key.code);
                // the GPU would read unmapped memory as zeros: bind the black texture and keep the draw
                static const uint32_t null_t[8] = {0xFFFFFF00u, 0x00A00000u, 0, 0x00000FACu | (9u << 28), 0, 0, 0, 0};
                uint32_t null_kind = 0;
                tx = GetTexture(null_t, null_kind);
                if (!tx) return false;
            }
            imgs[i] = {VK_NULL_HANDLE, tx->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            if (qtrace_)
                TraceNote("  tex %zu addr 0x%llx %ux%u fmt %u/%u tiling %u type %u levels %u-%u", i,
                          (unsigned long long)((uint64_t(d[0]) | (uint64_t(d[1] & 0x3F) << 32)) << 8), (d[2] & 0x3FFF) + 1,
                          ((d[2] >> 14) & 0x3FFF) + 1, (d[1] >> 20) & 0x3F, (d[1] >> 26) & 0xF, (d[3] >> 20) & 0x1F, d[3] >> 28,
                          (d[3] >> 12) & 0xF, (d[3] >> 16) & 0xF);
            static int raw_logged = 0;
            if ((d[3] >> 28) == 13 && raw_logged++ < 12)
                TraceNote("  raw T# %08x %08x %08x %08x %08x %08x %08x %08x (shader 0x%llx, slot %zu sgpr %u, submission %llu)", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], (unsigned long long)e->t->key.code, i, unsigned(r.sgpr), (unsigned long long)submit_index_);
            wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            wr.pImageInfo = &imgs[i];
        } else if (r.kind == ResourceSlot::kSampler) {
            imgs[i] = {GetSampler(d), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            wr.pImageInfo = &imgs[i];
        } else if (r.kind == ResourceSlot::kStorageImage) {
            RenderTarget* rt = GetStorageImage(d);
            if (!rt) return false;
            Barrier(Cmd(), rt->img, VK_IMAGE_LAYOUT_GENERAL);
            written_images_.push_back(rt);
            imgs[i] = {VK_NULL_HANDLE, rt->img.view, VK_IMAGE_LAYOUT_GENERAL};
            wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wr.pImageInfo = &imgs[i];
        } else {
            const uint64_t base = uint64_t(d[0]) | (uint64_t(d[1] & 0xFF) << 32);
            const uint32_t stride = (d[1] >> 16) & 0x3FFF;
            // geometry-pipeline rings stay on the GPU
            bool ring_bound = false;
            for (int k = 0; k < 2; ++k)
                if (ring_base_[k] && base == ring_base_[k] && ring_buf_[k].buffer) {
                    bufs[i + 1] = {ring_buf_[k].buffer, 0, kGpuRingSize};
                    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    wr.pBufferInfo = &bufs[i + 1];
                    writes.push_back(wr);
                    ring_bound = true;
                    break;
                }
            if (ring_bound) continue;
            // index-fetched vertex buffers only need the records the draw can reach
            const uint64_t records = (r.kind == ResourceSlot::kVertexBuffer && r.vertex_indexed && stride)
                                         ? std::min<uint64_t>(d[2], max_records) : d[2];
            uint64_t size = stride ? records * stride : d[2];
            size = std::min<uint64_t>(size, 32ull << 20);
            const uint64_t alloc = size + 64;
            const uint64_t off = Alloc(alloc);
            if (off == UINT64_MAX) return false;
            std::memset(RingPtr(off + size), 0, 64);
            if (size) SafeCopy(RingPtr(off), base, size_t(size));
            if (qtrace_) {
                float f[64] = {};
                const size_t nf = size_t(std::min<uint64_t>(std::max<uint64_t>(size / 4, 8), 64));
                SafeCopy(f, base, nf * 4);
                std::string vals;
                for (size_t k = 0; k < nf; ++k) vals += std::to_string(f[k]).substr(0, 9) + " ";
                if (r.kind == ResourceSlot::kRawBuffer && size >= (1u << 20)) { // large raw buffer: where is data?
                    std::vector<uint32_t> hv(static_cast<size_t>(size / 4));
                    SafeCopy(hv.data(), base, hv.size() * 4);
                    size_t nz = 0, first = SIZE_MAX, last = 0;
                    for (size_t k = 0; k < hv.size(); ++k)
                        if (hv[k]) { ++nz; first = std::min(first, k); last = k; }
                    char nb[160];
                    std::snprintf(nb, sizeof nb, "nonzero %zu dwords in [0x%zx, 0x%zx] ", nz, first == SIZE_MAX ? 0 : first * 4,
                                  last * 4);
                    vals = nb + vals;
                }
                if (r.vertex_indexed && stride >= 12 && size >= stride) { // bounding box of the first 3 floats
                    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
                    const auto* p = static_cast<const uint8_t*>(RingPtr(off));
                    std::vector<uint8_t> hv(static_cast<size_t>(size));
                    SafeCopy(hv.data(), base, size_t(size));
                    for (uint64_t o = 0; o + 12 <= size; o += stride) {
                        float v3[3];
                        std::memcpy(v3, hv.data() + o, 12);
                        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], v3[k]); mx[k] = std::max(mx[k], v3[k]); }
                    }
                    (void)p;
                    char bb[160];
                    std::snprintf(bb, sizeof bb, "bbox (%g %g %g)-(%g %g %g) ", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
                    vals = bb + vals;
                }
                TraceNote("  res %zu kind %d w3 %08x base 0x%llx stride %u records %u copied %llu vidx %d: %s", i, int(r.kind), d[3],
                          (unsigned long long)base, stride, d[2], (unsigned long long)size, int(r.vertex_indexed),
                          vals.c_str());
            }
            if (r.written && size) writebacks_.push_back({base, off, size});
            bufs[i + 1] = {ring_.buffer, off, alloc};
            wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr.pBufferInfo = &bufs[i + 1];
        }
        writes.push_back(wr);
    }
    vkUpdateDescriptorSets(dev_.device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
    return true;
}

uint32_t ShaderHashAndScan(uint64_t addr, bool* has_swappc) {
    const auto* code = reinterpret_cast<const uint32_t*>(addr);
    const uint32_t size = ShaderCodeSize(code, 16384);
    *has_swappc = false;
    for (uint32_t i = 0; i < size / 4; ++i)
        if ((code[i] >> 23) == 0x17D && ((code[i] >> 8) & 0xFF) == 33) *has_swappc = true;
    return uint32_t(Fnv(code, size));
}

VkPrimitiveTopology Topology(uint32_t prim, bool* ok) {
    *ok = true;
    switch (prim) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 0x11: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; // rect list: 3 vertices drawn as a strip (approx.)
    case 0x13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // quad list: converted with an index buffer
    default: *ok = false; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// Image written by a compute shader (image_store): a render-target image keyed by the T# address, so
// later texture reads of that address pick up the result.
RenderTarget* Renderer::GetStorageImage(const uint32_t* t) {
    const uint64_t addr = ((uint64_t(t[0]) | (uint64_t(t[1] & 0x3F) << 32)) << 8) /* 38-bit base; bits 38-39 are MTYPE_L2 */;
    const uint32_t w = (t[2] & 0x3FFF) + 1, h = ((t[2] >> 14) & 0x3FFF) + 1;
    TexFormatInfo fi = TexFormat((t[1] >> 20) & 0x3F, (t[1] >> 26) & 0xF);
    if (!addr || fi.block || fi.format == VK_FORMAT_UNDEFINED) { Skip("compute: unsupported storage image"); return nullptr; }
    switch (fi.format) { // storage images cannot be sRGB
    case VK_FORMAT_R8G8B8A8_SRGB: fi.format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case VK_FORMAT_R8G8_SRGB: fi.format = VK_FORMAT_R8G8_UNORM; break;
    case VK_FORMAT_R8_SRGB: fi.format = VK_FORMAT_R8_UNORM; break;
    default: break;
    }
    if (!FormatSupports(fi.format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { Skip("compute: storage format unsupported"); return nullptr; }
    RenderTarget* rt = GetColorTarget(addr, fi.format, w, h, 0);
    if (!rt) Skip("compute: storage image creation failed");
    return rt;
}

// Pre-executes the scalar code, and re-translates the shader when the bound textures need other image
// types (3D / cube / array, integer formats) than the ones it was compiled with.
bool Renderer::PrepareShader(ShaderEntry*& e, ShaderKey& key, const uint32_t* user, std::vector<uint32_t>& sd,
                             ResourceDescriptors& desc) {
    ScalarPreExecute(*e->t, user, sd, desc);
    bool changed = false;
    for (size_t i = 0; i < e->t->resources.size() && i < 64; ++i) {
        if (e->t->resources[i].kind != ResourceSlot::kTexture) continue;
        const uint32_t* t = desc.values[i].data();
        const uint64_t addr = ((uint64_t(t[0]) | (uint64_t(t[1] & 0x3F) << 32)) << 8) /* 38-bit base; bits 38-39 are MTYPE_L2 */;
        const bool from_rt = rts_.count(addr) || depths_.count(addr);
        const uint32_t type = t[3] >> 28;
        uint32_t dim = 0;
        if (!from_rt) dim = type == 10 ? 1 : type == 11 ? 2 : type == 13 ? 3 : 0;
        const TexFormatInfo fi = TexFormat((t[1] >> 20) & 0x3F, (t[1] >> 26) & 0xF);
        const uint32_t kind = depths_.count(addr) ? 0 : fi.kind;
        const uint8_t v = uint8_t(dim | (kind << 4));
        if (key.image_dims[i] != v) { key.image_dims[i] = v; changed = true; }
    }
    if (changed) {
        e = GetShader(key);
        if (!e->ok) return false;
        ScalarPreExecute(*e->t, user, sd, desc);
    }
    return true;
}

void Renderer::Dispatch(uint32_t gx, uint32_t gy, uint32_t gz, const Regs& r) {
    stats_.dispatches++;
    if (!Init()) return;
    if (!gx || !gy || !gz) return;
    if (ring_off_ > ring_.size / 2 || sets_used_ > 12000) Flush();
    const uint64_t cs_addr = PgmAddr(r, reg::COMPUTE_PGM_LO);
    if (!cs_addr || !Readable(cs_addr, 64)) { Skip("compute: no shader"); return; }
    const uint32_t rsrc2 = r.S(0x2E13);
    ShaderKey key;
    std::memset(&key, 0, sizeof key);
    key.stage = Stage::Compute;
    key.code = cs_addr;
    bool dummy;
    key.code_hash = ShaderHashAndScan(cs_addr, &dummy);
    key.user_sgprs = (rsrc2 >> 1) & 0x1F;
    key.cs_sys_sgprs = uint8_t(((rsrc2 >> 7) & 7) | (((rsrc2 >> 10) & 1) << 3));
    key.lds_dwords = ((rsrc2 >> 15) & 0x1FF) * 128;
    for (int i = 0; i < 3; ++i) key.cs_threads[i] = uint16_t(std::max<uint32_t>(r.S(0x2E07 + i) & 0xFFFF, 1));
    uint32_t user[16];
    for (uint32_t i = 0; i < 16; ++i) user[i] = r.S(0x2E40 + i);

    ShaderEntry* e = GetShader(key);
    if (!e->ok) { Skip("compute shader failed"); return; }
    std::vector<uint32_t> sdata;
    ResourceDescriptors desc;
    if (!PrepareShader(e, key, user, sdata, desc)) { Skip("compute shader failed"); return; }
    written_images_.clear();
    writebacks_.clear();
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!BindShaderResources(e, sdata, desc, set, ~0u)) { Skip("compute resources"); return; }
    Pipeline* pipe = GetComputePipeline(e);
    if (!pipe->pipe) { Skip("compute pipeline creation failed"); return; }

    VkCommandBuffer c = Cmd();
    for (RenderTarget* rt : written_images_) Barrier(c, rt->img, VK_IMAGE_LAYOUT_GENERAL);
    FullBarrier(c);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(c, gx, gy, gz);
    FullBarrier(c);
    for (RenderTarget* rt : written_images_) rt->gen++;
    stats_.dispatched++;
    // A compute shader that rewrites a depth buffer's HTILE is a fast clear: the depth then reads back as
    // DB_DEPTH_CLEAR / DB_STENCIL_CLEAR. Apply that to the host depth image.
    for (auto it = writebacks_.begin(); it != writebacks_.end();) {
        auto h = htile_to_depth_.find(it->guest);
        if (h == htile_to_depth_.end()) { ++it; continue; }
        if (RenderTarget* dt = GetDepthTarget(h->second.depth, h->second.w, h->second.h)) {
            Barrier(c, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearDepthStencilValue cv{r.Cf(0xA00B), r.C(0xA00A) & 0xFF};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(c, dt->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
            dt->gen++;
            stats_.clears++;
        }
        it = writebacks_.erase(it); // the HTILE contents themselves are not used on the host
    }
    if (!writebacks_.empty()) {
        // Buffer stores land in guest memory: run the work now and copy the results back, so the CPU and
        // later draws (which read guest memory when they are recorded) see them.
        Flush();
        for (const auto& wb : writebacks_) {
            std::vector<uint8_t> tmp(size_t(wb.size));
            std::memcpy(tmp.data(), RingPtr(wb.ring_off), tmp.size());
            SafeWrite(wb.guest, tmp.data(), tmp.size());
        }
        stats_.writebacks += writebacks_.size();
    }
}

// One compute pass of the emulated geometry pipeline (ES or GS), 64 invocations per group.
bool Renderer::RunGsPass(ShaderKey key, const uint32_t* user, uint64_t aux_off, uint64_t aux_size, uint32_t groups) {
    ShaderEntry* e = GetShader(key);
    if (!e->ok) return false;
    std::vector<uint32_t> sdata;
    ResourceDescriptors desc;
    if (!PrepareShader(e, key, user, sdata, desc)) return false;
    written_images_.clear();
    writebacks_.clear();
    aux_off_ = aux_off;
    aux_size_ = aux_size;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!BindShaderResources(e, sdata, desc, set, ~0u)) return false;
    writebacks_.clear(); // ring stores stay on the GPU; nothing else is written back
    Pipeline* pipe = GetComputePipeline(e);
    if (!pipe->pipe) return false;
    VkCommandBuffer c = Cmd();
    FullBarrier(c);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(c, groups, 1, 1);
    FullBarrier(c);
    return true;
}

// ES → GS for a draw with the geometry stages enabled (VGT_SHADER_STAGES_EN ES + GS). The ES shades each
// index of the draw (lane i of wave i/64, as the hardware would group them) into the ESGS ring; the GS
// shades each triangle into the GSVS ring and records how many vertices it emitted and where its strips
// were cut. The draw itself then becomes the copy VS over those strips (gs_).
bool Renderer::RunEsGs(const DrawInfo& d, const Regs& r) {
    gs_ = {};
    const uint32_t prim = r.uc[0x242];
    if (prim != 4) { Skip("geometry pipeline: only triangle lists"); return false; }
    const uint32_t count = d.index_count, nprim = count / 3;
    if (!nprim) { Skip("geometry pipeline: empty draw"); return false; }
    const uint32_t es_item = r.C(0xA2AB) & 0x7FFF, gsvs_item = r.C(0xA2AC) & 0x7FFF, maxv = r.C(0xA2CE) & 0x7FF;
    if (!es_item || !gsvs_item || maxv < 3 || maxv > 32) { Skip("geometry pipeline: ring layout"); return false; }

    // ring descriptors: the GS's table (GS user s[0:1]); ESGS written by the ES at +0x20, GSVS written by the
    // GS at +0x40 (the copy VS reads the GSVS through a linear view of the same memory)
    const uint64_t tbl = uint64_t(r.S(0x2C8C)) | (uint64_t(r.S(0x2C8D)) << 32);
    uint32_t t[0x14] = {};
    if (!tbl || !SafeCopy(t, tbl, sizeof t)) { Skip("geometry pipeline: ring table"); return false; }
    const uint64_t esgs = uint64_t(t[0x08]) | (uint64_t(t[0x09] & 0xFF) << 32);
    const uint64_t gsvs = uint64_t(t[0x10]) | (uint64_t(t[0x11] & 0xFF) << 32);
    if (!esgs || !gsvs || esgs == gsvs) { Skip("geometry pipeline: ring table"); return false; }
    if (uint64_t((count + 63) / 64) * 64 * es_item * 4 > kGpuRingSize ||
        uint64_t((nprim + 63) / 64) * 64 * gsvs_item * 4 > kGpuRingSize) {
        Skip("geometry pipeline: draw too large for the rings");
        return false;
    }
    for (int k = 0; k < 2; ++k)
        if (!ring_buf_[k].buffer) ring_buf_[k] = dev_.CreateBuffer(kGpuRingSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    ring_base_[0] = esgs;
    ring_base_[1] = gsvs;

    // ES pass: aux = [count, indices...]
    const uint64_t es_aux = Alloc(uint64_t(count + 1) * 4, 16);
    const uint64_t gs_aux = Alloc(uint64_t(nprim + 1) * 4, 16);
    if (es_aux == UINT64_MAX || gs_aux == UINT64_MAX) { Skip("geometry pipeline: aux upload"); return false; }
    std::vector<uint32_t> aux(size_t(count) + 1);
    aux[0] = count;
    if (d.index_base) {
        if (d.index_type == 1) {
            SafeCopy(aux.data() + 1, d.index_base, size_t(count) * 4);
        } else {
            std::vector<uint16_t> ix(count);
            SafeCopy(ix.data(), d.index_base, size_t(count) * 2);
            for (uint32_t i = 0; i < count; ++i) aux[1 + i] = ix[i];
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) aux[1 + i] = i;
    }
    std::memcpy(RingPtr(es_aux), aux.data(), aux.size() * 4);
    const uint32_t gs_head[1] = {nprim};
    std::memcpy(RingPtr(gs_aux), gs_head, 4);

    const uint32_t es_chunk = 64 * es_item * 4, gs_chunk = 64 * gsvs_item * 4;
    ShaderKey es{};
    std::memset(&es, 0, sizeof es);
    es.stage = Stage::Compute;
    es.code = PgmAddr(r, reg::SPI_SHADER_PGM_LO_ES);
    if (!es.code || !Readable(es.code, 64)) { Skip("geometry pipeline: no ES"); return false; }
    bool swappc = false;
    es.code_hash = ShaderHashAndScan(es.code, &swappc);
    uint32_t es_user[16], gs_user[16];
    for (uint32_t i = 0; i < 16; ++i) {
        es_user[i] = r.S(0x2CCC + i);
        gs_user[i] = r.S(0x2C8C + i);
    }
    if (swappc) es.fetch = uint64_t(es_user[0]) | (uint64_t(es_user[1]) << 32);
    es.user_sgprs = (r.S(0x2CCB) >> 1) & 0x1F;
    es.cs_threads[0] = 64;
    es.cs_threads[1] = es.cs_threads[2] = 1;
    es.gs_mode = ShaderKey::kGsExport;
    es.gs_params[0] = es_chunk;
    if (!RunGsPass(es, es_user, es_aux, uint64_t(count + 1) * 4, (count + 63) / 64)) {
        Skip("geometry pipeline: ES failed");
        return false;
    }

    ShaderKey gs{};
    std::memset(&gs, 0, sizeof gs);
    gs.stage = Stage::Compute;
    gs.code = PgmAddr(r, reg::SPI_SHADER_PGM_LO_GS);
    if (!gs.code || !Readable(gs.code, 64)) { Skip("geometry pipeline: no GS"); return false; }
    gs.code_hash = ShaderHashAndScan(gs.code, &swappc);
    gs.user_sgprs = (r.S(0x2C8B) >> 1) & 0x1F;
    gs.cs_threads[0] = 64;
    gs.cs_threads[1] = gs.cs_threads[2] = 1;
    gs.gs_mode = ShaderKey::kGsGeometry;
    gs.gs_params[0] = es_chunk;
    gs.gs_params[1] = gs_chunk;
    if (!RunGsPass(gs, gs_user, gs_aux, uint64_t(nprim + 1) * 4, (nprim + 63) / 64)) {
        Skip("geometry pipeline: GS failed");
        return false;
    }

    gs_.active = true;
    gs_.aux_off = gs_aux;
    gs_.aux_size = uint64_t(nprim + 1) * 4;
    gs_.nprim = nprim;
    gs_.verts = (maxv - 2) * 3;
    const uint32_t vstride = (t[0x11] >> 16) & 0x3FFF, is = 8u << ((t[0x13] >> 21) & 3), elem = 2u << ((t[0x13] >> 19) & 3);
    gs_.params[0] = gs_chunk;
    gs_.params[1] = maxv;
    gs_.params[2] = vstride;
    gs_.params[3] = is | (elem << 8);
    stats_.gs_draws++;
    return true;
}

Pipeline* Renderer::GetComputePipeline(ShaderEntry* e) {
    const uint64_t h = HashKey(e->t->key) ^ 0xC0C0C0C0ull;
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return &it->second;
    Pipeline p;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &e->layout;
    vkCreatePipelineLayout(dev_.device(), &plci, nullptr, &p.layout);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, e->module, "main", nullptr};
    // GCN compute code assumes 64 lanes run in lockstep (LDS exchanges without s_barrier).
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rs.requiredSubgroupSize = 64;
    const auto& k = e->t->key;
    const uint32_t total = uint32_t(k.cs_threads[0]) * k.cs_threads[1] * k.cs_threads[2];
    if (dev_.wave64() && total % 64 == 0) {
        ci.stage.pNext = &rs;
        if (k.cs_threads[0] % 64 == 0) ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    ci.layout = p.layout;
    if (VkResult res = vkCreateComputePipelines(dev_.device(), pcache_, 1, &ci, nullptr, &p.pipe); res != VK_SUCCESS) {
        TraceNote("renderer: vkCreateComputePipelines failed (%d)", int(res));
        p.pipe = VK_NULL_HANDLE;
    }
    stats_.pipelines++;
    return &(pipelines_[h] = p);
}

void Renderer::Draw(const DrawInfo& d, const Regs& r) {
    const double t0 = NowMs();
    struct Acc { double t0; double& acc; ~Acc() { acc += NowMs() - t0; } } acc{t0, stats_.draw_ms};
    mark_last_ = t0;
    written_images_.clear(); // stores from VS/PS are not tracked yet
    writebacks_.clear();
    stats_.draws++;
    if (!Init()) return;
    if (ring_off_ > ring_.size / 2 || sets_used_ > 12000) Flush();

    // ---- targets
    const uint32_t color_ctl = r.C(0xA202);
    const uint32_t cb_mode = (color_ctl >> 4) & 7;
    const uint32_t target_mask = r.C(0xA08E);
    const uint32_t depth_ctl = r.C(0xA200);
    const uint32_t z_info = r.C(0xA010), s_info = r.C(0xA011);
    const uint32_t db_render = r.C(0xA000);
    const uint64_t z_addr = uint64_t(r.C(0xA012)) << 8;
    const uint32_t db_size = r.C(0xA016);
    const uint32_t dw = ((db_size & 0x7FF) + 1) * 8, dh = (((db_size >> 11) & 0x7FF) + 1) * 8;
    const bool has_depth_surface = (z_info & 3) != 0 && z_addr;
    if (has_depth_surface && (s_info & 1)) {
        const uint64_t s_addr = uint64_t(r.C(0xA013)) << 8; // DB_STENCIL_READ_BASE
        if (s_addr && s_addr != z_addr) stencil_to_depth_[s_addr] = z_addr;
    }
    if (has_depth_surface) {
        const uint64_t htile = uint64_t(r.C(0xA005)) << 8; // DB_HTILE_DATA_BASE
        if (htile) htile_to_depth_[htile] = {z_addr, dw, dh};
    }

    // Depth / stencil clears through DB_RENDER_CONTROL (HTILE fast clear on the PS4).
    if ((db_render & 3) && has_depth_surface) {
        RenderTarget* dt = GetDepthTarget(z_addr, dw, dh);
        if (dt) {
            VkCommandBuffer c = Cmd();
            Barrier(c, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearDepthStencilValue cv{r.Cf(0xA00B), r.C(0xA00A) & 0xFF};
            VkImageSubresourceRange range{0, 0, 1, 0, 1};
            if (db_render & 1) range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (db_render & 2) range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            vkCmdClearDepthStencilImage(c, dt->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
            dt->gen++;
            stats_.clears++;
        }
        return;
    }

    Mark(0);
    // ---- shaders
    const uint64_t vs_addr = PgmAddr(r, reg::SPI_SHADER_PGM_LO_VS);
    const uint64_t ps_addr = PgmAddr(r, reg::SPI_SHADER_PGM_LO_PS);
    if (!vs_addr || !Readable(vs_addr, 64)) { Skip("no vertex shader"); return; }
    if (ps_addr && !Readable(ps_addr, 64)) { Skip("unreadable pixel shader"); return; }
    Mark(7);

    uint32_t vs_user[16], ps_user[16];
    for (uint32_t i = 0; i < 16; ++i) {
        vs_user[i] = r.S(0x2C4C + i);
        ps_user[i] = r.S(0x2C0C + i);
    }

    ShaderKey vk_key;
    std::memset(&vk_key, 0, sizeof vk_key);
    vk_key.stage = Stage::Vertex;
    vk_key.code = vs_addr;
    bool swappc = false;
    vk_key.code_hash = ShaderHashAndScan(vs_addr, &swappc);
    vk_key.user_sgprs = (r.S(0x2C4B) >> 1) & 0x1F;
    if (swappc) {
        vk_key.fetch = uint64_t(vs_user[0]) | (uint64_t(vs_user[1]) << 32);
        if (!Readable(vk_key.fetch, 16)) { Skip("unreadable fetch shader"); return; }
    }
    // Geometry stages (VGT_SHADER_STAGES_EN: ES_EN bits 3-4, GS_EN bit 5): run ES and GS first, then draw the
    // copy VS over the GS output.
    gs_ = {};
    {
        const uint32_t stages = r.C(0xA2D5);
        if ((stages & 0x18) && (stages & 0x20)) {
            if (!RunEsGs(d, r)) return;
            vk_key.gs_mode = ShaderKey::kGsCopy;
            std::memcpy(vk_key.gs_params, gs_.params, sizeof vk_key.gs_params);
        }
    }

    // colour targets (only the MRTs the PS can write)
    struct Att {
        RenderTarget* rt = nullptr;
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        uint32_t mask = 0;
    } att[8];
    uint32_t ncolor = 0;
    ShaderKey ps_key;
    std::memset(&ps_key, 0, sizeof ps_key);
    ps_key.stage = Stage::Fragment;
    ps_key.code = ps_addr;
    if (ps_addr) {
        bool dummy;
        ps_key.code_hash = ShaderHashAndScan(ps_addr, &dummy);
        ps_key.user_sgprs = (r.S(0x2C0B) >> 1) & 0x1F;
        ps_key.ps_input_addr = r.C(reg::SPI_PS_INPUT_ADDR);
        for (uint32_t i = 0; i < 32; ++i) ps_key.ps_input_cntl[i] = r.C(0xA191 + i);
        ps_key.num_interp = r.C(reg::SPI_PS_IN_CONTROL) & 0x3F;
    }
    uint32_t rt_w = 16384, rt_h = 16384;
    if (cb_mode != 0 && ps_addr) {
        for (uint32_t i = 0; i < 8; ++i) {
            const uint32_t base = kCbBase + i * kCbStride;
            const uint32_t info = r.C(base + 4);
            const uint32_t fmt = (info >> 2) & 0x1F, num = (info >> 8) & 7, swap = (info >> 11) & 3;
            const uint32_t mask = (target_mask >> (4 * i)) & 0xF;
            const uint64_t addr = uint64_t(r.C(base)) << 8;
            if (!fmt || !mask || !addr) continue;
            const VkFormat vf = CbFormat(fmt, num);
            if (vf == VK_FORMAT_UNDEFINED) { Skip("unsupported colour format"); continue; }
            const uint32_t pitch = ((r.C(base + 1) & 0x7FF) + 1) * 8;
            const uint32_t slice = ((r.C(base + 2) & 0x3FFFFF) + 1) * 64;
            const uint32_t height = std::max<uint32_t>(slice / pitch, 1);
            ps_key.mrt_swizzle[i] = CbSwizzle(CbComponents(fmt), swap);
            ps_key.mrt_kind[i] = num == 4 ? 1 : num == 5 ? 2 : 0;
            att[i].fmt = vf;
            att[i].mask = mask;
            att[i].rt = GetColorTarget(addr, vf, pitch, height, info);
            if (!att[i].rt) { Skip("render target creation failed"); att[i] = {}; continue; }
            rt_w = std::min(rt_w, pitch);
            rt_h = std::min(rt_h, height);
            ncolor = i + 1;
        }
    }
    const bool z_enable = (depth_ctl & 2) != 0, z_write = (depth_ctl & 4) != 0, stencil = (depth_ctl & 1) != 0;
    const bool use_depth = has_depth_surface && (z_enable || stencil);
    if (ncolor == 0 && !use_depth) { Skip(ps_addr ? "no colour or depth output" : "no pixel shader and no depth"); return; }

    Mark(1);
    // ---- translate + pre-execute (twice when image dimensions/types differ from the defaults)
    ShaderEntry* vs = GetShader(vk_key);
    if (!vs->ok) { Skip("vertex shader failed"); return; }
    ShaderEntry* ps = nullptr;
    if (ps_addr) {
        ps = GetShader(ps_key);
        if (!ps->ok) { Skip("pixel shader failed"); return; }
    }
    std::vector<uint32_t> vs_sdata, ps_sdata;
    ResourceDescriptors vs_desc, ps_desc;
    if (!PrepareShader(vs, vk_key, vs_user, vs_sdata, vs_desc)) { Skip("vertex shader failed"); return; }
    if (ps && !PrepareShader(ps, ps_key, ps_user, ps_sdata, ps_desc)) { Skip("pixel shader failed"); return; }

    Mark(2);
    // drop attachments the PS does not export
    for (uint32_t i = 0; i < ncolor; ++i)
        if (att[i].rt && !(ps->t->mrt_mask & (1u << i))) att[i].mask = 0;

    RenderTarget* dt = nullptr;
    if (use_depth) {
        dt = GetDepthTarget(z_addr, dw, dh);
        if (!dt) { Skip("depth target creation failed"); return; }
        rt_w = std::min(rt_w, dt->img.width);
        rt_h = std::min(rt_h, dt->img.height);
    }

    // ---- topology / indices
    bool topo_ok;
    const uint32_t prim = gs_.active ? 4u : r.uc[0x242]; // the copy VS draws a triangle list
    const VkPrimitiveTopology topo = Topology(prim, &topo_ok);
    if (!topo_ok) { Skip("unsupported primitive type"); return; }

    uint64_t ib_off = UINT64_MAX;
    uint32_t count = gs_.active ? gs_.verts : d.index_count;
    VkIndexType itype = d.index_type == 1 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (prim == 0x13) { // quad list → triangle list
        const uint32_t quads = count / 4;
        ib_off = Alloc(uint64_t(quads) * 6 * 4, 4);
        if (ib_off == UINT64_MAX) { Skip("index upload"); return; }
        auto* out = reinterpret_cast<uint32_t*>(RingPtr(ib_off));
        std::vector<uint32_t> src(count);
        if (d.index_base) {
            const uint32_t es = d.index_type == 1 ? 4 : 2;
            std::vector<uint8_t> raw(size_t(count) * es);
            SafeCopy(raw.data(), d.index_base, raw.size());
            for (uint32_t i = 0; i < count; ++i)
                src[i] = es == 4 ? reinterpret_cast<uint32_t*>(raw.data())[i] : reinterpret_cast<uint16_t*>(raw.data())[i];
        } else {
            for (uint32_t i = 0; i < count; ++i) src[i] = i;
        }
        for (uint32_t q = 0; q < quads; ++q) {
            const uint32_t* v = &src[q * 4];
            const uint32_t idx[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
            std::memcpy(out + q * 6, idx, sizeof idx);
        }
        count = quads * 6;
        itype = VK_INDEX_TYPE_UINT32;
    }
    // Highest vertex index the draw can fetch: bounds the vertex-buffer copies below. Computed from a
    // host copy: the ring buffer is write-combined memory and must never be read back by the CPU.
    uint32_t max_vertex = d.index_count ? d.index_count - 1 : 0;
    if (prim != 0x13 && d.index_base && !gs_.active) {
        const uint64_t bytes = uint64_t(count) * (d.index_type == 1 ? 4 : 2);
        ib_off = Alloc(bytes + 4, 4);
        if (ib_off == UINT64_MAX) { Skip("index upload"); return; }
        index_tmp_.resize(size_t(bytes + 3) / 4 + 1);
        SafeCopy(index_tmp_.data(), d.index_base, size_t(bytes));
        std::memcpy(RingPtr(ib_off), index_tmp_.data(), size_t(bytes));
        max_vertex = 0;
        if (itype == VK_INDEX_TYPE_UINT32) {
            const auto* ix = index_tmp_.data();
            for (uint32_t i = 0; i < count; ++i) max_vertex = std::max(max_vertex, ix[i]);
        } else {
            const auto* ix = reinterpret_cast<const uint16_t*>(index_tmp_.data());
            for (uint32_t i = 0; i < count; ++i) max_vertex = std::max<uint32_t>(max_vertex, ix[i]);
        }
    } else if (prim == 0x13 && d.index_base) {
        max_vertex = 0xFFFF; // quad lists are rare; keep the full buffers
    }
    const uint32_t max_records = std::max(max_vertex + 1, std::max<uint32_t>(d.num_instances, 1));

    Mark(3);
    // ---- resources (may record uploads/copies, so before rendering begins)
    VkDescriptorSet vs_set = VK_NULL_HANDLE, ps_set = VK_NULL_HANDLE;
    aux_off_ = gs_.aux_off;
    aux_size_ = gs_.aux_size;
    if (!BindShaderResources(vs, vs_sdata, vs_desc, vs_set, gs_.active ? ~0u : max_records)) { Skip("vertex resources"); return; }
    if (ps && !BindShaderResources(ps, ps_sdata, ps_desc, ps_set, ~0u)) { Skip("pixel resources"); return; }

    Mark(4);
    // ---- pipeline
    PipeKey pk;
    std::memset(&pk, 0, sizeof pk);
    pk.vs = HashKey(vs->t->key);
    pk.ps = ps ? HashKey(ps->t->key) : 0;
    pk.ncolor = ncolor;
    for (uint32_t i = 0; i < ncolor; ++i) {
        pk.color[i] = att[i].rt ? att[i].fmt : VK_FORMAT_UNDEFINED;
        pk.blend[i] = r.C(0xA1E0 + i);
        uint32_t m = att[i].rt ? att[i].mask : 0;
        pk.mask[i] = m; // bit order R,G,B,A = memory components x,y,z,w
    }
    pk.depth = dt ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
    pk.topology = uint32_t(topo);
    Pipeline* pipe = GetPipeline(pk, vs, ps);
    if (!pipe->pipe) { Skip("pipeline creation failed"); return; }

    Mark(5);
    // ---- record
    VkCommandBuffer c = Cmd();
    for (uint32_t i = 0; i < ncolor; ++i)
        if (att[i].rt) Barrier(c, att[i].rt->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (dt) Barrier(c, dt->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    FullBarrier(c);

    // viewport / scissor
    const float xs = r.Cf(0xA10F), xo = r.Cf(0xA110), ys = r.Cf(0xA111), yo = r.Cf(0xA112), zs = r.Cf(0xA113),
                zo = r.Cf(0xA114);
    VkViewport vp{xo - xs, yo - ys, 2 * xs, 2 * ys, 0.0f, 1.0f};
    if (vp.width <= 0) { vp.x = 0; vp.width = float(rt_w); }
    auto tl = [](uint32_t v, int32_t& x, int32_t& y) { x = int32_t(v & 0x7FFF); y = int32_t((v >> 16) & 0x7FFF); };
    int32_t x0 = 0, y0 = 0, x1 = int32_t(rt_w), y1 = int32_t(rt_h), ax, ay, bx, by;
    tl(r.C(0xA00C), ax, ay); tl(r.C(0xA00D), bx, by); // screen scissor
    x0 = std::max(x0, ax); y0 = std::max(y0, ay); x1 = std::min(x1, bx); y1 = std::min(y1, by);
    tl(r.C(0xA094), ax, ay); tl(r.C(0xA095), bx, by); // viewport 0 scissor
    if (bx > ax && by > ay) { x0 = std::max(x0, ax); y0 = std::max(y0, ay); x1 = std::min(x1, bx); y1 = std::min(y1, by); }
    tl(r.C(0xA090), ax, ay); tl(r.C(0xA091), bx, by); // generic scissor
    if (bx > ax && by > ay) { x0 = std::max(x0, ax); y0 = std::max(y0, ay); x1 = std::min(x1, bx); y1 = std::min(y1, by); }
    if (x1 <= x0 || y1 <= y0) { Skip("empty scissor"); return; }
    VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};

    VkRenderingAttachmentInfo cai[8]{};
    for (uint32_t i = 0; i < ncolor; ++i) {
        cai[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!att[i].rt) continue;
        cai[i].imageView = att[i].rt->img.view;
        cai[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cai[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        cai[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo dai{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (dt) {
        dai.imageView = dt->img.view;
        dai.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dai.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        dai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = scissor;
    ri.layerCount = 1;
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachments = cai;
    ri.pDepthAttachment = dt ? &dai : nullptr;
    ri.pStencilAttachment = dt ? &dai : nullptr;
    vkCmdBeginRendering(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipe);
    VkDescriptorSet sets[2] = {vs_set, ps_set};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->layout, 0, ps ? 2 : 1, sets, 0, nullptr);
    const float zfix[4] = {zs, zo, 0, 0};
    vkCmdPushConstants(c, pipe->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, zfix);
    vkCmdSetViewport(c, 0, 1, &vp);
    vkCmdSetScissor(c, 0, 1, &scissor);
    const uint32_t su = r.C(0xA205);
    VkCullModeFlags cull = 0;
    if (su & 1) cull |= VK_CULL_MODE_FRONT_BIT;
    if (su & 2) cull |= VK_CULL_MODE_BACK_BIT;
    vkCmdSetCullMode(c, cull);
    vkCmdSetFrontFace(c, (su & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    static const uint64_t dbg_nodepth_vs = [] { const char* e = std::getenv("GR_DEBUG_NODEPTH"); return e ? std::strtoull(e, nullptr, 16) : 0ull; }();
    const bool dbg_nodepth = dbg_nodepth_vs && vs_addr == dbg_nodepth_vs;
    vkCmdSetDepthTestEnable(c, dt && z_enable && !dbg_nodepth);
    vkCmdSetDepthWriteEnable(c, dt && z_enable && z_write);
    vkCmdSetDepthCompareOp(c, VkCompareOp((depth_ctl >> 4) & 7));
    vkCmdSetStencilTestEnable(c, dt && stencil && (s_info & 1));
    const uint32_t sc = r.C(0xA10B), srm = r.C(0xA10C), srm_bf = r.C(0xA10D);
    const bool bf = (depth_ctl >> 7) & 1;
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_BIT, StencilOp(sc & 0xF), StencilOp((sc >> 4) & 0xF),
                      StencilOp((sc >> 8) & 0xF), VkCompareOp((depth_ctl >> 8) & 7));
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, StencilOp(bf ? (sc >> 12) & 0xF : sc & 0xF),
                      StencilOp(bf ? (sc >> 16) & 0xF : (sc >> 4) & 0xF), StencilOp(bf ? (sc >> 20) & 0xF : (sc >> 8) & 0xF),
                      VkCompareOp(bf ? (depth_ctl >> 20) & 7 : (depth_ctl >> 8) & 7));
    const uint32_t brm = bf ? srm_bf : srm;
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, srm & 0xFF);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, brm & 0xFF);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, (srm >> 8) & 0xFF);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, (brm >> 8) & 0xFF);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, (srm >> 16) & 0xFF);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, (brm >> 16) & 0xFF);
    float bc[4];
    for (int i = 0; i < 4; ++i) bc[i] = r.Cf(0xA105 + i);
    vkCmdSetBlendConstants(c, bc);
    const uint32_t inst = gs_.active ? gs_.nprim : std::max<uint32_t>(d.num_instances, 1);
    const bool query = qtrace_ && qpool_ && qcount_ < kMaxQueries;
    last_query_ = query ? int32_t(qcount_) : -1;
    if (query) vkCmdBeginQuery(c, qpool_, qcount_, 0);
    if (ib_off != UINT64_MAX) {
        vkCmdBindIndexBuffer(c, ring_.buffer, ib_off, itype);
        vkCmdDrawIndexed(c, count, inst, 0, 0, 0);
    } else {
        vkCmdDraw(c, count, inst, 0, 0);
    }
    if (query) vkCmdEndQuery(c, qpool_, qcount_++);
    vkCmdEndRendering(c);
    for (uint32_t i = 0; i < ncolor; ++i)
        if (att[i].rt && att[i].mask) att[i].rt->gen++;
    if (dt && (z_write || stencil)) dt->gen++;
    Mark(6);
    stats_.drawn++;
}

void Renderer::Flip(int32_t index) {
    struct Acc { double t0; double& acc; ~Acc() { acc += NowMs() - t0; } } acc{NowMs(), stats_.flip_ms};
    stats_.flips++;
    if (!Init()) return;
    const videoout::FrameInfo fi = videoout::Buffer(index);
    auto it = rts_.find(fi.addr);
    if (!fi.addr || it == rts_.end()) {
        Flush();
        return;
    }
    RenderTarget& rt = it->second;
    const uint32_t w = std::min(fi.width ? fi.width : rt.img.width, rt.img.width);
    const uint32_t h = std::min(fi.height ? fi.height : rt.img.height, rt.img.height);
    // SCE pixel formats: 0x80000000 A8R8G8B8_SRGB (BGRA in memory), 0x80002200 A8B8G8R8_SRGB (RGBA)
    const VkFormat want = fi.pixel_format == 0x80002200u ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
    VkCommandBuffer c = Cmd();
    {
        std::lock_guard<std::mutex> lk(out_m_);
        if (out_.image && (out_w_ != w || out_h_ != h || out_fmt_ != want)) {
            Flush();
            c = Cmd();
            dev_.DestroyImage(out_);
            out_ready_ = false;
        }
        if (!out_.image) {
            out_ = dev_.CreateImage(want, w, h, 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            out_w_ = w;
            out_h_ = h;
            out_fmt_ = want;
        }
        Barrier(c, rt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Barrier(c, out_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (FormatBytes(rt.img.format) == 4 && rt.img.format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
            rt.img.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && uint64_t(w) * h * 4 <= scratch_.size) {
            // Raw byte copy through a buffer: the render target holds memory-order channels, the output
            // image reinterprets them in the display buffer's pixel format.
            VkBufferImageCopy bc{};
            bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bc.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(c, rt.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch_.buffer, 1, &bc);
            FullBarrier(c);
            vkCmdCopyBufferToImage(c, scratch_.buffer, out_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        } else {
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
            vkCmdBlitImage(c, rt.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        }
        Barrier(c, out_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const bool listed = std::find(cfg.dump_at.begin(), cfg.dump_at.end(), uint32_t(stats_.flips)) != cfg.dump_at.end();
        const bool dump = !cfg.dump_dir.empty() && uint64_t(w) * h * 4 <= readback_.size &&
                          (listed || (cfg.dump_every && stats_.flips >= cfg.dump_first &&
                                      (stats_.flips - cfg.dump_first) % cfg.dump_every == 0));
        const bool cpu = cpu_frames_ && uint64_t(w) * h * 4 <= readback_.size;
        if (dump || cpu) {
            VkBufferImageCopy bc{};
            bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bc.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(c, out_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &bc);
        }
        Flush();
        out_ready_ = true;
        stats_.flips_out++;
        if (stats_.pipelines != saved_pipelines_ && stats_.flips_out % 600 == 0) SavePipelineCache();
        if (cpu) {
            std::lock_guard<std::mutex> lk(cpu_m_);
            cpu_frame_.pixels.resize(size_t(w) * h * 4);
            std::memcpy(cpu_frame_.pixels.data(), readback_.mapped, cpu_frame_.pixels.size());
            cpu_frame_.width = w;
            cpu_frame_.height = h;
            cpu_frame_.format = want;
            cpu_frame_.seq++;
        }
        if (dump) {
            std::vector<uint8_t> px(size_t(w) * h * 4);
            std::memcpy(px.data(), readback_.mapped, px.size());
            if (want == VK_FORMAT_B8G8R8A8_SRGB)
                for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            std::error_code ec;
            fs::create_directories(cfg.dump_dir, ec);
            char name[64];
            std::snprintf(name, sizeof name, "frame_%05llu.png", (unsigned long long)stats_.flips);
            const std::string path = (fs::path(cfg.dump_dir) / name).string();
            stbi_write_png(path.c_str(), int(w), int(h), 4, px.data(), int(w * 4));
            stats_.dumped++;
            TraceNote("renderer: frame %llu written to %s", (unsigned long long)stats_.flips, path.c_str());
            // test runs (tools/run_scenario.ps1) end as soon as the last requested frame exists
            if (cfg.exit_after_dumps && listed && uint32_t(stats_.flips) == cfg.dump_at.back()) {
                std::printf("renderer: last requested frame written, exiting\n");
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 0);
            }
        }
    }
}

float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(float(m), -24);
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else v = std::ldexp(float(m + 1024), int(e) - 25);
    return s ? -v : v;
}

// Debug (GR_DUMP_RTS=<submission>): every colour render target and depth buffer as PNG. Float formats
// are shown with a simple Reinhard curve, depth/R32 as greyscale.
void Renderer::DumpRenderTargets() {
    std::error_code ec;
    const fs::path dir = ExeDir() / "rt_dump";
    fs::create_directories(dir, ec);
    auto dump = [&](uint64_t addr, RenderTarget& rt) {
        const uint32_t w = rt.img.width, h = rt.img.height;
        const VkFormat f = rt.depth ? VK_FORMAT_R32_SFLOAT : rt.img.format;
        const uint32_t bpp = FormatBytes(f);
        if (uint64_t(w) * h * bpp > readback_.size) return;
        VkCommandBuffer c = Cmd();
        Barrier(c, rt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VkImageAspectFlags(rt.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
        bc.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(c, rt.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &bc);
        Flush();
        std::vector<uint8_t> raw(size_t(w) * h * bpp);
        std::memcpy(raw.data(), readback_.mapped, raw.size());
        std::vector<uint8_t> px(size_t(w) * h * 4, 255);
        auto tm = [](float v) { v = v < 0 ? 0 : v; return uint8_t(std::pow(v / (1.0f + v), 1.0f / 2.2f) * 255.0f); };
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            uint8_t* o = &px[i * 4];
            const uint8_t* s = &raw[i * bpp];
            switch (f) {
            case VK_FORMAT_R16G16B16A16_SFLOAT: {
                uint16_t hv[4];
                std::memcpy(hv, s, 8);
                for (int k = 0; k < 3; ++k) o[k] = tm(HalfToFloat(hv[k]));
                break;
            }
            case VK_FORMAT_R32_SFLOAT: {
                float v;
                std::memcpy(&v, s, 4);
                o[0] = o[1] = o[2] = uint8_t(std::min(std::max(v, 0.0f), 1.0f) * 255.0f);
                break;
            }
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
                uint32_t v;
                std::memcpy(&v, s, 4);
                auto uf = [](uint32_t r, int mb) {
                    const uint32_t e = r >> mb, m = r & ((1u << mb) - 1);
                    return e == 0 ? std::ldexp(float(m), -14 - mb) : std::ldexp(1.0f + float(m) / float(1u << mb), int(e) - 15);
                };
                o[0] = tm(uf(v & 0x7FF, 6));
                o[1] = tm(uf((v >> 11) & 0x7FF, 6));
                o[2] = tm(uf(v >> 22, 5));
                break;
            }
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                uint32_t v;
                std::memcpy(&v, s, 4);
                o[0] = uint8_t((v & 0x3FF) >> 2);
                o[1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
                o[2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
                break;
            }
            default:
                if (bpp == 4) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; }
                else if (bpp == 1) o[0] = o[1] = o[2] = s[0];
                else if (bpp == 2) { o[0] = s[0]; o[1] = s[1]; o[2] = 0; }
                else if (bpp == 8) { o[0] = s[1]; o[1] = s[3]; o[2] = s[5]; }
                break;
            }
        }
        char name[96];
        std::snprintf(name, sizeof name, "%s_%010llx_%ux%u_fmt%d.png", rt.depth ? "depth" : "rt", (unsigned long long)addr,
                      w, h, int(rt.img.format));
        stbi_write_png((dir / name).string().c_str(), int(w), int(h), 4, px.data(), int(w * 4));
    };
    for (auto& [addr, rt] : rts_) dump(addr, rt);
    for (auto& [addr, rt] : depths_) dump(addr, rt);
    TraceNote("renderer: render targets of submission %llu written to %s", (unsigned long long)submit_index_,
              dir.string().c_str());
}

std::string Renderer::StatsText() {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "renderer: %llu draws (%llu drawn, %llu skipped, %llu clears), %llu shaders (%llu failed), "
                  "%llu pipelines, %llu render targets, %llu textures (%llu uploads, %llu RT copies), %llu flips "
                  "(%llu presented, %llu dumped)",
                  (unsigned long long)stats_.draws, (unsigned long long)stats_.drawn, (unsigned long long)stats_.skipped,
                  (unsigned long long)stats_.clears, (unsigned long long)stats_.shaders,
                  (unsigned long long)stats_.shader_fail, (unsigned long long)stats_.pipelines,
                  (unsigned long long)stats_.rts, (unsigned long long)stats_.textures,
                  (unsigned long long)stats_.tex_uploads, (unsigned long long)stats_.tex_rt_copies,
                  (unsigned long long)stats_.flips, (unsigned long long)stats_.flips_out,
                  (unsigned long long)stats_.dumped);
    std::string s = buf;
    std::snprintf(buf, sizeof buf, "\n  host time: %.0f ms recording draws, %.0f ms waiting for the GPU, %.0f ms in flips",
                  stats_.draw_ms, stats_.wait_ms, stats_.flip_ms);
    s += buf;
    std::snprintf(buf, sizeof buf, "\n  compute: %llu dispatches (%llu executed, %llu buffer write-backs), %llu tiny render-target read-backs, %llu geometry-pipeline draws",
                  (unsigned long long)stats_.dispatches, (unsigned long long)stats_.dispatched,
                  (unsigned long long)stats_.writebacks, (unsigned long long)stats_.tiny_readbacks, (unsigned long long)stats_.gs_draws);
    s += buf;
    std::snprintf(buf, sizeof buf,
                  "\n  draw sections (ms): setup %.0f, shaders %.0f, pre-exec %.0f, targets/indices %.0f, resources %.0f, "
                  "pipeline %.0f, record %.0f (readable checks %.0f)",
                  sec_[0], sec_[1], sec_[2], sec_[3], sec_[4], sec_[5], sec_[6], sec_[7]);
    s += buf;
    for (const auto& [why, n] : stats_.skip_reasons) s += "\n  " + why + ": " + std::to_string(n);
    return s;
}

RendererConfig g_cfg;

} // namespace

bool AcquireOutputFrame(OutputFrame* f) { return R().Acquire(f); }
void ReleaseOutputFrame() { R().Release(); }

void RendererEnableCpuFrames(bool on) { R().cpu_frames_ = on; }
bool CopyCpuFrame(CpuFrame& dst) {
    Renderer& r = R();
    std::lock_guard<std::mutex> lk(r.cpu_m_);
    if (r.cpu_frame_.seq == dst.seq || r.cpu_frame_.pixels.empty()) return false;
    dst = r.cpu_frame_;
    return true;
}

void RendererConfigure(const RendererConfig& c) {
    g_cfg = c;
    R().cfg = c;
}
bool RendererEnabled() { return g_cfg.enabled; }

void RendererBeginSubmission(uint64_t index) {
    if (g_cfg.enabled) R().BeginSubmission(index);
}
void RendererDispatch(uint32_t x, uint32_t y, uint32_t z, const uint32_t* ctx, const uint32_t* sh, const uint32_t* uc) {
    if (g_cfg.enabled) R().Dispatch(x, y, z, Regs{ctx, sh, uc});
}
void RendererDraw(const DrawInfo& d, const uint32_t* ctx, const uint32_t* sh, const uint32_t* uc) {
    if (!g_cfg.enabled) return;
    Renderer& r = R();
    // Debug (GR_DRAW_TRACE=<submission>): one trace line per draw with the renderer's decision.
    static const int64_t trace_at = [] { const char* e = std::getenv("GR_DRAW_TRACE"); return e ? std::atoll(e) : -1; }();
    const bool trace = int64_t(r.SubmitIndex()) == trace_at;
    const uint64_t drawn = r.DrawnCount();
    // Debug (GR_FIND_VS=<hex address>): logs the submissions that use a vertex shader
    static const uint64_t find_vs = [] { const char* e = std::getenv("GR_FIND_VS"); return e ? std::strtoull(e, nullptr, 16) : 0ull; }();
    if (find_vs) {
        static uint64_t last_sub = ~0ull;
        static int logged = 0;
        const Regs g{ctx, sh, uc};
        if (PgmAddr(g, reg::SPI_SHADER_PGM_LO_VS) == find_vs && r.SubmitIndex() != last_sub && (logged < 20 || r.SubmitIndex() % 100 < 2)) {
            last_sub = r.SubmitIndex();
            ++logged;
            TraceNote("findvs: submission %llu", (unsigned long long)last_sub);
        }
    }
    { // Debug (GR_DEBUG_PS_USER=<hex PS>): user data of a pixel shader and the table at s[12:13]
        static const uint64_t dbg_ps = [] { const char* e = std::getenv("GR_DEBUG_PS_USER"); return e ? std::strtoull(e, nullptr, 16) : 0ull; }();
        static int logged = 0;
        const Regs g{ctx, sh, uc};
        if (dbg_ps && logged < 4 && PgmAddr(g, reg::SPI_SHADER_PGM_LO_PS) == dbg_ps) {
            ++logged;
            const uint32_t* u = sh + 0x0C; // SPI_SHADER_USER_DATA_PS_0
            TraceNote("psuser rsrc2 %08x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                      g.S(0x2C0B), u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13],
                      u[14], u[15]);
            const uint64_t tbl = uint64_t(u[12]) | (uint64_t(u[13]) << 32);
            uint32_t w[24] = {};
            SafeCopy(w, tbl, sizeof w);
            TraceNote("psuser table 0x%llx: %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x %08x",
                      (unsigned long long)tbl, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                      w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19], w[20], w[21], w[22], w[23]);
        }
    }
    if (trace) { r.last_skip = nullptr; r.last_query_ = -1; }
    r.Draw(d, Regs{ctx, sh, uc});
    if (trace) {
        static uint32_t n = 0;
        const Regs g{ctx, sh, uc};
        TraceNote("drawtrace %u: q %d count %u prim %u VS 0x%llx PS 0x%llx CB0 0x%llx info %08x Z 0x%llx zctl %08x cbctl %08x -> %s",
                  n++, r.last_query_, d.index_count, uc[0x242], (unsigned long long)PgmAddr(g, reg::SPI_SHADER_PGM_LO_VS),
                  (unsigned long long)PgmAddr(g, reg::SPI_SHADER_PGM_LO_PS), (unsigned long long)(uint64_t(g.C(kCbBase)) << 8),
                  g.C(kCbBase + 4), (unsigned long long)(uint64_t(g.C(0xA012)) << 8), g.C(0xA200), g.C(0xA202),
                  r.DrawnCount() != drawn ? "drawn" : r.last_skip ? r.last_skip : "?");
        // streamout state: VGT_STRMOUT_CONFIG / BUFFER_CONFIG, buffer 0 size/stride/offset, VS rsrc2 (SO_EN)
        if (g.C(0xA2E5) || (g.S(0x2C4B) & (1u << 12)))
            TraceNote("  streamout cfg %08x bufcfg %08x buf0 size %08x stride %08x | buf1 %08x %08x | vs rsrc2 %08x",
                      g.C(0xA2E5), g.C(0xA2E6), g.C(0xA2B4), g.C(0xA2B5), g.C(0xA2B8), g.C(0xA2B9), g.S(0x2C4B));
    }
}
void RendererEndSubmission() {
    if (g_cfg.enabled) R().EndSubmission();
}
void RendererFlip(int32_t buffer_index) {
    if (g_cfg.enabled) R().Flip(buffer_index);
}
std::string RendererStats() { return g_cfg.enabled ? R().StatsText() : "renderer: disabled"; }

} // namespace gr::gpu

namespace gr::gpu {
void RendererMarker(uint32_t kind, const char* text) {
    if (!g_cfg.enabled) return;
    static const int64_t trace_at = [] { const char* e = std::getenv("GR_DRAW_TRACE"); return e ? std::atoll(e) : -1; }();
    if (int64_t(R().SubmitIndex()) != trace_at) return;
    char t[64] = {};
    for (int i = 0; i < 63 && text[i]; ++i) t[i] = text[i];
    TraceNote("marker %s %s", kind == 1 ? "push" : kind == 2 ? "set" : "pop", t);
}
} // namespace gr::gpu
