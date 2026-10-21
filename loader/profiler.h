// Sampling profiler for host threads (development only, GR_PROFILE=<thread names>|1).
#pragma once
#include <string>

namespace gr {

// Registers the calling thread for sampling when GR_PROFILE lists its name (comma separated) or is "1".
void ProfileThisThread(const char* name);
// Top functions (exclusive and inclusive samples) of every profiled thread; empty when profiling is off.
std::string ProfileReport();

} // namespace gr
