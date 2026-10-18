#pragma once
// Host-side "GPU": a thread that consumes the guest's PM4 command buffers in submission order.
//
// Phase 1.3 executes only the packets that synchronise with the CPU (label / end-of-pipe writes,
// memory waits, DMA copies, indirect buffers) and counts everything else, so the game's frame loop
// can make progress. Draws are translated to Vulkan in phases 1.4/1.5.
#include <cstdint>
#include <string>
#include <vector>

namespace gr {
class Hle;
}
namespace gr::kernel {
struct EventQueue;
}

namespace gr::gpu {

struct CommandBuffer {
    uint64_t addr;
    uint32_t size_bytes;
};

struct Submission {
    std::vector<CommandBuffer> dcbs;
    std::vector<CommandBuffer> ccbs;
    bool flip = false;
    uint32_t vo_handle = 0, buffer_index = 0, flip_mode = 0;
    int64_t flip_arg = 0;
};

void Start();
void Submit(Submission s);
// Queues a display flip behind the work already submitted (sceVideoOutSubmitFlip & co.).
void SubmitFlip(int32_t buffer_index, int64_t flip_arg);
void WaitIdle();

// End-of-pipe interrupt listeners (sceGnmAddEqEvent).
void AddEopEvent(kernel::EventQueue* eq, uint64_t id, uint64_t udata);
void RemoveEopEvent(kernel::EventQueue* eq, uint64_t id);

// Compute rings (sceGnmMapComputeQueue / sceGnmDingDong).
uint32_t MapComputeQueue(uint32_t pipe, uint32_t queue, uint64_t ring_base, uint32_t ring_size_dw,
                         uint64_t read_ptr_addr);
void DingDong(uint32_t vqid, uint32_t next_offset_dw);

uint64_t GpuTimestamp(); // 100 MHz-style monotonically increasing counter
std::string StatsSummary();

void RegisterGnmDriver(Hle& h); // gnm_driver.cpp

} // namespace gr::gpu
