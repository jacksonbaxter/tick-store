// Run-length coding: golden bytes both ways, with the run values plain and as deltas, the
// run edges, one corrupt fixture per check in the decoder, truncation at every byte
// length, and the encoded size pinned. The golden blocks were worked out by hand from the
// format in run_length.hpp, the nested PFOR blocks' choices included, and written by an
// independent model of the format, kept outside the repository with the raw logs.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/run_length.hpp"

#include <gtest/gtest.h>

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

CodecError error_of(CodecErrorKind kind, std::string_view field, std::uint64_t offset,
                    std::uint64_t expected, std::uint64_t actual) {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

Bytes encoded(const Values& values, std::size_t block_values = kDefaultBlockValues,
              RleStats* stats = nullptr) {
    Bytes out;
    const auto done = encode_rle(values, block_values, out, stats);
    EXPECT_TRUE(done.has_value()) << to_string(done.error());
    return out;
}

std::expected<Values, CodecError> decoded(std::span<const std::byte> column, std::uint64_t count) {
    Values out;
    const Bytes input = exact_copy(column);
    if (const auto done = decode_rle(input, count, out); !done.has_value()) {
        return std::unexpected(done.error());
    }
    return out;
}

std::expected<Values, CodecError> decoded_block(const Bytes& block) {
    Values out;
    const Bytes input = exact_copy(block);
    const auto taken = decode_rle_block(input, out);
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

Bytes pfor_of(const Values& values) {
    Bytes out;
    EXPECT_TRUE(encode_pfor_block(values, PforOptions{.transform = IntTransform::none,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                  out)
                    .has_value());
    return out;
}

// A run-length block from its fields: the run count, then the two nested blocks.
Bytes rle_block(std::uint32_t count, std::uint32_t runs, const Bytes& run_values,
                const Bytes& run_lengths) {
    Bytes payload;
    for (unsigned i = 0; i < 4; ++i) {
        payload.push_back(static_cast<std::byte>((runs >> (8U * i)) & 0xFFU));
    }
    payload.insert(payload.end(), run_values.begin(), run_values.end());
    payload.insert(payload.end(), run_lengths.begin(), run_lengths.end());
    return block_of(count, payload);
}

// Golden 1: runs of 5, 7 and 1, three, two and four long. The values plain, base 1, width
// 3; as deltas zigzag would take the same bytes, and a tie keeps plain. The lengths less
// one, 2, 1, 3, plain, base 1, width 2.
const Values& golden1() {
    static const Values value{5, 5, 5, 7, 7, 1, 1, 1, 1};
    return value;
}
const Bytes& golden1_block() {
    static const Bytes value = bytes({
    0x09, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00,  // 9 values, 41 payload bytes
    0x03, 0x00, 0x00, 0x00,                          // 3 runs
    0x03, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00,  // the run values:
    0x00, 0x01, 0x01, 0x00, 0x03, 0x01, 0x01,        // none, scale 1, plain, width 3, base 1
    0x34, 0x00, 0x00, 0x00,                          // 4, 6, 0; no exceptions
    0x03, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,  // the run lengths less one:
    0x00, 0x01, 0x01, 0x00, 0x02, 0x01, 0x01,        // none, scale 1, plain, width 2, base 1
    0x21, 0x00, 0x00,                                // 1, 0, 2; no exceptions
});
    return value;
}

// Golden 2: twelve runs rising by 1 from 100,000, one, two and three long in turn. As
// deltas the run values are 100,000 and then eleven 1s: width 0 over a base of 1, with
// one exception of 17 bits, 22 bytes against 25 plain.
const Values& golden2() {
    static const Values value = [] {
        Values v;
        for (std::uint64_t k = 0; k < 12; ++k) {
            v.insert(v.end(), (k % 3) + 1, 100'000 + k);
        }
        return v;
    }();
    return value;
}
const Bytes& golden2_block() {
    static const Bytes value = bytes({
    0x18, 0x00, 0x00, 0x00, 0x2D, 0x00, 0x00, 0x00,  // 24 values, 45 payload bytes
    0x0C, 0x00, 0x00, 0x00,                          // 12 runs
    0x0C, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00,  // the run values:
    0x01, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01,        // delta, scale 1, plain, width 0, base 1
    0x01, 0x00, 0x11, 0x00, 0x9F, 0x86, 0x01,        // one exception of 17 bits, at 0
    0x0C, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00,  // the run lengths less one:
    0x00, 0x01, 0x01, 0x00, 0x02, 0x00,              // none, scale 1, plain, width 2, base 0
    0x24, 0x49, 0x92, 0x00, 0x00,                    // 0, 1, 2 four times; no exceptions
});
    return value;
}

TEST(RunLength, GoldenBytesBothWays) {
    RleStats stats;
    EXPECT_EQ(encoded(golden1(), kDefaultBlockValues, &stats), golden1_block());
    EXPECT_EQ(decoded_block(golden1_block()), golden1());
    EXPECT_EQ(stats.delta_blocks, 0U);
    EXPECT_EQ(encoded(golden2(), kDefaultBlockValues, &stats), golden2_block());
    EXPECT_EQ(decoded_block(golden2_block()), golden2());
    EXPECT_EQ(stats.delta_blocks, 1U);
    EXPECT_EQ(stats.runs, 15U);
}

TEST(RunLength, EmptyColumnHasNoBlocks) {
    EXPECT_TRUE(encoded({}).empty());
    EXPECT_EQ(decoded({}, 0), Values{});
}

// One run filling a block, a run of one value at each end, every value its own run, and
// runs at the edges of the word.
TEST(RunLength, RunEdgesRoundTrip) {
    const std::vector<Values> cases{
        Values(kMaxBlockValues, 7),
        Values{kMax},
        Values{0, kMax},
        Values{0, kMax, 0, kMax, 0},
        Values{1, 2, 2, 2, 2, 3},
        Values{kMax, kMax, kMax, 0},
    };
    for (const Values& values : cases) {
        SCOPED_TRACE(values.size());
        for (const std::size_t block_values : {std::size_t{1}, std::size_t{2}, std::size_t{kMaxBlockValues}}) {
            RleStats stats;
            EXPECT_EQ(decoded(encoded(values, block_values, &stats), values.size()), values);
        }
    }
    RleStats stats;
    (void)encoded(Values(kMaxBlockValues, 7), kMaxBlockValues, &stats);
    EXPECT_EQ(stats.runs, 1U);
}

TEST(RunLength, SeededValuesRoundTripAtEveryBlockSize) {
    test::SplitMix64 random{73};
    Values values;
    std::uint64_t current = 0;
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t pick = random() % 100;
        if (pick < 5) {
            current = random();
        } else if (pick < 15) {
            current = random() % 4;
        }
        values.push_back(current);
    }
    for (const std::size_t block_values :
         {std::size_t{1}, std::size_t{3}, std::size_t{1000}, std::size_t{1025},
          std::size_t{kDefaultBlockValues}, std::size_t{kMaxBlockValues}}) {
        SCOPED_TRACE(block_values);
        EXPECT_EQ(decoded(encoded(values, block_values), values.size()), values);
    }
}

// A silent change in the format or the encoder's choices changes these sizes (T4).
TEST(RunLength, EncodedSizeIsPinned) {
    EXPECT_EQ(golden1_block().size(), 49U);
    EXPECT_EQ(golden2_block().size(), 53U);
    // A tracking-number-like column: 0, with a run of non-zero values about every 100.
    test::SplitMix64 random{11};
    Values tracking;
    while (tracking.size() < 10'000) {
        tracking.insert(tracking.end(), 50 + (random() % 100), 0);
        tracking.insert(tracking.end(), 1 + (random() % 3), 1 + (random() % 40));
    }
    tracking.resize(10'000);
    RleStats stats;
    EXPECT_EQ(encoded(tracking, kDefaultBlockValues, &stats).size(), 411U);
    EXPECT_EQ(stats.runs, 201U);
}

// encode_rle copies a block that repeats the one before, and the copy must be exactly
// what encoding it alone gives, the counts included.
TEST(RunLength, ABlockRepeatingTheOneBeforeIsTheSameBytes) {
    const Values five{5, 5, 5};
    const Values ramp{10, 20, 30};
    const Values steps{7, 7, 9};
    const std::vector<Values> blocks{five, five, five, ramp, ramp, steps, five, Values{5}};
    Values values;
    Bytes alone;
    RleStats alone_stats;
    for (const Values& block : blocks) {
        values.insert(values.end(), block.begin(), block.end());
        ASSERT_TRUE(encode_rle_block(block, alone, &alone_stats).has_value());
    }
    RleStats stats;
    EXPECT_EQ(encoded(values, 3, &stats), alone);
    EXPECT_EQ(stats.runs, alone_stats.runs);
    EXPECT_EQ(stats.delta_blocks, alone_stats.delta_blocks);
    EXPECT_EQ(stats.repeated, 3U);
    EXPECT_EQ(alone_stats.repeated, 0U);
    EXPECT_EQ(decoded(alone, values.size()), values);
}

// The fuzz input that timed out on debian, in
// crashers/run_length/timeout-one-run-of-two-to-the-twenty: one block of 2^20 equal values
// in 48 bytes, which the oracle re-encodes in blocks of 3. Each of those blocks but the
// first is a copy now; before, the encoder ran 349,526 times, 7.5 s with the fuzz build's
// sanitizers on the Mac and over the 10 s limit on debian.
TEST(RunLength, TwoToTheTwentyEqualValuesInBlocksOfThreeEncodeOneBlock) {
    const Values values(kMaxBlockValues, 42);
    RleStats stats;
    const Bytes column = encoded(values, 3, &stats);
    // 349,525 blocks of three, all but the first copied, and a last block of one value.
    constexpr std::uint64_t kFull = kMaxBlockValues / 3;
    EXPECT_EQ(stats.repeated, kFull - 1);
    EXPECT_EQ(stats.runs, kFull + 1);
    EXPECT_EQ(decoded(column, values.size()), values);
    // The crasher is the encoder's own block for these values.
    Bytes one;
    ASSERT_TRUE(encode_rle_block(values, one).has_value());
    EXPECT_EQ(one, bytes({0x00, 0x00, 0x10, 0x00, 0x28, 0x00, 0x00, 0x00,  // 2^20 values
                          0x01, 0x00, 0x00, 0x00,                          // one run
                          0x01, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,  // PFOR: its value
                          0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x2A, 0x00, 0x00,
                          0x01, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00,  // PFOR: its length
                          0x00, 0x01, 0x01, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0x0F, 0x00, 0x00}));
}

TEST(RunLength, EncoderRefusesBadBlockSizes) {
    Bytes out;
    EXPECT_EQ(encode_rle(golden1(), 0, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(encode_rle_block({}, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_TRUE(out.empty());
}

// One fixture per check in the decoder, each asserting the exact error (T1). Offsets are
// bits from the start of the block: the run count at 64 and the run values' block at 96.
TEST(RunLength, EachCorruptionHasItsError) {
    EXPECT_EQ(decoded_block(Bytes{}),
              std::unexpected(error_of(CodecErrorKind::truncated, "block header", 0, 64, 0)));
    EXPECT_EQ(decoded_block(block_of(0, bytes({0x00}))),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1, 0)));
    // Fewer bytes than the smallest payload, 36.
    EXPECT_EQ(decoded_block(block_of(3, Bytes(35, std::byte{0}))),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 0, 3)));
    const Bytes run_values = pfor_of({5, 7});
    const Bytes run_lengths = pfor_of({1, 0});
    const std::uint64_t lengths_at = 96 + (8 * run_values.size());
    ASSERT_EQ(decoded_block(rle_block(3, 2, run_values, run_lengths)), (Values{5, 5, 7}));
    EXPECT_EQ(decoded_block(rle_block(3, 0, run_values, run_lengths)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "rle runs", 64, 3, 0)));
    EXPECT_EQ(decoded_block(rle_block(3, 4, run_values, run_lengths)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "rle runs", 64, 3, 4)));
    Bytes bad_values = run_values;
    bad_values[kBlockHeaderSize] = std::byte{9};
    EXPECT_EQ(decoded_block(rle_block(3, 2, bad_values, run_lengths)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor transform", 160, 3, 9)));
    Bytes bad_lengths = run_lengths;
    bad_lengths[kBlockHeaderSize] = std::byte{9};
    EXPECT_EQ(decoded_block(rle_block(3, 2, run_values, bad_lengths)),
              std::unexpected(
                  error_of(CodecErrorKind::bad_field, "pfor transform", lengths_at + 64, 3, 9)));
    EXPECT_EQ(decoded_block(rle_block(3, 2, pfor_of({5, 7, 5}), run_lengths)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "rle value count", 96, 2, 3)));
    EXPECT_EQ(decoded_block(rle_block(3, 2, run_values, pfor_of({2}))),
              std::unexpected(
                  error_of(CodecErrorKind::bad_count, "rle length count", lengths_at, 2, 1)));
    Bytes spare = run_lengths;
    spare.push_back(std::byte{0});
    EXPECT_EQ(decoded_block(rle_block(3, 2, run_values, spare)),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "rle block",
                                       lengths_at + (8 * run_lengths.size()), 0, 8)));
    // Two runs of 5 side by side are one run.
    EXPECT_EQ(decoded_block(rle_block(3, 2, pfor_of({5, 5}), run_lengths)),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "rle run value", 96, 1, 0)));
    // The first run takes all three values, leaving none for the second.
    EXPECT_EQ(decoded_block(rle_block(3, 2, run_values, pfor_of({2, 0}))),
              std::unexpected(
                  error_of(CodecErrorKind::bad_count, "rle run lengths", lengths_at, 3, 4)));
    // The first run is four long in a block of three.
    EXPECT_EQ(decoded_block(rle_block(3, 2, run_values, pfor_of({3, 0}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "rle run length", lengths_at, 2, 3)));
    // Runs of one and one in a block of three.
    EXPECT_EQ(decoded_block(rle_block(3, 2, run_values, pfor_of({0, 0}))),
              std::unexpected(error_of(CodecErrorKind::bad_count, "rle run lengths", lengths_at, 3, 2)));
}

TEST(RunLength, ColumnCountMustMatchExactly) {
    const Values values{1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    const Bytes column = encoded(values, 4);  // blocks of 4, 4 and 2
    Bytes last;
    ASSERT_TRUE(encode_rle_block(std::span{values}.subspan(8), last).has_value());
    EXPECT_EQ(decoded(column, values.size()), values);
    EXPECT_EQ(decoded(column, 9),
              std::unexpected(error_of(CodecErrorKind::bad_count, "column count",
                                       8 * (column.size() - last.size()), 9, 10)));
    EXPECT_EQ(decoded(column, 11),
              std::unexpected(error_of(CodecErrorKind::truncated, "column values",
                                       8 * column.size(), 11, 10)));
}

TEST(RunLength, TruncationAtEveryByteLengthIsAnError) {
    for (const Values& values : {golden1(), golden2()}) {
        const Bytes block = encoded(values, values.size());
        for (std::size_t length = 0; length < block.size(); ++length) {
            SCOPED_TRACE(length);
            Values out{42};
            const Bytes cut = exact_copy(std::span{block}.first(length));
            EXPECT_FALSE(decode_rle_block(cut, out).has_value());
            EXPECT_EQ(out, Values{42});
        }
        const Bytes column = encoded(values, 5);
        for (std::size_t length = 0; length < column.size(); ++length) {
            SCOPED_TRACE(length);
            EXPECT_FALSE(decoded(std::span{column}.first(length), values.size()).has_value());
        }
    }
}

}  // namespace
}  // namespace tick_store::codec
