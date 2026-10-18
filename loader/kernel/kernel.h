#pragma once
// Shared helpers for libkernel HLE.
#include <cstdint>
#include <cstring>
#include <string>

#include "hle.h"

namespace gr::kernel {

// FreeBSD errno values (Orbis is FreeBSD 9 based).
enum : int {
    kEPERM = 1, kENOENT = 2, kESRCH = 3, kEINTR = 4, kEIO = 5, kEBADF = 9, kEDEADLK = 11, kENOMEM = 12,
    kEACCES = 13, kEFAULT = 14, kEBUSY = 16, kEEXIST = 17, kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22,
    kEMFILE = 24, kENOSPC = 28, kEROFS = 30, kEAGAIN = 35, kETIMEDOUT = 60, kENOTEMPTY = 66, kENOSYS = 78,
};

// sceKernel*/scePthread* return 0x80020000 | errno; POSIX entry points return -1 and set errno
// (pthread_* return the errno value directly).
constexpr uint64_t SceError(int e) { return 0x80020000u | uint32_t(e); }

int* ErrnoPtr();
inline uint64_t PosixFail(int e) {
    *ErrnoPtr() = e;
    return uint64_t(-1);
}

template <class T> inline T* Ptr(uint64_t guest) { return reinterpret_cast<T*>(guest); }
template <class T> inline void Store(uint64_t guest, T v) {
    if (guest) std::memcpy(Ptr<void>(guest), &v, sizeof(T));
}
template <class T> inline T Load(uint64_t guest) {
    T v{};
    if (guest) std::memcpy(&v, Ptr<void>(guest), sizeof(T));
    return v;
}
inline std::string Str(uint64_t guest) { return guest ? std::string(Ptr<const char>(guest)) : std::string(); }
// Float/double arguments passed in xmm0..xmm3 (captured by the thunk into a[10..13]).
inline float ArgF(const uint64_t* a, int i) { float f; std::memcpy(&f, &a[10 + i], 4); return f; }
inline double ArgD(const uint64_t* a, int i) { double d; std::memcpy(&d, &a[10 + i], 8); return d; }

// Registration entry points (one per source file).
void RegisterMisc(Hle& h);
void RegisterMemory(Hle& h);
void RegisterThreads(Hle& h);
void RegisterFiles(Hle& h);
void RegisterLibcInternal(Hle& h);
void RegisterSync(Hle& h);
void RegisterServices(Hle& h);
void RegisterFiber(Hle& h);
void RegisterAudio(Hle& h);
void RegisterPad(Hle& h);
void RegisterAvPlayer(Hle& h);
void RegisterFont(Hle& h);
void RegisterSaveData(Hle& h);
void SetPadScript(const std::string& script); // "5:OPTIONS,8.5:CROSS:0.3"

// Called on the guest main thread before any guest code runs.
void InitMainThread();

// Registration helpers: `a` is the SysV argument array. SCE() bodies return an errno that becomes
// 0x80020000|errno; POSIX() bodies return the value as-is (pthread_* style errno returns).

#define SCE(name, ...)                                                                         \
    h.Register(name, [](const uint64_t* a) -> uint64_t {                                        \
        const int e = [&]() -> int { __VA_ARGS__ }();                                                  \
        return e ? SceError(e) : 0;                                                             \
    })
#define POSIX(name, ...)                                                                       \
    h.Register(name, [](const uint64_t* a) -> uint64_t { return uint64_t(uint32_t([&]() -> int { __VA_ARGS__ }())); })
#define BOTH(sce_name, posix_name, ...) SCE(sce_name, __VA_ARGS__); POSIX(posix_name, __VA_ARGS__)
} // namespace gr::kernel
