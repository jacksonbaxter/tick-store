#pragma once

// Bit-granular writer and bounds-checked reader for the column codecs. Bits go most
// significant first, so a stream reads left to right as the Gorilla paper draws it.
//
// The reader is the only code that touches a codec's payload bytes (bar D1). Three
// properties hold for every read:
//
// - The width is checked against 64, and then against the bits left, before any
//   byte is touched. End of input is the typed `truncated` error, never zeros.
// - No shift reaches the type's width. Width 0 returns 0 without reading, and width
//   64 takes the whole word without a shift by 64.
// - No typed load. Bytes are assembled one at a time, so alignment and host order do
//   not matter.
//
// Over-read contract: the reader reads only inside the span it was given and needs no
// padding after it. Lifetime: it borrows that span.

#include "tick_store/codec/codec_error.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::codec {

inline constexpr unsigned kWordBits = 64;

// The low `width` bits set, for width 0 to 64.
[[nodiscard]] constexpr std::uint64_t low_bits(unsigned width) noexcept {
    return width >= kWordBits ? std::numeric_limits<std::uint64_t>::max()
                              : (std::uint64_t{1} << width) - 1U;
}

// Appends bits to the end of a byte vector. The vector may already hold bytes, such
// as a block header written before the payload.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::byte>& out) noexcept : out_(&out) {}

    // Appends the low `width` bits of `value`, most significant first. Bits above
    // `width` are ignored. Width 0 appends nothing. A width above 64 appends nothing
    // and makes finish() report bad_width, so an encoder bug cannot pass silently.
    void write(std::uint64_t value, unsigned width) {
        if (width == 0) {
            return;
        }
        if (width > kWordBits) {
            if (failed_width_ == 0) {
                failed_width_ = width;
            }
            return;
        }
        value &= low_bits(width);
        const unsigned free = kWordBits - pending_bits_;  // 1 to 64
        if (width < free) {
            // pending_bits_ + width < 64, so the shift is below the word size.
            pending_ = (pending_ << width) | value;
            pending_bits_ += width;
            bits_ += width;
            return;
        }
        // The word fills. `rest` bits of `value` start the next word.
        const unsigned rest = width - free;  // 0 to 63
        const std::uint64_t head = free == kWordBits ? 0 : pending_ << free;
        flush_word(head | (value >> rest));
        pending_ = value & low_bits(rest);
        pending_bits_ = rest;
        bits_ += width;
    }

    void write_bit(bool bit) { write(bit ? 1U : 0U, 1); }

    // Bits written so far, before padding.
    [[nodiscard]] std::uint64_t bit_count() const noexcept { return bits_; }

    // Pads with zero bits to a whole byte and appends what is left. Returns the bytes
    // this writer appended, or bad_width if any write asked for more than 64 bits.
    [[nodiscard]] std::expected<std::uint64_t, CodecError> finish() {
        if (failed_width_ != 0) {
            return std::unexpected(codec_error(CodecErrorKind::bad_width, "bit write", bits_,
                                               kWordBits, failed_width_));
        }
        // pending_bits_ is below 64, so the final bytes fit in one word.
        const unsigned bytes = (pending_bits_ + 7U) / 8U;
        const std::uint64_t padded = pending_ << ((bytes * 8U) - pending_bits_);
        for (unsigned i = bytes; i > 0; --i) {
            out_->push_back(static_cast<std::byte>((padded >> (8U * (i - 1U))) & 0xFFU));
        }
        pending_ = 0;
        pending_bits_ = 0;
        return (bits_ + 7U) / 8U;
    }

private:
    void flush_word(std::uint64_t word) {
        for (unsigned i = 8; i > 0; --i) {
            out_->push_back(static_cast<std::byte>((word >> (8U * (i - 1U))) & 0xFFU));
        }
    }

    std::vector<std::byte>* out_;
    std::uint64_t pending_ = 0;
    unsigned pending_bits_ = 0;  // always below 64
    unsigned failed_width_ = 0;
    std::uint64_t bits_ = 0;
};

class BitReader {
public:
    // `base_offset` is the bit offset of the span's first byte in whatever the caller
    // reports errors against, such as the block that holds the payload.
    explicit BitReader(std::span<const std::byte> bytes, std::uint64_t base_offset = 0) noexcept
        : bytes_(bytes),
          // A span cannot reach 2^61 bytes in a 64-bit address space, but the product is
          // capped anyway, so it cannot wrap.
          size_bits_(std::min<std::uint64_t>(bytes.size(), kMaxBytes) * 8U),
          base_(base_offset) {}

    // The next `width` bits as an unsigned value, most significant first.
    [[nodiscard]] std::expected<std::uint64_t, CodecError> read(unsigned width,
                                                                std::string_view field) noexcept {
        if (width == 0) {
            return std::uint64_t{0};
        }
        if (width > kWordBits) {
            return std::unexpected(
                codec_error(CodecErrorKind::bad_width, field, base_ + pos_, kWordBits, width));
        }
        if (width > size_bits_ - pos_) {
            return std::unexpected(codec_error(CodecErrorKind::truncated, field, base_ + pos_,
                                               width, size_bits_ - pos_));
        }
        const auto first = static_cast<std::size_t>(pos_ / 8U);
        const auto skip = static_cast<unsigned>(pos_ % 8U);
        pos_ += width;
        if (bytes_.size() - first > 8) {
            // Nine bytes are in bounds: the word at `first`, then one more byte for the
            // up to 7 bits a misaligned start pushes out. skip < 8, 8 - skip > 0, and
            // 64 - width < 64, so every shift is below the word size.
            std::uint64_t word = 0;
            for (std::size_t i = 0; i < 8; ++i) {
                word = (word << 8U) | std::to_integer<std::uint64_t>(bytes_[first + i]);
            }
            if (skip != 0) {
                word = (word << skip) |
                       (std::to_integer<std::uint64_t>(bytes_[first + 8]) >> (8U - skip));
            }
            return width == kWordBits ? word : word >> (kWordBits - width);
        }
        // Near the end, a byte at a time. Each step takes at most 8 bits, and the value
        // never holds more than 64, so no shift reaches 64.
        std::uint64_t value = 0;
        unsigned need = width;
        std::size_t at = first;
        unsigned offset = skip;
        while (need > 0) {
            const unsigned available = 8U - offset;
            const unsigned take = std::min(available, need);
            const auto byte = std::to_integer<unsigned>(bytes_[at]);
            const unsigned chunk = (byte >> (available - take)) & ((1U << take) - 1U);
            value = (value << take) | chunk;
            need -= take;
            ++at;
            offset = 0;
        }
        return value;
    }

    [[nodiscard]] std::expected<bool, CodecError> read_bit(std::string_view field) noexcept {
        const auto bit = read(1, field);
        if (!bit.has_value()) {
            return std::unexpected(bit.error());
        }
        return *bit != 0;
    }

    // A fast peek for a decoder's inner loop: the next `width` bits, for a
    // width the caller has already proved is at most remaining() and at most
    // kMaxPeekBits. Outside those it returns 0 and reads nothing, so a caller that skips
    // the proof gets a wrong value, never a read out of bounds. Inline, and with no
    // error to build, so a loop over it costs a few instructions a value.
    static constexpr unsigned kMaxPeekBits = 57;
    [[nodiscard, gnu::always_inline]] std::uint64_t peek_within(unsigned width) const noexcept {
        if (width == 0 || width > kMaxPeekBits || width > size_bits_ - pos_) {
            return 0;
        }
        const auto first = static_cast<std::size_t>(pos_ / 8U);
        const auto skip = static_cast<unsigned>(pos_ % 8U);
        std::uint64_t word = 0;
        if (bytes_.size() - first >= 8) {
            // skip + width <= 7 + 57 = 64, so the eight bytes at `first` hold every bit.
            // Copied, not loaded as a type, then put in stream order.
            std::memcpy(&word, bytes_.data() + first, sizeof word);
            if constexpr (std::endian::native == std::endian::little) {
                word = std::byteswap(word);
            }
        } else {
            // Near the end, the bytes left, then zeros past them, which the width proved
            // above never reaches.
            for (std::size_t i = 0; i < 8; ++i) {
                const std::size_t at = first + i;
                word = (word << 8U) |
                       (at < bytes_.size() ? std::to_integer<std::uint64_t>(bytes_[at]) : 0U);
            }
        }
        // skip < 8 and 64 - width < 64, so both shifts are below the word size.
        return (word << skip) >> (kWordBits - width);
    }

    // Consumes `width` bits after peek_within() proved them present: the caller has
    // checked width <= remaining(). A wider one consumes nothing.
    void advance_within(unsigned width) noexcept {
        if (width <= size_bits_ - pos_) {
            pos_ += width;
        }
    }

    // Bits read so far.
    [[nodiscard]] std::uint64_t position() const noexcept { return pos_; }

    // Bits not yet read.
    [[nodiscard]] std::uint64_t remaining() const noexcept { return size_bits_ - pos_; }

    // Succeeds only if what is left is the zero padding of the last byte: fewer than
    // 8 bits, all zero. Anything else is trailing_bits, so every byte a block claims
    // is accounted for (D9).
    [[nodiscard]] std::expected<void, CodecError> finish(std::string_view field) noexcept {
        const std::uint64_t left = remaining();
        if (left >= 8) {
            return std::unexpected(
                codec_error(CodecErrorKind::trailing_bits, field, base_ + pos_, 0, left));
        }
        const std::uint64_t at = pos_;
        const auto padding = read(static_cast<unsigned>(left), field);
        if (!padding.has_value()) {
            return std::unexpected(padding.error());
        }
        if (*padding != 0) {
            return std::unexpected(
                codec_error(CodecErrorKind::trailing_bits, field, base_ + at, 0, *padding));
        }
        return {};
    }

private:
    static constexpr std::uint64_t kMaxBytes = std::uint64_t{1} << 60U;

    std::span<const std::byte> bytes_;
    std::uint64_t size_bits_;
    std::uint64_t base_;
    std::uint64_t pos_ = 0;
};

}  // namespace tick_store::codec
