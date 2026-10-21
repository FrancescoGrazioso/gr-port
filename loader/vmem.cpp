#include "vmem.h"
#include "write_watch.h"

#include <windows.h>

#include <algorithm>
#include <mutex>

namespace gr {
namespace {
constexpr uint64_t kGranularity = 0x10000; // Windows allocation granularity
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
std::mutex g_vmem;

uint64_t TryAt(uint64_t addr, uint64_t size, uint32_t type, uint32_t prot) {
    // new reservations are write-watched (see write_watch.h): the renderer re-reads only written pages
    const bool watch = (type & MEM_RESERVE) && WriteWatchEnabled();
    const uint64_t p = reinterpret_cast<uint64_t>(
        VirtualAlloc(reinterpret_cast<void*>(addr), size, type | (watch ? MEM_WRITE_WATCH : 0), prot));
    if (p && watch) WriteWatchAdd(p, size);
    return p;
}

uint64_t Search(uint64_t from, uint64_t to, uint64_t size, uint64_t align, uint32_t type, uint32_t prot) {
    uint64_t addr = AlignUp(from, align);
    while (addr + size <= to) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi)) return 0;
        const uint64_t region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_FREE && region_end - addr >= size) {
            if (uint64_t p = TryAt(addr, size, type, prot)) return p;
        }
        addr = AlignUp(std::max(region_end, addr + kGranularity), align);
    }
    return 0;
}
} // namespace

uint64_t GuestVirtualAlloc(uint64_t hint, uint64_t size, uint64_t align, uint32_t type, uint32_t prot, bool fixed,
                           uint64_t search_from) {
    std::lock_guard<std::mutex> lk(g_vmem);
    align = std::max(AlignUp(align ? align : kGranularity, kGranularity), kGranularity);
    size = AlignUp(size, 0x1000);
    if (hint) {
        if (uint64_t p = TryAt(hint, size, type, prot)) return p;
        if (fixed) return 0;
    }
    if (uint64_t p = Search(std::max(search_from, kGuestUserMin), kGuestUserMax, size, align, type, prot)) return p;
    return Search(kGuestUserMin, search_from, size, align, type, prot);
}

} // namespace gr
