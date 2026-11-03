#pragma once

// ITCH prices are fixed-point integers: Price(4) is a uint32 count of $0.0001 and
// Price(8), used only by the MWCB levels, a uint64 count of $0.00000001. A system that
// stores prices as doubles stores the dollar value, price / 10^d, so that is what XOR on
// doubles is measured on. The other mapping stores the integer itself as a double.
//
// Each mapping has an inverse that succeeds only if the double is bit for bit what the
// forward mapping gives for some integer in range, and then returns that integer. So a
// column that decodes through the inverse holds exactly the integers that went in, or
// the decode names the first double that is not one: nothing is rounded away quietly.
//
// Forward: the integer is converted to the nearest double, then divided by 10^d, and
// IEEE-754 division is correctly rounded. Every uint32 is exact as a double, so for
// Price(4) the result is the double nearest the decimal value, which is also what
// parsing the decimal text would give. tools/tick_codec_eval checks the inverse on all
// 2^32 Price(4) values. A uint64 above 2^53 is not exact as a double, so Price(8) and
// the integer mapping can fail for one; on real data the check reports any that do.

#include "tick_store/codec/codec_error.hpp"

#include <cstdint>
#include <expected>
#include <limits>

namespace tick_store::codec {

inline constexpr std::uint64_t kPrice4Scale = 10'000;
inline constexpr std::uint64_t kPrice8Scale = 100'000'000;

// `value` / `scale` as a double. `scale` is 1, kPrice4Scale or kPrice8Scale.
[[nodiscard]] double scaled_to_double(std::uint64_t value, std::uint64_t scale) noexcept;

// The integer n, at most `max`, with scaled_to_double(n, scale) bit-identical to
// `value`, or not_exact_price carrying the double's bit pattern. NaN, infinities,
// negatives and -0.0 are never the image of an integer, so they are refused.
[[nodiscard]] std::expected<std::uint64_t, CodecError> double_to_scaled(
    double value, std::uint64_t scale, std::uint64_t max) noexcept;

[[nodiscard]] inline double price4_to_dollars(std::uint32_t price) noexcept {
    return scaled_to_double(price, kPrice4Scale);
}

[[nodiscard]] inline std::expected<std::uint64_t, CodecError> dollars_to_price4(
    double dollars) noexcept {
    return double_to_scaled(dollars, kPrice4Scale, std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] inline double price8_to_dollars(std::uint64_t price) noexcept {
    return scaled_to_double(price, kPrice8Scale);
}

[[nodiscard]] inline std::expected<std::uint64_t, CodecError> dollars_to_price8(
    double dollars) noexcept {
    return double_to_scaled(dollars, kPrice8Scale, std::numeric_limits<std::uint64_t>::max());
}

}  // namespace tick_store::codec
