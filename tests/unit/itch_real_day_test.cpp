// Walks the start of the pinned day through the frame cursor and the codec. The
// data file is not in the repository, so a machine without it skips, and says so.
// A full-day pass belongs to the correctness oracle, not to a unit test.

#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/itch/raw_counters.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <variant>
#include <vector>

namespace tick_store::itch {
namespace {

constexpr auto kDayFile = "12302019.NASDAQ_ITCH50";
constexpr std::size_t kMessages = 2'000'000;

// No type is longer than 50 bytes, plus its prefix, so this many bytes hold at
// least kMessages whole frames.
constexpr std::size_t kBytesNeeded = kMessages * (50 + kFramePrefixSize);

std::vector<std::byte> read_prefix(const std::filesystem::path& path, std::size_t size) {
    std::vector<std::byte> bytes(size);
    std::ifstream file{path, std::ios::binary};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): istream reads into char*.
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return bytes;
}

TEST(ItchRealDay, FirstTwoMillionMessagesRoundTripByteForByte) {
    const std::filesystem::path path = std::filesystem::path{TICK_STORE_DATA_DIR} / kDayFile;
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "NOT CHECKED: " << kDayFile
                     << " is not in the data directory, so the real-day walk did not run. "
                        "Fetch it with scripts/fetch_data.py.";
    }

    const std::vector<std::byte> stream = read_prefix(path, kBytesNeeded);
    ASSERT_GE(stream.size(), kBytesNeeded);

    FrameCursor cursor{stream};
    RawPathCounters raw;
    std::array<std::byte, kMaxPayloadSize + kFramePrefixSize> out{};
    std::size_t expected_offset = 0;

    for (std::size_t index = 0; index < kMessages; ++index) {
        const auto frame = cursor.next();
        ASSERT_TRUE(frame.has_value()) << "message " << index << ": " << to_string(frame.error());
        ASSERT_EQ(frame->offset, expected_offset) << "message " << index;

        const auto message = decode(frame->payload);
        ASSERT_TRUE(message.has_value()) << "message " << index;
        raw.observe(*message);

        const auto written = encode_frame(*message, out);
        ASSERT_TRUE(written.has_value()) << "message " << index;
        const auto source = std::span{stream}.subspan(frame->offset, *written);
        ASSERT_TRUE(std::ranges::equal(std::span{out}.first(*written), source))
            << "message " << index << " at byte " << frame->offset;

        expected_offset += *written;
        if (index == 0) {
            // The file opens with System Event `O`, Start of Messages.
            ASSERT_TRUE(std::holds_alternative<SystemEvent>(*message));
            EXPECT_EQ(std::get<SystemEvent>(*message).event_code, std::byte{'O'});
        }
    }

    // The pinned day has no raw messages in this range. A change here means the
    // decoder has stopped recognising something real.
    EXPECT_EQ(raw.total(), 0U);
}

}  // namespace
}  // namespace tick_store::itch
