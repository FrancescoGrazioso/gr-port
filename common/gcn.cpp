#include "gcn.h"

#include <cstdio>
#include <unordered_map>

namespace gr::gcn {
namespace {

uint32_t Bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

bool IsVop3b(uint16_t op) { return (op >= 0x125 && op <= 0x12A) || op == 0x16D || op == 0x16E; }

using NameMap = std::unordered_map<uint16_t, const char*>;

const NameMap& Sop2Names() {
    static const NameMap m = {
        {0, "s_add_u32"}, {1, "s_sub_u32"}, {2, "s_add_i32"}, {3, "s_sub_i32"}, {4, "s_addc_u32"}, {5, "s_subb_u32"},
        {6, "s_min_i32"}, {7, "s_min_u32"}, {8, "s_max_i32"}, {9, "s_max_u32"}, {10, "s_cselect_b32"},
        {11, "s_cselect_b64"}, {14, "s_and_b32"}, {15, "s_and_b64"}, {16, "s_or_b32"}, {17, "s_or_b64"},
        {18, "s_xor_b32"}, {19, "s_xor_b64"}, {20, "s_andn2_b32"}, {21, "s_andn2_b64"}, {22, "s_orn2_b32"},
        {23, "s_orn2_b64"}, {24, "s_nand_b32"}, {25, "s_nand_b64"}, {26, "s_nor_b32"}, {27, "s_nor_b64"},
        {28, "s_xnor_b32"}, {29, "s_xnor_b64"}, {30, "s_lshl_b32"}, {31, "s_lshl_b64"}, {32, "s_lshr_b32"},
        {33, "s_lshr_b64"}, {34, "s_ashr_i32"}, {35, "s_ashr_i64"}, {36, "s_bfm_b32"}, {37, "s_bfm_b64"},
        {38, "s_mul_i32"}, {39, "s_bfe_u32"}, {40, "s_bfe_i32"}, {41, "s_bfe_u64"}, {42, "s_bfe_i64"},
        {43, "s_cbranch_g_fork"}, {44, "s_absdiff_i32"}};
    return m;
}
const NameMap& SopkNames() {
    static const NameMap m = {{0, "s_movk_i32"}, {2, "s_cmovk_i32"}, {3, "s_cmpk_eq_i32"}, {4, "s_cmpk_lg_i32"},
                              {5, "s_cmpk_gt_i32"}, {6, "s_cmpk_ge_i32"}, {7, "s_cmpk_lt_i32"}, {8, "s_cmpk_le_i32"},
                              {9, "s_cmpk_eq_u32"}, {10, "s_cmpk_lg_u32"}, {11, "s_cmpk_gt_u32"}, {12, "s_cmpk_ge_u32"},
                              {13, "s_cmpk_lt_u32"}, {14, "s_cmpk_le_u32"}, {15, "s_addk_i32"}, {16, "s_mulk_i32"},
                              {17, "s_cbranch_i_fork"}, {18, "s_getreg_b32"}, {19, "s_setreg_b32"},
                              {21, "s_setreg_imm32_b32"}};
    return m;
}
const NameMap& Sop1Names() {
    static const NameMap m = {
        {3, "s_mov_b32"}, {4, "s_mov_b64"}, {5, "s_cmov_b32"}, {6, "s_cmov_b64"}, {7, "s_not_b32"}, {8, "s_not_b64"},
        {9, "s_wqm_b32"}, {10, "s_wqm_b64"}, {11, "s_brev_b32"}, {12, "s_brev_b64"}, {13, "s_bcnt0_i32_b32"},
        {14, "s_bcnt0_i32_b64"}, {15, "s_bcnt1_i32_b32"}, {16, "s_bcnt1_i32_b64"}, {17, "s_ff0_i32_b32"},
        {18, "s_ff0_i32_b64"}, {19, "s_ff1_i32_b32"}, {20, "s_ff1_i32_b64"}, {21, "s_flbit_i32_b32"},
        {22, "s_flbit_i32_b64"}, {23, "s_flbit_i32"}, {24, "s_flbit_i32_i64"}, {25, "s_sext_i32_i8"},
        {26, "s_sext_i32_i16"}, {27, "s_bitset0_b32"}, {28, "s_bitset0_b64"}, {29, "s_bitset1_b32"},
        {30, "s_bitset1_b64"}, {31, "s_getpc_b64"}, {32, "s_setpc_b64"}, {33, "s_swappc_b64"}, {34, "s_rfe_b64"},
        {36, "s_and_saveexec_b64"}, {37, "s_or_saveexec_b64"}, {38, "s_xor_saveexec_b64"},
        {39, "s_andn2_saveexec_b64"}, {40, "s_orn2_saveexec_b64"}, {41, "s_nand_saveexec_b64"},
        {42, "s_nor_saveexec_b64"}, {43, "s_xnor_saveexec_b64"}, {44, "s_quadmask_b32"}, {45, "s_quadmask_b64"},
        {46, "s_movrels_b32"}, {47, "s_movrels_b64"}, {48, "s_movreld_b32"}, {49, "s_movreld_b64"},
        {50, "s_cbranch_join"}, {52, "s_abs_i32"}, {53, "s_mov_fed_b32"}};
    return m;
}
const NameMap& SopcNames() {
    static const NameMap m = {{0, "s_cmp_eq_i32"}, {1, "s_cmp_lg_i32"}, {2, "s_cmp_gt_i32"}, {3, "s_cmp_ge_i32"},
                              {4, "s_cmp_lt_i32"}, {5, "s_cmp_le_i32"}, {6, "s_cmp_eq_u32"}, {7, "s_cmp_lg_u32"},
                              {8, "s_cmp_gt_u32"}, {9, "s_cmp_ge_u32"}, {10, "s_cmp_lt_u32"}, {11, "s_cmp_le_u32"},
                              {12, "s_bitcmp0_b32"}, {13, "s_bitcmp1_b32"}, {14, "s_bitcmp0_b64"}, {15, "s_bitcmp1_b64"},
                              {16, "s_setvskip"}};
    return m;
}
const NameMap& SoppNames() {
    static const NameMap m = {{0, "s_nop"}, {1, "s_endpgm"}, {2, "s_branch"}, {4, "s_cbranch_scc0"},
                              {5, "s_cbranch_scc1"}, {6, "s_cbranch_vccz"}, {7, "s_cbranch_vccnz"},
                              {8, "s_cbranch_execz"}, {9, "s_cbranch_execnz"}, {10, "s_barrier"}, {12, "s_waitcnt"},
                              {13, "s_sethalt"}, {14, "s_sleep"}, {15, "s_setprio"}, {16, "s_sendmsg"},
                              {17, "s_sendmsghalt"}, {18, "s_trap"}, {19, "s_icache_inv"}, {20, "s_incperflevel"},
                              {21, "s_decperflevel"}, {22, "s_ttracedata"}};
    return m;
}
const NameMap& SmrdNames() {
    static const NameMap m = {{0, "s_load_dword"},         {1, "s_load_dwordx2"},        {2, "s_load_dwordx4"},
                              {3, "s_load_dwordx8"},       {4, "s_load_dwordx16"},       {8, "s_buffer_load_dword"},
                              {9, "s_buffer_load_dwordx2"}, {10, "s_buffer_load_dwordx4"}, {11, "s_buffer_load_dwordx8"},
                              {12, "s_buffer_load_dwordx16"}, {30, "s_memtime"},          {31, "s_dcache_inv"}};
    return m;
}
const NameMap& Vop2Names() {
    static const NameMap m = {
        {0, "v_cndmask_b32"}, {1, "v_readlane_b32"}, {2, "v_writelane_b32"}, {3, "v_add_f32"}, {4, "v_sub_f32"},
        {5, "v_subrev_f32"}, {6, "v_mac_legacy_f32"}, {7, "v_mul_legacy_f32"}, {8, "v_mul_f32"},
        {9, "v_mul_i32_i24"}, {10, "v_mul_hi_i32_i24"}, {11, "v_mul_u32_u24"}, {12, "v_mul_hi_u32_u24"},
        {13, "v_min_legacy_f32"}, {14, "v_max_legacy_f32"}, {15, "v_min_f32"}, {16, "v_max_f32"}, {17, "v_min_i32"},
        {18, "v_max_i32"}, {19, "v_min_u32"}, {20, "v_max_u32"}, {21, "v_lshr_b32"}, {22, "v_lshrrev_b32"},
        {23, "v_ashr_i32"}, {24, "v_ashrrev_i32"}, {25, "v_lshl_b32"}, {26, "v_lshlrev_b32"}, {27, "v_and_b32"},
        {28, "v_or_b32"}, {29, "v_xor_b32"}, {30, "v_bfm_b32"}, {31, "v_mac_f32"}, {32, "v_madmk_f32"},
        {33, "v_madak_f32"}, {34, "v_bcnt_u32_b32"}, {35, "v_mbcnt_lo_u32_b32"}, {36, "v_mbcnt_hi_u32_b32"},
        {37, "v_add_i32"}, {38, "v_sub_i32"}, {39, "v_subrev_i32"}, {40, "v_addc_u32"}, {41, "v_subb_u32"},
        {42, "v_subbrev_u32"}, {43, "v_ldexp_f32"}, {44, "v_cvt_pkaccum_u8_f32"}, {45, "v_cvt_pknorm_i16_f32"},
        {46, "v_cvt_pknorm_u16_f32"}, {47, "v_cvt_pkrtz_f16_f32"}, {48, "v_cvt_pk_u16_u32"}, {49, "v_cvt_pk_i16_i32"}};
    return m;
}
const NameMap& Vop1Names() {
    static const NameMap m = {
        {0, "v_nop"}, {1, "v_mov_b32"}, {2, "v_readfirstlane_b32"}, {3, "v_cvt_i32_f64"}, {4, "v_cvt_f64_i32"},
        {5, "v_cvt_f32_i32"}, {6, "v_cvt_f32_u32"}, {7, "v_cvt_u32_f32"}, {8, "v_cvt_i32_f32"}, {9, "v_mov_fed_b32"},
        {10, "v_cvt_f16_f32"}, {11, "v_cvt_f32_f16"}, {12, "v_cvt_rpi_i32_f32"}, {13, "v_cvt_flr_i32_f32"},
        {14, "v_cvt_off_f32_i4"}, {15, "v_cvt_f32_f64"}, {16, "v_cvt_f64_f32"}, {17, "v_cvt_f32_ubyte0"},
        {18, "v_cvt_f32_ubyte1"}, {19, "v_cvt_f32_ubyte2"}, {20, "v_cvt_f32_ubyte3"}, {21, "v_cvt_u32_f64"},
        {22, "v_cvt_f64_u32"}, {23, "v_trunc_f64"}, {24, "v_ceil_f64"}, {25, "v_rndne_f64"}, {26, "v_floor_f64"},
        {32, "v_fract_f32"}, {33, "v_trunc_f32"}, {34, "v_ceil_f32"}, {35, "v_rndne_f32"}, {36, "v_floor_f32"},
        {37, "v_exp_f32"}, {38, "v_log_clamp_f32"}, {39, "v_log_f32"}, {40, "v_rcp_clamp_f32"},
        {41, "v_rcp_legacy_f32"}, {42, "v_rcp_f32"}, {43, "v_rcp_iflag_f32"}, {44, "v_rsq_clamp_f32"},
        {45, "v_rsq_legacy_f32"}, {46, "v_rsq_f32"}, {47, "v_rcp_f64"}, {48, "v_rcp_clamp_f64"}, {49, "v_rsq_f64"},
        {50, "v_rsq_clamp_f64"}, {51, "v_sqrt_f32"}, {52, "v_sqrt_f64"}, {53, "v_sin_f32"}, {54, "v_cos_f32"},
        {55, "v_not_b32"}, {56, "v_bfrev_b32"}, {57, "v_ffbh_u32"}, {58, "v_ffbl_b32"}, {59, "v_ffbh_i32"},
        {60, "v_frexp_exp_i32_f64"}, {61, "v_frexp_mant_f64"}, {62, "v_fract_f64"}, {63, "v_frexp_exp_i32_f32"},
        {64, "v_frexp_mant_f32"}, {65, "v_clrexcp"}, {66, "v_movreld_b32"}, {67, "v_movrels_b32"},
        {68, "v_movrelsd_b32"}, {69, "v_log_legacy_f32"}, {70, "v_exp_legacy_f32"}};
    return m;
}
const char* VopcName(uint16_t op) {
    static const char* f32[16] = {"f", "lt", "eq", "le", "gt", "lg", "ge", "o", "u", "nge", "nlg", "ngt", "nle", "neq", "nlt", "tru"};
    static const char* i[8] = {"f", "lt", "eq", "le", "gt", "ne", "ge", "t"};
    static char buf[64];
    const uint16_t group = op >> 4, cond = op & 15;
    switch (group) {
    case 0x0: std::snprintf(buf, sizeof buf, "v_cmp_%s_f32", f32[cond]); return buf;
    case 0x1: std::snprintf(buf, sizeof buf, "v_cmpx_%s_f32", f32[cond]); return buf;
    case 0x2: std::snprintf(buf, sizeof buf, "v_cmp_%s_f64", f32[cond]); return buf;
    case 0x3: std::snprintf(buf, sizeof buf, "v_cmpx_%s_f64", f32[cond]); return buf;
    case 0x4: std::snprintf(buf, sizeof buf, "v_cmps_%s_f32", f32[cond]); return buf;
    case 0x5: std::snprintf(buf, sizeof buf, "v_cmpsx_%s_f32", f32[cond]); return buf;
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF: {
        if (cond == 8) { std::snprintf(buf, sizeof buf, group & 1 ? "v_cmpx_class_%s" : "v_cmp_class_%s", group < 0xA ? "f32" : "f64"); return buf; }
        if (cond > 8) return "?";
        static const char* types[4] = {"i32", "i64", "u32", "u64"};
        std::snprintf(buf, sizeof buf, "v_cmp%s_%s_%s", (group & 1) ? "x" : "", i[cond], types[(group - 8) >> 1]);
        return buf;
    }
    default: return "?";
    }
}
const NameMap& Vop3OnlyNames() {
    static const NameMap m = {
        {0x140, "v_mad_legacy_f32"}, {0x141, "v_mad_f32"}, {0x142, "v_mad_i32_i24"}, {0x143, "v_mad_u32_u24"},
        {0x144, "v_cubeid_f32"}, {0x145, "v_cubesc_f32"}, {0x146, "v_cubetc_f32"}, {0x147, "v_cubema_f32"},
        {0x148, "v_bfe_u32"}, {0x149, "v_bfe_i32"}, {0x14A, "v_bfi_b32"}, {0x14B, "v_fma_f32"}, {0x14C, "v_fma_f64"},
        {0x14D, "v_lerp_u8"}, {0x14E, "v_alignbit_b32"}, {0x14F, "v_alignbyte_b32"}, {0x150, "v_mullit_f32"},
        {0x151, "v_min3_f32"}, {0x152, "v_min3_i32"}, {0x153, "v_min3_u32"}, {0x154, "v_max3_f32"},
        {0x155, "v_max3_i32"}, {0x156, "v_max3_u32"}, {0x157, "v_med3_f32"}, {0x158, "v_med3_i32"},
        {0x159, "v_med3_u32"}, {0x15A, "v_sad_u8"}, {0x15B, "v_sad_hi_u8"}, {0x15C, "v_sad_u16"}, {0x15D, "v_sad_u32"},
        {0x15E, "v_cvt_pk_u8_f32"}, {0x15F, "v_div_fixup_f32"}, {0x160, "v_div_fixup_f64"}, {0x161, "v_lshl_b64"},
        {0x162, "v_lshr_b64"}, {0x163, "v_ashr_i64"}, {0x164, "v_add_f64"}, {0x165, "v_mul_f64"}, {0x166, "v_min_f64"},
        {0x167, "v_max_f64"}, {0x168, "v_ldexp_f64"}, {0x169, "v_mul_lo_u32"}, {0x16A, "v_mul_hi_u32"},
        {0x16B, "v_mul_lo_i32"}, {0x16C, "v_mul_hi_i32"}, {0x16D, "v_div_scale_f32"}, {0x16E, "v_div_scale_f64"},
        {0x16F, "v_div_fmas_f32"}, {0x170, "v_div_fmas_f64"}, {0x171, "v_msad_u8"}, {0x172, "v_qsad_u8"},
        {0x173, "v_mqsad_u8"}, {0x174, "v_trig_preop_f64"}, {0x175, "v_mqsad_u32_u8"}, {0x176, "v_mad_u64_u32"},
        {0x177, "v_mad_i64_i32"}};
    return m;
}
const NameMap& MubufNames() {
    static const NameMap m = {
        {0, "buffer_load_format_x"}, {1, "buffer_load_format_xy"}, {2, "buffer_load_format_xyz"},
        {3, "buffer_load_format_xyzw"}, {4, "buffer_store_format_x"}, {5, "buffer_store_format_xy"},
        {6, "buffer_store_format_xyz"}, {7, "buffer_store_format_xyzw"}, {8, "buffer_load_ubyte"},
        {9, "buffer_load_sbyte"}, {10, "buffer_load_ushort"}, {11, "buffer_load_sshort"}, {12, "buffer_load_dword"},
        {13, "buffer_load_dwordx2"}, {14, "buffer_load_dwordx4"}, {15, "buffer_load_dwordx3"},
        {24, "buffer_store_byte"}, {26, "buffer_store_short"}, {28, "buffer_store_dword"},
        {29, "buffer_store_dwordx2"}, {30, "buffer_store_dwordx4"}, {31, "buffer_store_dwordx3"}};
    return m;
}
const NameMap& MimgNames() {
    static const NameMap m = {
        {0x00, "image_load"}, {0x01, "image_load_mip"}, {0x08, "image_store"}, {0x09, "image_store_mip"},
        {0x0E, "image_get_resinfo"}, {0x20, "image_sample"}, {0x21, "image_sample_cl"}, {0x22, "image_sample_d"},
        {0x23, "image_sample_d_cl"}, {0x24, "image_sample_l"}, {0x25, "image_sample_b"}, {0x26, "image_sample_b_cl"},
        {0x27, "image_sample_lz"}, {0x28, "image_sample_c"}, {0x29, "image_sample_c_cl"}, {0x2A, "image_sample_c_d"},
        {0x2C, "image_sample_c_l"}, {0x2D, "image_sample_c_b"}, {0x2F, "image_sample_c_lz"}, {0x30, "image_sample_o"},
        {0x34, "image_sample_l_o"}, {0x35, "image_sample_b_o"}, {0x37, "image_sample_lz_o"}, {0x38, "image_sample_c_o"},
        {0x3C, "image_sample_c_l_o"}, {0x3F, "image_sample_c_lz_o"}, {0x40, "image_gather4"},
        {0x44, "image_gather4_l"}, {0x47, "image_gather4_lz"}, {0x48, "image_gather4_c"}, {0x4F, "image_gather4_c_lz"},
        {0x60, "image_get_lod"}};
    return m;
}

const char* Lookup(const NameMap& m, uint16_t op) {
    auto it = m.find(op);
    return it == m.end() ? "?" : it->second;
}

} // namespace

std::vector<Instr> Decode(const uint32_t* code, uint32_t max_bytes) {
    std::vector<Instr> out;
    uint32_t i = 0;
    const uint32_t max_dw = max_bytes / 4;
    while (i < max_dw) {
        Instr in;
        in.pc = i * 4;
        const uint32_t w = code[i];
        in.w0 = w;
        uint32_t len = 1;
        const uint32_t top9 = w >> 23;
        if (top9 == 0x17F) { // SOPP
            in.fmt = Fmt::SOPP;
            in.op = uint16_t(Bits(w, 22, 16));
            in.simm16 = int16_t(w & 0xFFFF);
        } else if (top9 == 0x17E) { // SOPC
            in.fmt = Fmt::SOPC;
            in.op = uint16_t(Bits(w, 22, 16));
            in.src0 = uint16_t(Bits(w, 7, 0));
            in.src1 = uint16_t(Bits(w, 15, 8));
        } else if (top9 == 0x17D) { // SOP1
            in.fmt = Fmt::SOP1;
            in.op = uint16_t(Bits(w, 15, 8));
            in.dst = uint16_t(Bits(w, 22, 16));
            in.src0 = uint16_t(Bits(w, 7, 0));
        } else if ((w >> 28) == 0xB) { // SOPK
            in.fmt = Fmt::SOPK;
            in.op = uint16_t(Bits(w, 27, 23));
            in.dst = uint16_t(Bits(w, 22, 16));
            in.simm16 = int16_t(w & 0xFFFF);
            if (in.op == 21) { in.has_literal = true; in.literal = code[i + 1]; len = 2; }
        } else if ((w >> 30) == 2) { // SOP2
            in.fmt = Fmt::SOP2;
            in.op = uint16_t(Bits(w, 29, 23));
            in.dst = uint16_t(Bits(w, 22, 16));
            in.src0 = uint16_t(Bits(w, 7, 0));
            in.src1 = uint16_t(Bits(w, 15, 8));
        } else if ((w >> 27) == 0x18) { // SMRD
            in.fmt = Fmt::SMRD;
            in.op = uint16_t(Bits(w, 26, 22));
            in.dst = uint16_t(Bits(w, 21, 15));
            in.sbase = uint16_t(Bits(w, 14, 9) * 2);
            in.smrd_imm = Bits(w, 8, 8);
            in.smrd_offset = Bits(w, 7, 0);
            if (!in.smrd_imm && in.smrd_offset == kLiteral) {
                in.has_literal = true;
                in.literal = code[i + 1];
                in.smrd_imm = true;
                in.smrd_offset = in.literal / 4; // literal is a byte offset
                len = 2;
            }
        } else if ((w >> 31) == 0) { // VOP2 / VOP1 / VOPC
            const uint32_t top7 = w >> 25;
            in.src0 = uint16_t(Bits(w, 8, 0));
            if (top7 == 0x3F) {
                in.fmt = Fmt::VOP1;
                in.op = uint16_t(Bits(w, 16, 9));
                in.dst = uint16_t(kVgprBase + Bits(w, 24, 17));
            } else if (top7 == 0x3E) {
                in.fmt = Fmt::VOPC;
                in.op = uint16_t(Bits(w, 24, 17));
                in.src1 = uint16_t(kVgprBase + Bits(w, 16, 9));
                in.dst = kVccLo;
            } else {
                in.fmt = Fmt::VOP2;
                in.op = uint16_t(Bits(w, 30, 25));
                in.dst = uint16_t(kVgprBase + Bits(w, 24, 17));
                in.src1 = uint16_t(kVgprBase + Bits(w, 16, 9));
                if (in.op == 32 || in.op == 33) { in.has_literal = true; in.literal = code[i + 1]; len = 2; }
            }
            if (in.src0 == kLiteral && !in.has_literal) { in.has_literal = true; in.literal = code[i + 1]; len = 2; }
        } else {
            const uint32_t top6 = w >> 26;
            const uint32_t w1 = (i + 1 < max_dw) ? code[i + 1] : 0;
            in.w1 = w1;
            len = 2;
            switch (top6) {
            case 0x34: // VOP3
                in.fmt = Fmt::VOP3;
                in.op = uint16_t(Bits(w, 25, 17));
                if (IsVop3b(in.op)) {
                    in.sdst3 = uint16_t(Bits(w, 14, 8));
                } else {
                    in.abs = uint8_t(Bits(w, 10, 8));
                    in.clamp = Bits(w, 11, 11);
                }
                in.dst = uint16_t(Bits(w, 7, 0));
                if (in.op >= 0x100) in.dst += kVgprBase; // VOPC encoded as VOP3 writes an SGPR pair
                in.src0 = uint16_t(Bits(w1, 8, 0));
                in.src1 = uint16_t(Bits(w1, 17, 9));
                in.src2 = uint16_t(Bits(w1, 26, 18));
                in.omod = uint8_t(Bits(w1, 28, 27));
                in.neg = uint8_t(Bits(w1, 31, 29));
                break;
            case 0x32: // VINTRP (32-bit)
                in.fmt = Fmt::VINTRP;
                len = 1;
                in.dst = uint16_t(kVgprBase + Bits(w, 25, 18));
                in.op = uint16_t(Bits(w, 17, 16));
                in.attr = uint8_t(Bits(w, 15, 10));
                in.attr_chan = uint8_t(Bits(w, 9, 8));
                in.src0 = uint16_t(kVgprBase + Bits(w, 7, 0));
                break;
            case 0x36: // DS
                in.fmt = Fmt::DS;
                in.op = uint16_t(Bits(w, 25, 18));
                in.mem_offset = uint16_t(Bits(w, 15, 0));
                in.vaddr = uint16_t(Bits(w1, 7, 0));
                in.vdata = uint16_t(Bits(w1, 15, 8));
                in.dst = uint16_t(kVgprBase + Bits(w1, 31, 24));
                break;
            case 0x38: // MUBUF
            case 0x3A: // MTBUF
                in.fmt = top6 == 0x38 ? Fmt::MUBUF : Fmt::MTBUF;
                in.mem_offset = uint16_t(Bits(w, 11, 0));
                in.offen = Bits(w, 12, 12);
                in.idxen = Bits(w, 13, 13);
                in.glc = Bits(w, 14, 14);
                if (top6 == 0x38) {
                    in.lds = Bits(w, 16, 16);
                    in.op = uint16_t(Bits(w, 24, 18));
                } else {
                    in.op = uint16_t(Bits(w, 18, 16));
                    in.dfmt = uint8_t(Bits(w, 22, 19));
                    in.nfmt = uint8_t(Bits(w, 25, 23));
                }
                in.vaddr = uint16_t(Bits(w1, 7, 0));
                in.vdata = uint16_t(Bits(w1, 15, 8));
                in.srsrc = uint16_t(Bits(w1, 20, 16) * 4);
                in.slc = Bits(w1, 22, 22);
                in.tfe = Bits(w1, 23, 23);
                in.soffset = uint16_t(Bits(w1, 31, 24));
                break;
            case 0x3C: // MIMG
                in.fmt = Fmt::MIMG;
                in.dmask = uint8_t(Bits(w, 11, 8));
                in.unorm = Bits(w, 12, 12);
                in.glc = Bits(w, 13, 13);
                in.da = Bits(w, 14, 14);
                in.r128 = Bits(w, 15, 15);
                in.tfe = Bits(w, 16, 16);
                in.lwe = Bits(w, 17, 17);
                in.op = uint16_t(Bits(w, 24, 18));
                in.slc = Bits(w, 25, 25);
                in.vaddr = uint16_t(Bits(w1, 7, 0));
                in.vdata = uint16_t(Bits(w1, 15, 8));
                in.srsrc = uint16_t(Bits(w1, 20, 16) * 4);
                in.ssamp = uint16_t(Bits(w1, 25, 21) * 4);
                break;
            case 0x3E: // EXP
                in.fmt = Fmt::EXP;
                in.exp_en = uint8_t(Bits(w, 3, 0));
                in.exp_target = uint8_t(Bits(w, 9, 4));
                in.exp_compr = Bits(w, 10, 10);
                in.exp_done = Bits(w, 11, 11);
                in.exp_vm = Bits(w, 12, 12);
                for (int k = 0; k < 4; ++k) in.exp_src[k] = uint16_t(Bits(w1, 8 * k + 7, 8 * k));
                break;
            default:
                in.fmt = Fmt::Unknown;
                len = 1;
                break;
            }
        }
        // Scalar ALU literals (32-bit constant following the instruction).
        if ((in.fmt == Fmt::SOP2 || in.fmt == Fmt::SOPC) && (in.src0 == kLiteral || in.src1 == kLiteral) ||
            (in.fmt == Fmt::SOP1 && in.src0 == kLiteral)) {
            in.has_literal = true;
            in.literal = code[i + 1];
            len = 2;
        }
        in.size = uint8_t(len * 4);
        out.push_back(in);
        i += len;
        if (in.fmt == Fmt::SOPP && in.op == 1) break; // s_endpgm
        if (in.fmt == Fmt::SOP1 && in.op == 32) break; // s_setpc_b64: end of a fetch subroutine
    }
    return out;
}

const char* FmtName(Fmt f) {
    static const char* n[] = {"SOP2", "SOPK", "SOP1", "SOPC", "SOPP", "SMRD", "VOP2", "VOP1", "VOPC", "VOP3",
                              "VINTRP", "DS", "MUBUF", "MTBUF", "MIMG", "EXP", "UNKNOWN"};
    return n[int(f)];
}

const char* OpName(const Instr& in) {
    switch (in.fmt) {
    case Fmt::SOP2: return Lookup(Sop2Names(), in.op);
    case Fmt::SOPK: return Lookup(SopkNames(), in.op);
    case Fmt::SOP1: return Lookup(Sop1Names(), in.op);
    case Fmt::SOPC: return Lookup(SopcNames(), in.op);
    case Fmt::SOPP: return Lookup(SoppNames(), in.op);
    case Fmt::SMRD: return Lookup(SmrdNames(), in.op);
    case Fmt::VOP2: return Lookup(Vop2Names(), in.op);
    case Fmt::VOP1: return Lookup(Vop1Names(), in.op);
    case Fmt::VOPC: return VopcName(in.op);
    case Fmt::VOP3:
        if (in.op < 0x100) return VopcName(in.op);
        if (in.op < 0x140) return Lookup(Vop2Names(), uint16_t(in.op - 0x100));
        if (in.op >= 0x180) return Lookup(Vop1Names(), uint16_t(in.op - 0x180));
        return Lookup(Vop3OnlyNames(), in.op);
    case Fmt::VINTRP: return in.op == 0 ? "v_interp_p1_f32" : in.op == 1 ? "v_interp_p2_f32" : "v_interp_mov_f32";
    case Fmt::MUBUF: return Lookup(MubufNames(), in.op);
    case Fmt::MTBUF: {
        static const char* n[8] = {"tbuffer_load_format_x", "tbuffer_load_format_xy", "tbuffer_load_format_xyz",
                                   "tbuffer_load_format_xyzw", "tbuffer_store_format_x", "tbuffer_store_format_xy",
                                   "tbuffer_store_format_xyz", "tbuffer_store_format_xyzw"};
        return n[in.op & 7];
    }
    case Fmt::MIMG: return Lookup(MimgNames(), in.op);
    case Fmt::EXP: return "exp";
    case Fmt::DS: return "ds_?";
    default: return "?";
    }
}

std::string OperandName(uint16_t c, uint32_t literal) {
    char b[48];
    if (c < 104) std::snprintf(b, sizeof b, "s%u", c);
    else if (c == kVccLo) return "vcc_lo";
    else if (c == kVccHi) return "vcc_hi";
    else if (c == kM0) return "m0";
    else if (c == kExecLo) return "exec_lo";
    else if (c == kExecHi) return "exec_hi";
    else if (c >= 112 && c <= 123) std::snprintf(b, sizeof b, "ttmp%u", c - 112);
    else if (c >= 128 && c <= 192) std::snprintf(b, sizeof b, "%d", int(c) - 128);
    else if (c >= 193 && c <= 208) std::snprintf(b, sizeof b, "%d", 192 - int(c));
    else if (c >= 240 && c <= 247) {
        static const char* f[8] = {"0.5", "-0.5", "1.0", "-1.0", "2.0", "-2.0", "4.0", "-4.0"};
        return f[c - 240];
    } else if (c == kVccz) return "vccz";
    else if (c == kExecz) return "execz";
    else if (c == kScc) return "scc";
    else if (c == kLiteral) std::snprintf(b, sizeof b, "0x%x", literal);
    else if (c >= kVgprBase) std::snprintf(b, sizeof b, "v%u", c - kVgprBase);
    else std::snprintf(b, sizeof b, "?%u", c);
    return b;
}

std::string Disassemble(const Instr& in) {
    char b[256];
    const std::string s0 = OperandName(in.src0, in.literal), s1 = OperandName(in.src1, in.literal),
                      s2 = OperandName(in.src2, in.literal), d = OperandName(in.dst);
    switch (in.fmt) {
    case Fmt::SOP2: std::snprintf(b, sizeof b, "%s %s, %s, %s", OpName(in), d.c_str(), s0.c_str(), s1.c_str()); break;
    case Fmt::SOP1: std::snprintf(b, sizeof b, "%s %s, %s", OpName(in), d.c_str(), s0.c_str()); break;
    case Fmt::SOPK: std::snprintf(b, sizeof b, "%s %s, 0x%x", OpName(in), d.c_str(), uint16_t(in.simm16)); break;
    case Fmt::SOPC: std::snprintf(b, sizeof b, "%s %s, %s", OpName(in), s0.c_str(), s1.c_str()); break;
    case Fmt::SOPP: std::snprintf(b, sizeof b, "%s 0x%x", OpName(in), uint16_t(in.simm16)); break;
    case Fmt::SMRD:
        if (in.smrd_imm)
            std::snprintf(b, sizeof b, "%s s%u, s[%u:%u], 0x%x", OpName(in), in.dst, in.sbase, in.sbase + 1, in.smrd_offset);
        else
            std::snprintf(b, sizeof b, "%s s%u, s[%u:%u], s%u", OpName(in), in.dst, in.sbase, in.sbase + 1, in.smrd_offset);
        break;
    case Fmt::VOP1: std::snprintf(b, sizeof b, "%s %s, %s", OpName(in), d.c_str(), s0.c_str()); break;
    case Fmt::VOP2:
        if (in.op == 32 || in.op == 33)
            std::snprintf(b, sizeof b, "%s %s, %s, %s, 0x%x", OpName(in), d.c_str(), s0.c_str(), s1.c_str(), in.literal);
        else
            std::snprintf(b, sizeof b, "%s %s, %s, %s", OpName(in), d.c_str(), s0.c_str(), s1.c_str());
        break;
    case Fmt::VOPC: std::snprintf(b, sizeof b, "%s vcc, %s, %s", OpName(in), s0.c_str(), s1.c_str()); break;
    case Fmt::VOP3:
        std::snprintf(b, sizeof b, "%s %s, %s, %s, %s  [abs=%x neg=%x omod=%u clamp=%d sdst=%u]", OpName(in), d.c_str(),
                      s0.c_str(), s1.c_str(), s2.c_str(), in.abs, in.neg, in.omod, in.clamp, in.sdst3);
        break;
    case Fmt::VINTRP:
        std::snprintf(b, sizeof b, "%s %s, %s, attr%u.%c", OpName(in), d.c_str(), s0.c_str(), in.attr, "xyzw"[in.attr_chan]);
        break;
    case Fmt::MUBUF:
    case Fmt::MTBUF:
        std::snprintf(b, sizeof b, "%s v%u, v%u, s[%u:%u], %s offset:%u%s%s fmt=%u/%u", OpName(in), in.vdata, in.vaddr,
                      in.srsrc, in.srsrc + 3, OperandName(in.soffset).c_str(), in.mem_offset, in.idxen ? " idxen" : "",
                      in.offen ? " offen" : "", in.dfmt, in.nfmt);
        break;
    case Fmt::MIMG:
        std::snprintf(b, sizeof b, "%s v%u, v%u, s[%u:%u], s[%u:%u] dmask:0x%x%s%s", OpName(in), in.vdata, in.vaddr,
                      in.srsrc, in.srsrc + 7, in.ssamp, in.ssamp + 3, in.dmask, in.unorm ? " unorm" : "", in.da ? " da" : "");
        break;
    case Fmt::EXP: {
        const char* tgt;
        char t[16];
        if (in.exp_target < 8) { std::snprintf(t, sizeof t, "mrt%u", in.exp_target); tgt = t; }
        else if (in.exp_target == 8) tgt = "mrtz";
        else if (in.exp_target == 9) tgt = "null";
        else if (in.exp_target >= 12 && in.exp_target < 16) { std::snprintf(t, sizeof t, "pos%u", in.exp_target - 12); tgt = t; }
        else { std::snprintf(t, sizeof t, "param%u", in.exp_target - 32); tgt = t; }
        std::snprintf(b, sizeof b, "exp %s en=%x v%u v%u v%u v%u%s%s%s", tgt, in.exp_en, in.exp_src[0], in.exp_src[1],
                      in.exp_src[2], in.exp_src[3], in.exp_compr ? " compr" : "", in.exp_done ? " done" : "",
                      in.exp_vm ? " vm" : "");
        break;
    }
    default: std::snprintf(b, sizeof b, "%s op=%u raw=%08x %08x", FmtName(in.fmt), in.op, in.w0, in.w1); break;
    }
    return b;
}

} // namespace gr::gcn
