#include "tick_store/itch/big_endian.hpp"

#include "test_bytes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace tick_store::itch {
namespace {

constexpr std::array<std::byte, 8> kSequence{std::byte{0x01}, std::byte{0x23}, std::byte{0x45},
                                             std::byte{0x67}, std::byte{0x89}, std::byte{0xAB},
                                             std::byte{0xCD}, std::byte{0xEF}};

// The reader is constexpr, so these hold at compile time as well.
static_assert(BigEndianReader{kSequence}.read_uint<1>(0) == 0x01U);
static_assert(BigEndianReader{kSequence}.read_uint<2>(0) == 0x0123U);
static_assert(BigEndianReader{kSequence}.read_uint<4>(4) == 0x89ABCDEFU);
static_assert(BigEndianReader{kSequence}.read_uint<6>(1) == 0x23456789ABCDULL);
static_assert(BigEndianReader{kSequence}.read_uint<8>(0) == 0x0123456789ABCDEFULL);
static_assert(!BigEndianReader{kSequence}.read_uint<8>(1).has_value());

TEST(BigEndianReader, ReadsEveryWidthAtEveryOffsetAndRejectsTheRest) {
    // Mirrors the loop body at run time, so all widths share one expectation.
    const auto check = []<std::size_t Width>(std::integral_constant<std::size_t, Width>) {
        const BigEndianReader reader{kSequence};
        for (std::size_t offset = 0; offset <= kSequence.size() + 1; ++offset) {
            const auto value = reader.read_uint<Width>(offset);
            if (offset + Width > kSequence.size()) {
                EXPECT_FALSE(value.has_value()) << "width " << Width << " offset " << offset;
                continue;
            }
            std::uint64_t expected = 0;
            for (std::size_t i = 0; i < Width; ++i) {
                expected = (expected << 8U) | std::to_integer<std::uint64_t>(kSequence[offset + i]);
            }
            ASSERT_TRUE(value.has_value()) << "width " << Width << " offset " << offset;
            EXPECT_EQ(*value, expected) << "width " << Width << " offset " << offset;
        }
    };
    check(std::integral_constant<std::size_t, 1>{});
    check(std::integral_constant<std::size_t, 2>{});
    check(std::integral_constant<std::size_t, 3>{});
    check(std::integral_constant<std::size_t, 4>{});
    check(std::integral_constant<std::size_t, 5>{});
    check(std::integral_constant<std::size_t, 6>{});
    check(std::integral_constant<std::size_t, 7>{});
    check(std::integral_constant<std::size_t, 8>{});
}

TEST(BigEndianReader, AnEmptySpanFailsEveryRead) {
    const BigEndianReader reader{std::span<const std::byte>{}};
    EXPECT_EQ(reader.size(), 0U);
    EXPECT_FALSE(reader.read_uint<1>(0).has_value());
    EXPECT_FALSE(reader.read_byte(0).has_value());
    EXPECT_FALSE((reader.read_bytes<1>(0).has_value()));
}

TEST(BigEndianReader, AnOffsetNearSizeMaxCannotWrapPastTheCheck) {
    constexpr std::size_t max = std::numeric_limits<std::size_t>::max();
    const BigEndianReader reader{kSequence};
    EXPECT_FALSE(reader.read_uint<8>(max).has_value());
    EXPECT_FALSE(reader.read_uint<8>(max - 7).has_value());
    EXPECT_FALSE(reader.read_byte(max).has_value());
    EXPECT_FALSE((reader.read_bytes<8>(max - 3).has_value()));
}

TEST(BigEndianReader, WorksOnUnalignedBytes) {
    // Every start offset of an exact heap block, so ASan and the UBSan alignment
    // check both see each one.
    const test::Bytes block = test::exact_copy(kSequence);
    const std::span<const std::byte> bytes{block};
    for (std::size_t start = 0; start + 2 <= bytes.size(); ++start) {
        const BigEndianReader reader{bytes.subspan(start)};
        const auto value = reader.read_uint<2>(0);
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, (std::to_integer<std::uint64_t>(bytes[start]) << 8U) |
                              std::to_integer<std::uint64_t>(bytes[start + 1]));
    }
}

TEST(BigEndianReader, CopiesBytesUnchanged) {
    const BigEndianReader reader{kSequence};
    EXPECT_EQ(reader.read_byte(7), std::byte{0xEF});
    const auto middle = reader.read_bytes<3>(2);
    ASSERT_TRUE(middle.has_value());
    const std::array<std::byte, 3> expected{std::byte{0x45}, std::byte{0x67}, std::byte{0x89}};
    EXPECT_EQ(*middle, expected);
}

TEST(BigEndianWriter, WritesMostSignificantByteFirst) {
    std::array<std::byte, 8> out{};
    BigEndianWriter writer{out};
    ASSERT_TRUE(writer.write_uint<8>(0, 0x0123456789ABCDEFULL));
    EXPECT_EQ(out, kSequence);

    std::array<std::byte, 8> part{};
    BigEndianWriter part_writer{part};
    ASSERT_TRUE(part_writer.write_uint<6>(1, 0x23456789ABCDULL));
    const std::array<std::byte, 8> expected{std::byte{0x00}, std::byte{0x23}, std::byte{0x45},
                                            std::byte{0x67}, std::byte{0x89}, std::byte{0xAB},
                                            std::byte{0xCD}, std::byte{0x00}};
    EXPECT_EQ(part, expected);
}

TEST(BigEndianWriter, AWriteThatDoesNotFitChangesNothing) {
    std::array<std::byte, 4> out{};
    out.fill(std::byte{0x5A});
    const auto before = out;
    BigEndianWriter writer{out};

    EXPECT_FALSE(writer.write_uint<8>(0, 1));
    EXPECT_FALSE(writer.write_uint<2>(3, 1));
    EXPECT_FALSE(writer.write_byte(4, std::byte{1}));
    EXPECT_FALSE(writer.write_bytes(2, kSequence));
    EXPECT_FALSE(writer.write_uint<1>(std::numeric_limits<std::size_t>::max(), 1));
    EXPECT_EQ(out, before);
}

TEST(BigEndianWriter, RoundTripsThroughTheReaderAtEveryWidthEdge) {
    const auto check = []<std::size_t Width>(std::integral_constant<std::size_t, Width>) {
        const std::uint64_t top = Width == 8 ? std::numeric_limits<std::uint64_t>::max()
                                             : (std::uint64_t{1} << (8U * Width)) - 1U;
        for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, top - 1U, top}) {
            std::array<std::byte, 8> out{};
            BigEndianWriter writer{out};
            ASSERT_TRUE(writer.write_uint<Width>(0, value));
            EXPECT_EQ(BigEndianReader{out}.read_uint<Width>(0), value) << "width " << Width;
        }
    };
    check(std::integral_constant<std::size_t, 2>{});
    check(std::integral_constant<std::size_t, 4>{});
    check(std::integral_constant<std::size_t, 6>{});
    check(std::integral_constant<std::size_t, 8>{});
}

}  // namespace
}  // namespace tick_store::itch
