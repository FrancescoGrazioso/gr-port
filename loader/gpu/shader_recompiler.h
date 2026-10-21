#pragma once
// GCN → GLSL → SPIR-V shader recompiler.
//
// Execution model: one Vulkan invocation emulates one GCN lane. SGPRs/VGPRs are local arrays,
// EXEC / VCC / 64-bit lane masks are reduced to this lane's bit, and every VALU write is guarded by
// EXEC. Scalar memory reads (s_load / s_buffer_load) are resolved on the host before each draw by
// pre-executing the scalar instructions (see ScalarPreExecute); their results and the initial SGPR
// values reach the shader through one storage buffer ("sdata"). Image / buffer instructions get one
// descriptor binding each; the host resolves the T#/S#/V# found in the SGPRs at draw time.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gcn.h"

namespace gr::gpu {

enum class Stage : uint8_t { Vertex, Fragment, Compute };

struct ShaderKey {
    Stage stage = Stage::Vertex;
    uint64_t code = 0;        // guest address of the program
    uint64_t fetch = 0;       // VS: guest address of the fetch subroutine (0 = none)
    uint32_t user_sgprs = 0;  // SPI_SHADER_PGM_RSRC2.USER_SGPR
    uint32_t ps_input_addr = 0;
    uint32_t ps_input_cntl[32] = {}; // SPI_PS_INPUT_CNTL_n (attribute → VS param mapping)
    uint32_t num_interp = 0;
    uint64_t code_hash = 0;   // hash of the instruction words (self-modifying / reused memory guard)
    // Per resource slot (index into TranslatedShader::resources), textures only:
    // bits 0-3 dimension (0 = 2D, 1 = 3D, 2 = cube, 3 = 2D array), bits 4-5 sampled type (0 float, 1 uint, 2 sint).
    uint8_t image_dims[64] = {};
    // PS: per MRT, memory-order output swizzle (2 bits per destination component; 0 = identity) and
    // output type (0 float, 1 uint, 2 sint) derived from CB_COLORn_INFO.
    uint8_t mrt_swizzle[8] = {};
    uint8_t mrt_kind[8] = {};
    // CS: workgroup size (COMPUTE_NUM_THREAD_X/Y/Z), system SGPRs after the user ones
    // (bit 0-2 TGID_X/Y/Z, bit 3 TG_SIZE; COMPUTE_PGM_RSRC2) and LDS size in dwords.
    uint16_t cs_threads[3] = {};
    uint8_t cs_sys_sgprs = 0;
    uint32_t lds_dwords = 0;
    // Legacy geometry pipeline (ES → GS → copy VS), emulated with two compute passes and a draw:
    //   kGsExport: the ES runs as a compute shader, one invocation per index of the draw.
    //              aux[0] = index count, aux[1 + i] = vertex index; gs_params[0] = ES2GS bytes per 64 lanes.
    //   kGsGeometry: the GS runs as a compute shader, one invocation per triangle.
    //              aux[0] = primitive count; aux[1 + p] receives emitted count | cut mask << 8;
    //              gs_params[0] = ES2GS bytes per 64 lanes, gs_params[1] = GS2VS bytes per 64 lanes.
    //   kGsCopy:   the copy VS draws the GS output as a triangle list (instance = GS invocation,
    //              vertex = corner of a triangle of its strip). gs_params: 0 = GS2VS bytes per 64 lanes,
    //              1 = max vertices out, 2 = GSVS V# stride, 3 = index stride | element size << 8.
    enum GsMode : uint8_t { kGsNone = 0, kGsExport = 1, kGsGeometry = 2, kGsCopy = 3 };
    uint8_t gs_mode = kGsNone;
    uint32_t gs_params[4] = {};
    bool operator==(const ShaderKey& o) const;
};
uint64_t HashKey(const ShaderKey& k);

// Where a descriptor comes from at draw time.
struct ResourceSlot {
    enum Kind : uint8_t { kTexture, kSampler, kVertexBuffer, kRawBuffer, kStorageImage } kind;
    uint32_t instr = 0;  // index of the instruction in TranslatedShader::program
    uint16_t sgpr = 0;   // first SGPR of the descriptor
    uint32_t binding = 0;
    uint8_t components = 4; // vertex buffer: components read
    bool written = false;   // buffers: the shader stores to it (contents are written back to guest memory)
    bool vertex_indexed = true; // buffers: every access is idxen with the untouched vertex index (v0), so the
                                // host only needs the records the draw's indices reach
};

struct SmrdSlot {
    uint32_t instr = 0;     // instruction index
    uint32_t dwords = 0;    // dwords produced
    uint32_t sdata_index = 0; // where the host stores the result in sdata[]
};

struct TranslatedShader {
    ShaderKey key;
    std::vector<gcn::Instr> program; // VS: fetch subroutine inlined in place of s_swappc_b64
    std::vector<SmrdSlot> smrd;
    std::vector<ResourceSlot> resources;
    uint32_t sdata_dwords = 0;       // 104 initial SGPRs + all SMRD results
    uint32_t param_mask = 0;         // VS: exported params; PS: inputs read
    uint32_t mrt_mask = 0;           // PS: colour exports
    bool writes_depth = false;
    uint32_t num_vgprs = 0;
    std::string glsl;
    std::vector<uint32_t> spirv;
    std::string error;    // translation / compilation failed
    std::string warnings; // unsupported instructions that were skipped

    // Scalar pre-execution program (built once by Translate, run for every draw by ScalarPreExecute):
    // the instructions that change scalar state or capture resource descriptors, in program order.
    struct PreOp {
        enum Kind : uint8_t { kSalu, kSmrd, kMemtime, kEnd, kBranch, kNone } kind;
        uint32_t instr;              // index into program
        int32_t smrd;                // kSmrd: index into smrd
        int32_t target;              // kBranch: index into pre_ops (-1 = outside the program)
        uint16_t res_begin, res_end; // range of pre_res: resources captured at this instruction
    };
    std::vector<PreOp> pre_ops;
    std::vector<uint16_t> pre_res;
    bool pre_branches = false; // pre_ops has scalar branches (otherwise one pass suffices)
};

constexpr uint32_t kSgprCount = 104;
constexpr uint32_t kFirstResourceBinding = 1; // binding 0 = sdata
constexpr uint32_t kAuxBinding = 100;         // geometry-pipeline side buffer (ShaderKey::gs_mode != kGsNone)
// Descriptor set per stage: VS and CS use set 0, PS uses set 1. Bindings: 0 = sdata, then one per resource.
inline uint32_t DescriptorSetFor(Stage s) { return s == Stage::Fragment ? 1 : 0; }

// Translates a program. `code`/`fetch_code` point at guest memory.
std::unique_ptr<TranslatedShader> Translate(const ShaderKey& key, const uint32_t* code, const uint32_t* fetch_code);

// Host-side scalar pre-execution for one draw: fills `sdata` (size sdata_dwords) and returns the SGPR
// file as seen by each resource instruction (descriptor dwords for ResourceSlot::sgpr..+7).
struct ResourceDescriptors {
    std::vector<std::array<uint32_t, 8>> values; // one per TranslatedShader::resources entry
};
// SMRD reads of the pre-executor that faulted (statistics)
extern uint64_t g_read_faults;
void ScalarPreExecute(const TranslatedShader& s, const uint32_t* user_sgprs, std::vector<uint32_t>& sdata,
                      ResourceDescriptors& out);

// GLSL → SPIR-V through glslang. Returns false and fills `log` on failure.
bool CompileGlsl(const std::string& glsl, Stage stage, std::vector<uint32_t>& spirv, std::string& log);

} // namespace gr::gpu
