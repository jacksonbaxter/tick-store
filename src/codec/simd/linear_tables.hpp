#pragma once

// The linear layout's byte shuffle, one entry per width 1 to kLinearSimdWidest: eight
// values take exactly `width` bytes, so their bit offsets repeat every eight values. For
// each pair of values (2p, 2p + 1) the entry gives the byte to load 16 from, relative to
// the eight values' first byte, the shuffle that puts each value's eight bytes in its
// 64-bit lane, and each value's right shift. A value of up to kLinearSimdWidest bits
// starts at most 7 bits into its first byte, so eight bytes hold it, and a pair's sixteen
// bytes hold both. Index 0xFF is never used. Private to the library, in an unnamed
// namespace for the reason driver.hpp gives.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace tick_store::codec::simd::detail {
// NOLINTNEXTLINE(misc-anonymous-namespace-in-header,cert-dcl59-cpp): one copy per instruction set
namespace {

inline constexpr unsigned kLinearSimdWidest = 56;

struct LinearPair {
    std::uint8_t load = 0;
    std::array<std::uint8_t, 16> shuffle{};
    std::array<std::uint64_t, 2> shift{};
};

struct LinearWidth {
    std::array<LinearPair, 4> pairs{};
    // From the eight values' first byte, one past the last byte any pair's load reads.
    std::size_t reach = 0;
};

inline constexpr auto kLinearTable = [] {
    std::array<LinearWidth, kLinearSimdWidest + 1U> table{};
    for (unsigned w = 1; w <= kLinearSimdWidest; ++w) {
        for (unsigned p = 0; p < 4U; ++p) {
            LinearPair& pair = table[w].pairs[p];
            const unsigned first = (2U * p) * w;
            pair.load = static_cast<std::uint8_t>(first / 8U);
            for (unsigned lane = 0; lane < 2U; ++lane) {
                const unsigned bit = (2U * p + lane) * w;
                const unsigned at = (bit / 8U) - pair.load;
                for (unsigned b = 0; b < 8U; ++b) {
                    pair.shuffle[(lane * 8U) + b] = static_cast<std::uint8_t>(at + b);
                }
                pair.shift[lane] = bit % 8U;
            }
            table[w].reach = std::max<std::size_t>(table[w].reach, std::size_t{pair.load} + 16U);
        }
    }
    return table;
}();

}  // namespace
}  // namespace tick_store::codec::simd::detail
