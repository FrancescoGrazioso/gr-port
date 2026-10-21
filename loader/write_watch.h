#pragma once
// Guest memory write tracking (Windows write watches).
//
// Guest allocations are reserved with MEM_WRITE_WATCH, so the kernel records which pages are written,
// by any thread and by kernel-mode writes (ReadFile) alike. Once per GPU submission the renderer
// collects those pages and stamps them with a generation number; a texture or buffer copied from
// guest memory then only needs to be copied (or hashed) again when one of its pages carries a newer
// generation than the copy. GR_NO_WRITE_WATCH=1 disables it (everything counts as written).
#include <cstdint>

namespace gr {

bool WriteWatchEnabled();
// A guest allocation reserved with MEM_WRITE_WATCH / released.
void WriteWatchAdd(uint64_t base, uint64_t size);
void WriteWatchRemove(uint64_t base);
// Contents of a range changed without guest writes (decommitted pages read back as zeros).
void WriteWatchTouch(uint64_t addr, uint64_t size);
// Stamps the pages written since the previous call with `gen` (and resets the watches).
void WriteWatchCollect(uint32_t gen);
// Latest generation in which a page of [addr, addr + size) was written (0 = not since it was
// watched); kNotWatched when part of the range is not watched.
constexpr uint32_t kNotWatched = 0xFFFFFFFFu;
uint32_t WriteWatchLastWrite(uint64_t addr, uint64_t size);
double WriteWatchCollectMs(); // host time spent in WriteWatchCollect (statistics)
uint64_t WriteWatchBytes(uint32_t* regions); // collected bytes, and watched allocations (statistics)

} // namespace gr
