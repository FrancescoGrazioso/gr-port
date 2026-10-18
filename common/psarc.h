#pragma once
// Reader for PlayStation ARChive (PSARC v1.x, zlib) as used in uroot/arc/*.psarc.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gr {

class Psarc {
public:
    struct Entry {
        uint8_t md5[16];
        uint32_t first_block;
        uint64_t size;   // uncompressed
        uint64_t offset; // absolute file offset of first block
        std::string name;
    };

    ~Psarc();
    bool Open(const std::string& path, std::string* err);
    const std::vector<Entry>& Entries() const { return entries_; } // excludes the manifest
    bool Read(const Entry& e, std::vector<uint8_t>& out, std::string* err);
    const std::string& Compression() const { return compression_; }
    uint32_t BlockSize() const { return block_size_; }

private:
    bool ReadRaw(const Entry& e, std::vector<uint8_t>& out, std::string* err);

    FILE* f_ = nullptr;
    std::string compression_;
    uint32_t block_size_ = 0;
    std::vector<uint32_t> block_sizes_;
    std::vector<Entry> entries_;
};

} // namespace gr
