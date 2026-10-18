#pragma once
// Guest virtual memory placement.
//
// Orbis user space ends below 0xFC'0000'0000 (~1 TiB) and system libraries check this (libc's
// sceLibcMspaceCreate rejects heaps with base+size >= 0x3E << 34). Windows places allocations
// wherever it likes (often above 2 TiB), so every guest-visible allocation goes through here and
// is put at the first free, suitably aligned range inside the PS4 user window.
#include <cstdint>

namespace gr {

constexpr uint64_t kGuestUserMin = 0x0000'0040'0000ull; // 4 MiB
constexpr uint64_t kGuestUserMax = 0x00FC'0000'0000ull;
constexpr uint64_t kGuestModuleBase = 0x0008'0000'0000ull; // where PS4 maps system/app libraries
constexpr uint64_t kGuestHeapBase = 0x0010'0000'0000ull;   // default search start for data

// Allocates `size` bytes at `hint` if possible, otherwise at the first free aligned range at or
// above `search_from` (wrapping to kGuestUserMin). `type` and `prot` are VirtualAlloc flags.
// Returns 0 when the window is exhausted (or when `fixed` and `hint` is not available).
uint64_t GuestVirtualAlloc(uint64_t hint, uint64_t size, uint64_t align, uint32_t type, uint32_t prot,
                           bool fixed = false, uint64_t search_from = kGuestHeapBase);

} // namespace gr
