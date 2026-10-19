#include "gpu/shader_recompiler.h"

#include <windows.h>

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace gr::gpu {
using namespace gcn;

bool ShaderKey::operator==(const ShaderKey& o) const { return std::memcmp(this, &o, sizeof(ShaderKey)) == 0; }

uint64_t HashKey(const ShaderKey& k) {
    const auto* p = reinterpret_cast<const uint8_t*>(&k);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(ShaderKey); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

namespace {

constexpr uint32_t kFetchPcBase = 0x100000; // pc of inlined fetch-shader instructions (keeps labels distinct)
constexpr uint32_t kSArray = 128;           // s[] covers SGPRs, VCC, M0 and EXEC by their operand codes

// ---------------------------------------------------------------------------------------------------------
// Program helpers

uint32_t SmrdDwords(const Instr& in) {
    switch (in.op & 7) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    case 3: return 8;
    case 4: return 16;
    default: return 0;
    }
}
bool IsSmrdLoad(const Instr& in) { return in.fmt == Fmt::SMRD && (in.op <= 4 || (in.op >= 8 && in.op <= 12)); }

bool IsBranch(const Instr& in) {
    return in.fmt == Fmt::SOPP && (in.op == 2 || (in.op >= 4 && in.op <= 9));
}
uint32_t BranchTarget(const Instr& in) { return uint32_t(int32_t(in.pc) + 4 + int32_t(in.simm16) * 4); }

// Unified VALU opcode: VOPC 0x000-0x0FF, VOP2 0x100-0x13F, VOP3-only 0x140-0x17F, VOP1 0x180-0x1FF.
uint16_t ValuOp(const Instr& in) {
    switch (in.fmt) {
    case Fmt::VOPC: return in.op;
    case Fmt::VOP2: return uint16_t(0x100 + in.op);
    case Fmt::VOP1: return uint16_t(0x180 + in.op);
    default: return in.op;
    }
}
bool IsValu(const Instr& in) {
    return in.fmt == Fmt::VOP1 || in.fmt == Fmt::VOP2 || in.fmt == Fmt::VOPC || in.fmt == Fmt::VOP3;
}

// Is a sample-type MIMG op (needs a sampler)?
bool MimgNeedsSampler(uint16_t op) { return op >= 0x20 && op != 0x0E; }

bool Is64BitSalu(const Instr& in) {
    if (in.fmt == Fmt::SOP1)
        return in.op == 4 || in.op == 6 || in.op == 8 || in.op == 10 || in.op == 12 || in.op == 28 || in.op == 30 ||
               in.op == 31 || (in.op >= 36 && in.op <= 43) || in.op == 47 || in.op == 49;
    if (in.fmt == Fmt::SOP2)
        return in.op == 11 || (in.op >= 15 && in.op <= 29 && (in.op & 1)) || in.op == 31 || in.op == 33 ||
               in.op == 35 || in.op == 37 || in.op == 41 || in.op == 42;
    return false;
}

// ---------------------------------------------------------------------------------------------------------
// GLSL generator

const char* kHelpers = R"(
float F(uint x) { return uintBitsToFloat(x); }
// Depth compare of image_sample_c: f = sampler DEPTH_COMPARE_FUNC (never, less, equal, lequal, greater,
// notequal, gequal, always); r = reference, t = texel.
float DCmp(uint f, float r, float t) {
    bool p = f == 1u ? r < t : f == 2u ? r == t : f == 3u ? r <= t : f == 4u ? r > t : f == 5u ? r != t
           : f == 6u ? r >= t : f == 7u;
    return p ? 1.0 : 0.0;
}
uint U(float x) { return floatBitsToUint(x); }
#define EXEC ((s[126] & 1u) != 0u)
uint CvtI32(float f) { if (isnan(f)) return 0u; return uint(int(clamp(f, -2147483648.0, 2147483520.0))); }
uint CvtU32(float f) { if (isnan(f)) return 0u; return uint(clamp(f, 0.0, 4294967040.0)); }
float MulLegacy(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float Med3F(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }
int Med3I(int a, int b, int c) { return max(min(a, b), min(max(a, b), c)); }
uint Med3U(uint a, uint b, uint c) { return max(min(a, b), min(max(a, b), c)); }
float CubeId(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0 ? 5.0 : 4.0;
    if (abs(y) >= abs(x)) return y < 0.0 ? 3.0 : 2.0;
    return x < 0.0 ? 1.0 : 0.0;
}
float CubeSc(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0 ? -x : x;
    if (abs(y) >= abs(x)) return x;
    return x < 0.0 ? z : -z;
}
float CubeTc(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return -y;
    if (abs(y) >= abs(x)) return y < 0.0 ? -z : z;
    return -y;
}
float CubeMa(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return 2.0 * z;
    if (abs(y) >= abs(x)) return 2.0 * y;
    return 2.0 * x;
}
vec3 CubeDir(float sc, float tc, float face) {
    float u = (sc - 1.5) * 2.0, v = (tc - 1.5) * 2.0;
    int f = int(roundEven(face));
    if (f == 0) return vec3(1.0, -v, -u);
    if (f == 1) return vec3(-1.0, -v, u);
    if (f == 2) return vec3(u, 1.0, v);
    if (f == 3) return vec3(u, -1.0, -v);
    if (f == 4) return vec3(u, -v, 1.0);
    return vec3(-u, -v, -1.0);
}
bool CmpClass(float f, uint m) {
    uint b = floatBitsToUint(f);
    bool neg = (b >> 31) != 0u;
    uint e = (b >> 23) & 0xFFu, man = b & 0x7FFFFFu;
    if (e == 0xFFu) {
        if (man != 0u) return (m & ((man & 0x400000u) != 0u ? 2u : 1u)) != 0u;
        return (m & (neg ? 4u : 512u)) != 0u;
    }
    if (e == 0u) {
        if (man == 0u) return (m & (neg ? 32u : 64u)) != 0u;
        return (m & (neg ? 16u : 128u)) != 0u;
    }
    return (m & (neg ? 8u : 256u)) != 0u;
}
float UFloat(uint r, uint mbits) {
    uint e = r >> mbits, m = r & ((1u << mbits) - 1u);
    if (e == 0u) return float(m) / float(1u << mbits) * exp2(-14.0);
    if (e == 31u) return m == 0u ? uintBitsToFloat(0x7F800000u) : uintBitsToFloat(0x7FC00000u);
    return (1.0 + float(m) / float(1u << mbits)) * exp2(float(int(e) - 15));
}
uint CvtFmt(uint r, uint b, uint nf) {
    int sv = b >= 32u ? int(r) : bitfieldExtract(int(r), 0, int(b));
    float maxu = b >= 32u ? 4294967295.0 : float((1u << b) - 1u);
    float maxs = b >= 32u ? 2147483647.0 : float((1u << (b - 1u)) - 1u);
    switch (nf) {
    case 0u: return U(float(r) / maxu);
    case 1u: return U(max(float(sv) / maxs, -1.0));
    case 2u: return U(float(r));
    case 3u: return U(float(sv));
    case 4u: return r;
    case 5u: return uint(sv);
    case 6u: return U((2.0 * float(sv) + 1.0) / maxu);
    default:
        if (b == 32u) return r;
        if (b == 16u) return U(unpackHalf2x16(r).x);
        if (b == 11u) return U(UFloat(r, 6u));
        if (b == 10u) return U(UFloat(r, 5u));
        return r;
    }
}
uint Lshl64Lo(uint lo, uint n) { n &= 63u; return n >= 32u ? 0u : lo << n; }
uint Lshl64Hi(uint lo, uint hi, uint n) { n &= 63u; return n == 0u ? hi : n >= 32u ? lo << (n - 32u) : (hi << n) | (lo >> (32u - n)); }
uint Lshr64Lo(uint lo, uint hi, uint n) { n &= 63u; return n == 0u ? lo : n >= 32u ? hi >> (n - 32u) : (lo >> n) | (hi << (32u - n)); }
uint Lshr64Hi(uint hi, uint n) { n &= 63u; return n >= 32u ? 0u : hi >> n; }
// Byte address of a buffer access relative to the V# base (w1/w3 = V# dwords 1 and 3). Swizzled buffers
// (ring buffers of the geometry pipeline) interleave elements of index_stride consecutive records;
// ADD_TID adds the lane index to the record index.
uint buf_addr(uint w1, uint w3, uint index, uint offset, uint soff) {
    uint stride = (w1 >> 16u) & 0x3FFFu;
    if ((w3 & 0x800000u) != 0u) index += LANE;
    if ((w1 & 0x80000000u) == 0u) return index * stride + offset + soff;
    uint es = 2u << ((w3 >> 19u) & 3u), is = 8u << ((w3 >> 21u) & 3u);
    return soff + (index / is) * stride * is + (offset / es) * es * is + (index % is) * es + (offset % es);
}
)";

// Per-buffer format fetch helpers ('@' = binding number).
const char* kBufferHelpers = R"(
uint rw_@(uint a) {
    uint i = a >> 2u, sh = (a & 3u) * 8u;
    uint lo = b_@.d[i];
    return sh == 0u ? lo : (lo >> sh) | (b_@.d[i + 1u] << (32u - sh));
}
uvec4 fmt_@(uint a, uint w3) {
    uint df = (w3 >> 15u) & 15u, nf = (w3 >> 12u) & 7u;
    uint w0 = rw_@(a);
    uvec4 raw = uvec4(0u), bits = uvec4(32u);
    uint n = 0u;
    if (df == 1u) { n = 1u; bits = uvec4(8u); raw.x = w0 & 0xFFu; }
    else if (df == 2u) { n = 1u; bits = uvec4(16u); raw.x = w0 & 0xFFFFu; }
    else if (df == 3u) { n = 2u; bits = uvec4(8u); raw.xy = uvec2(w0 & 0xFFu, (w0 >> 8u) & 0xFFu); }
    else if (df == 4u) { n = 1u; raw.x = w0; }
    else if (df == 5u) { n = 2u; bits = uvec4(16u); raw.xy = uvec2(w0 & 0xFFFFu, w0 >> 16u); }
    else if (df == 6u) { n = 3u; bits = uvec4(11u, 11u, 10u, 0u); raw.xyz = uvec3(w0 & 0x7FFu, (w0 >> 11u) & 0x7FFu, w0 >> 22u); }
    else if (df == 7u) { n = 3u; bits = uvec4(10u, 11u, 11u, 0u); raw.xyz = uvec3(w0 & 0x3FFu, (w0 >> 10u) & 0x7FFu, w0 >> 21u); }
    else if (df == 8u) { n = 4u; bits = uvec4(10u, 10u, 10u, 2u); raw = uvec4(w0 & 0x3FFu, (w0 >> 10u) & 0x3FFu, (w0 >> 20u) & 0x3FFu, w0 >> 30u); }
    else if (df == 9u) { n = 4u; bits = uvec4(2u, 10u, 10u, 10u); raw = uvec4(w0 & 3u, (w0 >> 2u) & 0x3FFu, (w0 >> 12u) & 0x3FFu, w0 >> 22u); }
    else if (df == 10u) { n = 4u; bits = uvec4(8u); raw = uvec4(w0 & 0xFFu, (w0 >> 8u) & 0xFFu, (w0 >> 16u) & 0xFFu, w0 >> 24u); }
    else if (df == 11u) { n = 2u; raw.xy = uvec2(w0, rw_@(a + 4u)); }
    else if (df == 12u) { uint w1 = rw_@(a + 4u); n = 4u; bits = uvec4(16u); raw = uvec4(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu, w1 >> 16u); }
    else if (df == 13u) { n = 3u; raw.xyz = uvec3(w0, rw_@(a + 4u), rw_@(a + 8u)); }
    else if (df == 14u) { n = 4u; raw = uvec4(w0, rw_@(a + 4u), rw_@(a + 8u), rw_@(a + 12u)); }
    uint one = (nf == 4u || nf == 5u) ? 1u : 0x3F800000u;
    uvec4 c = uvec4(0u, 0u, 0u, one);
    for (uint i = 0u; i < n; ++i) c[i] = CvtFmt(raw[i], bits[i], nf);
    uvec4 o;
    for (uint i = 0u; i < 4u; ++i) {
        uint sel = (w3 >> (3u * i)) & 7u;
        o[i] = sel == 0u ? 0u : sel == 1u ? one : sel >= 4u ? c[sel - 4u] : 0u;
    }
    return o;
}
)";

std::string Fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string HexU(uint32_t v) { return Fmt("0x%08Xu", v); }

uint32_t InlineFloatBits(uint16_t c) {
    static const uint32_t f[8] = {0x3F000000, 0xBF000000, 0x3F800000, 0xBF800000,
                                  0x40000000, 0xC0000000, 0x40800000, 0xC0800000};
    return f[c - 240];
}

// Operand value (32-bit) as a GLSL uint expression.
std::string Src(uint16_t c, uint32_t lit) {
    if (c < kSArray) return Fmt("s[%u]", c);
    if (c >= 128 && c <= 192) return Fmt("%uu", c - 128);
    if (c >= 193 && c <= 208) return HexU(uint32_t(-(int32_t(c) - 192)));
    if (c >= 240 && c <= 247) return HexU(InlineFloatBits(c));
    if (c == kVccz) return "((s[106] & 1u) == 0u ? 1u : 0u)";
    if (c == kExecz) return "((s[126] & 1u) == 0u ? 1u : 0u)";
    if (c == kScc) return "(scc ? 1u : 0u)";
    if (c == kLiteral) return HexU(lit);
    if (c >= kVgprBase) return Fmt("v[%u]", c - kVgprBase);
    return "0u";
}
// High dword of a 64-bit operand.
std::string SrcHi(uint16_t c) {
    if (c < kSArray) return Fmt("s[%u]", c + 1);
    if (c >= 193 && c <= 208) return "0xFFFFFFFFu";
    return "0u";
}

struct ResInfo {
    int tex = -1, samp = -1, buf = -1; // resource slot indices
};

class Generator {
public:
    Generator(const ShaderKey& key, TranslatedShader& out, const std::vector<ResInfo>& res)
        : key_(key), t_(out), res_(res) {}

    std::string Run();

private:
    void L(const std::string& s) {
        body_ += std::string(indent_ * 4, ' ');
        body_ += s;
        body_ += '\n';
    }
    void Warn(const Instr& in) {
        const std::string w = Fmt("pc 0x%x: %s (%s op %u)\n", in.pc, Disassemble(in).c_str(), FmtName(in.fmt), in.op);
        if (t_.warnings.size() < 4096) t_.warnings += w;
        L("// unsupported: " + Disassemble(in));
    }
    void SWrite(uint16_t d, const std::string& v) {
        if (d < kSArray) L(Fmt("s[%u] = %s;", d, v.c_str()));
    }
    void Emit(size_t idx, const Instr& in);
    void EmitSop2(const Instr& in);
    void EmitSop1(const Instr& in);
    void EmitSopk(const Instr& in);
    void EmitSopc(const Instr& in);
    void EmitValu(const Instr& in);
    void EmitVopc(const Instr& in, uint16_t op);
    void EmitMimg(size_t idx, const Instr& in);
    void EmitBuffer(size_t idx, const Instr& in);
    void EmitExp(const Instr& in);
    void EmitInterp(const Instr& in);
    void EmitDs(const Instr& in);
    void EmitSmrd(size_t idx, const Instr& in);

    int InputLocation(uint32_t attr) const {
        const uint32_t cntl = key_.ps_input_cntl[attr & 31];
        const uint32_t off = cntl & 0x3F;
        return off >= 32 ? -1 : int(off);
    }

    const ShaderKey& key_;
    TranslatedShader& t_;
    const std::vector<ResInfo>& res_;
    std::string body_;
    int indent_ = 1;
    std::set<int> inputs_;       // PS input locations
    std::map<int, bool> flat_;   // PS input location -> flat
    std::map<size_t, uint32_t> smrd_index_; // instruction -> sdata index
    uint32_t lds_mask_ = 63;
public:
    void SetSmrdIndex(size_t i, uint32_t idx) { smrd_index_[i] = idx; }
};

void Generator::EmitSop2(const Instr& in) {
    const std::string a = Src(in.src0, in.literal), b = Src(in.src1, in.literal);
    const std::string ah = SrcHi(in.src0), bh = SrcHi(in.src1);
    const uint16_t d = in.dst;
    auto bin32 = [&](const char* expr) {
        L(Fmt("{ uint a = %s, b = %s; uint r = %s; scc = r != 0u; s[%u] = r; }", a.c_str(), b.c_str(), expr, d));
    };
    auto bin64 = [&](const char* expr) {
        L(Fmt("{ uint a = %s, ah = %s, b = %s, bh = %s; uint r = a, rh = ah; { uint x = a, y = b; r = %s; } "
              "{ uint x = ah, y = bh; rh = %s; } scc = (r | rh) != 0u; s[%u] = r; s[%u] = rh; }",
              a.c_str(), ah.c_str(), b.c_str(), bh.c_str(), expr, expr, d, d + 1));
    };
    switch (in.op) {
    case 0: L(Fmt("{ uint a = %s, b = %s; uint r = a + b; scc = r < a; s[%u] = r; }", a.c_str(), b.c_str(), d)); break;
    case 1: L(Fmt("{ uint a = %s, b = %s; scc = b > a; s[%u] = a - b; }", a.c_str(), b.c_str(), d)); break;
    case 2: L(Fmt("{ s[%u] = uint(int(%s) + int(%s)); scc = false; }", d, a.c_str(), b.c_str())); break;
    case 3: L(Fmt("{ s[%u] = uint(int(%s) - int(%s)); scc = false; }", d, a.c_str(), b.c_str())); break;
    case 4: L(Fmt("{ uint a = %s, b = %s, c = scc ? 1u : 0u; uint r = a + b + c; scc = r < a || (c == 1u && r == a); s[%u] = r; }", a.c_str(), b.c_str(), d)); break;
    case 5: L(Fmt("{ uint a = %s, b = %s, c = scc ? 1u : 0u; scc = (b + c) > a || (c == 1u && b == 0xFFFFFFFFu); s[%u] = a - b - c; }", a.c_str(), b.c_str(), d)); break;
    case 6: L(Fmt("{ int a = int(%s), b = int(%s); scc = a < b; s[%u] = uint(min(a, b)); }", a.c_str(), b.c_str(), d)); break;
    case 7: L(Fmt("{ uint a = %s, b = %s; scc = a < b; s[%u] = min(a, b); }", a.c_str(), b.c_str(), d)); break;
    case 8: L(Fmt("{ int a = int(%s), b = int(%s); scc = a > b; s[%u] = uint(max(a, b)); }", a.c_str(), b.c_str(), d)); break;
    case 9: L(Fmt("{ uint a = %s, b = %s; scc = a > b; s[%u] = max(a, b); }", a.c_str(), b.c_str(), d)); break;
    case 10: L(Fmt("s[%u] = scc ? %s : %s;", d, a.c_str(), b.c_str())); break;
    case 11: L(Fmt("{ uint r = scc ? %s : %s, rh = scc ? %s : %s; s[%u] = r; s[%u] = rh; }", a.c_str(), b.c_str(), ah.c_str(), bh.c_str(), d, d + 1)); break;
    case 14: bin32("a & b"); break;
    case 15: bin64("x & y"); break;
    case 16: bin32("a | b"); break;
    case 17: bin64("x | y"); break;
    case 18: bin32("a ^ b"); break;
    case 19: bin64("x ^ y"); break;
    case 20: bin32("a & ~b"); break;
    case 21: bin64("x & ~y"); break;
    case 22: bin32("a | ~b"); break;
    case 23: bin64("x | ~y"); break;
    case 24: bin32("~(a & b)"); break;
    case 25: bin64("~(x & y)"); break;
    case 26: bin32("~(a | b)"); break;
    case 27: bin64("~(x | y)"); break;
    case 28: bin32("~(a ^ b)"); break;
    case 29: bin64("~(x ^ y)"); break;
    case 30: bin32("a << (b & 31u)"); break;
    case 32: bin32("a >> (b & 31u)"); break;
    case 34: bin32("uint(int(a) >> int(b & 31u))"); break;
    case 31:
        L(Fmt("{ uint a = %s, ah = %s, n = %s; uint r = Lshl64Lo(a, n), rh = Lshl64Hi(a, ah, n); scc = (r | rh) != 0u; s[%u] = r; s[%u] = rh; }",
              a.c_str(), ah.c_str(), b.c_str(), d, d + 1));
        break;
    case 33:
        L(Fmt("{ uint a = %s, ah = %s, n = %s; uint r = Lshr64Lo(a, ah, n), rh = Lshr64Hi(ah, n); scc = (r | rh) != 0u; s[%u] = r; s[%u] = rh; }",
              a.c_str(), ah.c_str(), b.c_str(), d, d + 1));
        break;
    case 36: L(Fmt("{ uint a = %s, b = %s; s[%u] = ((1u << (a & 31u)) - 1u) << (b & 31u); }", a.c_str(), b.c_str(), d)); break;
    case 38: L(Fmt("s[%u] = uint(int(%s) * int(%s));", d, a.c_str(), b.c_str())); break;
    case 39:
        L(Fmt("{ uint a = %s, b = %s; uint o = b & 31u, w = min((b >> 16u) & 0x7Fu, 32u - o); uint r = w == 0u ? 0u : bitfieldExtract(a, int(o), int(w)); scc = r != 0u; s[%u] = r; }",
              a.c_str(), b.c_str(), d));
        break;
    case 40:
        L(Fmt("{ uint a = %s, b = %s; uint o = b & 31u, w = min((b >> 16u) & 0x7Fu, 32u - o); uint r = w == 0u ? 0u : uint(bitfieldExtract(int(a), int(o), int(w))); scc = r != 0u; s[%u] = r; }",
              a.c_str(), b.c_str(), d));
        break;
    case 44: L(Fmt("{ int a = int(%s), b = int(%s); uint r = uint(abs(a - b)); scc = r != 0u; s[%u] = r; }", a.c_str(), b.c_str(), d)); break;
    default: Warn(in); break;
    }
}

void Generator::EmitSopk(const Instr& in) {
    const uint16_t d = in.dst;
    const int32_t k = in.simm16;
    const uint32_t ku = uint16_t(in.simm16);
    static const char* ops[6] = {"==", "!=", ">", ">=", "<", "<="};
    if (in.op >= 3 && in.op <= 8) { L(Fmt("scc = int(s[%u]) %s %d;", d, ops[in.op - 3], k)); return; }
    if (in.op >= 9 && in.op <= 14) { L(Fmt("scc = s[%u] %s %uu;", d, ops[in.op - 9], ku)); return; }
    switch (in.op) {
    case 0: L(Fmt("s[%u] = %s;", d, HexU(uint32_t(k)).c_str())); break;
    case 2: L(Fmt("if (scc) s[%u] = %s;", d, HexU(uint32_t(k)).c_str())); break;
    case 15: L(Fmt("{ uint r = s[%u] + %s; s[%u] = r; scc = false; }", d, HexU(uint32_t(k)).c_str(), d)); break;
    case 16: L(Fmt("s[%u] = uint(int(s[%u]) * %d);", d, d, k)); break;
    case 18: L(Fmt("s[%u] = 0u;", d)); break;
    case 19: case 21: break; // s_setreg: mode bits, ignored
    default: Warn(in); break;
    }
}

void Generator::EmitSop1(const Instr& in) {
    const std::string a = Src(in.src0, in.literal), ah = SrcHi(in.src0);
    const uint16_t d = in.dst;
    auto saveexec = [&](const char* expr) {
        L(Fmt("{ uint x = %s, e = s[126]; s[%u] = s[126]; s[%u] = s[127]; s[126] = %s; s[127] = 0u; scc = (s[126] & 1u) != 0u; }",
              a.c_str(), d, d + 1, expr));
    };
    switch (in.op) {
    case 3: SWrite(d, a); break;
    case 4: L(Fmt("{ uint r = %s, rh = %s; s[%u] = r; s[%u] = rh; }", a.c_str(), ah.c_str(), d, d + 1)); break;
    case 5: L(Fmt("if (scc) s[%u] = %s;", d, a.c_str())); break;
    case 6: L(Fmt("if (scc) { uint r = %s, rh = %s; s[%u] = r; s[%u] = rh; }", a.c_str(), ah.c_str(), d, d + 1)); break;
    case 7: L(Fmt("{ uint r = ~%s; scc = r != 0u; s[%u] = r; }", a.c_str(), d)); break;
    case 8: L(Fmt("{ uint r = ~%s, rh = ~%s; scc = (r | rh) != 0u; s[%u] = r; s[%u] = rh; }", a.c_str(), ah.c_str(), d, d + 1)); break;
    case 9: L(Fmt("{ uint r = %s; scc = r != 0u; s[%u] = r; }", a.c_str(), d)); break; // wqm on a lane bit is identity
    case 10: L(Fmt("{ uint r = %s, rh = %s; scc = (r | rh) != 0u; s[%u] = r; s[%u] = rh; }", a.c_str(), ah.c_str(), d, d + 1)); break;
    case 11: SWrite(d, "bitfieldReverse(" + a + ")"); break;
    case 13: L(Fmt("{ uint r = uint(bitCount(~%s)); scc = r != 0u; s[%u] = r; }", a.c_str(), d)); break;
    case 15: L(Fmt("{ uint r = uint(bitCount(%s)); scc = r != 0u; s[%u] = r; }", a.c_str(), d)); break;
    case 16: L(Fmt("{ uint r = uint(bitCount(%s) + bitCount(%s)); scc = r != 0u; s[%u] = r; }", a.c_str(), ah.c_str(), d)); break;
    case 17: SWrite(d, "uint(findLSB(~" + a + "))"); break;
    case 19: SWrite(d, "uint(findLSB(" + a + "))"); break;
    case 21: L(Fmt("{ uint x = %s; s[%u] = x == 0u ? 0xFFFFFFFFu : uint(31 - findMSB(x)); }", a.c_str(), d)); break;
    case 25: SWrite(d, "uint(bitfieldExtract(int(" + a + "), 0, 8))"); break;
    case 26: SWrite(d, "uint(bitfieldExtract(int(" + a + "), 0, 16))"); break;
    case 27: L(Fmt("s[%u] &= ~(1u << (%s & 31u));", d, a.c_str())); break;
    case 29: L(Fmt("s[%u] |= (1u << (%s & 31u));", d, a.c_str())); break;
    case 31: {
        const uint64_t pc = key_.code + in.pc + 4;
        L(Fmt("s[%u] = %s; s[%u] = %s;", d, HexU(uint32_t(pc)).c_str(), d + 1, HexU(uint32_t(pc >> 32)).c_str()));
        break;
    }
    case 32: case 33: break; // handled by fetch inlining
    case 36: saveexec("x & e"); break;
    case 37: saveexec("x | e"); break;
    case 38: saveexec("x ^ e"); break;
    case 39: saveexec("x & ~e"); break;
    case 40: saveexec("x | ~e"); break;
    case 41: saveexec("~(x & e)"); break;
    case 42: saveexec("~(x | e)"); break;
    case 43: saveexec("~(x ^ e)"); break;
    case 46: L(Fmt("s[%u] = s[min(%uu + s[124], 127u)];", d, in.src0)); break;
    case 48: L(Fmt("s[min(%uu + s[124], 127u)] = %s;", d, a.c_str())); break;
    case 52: L(Fmt("{ uint r = uint(abs(int(%s))); scc = r != 0u; s[%u] = r; }", a.c_str(), d)); break;
    default: Warn(in); break;
    }
}

void Generator::EmitSopc(const Instr& in) {
    const std::string a = Src(in.src0, in.literal), b = Src(in.src1, in.literal);
    static const char* ops[6] = {"==", "!=", ">", ">=", "<", "<="};
    if (in.op <= 5) L(Fmt("scc = int(%s) %s int(%s);", a.c_str(), ops[in.op], b.c_str()));
    else if (in.op <= 11) L(Fmt("scc = %s %s %s;", a.c_str(), ops[in.op - 6], b.c_str()));
    else if (in.op == 12) L(Fmt("scc = ((%s >> (%s & 31u)) & 1u) == 0u;", a.c_str(), b.c_str()));
    else if (in.op == 13) L(Fmt("scc = ((%s >> (%s & 31u)) & 1u) == 1u;", a.c_str(), b.c_str()));
    else Warn(in);
}

void Generator::EmitVopc(const Instr& in, uint16_t op) {
    const bool vop3 = in.fmt == Fmt::VOP3;
    const uint16_t dst = vop3 ? in.dst : uint16_t(kVccLo);
    const uint16_t group = op >> 4, cond = op & 15;
    std::string f0 = Fmt("F(%s)", Src(in.src0, in.literal).c_str()), f1 = Fmt("F(%s)", Src(in.src1, in.literal).c_str());
    if (vop3) {
        if (in.abs & 1) f0 = "abs(" + f0 + ")";
        if (in.abs & 2) f1 = "abs(" + f1 + ")";
        if (in.neg & 1) f0 = "(-" + f0 + ")";
        if (in.neg & 2) f1 = "(-" + f1 + ")";
    }
    std::string c;
    bool cmpx = false;
    if (group == 0 || group == 1 || group == 4 || group == 5) {
        cmpx = group & 1;
        static const char* fc[16] = {"false", "a < b", "a == b", "a <= b", "a > b", "(a < b || a > b)", "a >= b",
                                     "(!isnan(a) && !isnan(b))", "(isnan(a) || isnan(b))", "!(a >= b)",
                                     "!(a < b || a > b)", "!(a > b)", "!(a <= b)", "!(a == b)", "!(a < b)", "true"};
        c = Fmt("{ float a = %s, b = %s; c = %s; }", f0.c_str(), f1.c_str(), fc[cond]);
    } else if (group >= 8) {
        cmpx = group & 1;
        if (cond == 8) {
            c = Fmt("c = CmpClass(%s, %s);", f0.c_str(), Src(in.src1, in.literal).c_str());
        } else if (cond < 8) {
            static const char* ic[8] = {"false", "a < b", "a == b", "a <= b", "a > b", "a != b", "a >= b", "true"};
            const uint16_t type = (group - 8) >> 1; // 0 i32, 1 i64, 2 u32, 3 u64 (64-bit compared on the low dword)
            const bool sgn = type == 0 || type == 1;
            if (type == 1 || type == 3) Warn(in);
            c = Fmt("{ %s a = %s(%s), b = %s(%s); c = %s; }", sgn ? "int" : "uint", sgn ? "int" : "uint",
                    Src(in.src0, in.literal).c_str(), sgn ? "int" : "uint", Src(in.src1, in.literal).c_str(), ic[cond]);
        } else {
            Warn(in);
            return;
        }
    } else {
        Warn(in); // f64 compares
        return;
    }
    L(Fmt("{ bool c = false; %s uint r = (EXEC && c) ? 1u : 0u; %s%s}", c.c_str(),
          dst < kSArray ? Fmt("s[%u] = r; s[%u] = 0u; ", dst, dst + 1).c_str() : "",
          cmpx ? "s[126] = r; s[127] = 0u; " : ""));
}

void Generator::EmitValu(const Instr& in) {
    const uint16_t op = ValuOp(in);
    if (op < 0x100) { EmitVopc(in, op); return; }
    const bool vop3 = in.fmt == Fmt::VOP3;
    const uint16_t d = uint16_t(in.dst - kVgprBase);
    const std::string a0 = Src(in.src0, in.literal), a1 = Src(in.src1, in.literal),
                      a2 = vop3 ? Src(in.src2, in.literal) : "0u";
    auto fsrc = [&](int i) {
        std::string f = Fmt("F(a%d)", i);
        if (vop3 && !in.sdst3) {
            if (in.abs & (1 << i)) f = "abs(" + f + ")";
        }
        if (vop3 && (in.neg & (1 << i))) f = "(-" + f + ")";
        return f;
    };
    const std::string f0 = fsrc(0), f1 = fsrc(1), f2 = fsrc(2);
    std::string pre = Fmt("uint a0 = %s, a1 = %s, a2 = %s, dv = v[%u]; ", a0.c_str(), a1.c_str(), a2.c_str(), d);
    auto fop = [&](const std::string& e) {
        std::string r = "(" + e + ")";
        if (vop3) {
            if (in.omod == 1) r = "(" + r + " * 2.0)";
            else if (in.omod == 2) r = "(" + r + " * 4.0)";
            else if (in.omod == 3) r = "(" + r + " * 0.5)";
            if (in.clamp) r = "clamp(" + r + ", 0.0, 1.0)";
        }
        L("{ " + pre + "uint r = U(" + r + "); if (EXEC) v[" + std::to_string(d) + "] = r; }");
    };
    auto uop = [&](const std::string& e) {
        L("{ " + pre + "uint r = " + e + "; if (EXEC) v[" + std::to_string(d) + "] = r; }");
    };
    // carry-out destination for v_add_i32 & co
    const std::string carry = vop3 ? (in.sdst3 < kSArray ? Fmt("s[%u]", in.sdst3) : std::string()) : "s[106]";
    const std::string cin = vop3 ? Fmt("(s[%u] & 1u)", in.src2 < kSArray ? in.src2 : 106) : "(s[106] & 1u)";
    auto carry_op = [&](const std::string& r, const std::string& c) {
        std::string s = "{ " + pre + "uint r = " + r + "; bool c = " + c + "; if (EXEC) v[" + std::to_string(d) + "] = r; ";
        if (!carry.empty()) s += carry + " = (EXEC && c) ? 1u : 0u; s[" + std::to_string((carry == "s[106]" ? 106 : in.sdst3) + 1) + "] = 0u; ";
        L(s + "}");
    };
    switch (op) {
    case 0x100: { // v_cndmask_b32
        const std::string m = vop3 ? Fmt("(%s & 1u)", Src(in.src2, in.literal).c_str()) : "(s[106] & 1u)";
        if (vop3 && (in.abs || in.neg)) fop(m + " != 0u ? " + f1 + " : " + f0);
        else uop(m + " != 0u ? a1 : a0");
        break;
    }
    case 0x101: L(Fmt("s[%u] = subgroupShuffle(%s, %s & 63u);", d, a0.c_str(), a1.c_str())); break; // readlane
    case 0x102: L(Fmt("if (gl_SubgroupInvocationID == (%s & 63u)) v[%u] = %s;", a1.c_str(), d, a0.c_str())); break;
    case 0x103: fop(f0 + " + " + f1); break;
    case 0x104: fop(f0 + " - " + f1); break;
    case 0x105: fop(f1 + " - " + f0); break;
    case 0x106: fop("MulLegacy(" + f0 + ", " + f1 + ") + F(dv)"); break;
    case 0x107: fop("MulLegacy(" + f0 + ", " + f1 + ")"); break;
    case 0x108: fop(f0 + " * " + f1); break;
    case 0x109: uop("uint(bitfieldExtract(int(a0), 0, 24) * bitfieldExtract(int(a1), 0, 24))"); break;
    case 0x10A: L(Fmt("{ %sint hi, lo; imulExtended(bitfieldExtract(int(a0), 0, 24), bitfieldExtract(int(a1), 0, 24), hi, lo); if (EXEC) v[%u] = uint(hi); }", pre.c_str(), d)); break;
    case 0x10B: uop("(a0 & 0xFFFFFFu) * (a1 & 0xFFFFFFu)"); break;
    case 0x10C: L(Fmt("{ %suint hi, lo; umulExtended(a0 & 0xFFFFFFu, a1 & 0xFFFFFFu, hi, lo); if (EXEC) v[%u] = hi; }", pre.c_str(), d)); break;
    case 0x10D: case 0x10F: fop("min(" + f0 + ", " + f1 + ")"); break;
    case 0x10E: case 0x110: fop("max(" + f0 + ", " + f1 + ")"); break;
    case 0x111: uop("uint(min(int(a0), int(a1)))"); break;
    case 0x112: uop("uint(max(int(a0), int(a1)))"); break;
    case 0x113: uop("min(a0, a1)"); break;
    case 0x114: uop("max(a0, a1)"); break;
    case 0x115: uop("a0 >> (a1 & 31u)"); break;
    case 0x116: uop("a1 >> (a0 & 31u)"); break;
    case 0x117: uop("uint(int(a0) >> int(a1 & 31u))"); break;
    case 0x118: uop("uint(int(a1) >> int(a0 & 31u))"); break;
    case 0x119: uop("a0 << (a1 & 31u)"); break;
    case 0x11A: uop("a1 << (a0 & 31u)"); break;
    case 0x11B: uop("a0 & a1"); break;
    case 0x11C: uop("a0 | a1"); break;
    case 0x11D: uop("a0 ^ a1"); break;
    case 0x11E: uop("((1u << (a0 & 31u)) - 1u) << (a1 & 31u)"); break;
    case 0x11F: fop(f0 + " * " + f1 + " + F(dv)"); break;
    case 0x120: fop(f0 + " * F(" + HexU(in.literal) + ") + " + f1); break; // madmk
    case 0x121: fop(f0 + " * " + f1 + " + F(" + HexU(in.literal) + ")"); break; // madak
    case 0x122: uop("uint(bitCount(a0)) + a1"); break;
    case 0x123: uop("(gl_SubgroupInvocationID < 32u ? uint(bitCount(a0 & ((1u << gl_SubgroupInvocationID) - 1u))) : uint(bitCount(a0))) + a1"); break;
    case 0x124: uop("(gl_SubgroupInvocationID < 32u ? 0u : uint(bitCount(a0 & ((1u << (gl_SubgroupInvocationID - 32u)) - 1u)))) + a1"); break;
    case 0x125: carry_op("a0 + a1", "(a0 + a1) < a0"); break;
    case 0x126: carry_op("a0 - a1", "a1 > a0"); break;
    case 0x127: carry_op("a1 - a0", "a0 > a1"); break;
    case 0x128: carry_op("a0 + a1 + " + cin, "(a0 + a1) < a0 || (a0 + a1 + " + cin + ") < (a0 + a1)"); break;
    case 0x129: carry_op("a0 - a1 - " + cin, "(a1 + " + cin + ") > a0 || (" + cin + " == 1u && a1 == 0xFFFFFFFFu)"); break;
    case 0x12A: carry_op("a1 - a0 - " + cin, "(a0 + " + cin + ") > a1 || (" + cin + " == 1u && a0 == 0xFFFFFFFFu)"); break;
    case 0x12B: fop("ldexp(" + f0 + ", int(a1))"); break;
    case 0x12C: uop("(dv & ~(0xFFu << ((a1 & 3u) * 8u))) | (uint(clamp(roundEven(" + f0 + "), 0.0, 255.0)) << ((a1 & 3u) * 8u))"); break;
    case 0x12D: uop("packSnorm2x16(vec2(" + f0 + ", " + f1 + "))"); break;
    case 0x12E: uop("packUnorm2x16(vec2(" + f0 + ", " + f1 + "))"); break;
    case 0x12F: uop("packHalf2x16(vec2(" + f0 + ", " + f1 + "))"); break;
    case 0x130: uop("(a0 & 0xFFFFu) | (a1 << 16u)"); break;
    case 0x131: uop("(uint(clamp(int(a0), -32768, 32767)) & 0xFFFFu) | (uint(clamp(int(a1), -32768, 32767)) << 16u)"); break;
    // VOP3-only
    case 0x140: fop("MulLegacy(" + f0 + ", " + f1 + ") + " + f2); break;
    case 0x141: fop(f0 + " * " + f1 + " + " + f2); break;
    case 0x142: uop("uint(bitfieldExtract(int(a0), 0, 24) * bitfieldExtract(int(a1), 0, 24) + int(a2))"); break;
    case 0x143: uop("(a0 & 0xFFFFFFu) * (a1 & 0xFFFFFFu) + a2"); break;
    case 0x144: fop("CubeId(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
    case 0x145: fop("CubeSc(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
    case 0x146: fop("CubeTc(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
    case 0x147: fop("CubeMa(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
    case 0x148: uop("((a2 & 31u) == 0u) ? 0u : bitfieldExtract(a0, int(a1 & 31u), int(min(a2 & 31u, 32u - (a1 & 31u))))"); break;
    case 0x149: uop("((a2 & 31u) == 0u) ? 0u : uint(bitfieldExtract(int(a0), int(a1 & 31u), int(min(a2 & 31u, 32u - (a1 & 31u)))))"); break;
    case 0x14A: uop("(a0 & a1) | (~a0 & a2)"); break;
    case 0x14B: fop("fma(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
    case 0x14E: uop("(a2 & 31u) == 0u ? a1 : (a1 >> (a2 & 31u)) | (a0 << (32u - (a2 & 31u)))"); break;
    case 0x14F: uop("(a2 & 3u) == 0u ? a1 : (a1 >> ((a2 & 3u) * 8u)) | (a0 << (32u - (a2 & 3u) * 8u))"); break;
    case 0x151: fop("min(min(" + f0 + ", " + f1 + "), " + f2 + ")"); break;
    case 0x152: uop("uint(min(min(int(a0), int(a1)), int(a2)))"); break;
    case 0x153: uop("min(min(a0, a1), a2)"); break;
    case 0x154: fop("max(max(" + f0 + ", " + f1 + "), " + f2 + ")"); break;
    case 0x155: uop("uint(max(max(int(a0), int(a1)), int(a2)))"); break;
    case 0x156: uop("max(max(a0, a1), a2)"); break;
    case 0x157: fop("Med3F(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
    case 0x158: uop("uint(Med3I(int(a0), int(a1), int(a2)))"); break;
    case 0x159: uop("Med3U(a0, a1, a2)"); break;
    case 0x15E: uop("(a2 & ~(0xFFu << ((a1 & 3u) * 8u))) | (uint(clamp(roundEven(" + f0 + "), 0.0, 255.0)) << ((a1 & 3u) * 8u))"); break;
    case 0x15F: fop(f0); break; // div_fixup: the quotient is already correct for finite inputs
    case 0x169: case 0x16B: uop("a0 * a1"); break;
    case 0x16A: L(Fmt("{ %suint hi, lo; umulExtended(a0, a1, hi, lo); if (EXEC) v[%u] = hi; }", pre.c_str(), d)); break;
    case 0x16C: L(Fmt("{ %sint hi, lo; imulExtended(int(a0), int(a1), hi, lo); if (EXEC) v[%u] = uint(hi); }", pre.c_str(), d)); break;
    case 0x16D: // div_scale: no scaling needed in practice
        L(Fmt("{ %sif (EXEC) v[%u] = a0; %s}", pre.c_str(), d,
              in.sdst3 < kSArray ? Fmt("s[%u] = 0u; s[%u] = 0u; ", in.sdst3, in.sdst3 + 1).c_str() : ""));
        break;
    case 0x16F: fop("fma(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
    // VOP1
    case 0x180: break; // v_nop
    case 0x181: if (vop3 && (in.abs || in.neg || in.omod || in.clamp)) fop(f0); else uop("a0"); break;
    case 0x182: L(Fmt("s[%u] = subgroupBroadcastFirst(%s);", d, a0.c_str())); break; // readfirstlane (SGPR dst)
    case 0x185: fop("float(int(a0))"); break;
    case 0x186: fop("float(a0)"); break;
    case 0x187: uop("CvtU32(" + f0 + ")"); break;
    case 0x188: uop("CvtI32(" + f0 + ")"); break;
    case 0x18A: uop("packHalf2x16(vec2(" + f0 + ", 0.0)) & 0xFFFFu"); break;
    case 0x18B: fop("unpackHalf2x16(a0).x"); break;
    case 0x18C: uop("CvtI32(floor(" + f0 + " + 0.5))"); break;
    case 0x18D: uop("CvtI32(floor(" + f0 + "))"); break;
    case 0x191: fop("float(a0 & 0xFFu)"); break;
    case 0x192: fop("float((a0 >> 8u) & 0xFFu)"); break;
    case 0x193: fop("float((a0 >> 16u) & 0xFFu)"); break;
    case 0x194: fop("float(a0 >> 24u)"); break;
    case 0x1A0: fop("fract(" + f0 + ")"); break;
    case 0x1A1: fop("trunc(" + f0 + ")"); break;
    case 0x1A2: fop("ceil(" + f0 + ")"); break;
    case 0x1A3: fop("roundEven(" + f0 + ")"); break;
    case 0x1A4: fop("floor(" + f0 + ")"); break;
    case 0x1A5: case 0x1C6: fop("exp2(" + f0 + ")"); break;
    case 0x1A6: case 0x1A7: case 0x1C5: fop("log2(" + f0 + ")"); break;
    case 0x1A8: case 0x1A9: case 0x1AA: case 0x1AB: fop("1.0 / " + f0); break;
    case 0x1AC: case 0x1AD: case 0x1AE: fop("inversesqrt(" + f0 + ")"); break;
    case 0x1B3: fop("sqrt(" + f0 + ")"); break;
    case 0x1B5: fop("sin(" + f0 + " * 6.283185307179586)"); break;
    case 0x1B6: fop("cos(" + f0 + " * 6.283185307179586)"); break;
    case 0x1B7: uop("~a0"); break;
    case 0x1B8: uop("bitfieldReverse(a0)"); break;
    case 0x1B9: uop("a0 == 0u ? 0xFFFFFFFFu : uint(31 - findMSB(a0))"); break;
    case 0x1BA: uop("uint(findLSB(a0))"); break;
    case 0x1BB: uop("(a0 == 0u || a0 == 0xFFFFFFFFu) ? 0xFFFFFFFFu : uint(31 - findMSB(int(a0)))"); break;
    case 0x1BF: L(Fmt("{ %sint e; frexp(%s, e); if (EXEC) v[%u] = uint(e); }", pre.c_str(), f0.c_str(), d)); break;
    case 0x1C0: L(Fmt("{ %sint e; float m = frexp(%s, e); if (EXEC) v[%u] = U(m); }", pre.c_str(), f0.c_str(), d)); break;
    case 0x1C1: break; // clrexcp
    default: Warn(in); break;
    }
}

void Generator::EmitInterp(const Instr& in) {
    const int loc = InputLocation(in.attr);
    const uint16_t d = uint16_t(in.dst - kVgprBase);
    const char ch = "xyzw"[in.attr_chan & 3];
    if (loc < 0) {
        const uint32_t def = (key_.ps_input_cntl[in.attr & 31] >> 8) & 3;
        static const float defs[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 0}, {1, 1, 1, 1}};
        L(Fmt("if (EXEC) v[%u] = U(%f);", d, defs[def][in.attr_chan & 3]));
        return;
    }
    inputs_.insert(loc);
    if (key_.ps_input_cntl[in.attr & 31] & (1u << 10)) flat_[loc] = true;
    if (in.op == 2) flat_[loc] = flat_[loc] || false;
    L(Fmt("if (EXEC) v[%u] = U(i_%d.%c);", d, loc, ch));
}

void Generator::EmitDs(const Instr& in) {
    const uint16_t d = uint16_t(in.dst - kVgprBase);
    if (in.op == 53) { // ds_swizzle_b32
        const uint32_t off = in.mem_offset;
        std::string e;
        if (off & 0x8000) {
            const uint32_t p = off & 0xFF;
            if (p == 0x00 || p == 0x55 || p == 0xAA || p == 0xFF)
                e = Fmt("subgroupQuadBroadcast(v[%u], %uu)", in.vaddr, p & 3);
            else
                e = Fmt("subgroupShuffle(v[%u], (gl_SubgroupInvocationID & ~3u) | ((%uu >> ((gl_SubgroupInvocationID & 3u) * 2u)) & 3u))",
                        in.vaddr, p);
        } else {
            const uint32_t andm = off & 0x1F, orm = (off >> 5) & 0x1F, xorm = (off >> 10) & 0x1F;
            e = Fmt("subgroupShuffle(v[%u], (gl_SubgroupInvocationID & ~31u) | ((((gl_SubgroupInvocationID & 31u) & %uu) | %uu) ^ %uu))",
                    in.vaddr, andm, orm, xorm);
        }
        L(Fmt("{ uint t = %s; if (EXEC) v[%u] = t; }", e.c_str(), d));
        return;
    }
    // LDS. GCN waves run in lockstep and these shaders rely on it instead of s_barrier, so every LDS
    // write is followed by a shared-memory barrier (the pipeline requests 64-wide subgroups).
    if (key_.stage != Stage::Compute) { Warn(in); return; }
    const uint32_t off0 = in.mem_offset & 0xFF, off1 = in.mem_offset >> 8, off16 = in.mem_offset;
    const uint16_t a = in.vaddr, d0 = in.vdata, d1 = uint16_t((in.w1 >> 16) & 0xFF);
    const uint32_t mask = lds_mask_;
    auto addr = [&](uint32_t byte_off) { return Fmt("(((v[%u] + %uu) >> 2u) & %uu)", a, byte_off, mask); };
    static const char* atom[12] = {"atomicAdd", "atomicAdd", nullptr, nullptr, nullptr, "atomicMin", "atomicMax",
                                   "atomicMin", "atomicMax", "atomicAnd", "atomicOr", "atomicXor"};
    switch (in.op) {
    case 13: L(Fmt("if (EXEC) lds[%s] = v[%u]; memoryBarrierShared();", addr(off16).c_str(), d0)); return;
    case 14:
        L(Fmt("if (EXEC) { lds[%s] = v[%u]; lds[%s] = v[%u]; } memoryBarrierShared();", addr(off0 * 4).c_str(), d0,
              addr(off1 * 4).c_str(), d1));
        return;
    case 15:
        L(Fmt("if (EXEC) { lds[%s] = v[%u]; lds[%s] = v[%u]; } memoryBarrierShared();", addr(off0 * 256).c_str(), d0,
              addr(off1 * 256).c_str(), d1));
        return;
    case 77:
        L(Fmt("if (EXEC) { lds[%s] = v[%u]; lds[%s] = v[%u]; } memoryBarrierShared();", addr(off16).c_str(), d0,
              addr(off16 + 4).c_str(), d0 + 1));
        return;
    case 78:
        L(Fmt("if (EXEC) { lds[%s] = v[%u]; lds[%s] = v[%u]; lds[%s] = v[%u]; lds[%s] = v[%u]; } memoryBarrierShared();",
              addr(off0 * 8).c_str(), d0, addr(off0 * 8 + 4).c_str(), d0 + 1, addr(off1 * 8).c_str(), d1,
              addr(off1 * 8 + 4).c_str(), d1 + 1));
        return;
    case 54: L(Fmt("{ uint t = lds[%s]; if (EXEC) v[%u] = t; }", addr(off16).c_str(), in.dst - kVgprBase)); return;
    case 55:
        L(Fmt("{ uint t0 = lds[%s], t1 = lds[%s]; if (EXEC) { v[%u] = t0; v[%u] = t1; } }", addr(off0 * 4).c_str(),
              addr(off1 * 4).c_str(), in.dst - kVgprBase, in.dst - kVgprBase + 1));
        return;
    case 56:
        L(Fmt("{ uint t0 = lds[%s], t1 = lds[%s]; if (EXEC) { v[%u] = t0; v[%u] = t1; } }", addr(off0 * 256).c_str(),
              addr(off1 * 256).c_str(), in.dst - kVgprBase, in.dst - kVgprBase + 1));
        return;
    case 118:
        L(Fmt("{ uint t0 = lds[%s], t1 = lds[%s]; if (EXEC) { v[%u] = t0; v[%u] = t1; } }", addr(off16).c_str(),
              addr(off16 + 4).c_str(), in.dst - kVgprBase, in.dst - kVgprBase + 1));
        return;
    case 119:
        L(Fmt("{ uint t0 = lds[%s], t1 = lds[%s], t2 = lds[%s], t3 = lds[%s]; if (EXEC) { v[%u] = t0; v[%u] = t1; v[%u] = t2; v[%u] = t3; } }",
              addr(off0 * 8).c_str(), addr(off0 * 8 + 4).c_str(), addr(off1 * 8).c_str(), addr(off1 * 8 + 4).c_str(),
              in.dst - kVgprBase, in.dst - kVgprBase + 1, in.dst - kVgprBase + 2, in.dst - kVgprBase + 3));
        return;
    default:
        if (in.op < 12 && atom[in.op]) { // non-returning LDS atomics (signed min/max treated as unsigned)
            if (in.op == 1)
                L(Fmt("if (EXEC) atomicAdd(lds[%s], uint(-int(v[%u]))); memoryBarrierShared();", addr(off16).c_str(), d0));
            else
                L(Fmt("if (EXEC) %s(lds[%s], v[%u]); memoryBarrierShared();", atom[in.op], addr(off16).c_str(), d0));
            return;
        }
        Warn(in);
    }
}

void Generator::EmitSmrd(size_t idx, const Instr& in) {
    if (in.op == 30) { L(Fmt("s[%u] = 0u; s[%u] = 0u;", in.dst, in.dst + 1)); return; } // s_memtime
    if (!IsSmrdLoad(in)) return;
    auto it = smrd_index_.find(idx);
    if (it == smrd_index_.end()) return;
    const uint32_t n = SmrdDwords(in);
    std::string s = "{ ";
    for (uint32_t i = 0; i < n; ++i)
        if (in.dst + i < kSArray) s += Fmt("s[%u] = sd[%u]; ", in.dst + i, it->second + i);
    L(s + "}");
}

void Generator::EmitBuffer(size_t idx, const Instr& in) {
    const int slot = res_[idx].buf;
    const bool load_fmt = (in.fmt == Fmt::MUBUF && in.op <= 3) || (in.fmt == Fmt::MTBUF && in.op <= 3);
    const bool load_raw = in.fmt == Fmt::MUBUF && in.op >= 8 && in.op <= 15;
    const bool store_fmt = in.op >= 4 && in.op <= 7;
    const bool store_raw = in.fmt == Fmt::MUBUF && in.op >= 24 && in.op <= 31;
    if (slot < 0 || !(load_fmt || load_raw || store_fmt || store_raw)) { Warn(in); return; }
    const uint32_t b = t_.resources[slot].binding;
    const uint16_t r = in.srsrc;
    const std::string index = in.idxen ? Fmt("v[%u]", in.vaddr) : std::string("0u");
    std::string offset = Fmt("%uu", in.mem_offset);
    if (in.offen) offset += Fmt(" + v[%u]", in.vaddr + (in.idxen ? 1 : 0));
    const std::string addr = Fmt("buf_addr(s[%u], s[%u], %s, %s, %s)", r + 1, r + 3, index.c_str(), offset.c_str(),
                                 Src(in.soffset, 0).c_str());
    std::string s = Fmt("{ uint a = %s; ", addr.c_str());
    if (store_fmt || store_raw) {
        // Stores write dwords (formats other than 32-bit components are stored as raw dwords).
        uint32_t n = 1;
        if (store_fmt) n = (in.op & 3) + 1;
        else if (in.op == 29) n = 2;
        else if (in.op == 30) n = 4;
        else if (in.op == 31) n = 3;
        s += "if (EXEC) { ";
        if (store_raw && in.op == 24)
            s += Fmt("atomicAnd(b_%u.d[a >> 2u], ~(0xFFu << ((a & 3u) * 8u))); atomicOr(b_%u.d[a >> 2u], (v[%u] & 0xFFu) << ((a & 3u) * 8u)); ",
                     b, b, in.vdata);
        else if (store_raw && in.op == 26)
            s += Fmt("atomicAnd(b_%u.d[a >> 2u], ~(0xFFFFu << ((a & 2u) * 8u))); atomicOr(b_%u.d[a >> 2u], (v[%u] & 0xFFFFu) << ((a & 2u) * 8u)); ",
                     b, b, in.vdata);
        else
            for (uint32_t i = 0; i < n; ++i) s += Fmt("b_%u.d[(a >> 2u) + %uu] = v[%u]; ", b, i, in.vdata + i);
        L(s + "} }");
        return;
    }
    if (load_fmt) {
        const uint32_t n = (in.op & 3) + 1;
        std::string w3 = Fmt("s[%u]", r + 3);
        if (in.fmt == Fmt::MTBUF) w3 = HexU((uint32_t(in.dfmt) << 15) | (uint32_t(in.nfmt) << 12) | 0xFAC);
        s += Fmt("uvec4 r = fmt_%u(a, %s); if (EXEC) { ", b, w3.c_str());
        for (uint32_t i = 0; i < n; ++i) s += Fmt("v[%u] = r[%u]; ", in.vdata + i, i);
        s += "} }";
    } else {
        switch (in.op) {
        case 8: s += Fmt("uint r = rw_%u(a) & 0xFFu; if (EXEC) v[%u] = r; }", b, in.vdata); break;
        case 9: s += Fmt("uint r = uint(bitfieldExtract(int(rw_%u(a)), 0, 8)); if (EXEC) v[%u] = r; }", b, in.vdata); break;
        case 10: s += Fmt("uint r = rw_%u(a) & 0xFFFFu; if (EXEC) v[%u] = r; }", b, in.vdata); break;
        case 11: s += Fmt("uint r = uint(bitfieldExtract(int(rw_%u(a)), 0, 16)); if (EXEC) v[%u] = r; }", b, in.vdata); break;
        default: {
            const uint32_t n = in.op == 12 ? 1 : in.op == 13 ? 2 : in.op == 14 ? 4 : 3;
            s += "if (EXEC) { ";
            for (uint32_t i = 0; i < n; ++i) s += Fmt("v[%u] = rw_%u(a + %uu); ", in.vdata + i, b, i * 4);
            s += "} }";
        }
        }
    }
    L(s);
}

void Generator::EmitMimg(size_t idx, const Instr& in) {
    const int tslot = res_[idx].tex, sslot = res_[idx].samp;
    if (tslot < 0) { Warn(in); return; }
    if (in.op == 0x08 || in.op == 0x09) { // image_store / image_store_mip (2D storage image)
        const uint32_t sb = t_.resources[tslot].binding;
        std::string c[4] = {"0.0", "0.0", "0.0", "0.0"};
        uint32_t k = 0;
        for (uint32_t ch = 0; ch < 4; ++ch)
            if (in.dmask & (1 << ch)) c[ch] = Fmt("F(v[%u])", in.vdata + k++);
        L(Fmt("if (EXEC) imageStore(si_%u, ivec2(int(v[%u]), int(v[%u])), vec4(%s, %s, %s, %s));", sb, in.vaddr,
              in.vaddr + 1, c[0].c_str(), c[1].c_str(), c[2].c_str(), c[3].c_str()));
        return;
    }
    const uint32_t tb = t_.resources[tslot].binding;
    const uint8_t dimkind = tslot < 64 ? key_.image_dims[tslot] : 0;
    const uint32_t dim = dimkind & 15, kind = (dimkind >> 4) & 3;
    const char* samp_type = dim == 1 ? "sampler3D" : dim == 2 ? "samplerCube" : dim == 3 ? "sampler2DArray" : "sampler2D";
    const char* kp = kind == 1 ? "u" : kind == 2 ? "i" : "";
    const std::string tex = Fmt("t_%u", tb);
    auto conv = [&](const std::string& e) {
        return kind == 0 ? "U(" + e + ")" : kind == 1 ? e : "uint(" + e + ")";
    };
    const uint32_t ncoord = dim == 0 ? 2 : 3;
    const uint16_t op = in.op;
    uint32_t a = in.vaddr;
    std::string s = "{ ";
    std::string res; // vec4 expression
    bool gather = false;

    if (op == 0x00 || op == 0x01) { // image_load / image_load_mip
        const std::string c = ncoord == 2 ? Fmt("ivec2(int(v[%u]), int(v[%u]))", a, a + 1)
                                          : Fmt("ivec3(int(v[%u]), int(v[%u]), int(v[%u]))", a, a + 1, a + 2);
        const std::string lod = op == 0x01 ? Fmt("int(v[%u])", a + ncoord) : "0";
        if (dim == 2) res = Fmt("vec4(0.0)"); // cube loads not supported
        else res = Fmt("texelFetch(%s, %s, %s)", tex.c_str(), c.c_str(), lod.c_str());
    } else if (op == 0x0E) { // image_get_resinfo
        const std::string lod = Fmt("int(v[%u])", a);
        if (dim == 0 || dim == 2)
            s += Fmt("ivec2 sz = textureSize(%s, %s); uvec4 q = uvec4(uint(sz.x), uint(sz.y), 1u, uint(textureQueryLevels(%s))); ",
                     tex.c_str(), lod.c_str(), tex.c_str());
        else
            s += Fmt("ivec3 sz = textureSize(%s, %s); uvec4 q = uvec4(uint(sz.x), uint(sz.y), uint(sz.z), uint(textureQueryLevels(%s))); ",
                     tex.c_str(), lod.c_str(), tex.c_str());
        s += "if (EXEC) { ";
        uint32_t k = 0;
        for (uint32_t c = 0; c < 4; ++c)
            if (in.dmask & (1 << c)) s += Fmt("v[%u] = q[%u]; ", in.vdata + k++, c);
        L(s + "} }");
        return;
    } else if (op >= 0x20 && op <= 0x60) {
        if (sslot < 0) { Warn(in); return; }
        const uint32_t sb = t_.resources[sslot].binding;
        const std::string sampler = Fmt("%s%s(%s, sm_%u)", kp, samp_type, tex.c_str(), sb);
        gather = op >= 0x40 && op < 0x60;
        const uint16_t base = gather ? uint16_t(op - 0x40 + 0x20) : op;
        const uint16_t v = base & 7;
        const bool has_off = (base & 0x10) != 0;
        const bool cmp = (base & 0x08) != 0;
        const bool bias = v == 5 || v == 6;
        const bool lod = v == 4;
        const bool lz = v == 7;
        const bool grad = v == 2 || v == 3;
        const bool clampv = v == 1 || v == 3 || v == 6;
        if (has_off) s += Fmt("uint off = v[%u]; ", a++);
        if (bias) s += Fmt("float bias = F(v[%u]); ", a++);
        if (cmp) s += Fmt("float zref = F(v[%u]); ", a++);
        const uint32_t ngrad = dim == 1 || dim == 2 ? 3 : 2;
        if (grad) {
            s += ngrad == 2 ? Fmt("vec2 dx = vec2(F(v[%u]), F(v[%u])); vec2 dy = vec2(F(v[%u]), F(v[%u])); ", a, a + 1, a + 2, a + 3)
                            : Fmt("vec3 dx = vec3(F(v[%u]), F(v[%u]), F(v[%u])); vec3 dy = vec3(F(v[%u]), F(v[%u]), F(v[%u])); ",
                                  a, a + 1, a + 2, a + 3, a + 4, a + 5);
            a += ngrad * 2;
        }
        if (dim == 0) s += Fmt("vec2 uv = vec2(F(v[%u]), F(v[%u])); ", a, a + 1);
        else if (dim == 2) s += Fmt("vec3 uv = CubeDir(F(v[%u]), F(v[%u]), F(v[%u])); ", a, a + 1, a + 2);
        else if (dim == 3) s += Fmt("vec3 uv = vec3(F(v[%u]), F(v[%u]), roundEven(F(v[%u]))); ", a, a + 1, a + 2);
        else s += Fmt("vec3 uv = vec3(F(v[%u]), F(v[%u]), F(v[%u])); ", a, a + 1, a + 2);
        a += ncoord;
        if (lod) s += Fmt("float lod = F(v[%u]); ", a++);
        else if (clampv) a++;
        if (in.unorm && dim != 2) {
            if (dim == 0) s += Fmt("uv /= vec2(textureSize(%s, 0)); ", tex.c_str());
            else if (dim == 3) s += Fmt("uv.xy /= vec2(textureSize(%s, 0).xy); ", tex.c_str());
            else s += Fmt("uv /= vec3(textureSize(%s, 0)); ", tex.c_str());
        }
        if (has_off && dim != 2) {
            if (dim == 0 || dim == 3)
                s += Fmt("uv.xy += vec2(float(bitfieldExtract(int(off), 0, 6)), float(bitfieldExtract(int(off), 8, 6))) / vec2(textureSize(%s, 0).xy); ",
                         tex.c_str());
            else
                s += Fmt("uv += vec3(float(bitfieldExtract(int(off), 0, 6)), float(bitfieldExtract(int(off), 8, 6)), float(bitfieldExtract(int(off), 16, 6))) / vec3(textureSize(%s, 0)); ",
                         tex.c_str());
        }
        if (op == 0x60) {
            res = Fmt("vec4(textureQueryLod(%s, uv), 0.0, 0.0)", sampler.c_str());
        } else if (gather) {
            int comp = 0;
            for (int c = 0; c < 4; ++c)
                if (in.dmask & (1 << c)) { comp = c; break; }
            res = Fmt("textureGather(%s, uv, %d)", sampler.c_str(), comp);
            if (kind != 0) res = "vec4(" + res + ")";
        } else {
            std::string call;
            if (lz) call = Fmt("textureLod(%s, uv, 0.0)", sampler.c_str());
            else if (lod) call = Fmt("textureLod(%s, uv, lod)", sampler.c_str());
            else if (grad) call = Fmt("textureGrad(%s, uv, dx, dy)", sampler.c_str());
            else if (bias) call = Fmt("texture(%s, uv, bias)", sampler.c_str());
            else call = Fmt("texture(%s, uv)", sampler.c_str());
            res = call;
            if (cmp) {
                // compare each texel, then filter (2x2 PCF through a gather) like the hardware does
                s += Fmt("uint cf = (s[%u] >> 12u) & 7u; zref = clamp(zref, 0.0, 1.0); ", in.ssamp);
                if ((dim == 0 || dim == 3) && !grad && !lod) {
                    s += Fmt("vec2 gsz = vec2(textureSize(%s, 0).xy); vec2 gf = fract(uv.xy * gsz - 0.5); "
                             "vec4 gt = textureGather(%s, uv, 0); "
                             "vec4 gc = vec4(DCmp(cf, zref, gt.x), DCmp(cf, zref, gt.y), DCmp(cf, zref, gt.z), DCmp(cf, zref, gt.w)); ",
                             tex.c_str(), sampler.c_str());
                    res = "vec4(mix(mix(gc.w, gc.z, gf.x), mix(gc.x, gc.y, gf.x), gf.y))";
                } else {
                    res = Fmt("vec4(DCmp(cf, zref, float(%s.x)))", call.c_str());
                }
            }
        }
        if (kind != 0 && !cmp && !gather && op != 0x60) {
            // integer textures: keep raw values
            s += Fmt("%svec4 rv = %s; if (EXEC) { ", kind == 1 ? "u" : "i", res.c_str());
            uint32_t k = 0;
            for (uint32_t c = 0; c < 4; ++c)
                if (in.dmask & (1 << c)) s += Fmt("v[%u] = uint(rv[%u]); ", in.vdata + k++, c);
            L(s + "} }");
            return;
        }
    } else {
        Warn(in);
        return;
    }
    if (kind != 0 && (op == 0x00 || op == 0x01)) {
        s += Fmt("%svec4 rv = %s; if (EXEC) { ", kind == 1 ? "u" : "i", res.c_str());
        uint32_t k = 0;
        for (uint32_t c = 0; c < 4; ++c)
            if (in.dmask & (1 << c)) s += Fmt("v[%u] = uint(rv[%u]); ", in.vdata + k++, c);
        L(s + "} }");
        return;
    }
    s += Fmt("vec4 rv = %s; if (EXEC) { ", res.c_str());
    if (gather) {
        for (uint32_t c = 0; c < 4; ++c) s += Fmt("v[%u] = U(rv[%u]); ", in.vdata + c, c);
    } else {
        uint32_t k = 0;
        for (uint32_t c = 0; c < 4; ++c)
            if (in.dmask & (1 << c)) s += Fmt("v[%u] = U(rv[%u]); ", in.vdata + k++, c);
    }
    (void)conv;
    L(s + "} }");
}

void Generator::EmitExp(const Instr& in) {
    const uint8_t tgt = in.exp_target;
    const auto vs = [&](int k) { return Fmt("F(v[%u])", in.exp_src[k]); };
    if (key_.stage == Stage::Vertex) {
        if (tgt == 12) {
            L(Fmt("gl_Position = vec4(%s, %s, %s, %s);", vs(0).c_str(), vs(1).c_str(), vs(2).c_str(), vs(3).c_str()));
            L("gl_Position.z = gl_Position.z * pc.zfix.x + gl_Position.w * pc.zfix.y;");
            if (key_.gs_mode == ShaderKey::kGsCopy) L("if (gs_kill) gl_Position = vec4(0.0);");
        } else if (tgt >= 32 && tgt < 64) {
            const uint32_t p = tgt - 32;
            t_.param_mask |= 1u << p;
            L(Fmt("o_p%u = vec4(%s, %s, %s, %s);", p, vs(0).c_str(), vs(1).c_str(), vs(2).c_str(), vs(3).c_str()));
        }
        return;
    }
    // pixel shader
    if (tgt < 8) {
        t_.mrt_mask |= 1u << tgt;
        // Debug (GR_DEBUG_NOKILL=<hex PS address>): keep killed pixels of one pixel shader
        static const uint64_t nokill = [] { const char* e = std::getenv("GR_DEBUG_NOKILL"); return e ? std::strtoull(e, nullptr, 16) : 0ull; }();
        if (nokill != key_.code) L("if (!EXEC) discard;");
        const uint8_t kind = key_.mrt_kind[tgt];
        uint8_t sw = key_.mrt_swizzle[tgt];
        if (sw == 0) sw = 0xE4;
        std::string c;
        if (kind == 0) {
            c = in.exp_compr ? Fmt("vec4(unpackHalf2x16(v[%u]), unpackHalf2x16(v[%u]))", in.exp_src[0], in.exp_src[1])
                             : Fmt("vec4(%s, %s, %s, %s)", vs(0).c_str(), vs(1).c_str(), vs(2).c_str(), vs(3).c_str());
        } else {
            const char* ty = kind == 1 ? "uvec4" : "ivec4";
            c = in.exp_compr ? Fmt("%s(uvec4(v[%u] & 0xFFFFu, v[%u] >> 16u, v[%u] & 0xFFFFu, v[%u] >> 16u))", ty,
                                   in.exp_src[0], in.exp_src[0], in.exp_src[1], in.exp_src[1])
                             : Fmt("%s(uvec4(v[%u], v[%u], v[%u], v[%u]))", ty, in.exp_src[0], in.exp_src[1],
                                   in.exp_src[2], in.exp_src[3]);
        }
        L(Fmt("{ %s c = %s; o_c%u = %s(c[%u], c[%u], c[%u], c[%u]); }", kind == 0 ? "vec4" : kind == 1 ? "uvec4" : "ivec4",
              c.c_str(), tgt, kind == 0 ? "vec4" : kind == 1 ? "uvec4" : "ivec4", sw & 3, (sw >> 2) & 3, (sw >> 4) & 3,
              (sw >> 6) & 3));
    } else if (tgt == 8) {
        L("if (!EXEC) discard;");
        if (in.exp_en & 1) {
            t_.writes_depth = true;
            L(Fmt("gl_FragDepth = %s;", vs(0).c_str()));
        }
    } else if (tgt == 9) {
        L("if (!EXEC) discard;");
    }
}

void Generator::Emit(size_t idx, const Instr& in) {
    switch (in.fmt) {
    case Fmt::SOP2: EmitSop2(in); break;
    case Fmt::SOP1: EmitSop1(in); break;
    case Fmt::SOPK: EmitSopk(in); break;
    case Fmt::SOPC: EmitSopc(in); break;
    case Fmt::SOPP: break; // control flow handled by Run()
    case Fmt::SMRD: EmitSmrd(idx, in); break;
    case Fmt::VOP1:
    case Fmt::VOP2:
    case Fmt::VOPC:
    case Fmt::VOP3: EmitValu(in); break;
    case Fmt::VINTRP: EmitInterp(in); break;
    case Fmt::DS: EmitDs(in); break;
    case Fmt::MUBUF:
    case Fmt::MTBUF: EmitBuffer(idx, in); break;
    case Fmt::MIMG: EmitMimg(idx, in); break;
    case Fmt::EXP: EmitExp(in); break;
    default: Warn(in); break;
    }
}

std::string Generator::Run() {
    const auto& prog = t_.program;
    const bool vs = key_.stage == Stage::Vertex;
    const bool cs = key_.stage == Stage::Compute;
    uint32_t lds_size = 64;
    while (lds_size < key_.lds_dwords && lds_size < 16384) lds_size <<= 1;
    lds_mask_ = lds_size - 1;

    // labels
    std::set<uint32_t> labels;
    bool branches = false;
    for (const auto& in : prog)
        if (IsBranch(in)) { branches = true; labels.insert(BranchTarget(in)); }

    if (branches) {
        L("int lbl = 0;");
        L("while (true) {");
        ++indent_;
        L("switch (lbl) {");
        L("case 0:");
    }
    for (size_t i = 0; i < prog.size(); ++i) {
        const Instr& in = prog[i];
        if (branches && in.pc != 0 && labels.count(in.pc)) L(Fmt("case %u:", in.pc));
        if (in.fmt == Fmt::SOPP) {
            if (in.op == 1) { // s_endpgm
                if (key_.gs_mode == ShaderKey::kGsGeometry)
                    L("{ if (gid < ax[0]) ax[1u + gid] = gs_cnt | (gs_cut << 8u); return; }");
                else
                    L("return;");
                continue;
            }
            if (in.op == 16 && key_.gs_mode == ShaderKey::kGsGeometry) { // s_sendmsg: GS emit / cut
                const uint32_t msg = uint32_t(in.simm16) & 0xF, op = (uint32_t(in.simm16) >> 4) & 3;
                if (msg == 2 && (op == 2 || op == 3))
                    L("if (EXEC) { if (gs_pend) { gs_cut |= 1u << gs_cnt; gs_pend = false; } gs_cnt++; }");
                if (msg == 2 && (op == 1 || op == 3)) L("if (EXEC) gs_pend = true;");
                continue;
            }
            if (in.op == 10 && cs) { L("barrier();"); continue; } // s_barrier
            if (IsBranch(in)) {
                // VCCZ/EXECZ look at the whole wave: evaluated over the subgroup so control flow stays uniform.
                const uint32_t tgt = BranchTarget(in);
                const char* cond = "true";
                switch (in.op) {
                case 4: cond = "!scc"; break;
                case 5: cond = "scc"; break;
                case 6: cond = "!subgroupAny((s[106] & 1u) != 0u)"; break;
                case 7: cond = "subgroupAny((s[106] & 1u) != 0u)"; break;
                case 8: cond = "!subgroupAny(EXEC)"; break;
                case 9: cond = "subgroupAny(EXEC)"; break;
                default: break;
                }
                L(Fmt("if (%s) { lbl = %u; continue; }", cond, tgt));
            }
            continue;
        }
        Emit(i, in);
    }
    if (branches) {
        L("default: break;");
        L("}");
        L("break;");
        --indent_;
        L("}");
    }

    // ---- assemble the full shader
    const uint32_t set = DescriptorSetFor(key_.stage);
    std::string g = "#version 460\n"
                    "#extension GL_EXT_samplerless_texture_functions : require\n"
                    "#extension GL_KHR_shader_subgroup_basic : require\n"
                    "#extension GL_KHR_shader_subgroup_ballot : require\n"
                    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
                    "#extension GL_KHR_shader_subgroup_quad : require\n"
                    "#extension GL_KHR_shader_subgroup_vote : require\n";
    g += Fmt("// %s shader at 0x%llx (fetch 0x%llx)\n", vs ? "vertex" : cs ? "compute" : "pixel",
             (unsigned long long)key_.code, (unsigned long long)key_.fetch);
    if (cs) {
        g += Fmt("layout(local_size_x = %u, local_size_y = %u, local_size_z = %u) in;\n", std::max<uint32_t>(key_.cs_threads[0], 1),
                 std::max<uint32_t>(key_.cs_threads[1], 1), std::max<uint32_t>(key_.cs_threads[2], 1));
        g += Fmt("shared uint lds[%u];\n", lds_size);
    }
    g += Fmt("layout(set = %u, binding = 0, std430) readonly buffer SData { uint sd[]; };\n", set);
    for (size_t r = 0; r < t_.resources.size(); ++r) {
        const auto& rs = t_.resources[r];
        if (rs.kind == ResourceSlot::kTexture) {
            const uint8_t dk = r < 64 ? key_.image_dims[r] : 0;
            const uint32_t dim = dk & 15, kind = (dk >> 4) & 3;
            const char* ty = dim == 1 ? "texture3D" : dim == 2 ? "textureCube" : dim == 3 ? "texture2DArray" : "texture2D";
            g += Fmt("layout(set = %u, binding = %u) uniform %s%s t_%u;\n", set, rs.binding,
                     kind == 1 ? "u" : kind == 2 ? "i" : "", ty, rs.binding);
        } else if (rs.kind == ResourceSlot::kSampler) {
            g += Fmt("layout(set = %u, binding = %u) uniform sampler sm_%u;\n", set, rs.binding, rs.binding);
        } else if (rs.kind == ResourceSlot::kStorageImage) {
            g += Fmt("layout(set = %u, binding = %u) writeonly uniform image2D si_%u;\n", set, rs.binding, rs.binding);
        } else {
            g += Fmt("layout(set = %u, binding = %u, std430) %sbuffer Buf%u { uint d[]; } b_%u;\n", set, rs.binding,
                     rs.written ? "" : "readonly ", rs.binding, rs.binding);
        }
    }
    if (key_.gs_mode != ShaderKey::kGsNone)
        g += Fmt("layout(set = %u, binding = %u, std430) buffer Aux { uint ax[]; };\n", set, kAuxBinding);
    // lane index within the 64-wide wave (buffer ADD_TID addressing)
    g += cs ? "#define LANE (gl_LocalInvocationIndex & 63u)\n" : "#define LANE gl_SubgroupInvocationID\n";
    if (cs) {
        // no stage inputs/outputs
    } else if (vs) {
        g += "layout(push_constant) uniform PC { vec4 zfix; } pc;\n";
        for (uint32_t p = 0; p < 32; ++p)
            if (t_.param_mask & (1u << p)) g += Fmt("layout(location = %u) out vec4 o_p%u;\n", p, p);
    } else {
        for (int loc : inputs_) {
            g += Fmt("layout(location = %d) %sin vec4 i_%d;\n", loc, flat_.count(loc) && flat_[loc] ? "flat " : "", loc);
            t_.param_mask |= 1u << loc;
        }
        for (uint32_t m = 0; m < 8; ++m)
            if (t_.mrt_mask & (1u << m))
                g += Fmt("layout(location = %u) out %s o_c%u;\n", m,
                         key_.mrt_kind[m] == 1 ? "uvec4" : key_.mrt_kind[m] == 2 ? "ivec4" : "vec4", m);
    }
    g += kHelpers;
    for (const auto& rs : t_.resources) {
        if (rs.kind != ResourceSlot::kVertexBuffer && rs.kind != ResourceSlot::kRawBuffer) continue;
        std::string h = kBufferHelpers;
        const std::string n = std::to_string(rs.binding);
        for (size_t p = h.find('@'); p != std::string::npos; p = h.find('@', p)) h.replace(p, 1, n);
        g += h;
    }

    const uint32_t nv = t_.num_vgprs;
    g += "void main() {\n";
    g += Fmt("    uint s[%u];\n    uint v[%u];\n    bool scc = false;\n", kSArray, nv);
    g += Fmt("    for (int i = 0; i < %u; ++i) s[i] = sd[i];\n", kSgprCount);
    g += Fmt("    for (int i = %u; i < %u; ++i) s[i] = 0u;\n", kSgprCount, kSArray);
    g += "    s[126] = 1u;\n";
    g += Fmt("    for (int i = 0; i < %u; ++i) v[i] = 0u;\n", nv);
    const uint32_t gsm = key_.gs_mode;
    if (gsm == ShaderKey::kGsExport) {
        // ES as compute: invocation i shades index i of the draw; ES2GS_OFFSET follows the user SGPRs
        g += "    uint gid = gl_GlobalInvocationID.x;\n";
        g += "    if (gid >= ax[0]) s[126] = 0u;\n";
        g += "    v[0] = gid < ax[0] ? ax[1u + gid] : 0u;\n";
        g += Fmt("    s[%u] = gl_WorkGroupID.x * %uu;\n", key_.user_sgprs, key_.gs_params[0]);
    } else if (gsm == ShaderKey::kGsGeometry) {
        // GS as compute: invocation p shades triangle p. Input VGPRs: v0/v1/v3 = ESGS offsets (dwords) of the
        // triangle's vertices (indices 3p..3p+2 as the ES pass laid them out), v2 = primitive id.
        // SGPRs after the user ones: GS2VS_OFFSET, GS wave id.
        g += "    uint gid = gl_GlobalInvocationID.x;\n";
        g += "    if (gid >= ax[0]) s[126] = 0u;\n";
        const uint32_t esd = key_.gs_params[0] / 4;
        g += Fmt("    v[0] = ((3u * gid) / 64u) * %uu + (3u * gid) %% 64u;\n", esd);
        g += Fmt("    v[1] = ((3u * gid + 1u) / 64u) * %uu + (3u * gid + 1u) %% 64u;\n", esd);
        g += Fmt("    v[3] = ((3u * gid + 2u) / 64u) * %uu + (3u * gid + 2u) %% 64u;\n", esd);
        g += "    v[2] = gid;\n";
        g += Fmt("    s[%u] = gl_WorkGroupID.x * %uu;\n", key_.user_sgprs, key_.gs_params[1]);
        g += Fmt("    s[%u] = gl_WorkGroupID.x;\n", key_.user_sgprs + 1);
        g += "    uint gs_cnt = 0u, gs_cut = 0u;\n    bool gs_pend = false;\n";
    }
    if (gsm == ShaderKey::kGsExport || gsm == ShaderKey::kGsGeometry) {
        // (the regular compute prologue below is skipped)
    } else if (cs) {
        // VGPRs: thread id in the group; SGPRs after the user data: group id x/y/z and group size (if enabled)
        g += "    v[0] = gl_LocalInvocationID.x;\n    v[1] = gl_LocalInvocationID.y;\n    v[2] = gl_LocalInvocationID.z;\n";
        uint32_t sg = key_.user_sgprs;
        static const char* ids[3] = {"gl_WorkGroupID.x", "gl_WorkGroupID.y", "gl_WorkGroupID.z"};
        for (int k = 0; k < 3; ++k)
            if (key_.cs_sys_sgprs & (1 << k)) g += Fmt("    s[%u] = %s;\n", sg++, ids[k]);
        if (key_.cs_sys_sgprs & 8) g += Fmt("    s[%u] = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z;\n", sg++);
    } else if (vs && gsm == ShaderKey::kGsCopy) {
        // copy VS over the GS output: instance = GS invocation p, vertex = corner of triangle j of its strip.
        // v0 = dword offset of strip vertex e in the GSVS ring (component 0), as the swizzled GS stores put it.
        const uint32_t is = key_.gs_params[3] & 0xFF, es = (key_.gs_params[3] >> 8) & 0xFF;
        g += "    uint gk = uint(gl_VertexIndex), gp = uint(gl_InstanceIndex);\n";
        g += "    uint gj = gk / 3u, gc = gk % 3u, ge = gj + gc;\n";
        g += "    if ((gj & 1u) != 0u && gc < 2u) ge = gj + 1u - gc;\n";
        g += "    uint ginfo = ax[1u + gp], gcnt = ginfo & 0xFFu, gcut = ginfo >> 8u;\n";
        g += "    bool gs_kill = !(gj + 2u < gcnt && ((gcut >> (gj + 1u)) & 3u) == 0u);\n";
        g += "    uint gtid = gp % 64u;\n";
        g += Fmt("    v[0] = ((gp / 64u) * %uu + (gtid / %uu) * %uu + ge * %uu + (gtid %% %uu) * %uu) / 4u;\n",
                 key_.gs_params[0], is, key_.gs_params[2] * is, es * is, is, es);
        g += "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n";
    } else if (vs) {
        g += "    v[0] = uint(gl_VertexIndex);\n";
        if (nv > 3) g += "    v[3] = uint(gl_InstanceIndex);\n";
        g += "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n";
    } else {
        // input VGPRs laid out per SPI_PS_INPUT_ADDR
        static const uint32_t counts[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        uint32_t vi = 0;
        for (uint32_t b = 0; b < 16; ++b) {
            if (!(key_.ps_input_addr & (1u << b))) continue;
            std::string e;
            switch (b) {
            case 8: e = "U(gl_FragCoord.x)"; break;
            case 9: e = "U(gl_FragCoord.y)"; break;
            case 10: e = "U(gl_FragCoord.z)"; break;
            case 11: e = "U(gl_FragCoord.w)"; break;
            case 12: e = "(gl_FrontFacing ? 0x3F800000u : 0u)"; break;
            case 14: e = "1u"; break; // sample coverage (single-sampled)
            case 15: e = "(uint(gl_FragCoord.x) | (uint(gl_FragCoord.y) << 16u))"; break;
            default: break;
            }
            for (uint32_t k = 0; k < counts[b]; ++k, ++vi)
                if (!e.empty() && vi < nv) g += Fmt("    v[%u] = %s;\n", vi, e.c_str());
        }
    }
    g += body_;
    if (gsm == ShaderKey::kGsGeometry) g += "    if (gid < ax[0]) ax[1u + gid] = gs_cnt | (gs_cut << 8u);\n";
    g += "}\n";
    return g;
}

// ---------------------------------------------------------------------------------------------------------
// Scalar state tracking shared by Translate (versioning) and ScalarPreExecute (values)

// Registers written by a scalar-producing instruction: [first, first + count).
bool ScalarWrites(const Instr& in, uint16_t& first, uint16_t& count) {
    switch (in.fmt) {
    case Fmt::SOP2:
    case Fmt::SOP1:
    case Fmt::SOPK:
        if (in.fmt == Fmt::SOPK && in.op >= 3 && in.op <= 14) return false; // compares
        if (in.fmt == Fmt::SOP1 && (in.op == 32 || in.op == 33)) return false;
        first = in.dst;
        count = Is64BitSalu(in) ? 2 : 1;
        if (in.fmt == Fmt::SOP1 && in.op >= 36 && in.op <= 43) count = 2; // saveexec (exec handled separately)
        return true;
    case Fmt::SMRD:
        if (!IsSmrdLoad(in) && in.op != 30) return false;
        first = in.dst;
        count = in.op == 30 ? 2 : uint16_t(SmrdDwords(in));
        return true;
    case Fmt::VOPC:
        first = kVccLo;
        count = 2;
        return true;
    case Fmt::VOP3:
        if (in.op < 0x100) { first = in.dst; count = 2; return true; }
        if (in.sdst3) { first = in.sdst3; count = 2; return true; }
        if (in.op == 0x182 || in.op == 0x101) { first = uint16_t(in.dst - kVgprBase); count = 1; return true; }
        return false;
    case Fmt::VOP1:
        if (in.op == 2) { first = uint16_t(in.dst - kVgprBase); count = 1; return true; }
        return false;
    case Fmt::VOP2:
        if (in.op >= 37 && in.op <= 42) { first = kVccLo; count = 2; return true; }
        return false;
    default: return false;
    }
}

// Copies guest memory, tolerating faults (returns zeros on an unmapped page).
bool SafeRead(uint64_t addr, void* dst, size_t n) {
    if (addr < 0x10000) { std::memset(dst, 0, n); return false; }
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(dst, 0, n);
        return false;
    }
}

uint32_t SrcValue(const uint32_t* S, bool scc, uint16_t c, uint32_t lit) {
    if (c < kSArray) return S[c];
    if (c >= 128 && c <= 192) return c - 128;
    if (c >= 193 && c <= 208) return uint32_t(-(int32_t(c) - 192));
    if (c >= 240 && c <= 247) return InlineFloatBits(c);
    if (c == kScc) return scc ? 1 : 0;
    if (c == kLiteral) return lit;
    return 0;
}
uint32_t SrcValueHi(const uint32_t* S, uint16_t c) {
    if (c < kSArray) return S[c + 1];
    if (c >= 193 && c <= 208) return 0xFFFFFFFF;
    return 0;
}

// Host evaluation of the scalar ALU subset the pre-executor needs.
void ExecSalu(const Instr& in, uint32_t* S, bool& scc, uint64_t code_addr) {
    const uint32_t a = SrcValue(S, scc, in.src0, in.literal), b = SrcValue(S, scc, in.src1, in.literal);
    const uint32_t ah = SrcValueHi(S, in.src0), bh = SrcValueHi(S, in.src1);
    const uint16_t d = in.dst;
    auto w = [&](uint16_t r, uint32_t v) { if (r < kSArray) S[r] = v; };
    if (in.fmt == Fmt::SOP2) {
        uint32_t r = 0, rh = 0;
        bool is64 = false;
        switch (in.op) {
        case 0: r = a + b; scc = r < a; break;
        case 1: r = a - b; scc = b > a; break;
        case 2: r = uint32_t(int32_t(a) + int32_t(b)); break;
        case 3: r = uint32_t(int32_t(a) - int32_t(b)); break;
        case 4: r = a + b + (scc ? 1 : 0); break;
        case 5: r = a - b - (scc ? 1 : 0); break;
        case 6: r = uint32_t(std::min(int32_t(a), int32_t(b))); scc = int32_t(a) < int32_t(b); break;
        case 7: r = std::min(a, b); scc = a < b; break;
        case 8: r = uint32_t(std::max(int32_t(a), int32_t(b))); scc = int32_t(a) > int32_t(b); break;
        case 9: r = std::max(a, b); scc = a > b; break;
        case 10: r = scc ? a : b; break;
        case 11: r = scc ? a : b; rh = scc ? ah : bh; is64 = true; break;
        case 14: r = a & b; scc = r; break;
        case 15: r = a & b; rh = ah & bh; is64 = true; scc = r | rh; break;
        case 16: r = a | b; scc = r; break;
        case 17: r = a | b; rh = ah | bh; is64 = true; scc = r | rh; break;
        case 18: r = a ^ b; scc = r; break;
        case 19: r = a ^ b; rh = ah ^ bh; is64 = true; scc = r | rh; break;
        case 20: r = a & ~b; scc = r; break;
        case 21: r = a & ~b; rh = ah & ~bh; is64 = true; scc = r | rh; break;
        case 22: r = a | ~b; scc = r; break;
        case 23: r = a | ~b; rh = ah | ~bh; is64 = true; scc = r | rh; break;
        case 30: r = a << (b & 31); scc = r; break;
        case 31: {
            const uint64_t v = (uint64_t(ah) << 32 | a) << (b & 63);
            r = uint32_t(v); rh = uint32_t(v >> 32); is64 = true; scc = v != 0;
            break;
        }
        case 32: r = a >> (b & 31); scc = r; break;
        case 33: {
            const uint64_t v = (uint64_t(ah) << 32 | a) >> (b & 63);
            r = uint32_t(v); rh = uint32_t(v >> 32); is64 = true; scc = v != 0;
            break;
        }
        case 34: r = uint32_t(int32_t(a) >> (b & 31)); scc = r; break;
        case 36: r = ((1u << (a & 31)) - 1) << (b & 31); break;
        case 38: r = uint32_t(int32_t(a) * int32_t(b)); break;
        case 39: case 40: {
            const uint32_t o = b & 31, wd = std::min((b >> 16) & 0x7F, 32 - o);
            if (wd == 0) r = 0;
            else if (in.op == 39) r = (a >> o) & (wd == 32 ? 0xFFFFFFFFu : ((1u << wd) - 1));
            else r = uint32_t((int32_t(a << (32 - o - wd))) >> (32 - wd));
            scc = r;
            break;
        }
        default: return;
        }
        w(d, r);
        if (is64) w(uint16_t(d + 1), rh);
    } else if (in.fmt == Fmt::SOPK) {
        const int32_t k = in.simm16;
        switch (in.op) {
        case 0: w(d, uint32_t(k)); break;
        case 2: if (scc) w(d, uint32_t(k)); break;
        case 15: if (d < kSArray) S[d] += uint32_t(k); break;
        case 16: if (d < kSArray) S[d] = uint32_t(int32_t(S[d]) * k); break;
        default: break;
        }
    } else if (in.fmt == Fmt::SOP1) {
        switch (in.op) {
        case 3: w(d, a); break;
        case 4: w(d, a); w(uint16_t(d + 1), ah); break;
        case 5: if (scc) w(d, a); break;
        case 6: if (scc) { w(d, a); w(uint16_t(d + 1), ah); } break;
        case 7: w(d, ~a); break;
        case 8: w(d, ~a); w(uint16_t(d + 1), ~ah); break;
        case 9: w(d, a); break;
        case 10: w(d, a); w(uint16_t(d + 1), ah); break;
        case 31: {
            const uint64_t pc = code_addr + in.pc + 4;
            w(d, uint32_t(pc)); w(uint16_t(d + 1), uint32_t(pc >> 32));
            break;
        }
        default:
            if (in.op >= 36 && in.op <= 43) { w(d, 1); w(uint16_t(d + 1), 0); } // saveexec: old exec
            break;
        }
    } else if (in.fmt == Fmt::SOPC) {
        switch (in.op) {
        case 0: scc = int32_t(a) == int32_t(b); break;
        case 1: scc = a != b; break;
        case 2: scc = int32_t(a) > int32_t(b); break;
        case 3: scc = int32_t(a) >= int32_t(b); break;
        case 4: scc = int32_t(a) < int32_t(b); break;
        case 5: scc = int32_t(a) <= int32_t(b); break;
        case 6: scc = a == b; break;
        case 7: scc = a != b; break;
        case 8: scc = a > b; break;
        case 9: scc = a >= b; break;
        case 10: scc = a < b; break;
        case 11: scc = a <= b; break;
        default: break;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------

std::unique_ptr<TranslatedShader> Translate(const ShaderKey& key, const uint32_t* code, const uint32_t* fetch_code) {
    auto t = std::make_unique<TranslatedShader>();
    t->key = key;
    std::vector<Instr> main = Decode(code, 64 * 1024);
    if (main.empty() || !(main.back().fmt == Fmt::SOPP && main.back().op == 1)) {
        t->error = "program does not end with s_endpgm";
        return t;
    }
    for (const auto& in : main) {
        if (in.fmt == Fmt::SOP1 && in.op == 33) { // s_swappc_b64: inline the fetch subroutine
            if (!fetch_code) { t->error = "s_swappc_b64 without a fetch shader"; return t; }
            std::vector<Instr> fs = Decode(fetch_code, 4096);
            for (auto& f : fs) {
                if (f.fmt == Fmt::SOP1 && f.op == 32) break; // s_setpc_b64
                f.pc += kFetchPcBase;
                t->program.push_back(f);
            }
            continue;
        }
        if (in.fmt == Fmt::Unknown) {
            t->error = Fmt("unknown instruction at pc 0x%x (0x%08x)", in.pc, in.w0);
            return t;
        }
        t->program.push_back(in);
    }

    // Resource discovery with SGPR versioning: two instructions share a binding when they read the same
    // descriptor registers with no intervening write.
    uint32_t ver[kSArray] = {};
    uint32_t next_ver = 1;
    std::map<std::string, int> res_by_key;
    std::vector<ResInfo> res(t->program.size());
    uint32_t sdata = kSgprCount;
    uint32_t max_v = 0;
    bool v0_clobbered = false;
    auto note_v = [&](uint32_t v, uint32_t n) { if (v + n > max_v) max_v = v + n; };
    auto get_res = [&](ResourceSlot::Kind kind, uint16_t sgpr, uint32_t n, uint32_t instr) {
        std::string k = Fmt("%d:%u", int(kind), sgpr);
        for (uint32_t i = 0; i < n && sgpr + i < kSArray; ++i) k += Fmt(":%u", ver[sgpr + i]);
        auto it = res_by_key.find(k);
        if (it != res_by_key.end()) return it->second;
        ResourceSlot rs{};
        rs.kind = kind;
        rs.instr = instr;
        rs.sgpr = sgpr;
        rs.binding = kFirstResourceBinding + uint32_t(t->resources.size());
        t->resources.push_back(rs);
        const int idx = int(t->resources.size() - 1);
        res_by_key[k] = idx;
        return idx;
    };
    Generator gen(key, *t, res);
    for (size_t i = 0; i < t->program.size(); ++i) {
        const Instr& in = t->program[i];
        // VGPR usage
        for (uint16_t c : {in.src0, in.src1, in.src2})
            if (c >= kVgprBase && IsValu(in)) note_v(c - kVgprBase, 1);
        if ((IsValu(in) || in.fmt == Fmt::VINTRP || in.fmt == Fmt::DS) && in.dst >= kVgprBase) note_v(in.dst - kVgprBase, 1);
        if (in.fmt == Fmt::MIMG || in.fmt == Fmt::MUBUF || in.fmt == Fmt::MTBUF || in.fmt == Fmt::DS) {
            note_v(in.vaddr, 12);
            note_v(in.vdata, 4);
        }
        if (in.fmt == Fmt::VINTRP) note_v(in.src0 - kVgprBase, 1);
        if (in.fmt == Fmt::EXP)
            for (uint16_t v : in.exp_src) note_v(v, 1);
        // the vertex index (v0) stays usable for bounding vertex-buffer copies until something overwrites it
        // (the check is in program order, which covers the straight-line fetch code it is meant for)
        const bool v0_written = v0_clobbered;
        if ((IsValu(in) || in.fmt == Fmt::VINTRP || in.fmt == Fmt::DS) && in.dst == kVgprBase) v0_clobbered = true;
        if ((in.fmt == Fmt::MIMG || in.fmt == Fmt::MUBUF || in.fmt == Fmt::MTBUF || in.fmt == Fmt::DS) && in.vdata == 0)
            v0_clobbered = true;

        if (in.fmt == Fmt::MIMG) {
            const bool store = in.op == 0x08 || in.op == 0x09;
            res[i].tex = get_res(store ? ResourceSlot::kStorageImage : ResourceSlot::kTexture, in.srsrc, 8, uint32_t(i));
            if (MimgNeedsSampler(in.op)) res[i].samp = get_res(ResourceSlot::kSampler, in.ssamp, 4, uint32_t(i));
        } else if (in.fmt == Fmt::MUBUF || in.fmt == Fmt::MTBUF) {
            const bool fmt_load = in.op <= 3;
            const bool store = (in.op >= 4 && in.op <= 7) || (in.fmt == Fmt::MUBUF && in.op >= 24 && in.op <= 31);
            res[i].buf = get_res(fmt_load ? ResourceSlot::kVertexBuffer : ResourceSlot::kRawBuffer, in.srsrc, 4, uint32_t(i));
            if (store) t->resources[res[i].buf].written = true;
            if (!in.idxen || in.offen || in.vaddr != 0 || v0_written) t->resources[res[i].buf].vertex_indexed = false;
        } else if (IsSmrdLoad(in)) {
            SmrdSlot sl{uint32_t(i), SmrdDwords(in), sdata};
            t->smrd.push_back(sl);
            gen.SetSmrdIndex(i, sdata);
            sdata += sl.dwords;
        }
        uint16_t first = 0, count = 0;
        if (ScalarWrites(in, first, count))
            for (uint16_t r = first; r < first + count && r < kSArray; ++r) ver[r] = next_ver++;
        if (in.fmt == Fmt::SOP1 && in.op >= 36 && in.op <= 43) { ver[126] = next_ver++; ver[127] = next_ver++; }
    }
    if (t->resources.size() > 64) { t->error = "too many resources"; return t; }
    t->sdata_dwords = sdata;
    t->num_vgprs = std::min<uint32_t>(std::max<uint32_t>(max_v + 4, 24), 256);

    t->glsl = gen.Run();
    std::string log;
    if (!CompileGlsl(t->glsl, key.stage, t->spirv, log)) t->error = "glslang: " + log;
    return t;
}

void ScalarPreExecute(const TranslatedShader& s, const uint32_t* user_sgprs, std::vector<uint32_t>& sdata,
                      ResourceDescriptors& out) {
    uint32_t S0[kSArray] = {};
    const uint32_t nuser = std::min<uint32_t>(s.key.user_sgprs, 16);
    for (uint32_t i = 0; i < nuser; ++i) S0[i] = user_sgprs[i];
    S0[126] = 1;
    sdata.assign(s.sdata_dwords, 0);
    std::memcpy(sdata.data(), S0, kSgprCount * 4);
    out.values.assign(s.resources.size(), {});

    const size_t n = s.program.size();
    std::vector<int> smrd_of(n, -1);
    for (size_t k = 0; k < s.smrd.size(); ++k)
        if (s.smrd[k].instr < n) smrd_of[s.smrd[k].instr] = int(k);
    std::vector<std::vector<size_t>> res_at(n);
    for (size_t r = 0; r < s.resources.size(); ++r)
        if (s.resources[r].instr < n) res_at[s.resources[r].instr].push_back(r);

    // One scalar instruction. Resources are captured with the SGPR values seen when the instruction
    // that uses them is reached.
    auto step = [&](size_t i, uint32_t* S, bool& scc) {
        const Instr& in = s.program[i];
        for (size_t r : res_at[i]) {
            const uint16_t g = s.resources[r].sgpr;
            for (uint32_t k = 0; k < 8 && g + k < kSArray; ++k) out.values[r][k] = S[g + k];
        }
        switch (in.fmt) {
        case Fmt::SOP2:
        case Fmt::SOP1:
        case Fmt::SOPK:
        case Fmt::SOPC: ExecSalu(in, S, scc, s.key.code); break;
        case Fmt::SMRD: {
            if (smrd_of[i] >= 0) {
                const SmrdSlot& sl = s.smrd[size_t(smrd_of[i])];
                const uint32_t off = in.smrd_imm ? in.smrd_offset * 4 : (in.smrd_offset < kSArray ? S[in.smrd_offset] : 0);
                uint64_t base;
                if (in.op >= 8) base = uint64_t(S[in.sbase]) | (uint64_t(S[in.sbase + 1] & 0xFFFF) << 32);
                else base = (uint64_t(S[in.sbase]) | (uint64_t(S[in.sbase + 1] & 0xFFFF) << 32)) & ~3ull; // 48-bit address: a V# used as a pointer keeps its stride in the high bits
                uint32_t* dst = sdata.data() + sl.sdata_index;
                SafeRead(base + off, dst, sl.dwords * 4);
                for (uint32_t k = 0; k < sl.dwords && in.dst + k < kSArray; ++k) S[in.dst + k] = dst[k];
            } else if (in.op == 30) {
                S[in.dst] = 0;
                if (in.dst + 1 < kSArray) S[in.dst + 1] = 0;
            }
            break;
        }
        default: break;
        }
    };

    // Pass 1, program order: gives every SMRD and resource a value, including code that pass 2
    // does not reach (vector-dependent branches).
    {
        uint32_t S[kSArray];
        std::memcpy(S, S0, sizeof S);
        bool scc = false;
        for (size_t i = 0; i < n; ++i) step(i, S, scc);
    }
    // Pass 2, following the scalar control flow: SCC branches are decided with the known values,
    // VCCZ/EXECZ branches are taken as falling through. The values seen on this path replace pass 1
    // (a register reused for another purpose on a skipped path must not leak into a resource).
    {
        std::map<uint32_t, size_t> index_of;
        for (size_t i = 0; i < n; ++i) index_of[s.program[i].pc] = i;
        uint32_t S[kSArray];
        std::memcpy(S, S0, sizeof S);
        bool scc = false;
        size_t i = 0;
        for (uint32_t steps = 0; i < n && steps < 200000; ++steps) {
            const Instr& in = s.program[i];
            if (in.fmt == Fmt::SOPP) {
                if (in.op == 1) break; // s_endpgm
                if (IsBranch(in)) {
                    const bool taken = in.op == 2 || (in.op == 4 && !scc) || (in.op == 5 && scc);
                    if (taken) {
                        auto it = index_of.find(BranchTarget(in));
                        if (it == index_of.end()) break;
                        i = it->second;
                        continue;
                    }
                }
                ++i;
                continue;
            }
            step(i, S, scc);
            ++i;
        }
    }
}

bool CompileGlsl(const std::string& glsl, Stage stage, std::vector<uint32_t>& spirv, std::string& log) {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
    const EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : stage == Stage::Compute ? EShLangCompute : EShLangFragment;
    glslang::TShader shader(lang);
    const char* src = glsl.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    const auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, false, msgs)) {
        log = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        log = program.getInfoLog();
        return false;
    }
    spirv.clear();
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &opt);
    return !spirv.empty();
}

} // namespace gr::gpu
