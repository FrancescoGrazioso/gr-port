#pragma once
// Minimal DEFLATE / zlib decoder (RFC 1950/1951). No external dependencies.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gr {

// Decodes a raw DEFLATE stream into `out` (appended). Returns false on malformed input.
bool Inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out);

// Decodes a zlib-wrapped stream (2-byte header + deflate + adler32).
bool ZlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out);

// True if the first two bytes form a valid zlib header.
bool LooksLikeZlib(std::span<const uint8_t> in);

} // namespace gr
