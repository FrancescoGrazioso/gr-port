#include "write_watch.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gr {
namespace {

constexpr uint64_t kPage = 0x1000;
constexpr int kBlockShift = 18; // 2^18 pages (1 GiB) per generation block

std::mutex g_m;                             // guards the registry and the pending ranges
// Watched allocation base → size. Only regions the renderer has read from are collected (`active`):
// scanning the whole guest memory (GBs) every frame would cost milliseconds.
struct Region { uint64_t size; bool active; };
std::map<uint64_t, Region> g_regions;
std::vector<std::pair<uint64_t, uint64_t>> g_pending; // ranges whose contents changed without writes (new, decommitted)
// Page → generation of its last write. Only the renderer thread reads and writes it.
std::unordered_map<uint64_t, std::unique_ptr<uint32_t[]>> g_blocks;
double g_collect_ms = 0;

uint32_t* Block(uint64_t page, bool create) {
    const uint64_t b = page >> kBlockShift;
    auto it = g_blocks.find(b);
    if (it != g_blocks.end()) return it->second.get();
    if (!create) return nullptr;
    auto blk = std::make_unique<uint32_t[]>(size_t(1) << kBlockShift); // zero = never written
    uint32_t* p = blk.get();
    g_blocks.emplace(b, std::move(blk));
    return p;
}

void Stamp(uint64_t addr, uint64_t size, uint32_t gen) {
    for (uint64_t page = addr / kPage, end = (addr + size + kPage - 1) / kPage; page < end; ++page)
        Block(page, true)[page & ((1u << kBlockShift) - 1)] = gen;
}

} // namespace

bool WriteWatchEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("GR_NO_WRITE_WATCH");
        return !(e && *e && *e != '0');
    }();
    return on;
}

void WriteWatchAdd(uint64_t base, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_m);
    g_regions[base] = {size, false};
    g_pending.push_back({base, size});
}

void WriteWatchRemove(uint64_t base) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_regions.find(base);
    if (it == g_regions.end()) return;
    g_pending.push_back({it->first, it->second.size}); // a later allocation here starts with other contents
    g_regions.erase(it);
}

void WriteWatchTouch(uint64_t addr, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_m);
    g_pending.push_back({addr, size});
}

void WriteWatchCollect(uint32_t gen) {
    if (!WriteWatchEnabled()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<uint64_t, uint64_t>> regions, pending;
    {
        std::lock_guard<std::mutex> lk(g_m);
        for (const auto& [b, r] : g_regions)
            if (r.active) regions.push_back({b, r.size});
        pending.swap(g_pending);
    }
    for (const auto& [a, s] : pending) Stamp(a, s, gen);
    static std::vector<void*> hits(65536);
    for (const auto& [base, size] : regions) {
        uint64_t from = base;
        while (from < base + size) {
            ULONG_PTR count = hits.size();
            ULONG gran = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(from), SIZE_T(base + size - from), hits.data(),
                              &count, &gran) != 0)
                break; // released meanwhile
            for (ULONG_PTR i = 0; i < count; ++i) Stamp(reinterpret_cast<uint64_t>(hits[i]), kPage, gen);
            if (count < hits.size()) break;
            from = reinterpret_cast<uint64_t>(hits[count - 1]) + kPage; // more than fit in one call
        }
    }
    g_collect_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

uint32_t WriteWatchLastWrite(uint64_t addr, uint64_t size) {
    if (!WriteWatchEnabled() || !size) return kNotWatched;
    {
        std::lock_guard<std::mutex> lk(g_m);
        auto it = g_regions.upper_bound(addr);
        if (it == g_regions.begin()) return kNotWatched;
        --it;
        if (addr + size > it->first + it->second.size) return kNotWatched;
        if (!it->second.active) { // first read: collected from the next frame on, everything before counts as written
            it->second.active = true;
            return kNotWatched;
        }
    }
    uint32_t last = 0;
    uint64_t page = addr / kPage;
    const uint64_t end = (addr + size + kPage - 1) / kPage;
    while (page < end) {
        const uint64_t block_end = std::min(end, ((page >> kBlockShift) + 1) << kBlockShift);
        if (const uint32_t* b = Block(page, false))
            for (uint64_t p = page; p < block_end; ++p) last = std::max(last, b[p & ((1u << kBlockShift) - 1)]);
        page = block_end;
    }
    return last;
}

double WriteWatchCollectMs() { return g_collect_ms; }

} // namespace gr

namespace gr {
uint64_t WriteWatchBytes(uint32_t* regions) {
    std::lock_guard<std::mutex> lk(g_m);
    uint64_t n = 0;
    for (const auto& [b, r] : g_regions)
        if (r.active) n += r.size;
    if (regions) *regions = uint32_t(g_regions.size());
    return n;
}
} // namespace gr
