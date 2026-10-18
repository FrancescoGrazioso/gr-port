#pragma once
// Host -> guest calls and guest threads.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>

namespace gr {

class Linker;
class GuestTls;

// Calls a SysV function with up to 6 integer arguments. Preserves every register the MS ABI
// expects to survive (rdi, rsi, xmm6-xmm15), which SysV code is free to clobber.
uint64_t CallGuest(uint64_t fn, std::initializer_list<uint64_t> args = {});

// Runs `body` on a new host thread whose whole stack is committed up front (guest code does not
// probe the stack the way MSVC code does) and whose guest TLS is set up. Returns the thread handle.
void* StartGuestThread(std::function<void()> body, size_t stack_size, uint32_t* thread_id = nullptr,
                       const char* name = nullptr);

// Suspends every guest thread briefly and prints where it is (RIP + guest return addresses).
void DumpGuestThreads();

// Process-wide state that HLE functions need.
struct Runtime {
    std::filesystem::path app0;       // host directory mounted as /app0
    std::filesystem::path savedata;   // host root for save data, /data and /temp
    std::filesystem::path savedata_mount; // directory currently mounted as /savedata0 (sceSaveDataMount)
    uint64_t proc_param = 0;          // address of the eboot's PT_SCE_PROCPARAM
    Linker* linker = nullptr;
    GuestTls* tls = nullptr;
};
Runtime& Rt();

// Crash diagnostics + call tracing.
void InstallCrashHandler();
void SetCrashHandlerBypass(bool on); // for host code that handles its own faults with SEH
void TraceOpen(const std::string& path, uint64_t max_lines, double start_after_s = 0);
void TraceCall(const char* lib, const char* name, const uint64_t* args, uint64_t ret, bool implemented);
// Marks the HLE call the current thread is inside (shown by DumpGuestThreads for blocked threads).
void TraceEnter(const char* lib, const char* name, uint64_t caller, uint64_t arg0 = 0);
void TraceLeave();
void TraceNote(const char* fmt, ...);  // also flushes the trace file
std::string GuestStackScan(int max_frames); // guest return addresses on the current stack
void TraceLine(const char* text, uint64_t caller); // trace file only (subject to --trace limits)

} // namespace gr
