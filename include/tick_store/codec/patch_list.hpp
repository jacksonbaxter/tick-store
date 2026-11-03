#pragma once

// A patch list: the positions of the values that need a correction, and the corrections.
// The packed codecs use one for their exceptions (the bits above a vector's width), and
// for remainders (the part of a value its block's scale does not divide). The count and
// the value width are the caller's fields; the list itself is
//
//   u8         gap width g, 0 to bit_width(limit - 1), the fewest bits that hold the
//              largest gap
//   gaps       `count` values in the linear layout of lane_pack.hpp, each g bits: the
//              first position, then each position's distance from the one before less 1
//   values     `count` values in the linear layout, each `value_bits` bits
//
// each part zero-padded to a whole byte. A list of no entries is no bytes, not even the
// gap width. Positions are strictly increasing and below
// `limit`, and no value is 0, since a zero correction is no correction. So a list has one
// encoding, and its count is bounded by `limit` before anything is sized by it (D8).
//
// Gaps, not positions: exceptions are spread through a vector, so the largest gap is far
// below the vector's length. On the open of 12302019, about 11% of timestamp deltas are
// exceptions, and a gap of 6 bits or so in place of a 10-bit position saves around 0.4
// bits a value.

#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::codec {

struct Patches {
    std::vector<std::uint32_t> positions;
    std::vector<std::uint64_t> values;
};

// What a list's errors call its parts. All point at static storage.
struct PatchFields {
    std::string_view gap_width;
    std::string_view positions;
    std::string_view values;
};

// The widest a gap can be in a list that covers `limit` values: 0 for one value.
[[nodiscard]] constexpr unsigned patch_position_bits(std::size_t limit) noexcept {
    return limit <= 1 ? 0U : static_cast<unsigned>(std::bit_width(limit - 1));
}

// The gap width a list of these positions takes: the bits of its largest gap.
[[nodiscard]] unsigned patch_gap_bits(std::span<const std::uint32_t> positions) noexcept;

// Bytes a list of `count` entries takes, with gaps of `gap_bits` and values of
// `value_bits`: 0 for no entries. Counts are at most 2^20, so nothing here can wrap.
[[nodiscard]] std::size_t patch_list_bytes(std::size_t count, unsigned gap_bits,
                                           unsigned value_bits) noexcept;

// Appends a list. The positions must be strictly increasing, and the values non-zero
// and within `value_bits`: the encoders build them that way.
void append_patches(const Patches& patches, unsigned value_bits, std::vector<std::byte>& out);

// Reads a list of `count` entries, `count` already checked against `limit`, into `out`,
// checking every field. `value_bits` is 1 to 64.
[[nodiscard]] std::expected<void, CodecError> read_patches(ByteCursor& cursor, std::size_t count,
                                                           std::size_t limit, unsigned value_bits,
                                                           const PatchFields& fields,
                                                           Patches& out);

}  // namespace tick_store::codec
