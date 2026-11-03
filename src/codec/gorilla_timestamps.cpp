#include "tick_store/codec/gorilla_timestamps.hpp"

#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

namespace {

constexpr std::size_t kBucketCount = kDodBucketBits.size();
static_assert(kDodBucketBits.back() == kWordBits, "the last bucket must hold any 64-bit value");

// True if `value`, read as two's complement, fits a signed field of `width` bits,
// 1 to 63. Adding 2^(width-1) maps the field's range onto [0, 2^width), and the add
// wraps by design (D5).
[[nodiscard]] constexpr bool fits_signed(std::uint64_t value, unsigned width) noexcept {
    const std::uint64_t half = std::uint64_t{1} << (width - 1U);
    return value + half < (half << 1U);
}

// The bucket a nonzero D is stored in: the narrowest that holds it.
[[nodiscard]] constexpr std::size_t bucket_of(std::uint64_t dod) noexcept {
    for (std::size_t bucket = 0; bucket + 1 < kBucketCount; ++bucket) {
        if (fits_signed(dod, kDodBucketBits[bucket])) {
            return bucket;
        }
    }
    return kBucketCount - 1;
}

// `raw` as a signed field of `width` bits, 1 to 64, widened to 64 bits.
[[nodiscard]] constexpr std::uint64_t sign_extend(std::uint64_t raw, unsigned width) noexcept {
    if (width == kWordBits) {
        return raw;
    }
    const std::uint64_t sign = std::uint64_t{1} << (width - 1U);
    return (raw ^ sign) - sign;
}

void write_dod(BitWriter& writer, std::uint64_t dod, DodStats* stats) {
    if (dod == 0) {
        writer.write_bit(false);
        if (stats != nullptr) {
            ++stats->buckets[0];
        }
        return;
    }
    const std::size_t bucket = bucket_of(dod);
    // Bucket b is b + 1 ones, then a zero for all but the last: 10, 110, 1110, 1111.
    const unsigned ones = static_cast<unsigned>(bucket) + 1U;
    const bool last = bucket + 1 == kBucketCount;
    const unsigned control_bits = last ? ones : ones + 1U;
    const std::uint64_t control = last ? low_bits(ones) : low_bits(ones) << 1U;
    writer.write(control, control_bits);
    writer.write(dod, kDodBucketBits[bucket]);
    if (stats != nullptr) {
        ++stats->buckets[bucket + 1];
    }
}

}  // namespace

std::expected<void, CodecError> encode_timestamp_block(std::span<const std::uint64_t> values,
                                                       std::vector<std::byte>& out,
                                                       DodStats* stats) {
    if (values.empty() || values.size() > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "block values", 0,
                                           kMaxBlockValues, values.size()));
    }
    const std::size_t start = out.size();
    const std::size_t header = begin_block(out, static_cast<std::uint32_t>(values.size()));
    BitWriter writer{out};
    writer.write(values.front(), kWordBits);
    std::uint64_t previous = values.front();
    std::uint64_t previous_delta = 0;
    for (const std::uint64_t value : values.subspan(1)) {
        // Both differences wrap modulo 2^64 by design (D5).
        const std::uint64_t delta = value - previous;
        write_dod(writer, delta - previous_delta, stats);
        previous = value;
        previous_delta = delta;
    }
    const auto payload = writer.finish();
    if (!payload.has_value()) {
        out.resize(start);
        return std::unexpected(payload.error());
    }
    // At most 64 + (2^20 - 1) * 68 bits, under 9 MB, so the size fits 32 bits.
    patch_block_header(out, header, static_cast<std::uint32_t>(*payload));
    return {};
}

std::expected<std::size_t, CodecError> decode_timestamp_block(std::span<const std::byte> block,
                                                              std::vector<std::uint64_t>& out) {
    const auto header = read_block_header(block, kDodFirstValueBits, kDodMinBitsPerValue);
    if (!header.has_value()) {
        return std::unexpected(header.error());
    }
    const std::size_t start = out.size();
    const auto fail = [&](const CodecError& error) {
        out.resize(start);
        return std::unexpected(error);
    };
    // The count is at most kMaxBlockValues and within what the payload can hold.
    out.reserve(start + header->count);
    BitReader reader{block.subspan(kBlockHeaderSize, header->payload_bytes),
                     kBlockHeaderSize * 8U};

    const auto first = reader.read(kWordBits, "first timestamp");
    if (!first.has_value()) {
        return fail(first.error());
    }
    out.push_back(*first);
    std::uint64_t value = *first;
    std::uint64_t delta = 0;
    for (std::uint32_t i = 1; i < header->count; ++i) {
        const std::uint64_t at = (kBlockHeaderSize * 8U) + reader.position();
        // The control code: up to four ones, ended early by a zero.
        std::size_t ones = 0;
        while (ones < kBucketCount) {
            const auto bit = reader.read_bit("delta-of-delta control");
            if (!bit.has_value()) {
                return fail(bit.error());
            }
            if (!*bit) {
                break;
            }
            ++ones;
        }
        std::uint64_t dod = 0;
        if (ones > 0) {
            const std::size_t bucket = ones - 1;
            const unsigned width = kDodBucketBits[bucket];
            const auto raw = reader.read(width, "delta-of-delta");
            if (!raw.has_value()) {
                return fail(raw.error());
            }
            dod = sign_extend(*raw, width);
            // A zero belongs in '0', and anything else in its narrowest bucket.
            const unsigned canonical = dod == 0 ? 0 : kDodBucketBits[bucket_of(dod)];
            if (canonical != width) {
                return fail(codec_error(CodecErrorKind::non_canonical, "delta-of-delta bucket",
                                        at, canonical, width));
            }
        }
        // Both sums wrap modulo 2^64 by design, the inverse of the encoder's (D5).
        delta += dod;
        value += delta;
        out.push_back(value);
    }
    if (const auto end = reader.finish("block padding"); !end.has_value()) {
        return fail(end.error());
    }
    return kBlockHeaderSize + header->payload_bytes;
}

std::expected<void, CodecError> encode_timestamps(std::span<const std::uint64_t> values,
                                                  std::size_t block_values,
                                                  std::vector<std::byte>& out, DodStats* stats) {
    if (const auto checked = check_block_values(block_values); !checked.has_value()) {
        return checked;
    }
    for (std::size_t at = 0; at < values.size(); at += block_values) {
        const std::size_t size = values.size() - at < block_values ? values.size() - at
                                                                   : block_values;
        if (const auto block = encode_timestamp_block(values.subspan(at, size), out, stats);
            !block.has_value()) {
            return block;
        }
    }
    return {};
}

std::expected<void, CodecError> decode_timestamps(std::span<const std::byte> column,
                                                  std::uint64_t count,
                                                  std::vector<std::uint64_t>& out) {
    return decode_column(column, count, out, &decode_timestamp_block);
}

}  // namespace tick_store::codec
