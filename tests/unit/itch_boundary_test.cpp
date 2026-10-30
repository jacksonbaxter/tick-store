// T3: boundary values for each field width, and the property a round trip rests
// on, that every byte of every type reaches a field and comes back unchanged.

#include "itch_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>

namespace tick_store::itch {
namespace {

using test::Bytes;
using test::exact_copy;
using test::fixture;
using test::fixtures;

// Writes `value` big-endian into `width` bytes at `offset`. Done by hand here,
// so the test does not lean on the writer it is checking.
void poke(Bytes& bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) {
        const std::size_t shift = 8U * (width - 1U - i);
        bytes[offset + i] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

// Decodes strictly, encodes, and requires the same bytes back.
Message round_trip(const Bytes& input) {
    const auto decoded = decode_strict(input);
    EXPECT_TRUE(decoded.has_value());
    if (!decoded.has_value()) {
        return Message{};
    }
    Bytes out(input.size());
    EXPECT_EQ(encode(*decoded, out).value_or(0), input.size());
    EXPECT_EQ(out, input);
    return *decoded;
}

TEST(ItchBoundary, EveryFieldOfEveryTypeHoldsItsExtremes) {
    for (const auto& f : fixtures()) {
        for (const auto& field : f.fields) {
            if (field.name == "message_type") {
                continue;  // Changing it changes the type, covered in the raw path tests.
            }
            SCOPED_TRACE(std::format("{}.{}", f.label, field.name));
            const std::uint64_t top = field.width >= 8
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : (std::uint64_t{1} << (8U * field.width)) - 1U;
            const std::uint64_t high_bit = std::uint64_t{1} << ((8U * field.width) - 1U);
            for (const std::uint64_t value :
                 {std::uint64_t{0}, std::uint64_t{1}, high_bit - 1U, high_bit, top - 1U, top}) {
                Bytes bytes = f.bytes;
                poke(bytes, field.offset, field.width, value);
                round_trip(exact_copy(bytes));
            }
        }
    }
}

TEST(ItchBoundary, AllZerosAndAllOnesRoundTripForEveryType) {
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        for (const std::byte fill : {std::byte{0x00}, std::byte{0xFF}}) {
            Bytes bytes(f.length, fill);
            bytes.front() = static_cast<std::byte>(f.type);
            round_trip(exact_copy(bytes));
        }
    }
}

TEST(ItchBoundary, FlippingAnyBitChangesTheMessageAndSurvivesTheRoundTrip) {
    // If any byte were skipped or zeroed, as with the undecoded directory bytes
    // that broke an earlier project's first round trip, a flip there would leave
    // the message equal or the bytes different. Byte 0 is the type, so it is left
    // to the raw path tests.
    std::size_t flips = 0;
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        const Message original = round_trip(f.bytes);
        for (std::size_t offset = 1; offset < f.length; ++offset) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                Bytes bytes = f.bytes;
                bytes[offset] ^= static_cast<std::byte>(1U << bit);
                const Message changed = round_trip(bytes);
                EXPECT_NE(changed, original) << "offset " << offset << " bit " << bit;
                ++flips;
            }
        }
    }
    EXPECT_EQ(flips, (694U - 23U) * 8U);
}

TEST(ItchBoundary, LocateAndTrackingAreUnsigned16Bit) {
    Bytes bytes = fixture('D').bytes;
    poke(bytes, 1, 2, 0xFFFF);
    poke(bytes, 3, 2, 0x8000);
    const auto m = std::get<OrderDelete>(round_trip(bytes));
    EXPECT_EQ(m.header.stock_locate, 0xFFFFU);
    EXPECT_EQ(m.header.tracking_number, 0x8000U);
}

TEST(ItchBoundary, TimestampHolds48BitsWithoutARangeCheck) {
    constexpr std::uint64_t kLastNanosecondOfDay = 86'400'000'000'000ULL - 1U;
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, kLastNanosecondOfDay,
                                      kLastNanosecondOfDay + 1U, std::uint64_t{1} << 47U,
                                      kMaxTimestampNs - 1U, kMaxTimestampNs}) {
        SCOPED_TRACE(value);
        Bytes bytes = fixture('S').bytes;
        poke(bytes, 5, 6, value);
        EXPECT_EQ(std::get<SystemEvent>(round_trip(bytes)).header.timestamp_ns, value);
    }
}

TEST(ItchBoundary, FourByteIntegersAreUnsigned) {
    Bytes bytes = fixture('X').bytes;
    for (const std::uint32_t value : {0U, 1U, 0x7FFF'FFFFU, 0x8000'0000U, 0xFFFF'FFFFU}) {
        poke(bytes, 19, 4, value);
        EXPECT_EQ(std::get<OrderCancel>(round_trip(bytes)).cancelled_shares, value);
    }
}

TEST(ItchBoundary, EightByteIntegersAreUnsigned) {
    Bytes bytes = fixture('Q').bytes;
    for (const std::uint64_t value :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xFFFF'FFFFU},
          std::uint64_t{0x1'0000'0000U}, std::uint64_t{1} << 63U,
          std::numeric_limits<std::uint64_t>::max()}) {
        poke(bytes, 11, 8, value);
        EXPECT_EQ(std::get<CrossTrade>(round_trip(bytes)).shares, value);
    }
}

TEST(ItchBoundary, Price4HoldsTheSpecificationMaximumAndAnythingAboveIt) {
    Bytes bytes = fixture('A').bytes;
    // 200,000.0000, the documented maximum, is 0x77359400.
    for (const std::uint32_t value : {0U, 1U, 0x7735'9400U, 0x7735'9401U, 0xFFFF'FFFFU}) {
        poke(bytes, 32, 4, value);
        EXPECT_EQ(std::get<AddOrder>(round_trip(bytes)).price, Price4{value});
    }
}

TEST(ItchBoundary, Price8HoldsItsFullWidth) {
    Bytes bytes = fixture('V').bytes;
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 63U,
                                      std::numeric_limits<std::uint64_t>::max()}) {
        poke(bytes, 27, 8, value);
        EXPECT_EQ(std::get<MwcbDeclineLevel>(round_trip(bytes)).level_3, Price8{value});
    }
}

TEST(ItchBoundary, BytesAboveAsciiInAlphaFieldsSurvive) {
    // Nothing assumes Alpha is ASCII or that char is signed or unsigned.
    Bytes bytes = fixture('H').bytes;
    bytes[20] = std::byte{0xFF};
    bytes[21] = std::byte{0x80};
    bytes[24] = std::byte{0x00};
    const auto m = std::get<StockTradingAction>(round_trip(bytes));
    EXPECT_EQ(m.reserved, std::byte{0xFF});
    EXPECT_EQ(m.reason[0], std::byte{0x80});
    EXPECT_EQ(m.reason[3], std::byte{0x00});
}

TEST(ItchEncode, RefusesEveryBufferShorterThanTheMessage) {
    for (const auto& f : fixtures()) {
        const auto message = decode_strict(f.bytes);
        ASSERT_TRUE(message.has_value());
        for (std::size_t size = 0; size < f.length; ++size) {
            SCOPED_TRACE(std::format("{} into {}", f.label, size));
            Bytes out = exact_copy(Bytes(size, std::byte{0x5A}));
            const auto written = encode(*message, out);
            ASSERT_FALSE(written.has_value());
            const EncodeError expected{EncodeErrorKind::buffer_too_small, "out", f.length, size};
            EXPECT_EQ(written.error(), expected);
            EXPECT_EQ(to_string(written.error()),
                      std::format("buffer too small: field out: need {} bytes, have {}", f.length,
                                  size));
        }
    }
}

TEST(ItchEncode, WritesExactlyTheMessageIntoALargerBuffer) {
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        const auto message = decode_strict(f.bytes);
        ASSERT_TRUE(message.has_value());
        Bytes out(f.length + 16, std::byte{0x5A});
        ASSERT_EQ(encode(*message, out).value_or(0), f.length);
        EXPECT_TRUE(std::equal(f.bytes.begin(), f.bytes.end(), out.begin()));
        for (std::size_t i = f.length; i < out.size(); ++i) {
            EXPECT_EQ(out[i], std::byte{0x5A}) << i;
        }
    }
}

TEST(ItchEncode, RefusesATimestampThatDoesNotFitSixBytes) {
    // The writer would keep the low bytes, so this must be refused rather than
    // written as a different value.
    for (const std::uint64_t value : {kMaxTimestampNs + 1U, std::uint64_t{1} << 48U,
                                      std::numeric_limits<std::uint64_t>::max()}) {
        SCOPED_TRACE(value);
        SystemEvent event;
        event.header.timestamp_ns = value;
        Bytes out(SystemEvent::wire_size);
        const auto written = encode(Message{event}, out);
        ASSERT_FALSE(written.has_value());
        const EncodeError expected{EncodeErrorKind::value_out_of_range, "timestamp_ns",
                                   kMaxTimestampNs, value};
        EXPECT_EQ(written.error(), expected);
        EXPECT_EQ(to_string(written.error()),
                  std::format("value out of range: field timestamp_ns: largest is {}, have {}",
                              kMaxTimestampNs, value));
    }
}

TEST(ItchEncode, AcceptsTheLargestTimestamp) {
    SystemEvent event;
    event.header.timestamp_ns = kMaxTimestampNs;
    Bytes out(SystemEvent::wire_size);
    ASSERT_TRUE(encode(Message{event}, out).has_value());
    EXPECT_EQ(std::get<SystemEvent>(decode_strict(out).value()).header.timestamp_ns,
              kMaxTimestampNs);
}

TEST(ItchEncodeFrame, RebuildsTheFramedBytes) {
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        const auto message = decode_strict(f.bytes);
        ASSERT_TRUE(message.has_value());
        const Bytes expected = test::framed(static_cast<std::uint16_t>(f.length), f.bytes);
        Bytes out(expected.size());
        ASSERT_EQ(encode_frame(*message, out).value_or(0), expected.size());
        EXPECT_EQ(out, expected);
    }
}

TEST(ItchEncodeFrame, RefusesEveryBufferShorterThanTheFrame) {
    const auto& f = fixture('A');
    const auto message = decode_strict(f.bytes);
    ASSERT_TRUE(message.has_value());
    for (std::size_t size = 0; size < f.length + 2; ++size) {
        SCOPED_TRACE(size);
        Bytes out = exact_copy(Bytes(size, std::byte{0x5A}));
        const auto written = encode_frame(*message, out);
        ASSERT_FALSE(written.has_value());
        const EncodeError expected{EncodeErrorKind::buffer_too_small, "out", f.length + 2, size};
        EXPECT_EQ(written.error(), expected);
    }
}

TEST(ItchEncodeFrame, PassesARangeErrorThrough) {
    SystemEvent event;
    event.header.timestamp_ns = kMaxTimestampNs + 1U;
    Bytes out(SystemEvent::wire_size + 2);
    const auto written = encode_frame(Message{event}, out);
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().kind, EncodeErrorKind::value_out_of_range);
    EXPECT_EQ(written.error().field, "timestamp_ns");
}

}  // namespace
}  // namespace tick_store::itch
