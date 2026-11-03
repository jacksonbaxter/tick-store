#pragma once

// A patch list: the positions of the values that need a correction, and the corrections.
// The packed codecs use one for their exceptions (the bits above a vector's width), and
// for remainders (the part of a value its block's scale does not divide). The count and
// the value width are the caller's fields; the list itself is
//
//   positions  `count` values in the linear layout of lane_pack.hpp, each
//              bit_width(limit - 1) bits, so 0 bits when the list covers one value
//   values     `count` values in the linear layout, each `value_bits` bits
//
// each part zero-padded to a whole byte. Positions are strictly increasing and below
// `limit`, and no value is 0, since a zero correction is no correction. So a list has one
// encoding, and its count is bounded by `limit` before anything is sized by it (D8).

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

// What a list's errors call its two parts. Both point at static storage.
struct PatchFields {
    std::string_view positions;
    std::string_view values;
};

// Bits one position takes in a list that covers `limit` values: 0 for one value.
[[nodiscard]] constexpr unsigned patch_position_bits(std::size_t limit) noexcept {
    return limit <= 1 ? 0U : static_cast<unsigned>(std::bit_width(limit - 1));
}

// Bytes a list of `count` entries takes. Counts and limits are at most 2^20, so nothing
// here can wrap.
[[nodiscard]] std::size_t patch_list_bytes(std::size_t count, std::size_t limit,
                                           unsigned value_bits) noexcept;

// Appends a list. The positions must be strictly increasing and below `limit`, and the
// values non-zero and within `value_bits`: the encoders build them that way.
void append_patches(const Patches& patches, std::size_t limit, unsigned value_bits,
                    std::vector<std::byte>& out);

// Reads a list of `count` entries, `count` already checked against `limit`, into `out`,
// checking every position and value. `value_bits` is 1 to 64.
[[nodiscard]] std::expected<void, CodecError> read_patches(ByteCursor& cursor, std::size_t count,
                                                           std::size_t limit, unsigned value_bits,
                                                           const PatchFields& fields,
                                                           Patches& out);

}  // namespace tick_store::codec
