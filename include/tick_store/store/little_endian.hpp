#pragma once

// Bounds-checked little-endian access, for the store's own file format. ITCH bytes
// stay big-endian and go through itch/big_endian.hpp. The two properties are the
// same as there: the check runs before the first byte is touched, and values are
// assembled a byte at a time, so no typed load and no host order.

#include "tick_store/itch/big_endian.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tick_store::store {

// The Width-byte unsigned integer at `offset`, or nullopt if it does not fit.
template <std::size_t Width>
    requires(Width >= 1 && Width <= 8)
[[nodiscard]] constexpr std::optional<std::uint64_t> read_le(std::span<const std::byte> bytes,
                                                             std::size_t offset) noexcept {
    if (!itch::detail::in_bounds(offset, Width, bytes.size())) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t i = Width; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + i - 1]);
    }
    return value;
}

// Writes the low Width bytes of `value`, least significant first. A write that
// does not fit changes nothing.
template <std::size_t Width>
    requires(Width >= 1 && Width <= 8)
[[nodiscard]] constexpr bool write_le(std::span<std::byte> bytes, std::size_t offset,
                                      std::uint64_t value) noexcept {
    if (!itch::detail::in_bounds(offset, Width, bytes.size())) {
        return false;
    }
    for (std::size_t i = 0; i < Width; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
    return true;
}

}  // namespace tick_store::store
