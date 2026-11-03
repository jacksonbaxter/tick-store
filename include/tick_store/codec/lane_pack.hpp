#pragma once

// Bit-packing for the packed codecs, in two layouts.
//
// Interleaved, for a full vector of kVectorValues = 1,024 values. This is the layout a
// NEON or AVX2 kernel decodes, so the format stays the same when one is added:
//
//   16 lanes of 64-bit words. Value i belongs to lane i % 16, row i / 16, so a row is 16
//   consecutive values. Lane l holds its 64 rows at `width` bits each, least significant
//   bits first, which is exactly `width` words, so no lane is padded. Word k of lane l
//   is stored at word index k * 16 + l, and each word is 8 bytes, little-endian.
//
//   Unpacking row r needs words k = r * width / 64 (and k + 1 when the value straddles
//   two words) of all 16 lanes: 16 consecutive words, which are 8 NEON registers or 4
//   AVX2 registers, all shifted by the same amount. Every row writes 16 consecutive
//   values, so the output is in order and needs no transpose. A vector at width w is
//   16 * w words, 128 * w bytes, w bits per value.
//
//   What this layout gives up, against a format with no SIMD in mind: one width for 1,024
//   values, so a stretch of wide values widens all of them (patching, in pfor.hpp, takes
//   back most of that); and nothing smaller than a vector is interleaved, so short
//   blocks and the tail of a block take the linear layout below and a scalar decode.
//   Deltas stay sequential, so no lane needs a base of its own, unlike FastLanes'
//   transposed delta layout; the price is that the inverse delta is a prefix sum across
//   the row, not down each lane.
//
// Linear, for a tail of fewer than 1,024 values and for patch lists: value i at bits
//   [i * width, (i + 1) * width) of a little-endian byte string, least significant bit
//   first, zero-padded to a whole byte.
//
// Widths 0 and 64 are explicit everywhere: width 0 stores nothing and every value is 0,
// and width 64 stores every bit with no shift by 64. A width above 64 is never passed;
// the decoders refuse one before calling in, and these functions do nothing for one.
//
// Over-read contract: each function reads exactly the words or bytes its sizes say, and
// the callers check those sizes first. Nothing needs padding after it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tick_store::codec {

inline constexpr std::size_t kLanes = 16;
inline constexpr std::size_t kLaneRows = 64;
inline constexpr std::size_t kVectorValues = kLanes * kLaneRows;

// Words and bytes a full vector takes at `width`, 0 to 64.
[[nodiscard]] constexpr std::size_t vector_words(unsigned width) noexcept {
    return kLanes * width;
}
[[nodiscard]] constexpr std::size_t vector_bytes(unsigned width) noexcept {
    return vector_words(width) * 8U;
}

// Bytes `count` values take in the linear layout at `width`. Callers pass counts of at
// most 2^20, so the product cannot wrap.
[[nodiscard]] constexpr std::size_t linear_bytes(std::size_t count, unsigned width) noexcept {
    return ((count * width) + 7U) / 8U;
}

// Packs the low `width` bits of each value into `words`, which must hold exactly
// vector_words(width) words. Bits above `width` are ignored.
void pack_vector(std::span<const std::uint64_t, kVectorValues> values, unsigned width,
                 std::span<std::uint64_t> words) noexcept;

// The inverse: `words` holds exactly vector_words(width) words.
void unpack_vector(std::span<const std::uint64_t> words, unsigned width,
                   std::span<std::uint64_t, kVectorValues> values) noexcept;

// Words to and from their little-endian bytes. `bytes` holds exactly 8 per word.
void append_words_le(std::span<const std::uint64_t> words, std::vector<std::byte>& out);
void load_words_le(std::span<const std::byte> bytes, std::span<std::uint64_t> words) noexcept;

// Appends linear_bytes(values.size(), width) bytes.
void pack_linear(std::span<const std::uint64_t> values, unsigned width,
                 std::vector<std::byte>& out);

// Fills `values` from `bytes`, which must hold exactly linear_bytes(values.size(), width)
// bytes. Returns false if the padding bits after the last value are not all zero, if the
// size is wrong, or if the width is above 64.
[[nodiscard]] bool unpack_linear(std::span<const std::byte> bytes, unsigned width,
                                 std::span<std::uint64_t> values) noexcept;

}  // namespace tick_store::codec
