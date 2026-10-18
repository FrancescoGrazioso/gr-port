// libkernel: errno, TLS, process parameters, time, process exit, diagnostics.
#include <windows.h>

#include <cstdio>

#include "guest.h"
#include "kernel.h"
#include "tls.h"

namespace gr::kernel {
namespace {

thread_local int t_errno = 0;

int64_t Now100nsSinceEpoch() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000LL;
}
uint64_t QpcNs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(double(c.QuadPart) * 1e9 / double(f.QuadPart));
}

// clock ids (FreeBSD): 0 REALTIME, 4 MONOTONIC, others treated as monotonic
int ClockGettime(uint64_t id, uint64_t ts) {
    int64_t sec, nsec;
    if (id == 0) {
        const int64_t t = Now100nsSinceEpoch();
        sec = t / 10000000;
        nsec = (t % 10000000) * 100;
    } else {
        const uint64_t ns = QpcNs();
        sec = int64_t(ns / 1000000000);
        nsec = int64_t(ns % 1000000000);
    }
    Store<int64_t>(ts, sec);
    Store<int64_t>(ts + 8, nsec);
    return 0;
}

int Gettimeofday(uint64_t tv) {
    const int64_t t = Now100nsSinceEpoch();
    Store<int64_t>(tv, t / 10000000);
    Store<int64_t>(tv + 8, (t % 10000000) / 10);
    return 0;
}

void SleepUs(uint64_t us) {
    if (us == 0) SwitchToThread();
    else Sleep(DWORD((us + 999) / 1000));
}

[[noreturn]] void GuestExit(int code, const char* who) {
    TraceNote("guest called %s(%d): process exit", who, code);
    std::fflush(stdout);
    ExitProcess(UINT(code));
}

} // namespace

int* ErrnoPtr() { return &t_errno; }

void RegisterMisc(Hle& h) {
    h.Register("__error", [](const uint64_t*) -> uint64_t { return reinterpret_cast<uint64_t>(&t_errno); });
    // void* __tls_get_addr(tls_index* ti)   ti = { u64 module; u64 offset }
    h.Register("__tls_get_addr", [](const uint64_t* a) -> uint64_t {
        return GuestTls::Instance()->GetAddr(Load<uint64_t>(a[0]), Load<uint64_t>(a[0] + 8));
    });
    h.Register("sceKernelGetProcParam", [](const uint64_t*) -> uint64_t { return Rt().proc_param; });
    h.Register("getpid", [](const uint64_t*) -> uint64_t { return 1000; });
    for (const char* n : {"sigprocmask", "_sigprocmask"})
        h.Register(n, [](const uint64_t* a) -> uint64_t {
            if (a[2]) std::memset(Ptr<void>(a[2]), 0, 16); // old mask: nothing blocked
            return 0;
        });
    h.Register("__pthread_cxa_finalize", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceKernelIsAddressSanitizerEnabled", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sysctl", [](const uint64_t* a) -> uint64_t {
        TraceNote("sysctl(mib=%d.%d, len=%d) not implemented", Load<int32_t>(a[0]), Load<int32_t>(a[0] + 4), int(a[1]));
        return PosixFail(kENOENT);
    });

    h.Register("__stack_chk_fail", [](const uint64_t*) -> uint64_t {
        TraceNote("__stack_chk_fail: guest stack corruption detected");
        __debugbreak(); // crash handler prints registers + guest backtrace
        return 0;
    });
    h.Register("sceKernelDebugRaiseException", [](const uint64_t* a) -> uint64_t {
        TraceNote("sceKernelDebugRaiseException(0x%llx, 0x%llx)", (unsigned long long)a[0], (unsigned long long)a[1]);
        __debugbreak();
        return 0;
    });
    h.Register("sceKernelDebugRaiseExceptionOnReleaseMode", [](const uint64_t* a) -> uint64_t {
        TraceNote("sceKernelDebugRaiseExceptionOnReleaseMode(0x%llx, 0x%llx)", (unsigned long long)a[0],
                  (unsigned long long)a[1]);
        __debugbreak();
        return 0;
    });
    h.Register("_exit", [](const uint64_t* a) -> uint64_t { GuestExit(int(a[0]), "_exit"); });
    h.Register("exit", [](const uint64_t* a) -> uint64_t { GuestExit(int(a[0]), "exit"); });

    // ---- time
    SCE("sceKernelClockGettime", return ClockGettime(a[0], a[1]););
    h.Register("clock_gettime", [](const uint64_t* a) -> uint64_t { return uint64_t(ClockGettime(a[0], a[1])); });
    h.Register("clock_getres", [](const uint64_t* a) -> uint64_t {
        Store<int64_t>(a[1], 0);
        Store<int64_t>(a[1] + 8, 100);
        return 0;
    });
    SCE("sceKernelGettimeofday", return Gettimeofday(a[0]););
    h.Register("gettimeofday", [](const uint64_t* a) -> uint64_t { return uint64_t(Gettimeofday(a[0])); });
    h.Register("sceKernelGetProcessTime", [](const uint64_t*) -> uint64_t { return QpcNs() / 1000; });
    SCE("sceKernelUsleep", SleepUs(a[0]); return 0;);
    h.Register("usleep", [](const uint64_t* a) -> uint64_t { SleepUs(a[0]); return 0; });
    SCE("sceKernelSleep", Sleep(DWORD(a[0] * 1000)); return 0;);
    h.Register("sleep", [](const uint64_t* a) -> uint64_t { Sleep(DWORD(a[0] * 1000)); return 0; });
    auto nanosleep = [](const uint64_t* a) -> uint64_t {
        const int64_t sec = Load<int64_t>(a[0]), nsec = Load<int64_t>(a[0] + 8);
        SleepUs(uint64_t(sec) * 1000000 + uint64_t(nsec) / 1000);
        if (a[1]) { Store<int64_t>(a[1], 0); Store<int64_t>(a[1] + 8, 0); }
        return 0;
    };
    h.Register("nanosleep", nanosleep);
    h.Register("sceKernelNanosleep", nanosleep);
}

} // namespace gr::kernel
