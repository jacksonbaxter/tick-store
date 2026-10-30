// The raw exception path: an unknown type or a wrong length keeps every byte, is
// told apart by reason, is counted, and encodes back exactly.

#include "itch_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/itch/raw_counters.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <variant>

namespace tick_store::itch {
namespace {

using test::Bytes;
using test::exact_copy;
using test::fixture;
using test::fixtures;

Bytes payload_of_type(unsigned type, std::size_t size) {
    Bytes bytes(size, std::byte{0xA5});
    bytes.front() = static_cast<std::byte>(type);
    return exact_copy(bytes);
}

void expect_raw_round_trip(const Bytes& input, RawReason reason) {
    const auto decoded = decode(input);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<RawMessage>(*decoded));
    const auto& raw = std::get<RawMessage>(*decoded);
    EXPECT_EQ(raw.reason, reason);
    EXPECT_EQ(raw.bytes.data(), input.data());
    EXPECT_EQ(raw.bytes.size(), input.size());

    Bytes out(input.size());
    ASSERT_EQ(encode(*decoded, out).value_or(0), input.size());
    EXPECT_EQ(out, input);
}

TEST(ItchRawPath, EveryUnknownTypeByteIsKeptAndStrictDecodeRejectsIt) {
    std::size_t unknown = 0;
    for (unsigned type = 0; type < 256; ++type) {
        if (wire_size_of(static_cast<std::byte>(type)).has_value()) {
            continue;
        }
        SCOPED_TRACE(type);
        ++unknown;
        // Several sizes, because an unknown type has no length to compare with.
        for (const std::size_t size : {std::size_t{1}, std::size_t{12}, std::size_t{300}}) {
            const Bytes payload = payload_of_type(type, size);
            expect_raw_round_trip(payload, RawReason::unknown_type);

            const auto strict = decode_strict(payload);
            ASSERT_FALSE(strict.has_value());
            const DecodeError expected{DecodeErrorKind::unknown_type, "message_type", 0, 0, type};
            EXPECT_EQ(strict.error(), expected);
            EXPECT_EQ(to_string(strict.error()),
                      std::format("unknown type: field message_type at offset 0: byte 0x{:02x}",
                                  type));
        }
    }
    EXPECT_EQ(unknown, 256U - 23U);
}

TEST(ItchRawPath, ALowercaseCodeOtherThanOperationalHaltIsUnknown) {
    // `h` is the one lowercase type, so case-folding it would be a bug both ways.
    expect_raw_round_trip(payload_of_type('s', 12), RawReason::unknown_type);
    expect_raw_round_trip(payload_of_type('a', 36), RawReason::unknown_type);
    EXPECT_TRUE(wire_size_of(std::byte{'h'}).has_value());
}

TEST(ItchRawPath, WrongLengthKeepsEveryByteForEveryType) {
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        for (const std::size_t size : {std::size_t{1}, f.length - 1, f.length + 1, f.length + 7}) {
            Bytes bytes = f.bytes;
            bytes.resize(size, std::byte{0xEE});
            expect_raw_round_trip(exact_copy(bytes), RawReason::wrong_length);
        }
    }
}

TEST(ItchRawPath, TheLargestPayloadAFrameCanHoldRoundTrips) {
    const Bytes payload = payload_of_type('z', kMaxPayloadSize);
    expect_raw_round_trip(payload, RawReason::unknown_type);

    const auto decoded = decode(payload);
    ASSERT_TRUE(decoded.has_value());
    Bytes out(kMaxPayloadSize + kFramePrefixSize);
    ASSERT_EQ(encode_frame(*decoded, out).value_or(0), out.size());
    EXPECT_EQ(out, test::framed(0xFFFF, payload));
}

TEST(ItchRawPath, EncodeRefusesABufferSmallerThanTheRawBytes) {
    const Bytes payload = payload_of_type('z', 20);
    const auto decoded = decode(payload);
    ASSERT_TRUE(decoded.has_value());
    Bytes out(19);
    const auto written = encode(*decoded, out);
    ASSERT_FALSE(written.has_value());
    const EncodeError expected{EncodeErrorKind::buffer_too_small, "out", 20, 19};
    EXPECT_EQ(written.error(), expected);
}

TEST(ItchRawPath, EncodeFrameRefusesAPayloadAPrefixCannotDeclare) {
    const Bytes payload = payload_of_type('z', kMaxPayloadSize + 1);
    const Message raw{RawMessage{payload, RawReason::unknown_type}};
    Bytes out(payload.size() + kFramePrefixSize);
    const auto written = encode_frame(raw, out);
    ASSERT_FALSE(written.has_value());
    const EncodeError expected{EncodeErrorKind::value_out_of_range, "payload_size",
                               kMaxPayloadSize, kMaxPayloadSize + 1};
    EXPECT_EQ(written.error(), expected);
}

TEST(ItchRawPath, EncodeFrameOfAnEmptyRawMessageNeedsTheWholePrefix) {
    const Message raw{RawMessage{{}, RawReason::wrong_length}};
    Bytes small(1);
    const auto refused = encode_frame(raw, small);
    ASSERT_FALSE(refused.has_value());
    const EncodeError expected{EncodeErrorKind::buffer_too_small, "out", 2, 1};
    EXPECT_EQ(refused.error(), expected);

    Bytes out(2, std::byte{0x5A});
    ASSERT_EQ(encode_frame(raw, out).value_or(0), 2U);
    EXPECT_EQ(out, Bytes(2, std::byte{0x00}));
}

TEST(RawMessage, ComparesContentAndReasonNotAddress) {
    const Bytes a = payload_of_type('z', 4);
    const Bytes b = payload_of_type('z', 4);
    Bytes c = payload_of_type('z', 4);
    c.back() = std::byte{0x00};
    const Bytes shorter = payload_of_type('z', 3);

    const RawMessage base{a, RawReason::unknown_type};
    EXPECT_EQ(base, (RawMessage{b, RawReason::unknown_type}));
    EXPECT_NE(base, (RawMessage{b, RawReason::wrong_length}));
    EXPECT_NE(base, (RawMessage{c, RawReason::unknown_type}));
    EXPECT_NE(base, (RawMessage{shorter, RawReason::unknown_type}));
}

TEST(RawPathCounters, StartAtZero) {
    const RawPathCounters counters;
    EXPECT_EQ(counters.total(), 0U);
    EXPECT_EQ(counters.count(RawReason::unknown_type), 0U);
    EXPECT_EQ(counters.count(RawReason::wrong_length), 0U);
}

TEST(RawPathCounters, CountEachReasonSeparately) {
    RawPathCounters counters;
    counters.record(RawReason::unknown_type);
    counters.record(RawReason::wrong_length);
    counters.record(RawReason::wrong_length);
    EXPECT_EQ(counters.count(RawReason::unknown_type), 1U);
    EXPECT_EQ(counters.count(RawReason::wrong_length), 2U);
    EXPECT_EQ(counters.total(), 3U);
}

TEST(RawPathCounters, ObserveCountsRawMessagesOnly) {
    RawPathCounters counters;
    const Bytes unknown = payload_of_type('z', 5);
    Bytes short_add = fixture('A').bytes;
    short_add.pop_back();

    const std::array<const Bytes*, 5> payloads{&fixture('A').bytes, &fixture('h').bytes, &unknown,
                                               &short_add, &unknown};
    for (const Bytes* payload : payloads) {
        const auto decoded = decode(*payload);
        ASSERT_TRUE(decoded.has_value());
        counters.observe(*decoded);
    }
    EXPECT_EQ(counters.count(RawReason::unknown_type), 2U);
    EXPECT_EQ(counters.count(RawReason::wrong_length), 1U);
    EXPECT_EQ(counters.total(), 3U);
}

TEST(RawPathCounters, AWalkOverAMixedStreamRebuildsItExactly) {
    // Frames of known, unknown and wrong-length payloads, end to end: the cursor
    // and the codec together must reproduce the stream, and report what was raw.
    Bytes stream;
    const auto append = [&stream](const Bytes& payload) {
        const Bytes frame = test::framed(static_cast<std::uint16_t>(payload.size()), payload);
        stream.insert(stream.end(), frame.begin(), frame.end());
    };
    Bytes long_delete = fixture('D').bytes;
    long_delete.push_back(std::byte{0x01});
    append(fixture('S').bytes);
    append(payload_of_type('z', 9));
    append(fixture('A').bytes);
    append(long_delete);
    append(fixture('h').bytes);
    const Bytes input = exact_copy(stream);

    Bytes rebuilt(input.size());
    std::size_t out = 0;
    RawPathCounters counters;
    FrameCursor cursor{input};
    auto frame = cursor.next();
    for (; frame.has_value(); frame = cursor.next()) {
        const auto message = decode(frame->payload);
        ASSERT_TRUE(message.has_value());
        counters.observe(*message);
        const auto written = encode_frame(*message, std::span{rebuilt}.subspan(out));
        ASSERT_TRUE(written.has_value());
        out += *written;
    }
    EXPECT_EQ(frame.error().kind, FrameStopKind::end);
    EXPECT_EQ(rebuilt, input);
    EXPECT_EQ(counters.count(RawReason::unknown_type), 1U);
    EXPECT_EQ(counters.count(RawReason::wrong_length), 1U);
}

}  // namespace
}  // namespace tick_store::itch
