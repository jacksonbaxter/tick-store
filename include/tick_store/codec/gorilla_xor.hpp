#pragma once

// Gorilla XOR compression (Pelkonen et al., VLDB 2015, section 4.1.2) on 64-bit words.
// A double goes in as its IEEE-754 bit pattern, so the same code serves XOR on doubles
// and XOR on raw integers; price_double.hpp maps prices to doubles.
//
// A block is the frame in block.hpp around this bit stream:
//
//   the first word, 64 bits
//   then for each later word, X = word ^ previous word:
//
//     '0'                                    X = 0
//     '10' then the window's data bits       X fits the current window
//     '11' then 5 bits of leading zeros L,   a new window
//          6 bits of data bits minus one, B - 1,
//          then the B data bits
//
// A window is (L, B): X's set bits lie inside bits [64 - L - B, 64 - L) of the word,
// counting from the least significant. The encoder reuses the window whenever X fits
// it, that is, has at least L leading and 64 - L - B trailing zeros, and otherwise
// opens a new one with L = min(leading zeros of X, 31) and B running down to X's
// lowest set bit. These are Gorilla's rules. L is capped at 31 because it has 5 bits;
// B - 1 is stored, not B, so B = 64 is representable and B = 0 is not.
//
// The decoder checks the window before any shift (D4): L + B above 64 is refused, and
// so is a reuse before any window exists. Canonical form: it also refuses anything the
// encoder would not have written, so a block that decodes re-encodes to the same bytes:
// a new window where the old one fitted, an L that is not X's leading zeros (below the
// cap), data bits that do not end in a set bit, and a reuse whose X is zero.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

inline constexpr unsigned kXorLeadingBits = 5;
inline constexpr unsigned kXorLengthBits = 6;
inline constexpr unsigned kXorMaxLeading = (1U << kXorLeadingBits) - 1U;  // 31
inline constexpr std::uint64_t kXorMinBitsPerValue = 1;
inline constexpr std::uint64_t kXorFirstValueBits = 64;

// How a block's words were coded, for explaining a ratio.
struct XorStats {
    std::uint64_t zero = 0;
    std::uint64_t reuse = 0;
    std::uint64_t new_window = 0;
    // Data bits written, over reuses and new windows.
    std::uint64_t data_bits = 0;
};

// Appends one block holding `words`, which must hold 1 to kMaxBlockValues words.
[[nodiscard]] std::expected<void, CodecError> encode_xor_block(std::span<const std::uint64_t> words,
                                                               std::vector<std::byte>& out,
                                                               XorStats* stats = nullptr);

// Decodes the block at the front of `block`, appending exactly its count of words to
// `out`, and returns the bytes it took. On error, `out` keeps what it held before.
[[nodiscard]] std::expected<std::size_t, CodecError> decode_xor_block(
    std::span<const std::byte> block, std::vector<std::uint64_t>& out);

// Appends `words` as consecutive blocks of `block_values`, the last one shorter.
[[nodiscard]] std::expected<void, CodecError> encode_xor(std::span<const std::uint64_t> words,
                                                         std::size_t block_values,
                                                         std::vector<std::byte>& out,
                                                         XorStats* stats = nullptr);

// Decodes a whole column that must hold exactly `count` words, appending them to `out`.
[[nodiscard]] std::expected<void, CodecError> decode_xor(std::span<const std::byte> column,
                                                         std::uint64_t count,
                                                         std::vector<std::uint64_t>& out);

// The same column as doubles, each stored as its bit pattern, so NaN payloads and the
// sign of zero survive.
[[nodiscard]] std::expected<void, CodecError> encode_xor_doubles(std::span<const double> values,
                                                                 std::size_t block_values,
                                                                 std::vector<std::byte>& out,
                                                                 XorStats* stats = nullptr);

[[nodiscard]] std::expected<void, CodecError> decode_xor_doubles(std::span<const std::byte> column,
                                                                 std::uint64_t count,
                                                                 std::vector<double>& out);

}  // namespace tick_store::codec
