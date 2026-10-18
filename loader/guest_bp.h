#pragma once
#include <cstdint>

namespace gr {
// Debug hit counters on guest code (GR_BP env var); call before the eboot starts running.
void InstallGuestBreakpoints(uint64_t eboot_base);
} // namespace gr
namespace gr {
// Debug (GR_WATCH_RTS): makes a guest range inaccessible and logs the first CPU access to it.
void WatchGuestRange(uint64_t addr, uint64_t size, const char* label);
} // namespace gr
