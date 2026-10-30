// D2, D9, D10 and T1: every way a payload can be the wrong length, for every type,
// through exact-size heap buffers, with the exact error asserted. The expected
// field comes from the hand-written tables in the fixtures.

#include "itch_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <variant>

namespace tick_store::itch {
namespace {

using test::Bytes;
using test::exact_copy;
using test::FieldSpec;
using test::fixtures;

// The field a cut at `length` lands in.
const FieldSpec& field_at(const test::Fixture& f, std::size_t length) {
    const auto found = std::ranges::find_if(
        f.fields, [length](const FieldSpec& s) { return length < s.offset + s.width; });
    return *found;
}

TEST(ItchTruncation, StrictDecodeNamesTheFieldAtEveryLengthOfEveryType) {
    std::size_t cases = 0;
    for (const auto& f : fixtures()) {
        for (std::size_t length = 0; length < f.length; ++length) {
            SCOPED_TRACE(std::format("{} cut to {}", f.label, length));
            const Bytes payload = exact_copy(std::span{f.bytes}.first(length));
            const auto result = decode_strict(payload);
            ASSERT_FALSE(result.has_value());

            const FieldSpec& field = field_at(f, length);
            const std::size_t left = length - field.offset;
            const DecodeError expected{DecodeErrorKind::truncated, field.name, field.offset,
                                       field.width, left};
            EXPECT_EQ(result.error(), expected);
            EXPECT_EQ(to_string(result.error()),
                      std::format("truncated: field {} at offset {}: expected {} bytes, have {}",
                                  field.name, field.offset, field.width, left));
            ++cases;
        }
    }
    // Sum of the 23 lengths, so a silently empty loop cannot pass.
    EXPECT_EQ(cases, 694U);
}

TEST(ItchTruncation, EveryFieldOfEveryTypeFailsItsOwnCheck) {
    // One corrupt fixture per validated field (T1): a cut at each field's first
    // byte must blame that field and no other.
    std::size_t fields = 0;
    for (const auto& f : fixtures()) {
        for (const auto& field : f.fields) {
            SCOPED_TRACE(std::format("{}.{}", f.label, field.name));
            const Bytes payload = exact_copy(std::span{f.bytes}.first(field.offset));
            const auto result = decode_strict(payload);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().field, field.name);
            EXPECT_EQ(result.error().offset, field.offset);
            EXPECT_EQ(result.error().expected, field.width);
            EXPECT_EQ(result.error().actual, 0U);
            ++fields;
        }
    }
    EXPECT_EQ(fields, 191U);
}

TEST(ItchTruncation, LosslessDecodeKeepsEveryCutAsRawBytes) {
    // The same cuts through decode(): short payloads are data on this path, kept
    // whole and encoded back exactly, except the empty one, which has no bytes.
    for (const auto& f : fixtures()) {
        for (std::size_t length = 1; length < f.length; ++length) {
            SCOPED_TRACE(std::format("{} cut to {}", f.label, length));
            const Bytes payload = exact_copy(std::span{f.bytes}.first(length));
            const auto result = decode(payload);
            ASSERT_TRUE(result.has_value());
            ASSERT_TRUE(std::holds_alternative<RawMessage>(*result));
            const auto& raw = std::get<RawMessage>(*result);
            EXPECT_EQ(raw.reason, RawReason::wrong_length);
            EXPECT_EQ(raw.bytes.data(), payload.data());
            EXPECT_EQ(raw.bytes.size(), length);

            Bytes out(length);
            ASSERT_EQ(encode(*result, out).value_or(0), length);
            EXPECT_EQ(out, payload);
        }
    }
}

TEST(ItchTruncation, AnEmptyPayloadFailsBothDecoders) {
    const DecodeError expected{DecodeErrorKind::truncated, "message_type", 0, 1, 0};
    const auto strict = decode_strict({});
    const auto lossless = decode({});
    ASSERT_FALSE(strict.has_value());
    ASSERT_FALSE(lossless.has_value());
    EXPECT_EQ(strict.error(), expected);
    EXPECT_EQ(lossless.error(), expected);
    EXPECT_EQ(to_string(expected),
              "truncated: field message_type at offset 0: expected 1 bytes, have 0");
}

TEST(ItchTruncation, TrailingBytesAreRejectedNotIgnored) {
    // D9: an exact length or an error. A longer payload must not parse as its
    // valid prefix, or the extra bytes would vanish on the way back.
    for (const auto& f : fixtures()) {
        for (const std::size_t extra : {std::size_t{1}, std::size_t{2}, std::size_t{100}}) {
            SCOPED_TRACE(std::format("{} plus {}", f.label, extra));
            Bytes longer = f.bytes;
            longer.resize(f.length + extra, std::byte{0x5A});
            const Bytes payload = exact_copy(longer);

            const auto strict = decode_strict(payload);
            ASSERT_FALSE(strict.has_value());
            const DecodeError expected{DecodeErrorKind::trailing_bytes, "end of message", f.length,
                                       0, extra};
            EXPECT_EQ(strict.error(), expected);
            EXPECT_EQ(to_string(strict.error()),
                      std::format("trailing bytes: end of message at offset {}: expected 0 more "
                                  "bytes, have {}",
                                  f.length, extra));

            const auto lossless = decode(payload);
            ASSERT_TRUE(lossless.has_value());
            ASSERT_TRUE(std::holds_alternative<RawMessage>(*lossless));
            EXPECT_EQ(std::get<RawMessage>(*lossless).reason, RawReason::wrong_length);
            EXPECT_EQ(std::get<RawMessage>(*lossless).bytes.size(), f.length + extra);
        }
    }
}

TEST(ItchTruncation, AFramedMessageCutAtEveryLengthStopsTheCursor) {
    // The way a real truncation arrives: through the frame layer, at every prefix
    // and body length of every type.
    for (const auto& f : fixtures()) {
        const Bytes whole = test::framed(static_cast<std::uint16_t>(f.length), f.bytes);
        for (std::size_t length = 0; length < whole.size(); ++length) {
            SCOPED_TRACE(std::format("{} frame cut to {}", f.label, length));
            const Bytes stream = exact_copy(std::span{whole}.first(length));
            FrameCursor cursor{stream};
            const auto step = cursor.next();
            ASSERT_FALSE(step.has_value());
            if (length == 0) {
                EXPECT_EQ(step.error(), (FrameStop{FrameStopKind::end, 0, 0, 0}));
            } else if (length == 1) {
                EXPECT_EQ(step.error(), (FrameStop{FrameStopKind::truncated_prefix, 0, 2, 1}));
            } else {
                EXPECT_EQ(step.error(),
                          (FrameStop{FrameStopKind::truncated_body, 0, f.length, length - 2}));
            }
        }
    }
}

}  // namespace
}  // namespace tick_store::itch
