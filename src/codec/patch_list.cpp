#include "tick_store/codec/patch_list.hpp"

#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/lane_pack.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

namespace {

// The gaps of strictly increasing positions: the first position, then each distance
// less one.
[[nodiscard]] std::vector<std::uint64_t> gaps_of(std::span<const std::uint32_t> positions) {
    std::vector<std::uint64_t> gaps(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        gaps[i] = i == 0 ? positions[0] : positions[i] - positions[i - 1] - 1U;
    }
    return gaps;
}

}  // namespace

unsigned patch_gap_bits(std::span<const std::uint32_t> positions) noexcept {
    std::uint32_t widest = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        widest = std::max(widest, i == 0 ? positions[0] : positions[i] - positions[i - 1] - 1U);
    }
    return static_cast<unsigned>(std::bit_width(widest));
}

std::size_t patch_list_bytes(std::size_t count, unsigned gap_bits, unsigned value_bits) noexcept {
    return count == 0 ? 0 : 1 + linear_bytes(count, gap_bits) + linear_bytes(count, value_bits);
}

void append_patches(const Patches& patches, unsigned value_bits, std::vector<std::byte>& out) {
    // An empty list is no bytes at all, as read_patches reads it.
    if (patches.positions.empty()) {
        return;
    }
    const unsigned gap_bits = patch_gap_bits(patches.positions);
    out.push_back(static_cast<std::byte>(gap_bits));
    pack_linear(gaps_of(patches.positions), gap_bits, out);
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
    const std::uint64_t gap_width_at = cursor.bit_offset();
    const auto gap_width = cursor.read_le(1, fields.gap_width);
    if (!gap_width.has_value()) {
        return std::unexpected(gap_width.error());
    }
    if (*gap_width > patch_position_bits(limit)) {
        return std::unexpected(codec_error(CodecErrorKind::bad_width, fields.gap_width,
                                           gap_width_at, patch_position_bits(limit), *gap_width));
    }
    const auto gap_bits = static_cast<unsigned>(*gap_width);
    // Both parts are taken before anything is sized by `count`, so a count the bytes
    // cannot hold is refused as truncated first. With gaps of 0 bits the positions take
    // no bytes, but then they are 0, 1, 2 and so on, and `count` is at most `limit`.
    const std::uint64_t gaps_at = cursor.bit_offset();
    const auto gap_bytes = cursor.take(linear_bytes(count, gap_bits), fields.positions);
    if (!gap_bytes.has_value()) {
        return std::unexpected(gap_bytes.error());
    }
    const std::uint64_t values_at = cursor.bit_offset();
    const auto value_bytes = cursor.take(linear_bytes(count, value_bits), fields.values);
    if (!value_bytes.has_value()) {
        return std::unexpected(value_bytes.error());
    }

    thread_local std::vector<std::uint64_t> gaps;
    gaps.resize(count);
    if (!unpack_linear(*gap_bytes, gap_bits, gaps)) {
        return std::unexpected(codec_error(CodecErrorKind::trailing_bits, fields.positions,
                                           values_at - 8U, 0, 1));
    }
    out.values.resize(count);
    if (!unpack_linear(*value_bytes, value_bits, out.values)) {
        return std::unexpected(codec_error(CodecErrorKind::trailing_bits, fields.values,
                                           cursor.bit_offset() - 8U, 0, 1));
    }
    const auto needed = static_cast<unsigned>(std::bit_width(std::ranges::max(gaps)));
    if (needed != gap_bits) {
        return std::unexpected(codec_error(CodecErrorKind::non_canonical, fields.gap_width,
                                           gap_width_at, needed, gap_bits));
    }
    out.positions.reserve(count);
    std::uint64_t position = 0;
    for (std::size_t i = 0; i < count; ++i) {
        // A gap is below 2^20 and so is the position before it, so the sum cannot wrap.
        position = i == 0 ? gaps[0] : position + gaps[i] + 1U;
        if (position >= limit) {
            return std::unexpected(codec_error(CodecErrorKind::bad_field, fields.positions,
                                               gaps_at + (i * gap_bits), limit - 1U, position));
        }
        if (out.values[i] == 0) {
            return std::unexpected(codec_error(CodecErrorKind::non_canonical, fields.values,
                                               values_at + (i * value_bits), 1, 0));
        }
        // Below `limit`, which is at most 2^20, so the cast keeps every bit.
        out.positions.push_back(static_cast<std::uint32_t>(position));
    }
    return {};
}

}  // namespace tick_store::codec
