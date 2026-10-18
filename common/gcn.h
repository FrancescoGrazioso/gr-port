#pragma once
// AMD GCN 1.1 ("Sea Islands" / CI, the PS4 Liverpool GPU) instruction decoder.
#include <cstdint>
#include <string>
#include <vector>

namespace gr::gcn {

enum class Fmt : uint8_t { SOP2, SOPK, SOP1, SOPC, SOPP, SMRD, VOP2, VOP1, VOPC, VOP3, VINTRP, DS, MUBUF, MTBUF, MIMG, EXP, Unknown };

// Operand codes (9-bit "src" space): 0..103 SGPR, 106/107 VCC, 124 M0, 126/127 EXEC, 128..208 inline
// integers, 240..247 inline floats, 251 VCCZ, 252 EXECZ, 253 SCC, 255 literal, 256..511 VGPR.
enum : uint16_t {
    kVccLo = 106, kVccHi = 107, kM0 = 124, kExecLo = 126, kExecHi = 127, kConstZero = 128, kVccz = 251,
    kExecz = 252, kScc = 253, kLiteral = 255, kVgprBase = 256,
};

struct Instr {
    uint32_t pc = 0; // byte offset within the program
    Fmt fmt = Fmt::Unknown;
    uint16_t op = 0; // opcode within the format (VOP3: unified 9-bit opcode space)
    uint8_t size = 4; // bytes, including a literal
    uint32_t literal = 0;
    bool has_literal = false;
    uint32_t w0 = 0, w1 = 0; // raw encoding

    // Common decoded fields
    uint16_t dst = 0;           // sdst / vdst (VGPRs as 256+n for vector formats)
    uint16_t src0 = 0, src1 = 0, src2 = 0;
    int16_t simm16 = 0;         // SOPP / SOPK
    // VOP3 modifiers
    uint8_t abs = 0, neg = 0, omod = 0;
    bool clamp = false;
    uint16_t sdst3 = 0;         // VOP3b carry-out / VOPC-in-VOP3 destination SGPR
    // SMRD
    uint16_t sbase = 0;         // first SGPR of the base pair
    bool smrd_imm = false;
    uint32_t smrd_offset = 0;   // dwords when imm, else an SGPR number
    // memory (MUBUF/MTBUF/MIMG)
    uint16_t vaddr = 0, vdata = 0, srsrc = 0, ssamp = 0, soffset = 0;
    uint16_t mem_offset = 0;
    bool offen = false, idxen = false, glc = false, slc = false, tfe = false, lds = false;
    uint8_t dmask = 0, dfmt = 0, nfmt = 0;
    bool unorm = false, da = false, r128 = false, lwe = false;
    // EXP
    uint8_t exp_target = 0, exp_en = 0;
    bool exp_compr = false, exp_done = false, exp_vm = false;
    uint16_t exp_src[4] = {};
    // VINTRP
    uint8_t attr = 0, attr_chan = 0;
};

// Decodes a whole program (stops after s_endpgm or at `max_bytes`).
std::vector<Instr> Decode(const uint32_t* code, uint32_t max_bytes);

const char* FmtName(Fmt f);
const char* OpName(const Instr& in); // "?" when unknown
std::string OperandName(uint16_t code, uint32_t literal = 0);
std::string Disassemble(const Instr& in);

} // namespace gr::gcn
