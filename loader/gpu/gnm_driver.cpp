// libSceGnmDriver HLE.
//
// Many Gnm driver entry points don't talk to the GPU at all: they write PM4 packets into a slot of
// exactly `size` dwords that the game has reserved in its command buffer. They must fill that slot
// completely, otherwise the command stream contains garbage. Shader-binding calls emit the real
// SET_SH_REG / SET_CONTEXT_REG packets (needed by the draw translator later); everything else is
// padded with tagged NOP packets.
#include <windows.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "gpu/gpu.h"
#include "gpu/pm4.h"
#include "gpu/videoout.h"
#include "guest.h"
#include "kernel/equeue.h"
#include "kernel/kernel.h"
#include "vmem.h"

namespace gr::gpu {
namespace {

using kernel::Load;
using kernel::Ptr;

class CmdWriter {
public:
    CmdWriter(uint64_t cmd, uint64_t size_dw) : p_(Ptr<uint32_t>(cmd)), cap_(uint32_t(size_dw)) {}

    bool SetRegs(uint32_t opcode, uint32_t base, uint32_t reg, std::initializer_list<uint32_t> values) {
        const uint32_t body = 1 + uint32_t(values.size());
        if (used_ + 1 + body > cap_) return false;
        p_[used_++] = Pm4Type3(opcode, body);
        p_[used_++] = reg - base;
        for (uint32_t v : values) p_[used_++] = v;
        return true;
    }
    bool Sh(uint32_t reg, std::initializer_list<uint32_t> v) { return SetRegs(kSetShReg, kShRegBase, reg, v); }
    bool Ctx(uint32_t reg, std::initializer_list<uint32_t> v) { return SetRegs(kSetContextReg, kContextRegBase, reg, v); }

    // Pads the rest of the slot with one NOP packet carrying a marker.
    uint32_t Finish(Marker marker) {
        const uint32_t left = cap_ - used_;
        if (left == 0) return used_;
        if (left == 1) {
            p_[used_++] = 0x80000000u; // type-2 filler
            return used_;
        }
        p_[used_] = Pm4Type3(kNop, left - 1);
        for (uint32_t i = 1; i < left; ++i) p_[used_ + i] = 0;
        if (left >= 3) {
            p_[used_ + 1] = kMarkerTag;
            p_[used_ + 2] = uint32_t(marker);
        }
        used_ = cap_;
        return used_;
    }

private:
    uint32_t* p_;
    uint32_t cap_;
    uint32_t used_ = 0;
};

uint64_t FillNop(const uint64_t* a, Marker m) {
    if (!a[0] || !a[1]) return 0;
    CmdWriter(a[0], a[1]).Finish(m);
    return 0;
}

// Debug markers keep their text in the NOP: body = tag, kDebugMarker, kind (0 pop, 1 push, 2 set), text.
uint64_t FillMarker(const uint64_t* a, uint32_t kind, const char* text) {
    if (!a[0] || !a[1]) return 0;
    auto* p = Ptr<uint32_t>(a[0]);
    const uint32_t cap = uint32_t(a[1]);
    CmdWriter(a[0], a[1]).Finish(Marker::kDebugMarker);
    if (cap >= 5) {
        p[3] = kind;
        char* dst = reinterpret_cast<char*>(p + 4);
        const size_t room = (cap - 4) * 4;
        size_t i = 0;
        if (text)
            for (; i + 1 < room && text[i]; ++i) dst[i] = text[i];
        if (room) dst[i < room ? i : room - 1] = 0;
    }
    return 0;
}

// Gnm::VsStageRegisters { pgmLo, pgmHi, rsrc1, rsrc2, spiVsOutConfig, spiShaderPosFormat, paClVsOutCntl }
uint64_t SetVs(const uint64_t* a) {
    CmdWriter w(a[0], a[1]);
    if (a[2]) {
        const uint32_t* r = Ptr<uint32_t>(a[2]);
        const uint32_t modifier = uint32_t(a[3]);
        const uint32_t rsrc1 = modifier ? (r[2] & 0xFCFFFC3Fu) | modifier : r[2];
        // Debug (GR_DEBUG_SETVS=<hex VS address>): who binds that vertex shader
        static const uint64_t dbg_vs = [] { const char* e = std::getenv("GR_DEBUG_SETVS"); return e ? std::strtoull(e, nullptr, 16) : 0ull; }();
        // Debug (GR_SETVS_STATS): per-VS bind counts, logged every 20000 binds
        static const bool vs_stats = std::getenv("GR_SETVS_STATS") != nullptr;
        if (vs_stats) {
            static std::mutex m;
            static std::map<uint64_t, uint64_t> counts;
            static uint64_t total = 0;
            std::lock_guard<std::mutex> lk(m);
            counts[(uint64_t(r[0]) | (uint64_t(r[1] & 0xFF) << 32)) << 8]++;
            if (++total % 20000 == 0) {
                std::string s;
                for (auto& [addr, c] : counts) {
                    char b[48];
                    std::snprintf(b, sizeof b, " %llx=%llu", (unsigned long long)addr, (unsigned long long)c);
                    s += b;
                }
                TraceNote("setvs stats after %llu:%s", (unsigned long long)total, s.c_str());
            }
        }
        // Debug (GR_SETVS_FROM=<substring>): VS bound while the guest stack contains that symbol text
        static const char* vs_from = std::getenv("GR_SETVS_FROM");
        if (vs_from) {
            static int logged = 0;
            if (logged < 40) {
                const std::string st = GuestStackScan(24);
                if (st.find(vs_from) != std::string::npos) {
                    ++logged;
                    TraceNote("setvs-from %s: vs 0x%llx stack:%s", vs_from,
                              (unsigned long long)((uint64_t(r[0]) | (uint64_t(r[1] & 0xFF) << 32)) << 8), st.c_str());
                }
            }
        }
        if (dbg_vs && ((uint64_t(r[0]) | (uint64_t(r[1] & 0xFF) << 32)) << 8) == dbg_vs) {
            static int logged = 0;
            if (logged++ < 16) TraceNote("setvs 0x%llx from 0x%llx stack:%s", (unsigned long long)dbg_vs, (unsigned long long)a[8], GuestStackScan(16).c_str());
        }
        w.Sh(reg::SPI_SHADER_PGM_LO_VS, {r[0], r[1], rsrc1, r[3]});
        w.Ctx(reg::SPI_VS_OUT_CONFIG, {r[4]});
        w.Ctx(reg::SPI_SHADER_POS_FORMAT, {r[5]});
        w.Ctx(reg::PA_CL_VS_OUT_CNTL, {r[6]});
    }
    w.Finish(Marker::kShaderPlaceholder);
    return 0;
}

// Gnm::PsStageRegisters { pgmLo, pgmHi, rsrc1, rsrc2, zFormat, colFormat, inputEna, inputAddr,
//                         inControl, barycCntl, dbShaderControl, cbShaderMask }
uint64_t SetPs(const uint64_t* a) {
    CmdWriter w(a[0], a[1]);
    if (a[2]) {
        const uint32_t* r = Ptr<uint32_t>(a[2]);
        w.Sh(reg::SPI_SHADER_PGM_LO_PS, {r[0], r[1], r[2], r[3]});
        w.Ctx(reg::SPI_SHADER_Z_FORMAT, {r[4], r[5]});
        w.Ctx(reg::SPI_PS_INPUT_ENA, {r[6], r[7]});
        w.Ctx(reg::SPI_PS_IN_CONTROL, {r[8]});
        w.Ctx(reg::SPI_BARYC_CNTL, {r[9]});
        w.Ctx(reg::DB_SHADER_CONTROL, {r[10]});
        w.Ctx(reg::CB_SHADER_MASK, {r[11]});
    } else { // no pixel shader bound
        w.Sh(reg::SPI_SHADER_PGM_LO_PS, {0, 0});
        w.Ctx(reg::CB_SHADER_MASK, {0});
    }
    w.Finish(Marker::kShaderPlaceholder);
    return 0;
}

// Gnm::CsStageRegisters { pgmLo, pgmHi, rsrc1, rsrc2, numThreadX, numThreadY, numThreadZ }
uint64_t SetCs(const uint64_t* a) {
    CmdWriter w(a[0], a[1]);
    if (a[2]) {
        const uint32_t* r = Ptr<uint32_t>(a[2]);
        w.Sh(reg::COMPUTE_PGM_LO, {r[0], r[1]});
        w.Sh(reg::COMPUTE_PGM_RSRC1, {r[2], r[3]});
        w.Sh(reg::COMPUTE_NUM_THREAD_X, {r[4], r[5], r[6]});
    }
    w.Finish(Marker::kShaderPlaceholder);
    return 0;
}

// ES/GS/HS/LS: program address + resources (the stage-specific extra registers come with 1.4/1.5).
uint64_t SetOtherStage(const uint64_t* a, uint32_t pgm_lo_reg) {
    CmdWriter w(a[0], a[1]);
    if (a[2]) {
        const uint32_t* r = Ptr<uint32_t>(a[2]);
        w.Sh(pgm_lo_reg, {r[0], r[1], r[2], r[3]});
    }
    w.Finish(Marker::kShaderPlaceholder);
    return 0;
}

// Default hardware state: the real driver writes 0x100 dwords and returns that count.
uint64_t InitDefaultState(const uint64_t* a) {
    const uint64_t n = a[1] < 0x100 ? a[1] : 0x100;
    if (!a[0] || !n) return 0;
    CmdWriter(a[0], n).Finish(Marker::kInitDefaultState);
    return n;
}

Submission BuildSubmission(const uint64_t* a) {
    Submission s;
    const uint32_t count = uint32_t(a[0]);
    for (uint32_t i = 0; i < count; ++i) {
        if (a[1]) s.dcbs.push_back({Load<uint64_t>(a[1] + 8ull * i), a[2] ? Load<uint32_t>(a[2] + 4ull * i) : 0});
        if (a[3]) s.ccbs.push_back({Load<uint64_t>(a[3] + 8ull * i), a[4] ? Load<uint32_t>(a[4] + 4ull * i) : 0});
    }
    return s;
}

uint64_t g_tess_ring = 0;
std::once_flag g_tess_once;

} // namespace

void RegisterGnmDriver(Hle& h) {
    h.Register("sceGnmSetVsShader", SetVs);
    h.Register("sceGnmUpdateVsShader", SetVs);
    h.Register("sceGnmSetPsShader", SetPs);
    h.Register("sceGnmSetPsShader350", SetPs);
    h.Register("sceGnmUpdatePsShader", SetPs);
    h.Register("sceGnmUpdatePsShader350", SetPs);
    h.Register("sceGnmSetCsShader", SetCs);
    h.Register("sceGnmSetCsShaderWithModifier", SetCs);
    // Gnm::EsStageRegisters { pgmLo, pgmHi, rsrc1, rsrc2 } + shader modifier (as for the VS)
    h.Register("sceGnmSetEsShader", [](const uint64_t* a) -> uint64_t {
        CmdWriter w(a[0], a[1]);
        if (a[2]) {
            const uint32_t* r = Ptr<uint32_t>(a[2]);
            const uint32_t modifier = uint32_t(a[3]);
            w.Sh(reg::SPI_SHADER_PGM_LO_ES, {r[0], r[1], modifier ? (r[2] & 0xFCFFFC3Fu) | modifier : r[2], r[3]});
        }
        w.Finish(Marker::kShaderPlaceholder);
        return 0;
    });
    // Gnm::GsStageRegisters { pgmLo, pgmHi, rsrc1, rsrc2, vgtStrmoutConfig, vgtGsOutPrimType, vgtGsInstanceCnt }
    h.Register("sceGnmSetGsShader", [](const uint64_t* a) -> uint64_t {
        CmdWriter w(a[0], a[1]);
        if (a[2]) {
            const uint32_t* r = Ptr<uint32_t>(a[2]);
            w.Sh(reg::SPI_SHADER_PGM_LO_GS, {r[0], r[1], r[2], r[3]});
            w.Ctx(0xA2E5, {r[4]}); // VGT_STRMOUT_CONFIG
            w.Ctx(0xA29B, {r[5]}); // VGT_GS_OUT_PRIM_TYPE
            w.Ctx(0xA2E4, {r[6]}); // VGT_GS_INSTANCE_CNT
        }
        w.Finish(Marker::kShaderPlaceholder);
        return 0;
    });
    h.Register("sceGnmUpdateGsShader", [](const uint64_t* a) -> uint64_t { return SetOtherStage(a, reg::SPI_SHADER_PGM_LO_GS); });
    h.Register("sceGnmSetHsShader", [](const uint64_t* a) -> uint64_t { return SetOtherStage(a, reg::SPI_SHADER_PGM_LO_HS); });
    h.Register("sceGnmUpdateHsShader", [](const uint64_t* a) -> uint64_t { return SetOtherStage(a, reg::SPI_SHADER_PGM_LO_HS); });
    h.Register("sceGnmSetLsShader", [](const uint64_t* a) -> uint64_t { return SetOtherStage(a, reg::SPI_SHADER_PGM_LO_LS); });

    h.Register("sceGnmDrawInitDefaultHardwareState", InitDefaultState);
    h.Register("sceGnmDrawInitDefaultHardwareState175", InitDefaultState);
    h.Register("sceGnmDrawInitDefaultHardwareState200", InitDefaultState);
    h.Register("sceGnmDrawInitDefaultHardwareState350", InitDefaultState);
    h.Register("sceGnmDispatchInitDefaultHardwareState", InitDefaultState);

    h.Register("sceGnmInsertWaitFlipDone", [](const uint64_t* a) -> uint64_t { return FillNop(a, Marker::kWaitFlipDone); });
    h.Register("sceGnmInsertPushMarker", [](const uint64_t* a) -> uint64_t { return FillMarker(a, 1, Ptr<const char>(a[2])); });
    h.Register("sceGnmInsertPushColorMarker", [](const uint64_t* a) -> uint64_t { return FillMarker(a, 1, Ptr<const char>(a[2])); });
    h.Register("sceGnmInsertPopMarker", [](const uint64_t* a) -> uint64_t { return FillMarker(a, 0, nullptr); });
    h.Register("sceGnmInsertSetMarker", [](const uint64_t* a) -> uint64_t { return FillMarker(a, 2, Ptr<const char>(a[2])); });
    h.Register("sceGnmInsertSetColorMarker", [](const uint64_t* a) -> uint64_t { return FillMarker(a, 2, Ptr<const char>(a[2])); });
    for (const char* n : {"sceGnmInsertThreadTraceMarker", "sceGnmRequestMipStatsReportAndReset"})
        h.Register(n, [](const uint64_t* a) -> uint64_t { return FillNop(a, Marker::kDebugMarker); });

    // int sceGnmSubmitCommandBuffers(u32 count, void** dcb, u32* dcbSizes, void** ccb, u32* ccbSizes)
    h.Register("sceGnmSubmitCommandBuffers", [](const uint64_t* a) -> uint64_t {
        Submit(BuildSubmission(a));
        return 0;
    });
    h.Register("sceGnmSubmitCommandBuffersForWorkload", [](const uint64_t* a) -> uint64_t {
        // (workload, count, dcb, dcbSizes, ccb, ccbSizes)
        const uint64_t shifted[6] = {a[1], a[2], a[3], a[4], a[5], 0};
        Submit(BuildSubmission(shifted));
        return 0;
    });
    // (..., u32 videoOutHandle, u32 displayBufferIndex, u32 flipMode, i64 flipArg)
    h.Register("sceGnmSubmitAndFlipCommandBuffers", [](const uint64_t* a) -> uint64_t {
        Submission s = BuildSubmission(a);
        s.flip = true;
        s.vo_handle = uint32_t(a[5]);
        s.buffer_index = uint32_t(a[6]);
        s.flip_mode = uint32_t(a[7]);
        s.flip_arg = int64_t(a[9]);
        Submit(std::move(s));
        return 0;
    });
    // (videoOutHandle, displayBufferIndex, flipMode, flipArg)
    h.Register("sceGnmRequestFlipAndSubmitDone", [](const uint64_t* a) -> uint64_t {
        SubmitFlip(int32_t(a[1]), int64_t(a[3]));
        return 0;
    });
    h.Register("sceGnmSubmitDone", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceGnmFlushGarlic", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceGnmAreSubmitsAllowed", [](const uint64_t*) -> uint64_t { return 1; });
    h.Register("sceGnmIsUserPaEnabled", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceGnmDebugHardwareStatus", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceGnmValidateCommandBuffers", [](const uint64_t*) -> uint64_t { return 0; });

    // int sceGnmAddEqEvent(SceKernelEqueue eq, u64 id, void* udata)
    h.Register("sceGnmAddEqEvent", [](const uint64_t* a) -> uint64_t {
        if (!a[0]) return kernel::SceError(kernel::kEBADF);
        AddEopEvent(Ptr<kernel::EventQueue>(a[0]), a[1], a[2]);
        return 0;
    });
    h.Register("sceGnmDeleteEqEvent", [](const uint64_t* a) -> uint64_t {
        if (a[0]) RemoveEopEvent(Ptr<kernel::EventQueue>(a[0]), a[1]);
        return 0;
    });
    h.Register("sceGnmGetEqEventType", [](const uint64_t* a) -> uint64_t { return Load<uint64_t>(a[0]); });
    h.Register("sceGnmGetEqTimeStamp", [](const uint64_t*) -> uint64_t { return GpuTimestamp(); });

    // u32 sceGnmMapComputeQueue(u32 pipe, u32 queue, void* ringBase, u32 ringSizeDw, void* readPtr)
    h.Register("sceGnmMapComputeQueue", [](const uint64_t* a) -> uint64_t {
        return MapComputeQueue(uint32_t(a[0]), uint32_t(a[1]), a[2], uint32_t(a[3]), a[4]);
    });
    h.Register("sceGnmMapComputeQueueWithPriority", [](const uint64_t* a) -> uint64_t {
        return MapComputeQueue(uint32_t(a[0]), uint32_t(a[1]), a[2], uint32_t(a[3]), a[4]);
    });
    h.Register("sceGnmUnmapComputeQueue", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceGnmDingDong", [](const uint64_t* a) -> uint64_t {
        DingDong(uint32_t(a[0]), uint32_t(a[1]));
        return 0;
    });
    h.Register("sceGnmDingDongForWorkload", [](const uint64_t* a) -> uint64_t {
        DingDong(uint32_t(a[0]), uint32_t(a[1]));
        return 0;
    });

    h.Register("sceGnmLogicalCuIndexToPhysicalCuIndex", [](const uint64_t* a) -> uint64_t { return a[0]; });
    h.Register("sceGnmLogicalCuMaskToPhysicalCuMask", [](const uint64_t* a) -> uint64_t { return a[0]; });
    h.Register("sceGnmSetGsRingSizes", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceGnmGetTheTessellationFactorRingBufferBaseAddress", [](const uint64_t*) -> uint64_t {
        std::call_once(g_tess_once, [] {
            g_tess_ring = GuestVirtualAlloc(0, 0x20000, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        });
        return g_tess_ring;
    });
    for (const char* n : {"sceGnmSetupMipStatsReport", "sceGnmDisableMipStatsReport", "sceGnmDriverCaptureInProgress",
                          "sceGnmDriverTriggerCapture", "sceRazorIsLoaded"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });
}

} // namespace gr::gpu
