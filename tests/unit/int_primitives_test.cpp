// The packed codecs' primitives: zigzag, delta and delta-of-delta at the edges of 64-bit
// wrap, the interleaved lane layout against a bit-by-bit model at every width from 0 to
// 64, the linear layout, and the patch list's checks, one corrupt fixture each.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/int_transform.hpp"
#include "tick_store/codec/lane_pack.hpp"
#include "tick_store/codec/patch_list.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
constexpr std::uint64_t kSignBit = std::uint64_t{1} << 63U;

CodecError error_of(CodecErrorKind kind, std::string_view field, std::uint64_t offset,
                    std::uint64_t expected, std::uint64_t actual) {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

TEST(Zigzag, MapsSmallMagnitudesToSmallValuesAndBack) {
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 7> cases{{
        {0, 0},
        {kMax, 1},  // -1
        {1, 2},
        {kMax - 1, 3},  // -2
        {(kSignBit - 1), kMax - 1},  // INT64_MAX
        {kSignBit, kMax},  // INT64_MIN
        {kSignBit + 1, kMax - 2},
    }};
    for (const auto& [value, coded] : cases) {
        SCOPED_TRACE(value);
        EXPECT_EQ(zigzag_encode(value), coded);
        EXPECT_EQ(zigzag_decode(coded), value);
    }
}

TEST(Delta, RoundTripsEveryWrapAndTakesTheFirstValueWhole) {
    const Values values{7, kMax, 0, kSignBit, kSignBit - 1, 0, 0, kMax, 1, kMax - 1, 5};
    Values coded(values.size());
    delta_encode(values, coded);
    EXPECT_EQ(coded[0], 7U);
    EXPECT_EQ(coded[1], kMax - 7U);
    EXPECT_EQ(coded[2], 1U);
    delta_decode(coded);
    EXPECT_EQ(coded, values);

    Values dod(values.size());
    delta_of_delta_encode(values, dod);
    EXPECT_EQ(dod[0], 7U);
    EXPECT_EQ(dod[1], kMax - 7U);
    // (0 - kMax) - (kMax - 7), modulo 2^64.
    EXPECT_EQ(dod[2], std::uint64_t{1} - (kMax - 7U));
    delta_of_delta_decode(dod);
    EXPECT_EQ(dod, values);
}

TEST(Delta, HandlesEmptyAndSingleInputs) {
    Values none;
    delta_encode({}, none);
    delta_of_delta_encode({}, none);
    delta_decode(none);
    delta_of_delta_decode(none);
    EXPECT_TRUE(none.empty());
    Values one{kMax};
    Values coded(1);
    delta_of_delta_encode(one, coded);
    EXPECT_EQ(coded, one);
    delta_of_delta_decode(coded);
    EXPECT_EQ(coded, one);
}

// Where each bit of each value lands, written from the layout's description and nothing
// else: value i in lane i % 16, row i / 16, its bit j at bit row * width + j of the lane,
// and the lane's word k at index k * 16 + lane.
std::vector<std::uint64_t> model_pack(std::span<const std::uint64_t> values, unsigned width) {
    std::vector<std::uint64_t> words(kLanes * width, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t lane = i % kLanes;
        const std::size_t row = i / kLanes;
        for (unsigned j = 0; j < width; ++j) {
            if (((values[i] >> j) & 1U) != 0) {
                const std::size_t bit = (row * width) + j;
                words[((bit / 64) * kLanes) + lane] |= std::uint64_t{1} << (bit % 64);
            }
        }
    }
    return words;
}

Values random_values(std::uint64_t seed, std::size_t count) {
    test::SplitMix64 random{seed};
    Values values(count);
    for (std::uint64_t& value : values) {
        value = random();
    }
    return values;
}

TEST(LanePack, MatchesTheBitModelAtEveryWidth) {
    const Values values = random_values(3, kVectorValues);
    for (unsigned width = 0; width <= 64; ++width) {
        SCOPED_TRACE(width);
        std::vector<std::uint64_t> words(vector_words(width), 0xDEAD);
        pack_vector(std::span<const std::uint64_t>{values}.first<kVectorValues>(), width, words);
        EXPECT_EQ(words, model_pack(values, width));
        std::array<std::uint64_t, kVectorValues> back{};
        back.fill(0xBEEF);
        unpack_vector(words, width, back);
        for (std::size_t i = 0; i < kVectorValues; ++i) {
            ASSERT_EQ(back[i], values[i] & low_bits(width)) << i;
        }
    }
}

// Lane l of a vector where value i is i & 15 holds only l, so every one of its words is
// l in each nibble.
TEST(LanePack, GoldenWordsAtWidthFour) {
    Values values(kVectorValues);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = i & 15U;
    }
    std::vector<std::uint64_t> words(vector_words(4));
    pack_vector(std::span<const std::uint64_t>{values}.first<kVectorValues>(), 4, words);
    for (std::size_t k = 0; k < 4; ++k) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            EXPECT_EQ(words[(k * kLanes) + lane], lane * 0x1111'1111'1111'1111ULL);
        }
    }
    Bytes stored;
    append_words_le(std::span{words}.first(2), stored);
    EXPECT_EQ(stored, bytes({0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                             0x11}));
    std::array<std::uint64_t, 2> loaded{};
    load_words_le(stored, loaded);
    EXPECT_EQ(loaded[1], 0x1111'1111'1111'1111ULL);
}

// Width 0 stores nothing and gives zeros; width 64 stores every bit.
TEST(LanePack, WidthsZeroAndSixtyFourAreExplicit) {
    const Values values = random_values(11, kVectorValues);
    std::array<std::uint64_t, kVectorValues> back{};
    back.fill(1);
    unpack_vector({}, 0, back);
    EXPECT_EQ(back, (std::array<std::uint64_t, kVectorValues>{}));
    std::vector<std::uint64_t> words(vector_words(64));
    pack_vector(std::span<const std::uint64_t>{values}.first<kVectorValues>(), 64, words);
    unpack_vector(words, 64, back);
    EXPECT_TRUE(std::ranges::equal(back, values));
    // Each word of a lane at width 64 is one value: row k of lane l.
    EXPECT_EQ(words[(5 * kLanes) + 3], values[(5 * kLanes) + 3]);
}

TEST(LinearPack, GoldenBytes) {
    Bytes out;
    pack_linear(Values{1, 2, 3}, 2, out);
    EXPECT_EQ(out, bytes({0x39}));  // 01, 10, 11 from the least significant bit
    out.clear();
    pack_linear(Values{0x1FF, 0x3}, 9, out);
    EXPECT_EQ(out, bytes({0xFF, 0x07, 0x00}));
    out.clear();
    pack_linear(Values{kMax, 0x0102030405060708ULL}, 64, out);
    EXPECT_EQ(out, bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x08, 0x07, 0x06, 0x05,
                          0x04, 0x03, 0x02, 0x01}));
    out.clear();
    pack_linear(Values{kMax, kMax}, 0, out);
    EXPECT_TRUE(out.empty());
}

TEST(LinearPack, RoundTripsEveryWidthAndCount) {
    const Values source = random_values(5, 70);
    for (unsigned width = 0; width <= 64; ++width) {
        for (std::size_t count = 0; count <= source.size(); count += 7) {
            SCOPED_TRACE(width);
            SCOPED_TRACE(count);
            Bytes packed;
            pack_linear(std::span{source}.first(count), width, packed);
            ASSERT_EQ(packed.size(), linear_bytes(count, width));
            Values back(count, 0xDEAD);
            EXPECT_TRUE(unpack_linear(exact_copy(packed), width, back));
            for (std::size_t i = 0; i < count; ++i) {
                ASSERT_EQ(back[i], source[i] & low_bits(width)) << i;
            }
        }
    }
}

TEST(LinearPack, RefusesPaddingThatIsNotZeroAndAWrongSize) {
    Values back(3);
    EXPECT_TRUE(unpack_linear(bytes({0x39}), 2, back));
    EXPECT_FALSE(unpack_linear(bytes({0x79}), 2, back));  // bit 6 is padding
    EXPECT_FALSE(unpack_linear(bytes({0x39, 0x00}), 2, back));
    EXPECT_FALSE(unpack_linear(bytes({0x00}), 0, back));
    EXPECT_FALSE(unpack_linear({}, 65, back));
}

constexpr PatchFields kFields{.positions = "test position", .values = "test value"};

std::expected<Patches, CodecError> read_list(const Bytes& list, std::size_t count,
                                             std::size_t limit, unsigned value_bits) {
    const Bytes input = exact_copy(list);
    ByteCursor cursor{input, 80};
    Patches patches;
    if (const auto read = read_patches(cursor, count, limit, value_bits, kFields, patches);
        !read.has_value()) {
        return std::unexpected(read.error());
    }
    EXPECT_EQ(cursor.remaining(), 0U);
    return patches;
}

TEST(PatchList, GoldenBytesBothWays) {
    // Positions 1, 2 and 6 of 8 in 3 bits; values 5, 1 and 9 in 4 bits.
    const Patches patches{.positions = {1, 2, 6}, .values = {5, 1, 9}};
    Bytes out;
    append_patches(patches, 8, 4, out);
    EXPECT_EQ(out, bytes({0x91, 0x01, 0x15, 0x09}));
    EXPECT_EQ(out.size(), patch_list_bytes(3, 8, 4));
    const auto back = read_list(out, 3, 8, 4);
    ASSERT_TRUE(back.has_value()) << to_string(back.error());
    EXPECT_EQ(back->positions, patches.positions);
    EXPECT_EQ(back->values, patches.values);
    // A list over one value spends no bits on its position.
    EXPECT_EQ(patch_position_bits(1), 0U);
    EXPECT_EQ(patch_position_bits(2), 1U);
    EXPECT_EQ(patch_position_bits(1024), 10U);
    EXPECT_EQ(patch_position_bits(1025), 11U);
}

TEST(PatchList, EachCorruptionHasItsError) {
    // Out of order: positions 2 then 1. Bits of the positions start at 80.
    EXPECT_EQ(read_list(bytes({0x0A, 0x11}), 2, 8, 4),
              std::unexpected(error_of(CodecErrorKind::bad_field, "test position", 83, 3, 1)));
    // A repeat is out of order too.
    EXPECT_EQ(read_list(bytes({0x12, 0x11}), 2, 8, 4),
              std::unexpected(error_of(CodecErrorKind::bad_field, "test position", 83, 3, 2)));
    // A position past the limit of 6, in 3 bits.
    EXPECT_EQ(read_list(bytes({0x06, 0x01}), 1, 6, 4),
              std::unexpected(error_of(CodecErrorKind::bad_field, "test position", 80, 5, 6)));
    // A zero value.
    EXPECT_EQ(read_list(bytes({0x01, 0x00}), 1, 8, 4),
              std::unexpected(error_of(CodecErrorKind::non_canonical, "test value", 88, 1, 0)));
    // Padding that is not zero, in each part.
    EXPECT_EQ(read_list(bytes({0x81, 0x01}), 1, 8, 4),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "test position", 80, 0, 1)));
    EXPECT_EQ(read_list(bytes({0x01, 0x21}), 1, 8, 4),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "test value", 88, 0, 1)));
    // Cut short, in each part.
    EXPECT_EQ(read_list(bytes({}), 1, 8, 4),
              std::unexpected(error_of(CodecErrorKind::truncated, "test position", 80, 8, 0)));
    EXPECT_EQ(read_list(bytes({0x01}), 1, 8, 4),
              std::unexpected(error_of(CodecErrorKind::truncated, "test value", 88, 8, 0)));
    // A value width the list cannot have.
    EXPECT_EQ(read_list(bytes({0x01, 0x01}), 1, 8, 0),
              std::unexpected(error_of(CodecErrorKind::bad_width, "test value", 80, 64, 0)));
    EXPECT_EQ(read_list(bytes({0x01, 0x01}), 1, 8, 65),
              std::unexpected(error_of(CodecErrorKind::bad_width, "test value", 80, 64, 65)));
}

TEST(ByteCursor, ReadsLittleEndianAndRefusesPastTheEnd) {
    const Bytes input = exact_copy(bytes({0x01, 0x02, 0x03}));
    ByteCursor cursor{input, 64};
    EXPECT_EQ(cursor.read_le(0, "none"), 0U);
    EXPECT_EQ(cursor.read_le(2, "two"), 0x0201U);
    EXPECT_EQ(cursor.read_le(2, "two more"),
              std::unexpected(error_of(CodecErrorKind::truncated, "two more", 80, 16, 8)));
    EXPECT_EQ(cursor.read_le(9, "nine"),
              std::unexpected(error_of(CodecErrorKind::bad_width, "nine", 80, 64, 72)));
    EXPECT_EQ(cursor.remaining(), 1U);
    EXPECT_EQ(bytes_for(0), 0U);
    EXPECT_EQ(bytes_for(255), 1U);
    EXPECT_EQ(bytes_for(256), 2U);
    EXPECT_EQ(bytes_for(kMax), 8U);
}

}  // namespace
}  // namespace tick_store::codec
