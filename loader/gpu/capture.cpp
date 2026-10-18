#include "gpu/capture.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

#include "gpu/pm4.h"
#include "guest.h"

namespace fs = std::filesystem;

namespace gr::gpu {
namespace {

std::mutex g_cap_m;
fs::path g_dir;
int64_t g_log_submission = -1;
uint64_t g_cur_submission = 0;
FILE* g_log = nullptr;
uint32_t g_draw_in_sub = 0;
std::unordered_set<uint64_t> g_dumped;

uint32_t Ctx(const uint32_t* ctx, uint32_t reg) { return ctx[reg - kContextRegBase]; }
uint32_t Sh(const uint32_t* sh, uint32_t reg) { return sh[reg - kShRegBase]; }

bool CopyGuest(void* dst, uint64_t src, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t PgmAddr(const uint32_t* sh, uint32_t lo_reg) {
    return (uint64_t(Sh(sh, lo_reg)) << 8) | (uint64_t(Sh(sh, lo_reg + 1) & 0xFF) << 40);
}

bool Readable(uint64_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!addr || !VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return addr + n <= reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void Dump(const char* stage, uint64_t addr) {
    if (!addr || g_dir.empty() || g_dumped.count(addr)) return;
    g_dumped.insert(addr);
    if (!Readable(addr, 16)) return;
    const uint32_t size = ShaderCodeSize(reinterpret_cast<const uint32_t*>(addr));
    if (!size) return;
    char name[64];
    std::snprintf(name, sizeof name, "%s_%010llx.bin", stage, (unsigned long long)addr);
    std::error_code ec;
    fs::create_directories(g_dir / "shaders", ec);
    std::ofstream(g_dir / "shaders" / name, std::ios::binary).write(reinterpret_cast<const char*>(addr), size);
}

} // namespace

uint32_t ShaderCodeSize(const uint32_t* code, uint32_t max_dwords) {
    // Walk instructions so literals and 64-bit encodings are skipped correctly.
    uint32_t i = 0;
    while (i < max_dwords) {
        const uint32_t w = code[i];
        if (w == 0xBF810000u) return (i + 1) * 4;                // s_endpgm
        if ((w & 0xFFFFFF00u) == 0xBE802000u) return (i + 1) * 4; // s_setpc_b64 (fetch subroutine return)
        uint32_t len = 1;
        const uint32_t top2 = w >> 30, top5 = w >> 27, top6 = w >> 26;
        bool literal = false;
        if (top2 == 2) { // scalar ALU: SOP2/SOPK/SOP1/SOPC/SOPP
            const uint32_t top9 = w >> 23;
            if ((w >> 28) == 0xB) {                                  // SOPK
                if (((w >> 23) & 0x1F) == 0x15) len = 2;             // s_setreg_imm32_b32
            } else if (top9 == 0x17D || top9 == 0x17E) {             // SOP1 / SOPC
                literal = (w & 0xFF) == 0xFF || (top9 == 0x17E && ((w >> 8) & 0xFF) == 0xFF);
            } else if (top9 != 0x17F) {                              // SOP2
                literal = (w & 0xFF) == 0xFF || ((w >> 8) & 0xFF) == 0xFF;
            }
        } else if (top5 == 0x18) {                                   // SMRD
            if (!((w >> 8) & 1) && (w & 0xFF) == 0xFF) len = 2;      // literal offset (CI)
        } else if (top6 == 0x34 || top6 == 0x36 || top6 == 0x38 || top6 == 0x3A || top6 == 0x3C ||
                   top6 == 0x3E || top6 == 0x31) {                   // VOP3 DS MUBUF MTBUF MIMG EXP (64-bit)
            len = 2;
        } else if (top6 == 0x32) {                                   // VINTRP
            len = 1;
        } else if ((w >> 31) == 0) {                                 // VOP2 / VOP1 / VOPC
            literal = (w & 0x1FF) == 0xFF;
            const uint32_t op = (w >> 25) & 0x3F;
            if ((op == 0x20 || op == 0x21) && (w >> 25) != 0x3F && (w >> 25) != 0x3E) literal = true; // v_madmk/v_madak
        }
        i += len + (literal ? 1 : 0);
    }
    return 0;
}

void CaptureConfigure(const std::string& dir, int64_t submission_to_log) {
    std::lock_guard<std::mutex> lk(g_cap_m);
    g_dir = dir;
    g_log_submission = submission_to_log;
}

void CaptureBeginSubmission(uint64_t submission) {
    std::lock_guard<std::mutex> lk(g_cap_m);
    g_cur_submission = submission;
    g_draw_in_sub = 0;
    if (g_log && int64_t(submission) != g_log_submission) {
        std::fclose(g_log);
        g_log = nullptr;
        TraceNote("capture: draw log written to %s", (g_dir / "draws.txt").string().c_str());
    }
    if (!g_log && int64_t(submission) == g_log_submission && !g_dir.empty()) {
        std::error_code ec;
        fs::create_directories(g_dir, ec);
        g_log = std::fopen((g_dir / "draws.txt").string().c_str(), "w");
    }
}

void CaptureDraw(const DrawInfo& d, const uint32_t* ctx, const uint32_t* sh, const uint32_t* uc) {
    std::lock_guard<std::mutex> lk(g_cap_m);
    if (g_dir.empty()) return;
    const uint64_t vs = PgmAddr(sh, reg::SPI_SHADER_PGM_LO_VS), ps = PgmAddr(sh, reg::SPI_SHADER_PGM_LO_PS);
    Dump("vs", vs);
    Dump("ps", ps);
    // Vertex fetch subroutine: VS user SGPRs s[0:1] hold its address when the VS calls s_swappc_b64.
    Dump("fs", uint64_t(Sh(sh, 0x2C4C)) | (uint64_t(Sh(sh, 0x2C4D)) << 32));
    // Geometry pipeline (VGT_SHADER_STAGES_EN: ES_EN bits 0-1, GS_EN bit 7): export and geometry shaders
    const uint32_t stages = Ctx(ctx, 0xA2D5);
    const bool esgs = (stages & 3) != 0 || (stages & 0x80) != 0;
    if (esgs) {
        Dump("es", PgmAddr(sh, reg::SPI_SHADER_PGM_LO_ES));
        Dump("gs", PgmAddr(sh, reg::SPI_SHADER_PGM_LO_GS));
        Dump("fs", uint64_t(Sh(sh, 0x2CCC)) | (uint64_t(Sh(sh, 0x2CCD)) << 32)); // ES fetch subroutine
    }
    if (!g_log) return;
    FILE* f = g_log;
    const uint32_t n = g_draw_in_sub++;
    std::fprintf(f, "draw %u op=0x%02x count=%u ibase=0x%llx itype=%u inst=%u prim=%u\n", n, d.opcode, d.index_count,
                 (unsigned long long)d.index_base, d.index_type, d.num_instances, uc[0x242]);
    if (esgs) {
        std::fprintf(f, "  STAGES %08x  ES 0x%llx rsrc1=%08x rsrc2=%08x  GS 0x%llx rsrc1=%08x rsrc2=%08x\n", stages,
                     (unsigned long long)PgmAddr(sh, reg::SPI_SHADER_PGM_LO_ES), Sh(sh, 0x2CCA), Sh(sh, 0x2CCB),
                     (unsigned long long)PgmAddr(sh, reg::SPI_SHADER_PGM_LO_GS), Sh(sh, 0x2C8A), Sh(sh, 0x2C8B));
        std::fprintf(f, "  ES user:");
        for (uint32_t i = 0; i < 16; ++i) std::fprintf(f, " %08x", Sh(sh, 0x2CCC + i));
        std::fprintf(f, "\n  GS user:");
        for (uint32_t i = 0; i < 16; ++i) std::fprintf(f, " %08x", Sh(sh, 0x2C8C + i));
        // VGT_GS_MODE, VGT_GS_OUT_PRIM_TYPE, VGT_GS_MAX_VERT_OUT, VGT_GSVS_RING_OFFSET_1..3, VGT_ESGS_RING_ITEMSIZE,
        // VGT_GSVS_RING_ITEMSIZE, VGT_GS_VERT_ITEMSIZE, VGT_GS_INSTANCE_CNT, VGT_STRMOUT_CONFIG; uconfig ring sizes
        std::fprintf(f, "\n  gs_mode=%08x out_prim=%08x max_vert=%08x gsvs_off=%08x/%08x/%08x esgs_item=%08x gsvs_item=%08x "
                        "vert_item=%08x/%08x/%08x/%08x inst=%08x esgs_ring=%08x gsvs_ring=%08x\n",
                     Ctx(ctx, 0xA290), Ctx(ctx, 0xA29B), Ctx(ctx, 0xA2CE), Ctx(ctx, 0xA298), Ctx(ctx, 0xA299),
                     Ctx(ctx, 0xA29A), Ctx(ctx, 0xA2AB), Ctx(ctx, 0xA2AC), Ctx(ctx, 0xA2D7), Ctx(ctx, 0xA2D8),
                     Ctx(ctx, 0xA2D9), Ctx(ctx, 0xA2DA), Ctx(ctx, 0xA2E4), uc[0x240 - 0x0], uc[0x241 - 0x0]);
        // descriptor table the GS uses for its rings (GS user s[0:1]): ESGS read V# at +0xc, GSVS write V# at +0x10
        const uint64_t tbl = uint64_t(Sh(sh, 0x2C8C)) | (uint64_t(Sh(sh, 0x2C8D)) << 32);
        if (tbl) {
            std::fprintf(f, "  ring table 0x%llx:", (unsigned long long)tbl);
            uint32_t w[0x28] = {};
            if (CopyGuest(w, tbl, sizeof w))
                for (uint32_t i = 0; i < 0x28; ++i) std::fprintf(f, "%s%08x", (i % 4) ? " " : " | ", w[i]);
            else
                std::fprintf(f, " (unreadable)");
            std::fprintf(f, "\n");
        }
    }
    std::fprintf(f, "  VS 0x%llx rsrc1=%08x rsrc2=%08x  PS 0x%llx rsrc1=%08x rsrc2=%08x\n", (unsigned long long)vs,
                 Sh(sh, 0x2C4A), Sh(sh, 0x2C4B), (unsigned long long)ps, Sh(sh, 0x2C0A), Sh(sh, 0x2C0B));
    std::fprintf(f, "  VS user:");
    for (uint32_t i = 0; i < 16; ++i) std::fprintf(f, " %08x", Sh(sh, 0x2C4C + i));
    std::fprintf(f, "\n  PS user:");
    for (uint32_t i = 0; i < 16; ++i) std::fprintf(f, " %08x", Sh(sh, 0x2C0C + i));
    std::fprintf(f, "\n  CB0 base=%08x pitch=%08x slice=%08x view=%08x info=%08x attrib=%08x  target_mask=%08x\n",
                 Ctx(ctx, 0xA318), Ctx(ctx, 0xA319), Ctx(ctx, 0xA31A), Ctx(ctx, 0xA31B), Ctx(ctx, 0xA31C),
                 Ctx(ctx, 0xA31D), Ctx(ctx, 0xA08E));
    std::fprintf(f, "  DB z_info=%08x zread=%08x zwrite=%08x size=%08x depth_ctl=%08x\n", Ctx(ctx, 0xA010),
                 Ctx(ctx, 0xA012), Ctx(ctx, 0xA014), Ctx(ctx, 0xA016), Ctx(ctx, 0xA200));
    float vp[6];
    std::memcpy(vp, ctx + (0xA10F - kContextRegBase), sizeof vp);
    std::fprintf(f, "  viewport scale=(%g,%g,%g) offset=(%g,%g,%g) scissor=%08x-%08x blend0=%08x color_ctl=%08x\n",
                 vp[0], vp[2], vp[4], vp[1], vp[3], vp[5], Ctx(ctx, 0xA00C), Ctx(ctx, 0xA00D), Ctx(ctx, 0xA1E0),
                 Ctx(ctx, 0xA202));
    std::fprintf(f, "  spi: vs_out=%08x pos_fmt=%08x ps_in_ena=%08x ps_in_addr=%08x col_fmt=%08x z_fmt=%08x\n",
                 Ctx(ctx, 0xA1B1), Ctx(ctx, 0xA1C3), Ctx(ctx, 0xA1B3), Ctx(ctx, 0xA1B4), Ctx(ctx, 0xA1C5),
                 Ctx(ctx, 0xA1C4));
}

void CaptureDispatch(uint32_t x, uint32_t y, uint32_t z, const uint32_t* sh) {
    std::lock_guard<std::mutex> lk(g_cap_m);
    if (g_dir.empty()) return;
    const uint64_t cs = PgmAddr(sh, reg::COMPUTE_PGM_LO);
    Dump("cs", cs);
    if (!g_log) return;
    std::fprintf(g_log, "dispatch %ux%ux%u  CS 0x%llx rsrc1=%08x rsrc2=%08x threads=%ux%ux%u\n  CS user:", x, y, z,
                 (unsigned long long)cs, Sh(sh, 0x2E12), Sh(sh, 0x2E13), Sh(sh, 0x2E07), Sh(sh, 0x2E08), Sh(sh, 0x2E09));
    for (uint32_t i = 0; i < 16; ++i) std::fprintf(g_log, " %08x", Sh(sh, 0x2E40 + i));
    std::fprintf(g_log, "\n");
}

} // namespace gr::gpu
