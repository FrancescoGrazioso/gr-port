#include "self.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "elf_orbis.h"
#include "inflate.h"

namespace gr {
namespace {
constexpr uint8_t kSelfMagic[4] = {0x4F, 0x15, 0x3D, 0x1D};
constexpr uint8_t kElfMagic[4] = {0x7F, 'E', 'L', 'F'};

template <class T> T Load(std::span<const uint8_t> b, size_t off) {
    T v{};
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}
} // namespace

bool SelfToElf(std::span<const uint8_t> file, std::vector<uint8_t>& elf, SelfInfo* info, std::string* err) {
    SelfInfo local;
    SelfInfo& si = info ? *info : local;
    si = {};
    if (file.size() >= 4 && std::memcmp(file.data(), kElfMagic, 4) == 0) {
        elf.assign(file.begin(), file.end());
        return true;
    }
    if (file.size() < 0x20 || std::memcmp(file.data(), kSelfMagic, 4) != 0) {
        *err = "neither SELF nor ELF";
        return false;
    }
    si.is_self = true;
    si.num_segments = Load<uint16_t>(file, 0x18);
    si.elf_offset = 0x20 + uint64_t(si.num_segments) * 0x20;
    if (file.size() < si.elf_offset + sizeof(Elf64_Ehdr)) { *err = "truncated SELF header"; return false; }
    for (uint16_t i = 0; i < si.num_segments; ++i) {
        size_t o = 0x20 + size_t(i) * 0x20;
        si.segments.push_back({Load<uint64_t>(file, o), Load<uint64_t>(file, o + 8), Load<uint64_t>(file, o + 16),
                               Load<uint64_t>(file, o + 24)});
    }
    auto ef = file.subspan(si.elf_offset);
    if (std::memcmp(ef.data(), kElfMagic, 4) != 0) { *err = "SELF does not embed an ELF header (encrypted?)"; return false; }
    const auto eh = Load<Elf64_Ehdr>(ef, 0);
    if (eh.e_phentsize != sizeof(Elf64_Phdr) || ef.size() < eh.e_phoff + eh.e_phnum * sizeof(Elf64_Phdr)) {
        *err = "bad program header table";
        return false;
    }
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    std::memcpy(ph.data(), ef.data() + eh.e_phoff, ph.size() * sizeof(Elf64_Phdr));

    uint64_t out_size = eh.e_phoff + ph.size() * sizeof(Elf64_Phdr);
    for (const auto& p : ph) out_size = std::max(out_size, p.p_offset + p.p_filesz);
    elf.assign(out_size, 0);
    std::memcpy(elf.data(), ef.data(), sizeof(Elf64_Ehdr));
    std::memcpy(elf.data() + eh.e_phoff, ph.data(), ph.size() * sizeof(Elf64_Phdr));

    std::vector<bool> filled(ph.size(), false);
    std::vector<uint8_t> tmp;
    for (const auto& s : si.segments) {
        if (!s.blocked()) continue; // digests / metadata segments
        const uint32_t id = s.phdr_id();
        if (id >= ph.size()) { si.warnings.push_back("segment refers to missing phdr " + std::to_string(id)); continue; }
        if (s.file_offset + s.stored_size > file.size()) { *err = "segment data beyond end of file"; return false; }
        auto src = file.subspan(s.file_offset, s.stored_size);
        if (s.encrypted()) si.warnings.push_back("segment " + std::to_string(id) + " flagged encrypted (copied as-is)");
        if (s.compressed()) {
            tmp.clear();
            if (!(LooksLikeZlib(src) ? ZlibInflate(src, tmp) : Inflate(src, tmp))) {
                *err = "failed to inflate segment " + std::to_string(id);
                return false;
            }
            src = tmp;
        }
        const auto& p = ph[id];
        const size_t n = std::min<uint64_t>(src.size(), p.p_filesz);
        if (src.size() != p.p_filesz)
            si.warnings.push_back("segment " + std::to_string(id) + " size " + std::to_string(src.size()) +
                                  " != p_filesz " + std::to_string(p.p_filesz));
        std::memcpy(elf.data() + p.p_offset, src.data(), n);
        filled[id] = true;
    }
    // PT_SCE_VERSION data lives at the very end of the SELF without a segment entry.
    for (size_t i = 0; i < ph.size(); ++i) {
        if (filled[i] || ph[i].p_filesz == 0) continue;
        bool covered = false; // e.g. DYNAMIC / TLS / EH_FRAME live inside a copied LOAD segment
        for (size_t j = 0; j < ph.size() && !covered; ++j)
            covered = filled[j] && ph[i].p_offset >= ph[j].p_offset &&
                      ph[i].p_offset + ph[i].p_filesz <= ph[j].p_offset + ph[j].p_filesz;
        if (covered) continue;
        if (ph[i].p_type == PT_SCE_VERSION && ph[i].p_filesz <= file.size()) {
            std::memcpy(elf.data() + ph[i].p_offset, file.data() + file.size() - ph[i].p_filesz, ph[i].p_filesz);
            filled[i] = true;
        } else {
            si.warnings.push_back(std::string("phdr ") + std::to_string(i) + " (" + PhdrTypeName(ph[i].p_type) +
                                  ") has file data but no SELF segment");
        }
    }
    return true;
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize(size_t(n));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool WriteWholeFile(const std::string& path, std::span<const uint8_t> data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

} // namespace gr
