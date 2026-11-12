// The dictionary codec: golden bytes both ways in each of its three modes, the dictionary
// and code edges, one corrupt fixture per check in the decoder, truncation at every byte
// length, and the encoded size pinned. The golden blocks were worked out by hand from the
// format in dictionary.hpp, the nested PFOR blocks' choices included, and written by an
// independent model of the format, kept outside the repository with the raw logs.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/pfor.hpp"

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
#include <utility>
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
              DictStats* stats = nullptr) {
    Bytes out;
    const auto done = encode_dict(values, block_values, out, stats);
    EXPECT_TRUE(done.has_value()) << to_string(done.error());
    return out;
}

std::expected<Values, CodecError> decoded(std::span<const std::byte> column, std::uint64_t count) {
    Values out;
    const Bytes input = exact_copy(column);
    if (const auto done = decode_dict(input, count, out); !done.has_value()) {
        return std::unexpected(done.error());
    }
    return out;
}

std::expected<Values, CodecError> decoded_block(const Bytes& block) {
    Values out;
    const Bytes input = exact_copy(block);
    const auto taken = decode_dict_block(input, out);
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

// A nested PFOR block, for building fixtures.
Bytes pfor_of(const Values& values, IntTransform transform = IntTransform::none) {
    Bytes out;
    EXPECT_TRUE(encode_pfor_block(values, PforOptions{.transform = transform,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                  out)
                    .has_value());
    return out;
}

// A dictionary block from its fields: mode, entry count, dictionary block, and the rest.
Bytes dict_block(std::uint32_t count, unsigned mode, std::uint32_t entries, const Bytes& dictionary,
                 const Bytes& rest) {
    Bytes payload{static_cast<std::byte>(mode)};
    for (unsigned i = 0; i < 4; ++i) {
        payload.push_back(static_cast<std::byte>((entries >> (8U * i)) & 0xFFU));
    }
    payload.insert(payload.end(), dictionary.begin(), dictionary.end());
    payload.insert(payload.end(), rest.begin(), rest.end());
    return block_of(count, payload);
}

// Golden 1, Huffman: 3 twelve times, 5 twice, 8 and 2 once. The ranked dictionary is 3, 5,
// 2, 8, and the code lengths 1, 2, 3, 3 give codes 0, 10, 110 and 111: 39 bits with the
// table. PFOR indices would take 20 bytes, and move-to-front ranks 20 with an 18-byte
// sorted dictionary.
const Values& golden1() {
    static const Values value{3, 3, 3, 5, 3, 3, 3, 3, 5, 3, 3, 8, 3, 3, 3, 2};
    return value;
}
const Bytes& golden1_block() {
    static const Bytes value = bytes({
    0x10, 0x00, 0x00, 0x00, 0x1D, 0x00, 0x00, 0x00,  // 16 values, 29 payload bytes
    0x01,                                            // ranked, Huffman
    0x04, 0x00, 0x00, 0x00,                          // 4 entries
    0x04, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00,  // the dictionary's PFOR block:
    0x00, 0x01, 0x01,                                // none, scale 1
    0x00, 0x03, 0x01, 0x02,                          // plain, width 3, base 2
    0x19, 0x0C, 0x00, 0x00,                          // 1, 3, 0, 6; no exceptions
    0x03, 0x25, 0x08, 0x23, 0x8C,                    // longest 3; counts 1, 1, 2 in 3 bits
                                                     // each; then the 16 codes
});
    return value;
}

// Golden 2, ranked PFOR: 200 values of 9 but a 4 at index 150. Indices at width 0 with one
// exception, 1 bit at 150, beat Huffman's bit a value.
const Values& golden2() {
    static const Values value = [] {
        Values v(200, 9);
        v[150] = 4;
        return v;
    }();
    return value;
}
const Bytes& golden2_block() {
    static const Bytes value = bytes({
    0xC8, 0x00, 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00,  // 200 values, 43 payload bytes
    0x00,                                            // ranked, PFOR
    0x02, 0x00, 0x00, 0x00,                          // 2 entries
    0x02, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,  // the dictionary, 9 and 4:
    0x00, 0x01, 0x01, 0x00, 0x03, 0x01, 0x04,        // none, scale 1, plain, width 3, base 4
    0x05, 0x00, 0x00,                                // 5, 0; no exceptions
    0xC8, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,  // the 200 indices:
    0x00, 0x01, 0x01, 0x00, 0x00, 0x00,              // none, scale 1, plain, width 0
    0x01, 0x00, 0x01, 0x08, 0x96, 0x01,              // one exception of 1 bit, at 150
});
    return value;
}

// Golden 3, sorted with move-to-front: six runs of 30 values. Each run's first value has
// rank k, the rest rank 0, so the ranks are width 0 with five exceptions, and the sorted
// dictionary's deltas, 11, 9, 10, 10, 10, 11, pack in 2 bits.
const Values& golden3() {
    static const Values value = [] {
        Values v;
        for (const std::uint64_t run : {11U, 20U, 30U, 40U, 50U, 61U}) {
            v.insert(v.end(), 30, run);
        }
        return v;
    }();
    return value;
}
const Bytes& golden3_block() {
    static const Bytes value = bytes({
    0xB4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,  // 180 values, 48 payload bytes
    0x02,                                            // sorted, move-to-front
    0x06, 0x00, 0x00, 0x00,                          // 6 entries
    0x06, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00,  // the dictionary:
    0x01, 0x01, 0x01, 0x00, 0x02, 0x01, 0x09,        // delta, scale 1, plain, width 2, base 9
    0x52, 0x09, 0x00, 0x00,                          // 2, 0, 1, 1, 1, 2; no exceptions
    0xB4, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,  // the 180 ranks:
    0x00, 0x01, 0x01, 0x00, 0x00, 0x00,              // none, scale 1, plain, width 0
    0x05, 0x00, 0x03, 0x05,                          // five exceptions of 3 bits, gaps in 5
    0xBE, 0xF7, 0xDE, 0x01,                          // gaps 30, 29, 29, 29, 29
    0xD1, 0x58,                                      // ranks 1, 2, 3, 4, 5
});
    return value;
}

TEST(Dictionary, GoldenBytesBothWays) {
    DictStats stats;
    EXPECT_EQ(encoded(golden1(), kDefaultBlockValues, &stats), golden1_block());
    EXPECT_EQ(decoded_block(golden1_block()), golden1());
    EXPECT_EQ(encoded(golden2(), kDefaultBlockValues, &stats), golden2_block());
    EXPECT_EQ(decoded_block(golden2_block()), golden2());
    EXPECT_EQ(encoded(golden3(), kDefaultBlockValues, &stats), golden3_block());
    EXPECT_EQ(decoded_block(golden3_block()), golden3());
    EXPECT_EQ(stats.blocks, (std::array<std::uint64_t, kDictModes>{1, 1, 1}));
    EXPECT_EQ(stats.entries, 4U + 2U + 6U);
}

TEST(Dictionary, EmptyColumnHasNoBlocks) {
    EXPECT_TRUE(encoded({}).empty());
    EXPECT_EQ(decoded({}, 0), Values{});
}

// One distinct value, at the edges of the word: a dictionary of one entry, and in Huffman
// mode a code of no bits.
TEST(Dictionary, OneEntryTakesNoIndexBits) {
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, kMax}) {
        for (const std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{5000}}) {
            SCOPED_TRACE(value);
            SCOPED_TRACE(count);
            const Values values(count, value);
            EXPECT_EQ(decoded(encoded(values), values.size()), values);
        }
    }
    // A Huffman block of one entry, by hand: longest code 0 and nothing after it.
    const Bytes one = dict_block(3, 1, 1, pfor_of({42}), bytes({0x00}));
    EXPECT_EQ(decoded_block(one), (Values{42, 42, 42}));
}

// Move-to-front is held to 2,048 entries: the encoder stops trying it past that, and the
// decoder refuses a mode-2 block with more.
TEST(Dictionary, MoveToFrontStopsAtItsEntryLimit) {
    for (const std::uint64_t distinct : {kDictMtfMaxEntries, kDictMtfMaxEntries + 1}) {
        SCOPED_TRACE(distinct);
        // Runs of 4, so move-to-front would win where it is allowed.
        Values values;
        for (std::uint64_t i = 0; i < distinct; ++i) {
            values.insert(values.end(), 4, i * 3);
        }
        DictStats stats;
        const Bytes column = encoded(values, kMaxBlockValues, &stats);
        EXPECT_EQ(stats.blocks[2], distinct == kDictMtfMaxEntries ? 1U : 0U);
        EXPECT_EQ(decoded(column, values.size()), values);
    }
}

// Counts that grow like the Fibonacci numbers give the longest codes for their size: 26
// entries, codes of up to 25 bits. Huffman wins there, and every value round-trips.
TEST(Dictionary, LongHuffmanCodesRoundTrip) {
    Values values;
    std::uint64_t a = 1;
    std::uint64_t b = 1;
    for (std::uint64_t entry = 0; entry < 26; ++entry) {
        values.insert(values.end(), a, entry);
        const std::uint64_t next = a + b;
        a = b;
        b = next;
    }
    ASSERT_LE(values.size(), kMaxBlockValues);
    // Shuffled, so runs give move-to-front nothing.
    test::SplitMix64 random{26};
    for (std::size_t i = values.size() - 1; i > 0; --i) {
        std::swap(values[i], values[random() % (i + 1)]);
    }
    DictStats stats;
    const Bytes column = encoded(values, kMaxBlockValues, &stats);
    EXPECT_EQ(stats.blocks[1], 1U);
    EXPECT_EQ(decoded(column, values.size()), values);
}

// The decoder takes codes from a loaded word, two a lookup where both are short, one where
// the first is long, then finishes the last ones bit by bit. A skewed mix of short and long
// codes, shuffled, at every count around those hand-overs, must decode to the values both
// ways a block can end: on a pair and on a single code.
TEST(Dictionary, HuffmanCodesMixedShortAndLongRoundTripAtEveryCount) {
    Values pool;
    std::uint64_t a = 1;
    std::uint64_t b = 2;
    for (std::uint64_t entry = 0; entry < 18; ++entry) {
        pool.insert(pool.end(), a, entry * 1'000);
        const std::uint64_t next = a + b;
        a = b;
        b = next;
    }
    test::SplitMix64 random{18};
    for (std::size_t i = pool.size() - 1; i > 0; --i) {
        std::swap(pool[i], pool[random() % (i + 1)]);
    }
    std::array<std::uint64_t, kDictModes> blocks{};
    for (std::size_t count = 1; count <= 400; ++count) {
        SCOPED_TRACE(count);
        const Values values(pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(count));
        DictStats stats;
        const Bytes column = encoded(values, kMaxBlockValues, &stats);
        EXPECT_EQ(decoded(column, values.size()), values);
        for (std::size_t mode = 0; mode < kDictModes; ++mode) {
            blocks[mode] += stats.blocks[mode];
        }
    }
    // Most counts choose Huffman, so the test reaches its loops.
    EXPECT_GT(blocks[1], 300U);
}

TEST(Dictionary, SeededValuesRoundTripAtEveryBlockSize) {
    test::SplitMix64 random{61};
    Values values;
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t pick = random() % 100;
        if (pick < 60) {
            values.push_back(100);
        } else if (pick < 80) {
            values.push_back((random() % 10) * 100);
        } else if (pick < 95) {
            values.push_back(random() % 5'000);
        } else {
            values.push_back(random());
        }
    }
    values.insert(values.end(), {0, kMax, 0, kMax, 1, kMax - 1});
    for (const std::size_t block_values :
         {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{100}, std::size_t{1025},
          std::size_t{kDefaultBlockValues}, std::size_t{kMaxBlockValues}}) {
        SCOPED_TRACE(block_values);
        DictStats stats;
        const Bytes column = encoded(values, block_values, &stats);
        EXPECT_EQ(decoded(column, values.size()), values);
    }
}

// A silent change in the format or the encoder's choices changes these sizes (T4).
TEST(Dictionary, EncodedSizeIsPinned) {
    EXPECT_EQ(golden1_block().size(), 37U);
    EXPECT_EQ(golden2_block().size(), 51U);
    EXPECT_EQ(golden3_block().size(), 56U);
    // Share sizes from a fixed mix: 100 half the time, then a few round lots and odd lots.
    test::SplitMix64 random{5};
    constexpr std::array<std::uint64_t, 8> kLots{100, 200, 300, 500, 1000, 1, 50, 400};
    Values shares;
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t pick = random() % 16;
        shares.push_back(pick < 8 ? 100 : kLots[pick % kLots.size()]);
    }
    DictStats stats;
    EXPECT_EQ(encoded(shares, kDefaultBlockValues, &stats).size(), 2'929U);
    EXPECT_EQ(stats.blocks, (std::array<std::uint64_t, kDictModes>{0, 3, 0}));
}

// A mode set limits what the encoder tries, never what the decoder reads: the share mix
// above under each set, its blocks only in allowed modes, ranked PFOR when nothing else is,
// and every column decoded back by the same decoder.
TEST(Dictionary, AModeSetLimitsTheEncoderAlone) {
    test::SplitMix64 random{5};
    constexpr std::array<std::uint64_t, 8> kLots{100, 200, 300, 500, 1000, 1, 50, 400};
    Values shares;
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t pick = random() % 16;
        shares.push_back(pick < 8 ? 100 : kLots[pick % kLots.size()]);
    }
    const auto under = [&](DictModeSet modes, DictStats& stats) {
        Bytes out;
        const auto done = encode_dict(shares, kDefaultBlockValues, out, &stats, modes);
        EXPECT_TRUE(done.has_value()) << to_string(done.error());
        EXPECT_EQ(decoded(out, shares.size()), shares);
        return out;
    };
    DictStats all;
    EXPECT_EQ(under(kDictAllModes, all).size(), 2'929U);
    EXPECT_EQ(all.blocks, (std::array<std::uint64_t, kDictModes>{0, 3, 0}));
    for (const DictModeSet modes : {kDictRankedPforOnly, DictModeSet{0}}) {
        SCOPED_TRACE(static_cast<unsigned>(modes));
        DictStats stats;
        EXPECT_EQ(under(modes, stats).size(), 3'937U);
        EXPECT_EQ(stats.blocks, (std::array<std::uint64_t, kDictModes>{3, 0, 0}));
    }
    DictStats no_huffman;
    EXPECT_LE(under(0b101, no_huffman).size(), 3'937U);
    EXPECT_EQ(no_huffman.blocks[1], 0U);
    EXPECT_EQ(no_huffman.blocks[0] + no_huffman.blocks[2], 3U);
}

// encode_dict copies a block that repeats the one before, and the copy must be exactly
// what encoding it alone gives, the counts included.
TEST(Dictionary, ABlockRepeatingTheOneBeforeIsTheSameBytes) {
    const Values five{5, 5, 5};
    const Values ramp{1, 2, 3};
    const std::vector<Values> blocks{five, five, five, ramp, ramp, five, Values{5}};
    Values values;
    Bytes alone;
    DictStats alone_stats;
    for (const Values& block : blocks) {
        values.insert(values.end(), block.begin(), block.end());
        ASSERT_TRUE(encode_dict_block(block, alone, &alone_stats).has_value());
    }
    DictStats stats;
    EXPECT_EQ(encoded(values, 3, &stats), alone);
    EXPECT_EQ(stats.blocks, alone_stats.blocks);
    EXPECT_EQ(stats.entries, alone_stats.entries);
    EXPECT_EQ(stats.repeated, 3U);
    EXPECT_EQ(alone_stats.repeated, 0U);
    EXPECT_EQ(decoded(alone, values.size()), values);
}

// The fuzz input that timed out on the CI runner, in
// crashers/dictionary/timeout-one-entry-codes-of-no-bits: one block of 2^20 equal values in
// 31 bytes, which the oracle re-encodes in blocks of 3. Each of those blocks but the first
// is a copy now; before, the encoder ran 349,526 times, which took over ten seconds
// with the fuzz build's sanitizers on the runner.
TEST(Dictionary, TwoToTheTwentyEqualValuesInBlocksOfThreeEncodeOneBlock) {
    const Values values(kMaxBlockValues, 42);
    DictStats stats;
    const Bytes column = encoded(values, 3, &stats);
    // 349,525 blocks of three, all but the first copied, and a last block of one value.
    constexpr std::uint64_t kFull = kMaxBlockValues / 3;
    EXPECT_EQ(stats.repeated, kFull - 1);
    EXPECT_EQ(stats.blocks, (std::array<std::uint64_t, kDictModes>{0, kFull + 1, 0}));
    EXPECT_EQ(decoded(column, values.size()), values);
    // The crasher is the encoder's own block for these values.
    Bytes one;
    ASSERT_TRUE(encode_dict_block(values, one).has_value());
    EXPECT_EQ(one, bytes({0x00, 0x00, 0x10, 0x00, 0x17, 0x00, 0x00, 0x00,  // 2^20 values
                          0x01, 0x01, 0x00, 0x00, 0x00,                    // Huffman, 1 entry
                          0x01, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,  // PFOR: 1 value
                          0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x2A, 0x00, 0x00,
                          0x00}));                                         // codes of no bits
}

TEST(Dictionary, EncoderRefusesBadBlockSizes) {
    Bytes out;
    EXPECT_EQ(encode_dict(golden1(), 0, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(encode_dict_block({}, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_TRUE(out.empty());
}

// One fixture per check in the decoder, each asserting the exact error (T1). Offsets are
// bits from the start of the block: the mode at 64, the entry count at 72 and the
// dictionary's block at 104.
TEST(Dictionary, EachHeaderCorruptionHasItsError) {
    EXPECT_EQ(decoded_block(Bytes{}),
              std::unexpected(error_of(CodecErrorKind::truncated, "block header", 0, 64, 0)));
    EXPECT_EQ(decoded_block(block_of(0, bytes({0x00}))),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1, 0)));
    // Fewer bytes than the smallest payload, 21.
    EXPECT_EQ(decoded_block(block_of(3, Bytes(20, std::byte{0}))),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 0, 3)));
    const Bytes dictionary = pfor_of({3, 5});
    const Bytes indices = pfor_of({0, 0, 1});
    ASSERT_EQ(decoded_block(dict_block(3, 0, 2, dictionary, indices)), (Values{3, 3, 5}));
    EXPECT_EQ(decoded_block(dict_block(3, 3, 2, dictionary, indices)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict mode", 64, 2, 3)));
    EXPECT_EQ(decoded_block(dict_block(3, 0, 0, dictionary, indices)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "dict entries", 72, 3, 0)));
    EXPECT_EQ(decoded_block(dict_block(3, 0, 4, dictionary, indices)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "dict entries", 72, 3, 4)));
    EXPECT_EQ(decoded_block(dict_block(3'000, 2, 2'049, dictionary, indices)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "dict entries", 72, 2'048, 2'049)));
    // The dictionary's own block refuses its transform byte, reported from this block's
    // start: 104 + 64.
    Bytes bad_transform = dictionary;
    bad_transform[kBlockHeaderSize] = std::byte{9};
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, bad_transform, indices)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor transform", 168, 3, 9)));
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, pfor_of({3, 5, 7}), indices)),
              std::unexpected(error_of(CodecErrorKind::bad_count, "dict dictionary count", 104, 2, 3)));
}

TEST(Dictionary, EachIndexCorruptionHasItsError) {
    const Bytes dictionary = pfor_of({3, 5});
    const std::uint64_t indices_at = 104 + (8 * dictionary.size());
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, dictionary, pfor_of({0, 1}))),
              std::unexpected(
                  error_of(CodecErrorKind::bad_count, "dict index count", indices_at, 3, 2)));
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, dictionary, pfor_of({0, 0, 2}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict index", indices_at, 1, 2)));
    Bytes spare = pfor_of({0, 0, 1});
    const std::uint64_t spare_at = indices_at + (8 * spare.size());
    spare.push_back(std::byte{0});
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, dictionary, spare)),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "dict block", spare_at, 0, 8)));
    Bytes bad_indices = pfor_of({0, 0, 1});
    bad_indices[kBlockHeaderSize] = std::byte{9};
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, dictionary, bad_indices)),
              std::unexpected(
                  error_of(CodecErrorKind::bad_field, "pfor transform", indices_at + 64, 3, 9)));
}

// Huffman streams by hand. With two entries, each count takes 2 bits.
TEST(Dictionary, EachHuffmanCorruptionHasItsError) {
    const Bytes dictionary = pfor_of({3, 5});
    const std::uint64_t codes_at = 104 + (8 * dictionary.size());
    // Longest 1, two codes of length 1, then the codes 0, 0, 1: 13 bits.
    const Bytes valid = bytes({0x01, 0x88});
    ASSERT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, valid)), (Values{3, 3, 5}));
    EXPECT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, bytes({0x00, 0x00}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict longest code", codes_at, 32, 0)));
    EXPECT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, bytes({0x21, 0x00}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict longest code", codes_at, 32, 33)));
    EXPECT_EQ(decoded_block(dict_block(3, 1, 1, pfor_of({3}), bytes({0x01, 0x00}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict longest code", codes_at - 8, 0, 1)));
    // One code of length 1 for two entries.
    EXPECT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, bytes({0x01, 0x40}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict code counts", codes_at + 8, 2, 1)));
    // Longest 2, but no code of length 2.
    EXPECT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, bytes({0x02, 0x80}))),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "dict longest code", codes_at, 1, 2)));
    // Lengths 1 and 2 leave a quarter of the code space unused.
    EXPECT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, bytes({0x02, 0x50}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict code lengths", codes_at + 8, 4, 3)));
    // Seven values need 17 bits; the stream has 16.
    EXPECT_EQ(decoded_block(dict_block(7, 1, 2, dictionary, bytes({0x01, 0x81}))),
              std::unexpected(error_of(CodecErrorKind::truncated, "dict codes", codes_at + 16, 1, 0)));
    Bytes spare = valid;
    spare.push_back(std::byte{0});
    EXPECT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, spare)),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "dict codes", codes_at + 13, 0, 11)));
    // A padding bit set after the 13 bits.
    EXPECT_EQ(decoded_block(dict_block(3, 1, 2, dictionary, bytes({0x01, 0x89}))),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "dict codes", codes_at + 13, 0, 1)));
}

TEST(Dictionary, EachDictionaryOrderCorruptionHasItsError) {
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, pfor_of({3, 5}), pfor_of({0, 0, 0}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict entry unused", 104, 1, 0)));
    // Ranked: 3 is used more often than 5, which comes first.
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, pfor_of({5, 3}), pfor_of({1, 1, 0}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict entry order", 104, 0, 1)));
    // Ranked: used equally often, and not in ascending order.
    EXPECT_EQ(decoded_block(dict_block(2, 0, 2, pfor_of({5, 3}), pfor_of({0, 1}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict entry order", 104, 0, 1)));
    // Ranked: the same value twice, the first used more often.
    EXPECT_EQ(decoded_block(dict_block(3, 0, 2, pfor_of({3, 3}), pfor_of({0, 0, 1}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict entry repeated", 104, 0, 1)));
    // Sorted: not ascending.
    EXPECT_EQ(decoded_block(dict_block(2, 2, 2, pfor_of({5, 3}, IntTransform::delta), pfor_of({0, 1}))),
              std::unexpected(error_of(CodecErrorKind::bad_field, "dict entry order", 104, 0, 1)));
}

// Entries the decoder's hash set puts in one slot (B-1 of the final review, docs/REVIEW.md).
// The set hashes a value by the bits from 32 up of its product with 0x9E3779B97F4A7C15, so
// entries that agree in their low 48 bits share a slot, as do entries whose products with
// it are all below 2^32 (j times the multiplier's inverse). In a ranked block each entry used
// once, both are valid dictionaries; the second also defeats a hash on the top bits.
Values entries_in_one_slot(std::size_t count, bool by_inverse) {
    constexpr std::uint64_t kMultiplier = 0x9E3779B97F4A7C15ULL;
    std::uint64_t inverse = kMultiplier;
    for (int i = 0; i < 6; ++i) {
        inverse *= 2U - (kMultiplier * inverse);
    }
    Values entries;
    entries.reserve(count);
    for (std::uint64_t k = 1; k <= count; ++k) {
        entries.push_back(by_inverse ? k * inverse : k << 48U);
    }
    std::ranges::sort(entries);
    return entries;
}

// The repeat check's work on K entries in one slot was K^2 / 2 probes, 536,887,296 steps for
// K = 32,768, from a block of under a kilobyte. Bounded now: at most 4 K + 17 steps in the
// hash set, then the sort's comparisons, at most 3 K log2 K for these entries, and one pass
// of K to find a pair. Work, not wall time.
TEST(Dictionary, RepeatScanWorkIsBoundedOnEntriesInOneHashSlot) {
    for (const bool by_inverse : {false, true}) {
        for (const std::size_t count : {std::size_t{1'024}, std::size_t{32'768}}) {
            SCOPED_TRACE(by_inverse);
            SCOPED_TRACE(count);
            const auto bound = [](std::uint64_t k) {
                const auto log2 = static_cast<std::uint64_t>(std::bit_width(k - 1));
                return (4U * k) + 17U + (3U * k * log2) + k;
            };
            const Values entries = entries_in_one_slot(count, by_inverse);
            const RepeatScan distinct = scan_for_repeats(entries);
            EXPECT_FALSE(distinct.repeated);
            EXPECT_LE(distinct.steps, bound(count));
            // The same entries with the last repeated at the end, where a scan finds it last.
            Values repeated = entries;
            repeated.push_back(entries.back());
            const RepeatScan found = scan_for_repeats(repeated);
            EXPECT_TRUE(found.repeated);
            EXPECT_LE(found.steps, bound(count + 1));
        }
    }
    // An ordinary dictionary takes the hash set alone: about one step an entry.
    test::SplitMix64 random{48};
    Values ordinary(32'768);
    for (std::uint64_t& value : ordinary) {
        value = random();
    }
    EXPECT_LE(scan_for_repeats(ordinary).steps, 2U * ordinary.size());
}

// The reviewer's block: 32,768 entries in one slot, each used once, the dictionary and the
// indices both delta-coded PFOR blocks, so it is 738 bytes. It must decode. These are the
// bytes of crashers/dictionary/timeout-entries-in-one-hash-slot and of a fuzz seed, which
// cost the crasher replay 13.9 s in a Debug build before the repeat check could sort.
TEST(Dictionary, ABlockOfEntriesInOneHashSlotDecodes) {
    constexpr std::uint32_t kEntries = 32'768;
    const Values dictionary = entries_in_one_slot(kEntries, false);
    Values indices(kEntries);
    for (std::uint32_t i = 0; i < kEntries; ++i) {
        indices[i] = i;
    }
    const Bytes block = dict_block(kEntries, 0, kEntries, pfor_of(dictionary, IntTransform::delta),
                                   pfor_of(indices, IntTransform::delta));
    EXPECT_EQ(block.size(), 738U);
    EXPECT_EQ(decoded_block(block), dictionary);
}

TEST(Dictionary, ColumnCountMustMatchExactly) {
    const Values values{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const Bytes column = encoded(values, 4);  // blocks of 4, 4 and 2
    Bytes last;
    ASSERT_TRUE(encode_dict_block(std::span{values}.subspan(8), last).has_value());
    EXPECT_EQ(decoded(column, values.size()), values);
    EXPECT_EQ(decoded(column, 9),
              std::unexpected(error_of(CodecErrorKind::bad_count, "column count",
                                       8 * (column.size() - last.size()), 9, 10)));
    EXPECT_EQ(decoded(column, 11),
              std::unexpected(error_of(CodecErrorKind::truncated, "column values",
                                       8 * column.size(), 11, 10)));
}

TEST(Dictionary, TruncationAtEveryByteLengthIsAnError) {
    for (const Values& values : {golden1(), golden2(), golden3()}) {
        const Bytes block = encoded(values, values.size());
        for (std::size_t length = 0; length < block.size(); ++length) {
            SCOPED_TRACE(length);
            Values out{42};
            const Bytes cut = exact_copy(std::span{block}.first(length));
            EXPECT_FALSE(decode_dict_block(cut, out).has_value());
            EXPECT_EQ(out, Values{42});
        }
        const Bytes column = encoded(values, 7);
        for (std::size_t length = 0; length < column.size(); ++length) {
            SCOPED_TRACE(length);
            EXPECT_FALSE(decoded(std::span{column}.first(length), values.size()).has_value());
        }
    }
}

}  // namespace
}  // namespace tick_store::codec
