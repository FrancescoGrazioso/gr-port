// Sampling profiler: a host thread suspends the registered threads about 1000 times per second, unwinds
// their stacks with the x64 unwind tables (RtlVirtualUnwind; guest code has none, so a walk stops
// there) and keeps the return addresses. Symbols are resolved at exit with DbgHelp.
#include "profiler.h"

#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gr {
namespace {

constexpr int kDepth = 32;

struct Profiled {
    std::string name;
    HANDLE thread = nullptr;
    std::vector<uint64_t> frames; // kDepth addresses per sample (0 = end)
    uint64_t samples = 0;
};

std::mutex g_m;
std::vector<Profiled*> g_threads;
std::once_flag g_started;

bool Wanted(const char* name) {
    static const char* env = std::getenv("GR_PROFILE");
    if (!env || !*env) return false;
    if (std::strcmp(env, "1") == 0) return true;
    const std::string list = std::string(",") + env + ",";
    return list.find(std::string(",") + name + ",") != std::string::npos;
}

void Sample(Profiled& p) {
    if (SuspendThread(p.thread) == DWORD(-1)) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    uint64_t pcs[kDepth] = {};
    if (GetThreadContext(p.thread, &ctx)) {
        for (int d = 0; d < kDepth && ctx.Rip; ++d) {
            pcs[d] = ctx.Rip;
            DWORD64 image = 0;
            RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
            if (!fn) break; // guest code or a leaf without unwind data: stop here
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
        }
    }
    ResumeThread(p.thread);
    p.frames.insert(p.frames.end(), pcs, pcs + kDepth);
    p.samples++;
}

void SamplerLoop() {
    SetThreadDescription(GetCurrentThread(), L"gr-profiler");
    timeBeginPeriod(1);
    const char* from = std::getenv("GR_PROFILE_FROM"); // seconds after start
    if (from) Sleep(DWORD(std::atof(from) * 1000));
    for (;;) {
        Sleep(1);
        std::lock_guard<std::mutex> lk(g_m);
        for (Profiled* p : g_threads) Sample(*p);
    }
}

std::string Symbol(uint64_t pc) {
    HANDLE proc = GetCurrentProcess();
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 511;
    DWORD64 disp = 0;
    HMODULE mod = nullptr;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &mod)) {
        GetModuleFileNameA(mod, modname, MAX_PATH);
        const char* s = std::strrchr(modname, '\\');
        if (s) std::memmove(modname, s + 1, std::strlen(s));
    } else {
        return "[guest code]";
    }
    if (SymFromAddr(proc, pc, &disp, si)) return std::string(modname) + "!" + si->Name;
    return std::string(modname) + "!?";
}

} // namespace

void ProfileThisThread(const char* name) {
    if (!Wanted(name)) return;
    auto* p = new Profiled;
    p->name = name;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &p->thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_threads.push_back(p);
    }
    std::call_once(g_started, [] { std::thread(SamplerLoop).detach(); });
}

std::string ProfileReport() {
    std::lock_guard<std::mutex> lk(g_m); // also stops the sampler
    if (g_threads.empty()) return {};
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::unordered_map<uint64_t, std::string> names;
    auto name_of = [&](uint64_t pc) -> const std::string& {
        auto it = names.find(pc);
        if (it == names.end()) it = names.emplace(pc, Symbol(pc)).first;
        return it->second;
    };
    std::string out;
    char line[640];
    for (Profiled* p : g_threads) {
        std::map<std::string, uint64_t> self, incl;
        for (uint64_t s = 0; s < p->samples; ++s) {
            const uint64_t* f = &p->frames[s * kDepth];
            if (!f[0]) continue;
            self[name_of(f[0])]++;
            std::vector<const std::string*> seen;
            for (int d = 0; d < kDepth && f[d]; ++d) {
                const std::string& n = name_of(f[d]);
                if (std::find(seen.begin(), seen.end(), &n) != seen.end()) continue;
                seen.push_back(&n);
                incl[n]++;
            }
        }
        auto top = [&](const std::map<std::string, uint64_t>& m, const char* title, size_t n) {
            std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::snprintf(line, sizeof line, "\n  %s:", title);
            out += line;
            for (size_t k = 0; k < v.size() && k < n; ++k) {
                std::snprintf(line, sizeof line, "\n    %5.1f%%  %s", 100.0 * double(v[k].second) / double(p->samples),
                              v[k].first.c_str());
                out += line;
            }
        };
        std::snprintf(line, sizeof line, "\nprofile of thread %s: %llu samples", p->name.c_str(),
                      (unsigned long long)p->samples);
        out += line;
        top(self, "self", 40);
        top(incl, "inclusive", 50);
    }
    return out;
}

} // namespace gr
