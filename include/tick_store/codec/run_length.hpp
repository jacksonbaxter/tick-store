#pragma once

// Run-length coding for columns that hold one value for long stretches, such as tracking
// numbers and flags: each run of equal values as the value and its length.
//
// A block is the frame in block.hpp around this payload, little-endian throughout:
//
//   u32    runs R        1 to the block's value count n
//   ...    values        a PFOR block (pfor.hpp), its frame included, of the R run values
//   ...    lengths       a PFOR block of the R run lengths, each less one
//
// Runs are maximal, so neighbouring run values differ, and the lengths add up to n
// exactly. So a sequence has one set of runs; the encoder's only choices are the PFOR
// blocks' own, and for the values the transform, none or delta, whichever is smaller. The
// fuzz oracle checks values and the encoder's own consistency, as for PFOR. The decoder
// checks every field, every size before it is used, that runs are maximal and that the
// lengths add up. Errors follow codec_error.hpp; "rle run value" has expected = the run's
// index, actual = the index of the run before it with the same value.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

// The fewest payload bytes a block takes: the run count and two PFOR blocks at their
// smallest.
inline constexpr std::uint64_t kRleMinPayloadBytes = 4 + (2 * (kBlockHeaderSize + 8));

struct RleStats {
    std::uint64_t runs = 0;
    // Blocks whose run values took delta.
    std::uint64_t delta_blocks = 0;
    // Blocks encode_rle wrote as a copy of the block before, whose values they repeat.
    // They are counted in `runs` and `delta_blocks` too.
    std::uint64_t repeated = 0;
};

// Appends one block holding `values`, which must hold 1 to kMaxBlockValues values.
[[nodiscard]] std::expected<void, CodecError> encode_rle_block(std::span<const std::uint64_t> values,
                                                               std::vector<std::byte>& out,
                                                               RleStats* stats = nullptr);

// Decodes the block at the front of `block`, appending exactly its count of values to
// `out`, and returns the bytes it took. On error, `out` keeps what it held before.
[[nodiscard]] std::expected<std::size_t, CodecError> decode_rle_block(
    std::span<const std::byte> block, std::vector<std::uint64_t>& out);

// Appends `values` as consecutive blocks of `block_values`, the last one shorter. A block
// whose values repeat the block before's is a copy of its bytes, which is what encoding it
// would give: the encoder is deterministic. A block of 2^20 equal values is one run in a
// few dozen bytes, and cut into blocks of 3 it would otherwise cost the whole encoder
// 349,526 times over, as encode_dict found first.
[[nodiscard]] std::expected<void, CodecError> encode_rle(std::span<const std::uint64_t> values,
                                                         std::size_t block_values,
                                                         std::vector<std::byte>& out,
                                                         RleStats* stats = nullptr);

// Decodes a whole column that must hold exactly `count` values, appending them to `out`.
[[nodiscard]] std::expected<void, CodecError> decode_rle(std::span<const std::byte> column,
                                                         std::uint64_t count,
                                                         std::vector<std::uint64_t>& out);

}  // namespace tick_store::codec
