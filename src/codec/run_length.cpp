#include "tick_store/codec/run_length.hpp"

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/pfor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

namespace {

constexpr unsigned kRunCountBytes = 4;
constexpr std::uint64_t kPayloadStart = kBlockHeaderSize * 8U;

constexpr PforOptions kPlainValues{.transform = IntTransform::none, .adaptive_scale = true,
                                   .patch = true};
constexpr PforOptions kDeltaValues{.transform = IntTransform::delta, .adaptive_scale = true,
                                   .patch = true};
constexpr PforOptions kLengths{.transform = IntTransform::none, .adaptive_scale = false,
                               .patch = true};

[[nodiscard]] CodecError rebased(CodecError error, std::uint64_t base_bits) noexcept {
    error.offset += base_bits;
    return error;
}

}  // namespace

std::expected<void, CodecError> encode_rle_block(std::span<const std::uint64_t> values,
                                                 std::vector<std::byte>& out, RleStats* stats) {
    if (values.empty() || values.size() > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "block values", 0,
                                           kMaxBlockValues, values.size()));
    }
    std::vector<std::uint64_t> run_values;
    std::vector<std::uint64_t> run_lengths;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i == 0 || values[i] != values[i - 1]) {
            run_values.push_back(values[i]);
            run_lengths.push_back(0);
        } else {
            ++run_lengths.back();
        }
    }
    // Each PFOR block holds 1 to kMaxBlockValues values, so none is refused, but the
    // results are passed on all the same.
    std::vector<std::byte> plain;
    if (auto done = encode_pfor_block(run_values, kPlainValues, plain); !done.has_value()) {
        return done;
    }
    std::vector<std::byte> delta;
    if (auto done = encode_pfor_block(run_values, kDeltaValues, delta); !done.has_value()) {
        return done;
    }
    std::vector<std::byte> lengths;
    if (auto done = encode_pfor_block(run_lengths, kLengths, lengths); !done.has_value()) {
        return done;
    }
    const bool use_delta = delta.size() < plain.size();

    const std::size_t header = begin_block(out, static_cast<std::uint32_t>(values.size()));
    append_le(out, run_values.size(), kRunCountBytes);
    const std::vector<std::byte>& chosen = use_delta ? delta : plain;
    out.insert(out.end(), chosen.begin(), chosen.end());
    out.insert(out.end(), lengths.begin(), lengths.end());
    // Two PFOR blocks of at most 2^20 values, each under 21 MB, so this fits 32 bits.
    patch_block_header(out, header,
                       static_cast<std::uint32_t>(out.size() - header - kBlockHeaderSize));
    if (stats != nullptr) {
        stats->runs += run_values.size();
        stats->delta_blocks += use_delta ? 1U : 0U;
    }
    return {};
}

std::expected<std::size_t, CodecError> decode_rle_block(std::span<const std::byte> block,
                                                        std::vector<std::uint64_t>& out) {
    const auto header = read_block_header(block, kRleMinPayloadBytes * 8U, 0);
    if (!header.has_value()) {
        return std::unexpected(header.error());
    }
    const std::size_t count = header->count;
    const std::span<const std::byte> payload = block.subspan(kBlockHeaderSize, header->payload_bytes);
    ByteCursor cursor{payload, kPayloadStart};
    const std::uint64_t runs_at = cursor.bit_offset();
    const auto runs = cursor.read_le(kRunCountBytes, "rle runs");
    if (!runs.has_value()) {
        return std::unexpected(runs.error());
    }
    if (*runs == 0 || *runs > count) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_count, "rle runs", runs_at, count, *runs));
    }
    const auto run_count = static_cast<std::size_t>(*runs);

    std::size_t at = kRunCountBytes;
    std::vector<std::uint64_t> run_values;
    const std::uint64_t values_at = kPayloadStart + (at * 8U);
    {
        const auto taken = decode_pfor_block(payload.subspan(at), run_values);
        if (!taken.has_value()) {
            return std::unexpected(rebased(taken.error(), values_at));
        }
        if (run_values.size() != run_count) {
            return std::unexpected(codec_error(CodecErrorKind::bad_count, "rle value count",
                                               values_at, run_count, run_values.size()));
        }
        at += *taken;
    }
    std::vector<std::uint64_t> run_lengths;
    const std::uint64_t lengths_at = kPayloadStart + (at * 8U);
    {
        const auto taken = decode_pfor_block(payload.subspan(at), run_lengths);
        if (!taken.has_value()) {
            return std::unexpected(rebased(taken.error(), lengths_at));
        }
        if (run_lengths.size() != run_count) {
            return std::unexpected(codec_error(CodecErrorKind::bad_count, "rle length count",
                                               lengths_at, run_count, run_lengths.size()));
        }
        at += *taken;
    }
    if (at != payload.size()) {
        return std::unexpected(codec_error(CodecErrorKind::trailing_bits, "rle block",
                                           kPayloadStart + (at * 8U), 0,
                                           std::uint64_t{payload.size() - at} * 8U));
    }
    for (std::size_t i = 1; i < run_count; ++i) {
        if (run_values[i] == run_values[i - 1]) {
            return std::unexpected(
                codec_error(CodecErrorKind::non_canonical, "rle run value", values_at, i, i - 1));
        }
    }
    // Each length is checked against what is left before it is added, so nothing wraps,
    // and the total never passes the count.
    std::uint64_t total = 0;
    for (const std::uint64_t length : run_lengths) {
        const std::uint64_t left = count - total;
        if (left == 0) {
            return std::unexpected(codec_error(CodecErrorKind::bad_count, "rle run lengths",
                                               lengths_at, count, count + 1U));
        }
        if (length >= left) {
            return std::unexpected(
                codec_error(CodecErrorKind::bad_field, "rle run length", lengths_at, left - 1U, length));
        }
        total += length + 1U;
    }
    if (total != count) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_count, "rle run lengths", lengths_at, count, total));
    }
    reserve_for_block(out, header->count);
    for (std::size_t i = 0; i < run_count; ++i) {
        out.insert(out.end(), run_lengths[i] + 1U, run_values[i]);
    }
    return kBlockHeaderSize + header->payload_bytes;
}

std::expected<void, CodecError> encode_rle(std::span<const std::uint64_t> values,
                                           std::size_t block_values, std::vector<std::byte>& out,
                                           RleStats* stats) {
    if (const auto checked = check_block_values(block_values); !checked.has_value()) {
        return checked;
    }
    const std::size_t start = out.size();
    for (std::size_t at = 0; at < values.size(); at += block_values) {
        const std::size_t size = std::min(values.size() - at, block_values);
        if (auto block = encode_rle_block(values.subspan(at, size), out, stats);
            !block.has_value()) {
            out.resize(start);
            return block;
        }
    }
    return {};
}

std::expected<void, CodecError> decode_rle(std::span<const std::byte> column, std::uint64_t count,
                                           std::vector<std::uint64_t>& out) {
    return decode_column(column, count, out, &decode_rle_block);
}

}  // namespace tick_store::codec
