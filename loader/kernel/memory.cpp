// libkernel memory: mmap/munmap, flexible memory, direct memory (simplified), protection queries.
// Direct memory is modelled as a bump-allocated "physical" offset space; mapping an offset gives a
// fresh host allocation (true aliasing of one physical range at two addresses is phase 1.2 work).
#include <windows.h>

#include <map>
#include <mutex>

#include "guest.h"
#include "kernel.h"
#include "vmem.h"
#include "write_watch.h"

namespace gr::kernel {
namespace {

constexpr uint64_t kPageSize = 0x4000;
constexpr uint64_t kDirectMemorySize = 5ull << 30;    // 5 GiB visible to games
constexpr uint64_t kFlexibleMemorySize = 448ull << 20;
constexpr uint64_t kDirectMapBase = 0x2'0000'0000ull; // default placement of direct mappings

// PS4 PROT_* (CPU bits; GPU bits 0x10/0x20 are ignored on the CPU side).
DWORD ToWinProt(uint64_t prot) {
    const bool r = prot & 1, w = prot & 2, x = prot & 4;
    if (x) return w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (w) return PAGE_READWRITE;
    if (r || (prot & 0x30)) return PAGE_READWRITE; // GPU-only mappings are still touched by our CPU code
    return PAGE_NOACCESS;
}
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

std::mutex g_mem;
uint64_t g_direct_next = 0;
std::map<uint64_t, uint64_t> g_direct_alloc;   // phys offset -> length
std::map<uint64_t, uint64_t> g_direct_mapped;  // phys offset -> host address of first mapping
uint64_t g_flex_used = 0;

// Maps `len` bytes; honours a fixed address (inside a reservation or free space).
uint64_t MapAnon(uint64_t addr, uint64_t len, uint64_t prot, bool fixed, uint64_t align = 0,
                 uint64_t search_from = kGuestHeapBase) {
    len = AlignUp(len, kPageSize);
    if (addr) {
        // Inside a range reserved earlier (sceKernelReserveVirtualRange): commit only.
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi);
        if (mbi.State == MEM_RESERVE) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(addr), len, MEM_COMMIT, ToWinProt(prot)))
                return reinterpret_cast<uint64_t>(p);
            if (fixed) return 0;
        }
    }
    return GuestVirtualAlloc(addr, len, align, MEM_COMMIT | MEM_RESERVE, ToWinProt(prot), fixed, search_from);
}

void Unmap(uint64_t addr, uint64_t len) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) return;
    if (reinterpret_cast<uint64_t>(mbi.AllocationBase) == addr && mbi.RegionSize >= len)
    {
        VirtualFree(reinterpret_cast<void*>(addr), 0, MEM_RELEASE);
        WriteWatchRemove(addr);
    } else {
        VirtualFree(reinterpret_cast<void*>(addr), len, MEM_DECOMMIT);
        WriteWatchTouch(addr, len);
    }
}

int MapDirect(uint64_t addr_pp, uint64_t len, uint64_t prot, uint64_t flags, uint64_t phys, uint64_t align,
              const char* name) {
    std::lock_guard<std::mutex> lk(g_mem);
    const uint64_t hint = Load<uint64_t>(addr_pp);
    const bool fixed = flags & 0x10;
    // Without a hint the Orbis kernel places direct mappings from 0x2'0000'0000 up (shadPS4 does the
    // same, DEFAULT_MAPPING_BASE); games can depend on that layout.
    const uint64_t p = MapAnon(hint, len, prot, fixed, align, kDirectMapBase);
    if (!p) return kENOMEM;
    g_direct_mapped.emplace(phys, p);
    Store<uint64_t>(addr_pp, p);
    TraceNote("direct map %s hint=0x%llx flags=0x%llx align=0x%llx phys=0x%llx len=0x%llx -> 0x%llx", name ? name : "", (unsigned long long)hint, (unsigned long long)flags, (unsigned long long)align, (unsigned long long)phys,
              (unsigned long long)len, (unsigned long long)p);
    return 0;
}

int AllocDirect(uint64_t start, uint64_t end, uint64_t len, uint64_t align, uint64_t phys_out) {
    std::lock_guard<std::mutex> lk(g_mem);
    if (!align) align = kPageSize;
    uint64_t off = AlignUp(std::max(g_direct_next, start), align);
    if (end && off + len > end) return kEAGAIN;
    if (off + len > kDirectMemorySize) return kENOMEM;
    g_direct_next = off + len;
    g_direct_alloc[off] = len;
    Store<uint64_t>(phys_out, off);
    return 0;
}

} // namespace

void RegisterMemory(Hle& h) {
    h.Register("getpagesize", [](const uint64_t*) -> uint64_t { return kPageSize; });

    // void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
    h.Register("mmap", [](const uint64_t* a) -> uint64_t {
        const bool anon = a[3] & 0x1000, fixed = a[3] & 0x10;
        const uint64_t p = MapAnon(a[0], a[1], a[2] | 2, fixed); // writable while we fill it
        if (!p) return PosixFail(kENOMEM);
        if (!anon) TraceNote("mmap of fd %d not supported yet (zero-filled)", int(a[4]));
        return p;
    });
    h.Register("munmap", [](const uint64_t* a) -> uint64_t {
        Unmap(a[0], a[1]);
        return 0;
    });
    SCE("sceKernelMunmap", Unmap(a[0], a[1]); return 0;);
    SCE("sceKernelMmap", { // (addr, len, prot, flags, fd, off, void** res)
        const uint64_t p = MapAnon(a[0], a[1], a[2] | 2, a[3] & 0x10);
        if (!p) return kENOMEM;
        Store<uint64_t>(a[6], p); // 7th argument: first stack slot
        return 0;
    });

    // ---- flexible memory: (void** addr_inout, size_t len, int prot, int flags[, const char* name])
    auto map_flex = [](const uint64_t* a) -> uint64_t {
        const uint64_t p = MapAnon(Load<uint64_t>(a[0]), a[1], a[2], a[3] & 0x10);
        if (!p) return SceError(kENOMEM);
        g_flex_used += AlignUp(a[1], kPageSize);
        Store<uint64_t>(a[0], p);
        return 0;
    };
    h.Register("sceKernelMapFlexibleMemory", map_flex);
    h.Register("sceKernelMapNamedFlexibleMemory", map_flex);
    SCE("sceKernelReleaseFlexibleMemory", Unmap(a[0], a[1]); return 0;);
    SCE("sceKernelAvailableFlexibleMemorySize", Store<uint64_t>(a[0], kFlexibleMemorySize - g_flex_used); return 0;);

    // ---- direct memory
    h.Register("sceKernelGetDirectMemorySize", [](const uint64_t*) -> uint64_t { return kDirectMemorySize; });
    // (searchStart, searchEnd, len, align, memoryType, off_t* physOut)
    SCE("sceKernelAllocateDirectMemory", return AllocDirect(a[0], a[1], a[2], a[3], a[5]););
    // (len, align, memoryType, off_t* physOut)
    SCE("sceKernelAllocateMainDirectMemory", return AllocDirect(0, kDirectMemorySize, a[0], a[1], a[3]););
    SCE("sceKernelReleaseDirectMemory", {
        std::lock_guard<std::mutex> lk(g_mem);
        g_direct_alloc.erase(a[0]);
        return 0;
    });
    SCE("sceKernelCheckedReleaseDirectMemory", {
        std::lock_guard<std::mutex> lk(g_mem);
        g_direct_alloc.erase(a[0]);
        return 0;
    });
    // (searchStart, searchEnd, align, off_t* physOut, size_t* sizeOut)
    SCE("sceKernelAvailableDirectMemorySize", {
        Store<uint64_t>(a[3], g_direct_next);
        Store<uint64_t>(a[4], kDirectMemorySize - g_direct_next);
        return 0;
    });
    // (void** addr, size_t len, int prot, int flags, off_t phys, size_t align)
    SCE("sceKernelMapDirectMemory", return MapDirect(a[0], a[1], a[2], a[3], a[4], a[5], nullptr););
    // (void** addr, size_t len, int prot, int flags, off_t phys, size_t align, const char* name)
    SCE("sceKernelMapNamedDirectMemory", return MapDirect(a[0], a[1], a[2], a[3], a[4], a[5], Ptr<const char>(a[6])););
    // (void** addr, size_t len, int flags, size_t align)
    SCE("sceKernelReserveVirtualRange", {
        const uint64_t p = GuestVirtualAlloc(Load<uint64_t>(a[0]), AlignUp(a[1], kPageSize), a[3], MEM_RESERVE,
                                             PAGE_NOACCESS, a[2] & 0x10);
        if (!p) return kENOMEM;
        Store<uint64_t>(a[0], p);
        return 0;
    });
    SCE("sceKernelSetVirtualRangeName", return 0;);

    // ---- protection
    SCE("sceKernelMprotect", {
        DWORD old;
        return VirtualProtect(Ptr<void>(a[0]), a[1], ToWinProt(a[2]), &old) ? 0 : kEINVAL;
    });
    SCE("sceKernelMtypeprotect", { // (addr, len, mtype, prot)
        DWORD old;
        return VirtualProtect(Ptr<void>(a[0]), a[1], ToWinProt(a[3]), &old) ? 0 : kEINVAL;
    });
    h.Register("mprotect", [](const uint64_t* a) -> uint64_t {
        DWORD old;
        return VirtualProtect(Ptr<void>(a[0]), a[1], ToWinProt(a[2]), &old) ? 0 : PosixFail(kEINVAL);
    });
    // (addr, void** start, void** end, int* prot)
    SCE("sceKernelQueryMemoryProtection", {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(Ptr<void>(a[0]), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return kEACCES;
        const uint64_t s = reinterpret_cast<uint64_t>(mbi.BaseAddress);
        Store<uint64_t>(a[1], s);
        Store<uint64_t>(a[2], s + mbi.RegionSize - 1);
        int prot = 0;
        if (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) prot |= 1;
        if (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) prot |= 2;
        if (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) prot |= 4;
        Store<int32_t>(a[3], prot);
        return 0;
    });
    // (addr, int flags, SceKernelVirtualQueryInfo* info, size_t infoSize)
    SCE("sceKernelVirtualQuery", {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(Ptr<void>(a[0]), &mbi, sizeof mbi) || mbi.State == MEM_FREE) return kEACCES;
        uint8_t info[72] = {};
        const uint64_t s = reinterpret_cast<uint64_t>(mbi.BaseAddress), e = s + mbi.RegionSize;
        std::memcpy(info + 0, &s, 8);
        std::memcpy(info + 8, &e, 8);
        const int32_t prot = mbi.State == MEM_COMMIT ? 3 : 0;
        std::memcpy(info + 24, &prot, 4);
        info[32] = mbi.State == MEM_COMMIT ? 0x10 : 0; // isCommitted bit
        std::memcpy(Ptr<void>(a[2]), info, std::min<uint64_t>(a[3], sizeof info));
        return 0;
    });
}

} // namespace gr::kernel
