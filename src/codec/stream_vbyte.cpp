#include "tick_store/codec/stream_vbyte.hpp"

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/int_transform.hpp"
#include "tick_store/codec/patch_list.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

namespace {

constexpr std::uint8_t kFlagDelta = 1;
constexpr std::uint8_t kFlagZigzag = 2;
constexpr std::uint8_t kKnownFlags = kFlagDelta | kFlagZigzag;
constexpr unsigned kHighCountBytes = 4;
constexpr unsigned kLowBits = 32;
constexpr std::uint64_t kLowMask = 0xFFFF'FFFFU;
constexpr std::size_t kCodesPerByte = 4;

constexpr PatchFields kHighFields{.positions = "svb high position", .values = "svb high bits"};

// Bytes the low 32 bits of `value` take: 1 to 4.
[[nodiscard]] constexpr unsigned low_length(std::uint64_t value) noexcept {
    const auto low = static_cast<std::uint32_t>(value & kLowMask);
    return std::max(1U, (static_cast<unsigned>(std::bit_width(low)) + 7U) / 8U);
}

[[nodiscard]] constexpr std::size_t control_bytes(std::size_t count) noexcept {
    return (count + kCodesPerByte - 1) / kCodesPerByte;
}

// Payload bytes for `values` as given: flags, control, data and the high patches.
[[nodiscard]] std::size_t payload_size(std::span<const std::uint64_t> values) noexcept {
    std::size_t data = 0;
    std::size_t highs = 0;
    std::uint64_t highest = 0;
    for (const std::uint64_t value : values) {
        data += low_length(value);
        const std::uint64_t high = value >> kLowBits;
        highs += high != 0 ? 1U : 0U;
        highest = std::max(highest, high);
    }
    std::size_t size = 1 + control_bytes(values.size()) + data + kHighCountBytes;
    if (highs > 0) {
        size += 1 + patch_list_bytes(highs, values.size(),
                                     static_cast<unsigned>(std::bit_width(highest)));
    }
    return size;
}

}  // namespace

std::expected<void, CodecError> encode_svb_block(std::span<const std::uint64_t> values, bool delta,
                                                 std::vector<std::byte>& out, SvbStats* stats) {
    if (values.empty() || values.size() > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "block values", 0,
                                           kMaxBlockValues, values.size()));
    }
    std::vector<std::uint64_t> plain(values.size());
    if (delta) {
        delta_encode(values, plain);
    } else {
        std::ranges::copy(values, plain.begin());
    }
    std::vector<std::uint64_t> zigzagged(plain.size());
    std::ranges::transform(plain, zigzagged.begin(), &zigzag_encode);
    const bool zigzag = payload_size(zigzagged) < payload_size(plain);
    const std::span<const std::uint64_t> coded = zigzag ? zigzagged : plain;

    const std::size_t header = begin_block(out, static_cast<std::uint32_t>(values.size()));
    const std::uint8_t flags = (delta ? kFlagDelta : 0U) | (zigzag ? kFlagZigzag : 0U);
    out.push_back(static_cast<std::byte>(flags));
    const std::size_t controls = out.size();
    out.resize(controls + control_bytes(coded.size()), std::byte{0});
    Patches highs;
    SvbStats counted;
    for (std::size_t i = 0; i < coded.size(); ++i) {
        const std::uint64_t value = coded[i];
        const unsigned length = low_length(value);
        out[controls + (i / kCodesPerByte)] |=
            static_cast<std::byte>((length - 1U) << (2U * (i % kCodesPerByte)));
        append_le(out, value & kLowMask, length);
        ++counted.lengths[length - 1U];
        if (const std::uint64_t high = value >> kLowBits; high != 0) {
            highs.positions.push_back(static_cast<std::uint32_t>(i));
            highs.values.push_back(high);
        }
    }
    append_le(out, highs.positions.size(), kHighCountBytes);
    if (!highs.positions.empty()) {
        const auto width = static_cast<unsigned>(std::bit_width(std::ranges::max(highs.values)));
        out.push_back(static_cast<std::byte>(width));
        append_patches(highs, coded.size(), width, out);
    }
    // At most 2^20 values of under 9 bytes each, so the payload fits 32 bits.
    patch_block_header(out, header,
                       static_cast<std::uint32_t>(out.size() - header - kBlockHeaderSize));
    if (stats != nullptr) {
        stats->zigzag_blocks += zigzag ? 1U : 0U;
        for (std::size_t i = 0; i < counted.lengths.size(); ++i) {
            stats->lengths[i] += counted.lengths[i];
        }
        stats->highs += highs.positions.size();
    }
    return {};
}

std::expected<std::size_t, CodecError> decode_svb_block(std::span<const std::byte> block,
                                                        std::vector<std::uint64_t>& out) {
    const auto header = read_block_header(block, kSvbFirstValueBits, kSvbMinBitsPerValue);
    if (!header.has_value()) {
        return std::unexpected(header.error());
    }
    const std::size_t count = header->count;
    constexpr std::uint64_t kPayloadStart = kBlockHeaderSize * 8U;
    ByteCursor cursor{block.subspan(kBlockHeaderSize, header->payload_bytes), kPayloadStart};
    const std::size_t start = out.size();
    const auto fail = [&](const CodecError& error) {
        out.resize(start);
        return std::unexpected(error);
    };

    const std::uint64_t flags_at = cursor.bit_offset();
    const auto flags = cursor.read_le(1, "svb flags");
    if (!flags.has_value()) {
        return fail(flags.error());
    }
    if ((*flags & ~std::uint64_t{kKnownFlags}) != 0) {
        return fail(codec_error(CodecErrorKind::bad_field, "svb flags", flags_at, kKnownFlags,
                                *flags));
    }
    const std::uint64_t controls_at = cursor.bit_offset();
    const auto controls = cursor.take(control_bytes(count), "svb control");
    if (!controls.has_value()) {
        return fail(controls.error());
    }
    const auto code_of = [&controls](std::size_t i) {
        return (std::to_integer<unsigned>((*controls)[i / kCodesPerByte]) >>
                (2U * (i % kCodesPerByte))) &
               3U;
    };
    if (const std::size_t used = count % kCodesPerByte; used != 0) {
        const unsigned spare = std::to_integer<unsigned>(controls->back()) >> (2U * used);
        if (spare != 0) {
            return fail(codec_error(CodecErrorKind::trailing_bits, "svb control padding",
                                    controls_at + ((controls->size() - 1U) * 8U) + (2U * used),
                                    0, spare));
        }
    }
    std::size_t data_bytes = 0;
    for (std::size_t i = 0; i < count; ++i) {
        data_bytes += code_of(i) + 1U;
    }
    const std::uint64_t data_at = cursor.bit_offset();
    const auto data = cursor.take(data_bytes, "svb data");
    if (!data.has_value()) {
        return fail(data.error());
    }

    // The count is at most kMaxBlockValues and the data holds a byte for each (D8).
    reserve_for_block(out, header->count);
    out.resize(start + count);
    const std::span<std::uint64_t> values = std::span{out}.subspan(start);
    std::size_t at = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned length = code_of(i) + 1U;
        std::uint64_t value = 0;
        for (unsigned b = length; b > 0; --b) {
            value = (value << 8U) | std::to_integer<std::uint64_t>((*data)[at + b - 1]);
        }
        if (low_length(value) != length) {
            return fail(codec_error(CodecErrorKind::non_canonical, "svb length",
                                    data_at + (at * 8U), low_length(value), length));
        }
        values[i] = value;
        at += length;
    }

    const std::uint64_t highs_at = cursor.bit_offset();
    const auto listed = cursor.read_le(kHighCountBytes, "svb high count");
    if (!listed.has_value()) {
        return fail(listed.error());
    }
    if (*listed > count) {
        return fail(codec_error(CodecErrorKind::bad_count, "svb high count", highs_at, count,
                                *listed));
    }
    if (*listed > 0) {
        const std::uint64_t width_at = cursor.bit_offset();
        const auto width = cursor.read_le(1, "svb high width");
        if (!width.has_value()) {
            return fail(width.error());
        }
        if (*width == 0 || *width > kLowBits) {
            return fail(codec_error(CodecErrorKind::bad_width, "svb high width", width_at,
                                    kLowBits, *width));
        }
        Patches highs;
        if (const auto read = read_patches(cursor, static_cast<std::size_t>(*listed), count,
                                           static_cast<unsigned>(*width), kHighFields, highs);
            !read.has_value()) {
            return fail(read.error());
        }
        const std::uint64_t highest = std::ranges::max(highs.values);
        if (static_cast<unsigned>(std::bit_width(highest)) != *width) {
            return fail(codec_error(CodecErrorKind::non_canonical, "svb high width", width_at,
                                    static_cast<unsigned>(std::bit_width(highest)), *width));
        }
        for (std::size_t i = 0; i < highs.positions.size(); ++i) {
            values[highs.positions[i]] |= highs.values[i] << kLowBits;
        }
    }
    if ((*flags & kFlagZigzag) != 0) {
        for (std::uint64_t& value : values) {
            value = zigzag_decode(value);
        }
    }
    if ((*flags & kFlagDelta) != 0) {
        delta_decode(values);
    }
    if (cursor.remaining() != 0) {
        return fail(codec_error(CodecErrorKind::trailing_bits, "svb block", cursor.bit_offset(), 0,
                                std::uint64_t{cursor.remaining()} * 8U));
    }
    return kBlockHeaderSize + header->payload_bytes;
}

std::expected<void, CodecError> encode_svb(std::span<const std::uint64_t> values,
                                           std::size_t block_values, bool delta,
                                           std::vector<std::byte>& out, SvbStats* stats) {
    if (const auto checked = check_block_values(block_values); !checked.has_value()) {
        return checked;
    }
    const std::size_t start = out.size();
    for (std::size_t at = 0; at < values.size(); at += block_values) {
        const std::size_t size = std::min(values.size() - at, block_values);
        if (const auto block = encode_svb_block(values.subspan(at, size), delta, out, stats);
            !block.has_value()) {
            out.resize(start);
            return block;
        }
    }
    return {};
}

std::expected<void, CodecError> decode_svb(std::span<const std::byte> column, std::uint64_t count,
                                           std::vector<std::uint64_t>& out) {
    return decode_column(column, count, out, &decode_svb_block);
}

}  // namespace tick_store::codec
