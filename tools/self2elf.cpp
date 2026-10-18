// self2elf: converts fake-signed PS4 SELF files (eboot.bin, *.prx, *.sprx) to plain ELF.
//   self2elf <input.self> <output.elf>
//   self2elf --info <input.self>
#include <cstdio>
#include <string>

#include "elf_orbis.h"
#include "self.h"

static void PrintInfo(const gr::SelfInfo& si, const std::vector<uint8_t>& elf) {
    if (si.is_self) {
        std::printf("SELF: %u segments, ELF header at 0x%llx\n", si.num_segments, (unsigned long long)si.elf_offset);
        for (const auto& s : si.segments)
            std::printf("  seg flags=%08llx id=%-3u %s%s%s off=0x%-9llx stored=0x%-9llx mem=0x%llx\n",
                        (unsigned long long)s.flags, s.phdr_id(), s.blocked() ? "B" : "-", s.compressed() ? "Z" : "-",
                        s.encrypted() ? "E" : "-", (unsigned long long)s.file_offset,
                        (unsigned long long)s.stored_size, (unsigned long long)s.memory_size);
    }
    gr::OrbisElf oe;
    std::string err;
    if (!oe.Parse(elf, &err)) {
        std::printf("ELF parse error: %s\n", err.c_str());
        return;
    }
    std::printf("ELF: type=0x%04x (%s) entry=0x%llx phnum=%u\n", oe.ehdr.e_type, gr::ElfTypeName(oe.ehdr.e_type),
                (unsigned long long)oe.ehdr.e_entry, oe.ehdr.e_phnum);
    for (const auto& p : oe.phdrs)
        std::printf("  %-16s off=0x%-9llx vaddr=0x%-11llx filesz=0x%-9llx memsz=0x%-9llx flags=%c%c%c align=0x%llx\n",
                    gr::PhdrTypeName(p.p_type), (unsigned long long)p.p_offset, (unsigned long long)p.p_vaddr,
                    (unsigned long long)p.p_filesz, (unsigned long long)p.p_memsz, (p.p_flags & 4) ? 'R' : '-',
                    (p.p_flags & 2) ? 'W' : '-', (p.p_flags & 1) ? 'X' : '-', (unsigned long long)p.p_align);
    for (const auto& w : si.warnings) std::printf("warning: %s\n", w.c_str());
}

int main(int argc, char** argv) {
    const bool info_only = argc == 3 && std::string(argv[1]) == "--info";
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.self> <output.elf> | --info <input.self>\n", argv[0]);
        return 1;
    }
    const std::string in = info_only ? argv[2] : argv[1];
    std::vector<uint8_t> file, elf;
    if (!gr::ReadWholeFile(in, file)) { std::fprintf(stderr, "cannot read %s\n", in.c_str()); return 2; }
    gr::SelfInfo si;
    std::string err;
    if (!gr::SelfToElf(file, elf, &si, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 3; }
    PrintInfo(si, elf);
    if (info_only) return 0;
    if (!gr::WriteWholeFile(argv[2], elf)) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 4; }
    std::printf("wrote %s (%zu bytes)\n", argv[2], elf.size());
    return 0;
}
