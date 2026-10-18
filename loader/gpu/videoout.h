#pragma once
// libSceVideoOut HLE: display buffers, flips paced by a 60 Hz vblank thread, flip/vblank events.
#include <cstdint>

namespace gr {
class Hle;
}

namespace gr::videoout {

struct FrameInfo {
    uint64_t addr = 0;
    uint32_t width = 0, height = 0, pitch = 0, pixel_format = 0, tiling = 0;
    uint64_t flip_count = 0, vblank_count = 0;
};

// Queues a flip of `buffer_index` (-1 = blank); it completes on the next vblank.
void Flip(int32_t buffer_index, int64_t flip_arg);
FrameInfo Current();
FrameInfo Buffer(int32_t index); // attributes of a registered display buffer

void Register(Hle& h);

} // namespace gr::videoout
