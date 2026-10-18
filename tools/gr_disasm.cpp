// gr_disasm: disassembles an Orbis ELF/SELF at a virtual address, annotating calls through the
// PLT/GOT with the imported symbol name when known.
//   gr_disasm [--names f.txt] <file.elf|self> <vaddr_hex> [count=40] [--back N]
#include <capstone/capstone.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "elf_orbis.h"
#include "nid.h"
#include "self.h"

int main(int argc, char** argv) {
    gr::NidDatabase db;
    std::vector<std::string> pos;
    uint64_t back = 0;
    bool xref = false; // --xref: list references to the address instead of disassembling it
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--names" && i + 1 < argc) db.LoadNamesFile(argv[++i]);
        else if (a == "--back" && i + 1 < argc) back = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--xref") xref = true;
        else pos.push_back(a);
    }
    if (pos.size() < 2) {
        std::fprintf(stderr, "usage: %s [--names f.txt] <elf|self> <vaddr> [count] [--back bytes]\n", argv[0]);
        return 1;
    }
    std::vector<uint8_t> file, elf;
    std::string err;
    gr::OrbisElf oe;
    if (!gr::ReadWholeFile(pos[0], file) || !gr::SelfToElf(file, elf, nullptr, &err) || !oe.Parse(elf, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    // "name" instead of a hex address: look the symbol up among the exports
    uint64_t base_addr = 0;
    std::string import_name;
    const bool is_hex = pos[1].rfind("0x", 0) == 0 ||
                        pos[1].find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    if (is_hex) {
        base_addr = std::strtoull(pos[1].c_str(), nullptr, 16);
    } else {
        const std::string nid = gr::ComputeNid(pos[1]);
        bool found = false;
        for (const auto& s : oe.symbols)
            if (s.defined() && s.nid == nid) { base_addr = s.value; found = true; break; }
        if (!found) import_name = pos[1]; // not exported: resolved among the imports below
        else std::printf("%s = 0x%llx\n", pos[1].c_str(), (unsigned long long)base_addr);
    }
    const size_t count = pos.size() > 2 ? std::strtoul(pos[2].c_str(), nullptr, 10) : 40;

    // GOT slot vaddr -> import name (from JUMP_SLOT / GLOB_DAT relocations)
    std::unordered_map<uint64_t, std::string> got;
    for (const auto* rels : {&oe.jmprel, &oe.rela})
        for (const auto& r : *rels) {
            const uint32_t sym = uint32_t(r.r_info >> 32);
            if (sym == 0 || sym >= oe.symbols.size() || oe.symbols[sym].defined()) continue;
            const auto& s = oe.symbols[sym];
            const std::string* n = db.Lookup(s.nid);
            char b[96];
            std::snprintf(b, sizeof b, "sym#%u type=%u bind=%u reloc=%u addend=0x%llx", sym, s.type, s.bind,
                          uint32_t(r.r_info), (unsigned long long)r.r_addend);
            got[r.r_offset] = n ? *n : s.nid.empty() ? std::string(b) : s.nid;
        }

    if (!import_name.empty()) {
        bool found = false;
        for (const auto& [slot, n] : got)
            if (n == import_name) { base_addr = slot; found = true; break; }
        if (!found) { std::fprintf(stderr, "symbol %s not found\n", import_name.c_str()); return 3; }
        std::printf("import %s: GOT slot 0x%llx (xref it to find the PLT stub)\n", import_name.c_str(),
                    (unsigned long long)base_addr);
    }
    const uint64_t vaddr = base_addr - back;
    // pointer slot vaddr -> target (RELATIVE relocations), for indirect calls through data
    std::unordered_map<uint64_t, uint64_t> rel;
    for (const auto& r : oe.rela)
        if (uint32_t(r.r_info) == gr::R_X86_64_RELATIVE) rel[r.r_offset] = uint64_t(r.r_addend);

    if (xref) {
        // Data references: pointer slots (RELATIVE relocations) whose value is the target.
        for (const auto& [slot, target] : rel)
            if (target == base_addr) std::printf("data   slot 0x%llx -> 0x%llx\n", (unsigned long long)slot,
                                                 (unsigned long long)target);
        // Code references: linear sweep of every executable segment.
        csh xh;
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &xh) != CS_ERR_OK) return 4;
        cs_option(xh, CS_OPT_DETAIL, CS_OPT_ON);
        cs_option(xh, CS_OPT_SKIPDATA, CS_OPT_ON);
        cs_insn* in = cs_malloc(xh);
        for (const auto& p : oe.phdrs) {
            if (p.p_type != gr::PT_LOAD || !(p.p_flags & 1)) continue;
            const uint8_t* c = elf.data() + p.p_offset;
            size_t sz = size_t(p.p_filesz);
            uint64_t a = p.p_vaddr;
            while (cs_disasm_iter(xh, &c, &sz, &a, in)) {
                if (!in->detail) continue;
                const cs_x86& x = in->detail->x86;
                for (int i = 0; i < x.op_count; ++i) {
                    const auto& op = x.operands[i];
                    const bool hit = (op.type == X86_OP_IMM && uint64_t(op.imm) == base_addr) ||
                                     (op.type == X86_OP_MEM && op.mem.base == X86_REG_RIP &&
                                      in->address + in->size + op.mem.disp == base_addr);
                    if (hit) {
                        std::printf("code   %10llx  %-8s %s\n", (unsigned long long)in->address, in->mnemonic, in->op_str);
                        break;
                    }
                }
            }
        }
        cs_free(in, 1);
        cs_close(&xh);
        return 0;
    }

    // locate the file bytes for vaddr
    const gr::Elf64_Phdr* seg = nullptr;
    for (const auto& p : oe.phdrs)
        if (p.p_type == gr::PT_LOAD && vaddr >= p.p_vaddr && vaddr < p.p_vaddr + p.p_filesz) seg = &p;
    if (!seg) { std::fprintf(stderr, "address not in a loaded segment\n"); return 3; }
    const uint8_t* code = elf.data() + seg->p_offset + (vaddr - seg->p_vaddr);
    size_t size = size_t(seg->p_vaddr + seg->p_filesz - vaddr);

    csh h;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) != CS_ERR_OK) return 4;
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    cs_insn* insn = cs_malloc(h);
    uint64_t addr = vaddr;
    for (size_t n = 0; n < count && cs_disasm_iter(h, &code, &size, &addr, insn); ++n) {
        std::string note;
        // jmp/call [rip+disp] -> GOT slot
        const cs_x86& x = insn->detail->x86;
        for (int i = 0; i < x.op_count; ++i) {
            if (x.operands[i].type == X86_OP_MEM && x.operands[i].mem.base == X86_REG_RIP) {
                const uint64_t target = insn->address + insn->size + x.operands[i].mem.disp;
                if (auto it = got.find(target); it != got.end()) note = "  ; " + it->second;
                else if (auto r = rel.find(target); r != rel.end()) {
                    char b[64];
                    std::snprintf(b, sizeof b, "  ; -> 0x%llx", (unsigned long long)r->second);
                    note = b;
                }
            }
        }
        // call/jmp imm -> PLT stub "jmp [rip+got]"
        if (note.empty() && (insn->id == X86_INS_CALL || insn->id == X86_INS_JMP) && x.op_count == 1 &&
            x.operands[0].type == X86_OP_IMM) {
            const uint64_t t = uint64_t(x.operands[0].imm);
            for (const auto& p : oe.phdrs) {
                if (p.p_type != gr::PT_LOAD || t < p.p_vaddr || t + 6 > p.p_vaddr + p.p_filesz) continue;
                const uint8_t* s = elf.data() + p.p_offset + (t - p.p_vaddr);
                if (s[0] == 0xFF && s[1] == 0x25) {
                    int32_t d;
                    std::memcpy(&d, s + 2, 4);
                    if (auto it = got.find(t + 6 + int64_t(d)); it != got.end()) note = "  ; " + it->second;
                }
            }
        }
        std::printf("%10llx  %-8s %s%s\n", (unsigned long long)insn->address, insn->mnemonic, insn->op_str, note.c_str());
    }
    cs_free(insn, 1);
    cs_close(&h);
    return 0;
}
