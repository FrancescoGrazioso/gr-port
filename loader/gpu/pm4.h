#pragma once
// AMD GCN (Liverpool) PM4 command packet definitions used by the PS4 graphics ring.
#include <cstdint>

namespace gr::gpu {

// Header: [31:30] type, type 3: [29:16] count (= body dwords - 1), [15:8] opcode, [1] shader type.
inline uint32_t Pm4Type(uint32_t h) { return h >> 30; }
inline uint32_t Pm4Count(uint32_t h) { return (h >> 16) & 0x3FFF; }
inline uint32_t Pm4Opcode(uint32_t h) { return (h >> 8) & 0xFF; }
inline uint32_t Pm4Type3(uint32_t opcode, uint32_t body_dwords) {
    return 0xC0000000u | ((body_dwords - 1) << 16) | (opcode << 8);
}

enum Op : uint32_t {
    kNop = 0x10,
    kSetBase = 0x11,
    kClearState = 0x12,
    kIndexBufferSize = 0x13,
    kDispatchDirect = 0x15,
    kDispatchIndirect = 0x16,
    kAtomicMem = 0x1E,
    kOcclusionQuery = 0x1F,
    kSetPredication = 0x20,
    kRegRmw = 0x21,
    kCondExec = 0x22,
    kPredExec = 0x23,
    kDrawIndirect = 0x24,
    kDrawIndexIndirect = 0x25,
    kIndexBase = 0x26,
    kDrawIndex2 = 0x27,
    kContextControl = 0x28,
    kIndexType = 0x2A,
    kDrawIndirectMulti = 0x2C,
    kDrawIndexAuto = 0x2D,
    kNumInstances = 0x2F,
    kDrawIndexMultiAuto = 0x30,
    kIndirectBufferConst = 0x33,
    kStrmoutBufferUpdate = 0x34,
    kDrawIndexOffset2 = 0x35,
    kDrawPreamble = 0x36,
    kWriteData = 0x37,
    kDrawIndexIndirectMulti = 0x38,
    kMemSemaphore = 0x39,
    kWaitRegMem = 0x3C,
    kIndirectBuffer = 0x3F,
    kCopyData = 0x40,
    kPfpSyncMe = 0x42,
    kSurfaceSync = 0x43,
    kCondWrite = 0x45,
    kEventWrite = 0x46,
    kEventWriteEop = 0x47,
    kEventWriteEos = 0x48,
    kReleaseMem = 0x49,
    kDmaData = 0x50,
    kAcquireMem = 0x58,
    kRewind = 0x59,
    kLoadShReg = 0x5F,
    kLoadConfigReg = 0x60,
    kLoadContextReg = 0x61,
    kSetConfigReg = 0x68,
    kSetContextReg = 0x69,
    kSetContextRegIndirect = 0x73,
    kSetShReg = 0x76,
    kSetShRegOffset = 0x77,
    kSetUconfigReg = 0x79,
    kLoadConstRam = 0x80,
    kWriteConstRam = 0x81,
    kDumpConstRam = 0x83,
    kIncrementCeCounter = 0x84,
    kIncrementDeCounter = 0x85,
    kWaitOnCeCounter = 0x86,
    kWaitOnDeCounterDiff = 0x88,
    kDispatchDrawPreamble = 0x8C,
    kDispatchDraw = 0x8D,
};

// Register file bases (dword offsets) for SET_*_REG packets.
constexpr uint32_t kConfigRegBase = 0x2000;
constexpr uint32_t kShRegBase = 0x2C00;
constexpr uint32_t kContextRegBase = 0xA000;
constexpr uint32_t kUconfigRegBase = 0xC000;

// Selected registers (absolute dword offsets).
namespace reg {
constexpr uint32_t SPI_SHADER_PGM_LO_PS = 0x2C08, SPI_SHADER_PGM_LO_VS = 0x2C48, SPI_SHADER_PGM_LO_GS = 0x2C88,
                   SPI_SHADER_PGM_LO_ES = 0x2CC8, SPI_SHADER_PGM_LO_HS = 0x2D08, SPI_SHADER_PGM_LO_LS = 0x2D48,
                   COMPUTE_NUM_THREAD_X = 0x2E07, COMPUTE_PGM_LO = 0x2E0C, COMPUTE_PGM_RSRC1 = 0x2E12;
constexpr uint32_t CB_SHADER_MASK = 0xA08F, SPI_VS_OUT_CONFIG = 0xA1B1, SPI_PS_INPUT_ENA = 0xA1B3,
                   SPI_PS_INPUT_ADDR = 0xA1B4, SPI_PS_IN_CONTROL = 0xA1B6, SPI_BARYC_CNTL = 0xA1B8,
                   SPI_SHADER_POS_FORMAT = 0xA1C3, SPI_SHADER_Z_FORMAT = 0xA1C4, SPI_SHADER_COL_FORMAT = 0xA1C5,
                   DB_SHADER_CONTROL = 0xA203, PA_CL_VS_OUT_CNTL = 0xA207;
} // namespace reg

// Markers we put in NOP payloads for driver calls whose effect we handle on the host.
constexpr uint32_t kMarkerTag = 0x47525054; // "GRPT"
enum class Marker : uint32_t { kWaitFlipDone = 1, kDebugMarker = 2, kInitDefaultState = 3, kShaderPlaceholder = 4 };

} // namespace gr::gpu
