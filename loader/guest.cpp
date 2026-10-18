#include "guest.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <intrin.h>
#include <string>
#include <vector>

#include "linker.h"
#include "tls.h"

namespace gr {

Runtime& Rt() {
    static Runtime rt;
    return rt;
}

// ---------------------------------------------------------------------------------------------
// MS x64 -> SysV call stub, generated once:  uint64_t stub(uint64_t fn /*rcx*/, const uint64_t* args /*rdx*/)
namespace {
using CallStub = uint64_t (*)(uint64_t, const uint64_t*);

CallStub BuildCallStub() {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t* c = p;
    auto B = [&](std::initializer_list<uint8_t> b) {
        for (auto x : b) *c++ = x;
    };
    auto D32 = [&](uint32_t v) {
        std::memcpy(c, &v, 4);
        c += 4;
    };
    // modrm bytes for movups [rsp+disp32], xmmN (mod=10, rm=100/SIB)
    static constexpr uint8_t kModrm[10] = {0xB4, 0xBC, 0x84, 0x8C, 0x94, 0x9C, 0xA4, 0xAC, 0xB4, 0xBC};
    B({0x57, 0x56});                               // push rdi ; push rsi
    B({0x48, 0x81, 0xEC}); D32(0xA8);              // sub rsp, 0xA8   (rsp now 16-aligned)
    for (int i = 0; i < 10; ++i) {                 // movups [rsp+16*i], xmm(6+i)
        if (i >= 2) B({0x44});
        B({0x0F, 0x11, kModrm[i], 0x24}); D32(16 * i);
    }
    B({0x48, 0x89, 0xC8});                         // mov rax, rcx      (target)
    B({0x49, 0x89, 0xD2});                         // mov r10, rdx      (args)
    B({0x49, 0x8B, 0x3A});                         // mov rdi, [r10]
    B({0x49, 0x8B, 0x72, 0x08});                   // mov rsi, [r10+8]
    B({0x49, 0x8B, 0x52, 0x10});                   // mov rdx, [r10+16]
    B({0x49, 0x8B, 0x4A, 0x18});                   // mov rcx, [r10+24]
    B({0x4D, 0x8B, 0x42, 0x20});                   // mov r8,  [r10+32]
    B({0x4D, 0x8B, 0x4A, 0x28});                   // mov r9,  [r10+40]
    B({0xFF, 0xD0});                               // call rax
    for (int i = 0; i < 10; ++i) {                 // movups xmm(6+i), [rsp+16*i]
        if (i >= 2) B({0x44});
        B({0x0F, 0x10, kModrm[i], 0x24}); D32(16 * i);
    }
    B({0x48, 0x81, 0xC4}); D32(0xA8);              // add rsp, 0xA8
    B({0x5E, 0x5F, 0xC3});                         // pop rsi ; pop rdi ; ret
    FlushInstructionCache(GetCurrentProcess(), p, 4096);
    return reinterpret_cast<CallStub>(p);
}
} // namespace

uint64_t CallGuest(uint64_t fn, std::initializer_list<uint64_t> args) {
    static CallStub stub = BuildCallStub();
    uint64_t a[6] = {};
    size_t i = 0;
    for (uint64_t v : args)
        if (i < 6) a[i++] = v;
    return stub(fn, a);
}

// ---------------------------------------------------------------------------------------------
namespace {
struct ThreadStart {
    std::function<void()> body;
};

DWORD WINAPI GuestThreadProc(void* p) {
    auto* ts = static_cast<ThreadStart*>(p);
    if (Rt().tls) Rt().tls->SetupCurrentThread();
    ts->body();
    delete ts;
    return 0;
}
} // namespace

namespace {
struct GuestThreadRec {
    HANDLE handle;
    DWORD tid;
    std::string name;
};
std::mutex g_threads_m;
std::vector<GuestThreadRec> g_threads;
} // namespace

void* StartGuestThread(std::function<void()> body, size_t stack_size, uint32_t* thread_id, const char* name) {
    auto* ts = new ThreadStart{std::move(body)};
    DWORD tid = 0;
    // Without STACK_SIZE_PARAM_IS_A_RESERVATION the size is committed immediately.
    HANDLE h = CreateThread(nullptr, stack_size, GuestThreadProc, ts, 0, &tid);
    if (thread_id) *thread_id = tid;
    if (h) {
        HANDLE dup = nullptr;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        std::lock_guard<std::mutex> lk(g_threads_m);
        g_threads.push_back({dup, tid, name ? name : "guest"});
        if (name) SetThreadDescription(h, std::wstring(name, name + std::strlen(name)).c_str());
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// Tracing: every HLE/stub call goes into a ring buffer (dumped on crash) and, optionally, into a
// trace file for the first N calls.
namespace {
struct TraceEntry {
    uint32_t tid;
    const char* lib;
    const char* name;
    uint64_t args[4];
    uint64_t ret;
    uint64_t caller; // guest return address
    bool impl;
};
constexpr size_t kRing = 64;
TraceEntry g_ring[kRing];
std::atomic<uint64_t> g_ring_pos{0};
FILE* g_trace = nullptr;
std::atomic<uint64_t> g_trace_lines{0};
uint64_t g_trace_max = 0;
std::mutex g_trace_mutex;

std::string Symbolize(uint64_t addr);

void WriteEntry(FILE* f, const TraceEntry& e) {
    std::fprintf(f, "[%5u] %s %s::%s(%llx, %llx, %llx, %llx) -> %llx   <- %s\n", e.tid, e.impl ? "HLE " : "STUB",
                 e.lib, e.name, (unsigned long long)e.args[0], (unsigned long long)e.args[1],
                 (unsigned long long)e.args[2], (unsigned long long)e.args[3], (unsigned long long)e.ret,
                 Symbolize(e.caller).c_str());
}
} // namespace

ULONGLONG g_trace_start_ms = 0; // calls are written only after this tick count

void TraceOpen(const std::string& path, uint64_t max_lines, double start_after_s) {
    g_trace = std::fopen(path.c_str(), "w");
    g_trace_max = max_lines;
    g_trace_start_ms = GetTickCount64() + ULONGLONG(start_after_s * 1000.0);
}

void TraceCall(const char* lib, const char* name, const uint64_t* args, uint64_t ret, bool implemented) {
    TraceEntry e{GetCurrentThreadId(), lib, name, {args[0], args[1], args[2], args[3]}, ret, args[8], implemented};
    g_ring[g_ring_pos.fetch_add(1) % kRing] = e;
    if (g_trace && GetTickCount64() >= g_trace_start_ms && g_trace_lines.fetch_add(1) < g_trace_max) {
        std::lock_guard<std::mutex> lk(g_trace_mutex);
        WriteEntry(g_trace, e);
    }
}

void TraceLine(const char* text, uint64_t caller) {
    if (!g_trace || GetTickCount64() < g_trace_start_ms || g_trace_lines.fetch_add(1) >= g_trace_max) return;
    const std::string where = Symbolize(caller);
    std::lock_guard<std::mutex> lk(g_trace_mutex);
    std::fprintf(g_trace, "[%5lu] %s   <- %s\n", GetCurrentThreadId(), text, where.c_str());
}

void TraceNote(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
    if (g_trace) {
        std::lock_guard<std::mutex> lk(g_trace_mutex);
        std::fprintf(g_trace, "# %s\n", buf);
        std::fflush(g_trace);
    }
}

// ---------------------------------------------------------------------------------------------
namespace {
std::string Symbolize(uint64_t addr) {
    if (const Linker* l = Rt().linker) {
        for (const auto& m : l->Modules()) {
            const uint64_t lo = reinterpret_cast<uint64_t>(m->mapping);
            if (addr >= lo && addr < lo + m->size) {
                char b[128];
                std::snprintf(b, sizeof b, "%s+0x%llx", m->name.c_str(), (unsigned long long)(addr - m->base));
                return b;
            }
        }
    }
    return "host";
}

// Host addresses: function+offset (file:line) from the PDB via DbgHelp.
std::string SymbolizeHost(uint64_t addr) {
    static std::once_flag once;
    std::call_once(once, [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    });
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    char out[512];
    if (!SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
        std::snprintf(out, sizeof out, "host:0x%llx", (unsigned long long)addr);
        return out;
    }
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD ldisp = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &ldisp, &line)) {
        const char* file = std::strrchr(line.FileName, '\\');
        std::snprintf(out, sizeof out, "%s+0x%llx (%s:%lu)", sym->Name, (unsigned long long)disp,
                      file ? file + 1 : line.FileName, line.LineNumber);
    } else {
        std::snprintf(out, sizeof out, "%s+0x%llx", sym->Name, (unsigned long long)disp);
    }
    return out;
}

std::string SymbolizeAny(uint64_t addr) {
    std::string s = Symbolize(addr);
    return s == "host" ? SymbolizeHost(addr) : s;
}

thread_local bool t_bypass = false;
thread_local bool t_unhandled_pass = false; // set while the unhandled-exception filter reuses CrashHandler
thread_local const char* t_last_hle = nullptr; // last HLE function entered by this thread
thread_local uint64_t t_last_hle_caller = 0;

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    if (t_bypass) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT ||
                       code == EXCEPTION_DATATYPE_MISALIGNMENT;
    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
    // Faults inside other host DLLs (GPU drivers probe memory and recover through SEH) are left to their
    // own handlers; if nobody handles them the unhandled-exception filter below reports them.
    if (!t_unhandled_pass) {
        const uint64_t rip = ep->ContextRecord->Rip;
        const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const bool in_exe = rip >= base && rip < base + nt->OptionalHeader.SizeOfImage;
        if (!in_exe && Symbolize(rip) == "host") return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<bool> once{false};
    if (once.exchange(true)) TerminateProcess(GetCurrentProcess(), 0xDEAD);

    const CONTEXT* c = ep->ContextRecord;
    FILE* outs[2] = {stdout, g_trace};
    for (FILE* f : outs) {
        if (!f) continue;
        std::fprintf(f, "\n=== guest crash: code 0x%08lx at %s (rip=0x%llx) thread %lu\n", code, SymbolizeAny(c->Rip).c_str(),
                     (unsigned long long)c->Rip, GetCurrentThreadId());
        if (code == EXCEPTION_ACCESS_VIOLATION)
            std::fprintf(f, "    %s address 0x%llx\n",
                         ep->ExceptionRecord->ExceptionInformation[0] == 0   ? "read"
                         : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write"
                                                                              : "execute",
                         (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        std::fprintf(f,
                     "    rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
                     "    rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
                     "    r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n"
                     "    r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
                     c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11,
                     c->R12, c->R13, c->R14, c->R15);
        if (t_last_hle)
            std::fprintf(f, "    last HLE call on this thread: %s <- %s\n", t_last_hle, Symbolize(t_last_hle_caller).c_str());
        // Return addresses on the stack that point into guest modules (cheap backtrace).
        std::fprintf(f, "    stack scan:");
        const auto* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
        int found = 0;
        for (int i = 0; i < 512 && found < 12; ++i) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            const std::string s = Symbolize(sp[i]);
            if (s != "host") {
                std::fprintf(f, " %s", s.c_str());
                ++found;
            }
        }
        // Host frames (our own code) found on the stack, resolved through the PDB.
        std::fprintf(f, "\n    host frames:");
        {
            const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const uint64_t end = base + nt->OptionalHeader.SizeOfImage;
            int n = 0;
            for (int i = 0; i < 512 && n < 10; ++i) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                if (sp[i] >= base && sp[i] < end) {
                    std::fprintf(f, "\n      %s", SymbolizeHost(sp[i]).c_str());
                    ++n;
                }
            }
        }
        std::fprintf(f, "\n    last HLE calls (oldest first):\n");
        const uint64_t end = g_ring_pos.load();
        for (uint64_t i = end > kRing ? end - kRing : 0; i < end; ++i) {
            std::fprintf(f, "      ");
            WriteEntry(f, g_ring[i % kRing]);
        }
        std::fflush(f);
    }
    TerminateProcess(GetCurrentProcess(), 0xC0DE);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    t_unhandled_pass = true;
    t_bypass = false;
    return CrashHandler(ep);
}
} // namespace

void InstallCrashHandler() {
    AddVectoredExceptionHandler(1, CrashHandler);
    SetUnhandledExceptionFilter(UnhandledFilter);
}
void SetCrashHandlerBypass(bool on) { t_bypass = on; }

namespace {
struct CallSlot {
    std::atomic<const char*> lib{nullptr}, name{nullptr};
    std::atomic<uint64_t> caller{0}, arg0{0};
};
std::mutex g_slots_m;
std::unordered_map<DWORD, CallSlot*> g_slots;
thread_local CallSlot* t_slot = nullptr;

CallSlot* Slot() {
    if (!t_slot) {
        t_slot = new CallSlot;
        std::lock_guard<std::mutex> lk(g_slots_m);
        g_slots[GetCurrentThreadId()] = t_slot;
    }
    return t_slot;
}
} // namespace

void TraceEnter(const char* lib, const char* name, uint64_t caller, uint64_t arg0) {
    CallSlot* s = Slot();
    s->lib = lib;
    s->caller = caller;
    s->arg0 = arg0;
    s->name = name;
    t_last_hle = name;
    t_last_hle_caller = caller;
}
void TraceLeave() { Slot()->name = nullptr; }

void DumpGuestThreads() {
    std::lock_guard<std::mutex> lk(g_threads_m);
    std::lock_guard<std::mutex> lk2(g_slots_m);
    FILE* outs[2] = {stdout, g_trace};
    for (const auto& t : g_threads) {
        if (WaitForSingleObject(t.handle, 0) == WAIT_OBJECT_0) continue; // exited
        if (SuspendThread(t.handle) == DWORD(-1)) continue;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        const bool ok = GetThreadContext(t.handle, &c);
        std::string frames;
        if (ok) {
            const auto* sp = reinterpret_cast<const uint64_t*>(c.Rsp);
            int found = 0;
            for (int i = 0; i < 2048 && found < 8; ++i) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                const std::string s = Symbolize(sp[i]);
                if (s != "host") {
                    frames += " " + s;
                    ++found;
                }
            }
        }
        ResumeThread(t.handle);
        std::string in_call;
        if (auto it = g_slots.find(t.tid); it != g_slots.end() && it->second->name.load()) {
            char a0[32];
            std::snprintf(a0, sizeof a0, "(%llx)", (unsigned long long)it->second->arg0.load());
            in_call = std::string(" | in ") + it->second->lib.load() + "::" + it->second->name.load() + a0 + " <- " +
                      Symbolize(it->second->caller.load());
        }
        for (FILE* f : outs)
            if (f)
                std::fprintf(f, "  [%5lu] %-28s rip=%s%s%s\n", t.tid, t.name.c_str(),
                             ok ? Symbolize(c.Rip).c_str() : "?", in_call.c_str(), frames.c_str());
    }
    if (g_trace) std::fflush(g_trace);
}

// Return addresses into guest modules found on the current stack (debugging aid for HLE calls).
std::string GuestStackScan(int max_frames) {
    std::string out;
    const auto* sp = reinterpret_cast<const uint64_t*>(_AddressOfReturnAddress());
    int found = 0;
    for (int i = 0; i < 4096 && found < max_frames; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        const std::string s = Symbolize(sp[i]);
        if (s != "host") {
            out += ' ';
            out += s;
            ++found;
        }
    }
    return out;
}

} // namespace gr
