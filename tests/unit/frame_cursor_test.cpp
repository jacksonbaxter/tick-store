#include "tick_store/itch/frame.hpp"

#include "test_bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace tick_store::itch {
namespace {

using test::bytes;
using test::Bytes;
using test::exact_copy;
using test::framed;

Bytes short_payload() { return bytes({0xA1, 0xA2, 0xA3}); }

Bytes long_payload() { return bytes({0xB1, 0xB2, 0xB3, 0xB4, 0xB5}); }

Bytes two_frames() {
    Bytes stream = framed(3, short_payload());
    const Bytes second = framed(5, long_payload());
    stream.insert(stream.end(), second.begin(), second.end());
    return stream;
}

bool same(std::span<const std::byte> a, std::span<const std::byte> b) {
    return std::ranges::equal(a, b);
}

// constexpr, so a cursor walk can be proved at compile time too.
constexpr std::array<std::byte, 3> kOneByteFrame{std::byte{0}, std::byte{1}, std::byte{'S'}};
static_assert(FrameCursor{kOneByteFrame}.next().has_value());
static_assert(FrameCursor{kOneByteFrame}.next()->offset == 0);
static_assert(FrameCursor{kOneByteFrame}.next()->payload.size() == 1);

TEST(FrameCursor, WalksFramesAndReportsEachOffset) {
    const Bytes stream = exact_copy(two_frames());
    FrameCursor cursor{stream};

    const auto first = cursor.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->offset, 0U);
    EXPECT_TRUE(same(first->payload, short_payload()));
    EXPECT_EQ(cursor.offset(), 5U);

    const auto second = cursor.next();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->offset, 5U);
    EXPECT_TRUE(same(second->payload, long_payload()));
    EXPECT_EQ(cursor.offset(), 12U);

    const auto stop = cursor.next();
    ASSERT_FALSE(stop.has_value());
    EXPECT_EQ(stop.error(), (FrameStop{FrameStopKind::end, 12, 0, 0}));
    EXPECT_TRUE(cursor.remaining().empty());
}

TEST(FrameCursor, AnEmptyInputIsAnEnd) {
    FrameCursor cursor{std::span<const std::byte>{}};
    EXPECT_EQ(cursor.next().error(), (FrameStop{FrameStopKind::end, 0, 0, 0}));
}

TEST(FrameCursor, ALengthByteOnItsOwnIsATruncatedPrefix) {
    const Bytes stream = exact_copy(bytes({0x00}));
    FrameCursor cursor{stream};
    EXPECT_EQ(cursor.next().error(), (FrameStop{FrameStopKind::truncated_prefix, 0, 2, 1}));
    EXPECT_EQ(cursor.remaining().size(), 1U);
}

TEST(FrameCursor, TruncationAtEveryByteLengthIsTypedAndLocated) {
    // Cutting two frames at each length tests every prefix and body position.
    const Bytes whole = two_frames();
    for (std::size_t length = 0; length < whole.size(); ++length) {
        SCOPED_TRACE(length);
        const Bytes stream = exact_copy(std::span{whole}.first(length));
        FrameCursor cursor{stream};

        std::size_t frames = 0;
        auto step = cursor.next();
        for (; step.has_value(); step = cursor.next()) {
            ++frames;
        }

        // Frame ends are at 5 and 12. Prefixes occupy [0, 2) and [5, 7).
        const std::size_t frame_start = length < 5 ? 0 : 5;
        const std::size_t declared = length < 5 ? 3 : 5;
        const std::size_t left = length - frame_start;
        EXPECT_EQ(frames, length < 5 ? 0U : 1U);
        if (left == 0) {
            EXPECT_EQ(step.error(), (FrameStop{FrameStopKind::end, frame_start, 0, 0}));
        } else if (left == 1) {
            EXPECT_EQ(step.error(),
                      (FrameStop{FrameStopKind::truncated_prefix, frame_start, 2, 1}));
        } else {
            EXPECT_EQ(step.error(), (FrameStop{FrameStopKind::truncated_body, frame_start,
                                               declared, left - 2}));
        }
        EXPECT_EQ(cursor.remaining().size(), left);
    }
}

TEST(FrameCursor, AZeroLengthFrameIsReportedNotSkipped) {
    Bytes stream = framed(3, short_payload());
    const Bytes marker = bytes({0x00, 0x00, 0xEE, 0xFF});
    stream.insert(stream.end(), marker.begin(), marker.end());
    const Bytes exact = exact_copy(stream);
    FrameCursor cursor{exact};

    ASSERT_TRUE(cursor.next().has_value());
    const auto stop = cursor.next();
    ASSERT_FALSE(stop.has_value());
    EXPECT_EQ(stop.error(), (FrameStop{FrameStopKind::zero_length, 5, 0, 2}));
    EXPECT_TRUE(same(cursor.remaining(), marker));
}

TEST(FrameCursor, AZeroLengthFrameAtTheVeryEndIsStillReported) {
    const Bytes stream = exact_copy(bytes({0x00, 0x00}));
    FrameCursor cursor{stream};
    EXPECT_EQ(cursor.next().error(), (FrameStop{FrameStopKind::zero_length, 0, 0, 0}));
}

TEST(FrameCursor, AStopRepeatsAndTheCursorDoesNotMove) {
    const Bytes stream = exact_copy(bytes({0x00, 0x09, 0x01}));
    FrameCursor cursor{stream};
    const auto first = cursor.next();
    ASSERT_FALSE(first.has_value());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(cursor.next().error(), first.error());
        EXPECT_EQ(cursor.offset(), 0U);
    }
}

TEST(FrameCursor, TheLargestLengthIsReadAsUnsigned) {
    // 0xFFFF would be negative as a signed 16-bit value.
    Bytes payload(0xFFFF, std::byte{0x7E});
    const Bytes stream = exact_copy(framed(0xFFFF, payload));
    FrameCursor cursor{stream};
    const auto frame = cursor.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->payload.size(), 0xFFFFU);
    EXPECT_EQ(cursor.next().error(), (FrameStop{FrameStopKind::end, 65'537, 0, 0}));

    const Bytes cut = exact_copy(std::span{stream}.first(1'000));
    FrameCursor short_cursor{cut};
    EXPECT_EQ(short_cursor.next().error(),
              (FrameStop{FrameStopKind::truncated_body, 0, 65'535, 998}));
}

TEST(FrameStop, DescribesItselfExactly) {
    EXPECT_EQ(to_string(FrameStop{FrameStopKind::end, 12, 0, 0}),
              "end at offset 12: expected 0 bytes, have 0");
    EXPECT_EQ(to_string(FrameStop{FrameStopKind::truncated_prefix, 7, 2, 1}),
              "truncated prefix at offset 7: expected 2 bytes, have 1");
    EXPECT_EQ(to_string(FrameStop{FrameStopKind::truncated_body, 5, 36, 20}),
              "truncated body at offset 5: expected 36 bytes, have 20");
    EXPECT_EQ(to_string(FrameStop{FrameStopKind::zero_length, 9, 0, 4}),
              "zero-length frame at offset 9: expected 0 bytes, have 4");
}

}  // namespace
}  // namespace tick_store::itch
