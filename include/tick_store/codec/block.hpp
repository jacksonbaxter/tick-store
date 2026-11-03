#pragma once

// The frame every codec block shares, so a reader can skip a block without decoding
// it and a store can seek to any block start.
//
//   offset 0  u32 little-endian  value count, 1 to kMaxBlockValues
//   offset 4  u32 little-endian  payload bytes that follow
//   offset 8  payload            the codec's bit stream, zero-padded to a whole byte
//
// A block decodes alone: it carries its own first value, so no state crosses a block
// boundary. A column is its blocks back to back, and the column's value count lives
// with whatever names the column, such as a file footer. Codec ids, CRCs and the file
// header are the file format's job, not this frame's.

#include "tick_store/codec/codec_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

inline constexpr std::size_t kBlockHeaderSize = 8;

// The largest count a block may hold, a cap a decoder enforces before it allocates
// anything (D8). A block of this many 77-bit values is under 10 MB, so the payload
// size always fits its 32-bit field.
inline constexpr std::uint32_t kMaxBlockValues = std::uint32_t{1} << 20U;

// Values per block when the caller does not choose. Gorilla's paper uses two-hour
// blocks, which on ITCH would be tens of millions of records and make every seek a
// long decode. Q4 reads one millisecond of all symbols, and the busiest second of the
// pinned day carries on the order of a few thousand records per millisecond, so 4,096
// values keeps a point query to one or two blocks. The fixed cost per block, the
// 8-byte header and the 64-bit first value, is 128 bits, under 0.04 bits per value at
// this size. The block size sweep in TASKS.md measures the trade properly.
inline constexpr std::uint32_t kDefaultBlockValues = 4096;

struct BlockHeader {
    std::uint32_t count = 0;
    std::uint32_t payload_bytes = 0;

    friend bool operator==(const BlockHeader&, const BlockHeader&) = default;
};

// Reads and checks a block header at the front of `block`: it must be whole, its
// count between 1 and kMaxBlockValues, and its payload present in full. A payload
// longer than the count could fill is caught by the codec's trailing check.
// `min_bits_per_value` is the fewest payload bits any value after the first can take,
// and `first_bits` what the first takes, so a count the payload cannot hold is refused
// before anything is sized by it.
[[nodiscard]] std::expected<BlockHeader, CodecError> read_block_header(
    std::span<const std::byte> block, std::uint64_t first_bits, std::uint64_t min_bits_per_value);

// Appends a header with a zero payload size, to be filled in by patch_block_header.
// Returns its offset in `out`.
[[nodiscard]] std::size_t begin_block(std::vector<std::byte>& out, std::uint32_t count);

// Writes the payload size into the header begun at `header_offset`.
void patch_block_header(std::vector<std::byte>& out, std::size_t header_offset,
                        std::uint32_t payload_bytes);

// Checks an encoder's block size: 1 to kMaxBlockValues.
[[nodiscard]] std::expected<void, CodecError> check_block_values(std::size_t block_values);

// Makes room for `count` more values, growing geometrically so that a column of many
// small blocks appends in linear time. `count` must already be checked against the cap.
void reserve_for_block(std::vector<std::uint64_t>& out, std::uint32_t count);

// Decodes the block at the front of its input, appending its values, and returns the
// bytes it took. Every codec's block decoder has this shape.
using BlockDecoder = std::expected<std::size_t, CodecError> (*)(std::span<const std::byte>,
                                                                std::vector<std::uint64_t>&);

// Walks a column of blocks back to back, which must hold exactly `count` values, and
// appends them to `out`. Errors carry offsets from the column's first byte. On error,
// `out` keeps what it held before. For a column short of `count`, the error is
// truncated with expected = `count` and actual = the values found; for one with more,
// bad_count with the same two values, or trailing_bits for bytes after the last value.
[[nodiscard]] std::expected<void, CodecError> decode_column(std::span<const std::byte> column,
                                                            std::uint64_t count,
                                                            std::vector<std::uint64_t>& out,
                                                            BlockDecoder decode_block);

}  // namespace tick_store::codec
