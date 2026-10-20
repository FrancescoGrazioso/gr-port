#pragma once
// GNM draw → Vulkan translation (phase 1.4).
//
// Runs on the graphics PM4 thread. Every draw packet is turned into one Vulkan draw: shaders are
// recompiled from GCN (shader_recompiler), render targets / depth buffers are host images keyed by
// their guest address, textures are either copies of render targets (post-processing chains) or
// uploads from guest memory. All work of one submission goes into one command buffer that is
// submitted and waited for at the end of the submission. A flip copies the render target at the
// display buffer's address into the output image the presenter (vk_window) blits to the screen.
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/capture.h"
#include "gpu/vk_device.h"

namespace gr::gpu {

struct OutputFrame {
    VkImage image = VK_NULL_HANDLE; // TRANSFER_SRC_OPTIMAL
    uint32_t width = 0, height = 0;
    uint64_t seq = 0;               // increments with every guest flip written to the image
};

// Presenter side: locks the output image (always pair with ReleaseOutputFrame). Returns false when no
// frame has been produced yet.
bool AcquireOutputFrame(OutputFrame* f);
// Lock-free: number of guest flips written to the output image so far, and an auto-reset event signalled
// at each one (the presenter waits on it instead of polling the locked image, which starved the renderer).
uint64_t OutputFrameSeq();
void* OutputFrameEvent();
void ReleaseOutputFrame();

// Host-memory copy of the latest flipped frame, for a presenter running on another GPU (the display
// is attached to a different adapter than the one rendering).
struct CpuFrame {
    std::vector<uint8_t> pixels; // tightly packed, 4 bytes per pixel
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t seq = 0;
};
void RendererEnableCpuFrames(bool on);
bool CopyCpuFrame(CpuFrame& dst); // true when a frame newer than dst.seq was copied

struct RendererConfig {
    bool enabled = true;
    std::string dump_dir;   // PNG dumps of flipped frames (empty = off)
    uint32_t dump_every = 0; // dump every Nth flip (0 = off)
    uint32_t dump_first = 0; // skip flips before this one
    std::vector<uint32_t> dump_at; // also dump these flips (sorted)
    bool exit_after_dumps = false; // quit the process once the last dump_at flip is written
};
void RendererConfigure(const RendererConfig& c);
bool RendererEnabled();

// Graphics thread side.
void RendererBeginSubmission(uint64_t index);
void RendererDraw(const DrawInfo& d, const uint32_t* ctx, const uint32_t* sh, const uint32_t* uc);
// DISPATCH_DIRECT on the graphics ring (x/y/z = thread-group counts).
void RendererDispatch(uint32_t x, uint32_t y, uint32_t z, const uint32_t* ctx, const uint32_t* sh, const uint32_t* uc);
void RendererEndSubmission();
void RendererMarker(uint32_t kind, const char* text); // debug marker (logged in the traced submission)
void RendererFlip(int32_t buffer_index);

std::string RendererStats();

} // namespace gr::gpu
