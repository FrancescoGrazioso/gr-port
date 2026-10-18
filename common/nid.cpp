#include "nid.h"

#include <fstream>
#include <vector>

#include "sha1.h"

namespace gr {
namespace {
constexpr uint8_t kNidSuffix[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                    0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
} // namespace

std::string ComputeNid(std::string_view symbol_name) {
    std::vector<uint8_t> buf(symbol_name.begin(), symbol_name.end());
    buf.insert(buf.end(), std::begin(kNidSuffix), std::end(kNidSuffix));
    const auto digest = Sha1(buf);
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = digest[7 - i]; // little-endian u64 written big-endian
    // 8 bytes -> 11 base64 chars (last group has 2 bytes -> 3 chars, padding dropped)
    std::string out;
    for (int i = 0; i < 8; i += 3) {
        uint32_t v = uint32_t(b[i]) << 16;
        if (i + 1 < 8) v |= uint32_t(b[i + 1]) << 8;
        if (i + 2 < 8) v |= b[i + 2];
        const int chars = (i + 2 < 8) ? 4 : 3;
        for (int c = 0; c < chars; ++c) out.push_back(kAlphabet[(v >> (18 - 6 * c)) & 63]);
    }
    return out;
}

uint16_t DecodeNidIndex(std::string_view s) {
    uint32_t v = 0;
    for (char c : s) {
        int d = -1;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '-') d = 63;
        if (d < 0) return 0xFFFF;
        v = (v << 6) | uint32_t(d);
    }
    return uint16_t(v);
}

void NidDatabase::AddName(std::string_view name) { map_.emplace(ComputeNid(name), std::string(name)); }

size_t NidDatabase::LoadNamesFile(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    const size_t before = map_.size();
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        AddName(std::string_view(line).substr(start));
    }
    return map_.size() - before;
}

const std::string* NidDatabase::Lookup(std::string_view nid) const {
    auto it = map_.find(std::string(nid));
    return it == map_.end() ? nullptr : &it->second;
}

} // namespace gr
