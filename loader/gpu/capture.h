#pragma once
// Development capture: dumps every unique shader program the game binds and logs the full draw
// state of one submission (--capture N) to <exe dir>/capture/.
#include <cstdint>
#include <string>

namespace gr::gpu {

struct DrawInfo {
    uint32_t opcode;
    uint32_t index_count;
    uint64_t index_base; // 0 for auto-index draws
    uint32_t index_type; // 0 = u16, 1 = u32
    uint32_t num_instances;
};

void CaptureConfigure(const std::string& dir, int64_t submission_to_log);
void CaptureBeginSubmission(uint64_t submission);
void CaptureDraw(const DrawInfo& d, const uint32_t* ctx, const uint32_t* sh, const uint32_t* uc);
void CaptureDispatch(uint32_t x, uint32_t y, uint32_t z, const uint32_t* sh);

// Shader code length in bytes (up to and including s_endpgm), 0 if not found within the limit.
uint32_t ShaderCodeSize(const uint32_t* code, uint32_t max_dwords = 1u << 16);

} // namespace gr::gpu
