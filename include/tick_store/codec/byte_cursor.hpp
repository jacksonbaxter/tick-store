#pragma once

// Byte-granular fields for the packed codecs, which lay their headers out on whole bytes
// so the packed words after them can be loaded without bit parsing.
//
// ByteCursor is the only code that reads those fields (bar D1). Every take is checked
// against the bytes left before anything is touched, and the end of input is the typed
// `truncated` error. Multi-byte fields are little-endian and assembled a byte at a time,
// so alignment and host order do not matter. Error offsets are in bits, like the bit
// reader's, from whatever `base_bits` names, such as the first byte of a block.
//
// Over-read contract: the cursor reads only inside the span it was given. Lifetime: it
// borrows that span, and so do the spans take() returns.

#include "tick_store/codec/codec_error.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::codec {

class ByteCursor {
public:
    explicit ByteCursor(std::span<const std::byte> bytes, std::uint64_t base_bits = 0) noexcept
        : bytes_(bytes), base_bits_(base_bits) {}

    // The next `count` bytes.
    [[nodiscard]] std::expected<std::span<const std::byte>, CodecError> take(
        std::size_t count, std::string_view field) noexcept {
        if (count > remaining()) {
            return std::unexpected(truncated(count, field));
        }
        const std::span<const std::byte> taken = bytes_.subspan(pos_, count);
        pos_ += count;
        return taken;
    }

    // An unsigned little-endian field of `size` bytes, 0 to 8. Size 0 reads nothing and
    // gives 0.
    [[nodiscard]] std::expected<std::uint64_t, CodecError> read_le(unsigned size,
                                                                   std::string_view field) noexcept {
        if (size > 8) {
            return std::unexpected(codec_error(CodecErrorKind::bad_width, field, bit_offset(), 64,
                                               std::uint64_t{size} * 8U));
        }
        const auto taken = take(size, field);
        if (!taken.has_value()) {
            return std::unexpected(taken.error());
        }
        std::uint64_t value = 0;
        for (unsigned i = size; i > 0; --i) {
            value = (value << 8U) | std::to_integer<std::uint64_t>((*taken)[i - 1]);
        }
        return value;
    }

    // Bit offset of the next byte, for errors.
    [[nodiscard]] std::uint64_t bit_offset() const noexcept { return base_bits_ + (pos_ * 8U); }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - pos_; }

private:
    [[nodiscard]] CodecError truncated(std::size_t count, std::string_view field) const noexcept {
        // Spans hold far fewer than 2^61 bytes, so neither product wraps.
        return codec_error(CodecErrorKind::truncated, field, bit_offset(),
                           static_cast<std::uint64_t>(count) * 8U,
                           static_cast<std::uint64_t>(remaining()) * 8U);
    }

    std::span<const std::byte> bytes_;
    std::uint64_t base_bits_;
    std::size_t pos_ = 0;
};

// The fewest bytes that hold `value`: 0 for 0, up to 8.
[[nodiscard]] constexpr unsigned bytes_for(std::uint64_t value) noexcept {
    return (static_cast<unsigned>(std::bit_width(value)) + 7U) / 8U;
}

// Appends the low `size` bytes of `value`, 0 to 8, little-endian.
inline void append_le(std::vector<std::byte>& out, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size && i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
    }
}

}  // namespace tick_store::codec
