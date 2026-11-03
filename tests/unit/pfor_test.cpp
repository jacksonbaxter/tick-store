// Patched frame of reference: golden bytes both ways, every width and exception edge,
// the adaptive scale with no factor built in, one corrupt fixture per check in the
// decoder, truncation at every byte length, and the encoded size pinned. The golden
// blocks were worked out by hand from the format in pfor.hpp and checked against a small
// independent model of it, kept outside the repository with the raw logs.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/lane_pack.hpp"
#include "tick_store/codec/pfor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
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

constexpr PforOptions kDelta{.transform = IntTransform::delta, .adaptive_scale = false, .patch = true};
constexpr PforOptions kScaledFor{.transform = IntTransform::none, .adaptive_scale = true,
                                 .patch = true};
constexpr PforOptions kPatchedFor{.transform = IntTransform::none, .adaptive_scale = false,
                                  .patch = true};
constexpr PforOptions kPlainFor{.transform = IntTransform::none, .adaptive_scale = false,
                                .patch = false};

CodecError error_of(CodecErrorKind kind, std::string_view field, std::uint64_t offset,
                    std::uint64_t expected, std::uint64_t actual) {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

Bytes encoded(const Values& values, const PforOptions& options,
              std::size_t block_values = kDefaultBlockValues, PforStats* stats = nullptr) {
    Bytes out;
    const auto done = encode_pfor(values, block_values, options, out, stats);
    EXPECT_TRUE(done.has_value()) << to_string(done.error());
    return out;
}

std::expected<Values, CodecError> decoded(std::span<const std::byte> column, std::uint64_t count) {
    Values out;
    const Bytes input = exact_copy(column);
    if (const auto done = decode_pfor(input, count, out); !done.has_value()) {
        return std::unexpected(done.error());
    }
    return out;
}

std::expected<Values, CodecError> decoded_block(const Bytes& block) {
    Values out;
    const Bytes input = exact_copy(block);
    const auto taken = decode_pfor_block(input, out);
    if (!taken.has_value()) {
        return std::unexpected(taken.error());
    }
    EXPECT_EQ(*taken, input.size());
    return out;
}

// A block around a payload given byte by byte.
Bytes block_of(std::uint32_t count, const Bytes& payload) {
    Bytes block;
    const std::size_t at = begin_block(block, count);
    patch_block_header(block, at, static_cast<std::uint32_t>(payload.size()));
    block.insert(block.end(), payload.begin(), payload.end());
    return block;
}

Bytes joined(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

Bytes linear(const Values& values, unsigned width) {
    Bytes out;
    pack_linear(values, width, out);
    return out;
}

// Golden 1: delta, four values. y = 100, 5, -2, 0. Zigzag gives 200, 10, 3, 0, base 0,
// all in 8 bits: 72 bits against plain's 136, whose base of -2 takes 8 bytes.
const Values& golden1() {
    static const Values value{100, 105, 103, 103};
    return value;
}
const Bytes& golden1_payload() {
    static const Bytes value = bytes({
    0x01, 0x01, 0x01,        // delta, scale in 1 byte, scale 1
    0x01, 0x08, 0x00,        // zigzag, width 8, base 0 in no bytes
    0xC8, 0x0A, 0x03, 0x00,  // 200, 10, 3, 0
    0x00, 0x00,              // no exceptions
});
    return value;
}

// Golden 2: no transform, sixteen whole-cent prices and one at index 5 with 37 over. Scale
// 100 is the commonest divisor of a run of 8, so it is a candidate, and it wins: q - 496
// fits 4 bits, against 11 at scale 1.
const Values& golden2() {
    static const Values value{50000, 50300, 49700, 51000, 50500, 49937, 50100, 50200,
                      49800, 50400, 50000, 50700, 49600, 50300, 50100, 50000};
    return value;
}
const Bytes& golden2_payload() {
    static const Bytes value = bytes({
    0x00, 0x01, 0x64,                                // none, scale 100
    0x01, 0x00, 0x00, 0x00,                          // one remainder
    0x03,                                            // gaps in 3 bits
    0x05,                                            // the first, at index 5
    0x25,                                            // of 37, in 7 bits
    0x00, 0x04, 0x02, 0xF0, 0x01,                    // plain, width 4, base 496
    0x74, 0xE1, 0x39, 0x65, 0x82, 0xB4, 0x70, 0x45,  // q - 496, a nibble each
    0x00, 0x00,                                      // no exceptions
});
    return value;
}

// Golden 3: delta through a stub quote. Zigzag less a base of 1 gives 1999, 1, 3, 0,
// 1997993, 1997988, 1, 1: width 3, and three exceptions with 18 high bits each.
const Values& golden3() {
    static const Values value{1000, 1001, 1003, 1002, 999999, 1004, 1005, 1006};
    return value;
}
const Bytes& golden3_payload() {
    static const Bytes value = bytes({
    0x01, 0x01, 0x01,                          // delta, scale 1
    0x01, 0x03, 0x01, 0x01,                    // zigzag, width 3, base 1 in 1 byte
    0xCF, 0x10, 0x26,                          // low 3 bits: 7, 1, 3, 0, 1, 4, 1, 1
    0x03, 0x00,                                // three exceptions
    0x12,                                      // of 18 high bits
    0x02, 0x0C,                                // at 0, 4 and 5: gaps 0, 3, 0 in 2 bits
    0xF9, 0x00, 0x54, 0x3E, 0x4F, 0xF9, 0x3C,  // 249, 249749, 249748
});
    return value;
}

TEST(Pfor, GoldenBytesBothWays) {
    EXPECT_EQ(encoded(golden1(), kDelta), block_of(4, golden1_payload()));
    EXPECT_EQ(decoded_block(block_of(4, golden1_payload())), golden1());

    PforStats stats;
    EXPECT_EQ(encoded(golden2(), kScaledFor, kDefaultBlockValues, &stats),
              block_of(16, golden2_payload()));
    EXPECT_EQ(stats.scaled_blocks, 1U);
    EXPECT_EQ(stats.remainders, 1U);
    EXPECT_EQ(decoded_block(block_of(16, golden2_payload())), golden2());

    EXPECT_EQ(encoded(golden3(), kDelta), block_of(8, golden3_payload()));
    EXPECT_EQ(decoded_block(block_of(8, golden3_payload())), golden3());
}

TEST(Pfor, EmptyColumnHasNoBlocks) {
    EXPECT_TRUE(encoded({}, kDelta).empty());
    EXPECT_EQ(decoded({}, 0), Values{});
}

// The bytes a block of `values` takes with no patching and no scale, when every vector's
// smallest value is 0 and its largest is not above 2^63 - 1: its fields, then each
// vector's header, its values packed at the widest one's width, and its exception count.
std::size_t unpatched_size(const Values& values) {
    std::size_t size = kBlockHeaderSize + 3;
    for (std::size_t at = 0; at < values.size(); at += kVectorValues) {
        const std::size_t m = std::min(kVectorValues, values.size() - at);
        const auto vector = std::span{values}.subspan(at, m);
        const auto width = static_cast<unsigned>(std::bit_width(std::ranges::max(vector)));
        size += 3 + (m == kVectorValues ? vector_bytes(width) : linear_bytes(m, width)) + 2;
    }
    return size;
}

// Every width edge, with patching off so the width is the widest value's: 0 and 2^b - 1
// take b bits, and 0 and 2^b take b + 1. Width 64 needs a range of 2^63 or more, which
// 0, 2^63 - 1 and 2^63 give in either mode.
TEST(Pfor, EveryWidthEdgeTakesItsWidthAndRoundTrips) {
    for (unsigned width = 0; width < 64; ++width) {
        for (const std::size_t count : {std::size_t{1023}, kVectorValues, std::size_t{1025}}) {
            SCOPED_TRACE(width);
            SCOPED_TRACE(count);
            const std::uint64_t top = (std::uint64_t{1} << width) - 1U;
            Values values(count);
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = i % 2 == 0 ? 0 : top;
            }
            const Bytes block = encoded(values, kPlainFor, count);
            EXPECT_EQ(block.size(), unpatched_size(values));
            EXPECT_EQ(decoded_block(block), values);

            values[1] = top + 1U;
            const Bytes wider = encoded(values, kPlainFor, count);
            EXPECT_EQ(decoded_block(wider), values);
            if (width < 63) {
                // values[3] still holds `top`, so the widest is width + 1 bits.
                const std::size_t m = std::min(kVectorValues, count);
                EXPECT_EQ(wider.size() - block.size(),
                          m == kVectorValues ? vector_bytes(width + 1) - vector_bytes(width)
                                             : linear_bytes(m, width + 1) - linear_bytes(m, width));
            }
        }
    }
    Values extremes(kVectorValues, 0);
    extremes[1] = kSignBit - 1U;
    extremes[2] = kSignBit;
    const Bytes block = encoded(extremes, kPlainFor, kVectorValues);
    // Zigzag maps these to 0, 2^64 - 2 and 2^64 - 1, so its base is 0 and its width 64.
    EXPECT_EQ(block.size(), kBlockHeaderSize + 3 + 3 + vector_bytes(64) + 2);
    EXPECT_EQ(decoded_block(block), extremes);
}

// One exception of every high width over every width below it, in a full vector: one
// value is 0 and the rest 2^b - 1, so the vector takes width b and patches the one value.
TEST(Pfor, EveryExceptionEdgeIsPatchedAndRoundTrips) {
    for (unsigned width = 0; width < 63; ++width) {
        for (unsigned high = 1; width + high <= 63; ++high) {
            SCOPED_TRACE(width);
            SCOPED_TRACE(high);
            const std::uint64_t top = (std::uint64_t{1} << width) - 1U;
            Values values(kVectorValues, top);
            values[0] = 0;
            values[777] = std::uint64_t{1} << (width + high - 1U);
            PforStats stats;
            const Bytes block = encoded(values, kPatchedFor, kVectorValues, &stats);
            EXPECT_EQ(stats.exceptions, 1U);
            // Header fields, the vector header, the packed values, the exception count,
            // its high width, a gap width, the 10-bit gap of 777 and the high bits.
            EXPECT_EQ(block.size(), kBlockHeaderSize + 3 + 3 + vector_bytes(width) + 2 + 1 + 1 +
                                        2 + ((high + 7) / 8));
            EXPECT_EQ(decoded_block(block), values);
        }
    }
}

TEST(Pfor, ShortVectorsTakeTheLinearLayoutAtEveryLength) {
    test::SplitMix64 random{21};
    for (const unsigned width : {0U, 1U, 7U, 8U, 9U, 33U, 63U}) {
        for (const std::size_t count :
             {std::size_t{1}, std::size_t{2}, std::size_t{15}, std::size_t{16}, std::size_t{17},
              std::size_t{1023}, std::size_t{2047}}) {
            SCOPED_TRACE(width);
            SCOPED_TRACE(count);
            Values values(count);
            const std::uint64_t mask = width == 0 ? 0 : (kMax >> (64U - width));
            for (std::uint64_t& value : values) {
                value = random() & mask;
            }
            for (const PforOptions& options : {kDelta, kScaledFor, kPlainFor}) {
                PforStats stats;
                const Bytes block = encoded(values, options, count, &stats);
                EXPECT_EQ(decoded_block(block), values);
                EXPECT_EQ(stats.tail_values, count % kVectorValues);
                EXPECT_EQ(stats.full_vectors, count / kVectorValues);
            }
        }
    }
}

Values walk(std::uint64_t seed, std::size_t count, std::uint64_t scale) {
    test::SplitMix64 random{seed};
    Values values;
    std::uint64_t price = 5'000;
    for (std::size_t i = 0; i < count; ++i) {
        price = price + (random() % 41U) - 20U;
        values.push_back(price * scale);
    }
    return values;
}

// The scale is found in the data: 7, 50, 100 and 10^4 are all detected, with nothing
// in the code naming any of them.
TEST(Pfor, AdaptiveScaleFindsAFactorThatIsNotBuiltIn) {
    for (const std::uint64_t scale : {7ULL, 50ULL, 100ULL, 10'000ULL}) {
        SCOPED_TRACE(scale);
        const Values values = walk(scale, 4'096, scale);
        PforStats stats;
        const Bytes scaled = encoded(values, kScaledFor, kDefaultBlockValues, &stats);
        EXPECT_EQ(stats.scaled_blocks, 1U);
        EXPECT_EQ(stats.remainders, 0U);
        EXPECT_LT(scaled.size(),
                  encoded(values, PforOptions{.transform = IntTransform::none,
                                              .adaptive_scale = false, .patch = true})
                      .size());
        EXPECT_EQ(decoded(scaled, values.size()), values);
    }
}

// A factor that holds for most values: those off it carry a remainder. One that holds
// for few: the block falls back to scale 1. Both round-trip exactly.
TEST(Pfor, AScaleThatDoesNotHoldEverywhereStillRoundTrips) {
    Values mostly = walk(9, 4'096, 100);
    for (std::size_t i = 0; i < mostly.size(); i += 97) {
        mostly[i] += 1 + (i % 99);
    }
    PforStats stats;
    const Bytes scaled = encoded(mostly, kScaledFor, kDefaultBlockValues, &stats);
    EXPECT_EQ(stats.scaled_blocks, 1U);
    EXPECT_EQ(stats.remainders, (mostly.size() + 96) / 97);
    EXPECT_EQ(decoded(scaled, mostly.size()), mostly);

    const Values rarely = walk(10, 4'096, 1);
    PforStats none;
    const Bytes unscaled = encoded(rarely, kScaledFor, kDefaultBlockValues, &none);
    EXPECT_EQ(none.scaled_blocks, 0U);
    EXPECT_EQ(decoded(unscaled, rarely.size()), rarely);
}

// Scales at the top of the range, where q * s + r sits at 2^64 - 1.
TEST(Pfor, ScalesNearTwoToTheSixtyFourRoundTrip) {
    // Eight multiples of 3 just below 2^64 make 3 a candidate; then 2^64 - 2 carries a
    // remainder of 2 on the largest q, and 2^64 - 1 is exactly q * 3.
    Values threes;
    for (std::uint64_t k = 1; k <= 8; ++k) {
        threes.push_back(kMax - (3 * k));
    }
    threes.push_back(kMax - 1);
    threes.push_back(kMax);
    for (const Values& values :
         {Values{kMax, kMax}, Values{kSignBit, kSignBit, 0}, Values{kMax - 1, kMax - 1, 1},
          threes}) {
        for (const IntTransform transform :
             {IntTransform::none, IntTransform::delta, IntTransform::delta_of_delta}) {
            const PforOptions options{.transform = transform, .adaptive_scale = true, .patch = true};
            EXPECT_EQ(decoded(encoded(values, options), values.size()), values);
        }
    }
}

// Every transform and option on walks with repeats, jumps and 64-bit wraps, at block
// sizes either side of a vector.
TEST(Pfor, SeededWalksRoundTripInEveryVariant) {
    test::SplitMix64 random{48};
    Values values;
    std::uint64_t t = 34'200'000'000'000ULL;
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t pick = random() % 100;
        if (pick < 2) {
            // a repeat
        } else if (pick < 3) {
            t += random();
        } else {
            t += random() % 100'000;
        }
        values.push_back(t);
    }
    values.insert(values.end(), {0, kMax, 0, kMax, kSignBit, kSignBit - 1, 1, kMax - 1});
    for (const IntTransform transform :
         {IntTransform::none, IntTransform::delta, IntTransform::delta_of_delta}) {
        for (const bool scale : {false, true}) {
            for (const bool patch : {false, true}) {
                const PforOptions options{.transform = transform, .adaptive_scale = scale,
                                          .patch = patch};
                for (const std::size_t block_values :
                     {std::size_t{1}, std::size_t{3}, std::size_t{1000}, kVectorValues,
                      std::size_t{1025}, std::size_t{kDefaultBlockValues}}) {
                    SCOPED_TRACE(block_values);
                    const Bytes column = encoded(values, options, block_values);
                    ASSERT_EQ(decoded(column, values.size()), values);
                }
            }
        }
    }
    Values many(kMaxBlockValues);
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i] = values[i % values.size()];
    }
    EXPECT_EQ(decoded(encoded(many, kDelta, kMaxBlockValues), many.size()), many);
}

// A silent change in the format or the encoder's choices changes these sizes (T4).
TEST(Pfor, EncodedSizeIsPinned) {
    EXPECT_EQ(block_of(4, golden1_payload()).size(), 20U);
    EXPECT_EQ(block_of(16, golden2_payload()).size(), 33U);
    EXPECT_EQ(block_of(8, golden3_payload()).size(), 30U);

    // 10,000 timestamps 25 us apart, delta, in blocks of 4,096. Each block's first delta
    // is its first value, a 45-bit exception over a base of 25,000; every other offset is
    // 0, so every vector has width 0. A first vector is 3 header bytes, a 2-byte base, an
    // exception count, a high width, a gap width of 0, no gap bytes, since its one
    // exception is at 0, and 45 high bits: 15 bytes. Any other vector is 7. So
    // 8 + 3 + 15 + 3 * 7 = 47 bytes for each full block, and 8 + 3 + 15 + 7 = 33 for the
    // last, of 1,808 values: 127 in all.
    Values regular;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        regular.push_back(34'200'000'000'000ULL + (i * 25'000));
    }
    EXPECT_EQ(encoded(regular, kDelta).size(), 127U);

    // Gaps uniform below 50 us take 16 bits a value as deltas and 17 as delta-of-deltas,
    // and a walk of whole cents with steps of at most 20 takes 6 bits at scale 100: so
    // 20,000, 21,250 and 7,500 bytes before the fixed costs.
    test::SplitMix64 random{7};
    Values jittered{34'200'000'000'000ULL};
    for (int i = 1; i < 10'000; ++i) {
        jittered.push_back(jittered.back() + (random() % 50'000));
    }
    PforStats stats;
    EXPECT_EQ(encoded(jittered, kDelta, kDefaultBlockValues, &stats).size(), 20'110U);
    EXPECT_EQ(stats.full_vectors, 9U);
    EXPECT_EQ(stats.tail_values, 784U);
    EXPECT_EQ(stats.exceptions, 3U);
    EXPECT_EQ(encoded(jittered, PforOptions{.transform = IntTransform::delta_of_delta,
                                            .adaptive_scale = false, .patch = true})
                  .size(),
              21'250U);
    const Values prices = walk(3, 10'000, 100);
    EXPECT_EQ(encoded(prices, PforOptions{.transform = IntTransform::delta,
                                          .adaptive_scale = true, .patch = true})
                  .size(),
              7'604U);
}

TEST(Pfor, EncoderRefusesBadParameters) {
    Bytes out;
    EXPECT_EQ(encode_pfor(golden1(), 0, kDelta, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(encode_pfor_block({}, kDelta, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(encode_pfor_block(golden1(),
                                // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): an out-of-range transform is what this checks
                                PforOptions{.transform = static_cast<IntTransform>(3),
                                            .adaptive_scale = false, .patch = true},
                                out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "pfor transform", 0, 2, 3)));
    EXPECT_TRUE(out.empty());
}

// One fixture per check in the decoder, each asserting the exact error (T1). Offsets are
// bits from the start of the block; the payload starts at bit 64.
TEST(Pfor, EachHeaderCorruptionHasItsError) {
    EXPECT_EQ(decoded_block(Bytes{}),
              std::unexpected(error_of(CodecErrorKind::truncated, "block header", 0, 64, 0)));
    EXPECT_EQ(decoded_block(block_of(0, golden1_payload())),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1, 0)));
    EXPECT_EQ(decoded_block(block_of(kMaxBlockValues + 1, golden1_payload())),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0,
                                       kMaxBlockValues, kMaxBlockValues + 1)));
    // Fewer bytes than one vector's fixed fields.
    EXPECT_EQ(decoded_block(block_of(1, bytes({0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00}))),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 0, 1)));
    // Three vectors' worth of count in 12 bytes, which hold one vector's fixed fields.
    EXPECT_EQ(decoded_block(block_of(2'049, golden1_payload())),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1'024, 2'049)));
    Bytes cut = block_of(4, golden1_payload());
    cut.pop_back();
    EXPECT_EQ(decoded_block(cut),
              std::unexpected(error_of(CodecErrorKind::truncated, "block payload", 64, 96, 88)));

    const auto with = [](std::size_t at, unsigned value) {
        Bytes payload = golden1_payload();
        payload[at] = static_cast<std::byte>(value);
        return block_of(4, payload);
    };
    EXPECT_EQ(decoded_block(with(0, 3)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor transform", 64, 2, 3)));
    EXPECT_EQ(decoded_block(with(1, 0)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor scale bytes", 72, 8, 0)));
    EXPECT_EQ(decoded_block(with(1, 9)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor scale bytes", 72, 8, 9)));
    // A scale of 0, and a scale of 1 in two bytes.
    EXPECT_EQ(decoded_block(with(2, 0)),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "pfor scale", 80, 0, 1)));
    EXPECT_EQ(decoded_block(block_of(4, joined({bytes({0x01, 0x02, 0x01, 0x00}),
                                                Bytes(golden1_payload().begin() + 3,
                                                      golden1_payload().end())}))),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "pfor scale", 80, 1, 2)));
    // A scale of 8 bytes the payload cannot hold.
    EXPECT_EQ(decoded_block(block_of(1, bytes({0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}))),
              std::unexpected(error_of(CodecErrorKind::truncated, "pfor scale", 80, 64, 48)));
}

TEST(Pfor, EachRemainderCorruptionHasItsError) {
    const auto with = [](std::size_t at, unsigned value) {
        Bytes payload = golden2_payload();
        payload[at] = static_cast<std::byte>(value);
        return block_of(16, payload);
    };
    EXPECT_EQ(decoded_block(with(3, 17)),
              std::unexpected(
                  error_of(CodecErrorKind::bad_count, "pfor remainder count", 88, 16, 17)));
    EXPECT_EQ(decoded_block(with(9, 100)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor remainder", 120, 99, 100)));
    EXPECT_EQ(decoded_block(with(9, 0)),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "pfor remainder", 136, 1, 0)));
    EXPECT_EQ(decoded_block(with(7, 5)),
              std::unexpected(
                  error_of(CodecErrorKind::bad_width, "pfor remainder gap width", 120, 4, 5)));
    EXPECT_EQ(decoded_block(with(7, 4)),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "pfor remainder gap width", 120, 3, 4)));
    // Two remainders, gaps 5 and 15: the second at 21, past the block's 16 values.
    EXPECT_EQ(decoded_block(block_of(16, joined({bytes({0x00, 0x01, 0x64, 0x02, 0x00, 0x00, 0x00,
                                                        0x04, 0xF5, 0xA5, 0x12}),
                                                 Bytes(golden2_payload().begin() + 10,
                                                       golden2_payload().end())}))),
              std::unexpected(
                  error_of(CodecErrorKind::bad_field, "pfor remainder position", 132, 15, 21)));

    // q * s past 2^64 - 1: scale 2^63 and q = 2.
    EXPECT_EQ(decoded_block(block_of(1, bytes({0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                               0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                               0x01, 0x02, 0x00, 0x00}))),
              std::unexpected(
                  error_of(CodecErrorKind::bad_field, "pfor scaled value", 224, 1, 2)));
    // q * s + r past 2^64 - 1: scale 3, q = (2^64 - 1) / 3 and a remainder of 1.
    EXPECT_EQ(decoded_block(block_of(1, bytes({0x00, 0x01, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00,
                                               0x01, 0x00, 0x00, 0x08, 0x55, 0x55, 0x55, 0x55,
                                               0x55, 0x55, 0x55, 0x55, 0x00, 0x00}))),
              std::unexpected(
                  error_of(CodecErrorKind::bad_field, "pfor scaled value", 240, kMax - 1, kMax)));
}

TEST(Pfor, EachVectorCorruptionHasItsError) {
    const auto with = [](std::size_t at, unsigned value) {
        Bytes payload = golden1_payload();
        payload[at] = static_cast<std::byte>(value);
        return block_of(4, payload);
    };
    EXPECT_EQ(decoded_block(with(3, 2)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor mode", 88, 1, 2)));
    EXPECT_EQ(decoded_block(with(4, 65)),
              std::unexpected(error_of(CodecErrorKind::bad_width, "pfor width", 96, 64, 65)));
    EXPECT_EQ(decoded_block(with(5, 9)),
              std::unexpected(error_of(CodecErrorKind::bad_field, "pfor base bytes", 104, 8, 9)));
    // A base of 0 in one byte.
    EXPECT_EQ(decoded_block(block_of(4, joined({bytes({0x01, 0x01, 0x01, 0x01, 0x08, 0x01, 0x00}),
                                                Bytes(golden1_payload().begin() + 6,
                                                      golden1_payload().end())}))),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "pfor base", 112, 0, 1)));
    // A tail cut short, and a tail whose padding is not zero: 4 values of 7 bits leave 4.
    EXPECT_EQ(decoded_block(block_of(4, bytes({0x01, 0x01, 0x01, 0x01, 0x08, 0x00, 0xC8, 0x0A}))),
              std::unexpected(error_of(CodecErrorKind::truncated, "pfor packed tail", 112, 32, 16)));
    EXPECT_EQ(decoded_block(block_of(4, bytes({0x01, 0x01, 0x01, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00,
                                               0xF0, 0x00, 0x00}))),
              std::unexpected(
                  error_of(CodecErrorKind::trailing_bits, "pfor tail padding", 140, 0, 0xF)));
    // A full vector cut short: 1,024 values of 8 bits need 8,192 bits.
    EXPECT_EQ(decoded_block(block_of(1'024, bytes({0x00, 0x01, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00,
                                                   0x00, 0x00}))),
              std::unexpected(
                  error_of(CodecErrorKind::truncated, "pfor packed vector", 112, 8'192, 32)));
    // A second vector whose header the first vector's bytes left no room for.
    Bytes two = bytes({0x00, 0x01, 0x01, 0x00, 0x01, 0x00});
    two.resize(two.size() + vector_bytes(1), std::byte{0});
    two.insert(two.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    EXPECT_EQ(decoded_block(block_of(1'025, two)),
              std::unexpected(error_of(CodecErrorKind::truncated, "pfor base bytes",
                                       64 + (8 * (two.size())), 8, 0)));
    // More exceptions than values, and a spare byte after the last vector.
    EXPECT_EQ(decoded_block(with(10, 5)),
              std::unexpected(
                  error_of(CodecErrorKind::bad_count, "pfor exception count", 144, 4, 5)));
    Bytes spare = golden1_payload();
    spare.push_back(std::byte{0});
    EXPECT_EQ(decoded_block(block_of(4, spare)),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "pfor block", 160, 0, 8)));
}

TEST(Pfor, EachExceptionCorruptionHasItsError) {
    const auto with = [](std::size_t at, unsigned value) {
        Bytes payload = golden3_payload();
        payload[at] = static_cast<std::byte>(value);
        return block_of(8, payload);
    };
    EXPECT_EQ(decoded_block(with(10, 9)),
              std::unexpected(
                  error_of(CodecErrorKind::bad_count, "pfor exception count", 144, 8, 9)));
    EXPECT_EQ(decoded_block(with(12, 0)),
              std::unexpected(error_of(CodecErrorKind::bad_width, "pfor high width", 160, 61, 0)));
    EXPECT_EQ(decoded_block(with(12, 62)),
              std::unexpected(error_of(CodecErrorKind::bad_width, "pfor high width", 160, 61, 62)));
    // The same exceptions in 19 bits, one more than the highest needs.
    const Bytes head(golden3_payload().begin(), golden3_payload().begin() + 12);
    EXPECT_EQ(decoded_block(block_of(8, joined({head, bytes({19, 2}), linear({0, 3, 0}, 2),
                                                linear({249, 249'749, 249'748}, 19)}))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "pfor high width", 160, 18, 19)));
    // A gap width past what 8 positions can need, and one wider than the largest gap.
    EXPECT_EQ(decoded_block(with(13, 4)),
              std::unexpected(
                  error_of(CodecErrorKind::bad_width, "pfor exception gap width", 168, 3, 4)));
    EXPECT_EQ(decoded_block(block_of(8, joined({head, bytes({18, 3}), linear({0, 3, 0}, 3),
                                                linear({249, 249'749, 249'748}, 18)}))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "pfor exception gap width", 168, 2, 3)));
    // Gaps 0, 3 and 5 put the last position at 10, past the vector's 8 values; and a zero
    // high part.
    EXPECT_EQ(decoded_block(block_of(8, joined({head, bytes({18, 3}), linear({0, 3, 5}, 3),
                                                linear({249, 249'749, 249'748}, 18)}))),
              std::unexpected(
                  error_of(CodecErrorKind::bad_field, "pfor exception position", 182, 7, 10)));
    EXPECT_EQ(decoded_block(block_of(8, joined({head, bytes({18, 2}), linear({0, 3, 0}, 2),
                                                linear({249, 0, 249'748}, 18)}))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "pfor exception high bits", 202, 1, 0)));
}

TEST(Pfor, ColumnCountMustMatchExactly) {
    const Values values = walk(4, 10, 1);
    const Bytes column = encoded(values, kDelta, 4);  // blocks of 4, 4 and 2
    Bytes last;
    ASSERT_TRUE(encode_pfor_block(std::span{values}.subspan(8), kDelta, last).has_value());
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

// A whole column cut at every byte length is refused, never read as zeros (D10).
TEST(Pfor, TruncationAtEveryByteLengthIsAnError) {
    Values values = walk(5, 2'100, 100);
    values[3] += 1;
    values[1'500] = 1'999'999'900;
    for (const PforOptions& options : {kDelta, kScaledFor, kPlainFor}) {
        const Bytes column = encoded(values, options, 1'100);
        for (std::size_t length = 0; length < column.size(); ++length) {
            SCOPED_TRACE(length);
            EXPECT_FALSE(decoded(std::span{column}.first(length), values.size()).has_value());
        }
        const Bytes block = encoded(values, options, values.size());
        for (std::size_t length = 0; length < block.size(); ++length) {
            SCOPED_TRACE(length);
            Values out{42};
            const Bytes cut = exact_copy(std::span{block}.first(length));
            EXPECT_FALSE(decode_pfor_block(cut, out).has_value());
            // A refused block leaves what was there before.
            EXPECT_EQ(out, Values{42});
        }
    }
}

}  // namespace
}  // namespace tick_store::codec
