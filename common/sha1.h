#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace gr {
std::array<uint8_t, 20> Sha1(std::span<const uint8_t> data);
}
