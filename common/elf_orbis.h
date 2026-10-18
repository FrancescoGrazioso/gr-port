#pragma once
// ELF64 structures plus the Sony (Orbis) extensions used by eboot.bin / *.prx.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gr {

#pragma pack(push, 1)
struct Elf64_Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf64_Phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct Elf64_Dyn {
    int64_t d_tag;
    uint64_t d_val;
};
struct Elf64_Sym {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct Elf64_Rela {
    uint64_t r_offset, r_info;
    int64_t r_addend;
};
#pragma pack(pop)

// e_type
constexpr uint16_t ET_SCE_EXEC = 0xFE00;
constexpr uint16_t ET_SCE_DYNEXEC = 0xFE10;
constexpr uint16_t ET_SCE_DYNAMIC = 0xFE18;

// p_type
constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PT_TLS = 7;
constexpr uint32_t PT_SCE_RELA = 0x60000000;
constexpr uint32_t PT_SCE_DYNLIBDATA = 0x61000000;
constexpr uint32_t PT_SCE_PROCPARAM = 0x61000001;
constexpr uint32_t PT_SCE_MODULE_PARAM = 0x61000002;
constexpr uint32_t PT_SCE_RELRO = 0x61000010;
constexpr uint32_t PT_GNU_EH_FRAME = 0x6474E550;
constexpr uint32_t PT_SCE_COMMENT = 0x6FFFFF00;
constexpr uint32_t PT_SCE_VERSION = 0x6FFFFF01;

// d_tag (standard + SCE)
constexpr int64_t DT_NULL = 0, DT_NEEDED = 1, DT_INIT = 12, DT_FINI = 13, DT_SONAME = 14;
constexpr int64_t DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28;
constexpr int64_t DT_PREINIT_ARRAY = 32, DT_PREINIT_ARRAYSZ = 33;
constexpr int64_t DT_SCE_FINGERPRINT = 0x61000007;
constexpr int64_t DT_SCE_ORIGINAL_FILENAME = 0x61000009;
constexpr int64_t DT_SCE_MODULE_INFO = 0x6100000D;
constexpr int64_t DT_SCE_NEEDED_MODULE = 0x6100000F;
constexpr int64_t DT_SCE_MODULE_ATTR = 0x61000011;
constexpr int64_t DT_SCE_EXPORT_LIB = 0x61000013;
constexpr int64_t DT_SCE_IMPORT_LIB = 0x61000015;
constexpr int64_t DT_SCE_EXPORT_LIB_ATTR = 0x61000017;
constexpr int64_t DT_SCE_IMPORT_LIB_ATTR = 0x61000019;
constexpr int64_t DT_SCE_HASH = 0x61000025;
constexpr int64_t DT_SCE_PLTGOT = 0x61000027;
constexpr int64_t DT_SCE_JMPREL = 0x61000029;
constexpr int64_t DT_SCE_PLTREL = 0x6100002B;
constexpr int64_t DT_SCE_PLTRELSZ = 0x6100002D;
constexpr int64_t DT_SCE_RELA = 0x6100002F;
constexpr int64_t DT_SCE_RELASZ = 0x61000031;
constexpr int64_t DT_SCE_RELAENT = 0x61000033;
constexpr int64_t DT_SCE_STRTAB = 0x61000035;
constexpr int64_t DT_SCE_STRSZ = 0x61000037;
constexpr int64_t DT_SCE_SYMTAB = 0x61000039;
constexpr int64_t DT_SCE_SYMENT = 0x6100003B;
constexpr int64_t DT_SCE_HASHSZ = 0x6100003D;
constexpr int64_t DT_SCE_SYMTABSZ = 0x6100003F;

// relocation types (x86-64)
constexpr uint32_t R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7,
                   R_X86_64_RELATIVE = 8, R_X86_64_DTPMOD64 = 16, R_X86_64_DTPOFF64 = 17,
                   R_X86_64_TPOFF64 = 18;

// Library / module descriptors packed into a d_val.
struct OrbisLibrary {
    std::string name;
    uint16_t id = 0;
    uint16_t version = 0; // major << 8 | minor
    bool is_export = false;
};

struct OrbisSymbol {
    std::string raw;     // e.g. "Ou3iL1abvng#C#A"
    std::string nid;     // 11-char NID
    uint16_t library_id; // decoded from the first '#' field
    uint16_t module_id;  // decoded from the second '#' field
    uint8_t bind, type;
    uint16_t shndx;
    uint64_t value;
    bool defined() const { return shndx != 0; }
};

// Parsed view of an Orbis ELF (no memory mapping; just metadata).
struct OrbisElf {
    std::span<const uint8_t> bytes;
    Elf64_Ehdr ehdr{};
    std::vector<Elf64_Phdr> phdrs;
    std::vector<Elf64_Dyn> dynamic;
    std::span<const uint8_t> dynlib; // PT_SCE_DYNLIBDATA contents

    std::vector<OrbisLibrary> needed_modules, module_info, libraries;
    std::vector<OrbisSymbol> symbols;
    std::vector<Elf64_Rela> rela, jmprel;
    std::string original_filename;
    std::string fingerprint_hex;
    uint64_t init = 0, fini = 0;
    bool has_init = false;

    bool Parse(std::span<const uint8_t> elf, std::string* err);
    const OrbisLibrary* FindLibrary(uint16_t id, bool is_export) const;
    const OrbisLibrary* FindModule(uint16_t id) const;
    uint64_t VirtualSpan(uint64_t* min_vaddr) const; // extent of PT_LOAD-like segments
};

const char* PhdrTypeName(uint32_t t);
const char* ElfTypeName(uint16_t t);

} // namespace gr
