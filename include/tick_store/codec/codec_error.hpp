#pragma once

// The typed error of the column codecs: what went wrong and where.

#include <cstdint>
#include <string>
#include <string_view>

namespace tick_store::codec {

enum class CodecErrorKind : std::uint8_t {
    // The input ended before `field`.
    truncated,
    // A bit read or write wider than 64 bits.
    bad_width,
    // A block's value count is zero, above kMaxBlockValues, more than its payload
    // can hold, or more than the column has left.
    bad_count,
    // Bits after a block's last value: padding that is not zero, or a spare byte.
    trailing_bits,
    // A value stored in a form the encoder never writes. Every value has exactly one
    // encoding, so a decoded block re-encodes to the same bytes.
    non_canonical,
    // An XOR window that cannot be used: leading zeros plus data bits above 64, or a
    // reuse before any window exists.
    bad_window,
    // An encoder was given a parameter outside its range.
    bad_parameter,
    // A double that is not exactly the stored form of an integer price in range.
    not_exact_price,
};

// `offset` is in bits from the first byte of the input the decoder was given: a
// block for a block decoder, the whole column for a column decoder. Header fields are
// at whole bytes, so their offsets are multiples of 8. `expected` and `actual` are
// the values in conflict:
//
//   truncated        expected = bits `field` needs, actual = bits left
//   bad_width        expected = 64, actual = the width asked for
//   bad_count        expected = the largest count allowed (or 1 for an empty block),
//                    actual = the count found
//   trailing_bits    expected = 0, actual = the bits left after the last value, or the
//                    padding's value if it is short of a byte but not zero
//   non_canonical    expected = the width or form the encoder would have used,
//                    actual = the one found
//   bad_window       expected = 64, actual = leading zeros plus data bits; or for a
//                    reuse with no window, expected = 1 and actual = 0
//   bad_parameter    expected = the largest value allowed, actual = the value given
//   not_exact_price  expected = 0, actual = the double's bit pattern
//
// `field` points at static storage, so an error outlives its input.
struct CodecError {
    CodecErrorKind kind = CodecErrorKind::truncated;
    std::string_view field;
    std::uint64_t offset = 0;
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;

    friend bool operator==(const CodecError&, const CodecError&) = default;
};

[[nodiscard]] std::string to_string(const CodecError& error);

// Builds an error on a cold path, so the decoders' hot loops stay small.
[[gnu::cold, gnu::noinline]] CodecError codec_error(CodecErrorKind kind, std::string_view field,
                                                    std::uint64_t offset, std::uint64_t expected,
                                                    std::uint64_t actual) noexcept;

}  // namespace tick_store::codec
