#include "tick_store/codec/price_double.hpp"

#include "tick_store/codec/codec_error.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>

namespace tick_store::codec {

namespace {

// 2^64, exact as a double. Converting a double at or above it to uint64_t is undefined.
constexpr double kTwoTo64 = 18446744073709551616.0;

}  // namespace

double scaled_to_double(std::uint64_t value, std::uint64_t scale) noexcept {
    return static_cast<double>(value) / static_cast<double>(scale);
}

std::expected<std::uint64_t, CodecError> double_to_scaled(double value, std::uint64_t scale,
                                                          std::uint64_t max) noexcept {
    const auto refuse = [value] {
        return std::unexpected(codec_error(CodecErrorKind::not_exact_price, "price as double", 0,
                                           0, std::bit_cast<std::uint64_t>(value)));
    };
    // The range check comes before the conversion, which is undefined outside [0, 2^64).
    const double rounded = std::round(value * static_cast<double>(scale));
    if (std::isnan(rounded) || rounded < 0.0 || rounded >= kTwoTo64) {
        return refuse();
    }
    const auto integer = static_cast<std::uint64_t>(rounded);
    if (integer > max ||
        std::bit_cast<std::uint64_t>(scaled_to_double(integer, scale)) !=
            std::bit_cast<std::uint64_t>(value)) {
        return refuse();
    }
    return integer;
}

}  // namespace tick_store::codec
