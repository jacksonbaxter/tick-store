// The bit writer and reader under every width from 0 to 64 at every start offset,
// golden bytes both ways, each refusal with its exact error, and the block header.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::codec {
namespace {

using itch::test::bytes;
using itch::test::exact_copy;
using Bytes = std::vector<std::byte>;

constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

CodecError error_of(CodecErrorKind kind, std::string_view field, std::uint64_t offset,
                    std::uint64_t expected, std::uint64_t actual) {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

TEST(BitStream, LowBitsCoversZeroAndSixtyFour) {
    EXPECT_EQ(low_bits(0), 0U);
    EXPECT_EQ(low_bits(1), 1U);
    EXPECT_EQ(low_bits(63), kAllOnes >> 1U);
    EXPECT_EQ(low_bits(64), kAllOnes);
}

// Typed by hand: 101, 11111111, 0, then 64 bits of 0x0123456789ABCDEF, 1, padding.
TEST(BitStream, GoldenBytesBothWays) {
    const Bytes golden = bytes({0xBF, 0xE0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF8});
    Bytes out;
    BitWriter writer{out};
    writer.write(0b101, 3);
    writer.write(0xFF, 8);
    writer.write_bit(false);
    writer.write(0x0123456789ABCDEFULL, 64);
    writer.write_bit(true);
    EXPECT_EQ(writer.bit_count(), 77U);
    const auto written = writer.finish();
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 10U);
    EXPECT_EQ(out, golden);

    const Bytes input = exact_copy(golden);
    BitReader reader{input};
    EXPECT_EQ(reader.read(3, "a"), 0b101U);
    EXPECT_EQ(reader.read(8, "b"), 0xFFU);
    EXPECT_EQ(reader.read_bit("c"), false);
    EXPECT_EQ(reader.read(64, "d"), 0x0123456789ABCDEFULL);
    EXPECT_EQ(reader.read_bit("e"), true);
    EXPECT_EQ(reader.position(), 77U);
    EXPECT_EQ(reader.remaining(), 3U);
    EXPECT_TRUE(reader.finish("end").has_value());
}

TEST(BitStream, WriterAppendsAfterBytesAlreadyThere) {
    Bytes out = bytes({0xAA});
    BitWriter writer{out};
    writer.write(0b1, 1);
    ASSERT_EQ(writer.finish(), 1U);
    EXPECT_EQ(out, bytes({0xAA, 0x80}));
}

TEST(BitStream, WriterIgnoresBitsAboveTheWidth) {
    Bytes out;
    BitWriter writer{out};
    writer.write(kAllOnes, 4);
    writer.write(kAllOnes << 4U, 4);
    ASSERT_EQ(writer.finish(), 1U);
    EXPECT_EQ(out, bytes({0xF0}));
}

TEST(BitStream, WidthZeroWritesAndReadsNothing) {
    Bytes out;
    BitWriter writer{out};
    writer.write(kAllOnes, 0);
    EXPECT_EQ(writer.bit_count(), 0U);
    ASSERT_EQ(writer.finish(), 0U);
    EXPECT_TRUE(out.empty());

    const Bytes empty;
    BitReader reader{empty};
    EXPECT_EQ(reader.read(0, "nothing"), 0U);
    EXPECT_EQ(reader.position(), 0U);
    EXPECT_TRUE(reader.finish("end").has_value());
}

TEST(BitStream, WidthSixtyFourAtEveryOffset) {
    for (unsigned offset = 0; offset < 16; ++offset) {
        SCOPED_TRACE(offset);
        Bytes out;
        BitWriter writer{out};
        writer.write(0, offset);
        writer.write(0x8000000000000001ULL, 64);
        writer.write(kAllOnes, 64);
        ASSERT_TRUE(writer.finish().has_value());
        const Bytes input = exact_copy(out);
        BitReader reader{input};
        EXPECT_EQ(reader.read(offset, "lead"), 0U);
        EXPECT_EQ(reader.read(64, "first"), 0x8000000000000001ULL);
        EXPECT_EQ(reader.read(64, "second"), kAllOnes);
        EXPECT_TRUE(reader.finish("end").has_value());
    }
}

// Every width at every start offset, on patterns and seeded random values, read back
// from a buffer of exactly the written size so ASan sees any read past it. The streams
// end close to a value, so the byte-at-a-time path near the end runs for every case.
TEST(BitStream, EveryWidthAtEveryOffsetRoundTrips) {
    test::SplitMix64 random{20261103};
    for (unsigned width = 0; width <= 64; ++width) {
        for (unsigned offset = 0; offset < 8; ++offset) {
            SCOPED_TRACE(testing::Message() << "width " << width << " offset " << offset);
            std::vector<std::uint64_t> values{0, kAllOnes, 0xAAAAAAAAAAAAAAAAULL,
                                              0x5555555555555555ULL};
            for (int i = 0; i < 8; ++i) {
                values.push_back(random());
            }
            Bytes out;
            BitWriter writer{out};
            writer.write(kAllOnes, offset);
            for (const std::uint64_t value : values) {
                writer.write(value, width);
            }
            ASSERT_TRUE(writer.finish().has_value());
            const Bytes input = exact_copy(out);
            BitReader reader{input};
            ASSERT_EQ(reader.read(offset, "lead"), low_bits(offset));
            for (const std::uint64_t value : values) {
                ASSERT_EQ(reader.read(width, "value"), value & low_bits(width));
            }
            EXPECT_TRUE(reader.finish("end").has_value());
        }
    }
}

// peek_within is read() without the error or the move, for every width it accepts at
// every bit position of a buffer of every length up to 17 bytes, so both its whole-word
// and its near-the-end paths meet every offset; outside its bounds it returns 0 and
// advance_within moves nothing.
TEST(BitStream, PeekWithinMatchesReadEverywhereAndRefusesOutsideItsBounds) {
    test::SplitMix64 random{20261104};
    for (std::size_t size = 0; size <= 17; ++size) {
        Bytes raw(size);
        for (std::byte& b : raw) {
            b = static_cast<std::byte>(random() & 0xFFU);
        }
        const Bytes input = exact_copy(raw);
        const std::uint64_t bits = std::uint64_t{size} * 8U;
        for (std::uint64_t at = 0; at <= bits; ++at) {
            SCOPED_TRACE(testing::Message() << "size " << size << " at " << at);
            BitReader reader{input};
            reader.advance_within(static_cast<unsigned>(at));
            ASSERT_EQ(reader.position(), at);
            for (unsigned width = 0; width <= 64; ++width) {
                const std::uint64_t peeked = reader.peek_within(width);
                if (width == 0 || width > BitReader::kMaxPeekBits || width > bits - at) {
                    EXPECT_EQ(peeked, 0U) << width;
                    continue;
                }
                BitReader copy = reader;
                EXPECT_EQ(copy.read(width, "value"), peeked) << width;
                EXPECT_EQ(reader.position(), at);
            }
            BitReader past = reader;
            past.advance_within(static_cast<unsigned>(bits - at + 1U));
            EXPECT_EQ(past.position(), at);
        }
    }
}

TEST(BitStream, WidthAboveSixtyFourIsRefused) {
    const Bytes input(16, std::byte{0xFF});
    BitReader reader{input};
    EXPECT_EQ(reader.read(65, "wide"), std::unexpected(error_of(CodecErrorKind::bad_width,
                                                                "wide", 0, 64, 65)));
    EXPECT_EQ(reader.position(), 0U);

    Bytes out;
    BitWriter writer{out};
    writer.write(1, 3);
    writer.write(1, 65);
    writer.write(1, 200);
    EXPECT_EQ(writer.finish(), std::unexpected(error_of(CodecErrorKind::bad_width, "bit write",
                                                        3, 64, 65)));
}

TEST(BitStream, ReadingPastTheEndIsTruncatedNeverZeros) {
    const Bytes input = exact_copy(bytes({0xFF, 0x00}));
    BitReader reader{input, 64};
    ASSERT_EQ(reader.read(10, "first"), 0b1111111100U);
    EXPECT_EQ(reader.read(7, "second"),
              std::unexpected(error_of(CodecErrorKind::truncated, "second", 74, 7, 6)));
    // A refused read moves nothing, so the bits that are there can still be read.
    EXPECT_EQ(reader.read(6, "third"), 0U);
    EXPECT_EQ(reader.read_bit("fourth"),
              std::unexpected(error_of(CodecErrorKind::truncated, "fourth", 80, 1, 0)));
}

TEST(BitStream, FinishRefusesSetPaddingAndSpareBytes) {
    const Bytes padded = exact_copy(bytes({0xA1}));
    BitReader set_padding{padded};
    ASSERT_EQ(set_padding.read(4, "value"), 0xAU);
    EXPECT_EQ(set_padding.finish("padding"),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "padding", 4, 0, 1)));

    const Bytes spare = exact_copy(bytes({0xA0, 0x00}));
    BitReader spare_byte{spare, 8};
    ASSERT_EQ(spare_byte.read(4, "value"), 0xAU);
    EXPECT_EQ(spare_byte.finish("padding"),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "padding", 12, 0, 12)));

    const Bytes exact = exact_copy(bytes({0xA0}));
    BitReader zero_padding{exact};
    ASSERT_EQ(zero_padding.read(4, "value"), 0xAU);
    EXPECT_TRUE(zero_padding.finish("padding").has_value());
}

Bytes header(std::uint32_t count, std::uint32_t payload) {
    Bytes out;
    const std::size_t at = begin_block(out, count);
    patch_block_header(out, at, payload);
    return out;
}

TEST(BlockHeader, PinnedAsLittleEndianBytes) {
    EXPECT_EQ(header(0x01020304U, 0x0A0B0C0DU),
              bytes({0x04, 0x03, 0x02, 0x01, 0x0D, 0x0C, 0x0B, 0x0A}));
}

TEST(BlockHeader, ReadsAWholeHeaderAndPayload) {
    Bytes block = header(3, 9);
    block.resize(block.size() + 9);
    const auto read = read_block_header(exact_copy(block), 64, 1);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, (BlockHeader{.count = 3, .payload_bytes = 9}));
}

TEST(BlockHeader, EachCheckHasItsError) {
    const Bytes whole = header(2, 9);
    for (std::size_t length = 0; length < kBlockHeaderSize; ++length) {
        SCOPED_TRACE(length);
        EXPECT_EQ(read_block_header(exact_copy(std::span{whole}.first(length)), 64, 1),
                  std::unexpected(
                      error_of(CodecErrorKind::truncated, "block header", 0, 64, length * 8)));
    }
    EXPECT_EQ(read_block_header(exact_copy(header(0, 0)), 64, 1),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1, 0)));
    EXPECT_EQ(read_block_header(exact_copy(header(kMaxBlockValues + 1, 0)), 64, 1),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0,
                                       kMaxBlockValues, kMaxBlockValues + 1)));
    // A 9-byte payload holds the 64-bit first value and at most 8 one-bit values.
    EXPECT_EQ(read_block_header(exact_copy(header(10, 9)), 64, 1),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 9, 10)));
    EXPECT_EQ(read_block_header(exact_copy(header(1, 7)), 64, 1),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 0, 1)));
    // The largest count, claimed by a block with no room for it, allocates nothing.
    EXPECT_EQ(read_block_header(exact_copy(header(kMaxBlockValues, 8)), 64, 1),
              std::unexpected(
                  error_of(CodecErrorKind::bad_count, "block count", 0, 1, kMaxBlockValues)));
    Bytes short_payload = header(2, 9);
    short_payload.resize(short_payload.size() + 8);
    EXPECT_EQ(read_block_header(exact_copy(short_payload), 64, 1),
              std::unexpected(error_of(CodecErrorKind::truncated, "block payload", 64, 72, 64)));
}

TEST(BlockHeader, EncoderBlockSizeIsChecked) {
    EXPECT_TRUE(check_block_values(1).has_value());
    EXPECT_TRUE(check_block_values(kMaxBlockValues).has_value());
    EXPECT_EQ(check_block_values(0),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(check_block_values(std::size_t{kMaxBlockValues} + 1),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, kMaxBlockValues + 1)));
}

TEST(CodecError, NamesFieldOffsetAndBothValues) {
    EXPECT_EQ(to_string(error_of(CodecErrorKind::non_canonical, "delta-of-delta bucket", 77, 16,
                                 23)),
              "non-canonical encoding: delta-of-delta bucket at bit 77: expected 16, have 23");
    for (const auto kind :
         {CodecErrorKind::truncated, CodecErrorKind::bad_width, CodecErrorKind::bad_count,
          CodecErrorKind::trailing_bits, CodecErrorKind::non_canonical,
          CodecErrorKind::bad_window, CodecErrorKind::bad_parameter,
          CodecErrorKind::not_exact_price}) {
        EXPECT_FALSE(to_string(error_of(kind, "f", 0, 0, 0)).starts_with("unknown"));
    }
}

}  // namespace
}  // namespace tick_store::codec
