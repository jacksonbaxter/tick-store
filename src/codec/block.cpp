#include "tick_store/codec/block.hpp"

#include "tick_store/codec/codec_error.hpp"
#include "tick_store/store/little_endian.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

namespace {

constexpr std::size_t kCountOffset = 0;
constexpr std::size_t kPayloadOffset = 4;
static_assert(kPayloadOffset + 4 == kBlockHeaderSize);

}  // namespace

std::expected<BlockHeader, CodecError> read_block_header(std::span<const std::byte> block,
                                                         std::uint64_t first_bits,
                                                         std::uint64_t min_bits_per_value) {
    const auto count = store::read_le<4>(block, kCountOffset);
    const auto payload = store::read_le<4>(block, kPayloadOffset);
    if (!count.has_value() || !payload.has_value()) {
        return std::unexpected(codec_error(CodecErrorKind::truncated, "block header", 0,
                                           kBlockHeaderSize * 8U, block.size() * 8U));
    }
    if (*count == 0) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_count, "block count", kCountOffset * 8U, 1, 0));
    }
    if (*count > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_count, "block count",
                                           kCountOffset * 8U, kMaxBlockValues, *count));
    }
    // Both factors are below 2^32 and 2^7, so neither the product nor the sum can wrap.
    const std::uint64_t payload_bits = *payload * 8U;
    const std::uint64_t least_bits = first_bits + ((*count - 1U) * min_bits_per_value);
    if (payload_bits < least_bits) {
        // The largest count this payload could hold, as the expected value.
        const std::uint64_t most = payload_bits < first_bits
                                       ? 0
                                       : 1U + ((payload_bits - first_bits) / min_bits_per_value);
        return std::unexpected(
            codec_error(CodecErrorKind::bad_count, "block count", kCountOffset * 8U, most, *count));
    }
    // `block.size() >= kBlockHeaderSize` here, so the subtraction cannot wrap.
    if (*payload > block.size() - kBlockHeaderSize) {
        return std::unexpected(codec_error(CodecErrorKind::truncated, "block payload",
                                           kBlockHeaderSize * 8U, payload_bits,
                                           (block.size() - kBlockHeaderSize) * 8U));
    }
    return BlockHeader{.count = static_cast<std::uint32_t>(*count),
                       .payload_bytes = static_cast<std::uint32_t>(*payload)};
}

std::size_t begin_block(std::vector<std::byte>& out, std::uint32_t count) {
    const std::size_t at = out.size();
    out.resize(at + kBlockHeaderSize);
    const std::span<std::byte> header = std::span{out}.subspan(at);
    // The span is exactly the header's size, so neither write can fail.
    [[maybe_unused]] const bool wrote = store::write_le<4>(header, kCountOffset, count) &&
                                        store::write_le<4>(header, kPayloadOffset, 0);
    return at;
}

void patch_block_header(std::vector<std::byte>& out, std::size_t header_offset,
                        std::uint32_t payload_bytes) {
    [[maybe_unused]] const bool wrote =
        store::write_le<4>(out, header_offset + kPayloadOffset, payload_bytes);
}

std::expected<void, CodecError> check_block_values(std::size_t block_values) {
    if (block_values == 0 || block_values > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "block values", 0,
                                           kMaxBlockValues, block_values));
    }
    return {};
}

std::expected<void, CodecError> decode_column(std::span<const std::byte> column,
                                              std::uint64_t count, std::vector<std::uint64_t>& out,
                                              BlockDecoder decode_block) {
    const std::size_t start = out.size();
    const auto fail = [&](CodecError error) {
        out.resize(start);
        return std::unexpected(error);
    };
    std::size_t offset = 0;
    std::uint64_t decoded = 0;
    while (offset < column.size()) {
        if (decoded == count) {
            return fail(codec_error(CodecErrorKind::trailing_bits, "column", offset * 8U, 0,
                                    (column.size() - offset) * 8U));
        }
        const auto taken = decode_block(column.subspan(offset), out);
        if (!taken.has_value()) {
            CodecError error = taken.error();
            error.offset += offset * 8U;
            return fail(error);
        }
        decoded = out.size() - start;
        if (decoded > count) {
            return fail(codec_error(CodecErrorKind::bad_count, "column count", offset * 8U,
                                    count, decoded));
        }
        offset += *taken;
    }
    if (decoded < count) {
        return fail(codec_error(CodecErrorKind::truncated, "column values", column.size() * 8U,
                                count, decoded));
    }
    return {};
}

}  // namespace tick_store::codec
