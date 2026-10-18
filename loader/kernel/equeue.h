#pragma once
// Kernel event queue shared by libkernel (user events) and future VideoOut / GNM event sources.
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gr::kernel {

// FreeBSD/Orbis event filters
constexpr int16_t kFilterUser = -11;
constexpr int16_t kFilterVideoOut = -13;
constexpr int16_t kFilterGraphics = -14;

struct EventQueue {
    struct Reg {
        uint64_t ident;
        int16_t filter;
        uint64_t udata;
        bool edge;
        bool triggered;
        int64_t data;
    };
    std::mutex m;
    std::condition_variable cv;
    std::vector<Reg> regs;
    char name[32] = {};

    void Add(uint64_t ident, int16_t filter, uint64_t udata, bool edge_triggered);
    bool Remove(uint64_t ident, int16_t filter);
    bool Trigger(uint64_t ident, int16_t filter, int64_t data, uint64_t udata_override = 0, bool use_override = false);
    int Wait(uint64_t events, int max_events, uint64_t out_count, uint64_t timeout_ptr); // returns errno
};

} // namespace gr::kernel
