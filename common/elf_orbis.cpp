#include "elf_orbis.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "nid.h"

namespace gr {
namespace {
template <class T> bool LoadAt(std::span<const uint8_t> b, uint64_t off, T& v) {
    if (off + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + off, sizeof(T));
    return true;
}

OrbisLibrary DecodeLib(uint64_t v, std::span<const uint8_t> strtab, bool is_module) {
    OrbisLibrary l;
    const uint32_t name_off = uint32_t(v);
    if (name_off < strtab.size()) {
        const char* s = reinterpret_cast<const char*>(strtab.data() + name_off);
        l.name.assign(s, strnlen(s, strtab.size() - name_off));
    }
    if (is_module) l.version = uint16_t((((v >> 40) & 0xFF) << 8) | ((v >> 32) & 0xFF)); // major.minor
    else l.version = uint16_t(v >> 32);
    l.id = uint16_t(v >> 48);
    return l;
}
} // namespace

bool OrbisElf::Parse(std::span<const uint8_t> elf, std::string* err) {
    bytes = elf;
    if (!LoadAt(elf, 0, ehdr) || std::memcmp(ehdr.e_ident, "\x7F" "ELF", 4) != 0) { *err = "not an ELF"; return false; }
    phdrs.resize(ehdr.e_phnum);
    for (uint16_t i = 0; i < ehdr.e_phnum; ++i)
        if (!LoadAt(elf, ehdr.e_phoff + i * sizeof(Elf64_Phdr), phdrs[i])) { *err = "truncated phdrs"; return false; }

    for (const auto& p : phdrs) {
        if (p.p_offset + p.p_filesz > elf.size()) continue;
        if (p.p_type == PT_DYNAMIC) {
            for (uint64_t o = 0; o + sizeof(Elf64_Dyn) <= p.p_filesz; o += sizeof(Elf64_Dyn)) {
                Elf64_Dyn d;
                LoadAt(elf, p.p_offset + o, d);
                if (d.d_tag == DT_NULL) break;
                dynamic.push_back(d);
            }
        } else if (p.p_type == PT_SCE_DYNLIBDATA) {
            dynlib = elf.subspan(p.p_offset, p.p_filesz);
        }
    }
    if (dynamic.empty()) return true; // static object: nothing more to decode

    uint64_t strtab_off = 0, strsz = 0, symtab_off = 0, symtabsz = 0, jmprel_off = 0, jmprelsz = 0, rela_off = 0,
             relasz = 0;
    for (const auto& d : dynamic) {
        switch (d.d_tag) {
        case DT_SCE_STRTAB: strtab_off = d.d_val; break;
        case DT_SCE_STRSZ: strsz = d.d_val; break;
        case DT_SCE_SYMTAB: symtab_off = d.d_val; break;
        case DT_SCE_SYMTABSZ: symtabsz = d.d_val; break;
        case DT_SCE_JMPREL: jmprel_off = d.d_val; break;
        case DT_SCE_PLTRELSZ: jmprelsz = d.d_val; break;
        case DT_SCE_RELA: rela_off = d.d_val; break;
        case DT_SCE_RELASZ: relasz = d.d_val; break;
        case DT_INIT: init = d.d_val; has_init = true; break;
        case DT_FINI: fini = d.d_val; break;
        default: break;
        }
    }
    if (strtab_off + strsz > dynlib.size()) { *err = "string table outside dynlibdata"; return false; }
    const auto strtab = dynlib.subspan(strtab_off, strsz);
    auto str_at = [&](uint64_t off) -> std::string {
        if (off >= strtab.size()) return {};
        const char* s = reinterpret_cast<const char*>(strtab.data() + off);
        return std::string(s, strnlen(s, strtab.size() - off));
    };

    for (const auto& d : dynamic) {
        switch (d.d_tag) {
        case DT_SCE_NEEDED_MODULE: needed_modules.push_back(DecodeLib(d.d_val, strtab, true)); break;
        case DT_SCE_MODULE_INFO: module_info.push_back(DecodeLib(d.d_val, strtab, true)); break;
        case DT_SCE_IMPORT_LIB: libraries.push_back(DecodeLib(d.d_val, strtab, false)); break;
        case DT_SCE_EXPORT_LIB: {
            auto l = DecodeLib(d.d_val, strtab, false);
            l.is_export = true;
            libraries.push_back(l);
            break;
        }
        case DT_SCE_ORIGINAL_FILENAME: original_filename = str_at(d.d_val); break;
        case DT_SCE_FINGERPRINT:
            if (d.d_val + 20 <= dynlib.size()) {
                char hex[3];
                for (int i = 0; i < 20; ++i) {
                    std::snprintf(hex, sizeof hex, "%02x", dynlib[d.d_val + i]);
                    fingerprint_hex += hex;
                }
            }
            break;
        default: break;
        }
    }

    for (uint64_t o = 0; o + sizeof(Elf64_Sym) <= symtabsz; o += sizeof(Elf64_Sym)) {
        Elf64_Sym s;
        if (!LoadAt(dynlib, symtab_off + o, s)) break;
        OrbisSymbol sym{};
        sym.raw = str_at(s.st_name);
        sym.bind = s.st_info >> 4;
        sym.type = s.st_info & 0xF;
        sym.shndx = s.st_shndx;
        sym.value = s.st_value;
        const auto h1 = sym.raw.find('#');
        if (h1 != std::string::npos) {
            sym.nid = sym.raw.substr(0, h1);
            const auto h2 = sym.raw.find('#', h1 + 1);
            sym.library_id = DecodeNidIndex(std::string_view(sym.raw).substr(h1 + 1, h2 - h1 - 1));
            sym.module_id = h2 == std::string::npos ? 0 : DecodeNidIndex(std::string_view(sym.raw).substr(h2 + 1));
        } else {
            sym.nid = sym.raw;
            sym.library_id = sym.module_id = 0xFFFF;
        }
        symbols.push_back(std::move(sym));
    }
    auto load_rela = [&](uint64_t off, uint64_t size, std::vector<Elf64_Rela>& out) {
        for (uint64_t o = 0; o + sizeof(Elf64_Rela) <= size; o += sizeof(Elf64_Rela)) {
            Elf64_Rela r;
            if (!LoadAt(dynlib, off + o, r)) break;
            out.push_back(r);
        }
    };
    load_rela(rela_off, relasz, rela);
    load_rela(jmprel_off, jmprelsz, jmprel);
    return true;
}

const OrbisLibrary* OrbisElf::FindLibrary(uint16_t id, bool is_export) const {
    for (const auto& l : libraries)
        if (l.id == id && l.is_export == is_export) return &l;
    return nullptr;
}

const OrbisLibrary* OrbisElf::FindModule(uint16_t id) const {
    for (const auto& m : needed_modules)
        if (m.id == id) return &m;
    for (const auto& m : module_info)
        if (m.id == id) return &m;
    return nullptr;
}

uint64_t OrbisElf::VirtualSpan(uint64_t* min_vaddr) const {
    uint64_t lo = ~0ull, hi = 0;
    for (const auto& p : phdrs) {
        if (p.p_type != PT_LOAD && p.p_type != PT_SCE_RELRO) continue;
        lo = std::min(lo, p.p_vaddr);
        hi = std::max(hi, p.p_vaddr + p.p_memsz);
    }
    if (lo == ~0ull) lo = hi = 0;
    if (min_vaddr) *min_vaddr = lo;
    return hi - lo;
}

const char* PhdrTypeName(uint32_t t) {
    switch (t) {
    case PT_LOAD: return "LOAD";
    case PT_DYNAMIC: return "DYNAMIC";
    case PT_INTERP: return "INTERP";
    case PT_TLS: return "TLS";
    case PT_SCE_RELA: return "SCE_RELA";
    case PT_SCE_DYNLIBDATA: return "SCE_DYNLIBDATA";
    case PT_SCE_PROCPARAM: return "SCE_PROCPARAM";
    case PT_SCE_MODULE_PARAM: return "SCE_MODULE_PARAM";
    case PT_SCE_RELRO: return "SCE_RELRO";
    case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";
    case PT_SCE_COMMENT: return "SCE_COMMENT";
    case PT_SCE_VERSION: return "SCE_VERSION";
    default: return "?";
    }
}

const char* ElfTypeName(uint16_t t) {
    switch (t) {
    case ET_SCE_EXEC: return "ET_SCE_EXEC";
    case ET_SCE_DYNEXEC: return "ET_SCE_DYNEXEC";
    case ET_SCE_DYNAMIC: return "ET_SCE_DYNAMIC";
    default: return "?";
    }
}

} // namespace gr
