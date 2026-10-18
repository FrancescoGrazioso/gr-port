#include "psarc.h"

#include <algorithm>
#include <cstring>

#include "inflate.h"

namespace gr {
namespace {
uint64_t Be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}
} // namespace

Psarc::~Psarc() {
    if (f_) std::fclose(f_);
}

bool Psarc::Open(const std::string& path, std::string* err) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) { *err = "cannot open " + path; return false; }
    uint8_t h[32];
    if (std::fread(h, 1, 32, f_) != 32 || std::memcmp(h, "PSAR", 4) != 0) {
        *err = "not a PSARC file";
        return false;
    }
    compression_.assign(reinterpret_cast<char*>(h + 8), 4);
    const uint32_t toc_len = uint32_t(Be(h + 12, 4));
    const uint32_t entry_size = uint32_t(Be(h + 16, 4));
    const uint32_t count = uint32_t(Be(h + 20, 4));
    block_size_ = uint32_t(Be(h + 24, 4));
    const uint32_t flags = uint32_t(Be(h + 28, 4));
    if (flags & 4) { *err = "encrypted TOC not supported"; return false; }
    if (entry_size != 30 || toc_len < 32 + count * 30ull) { *err = "unexpected TOC layout"; return false; }

    std::vector<uint8_t> toc(toc_len - 32);
    if (std::fread(toc.data(), 1, toc.size(), f_) != toc.size()) { *err = "truncated TOC"; return false; }

    std::vector<Entry> all(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = toc.data() + i * 30ull;
        std::memcpy(all[i].md5, p, 16);
        all[i].first_block = uint32_t(Be(p + 16, 4));
        all[i].size = Be(p + 20, 5);
        all[i].offset = Be(p + 25, 5);
    }
    int width = 1;
    while ((1ull << (8 * width)) < block_size_) ++width;
    const uint8_t* bp = toc.data() + count * 30ull;
    const size_t nblocks = (toc.size() - count * 30ull) / width;
    block_sizes_.resize(nblocks);
    for (size_t i = 0; i < nblocks; ++i) block_sizes_[i] = uint32_t(Be(bp + i * width, width));

    if (count == 0) return true;
    std::vector<uint8_t> manifest;
    if (!Read(all[0], manifest, err)) return false;
    size_t pos = 0;
    for (uint32_t i = 1; i < count; ++i) {
        size_t nl = std::find(manifest.begin() + pos, manifest.end(), '\n') - manifest.begin();
        all[i].name.assign(reinterpret_cast<char*>(manifest.data()) + pos, nl - pos);
        if (!all[i].name.empty() && all[i].name.back() == '\r') all[i].name.pop_back();
        pos = std::min(nl + 1, manifest.size());
    }
    entries_.assign(all.begin() + 1, all.end());
    return true;
}

bool Psarc::Read(const Entry& e, std::vector<uint8_t>& out, std::string* err) {
    out.clear();
    if (compression_ != "zlib") { *err = "unsupported compression '" + compression_ + "'"; return false; }
    return ReadRaw(e, out, err);
}

bool Psarc::ReadRaw(const Entry& e, std::vector<uint8_t>& out, std::string* err) {
    out.reserve(e.size);
    uint64_t file_pos = e.offset;
    uint32_t block = e.first_block;
    std::vector<uint8_t> buf;
    while (out.size() < e.size) {
        if (block >= block_sizes_.size()) { *err = "block index out of range"; return false; }
        const uint64_t expected = std::min<uint64_t>(block_size_, e.size - out.size());
        uint64_t csize = block_sizes_[block] ? block_sizes_[block] : block_size_;
        buf.resize(csize);
        _fseeki64(f_, int64_t(file_pos), SEEK_SET);
        if (std::fread(buf.data(), 1, csize, f_) != csize) { *err = "truncated block data"; return false; }
        const size_t before = out.size();
        if (csize == expected || !LooksLikeZlib(buf)) {
            out.insert(out.end(), buf.begin(), buf.begin() + std::min<uint64_t>(csize, expected));
        } else if (!ZlibInflate(buf, out) || out.size() - before != expected) {
            *err = "zlib block failed to decode";
            return false;
        }
        file_pos += csize;
        ++block;
    }
    return true;
}

} // namespace gr
