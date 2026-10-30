#pragma once

// Bounds-checked big-endian access to bytes. This is the only code in the
// project that touches ITCH wire bytes (bar D1 of the code-quality list), so
// everything above it inherits two properties:
//
// - No read or write ever goes outside the span it was given. The check runs
//   before the first byte is touched, and it cannot overflow.
// - No typed load. Values are assembled a byte at a time, so alignment and host
//   byte order do not matter, and no platform #ifdef is needed. Compilers fold
//   these loops into a single load and byte swap.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tick_store::itch {

namespace detail {

// True when [offset, offset + width) lies inside a buffer of `size` bytes. The
// subtraction is on the checked side of `offset <= size`, so it cannot wrap,
// whereas `offset + width <= size` could for an extreme offset.
[[nodiscard]] constexpr bool in_bounds(std::size_t offset, std::size_t width,
                                       std::size_t size) noexcept {
    return offset <= size && width <= size - offset;
}

}  // namespace detail

// Read-only view. It borrows the span and never outlives it by itself.
class BigEndianReader {
public:
    constexpr explicit BigEndianReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    // The Width-byte unsigned integer at `offset`, or nullopt if it does not fit.
    template <std::size_t Width>
        requires(Width >= 1 && Width <= 8)
    [[nodiscard]] constexpr std::optional<std::uint64_t> read_uint(std::size_t offset) const noexcept {
        if (!detail::in_bounds(offset, Width, bytes_.size())) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < Width; ++i) {
            value = (value << 8U) | std::to_integer<std::uint64_t>(bytes_[offset + i]);
        }
        return value;
    }

    // The single byte at `offset`, unchanged, or nullopt if it does not fit.
    [[nodiscard]] constexpr std::optional<std::byte> read_byte(std::size_t offset) const noexcept {
        if (!detail::in_bounds(offset, 1, bytes_.size())) {
            return std::nullopt;
        }
        return bytes_[offset];
    }

    // N bytes at `offset`, unchanged, or nullopt if they do not fit.
    template <std::size_t N>
    [[nodiscard]] constexpr std::optional<std::array<std::byte, N>> read_bytes(
        std::size_t offset) const noexcept {
        if (!detail::in_bounds(offset, N, bytes_.size())) {
            return std::nullopt;
        }
        std::array<std::byte, N> value{};
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = bytes_[offset + i];
        }
        return value;
    }

private:
    std::span<const std::byte> bytes_;
};

// Write-side counterpart. Each write reports whether it fitted, and a write that
// does not fit changes nothing.
class BigEndianWriter {
public:
    constexpr explicit BigEndianWriter(std::span<std::byte> bytes) noexcept : bytes_(bytes) {}

    // Writes the low Width bytes of `value`, most significant first. Any higher
    // bytes of `value` are dropped, so the caller proves the value fits first.
    template <std::size_t Width>
        requires(Width >= 1 && Width <= 8)
    [[nodiscard]] constexpr bool write_uint(std::size_t offset, std::uint64_t value) noexcept {
        if (!detail::in_bounds(offset, Width, bytes_.size())) {
            return false;
        }
        for (std::size_t i = 0; i < Width; ++i) {
            const std::size_t shift = 8U * (Width - 1U - i);
            bytes_[offset + i] = static_cast<std::byte>((value >> shift) & 0xFFU);
        }
        return true;
    }

    [[nodiscard]] constexpr bool write_byte(std::size_t offset, std::byte value) noexcept {
        if (!detail::in_bounds(offset, 1, bytes_.size())) {
            return false;
        }
        bytes_[offset] = value;
        return true;
    }

    // Copies `value` to `offset`. `value` must not overlap the destination.
    [[nodiscard]] constexpr bool write_bytes(std::size_t offset,
                                             std::span<const std::byte> value) noexcept {
        if (!detail::in_bounds(offset, value.size(), bytes_.size())) {
            return false;
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            bytes_[offset + i] = value[i];
        }
        return true;
    }

private:
    std::span<std::byte> bytes_;
};

}  // namespace tick_store::itch
