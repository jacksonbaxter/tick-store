#pragma once

// Stream VByte (Lemire, Kurz and Rupp, "Stream VByte: Faster Byte-Oriented Integer
// Compression", 2018), on 64-bit values: the low 32 bits of each value in Stream VByte's
// layout, and the high 32 bits, which ITCH almost never needs, in a patch list.
//
// A block is the frame in block.hpp around this payload, little-endian throughout:
//
//   u8     flags          bit 0: delta, bit 1: zigzag; the other bits zero
//   ...    control        ceil(n / 4) bytes. Value i's length code is bits
//                         2 * (i % 4) and up of byte i / 4: its low 32 bits take code
//                         + 1 bytes, the fewest that hold them (one for 0). Codes past
//                         the last value are zero
//   ...    data           each value's low 32 bits in its code + 1 bytes
//   u32    highs e        at most n
//   if e > 0:
//     u8   high width h   1 to 32, and the highest has bit h - 1 set
//     ...  patch list     (patch_list.hpp) over n values, of the high 32 bits, each at
//                         least 1, in h bits
//
// Decoding: each value is its low bits with its high bits above them, then zigzag
// decoded if the flag says so, then a running sum if the block is delta coded (the value
// before the first taken as 0, as int_transform.hpp has it). Arithmetic wraps modulo
// 2^64, so every sequence round-trips.
//
// The control bytes and the data are the layout SIMD decoders of Stream VByte read with
// one table lookup and one shuffle per four values, so a later kernel needs no format
// change. The encoder chooses zigzag per block, whichever is smaller, so a rising column
// pays no sign bit. That choice is the only one the format leaves open; the decoder
// checks every field, and the fuzz oracle checks values and the encoder's fixed point.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

// The fewest payload bytes a block takes for its first value, and for each value after:
// flags, one control byte, one data byte and the high count; then one data byte each.
inline constexpr std::uint64_t kSvbFirstValueBits = std::uint64_t{7} * 8U;
inline constexpr std::uint64_t kSvbMinBitsPerValue = 8;

struct SvbStats {
    std::uint64_t zigzag_blocks = 0;
    // Values whose low 32 bits took 1, 2, 3 and 4 bytes.
    std::array<std::uint64_t, 4> lengths{};
    std::uint64_t highs = 0;
};

// Appends one block holding `values`, which must hold 1 to kMaxBlockValues values, delta
// coded if `delta`.
[[nodiscard]] std::expected<void, CodecError> encode_svb_block(std::span<const std::uint64_t> values,
                                                               bool delta,
                                                               std::vector<std::byte>& out,
                                                               SvbStats* stats = nullptr);

// Decodes the block at the front of `block`, appending exactly its count of values to
// `out`, and returns the bytes it took. On error, `out` keeps what it held before.
[[nodiscard]] std::expected<std::size_t, CodecError> decode_svb_block(
    std::span<const std::byte> block, std::vector<std::uint64_t>& out);

[[nodiscard]] std::expected<void, CodecError> encode_svb(std::span<const std::uint64_t> values,
                                                         std::size_t block_values, bool delta,
                                                         std::vector<std::byte>& out,
                                                         SvbStats* stats = nullptr);

[[nodiscard]] std::expected<void, CodecError> decode_svb(std::span<const std::byte> column,
                                                         std::uint64_t count,
                                                         std::vector<std::uint64_t>& out);

}  // namespace tick_store::codec
