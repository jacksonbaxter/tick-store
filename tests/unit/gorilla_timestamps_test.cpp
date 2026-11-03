// Gorilla delta-of-delta timestamps: golden bytes both ways, every bucket edge, the
// extremes of 64-bit wrap, one corrupt fixture per check, truncation at every byte
// length, and the encoded size pinned.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
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

// One delta-of-delta in each bucket, worked out bit by bit from the format in
// gorilla_timestamps.hpp:
//   first value 0x0000123456789ABC
//   +5         D = 5        '10'   + 16 bits
//   +5         D = 0        '0'
//   +40005     D = 40000    '110'  + 19 bits
//   +40005     D = 0        '0'
//   +3040005   D = 3000000  '1110' + 23 bits
//   +2^40 more D = 2^40     '1111' + 64 bits
//   -1         D = -1       '10'   + 16 bits of ones
// 219 bits, so 28 bytes with 5 bits of padding.
constexpr std::array<std::uint64_t, 8> kGoldenArray{
    0x123456789ABCULL, 0x123456789AC1ULL, 0x123456789AC6ULL, 0x12345679370BULL,
    0x12345679D350ULL, 0x123456A83655ULL, 0x133456D6995AULL, 0x14345704FC5EULL};


const Values& golden_values() {
    static const Values values(kGoldenArray.begin(), kGoldenArray.end());
    return values;
}

const Bytes& golden_block() {
    static const Bytes block = bytes({
        0x08, 0x00, 0x00, 0x00,                          // count 8
        0x1C, 0x00, 0x00, 0x00,                          // payload 28 bytes
        0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,  // first value
        0x80, 0x01, 0x58, 0x4E, 0x20, 0x39, 0x6E, 0x36,  // 10 0x0005, 0, 110 40000, 0, 1110 ...
        0x07, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,  // ... 3000000, 1111 2^40 ...
        0x00, 0x5F, 0xFF, 0xE0,                          // ... 10 0xFFFF, padding
    });
    return block;
}

Bytes encoded(const Values& values, std::size_t block_values = kDefaultBlockValues,
              DodStats* stats = nullptr) {
    Bytes out;
    const auto done = encode_timestamps(values, block_values, out, stats);
    EXPECT_TRUE(done.has_value()) << to_string(done.error());
    return out;
}

std::expected<Values, CodecError> decoded(std::span<const std::byte> column, std::uint64_t count) {
    Values out;
    const Bytes input = exact_copy(column);
    if (const auto done = decode_timestamps(input, count, out); !done.has_value()) {
        return std::unexpected(done.error());
    }
    return out;
}

// A single block's values, or the error decoding it gives.
std::expected<Values, CodecError> decoded_block(const Bytes& block) {
    Values out;
    const Bytes input = exact_copy(block);
    const auto taken = decode_timestamp_block(input, out);
    if (!taken.has_value()) {
        return std::unexpected(taken.error());
    }
    EXPECT_EQ(*taken, input.size());
    return out;
}

// Timestamps whose delta-of-deltas are `dods`, from a first value of `first`.
Values from_dods(std::uint64_t first, const Values& dods) {
    Values values{first};
    std::uint64_t delta = 0;
    for (const std::uint64_t dod : dods) {
        delta += dod;
        values.push_back(values.back() + delta);
    }
    return values;
}

// A block with a hand-built payload: a count, then bits given as a string of 0 and 1
// after a 64-bit first value of zero, padded with zeros to a whole byte.
Bytes block_with_bits(std::uint32_t count, std::string_view bits) {
    Bytes payload(8, std::byte{0});
    unsigned filled = 0;
    for (const char bit : bits) {
        if (filled % 8 == 0) {
            payload.push_back(std::byte{0});
        }
        if (bit == '1') {
            payload.back() |= static_cast<std::byte>(0x80U >> (filled % 8U));
        }
        ++filled;
    }
    Bytes block;
    const std::size_t at = begin_block(block, count);
    patch_block_header(block, at, static_cast<std::uint32_t>(payload.size()));
    block.insert(block.end(), payload.begin(), payload.end());
    return block;
}

std::string bits_of(std::uint64_t value, unsigned width) {
    std::string out;
    for (unsigned i = width; i > 0; --i) {
        out += ((value >> (i - 1U)) & 1U) != 0 ? '1' : '0';
    }
    return out;
}

TEST(GorillaTimestamps, GoldenBytesBothWays) {
    EXPECT_EQ(encoded(golden_values()), golden_block());
    EXPECT_EQ(decoded_block(golden_block()), golden_values());
}

TEST(GorillaTimestamps, OneValueIsTheFirstValueAlone) {
    const Bytes block = encoded({0xFFFF'FFFF'FFFFULL});
    EXPECT_EQ(block, bytes({0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(decoded_block(block), (Values{0xFFFF'FFFF'FFFFULL}));
}

TEST(GorillaTimestamps, EmptyColumnHasNoBlocks) {
    EXPECT_TRUE(encoded({}).empty());
    EXPECT_EQ(decoded({}, 0), Values{});
}

struct Edge {
    std::int64_t dod;
    std::size_t bucket;  // 0 is the zero bucket, then kDodBucketBits in order
};

// Every bucket edge, approached from a few values before it: the last values each
// bucket holds and the first values it refuses, on both sides of zero, then 0, +-1 and
// the extremes of int64.
std::vector<Edge> bucket_edges() {
    std::vector<Edge> edges{{0, 0}, {1, 1}, {-1, 1}};
    for (std::size_t b = 0; b + 1 < kDodBucketBits.size(); ++b) {
        const auto half = static_cast<std::int64_t>(std::uint64_t{1} << (kDodBucketBits[b] - 1U));
        for (std::int64_t step = 0; step < 4; ++step) {
            edges.push_back({half - 1 - step, b + 1});
            edges.push_back({-half + step, b + 1});
        }
        edges.push_back({half, b + 2});
        edges.push_back({half + 1, b + 2});
        edges.push_back({-half - 1, b + 2});
        edges.push_back({-half - 2, b + 2});
    }
    edges.push_back({std::numeric_limits<std::int64_t>::max(), 4});
    edges.push_back({std::numeric_limits<std::int64_t>::min(), 4});
    edges.push_back({std::numeric_limits<std::int64_t>::max() - 1, 4});
    edges.push_back({std::numeric_limits<std::int64_t>::min() + 1, 4});
    return edges;
}

// Each edge alone: the bucket it lands in, the exact bits it takes, and its round trip.
TEST(GorillaTimestamps, EveryBucketEdgeLandsInItsBucketAndRoundTrips) {
    constexpr std::array<unsigned, 5> kCost{1, 2 + 16, 3 + 19, 4 + 23, 4 + 64};
    for (const Edge& edge : bucket_edges()) {
        SCOPED_TRACE(edge.dod);
        const Values values = from_dods(1'000'000, {static_cast<std::uint64_t>(edge.dod)});
        DodStats stats;
        const Bytes block = encoded(values, kDefaultBlockValues, &stats);
        EXPECT_EQ(stats.buckets[edge.bucket], 1U);
        EXPECT_EQ(block.size(), kBlockHeaderSize + ((64U + kCost[edge.bucket] + 7U) / 8U));
        EXPECT_EQ(decoded_block(block), values);
    }
}

// All the edges in one stream, so each is coded after a neighbour from another bucket.
TEST(GorillaTimestamps, AllEdgesInOneStreamRoundTrip) {
    Values dods;
    for (const Edge& edge : bucket_edges()) {
        dods.push_back(static_cast<std::uint64_t>(edge.dod));
    }
    const Values values = from_dods(kSignBit, dods);
    for (const std::size_t block_values : {std::size_t{1}, std::size_t{2}, std::size_t{7},
                                           std::size_t{kDefaultBlockValues}}) {
        SCOPED_TRACE(block_values);
        const Bytes column = encoded(values, block_values);
        EXPECT_EQ(decoded(column, values.size()), values);
    }
}

// Deltas and delta-of-deltas that wrap modulo 2^64, under UBSan (D5).
TEST(GorillaTimestamps, ExtremeStepsWrapAndRoundTrip) {
    const Values values{0, kMax, 0, kMax, kSignBit, kSignBit - 1, 0, 0, kMax, kMax, 1, kMax - 1};
    const Bytes column = encoded(values);
    EXPECT_EQ(decoded(column, values.size()), values);
}

TEST(GorillaTimestamps, SeededWalksRoundTripAtEveryBlockSize) {
    test::SplitMix64 random{48};
    Values values;
    std::uint64_t t = 34'200'000'000'000ULL;
    for (int i = 0; i < 20'000; ++i) {
        // Mostly ITCH-like gaps of tens of microseconds, some repeats, some jumps.
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
    for (const std::size_t block_values :
         {std::size_t{1}, std::size_t{3}, std::size_t{1000}, std::size_t{kDefaultBlockValues},
          std::size_t{kMaxBlockValues}}) {
        SCOPED_TRACE(block_values);
        const Bytes column = encoded(values, block_values);
        EXPECT_EQ(decoded(column, values.size()), values);
    }
}

// A silent change in the format or the buckets changes these sizes (T4).
TEST(GorillaTimestamps, EncodedSizeIsPinned) {
    EXPECT_EQ(golden_block().size(), 36U);

    // 10,000 timestamps 25 us apart: the first delta takes 18 bits and every later one
    // is a zero delta-of-delta, so 64 + 18 + 9,998 bits, in 1,260 bytes plus a header.
    Values regular;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        regular.push_back(34'200'000'000'000ULL + (i * 25'000));
    }
    EXPECT_EQ(encoded(regular, 10'000).size(), kBlockHeaderSize + 1'260U);

    test::SplitMix64 random{7};
    Values jittered{34'200'000'000'000ULL};
    for (int i = 1; i < 10'000; ++i) {
        jittered.push_back(jittered.back() + (random() % 50'000));
    }
    DodStats stats;
    const Bytes column = encoded(jittered, kDefaultBlockValues, &stats);
    EXPECT_EQ(column.size(), 23'156U);
    EXPECT_EQ(stats.buckets, (std::array<std::uint64_t, 5>{0, 8'771, 1'226, 0, 0}));
}

TEST(GorillaTimestamps, EncoderRefusesBadBlockSizes) {
    Bytes out;
    EXPECT_EQ(encode_timestamps(golden_values(), 0, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(encode_timestamp_block({}, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_TRUE(out.empty());
}

// One fixture per check in the decoder, each asserting the exact error (T1). Offsets
// are bits from the start of the block; the payload starts at bit 64, its first value
// ends at bit 128.
TEST(GorillaTimestamps, EachCorruptionHasItsError) {
    // Header checks.
    EXPECT_EQ(decoded_block(Bytes{}),
              std::unexpected(error_of(CodecErrorKind::truncated, "block header", 0, 64, 0)));
    EXPECT_EQ(decoded_block(block_with_bits(0, "")),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1, 0)));
    EXPECT_EQ(decoded_block(block_with_bits(kMaxBlockValues + 1, "")),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0,
                                       kMaxBlockValues, kMaxBlockValues + 1)));
    EXPECT_EQ(decoded_block(block_with_bits(10, "00000000")),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 9, 10)));
    Bytes cut_payload = block_with_bits(2, "0");
    cut_payload.pop_back();
    EXPECT_EQ(decoded_block(cut_payload),
              std::unexpected(error_of(CodecErrorKind::truncated, "block payload", 64, 72, 64)));

    // A control code cut short, and a delta-of-delta cut short.
    EXPECT_EQ(decoded_block(block_with_bits(9, "00000001")),
              std::unexpected(
                  error_of(CodecErrorKind::truncated, "delta-of-delta control", 136, 1, 0)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "10000000")),
              std::unexpected(error_of(CodecErrorKind::truncated, "delta-of-delta", 130, 16, 6)));

    // Values in a wider bucket than they need: zero outside '0', and a value one bucket
    // too wide in each bucket.
    EXPECT_EQ(decoded_block(block_with_bits(2, "10" + bits_of(0, 16))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "delta-of-delta bucket", 128, 0, 16)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "110" + bits_of(5, 19))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "delta-of-delta bucket", 128, 16, 19)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "1110" + bits_of((1U << 18U) - 1U, 23))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "delta-of-delta bucket", 128, 19, 23)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "1111" + bits_of((1U << 22U) - 1U, 64))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "delta-of-delta bucket", 128, 23, 64)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "1111" + bits_of(0, 64))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "delta-of-delta bucket", 128, 0, 64)));
    // The most negative value the 16-bit bucket holds, stored in the 19-bit bucket.
    EXPECT_EQ(decoded_block(block_with_bits(2, "110" + bits_of((1U << 19U) - (1U << 15U), 19))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "delta-of-delta bucket", 128, 16, 19)));

    // Padding that is not zero, and a spare byte after the last value.
    EXPECT_EQ(decoded_block(block_with_bits(2, "00000001")),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "block padding", 129, 0, 1)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "000000000")),
              std::unexpected(
                  error_of(CodecErrorKind::trailing_bits, "block padding", 129, 0, 15)));
}

TEST(GorillaTimestamps, ColumnCountMustMatchExactly) {
    const Values values = from_dods(100, {1, 2, 3, 4, 5, 6, 7, 8, 9});
    const Bytes column = encoded(values, 4);  // blocks of 4, 4 and 2
    // The last block: a header, the first value, and a delta of 45 in '10' + 16 bits.
    constexpr std::size_t kLast = kBlockHeaderSize + 11;
    EXPECT_EQ(decoded(column, values.size()), values);
    // One value short: the last block alone would overshoot.
    EXPECT_EQ(decoded(column, 9),
              std::unexpected(error_of(CodecErrorKind::bad_count, "column count",
                                       8 * (column.size() - kLast), 9, 10)));
    // A column that ends early.
    EXPECT_EQ(decoded(column, 11),
              std::unexpected(error_of(CodecErrorKind::truncated, "column values",
                                       8 * column.size(), 11, 10)));
    // A block after the last value the column holds.
    EXPECT_EQ(decoded(column, 8),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "column",
                                       8 * (column.size() - kLast), 0, 8 * kLast)));
}

// A whole column cut at every byte length is refused, never read as zeros (D10).
TEST(GorillaTimestamps, TruncationAtEveryByteLengthIsAnError) {
    const Values values = from_dods(34'200'000'000'000ULL,
                                    {25'000, 0, 40'000, 3'000'000, 1ULL << 40U, kMax, 7});
    const Bytes column = encoded(values, 3);
    for (std::size_t length = 0; length < column.size(); ++length) {
        SCOPED_TRACE(length);
        const auto result = decoded(std::span{column}.first(length), values.size());
        EXPECT_FALSE(result.has_value());
    }
    const Bytes block = encoded(values);
    for (std::size_t length = 0; length < block.size(); ++length) {
        SCOPED_TRACE(length);
        Values out{42};
        const Bytes cut = exact_copy(std::span{block}.first(length));
        EXPECT_FALSE(decode_timestamp_block(cut, out).has_value());
        // A refused block leaves what was there before.
        EXPECT_EQ(out, Values{42});
    }
}

}  // namespace
}  // namespace tick_store::codec
