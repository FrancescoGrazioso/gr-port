// Debug: hit counters on guest code addresses (GR_BP=<hex offset>[,<hex offset>...], offsets from
// the eboot base). Each address gets an int3; the exception handler counts the hit, puts the
// original byte back, single-steps the instruction and re-arms the breakpoint. Counts are written
// to the trace every 5 seconds. Hits by other threads while a breakpoint is disarmed are missed,
// so the numbers are lower bounds; good enough to see which code runs and which does not.
#include "guest_bp.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"

namespace gr {
namespace {

struct Bp {
    uint64_t addr = 0;
    uint8_t orig = 0;
    std::atomic<uint64_t> hits{0};
    // optional: "<offset>:<reg>:<hex delta>" captures a register at the first hit and dumps 8 floats
    // at reg + delta in each report
    int reg = -1; // 0 rdi 1 rsi 2 rdx 3 rcx 4 r8 5 r9 6 rax 7 rbx
    uint64_t delta = 0;
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> more[3] = {}; // further distinct values (cull dump)
};
void SafeRead32(uint64_t addr, float* out) { // 8 floats
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), 32);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
bool SafeCopyMem(void* dst, uint64_t src, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void DumpCullList(uint64_t ctx) {
    uint64_t items = 0;
    uint32_t n = 0;
    if (!SafeCopyMem(&items, ctx + 0x2a68, 8) || !SafeCopyMem(&n, ctx + 0x2a78, 4)) return;
    uint32_t vis = 0, valid = 0;
    for (uint32_t i = 0; i < n && i < 8192; ++i) {
        uint64_t it = 0;
        if (!SafeCopyMem(&it, items + i * 8ull, 8) || !it) continue;
        float bb[8] = {};
        uint8_t flags[0x10] = {};
        if (!SafeCopyMem(bb, it + 0xa0, 32) || !SafeCopyMem(flags, it + 0x1a0, 0x10)) continue;
        ++valid;
        const bool v = flags[2] != 0;
        vis += v;
        uint64_t vt = 0;
        SafeCopyMem(&vt, it, 8);
        float extra[8] = {};
        SafeCopyMem(extra, it + 0x254, 32); // lod (int) +0x254, dist2 +0x258, +0x25c opacity, ...
        if (i < 24)
            TraceNote("cull item %u @0x%llx vt 0x%llx vis %d flags %02x%02x%02x%02x center (%g %g %g) ext (%g %g %g) lod %d d2 %g op %g | %g %g %g", i,
                      (unsigned long long)it, (unsigned long long)vt, int(v), flags[4], flags[5], flags[6], flags[7], bb[0],
                      bb[1], bb[2], bb[4], bb[5], bb[6], *reinterpret_cast<int*>(&extra[0]), extra[1], extra[2], extra[3],
                      extra[4], extra[5]);
    }
    TraceNote("cull list ctx 0x%llx: %u items, %u valid, %u visible", (unsigned long long)ctx, n, valid, vis);
}
uint64_t RegValue(const CONTEXT* c, int reg) {
    switch (reg) {
    case 0: return c->Rdi;
    case 1: return c->Rsi;
    case 2: return c->Rdx;
    case 3: return c->Rcx;
    case 4: return c->R8;
    case 5: return c->R9;
    case 6: return c->Rax;
    case 7: return c->Rbx;
    default: return 0;
    }
}
std::vector<Bp*> g_bps;
std::atomic<uint64_t> g_mat_counts[256][2];
uint64_t g_base = 0;
thread_local Bp* t_pending = nullptr;

void Patch(uint64_t addr, uint8_t byte) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile uint8_t*>(addr) = byte;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 1);
}

struct Watch {
    uint64_t lo, hi;
    std::string label;
    bool hit = false;
};
SRWLOCK g_watch_lock = SRWLOCK_INIT;
std::vector<Watch> g_watches;

LONG CALLBACK BpHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const uint64_t fault = ep->ExceptionRecord->ExceptionInformation[1];
        AcquireSRWLockExclusive(&g_watch_lock);
        for (Watch& w : g_watches) {
            if (fault < w.lo || fault >= w.hi || w.hit) continue;
            w.hit = true;
            DWORD old;
            VirtualProtect(reinterpret_cast<void*>(w.lo), w.hi - w.lo, PAGE_READWRITE, &old);
            ReleaseSRWLockExclusive(&g_watch_lock);
            TraceNote("watch: %s access to %s at 0x%llx (+0x%llx) from rip 0x%llx (eboot+0x%llx)",
                      ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", w.label.c_str(),
                      (unsigned long long)fault, (unsigned long long)(fault - w.lo),
                      (unsigned long long)ep->ContextRecord->Rip, (unsigned long long)(ep->ContextRecord->Rip - g_base));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        ReleaseSRWLockExclusive(&g_watch_lock);
    }
    if (code == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
        for (Bp* b : g_bps) {
            if (b->addr != at) continue;
            b->hits++;
            // GR_DRAWSKIP: at eboot+0x142c0 (indexed draw helper, rdi = gfx context) count draws skipped
            // because a compute shader is bound (gfx+0xb98) or no shader (gfx+0xb88)
            static const bool dskip = std::getenv("GR_DRAWSKIP") != nullptr;
            if (dskip && at - g_base == 0x142c0) {
                uint64_t cs = 0, sh = 0;
                SafeCopyMem(&cs, ep->ContextRecord->Rdi + 0xb98, 8);
                SafeCopyMem(&sh, ep->ContextRecord->Rdi + 0xb88, 8);
                g_mat_counts[cs ? 1 : sh ? 0 : 2][0]++;
                if (cs) {
                    static std::atomic<int> logged{0};
                    if (logged++ < 3) b->captured = cs; // the compute shader object left bound
                }
            }
            // GR_Q1C: at eboot+0xed2f10 (queue 0x1c executor; rsi = &first key ptr, rdx = end) histogram
            // of the key subtypes (bits 16-20)
            static const bool q1c = std::getenv("GR_Q1C") != nullptr;
            if (q1c && at - g_base == 0xed2f10) {
                uint64_t first = 0;
                const uint64_t end = ep->ContextRecord->Rdx;
                if (SafeCopyMem(&first, ep->ContextRecord->Rsi, 8))
                    for (uint64_t p = first; p < end && p < first + 8 * 4096; p += 8) {
                        uint32_t key = 0;
                        if (!SafeCopyMem(&key, p, 4)) break;
                        if ((key >> 24) != 0x1c) break;
                        g_mat_counts[(key >> 16) & 0x1f][0]++;
                    }
            }
            // GR_MATTYPES: at eboot+0x269630 (main-pass submit) count material type / path taken
            static const bool mat = std::getenv("GR_MATTYPES") != nullptr;
            if (mat && at - g_base == 0x269630) {
                const uint64_t p2 = ep->ContextRecord->Rsi;
                uint64_t m = 0, l1 = 0;
                uint8_t type = 0, f1 = 0, f2 = 0;
                if (SafeCopyMem(&m, p2 + 0x10, 8) && SafeCopyMem(&l1, m + 0x20, 8) && SafeCopyMem(&type, l1 + 0x12, 1) &&
                    SafeCopyMem(&f1, p2 + 100, 1) && SafeCopyMem(&f2, l1 + 0x18, 1)) {
                    uint64_t skin = 0;
                    SafeCopyMem(&skin, p2 + 0x40, 8);
                    g_mat_counts[type][skin ? 1 : 0]++;
                }
            }
            if (b->reg >= 0) {
                const uint64_t v = RegValue(ep->ContextRecord, b->reg);
                if (!b->captured) b->captured = v;
                else if (v != b->captured)
                    for (auto& m : b->more) {
                        if (m == v) break;
                        if (!m) { m = v; break; }
                    }
            }
            Patch(at, b->orig);
            ep->ContextRecord->Rip = at;
            ep->ContextRecord->EFlags |= 0x100; // trap after the original instruction
            t_pending = b;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else if (code == EXCEPTION_SINGLE_STEP && t_pending) {
        Patch(t_pending->addr, 0xCC);
        t_pending = nullptr;
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void WatchGuestRange(uint64_t addr, uint64_t size, const char* label) {
    static const bool enabled = std::getenv("GR_WATCH_RTS") != nullptr || std::getenv("GR_WATCH") != nullptr;
    if (!enabled || !size) return;
    const uint64_t lo = addr & ~0xFFFull, hi = (addr + size + 0xFFF) & ~0xFFFull;
    AcquireSRWLockExclusive(&g_watch_lock);
    if (g_watches.size() < 256) {
        g_watches.push_back({lo, hi, label, false});
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(lo), hi - lo, PAGE_NOACCESS, &old);
    }
    ReleaseSRWLockExclusive(&g_watch_lock);
    static bool handler = false;
    if (!handler) {
        handler = true;
        g_base = 0x800000000ull;
        AddVectoredExceptionHandler(1, BpHandler);
    }
}

void InstallGuestBreakpoints(uint64_t eboot_base) {
    const char* env = std::getenv("GR_BP");
    if (!env || !*env) return;
    g_base = eboot_base;
    std::string s = env;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        const uint64_t off = std::strtoull(item.c_str(), nullptr, 16);
        if (off) {
            Bp* b = new Bp;
            b->addr = eboot_base + off;
            const size_t c1 = item.find(':');
            if (c1 != std::string::npos) {
                static const char* names[8] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9", "rax", "rbx"};
                const std::string rn = item.substr(c1 + 1, item.find(':', c1 + 1) - c1 - 1);
                for (int k = 0; k < 8; ++k)
                    if (rn == names[k]) b->reg = k;
                const size_t c2 = item.find(':', c1 + 1);
                if (c2 != std::string::npos) b->delta = std::strtoull(item.substr(c2 + 1).c_str(), nullptr, 16);
            }
            b->orig = *reinterpret_cast<const uint8_t*>(b->addr);
            g_bps.push_back(b);
        }
        pos = end + 1;
    }
    AddVectoredExceptionHandler(1, BpHandler); // ahead of the crash handler
    for (Bp* b : g_bps) Patch(b->addr, 0xCC);
    std::thread([] {
        for (int t = 5;; t += 5) {
            Sleep(5000);
            std::string line = "bp t=" + std::to_string(t) + "s:";
            for (Bp* b : g_bps) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " %llx=%llu", (unsigned long long)(b->addr - g_base),
                              (unsigned long long)b->hits.load());
                line += buf;
                if (const uint64_t p = b->captured.load()) {
                    float f[8] = {};
                    SafeRead32(p + b->delta, f);
                    char fb[256];
                    std::snprintf(fb, sizeof fb, " [0x%llx+0x%llx: %g %g %g %g %g %g %g %g]", (unsigned long long)p,
                                  (unsigned long long)b->delta, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                    line += fb;
                }
            }
            TraceNote("%s", line.c_str());
            // GR_PEEK=<hex offset>[,...]: floats/ints at eboot globals
            static const char* peek = std::getenv("GR_PEEK");
            if (peek) {
                std::string p = peek, pl = "peek:";
                size_t q = 0;
                while (q < p.size()) {
                    size_t e = p.find(',', q);
                    if (e == std::string::npos) e = p.size();
                    const uint64_t off = std::strtoull(p.substr(q, e - q).c_str(), nullptr, 16);
                    uint32_t v = 0;
                    SafeCopyMem(&v, g_base + off, 4);
                    float f;
                    std::memcpy(&f, &v, 4);
                    char pb[96];
                    std::snprintf(pb, sizeof pb, " %llx=%g(0x%08x)", (unsigned long long)off, f, v);
                    pl += pb;
                    q = e + 1;
                }
                TraceNote("%s", pl.c_str());
            }
            if ((std::getenv("GR_MATTYPES") || std::getenv("GR_Q1C") || std::getenv("GR_DRAWSKIP")) && (t % 10) == 0) {
                std::string ml = "mattypes:";
                for (int k = 0; k < 256; ++k)
                    if (g_mat_counts[k][0] || g_mat_counts[k][1]) {
                        char mb[64];
                        std::snprintf(mb, sizeof mb, " 0x%02x=%llu/%llu", k, (unsigned long long)g_mat_counts[k][1].load(),
                                      (unsigned long long)g_mat_counts[k][0].load());
                        ml += mb;
                    }
                TraceNote("%s (skinned/rigid)", ml.c_str());
            }
            // GR_WATCH=<hex addr>:<hex size>[@<seconds>]: watch a range from that time on (default 60 s)
            static const char* watch = std::getenv("GR_WATCH");
            if (watch) {
                const std::string w = watch;
                const size_t c = w.find(':'), at = w.find('@');
                const int when = at == std::string::npos ? 60 : std::atoi(w.c_str() + at + 1);
                if (t == when && c != std::string::npos)
                    WatchGuestRange(std::strtoull(w.c_str(), nullptr, 16), std::strtoull(w.c_str() + c + 1, nullptr, 16), "watched range");
            }
            // GR_PEEK64=<hex offset>:<count>[,...]: 64-bit values at eboot globals (once, at t=30s)
            static const char* peek64 = std::getenv("GR_PEEK64");
            if (peek64 && t == 30) {
                std::string p = peek64;
                size_t q = 0;
                while (q < p.size()) {
                    size_t e = p.find(',', q);
                    if (e == std::string::npos) e = p.size();
                    const std::string item = p.substr(q, e - q);
                    const uint64_t off = std::strtoull(item.c_str(), nullptr, 16);
                    const size_t c = item.find(':');
                    const uint32_t cnt = c == std::string::npos ? 1 : uint32_t(std::strtoul(item.c_str() + c + 1, nullptr, 10));
                    for (uint32_t k = 0; k < cnt && k < 256; ++k) {
                        uint64_t v = 0;
                        SafeCopyMem(&v, g_base + off + k * 8ull, 8);
                        TraceNote("peek64 %llx: 0x%llx%s", (unsigned long long)(off + k * 8), (unsigned long long)v,
                                  v >= g_base && v < g_base + 0x4000000 ? " (eboot)" : "");
                    }
                    q = e + 1;
                }
            }
            // GR_CULLDUMP: the breakpoint on the per-object cull job (eboot+0x26d010, rsi = cull
            // context) gives the object list: items at ctx+0x2a68, count at ctx+0x2a78. Each item has
            // its AABB at +0xa0/+0xb0 and the visible flag at +0x1a2.
            static const bool cull = std::getenv("GR_CULLDUMP") != nullptr;
            if (cull && (t % 30) == 0)
                for (Bp* b : g_bps)
                    if (b->addr - g_base == 0x26d010 && b->captured) {
                        DumpCullList(b->captured);
                        for (auto& m : b->more)
                            if (m) DumpCullList(m);
                    }
        }
    }).detach();
    TraceNote("guest breakpoints: %zu armed", g_bps.size());
}

} // namespace gr
