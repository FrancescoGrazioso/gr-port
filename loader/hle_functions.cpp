// Phase-0 HLE implementations: only trivial, side-effect-free services. Everything else is a
// logging stub until the corresponding subsystem is implemented (see docs/PLAN.md).
// Guest pointers are host pointers: modules are mapped into this process' address space.
#include <windows.h>

#include <cstdint>

#include "hle.h"

namespace gr {
namespace {

template <class T> void Store(uint64_t guest_ptr, T v) {
    if (guest_ptr) *reinterpret_cast<T*>(guest_ptr) = v;
}

uint64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return uint64_t(v.QuadPart);
}
uint64_t Qpf() {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return uint64_t(v.QuadPart);
}

constexpr uint32_t kSdkVersion = 0x02000000; // from param.sfo PUBTOOLINFO sdk_ver=02000000
constexpr int32_t kUserId = 1;

} // namespace

void RegisterPhase0Hle(Hle& h) {
    // --- self-test hook (not a real Orbis symbol)
    h.Register("gr_selftest_add", [](const uint64_t* a) -> uint64_t { return a[0] + a[1]; });

    // --- libkernel
    h.Register("sceKernelIsNeoMode", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceKernelGetCompiledSdkVersion", [](const uint64_t* a) -> uint64_t {
        Store<uint32_t>(a[0], kSdkVersion);
        return 0;
    });
    h.Register("sceKernelUsleep", [](const uint64_t* a) -> uint64_t {
        Sleep(DWORD(a[0] / 1000));
        return 0;
    });
    h.Register("sceKernelGetProcessTime", [](const uint64_t*) -> uint64_t { return Qpc() * 1000000 / Qpf(); });
    h.Register("sceKernelGetProcessTimeCounter", [](const uint64_t*) -> uint64_t { return Qpc(); });
    h.Register("sceKernelGetProcessTimeCounterFrequency", [](const uint64_t*) -> uint64_t { return Qpf(); });
    h.Register("sceKernelReadTsc", [](const uint64_t*) -> uint64_t { return Qpc(); });
    h.Register("sceKernelGetTscFrequency", [](const uint64_t*) -> uint64_t { return Qpf(); });

    // --- system services
    h.Register("sceSysmoduleLoadModule", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceSysmoduleIsLoaded", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceSystemServiceHideSplashScreen", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceUserServiceInitialize", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceUserServiceGetInitialUser", [](const uint64_t* a) -> uint64_t {
        Store<int32_t>(a[0], kUserId);
        return 0;
    });

    // --- input / video handles (real behaviour arrives with the respective subsystems)
    h.Register("scePadInit", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("scePadOpen", [](const uint64_t*) -> uint64_t { return 1; });
    h.Register("sceVideoOutOpen", [](const uint64_t*) -> uint64_t { return 1; });
}

} // namespace gr
