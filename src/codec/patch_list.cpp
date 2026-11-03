#include "tick_store/codec/patch_list.hpp"

#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/lane_pack.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

std::size_t patch_list_bytes(std::size_t count, std::size_t limit, unsigned value_bits) noexcept {
    return linear_bytes(count, patch_position_bits(limit)) + linear_bytes(count, value_bits);
}

void append_patches(const Patches& patches, std::size_t limit, unsigned value_bits,
                    std::vector<std::byte>& out) {
    std::vector<std::uint64_t> positions(patches.positions.begin(), patches.positions.end());
    pack_linear(positions, patch_position_bits(limit), out);
    pack_linear(patches.values, value_bits, out);
}

std::expected<void, CodecError> read_patches(ByteCursor& cursor, std::size_t count,
                                             std::size_t limit, unsigned value_bits,
                                             const PatchFields& fields, Patches& out) {
    out.positions.clear();
    out.values.clear();
    if (count == 0) {
        return {};
    }
    if (value_bits == 0 || value_bits > kWordBits) {
        return std::unexpected(codec_error(CodecErrorKind::bad_width, fields.values,
                                           cursor.bit_offset(), kWordBits, value_bits));
    }
    const unsigned position_bits = patch_position_bits(limit);
    // Both parts are taken before anything is sized by `count`, so a count the bytes
    // cannot hold is refused as truncated first.
    const std::uint64_t positions_at = cursor.bit_offset();
    const auto position_bytes = cursor.take(linear_bytes(count, position_bits), fields.positions);
    if (!position_bytes.has_value()) {
        return std::unexpected(position_bytes.error());
    }
    const std::uint64_t values_at = cursor.bit_offset();
    const auto value_bytes = cursor.take(linear_bytes(count, value_bits), fields.values);
    if (!value_bytes.has_value()) {
        return std::unexpected(value_bytes.error());
    }

    std::vector<std::uint64_t> positions(count);
    if (!unpack_linear(*position_bytes, position_bits, positions)) {
        return std::unexpected(codec_error(CodecErrorKind::trailing_bits, fields.positions,
                                           values_at - 8U, 0, 1));
    }
    out.values.resize(count);
    if (!unpack_linear(*value_bytes, value_bits, out.values)) {
        return std::unexpected(codec_error(CodecErrorKind::trailing_bits, fields.values,
                                           cursor.bit_offset() - 8U, 0, 1));
    }
    out.positions.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t at = positions_at + (i * position_bits);
        // The smallest position allowed here: one past the last.
        const std::uint64_t least = i == 0 ? 0 : std::uint64_t{out.positions.back()} + 1U;
        if (positions[i] < least) {
            return std::unexpected(
                codec_error(CodecErrorKind::bad_field, fields.positions, at, least, positions[i]));
        }
        if (positions[i] >= limit) {
            return std::unexpected(codec_error(CodecErrorKind::bad_field, fields.positions, at,
                                               limit - 1U, positions[i]));
        }
        if (out.values[i] == 0) {
            return std::unexpected(codec_error(CodecErrorKind::non_canonical, fields.values,
                                               values_at + (i * value_bits), 1, 0));
        }
        // Below `limit`, which is at most 2^20, so the cast keeps every bit.
        out.positions.push_back(static_cast<std::uint32_t>(positions[i]));
    }
    return {};
}

}  // namespace tick_store::codec
