// Stream VByte on 64-bit values: golden bytes both ways, every length edge and the high
// patch, one corrupt fixture per check in the decoder, truncation at every byte length,
// and the encoded size pinned. The golden block was worked out by hand from the format in
// stream_vbyte.hpp and checked against the same independent model as the PFOR goldens.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/int_transform.hpp"
#include "tick_store/codec/stream_vbyte.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::codec {
namespace {

using itch::test::bytes;
using itch::test::exact_copy;
using Bytes = std::vector<std::byte>;
using Values = std::vector<std::uint64_t>;

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kSignBit = std::uint64_t{1} << 63U;

CodecError error_of(CodecErrorKind kind, std::string_view field, std::uint64_t offset,
                    std::uint64_t expected, std::uint64_t actual) {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

Bytes encoded(const Values& values, bool delta, std::size_t block_values = kDefaultBlockValues,
              SvbStats* stats = nullptr) {
    Bytes out;
    const auto done = encode_svb(values, block_values, delta, out, stats);
    EXPECT_TRUE(done.has_value()) << to_string(done.error());
    return out;
}

std::expected<Values, CodecError> decoded(std::span<const std::byte> column, std::uint64_t count) {
    Values out;
    const Bytes input = exact_copy(column);
    if (const auto done = decode_svb(input, count, out); !done.has_value()) {
        return std::unexpected(done.error());
    }
    return out;
}

std::expected<Values, CodecError> decoded_block(const Bytes& block) {
    Values out;
    const Bytes input = exact_copy(block);
    const auto taken = decode_svb_block(input, out);
    if (!taken.has_value()) {
        return std::unexpected(taken.error());
    }
    EXPECT_EQ(*taken, input.size());
    return out;
}

Bytes block_of(std::uint32_t count, const Bytes& payload) {
    Bytes block;
    const std::size_t at = begin_block(block, count);
    patch_block_header(block, at, static_cast<std::uint32_t>(payload.size()));
    block.insert(block.end(), payload.begin(), payload.end());
    return block;
}

// Deltas 5, 295, 0, 69,700 and 2^40: lengths 1, 2, 1, 3 and 1, with the last one's high
// bits, 256, patched in 9 bits. Zigzag would need the same bytes, so it is not used.
const Values& golden() {
    static const Values value{5, 300, 300, 70'000, (std::uint64_t{1} << 40U) + 70'000};
    return value;
}
const Bytes& golden_payload() {
    static const Bytes value = bytes({
    0x01,                    // delta, no zigzag
    0x84, 0x00,              // codes 0, 1, 0, 2, then 0
    0x05,                    // 5
    0x27, 0x01,              // 295
    0x00,                    // 0
    0x44, 0x10, 0x01,        // 69,700
    0x00,                    // the low half of 2^40
    0x01, 0x00, 0x00, 0x00,  // one high patch
    0x09,                    // of 9 bits
    0x03,                    // its gap in 3 bits
    0x04,                    // at index 4
    0x00, 0x01,              // 256
});
    return value;
}

TEST(StreamVByte, GoldenBytesBothWays) {
    EXPECT_EQ(encoded(golden(), true), block_of(5, golden_payload()));
    EXPECT_EQ(decoded_block(block_of(5, golden_payload())), golden());
}

TEST(StreamVByte, EmptyColumnHasNoBlocks) {
    EXPECT_TRUE(encoded({}, true).empty());
    EXPECT_EQ(decoded({}, 0), Values{});
}

// What a block of one value takes, from the format: its fields, one control byte, the
// low bytes, the high count, and if the high half is not zero a high width, a gap width
// of 0, no gap bytes since the one position is 0, and the high bits.
std::size_t one_value_size(std::uint64_t value) {
    const auto low = static_cast<std::uint32_t>(value);
    const std::size_t length =
        std::max<std::size_t>(1, (static_cast<std::size_t>(std::bit_width(low)) + 7) / 8);
    const std::uint64_t high = value >> 32U;
    const std::size_t patch =
        high == 0 ? 0 : 2 + ((static_cast<std::size_t>(std::bit_width(high)) + 7) / 8);
    return kBlockHeaderSize + 1 + 1 + length + 4 + patch;
}

// Every length edge of the low half, and the high patch from its first bit to its last,
// each alone: the block is the smaller of the value as it is and zigzagged.
TEST(StreamVByte, EveryLengthEdgeTakesItsBytesAndRoundTrips) {
    Values edges{0, 1};
    for (unsigned bits = 8; bits <= 64; bits += 8) {
        const std::uint64_t edge = bits == 64 ? kMax : (std::uint64_t{1} << bits) - 1U;
        edges.push_back(edge - 1U);
        edges.push_back(edge);
        if (bits < 64) {
            edges.push_back(edge + 1U);
        }
    }
    edges.push_back(kSignBit);
    edges.push_back(kSignBit - 1U);
    for (const std::uint64_t value : edges) {
        SCOPED_TRACE(value);
        SvbStats stats;
        const Bytes block = encoded({value}, false, kDefaultBlockValues, &stats);
        const bool zigzag = one_value_size(zigzag_encode(value)) < one_value_size(value);
        EXPECT_EQ(stats.zigzag_blocks, zigzag ? 1U : 0U);
        EXPECT_EQ(block.size(), std::min(one_value_size(value), one_value_size(zigzag_encode(value))));
        EXPECT_EQ(decoded_block(block), Values{value});
    }
    // All of them in one block, delta coded and not.
    for (const bool delta : {false, true}) {
        EXPECT_EQ(decoded(encoded(edges, delta), edges.size()), edges);
    }
}

TEST(StreamVByte, SeededWalksRoundTripAtEveryBlockSize) {
    test::SplitMix64 random{48};
    Values values;
    std::uint64_t t = 34'200'000'000'000ULL;
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t pick = random() % 100;
        if (pick < 2) {
            // a repeat
        } else if (pick < 4) {
            t += random() % (std::uint64_t{1} << 40U);
        } else {
            t += random() % 100'000;
        }
        values.push_back(t);
    }
    values.insert(values.end(), {0, kMax, 0, kMax, kSignBit, kSignBit - 1, 1, kMax - 1});
    for (const bool delta : {false, true}) {
        for (const std::size_t block_values :
             {std::size_t{1}, std::size_t{3}, std::size_t{4}, std::size_t{5}, std::size_t{1000},
              std::size_t{kDefaultBlockValues}, std::size_t{kMaxBlockValues}}) {
            SCOPED_TRACE(block_values);
            const Bytes column = encoded(values, delta, block_values);
            EXPECT_EQ(decoded(column, values.size()), values);
        }
    }
}

// A silent change in the format changes these sizes (T4).
TEST(StreamVByte, EncodedSizeIsPinned) {
    EXPECT_EQ(block_of(5, golden_payload()).size(), 28U);

    // 10,000 timestamps 25 us apart: each block's first delta is its first value, 4 low
    // bytes and a 13-bit high patch, and every other delta is 25,000 in 2 bytes. A block
    // of 4,096 is a header, flags, 1,024 control bytes, 4 + 4,095 * 2 data bytes, the
    // high count, a high width, a gap width of 0 with no gap bytes, since the one position
    // is 0, and 13 high bits: 8 + 1 + 1,024 + 8,194 + 4 + 1 + 1 + 2 = 9,235. The last, of
    // 1,808, is 8 + 1 + 452 + 3,618 + 4 + 1 + 1 + 2 = 4,087: 22,557 in all.
    Values regular;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        regular.push_back(34'200'000'000'000ULL + (i * 25'000));
    }
    SvbStats stats;
    EXPECT_EQ(encoded(regular, true, kDefaultBlockValues, &stats).size(), 22'557U);
    EXPECT_EQ(stats.lengths, (std::array<std::uint64_t, 4>{0, 9'997, 0, 3}));
    EXPECT_EQ(stats.highs, 3U);
    EXPECT_EQ(stats.zigzag_blocks, 0U);
}

TEST(StreamVByte, EncoderRefusesBadBlockSizes) {
    Bytes out;
    EXPECT_EQ(encode_svb(golden(), 0, true, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(encode_svb_block({}, true, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_TRUE(out.empty());
}

// One fixture per check in the decoder, each asserting the exact error (T1). Offsets are
// bits from the start of the block: flags at 64, control at 72, data at 88, the high
// count at 152, its width at 184, the gap width at 192, the gap at 200 and the high bits
// at 208.
TEST(StreamVByte, EachCorruptionHasItsError) {
    EXPECT_EQ(decoded_block(Bytes{}),
              std::unexpected(error_of(CodecErrorKind::truncated, "block header", 0, 64, 0)));
    EXPECT_EQ(decoded_block(block_of(0, golden_payload())),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1, 0)));
    // Twenty values need at least 26 payload bytes; 20 hold at most 14.
    EXPECT_EQ(decoded_block(block_of(20, golden_payload())),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 14, 20)));
    Bytes cut = block_of(5, golden_payload());
    cut.pop_back();
    EXPECT_EQ(decoded_block(cut),
              std::unexpected(error_of(CodecErrorKind::truncated, "block payload", 64, 160, 152)));

    const auto with = [](std::size_t at, unsigned value) {
        Bytes payload = golden_payload();
        payload[at] = static_cast<std::byte>(value);
        return block_of(5, payload);
    };
    EXPECT_EQ(decoded_block(with(0, 5)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "svb flags", 64, 3, 5)));
    EXPECT_EQ(decoded_block(with(2, 0x04)),
              std::unexpected(
                  error_of(CodecErrorKind::trailing_bits, "svb control padding", 82, 0, 1)));
    // 5 in two bytes.
    EXPECT_EQ(decoded_block(block_of(5, bytes({0x01, 0x85, 0x00, 0x05, 0x00, 0x27, 0x01, 0x00,
                                               0x44, 0x10, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
                                               0x09, 0x03, 0x04, 0x00, 0x01}))),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "svb length", 88, 1, 2)));
    // Five codes of 4 bytes, and 8 data bytes.
    EXPECT_EQ(decoded_block(block_of(5, bytes({0x01, 0xFF, 0x03, 0, 0, 0, 0, 0, 0, 0, 0}))),
              std::unexpected(error_of(CodecErrorKind::truncated, "svb data", 88, 160, 64)));
    EXPECT_EQ(decoded_block(with(11, 6)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "svb high count", 152, 5, 6)));
    EXPECT_EQ(decoded_block(with(15, 0)),
              std::unexpected(error_of(CodecErrorKind::bad_width, "svb high width", 184, 32, 0)));
    EXPECT_EQ(decoded_block(with(15, 33)),
              std::unexpected(error_of(CodecErrorKind::bad_width, "svb high width", 184, 32, 33)));
    // 256 in 10 bits, one more than it needs.
    Bytes wide(golden_payload().begin(), golden_payload().begin() + 15);
    wide.insert(wide.end(), {std::byte{10}, std::byte{0x03}, std::byte{0x04}, std::byte{0x00},
                             std::byte{0x01}});
    EXPECT_EQ(decoded_block(block_of(5, wide)),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "svb high width", 184, 9, 10)));
    EXPECT_EQ(decoded_block(with(16, 4)),
              std::unexpected(error_of(CodecErrorKind::bad_width, "svb high gap width", 192, 3, 4)));
    EXPECT_EQ(decoded_block(with(17, 5)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "svb high position", 200, 4, 5)));
    Bytes zero = golden_payload();
    zero[18] = std::byte{0};
    zero[19] = std::byte{0};
    EXPECT_EQ(decoded_block(block_of(5, zero)),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "svb high bits", 208, 1, 0)));
    Bytes spare = golden_payload();
    spare.push_back(std::byte{0});
    EXPECT_EQ(decoded_block(block_of(5, spare)),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "svb block", 224, 0, 8)));
}

TEST(StreamVByte, ColumnCountMustMatchExactly) {
    const Values values{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const Bytes column = encoded(values, true, 4);  // blocks of 4, 4 and 2
    Bytes last;
    ASSERT_TRUE(encode_svb_block(std::span{values}.subspan(8), true, last).has_value());
    EXPECT_EQ(decoded(column, values.size()), values);
    EXPECT_EQ(decoded(column, 9),
              std::unexpected(error_of(CodecErrorKind::bad_count, "column count",
                                       8 * (column.size() - last.size()), 9, 10)));
    EXPECT_EQ(decoded(column, 11),
              std::unexpected(error_of(CodecErrorKind::truncated, "column values",
                                       8 * column.size(), 11, 10)));
    EXPECT_EQ(decoded(column, 8),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "column",
                                       8 * (column.size() - last.size()), 0, 8 * last.size())));
}

TEST(StreamVByte, TruncationAtEveryByteLengthIsAnError) {
    Values values{34'200'000'000'000ULL};
    for (std::uint64_t i = 1; i < 300; ++i) {
        values.push_back(values.back() + ((i * 7'919U) % 300'000U));
    }
    values[100] = 0;
    for (const bool delta : {false, true}) {
        const Bytes column = encoded(values, delta, 128);
        for (std::size_t length = 0; length < column.size(); ++length) {
            SCOPED_TRACE(length);
            EXPECT_FALSE(decoded(std::span{column}.first(length), values.size()).has_value());
        }
        const Bytes block = encoded(values, delta, values.size());
        for (std::size_t length = 0; length < block.size(); ++length) {
            SCOPED_TRACE(length);
            Values out{42};
            const Bytes cut = exact_copy(std::span{block}.first(length));
            EXPECT_FALSE(decode_svb_block(cut, out).has_value());
            EXPECT_EQ(out, Values{42});
        }
    }
}

}  // namespace
}  // namespace tick_store::codec
