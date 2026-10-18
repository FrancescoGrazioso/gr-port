#include "inflate.h"

#include <array>
#include <cstring>

namespace gr {
namespace {

struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int cnt = 0;
    bool overrun = false;

    int pad = 0; // zero bytes fed after the end of input

    void Refill() {
        while (cnt <= 56) {
            uint64_t b = 0;
            if (p < end) {
                b = *p++;
            } else if (++pad > 16) {
                overrun = true; // far past the real data: truncated stream
            }
            buf |= b << cnt;
            cnt += 8;
        }
    }
    uint32_t Peek(int n) {
        if (cnt < n) Refill();
        return static_cast<uint32_t>(buf & ((1ull << n) - 1));
    }
    void Drop(int n) {
        buf >>= n;
        cnt -= n;
    }
    uint32_t Get(int n) {
        if (n == 0) return 0;
        uint32_t v = Peek(n);
        Drop(n);
        return v;
    }
    void AlignByte() { Drop(cnt & 7); }
};

// Lookup-table Huffman decoder: table indexed by the next `max_len` bits (LSB-first).
struct Huffman {
    std::vector<uint16_t> table; // (symbol << 4) | length, 0 = invalid
    int max_len = 0;

    bool Build(const uint8_t* lengths, int n) {
        int count[16] = {};
        max_len = 0;
        for (int i = 0; i < n; ++i) {
            count[lengths[i]]++;
            if (lengths[i] > max_len) max_len = lengths[i];
        }
        if (max_len == 0) { // empty code (allowed for distance trees)
            max_len = 1;
            table.assign(2, 0);
            return true;
        }
        count[0] = 0;
        int next[16] = {};
        int code = 0;
        for (int len = 1; len <= 15; ++len) {
            code = (code + count[len - 1]) << 1;
            next[len] = code;
        }
        table.assign(size_t(1) << max_len, 0);
        for (int sym = 0; sym < n; ++sym) {
            int len = lengths[sym];
            if (!len) continue;
            int c = next[len]++;
            // reverse bits
            int r = 0;
            for (int i = 0; i < len; ++i) r |= ((c >> i) & 1) << (len - 1 - i);
            if (r >= (1 << max_len)) return false;
            for (int i = r; i < (1 << max_len); i += 1 << len)
                table[i] = static_cast<uint16_t>((sym << 4) | len);
        }
        return true;
    }

    int Decode(BitReader& br) const {
        uint16_t e = table[br.Peek(max_len)];
        if (!e) return -1;
        br.Drop(e & 15);
        return e >> 4;
    }
};

constexpr uint16_t kLenBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                  33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool InflateBlock(BitReader& br, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out,
                  size_t base) {
    for (;;) {
        int sym = lit.Decode(br);
        if (sym < 0 || br.overrun) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + br.Get(kLenExtra[sym]);
            int ds = dist.Decode(br);
            if (ds < 0 || ds >= 30) return false;
            size_t d = kDistBase[ds] + br.Get(kDistExtra[ds]);
            if (d > out.size() - base) return false;
            size_t from = out.size() - d;
            out.resize(out.size() + len);
            uint8_t* o = out.data() + out.size() - len;
            const uint8_t* s = out.data() + from;
            for (size_t i = 0; i < len; ++i) o[i] = s[i]; // overlapping copy is intended
        }
    }
}

} // namespace

bool Inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
    BitReader br{in.data(), in.data() + in.size()};
    const size_t base = out.size();
    Huffman lit, dist;
    int final_block = 0;
    do {
        final_block = br.Get(1);
        int type = br.Get(2);
        if (type == 0) {
            br.AlignByte();
            uint32_t len = br.Get(16);
            uint32_t nlen = br.Get(16);
            if ((len ^ 0xFFFF) != nlen) return false;
            for (uint32_t i = 0; i < len; ++i) out.push_back(static_cast<uint8_t>(br.Get(8)));
            if (br.overrun) return false;
        } else if (type == 1) {
            uint8_t l[288];
            std::memset(l, 8, 144);
            std::memset(l + 144, 9, 112);
            std::memset(l + 256, 7, 24);
            std::memset(l + 280, 8, 8);
            uint8_t d[30];
            std::memset(d, 5, 30);
            if (!lit.Build(l, 288) || !dist.Build(d, 30)) return false;
            if (!InflateBlock(br, lit, dist, out, base)) return false;
        } else if (type == 2) {
            int hlit = br.Get(5) + 257, hdist = br.Get(5) + 1, hclen = br.Get(4) + 4;
            static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                  11, 4,  12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; ++i) cl[order[i]] = static_cast<uint8_t>(br.Get(3));
            Huffman clh;
            if (!clh.Build(cl, 19)) return false;
            uint8_t lens[320] = {};
            int n = 0;
            while (n < hlit + hdist) {
                int sym = clh.Decode(br);
                if (sym < 0) return false;
                if (sym < 16) {
                    lens[n++] = static_cast<uint8_t>(sym);
                } else {
                    int rep = 0;
                    uint8_t val = 0;
                    if (sym == 16) {
                        if (n == 0) return false;
                        val = lens[n - 1];
                        rep = 3 + br.Get(2);
                    } else if (sym == 17) {
                        rep = 3 + br.Get(3);
                    } else {
                        rep = 11 + br.Get(7);
                    }
                    if (n + rep > hlit + hdist) return false;
                    while (rep--) lens[n++] = val;
                }
            }
            if (!lit.Build(lens, hlit) || !dist.Build(lens + hlit, hdist)) return false;
            if (!InflateBlock(br, lit, dist, out, base)) return false;
        } else {
            return false;
        }
    } while (!final_block);
    return true;
}

bool LooksLikeZlib(std::span<const uint8_t> in) {
    return in.size() >= 2 && (in[0] & 0x0F) == 8 && ((in[0] << 8) | in[1]) % 31 == 0;
}

bool ZlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
    if (!LooksLikeZlib(in)) return false;
    if (in[1] & 0x20) return false; // preset dictionary unsupported
    return Inflate(in.subspan(2), out);
}

} // namespace gr
