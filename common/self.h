#pragma once
// PS4 SELF container -> plain ELF image. Only fake-signed (fSELF) files are supported:
// their segments are stored in clear, so no keys are involved.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gr {

struct SelfSegmentInfo {
    uint64_t flags;
    uint64_t file_offset;
    uint64_t stored_size;
    uint64_t memory_size;
    uint32_t phdr_id() const { return uint32_t((flags >> 20) & 0xFFF); }
    bool blocked() const { return flags & 0x800; }
    bool compressed() const { return flags & 0x8; }
    bool encrypted() const { return flags & 0x2; }
};

struct SelfInfo {
    bool is_self = false;
    uint16_t num_segments = 0;
    uint64_t elf_offset = 0;
    std::vector<SelfSegmentInfo> segments;
    std::vector<std::string> warnings;
};

// Accepts either a SELF (magic 4F 15 3D 1D) or an already-plain ELF; returns the ELF bytes.
bool SelfToElf(std::span<const uint8_t> file, std::vector<uint8_t>& elf, SelfInfo* info, std::string* err);

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out);
bool WriteWholeFile(const std::string& path, std::span<const uint8_t> data);

} // namespace gr
