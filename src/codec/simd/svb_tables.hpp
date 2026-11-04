#pragma once

// Stream VByte's tables for a byte shuffle, one entry per control byte: which data byte
// goes to each byte of four 32-bit values, and the least value each length may hold.
// Index 0xFF selects zero in NEON's tbl and in SSSE3's pshufb alike. Private to the
// library, in an unnamed namespace for the reason driver.hpp gives.

#include <array>
#include <cstddef>
#include <cstdint>

namespace tick_store::codec::simd::detail {
// NOLINTNEXTLINE(misc-anonymous-namespace-in-header,cert-dcl59-cpp): one copy per instruction set
namespace {

inline constexpr std::size_t kSvbControls = 256;

inline constexpr auto kSvbShuffle = [] {
    std::array<std::array<std::uint8_t, 16>, kSvbControls> table{};
    for (unsigned c = 0; c < kSvbControls; ++c) {
        unsigned offset = 0;
        for (unsigned lane = 0; lane < 4U; ++lane) {
            const unsigned length = ((c >> (2U * lane)) & 3U) + 1U;
            for (unsigned b = 0; b < 4U; ++b) {
                table[c][(lane * 4U) + b] =
                    b < length ? static_cast<std::uint8_t>(offset + b) : std::uint8_t{0xFF};
            }
            offset += length;
        }
    }
    return table;
}();

// A value coded in length L > 1 bytes must have a byte L - 1 that is not zero, so it is
// at least 2^(8 (L - 1)); a value of one byte may be anything.
inline constexpr auto kSvbLeast = [] {
    std::array<std::array<std::uint32_t, 4>, kSvbControls> table{};
    for (unsigned c = 0; c < kSvbControls; ++c) {
        for (unsigned lane = 0; lane < 4U; ++lane) {
            const unsigned code = (c >> (2U * lane)) & 3U;
            table[c][lane] = code == 0 ? 0U : std::uint32_t{1} << (8U * code);
        }
    }
    return table;
}();

}  // namespace
}  // namespace tick_store::codec::simd::detail
