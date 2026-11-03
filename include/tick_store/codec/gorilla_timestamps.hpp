#pragma once

// Gorilla delta-of-delta timestamps (Pelkonen et al., VLDB 2015, section 4.1.1), with
// buckets chosen for ITCH's nanoseconds.
//
// A block is the frame in block.hpp around this bit stream:
//
//   the first value, 64 bits
//   then for each later value, D = (t[i] - t[i-1]) - (t[i-1] - t[i-2]), with the delta
//   before the first taken as 0, so the first delta is coded as a delta-of-delta too:
//
//     '0'                          D = 0
//     '10'   then 16 bits          D in [-2^15, 2^15 - 1]
//     '110'  then 19 bits          D in [-2^18, 2^18 - 1]
//     '1110' then 23 bits          D in [-2^22, 2^22 - 1]
//     '1111' then 64 bits          anything else
//
//   each field holding D in two's complement, and each D in the narrowest bucket it fits.
//
// The control codes are Gorilla's. The widths are not: Gorilla's 7, 9 and 12 bits were
// chosen for seconds, and Prometheus widened them to 14, 17 and 20 for milliseconds.
// ITCH's gaps are nanoseconds. On both pinned days the file-order delta-of-delta has its
// mode at 2^13 to 2^15 ns and only 0.75% zeros (results/census-*.json). An exhaustive
// search over three widths, costing each census bucket at the bits a two's-complement
// field needs, puts 16, 19 and 23 first on the sum of both days, at about 20.38 bits per
// value; Prometheus's widths cost 22.3 and Gorilla's 59.1 on the same histograms. The
// search is in NOTES.md, "Gorilla on ITCH".
//
// All arithmetic is on uint64_t and wraps modulo 2^64 (D5), so every sequence of 64-bit
// values round-trips, including a step backwards or a jump across the whole range, and
// the 64-bit escape needs no special case. ITCH timestamps fit 48 bits, so the escape
// is rare; a codec limited to them could narrow it.
//
// Canonical form: a decoder refuses a D stored in a wider bucket than it needs, and a
// zero stored anywhere but '0'. So a block that decodes re-encodes to the same bytes.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

// Bits of D in each bucket after the zero bucket, narrowest first.
inline constexpr std::array<unsigned, 4> kDodBucketBits{16, 19, 23, 64};

// The fewest payload bits a value after the first can take, and what the first takes.
inline constexpr std::uint64_t kDodMinBitsPerValue = 1;
inline constexpr std::uint64_t kDodFirstValueBits = 64;

// How a block's values fell into the buckets: zero, then kDodBucketBits in order.
struct DodStats {
    std::array<std::uint64_t, 5> buckets{};
};

// Appends one block holding `values`, which must hold 1 to kMaxBlockValues values.
[[nodiscard]] std::expected<void, CodecError> encode_timestamp_block(
    std::span<const std::uint64_t> values, std::vector<std::byte>& out,
    DodStats* stats = nullptr);

// Decodes the block at the front of `block`, appending exactly its count of values to
// `out`, and returns the bytes it took. On error, `out` keeps what it held before.
[[nodiscard]] std::expected<std::size_t, CodecError> decode_timestamp_block(
    std::span<const std::byte> block, std::vector<std::uint64_t>& out);

// Appends `values` as consecutive blocks of `block_values`, the last one shorter.
[[nodiscard]] std::expected<void, CodecError> encode_timestamps(
    std::span<const std::uint64_t> values, std::size_t block_values, std::vector<std::byte>& out,
    DodStats* stats = nullptr);

// Decodes a whole column that must hold exactly `count` values, appending them to
// `out`. A column cut at any byte, a block too many or a value too many is an error.
[[nodiscard]] std::expected<void, CodecError> decode_timestamps(std::span<const std::byte> column,
                                                                std::uint64_t count,
                                                                std::vector<std::uint64_t>& out);

}  // namespace tick_store::codec
