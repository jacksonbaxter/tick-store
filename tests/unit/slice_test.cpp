// The slice tool on a stream small enough to read: which records go in, in what order,
// and that the check at the end refuses what is not a clean stream.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/data_tools_cli.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/harness/slice.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace tick_store::harness {
namespace {

using itch::test::bytes;
using test::Bytes;

void append(Bytes& out, const itch::Message& message) {
    std::array<std::byte, 128> frame{};
    const std::size_t size = itch::encode_frame(message, frame).value();
    out.insert(out.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
}

itch::Message add(std::uint64_t timestamp) {
    itch::AddOrder message;
    message.header.timestamp_ns = timestamp;
    return message;
}

itch::Message event(std::uint64_t timestamp) {
    itch::SystemEvent message;
    message.header.timestamp_ns = timestamp;
    return message;
}

itch::Message directory(std::uint64_t timestamp) {
    itch::StockDirectory message;
    message.header.timestamp_ns = timestamp;
    return message;
}

// The timestamps of a stream, in order.
std::vector<std::uint64_t> timestamps_of(const Bytes& stream) {
    std::vector<std::uint64_t> out;
    for (const itch::Message& message : test::messages_of(stream)) {
        out.push_back(std::visit(
            []<class M>(const M& typed) -> std::uint64_t {
                if constexpr (std::is_same_v<M, itch::RawMessage>) {
                    return 0;
                } else {
                    return typed.header.timestamp_ns;
                }
            },
            message));
    }
    return out;
}

TEST(TimeOfDay, ParsesToNanosecondsAndRefusesEverythingElse) {
    EXPECT_EQ(parse_time_of_day("00:00:00"), 0U);
    EXPECT_EQ(parse_time_of_day("09:29:30"), 34'170'000'000'000U);
    EXPECT_EQ(parse_time_of_day("23:59:59"), 86'399'000'000'000U);
    for (const std::string_view bad :
         {"", "9:29:30", "09:29", "24:00:00", "09:60:00", "09:29:60", "09-29-30", "09:29:3x",
          "09:29:300", "+9:29:30"}) {
        EXPECT_EQ(parse_time_of_day(bad), std::nullopt) << bad;
    }
}

class Slice : public ::testing::Test {
protected:
    test::ScratchDir dir;
    std::filesystem::path path = dir / "slice";

    Bytes source_ = [] {
        Bytes out;
        append(out, event(1));       // before the window, kept
        append(out, directory(2));   // before the window, kept
        append(out, add(3));         // before the window, dropped
        append(out, add(10));        // the window's first record: from is inclusive
        append(out, directory(11));  // inside, once only
        append(out, add(19));        // the window's last record
        append(out, add(20));        // after: to is exclusive
        append(out, directory(21));  // after the window, kept
        append(out, add(30));        // after, dropped
        append(out, event(40));      // after the window, kept
        return out;
    }();
};

TEST_F(Slice, KeepsEverySAndRPlusTheWindowInFileOrder) {
    const auto report = write_slice(source_, path, {.from_ns = 10, .to_ns = 20});
    ASSERT_TRUE(report.has_value()) << report.error();

    const Bytes written = test::read_file(path);
    EXPECT_EQ(timestamps_of(written), (std::vector<std::uint64_t>{1, 2, 10, 11, 19, 21, 40}));
    EXPECT_EQ(report->messages, 7U);
    EXPECT_EQ(report->bytes, written.size());
    EXPECT_EQ(report->window_messages, 3U);
    EXPECT_EQ(report->system_messages_outside, 2U);
    EXPECT_EQ(report->directory_messages_outside, 2U);
    EXPECT_EQ(report->in_window_outside_range, 0U);
    EXPECT_EQ(report->in_range_outside_window, 0U);

    // Named, not auto: libstdc++'s operator[] returns its own alias for the element type, so
    // with auto the use of itch::Frame would not show on Linux.
    const std::vector<itch::Frame> frames = test::frames_of(source_).frames;
    EXPECT_EQ(report->window_first_offset, frames[3].offset);
    EXPECT_EQ(report->window_end_offset, frames[6].offset);
}

TEST_F(Slice, CopiesEveryFrameVerbatim) {
    ASSERT_TRUE(write_slice(source_, path, {.from_ns = 0, .to_ns = 1000}).has_value());
    EXPECT_EQ(test::read_file(path), source_);
}

TEST_F(Slice, KeepsNoPartialFile) {
    ASSERT_TRUE(write_slice(source_, path, {.from_ns = 10, .to_ns = 20}).has_value());
    std::filesystem::path partial = path;
    partial += ".partial";
    EXPECT_FALSE(std::filesystem::exists(partial));
}

TEST_F(Slice, AWindowThatNothingReachesStillKeepsTheDirectory) {
    const auto report = write_slice(source_, path, {.from_ns = 500, .to_ns = 600});
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->window_messages, 0U);
    EXPECT_EQ(report->messages, 5U);
    EXPECT_EQ(report->window_first_offset, source_.size());
    EXPECT_EQ(report->window_end_offset, source_.size());
}

TEST_F(Slice, AWindowRunningToTheEndOfTheFileEndsThere) {
    const auto report = write_slice(source_, path, {.from_ns = 30, .to_ns = 1000});
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->window_end_offset, source_.size());
    EXPECT_EQ(report->window_messages, 2U);
}

// A step backwards puts records on the wrong side of a run of the file, and the report
// must say so, because the slice would otherwise claim more than it holds.
TEST_F(Slice, CountsRecordsThatTimeOrderPutsOnTheWrongSideOfTheWindow) {
    Bytes source;
    append(source, add(5));
    append(source, add(10));
    append(source, add(7));   // inside the run, before the range
    append(source, add(25));  // ends the window
    append(source, add(15));  // in the range, after the run
    const auto report = write_slice(source, path, {.from_ns = 10, .to_ns = 20});
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->window_messages, 2U);
    EXPECT_EQ(report->in_window_outside_range, 1U);
    EXPECT_EQ(report->in_range_outside_window, 1U);
}

TEST_F(Slice, RefusesAnEmptyWindowAndAPathItCannotWrite) {
    EXPECT_FALSE(write_slice(source_, path, {.from_ns = 20, .to_ns = 20}).has_value());
    EXPECT_FALSE(write_slice(source_, dir / "no" / "dir", {.from_ns = 1, .to_ns = 2}).has_value());
}

TEST_F(Slice, CheckCountsAndHashesACleanStream) {
    ASSERT_TRUE(write_slice(source_, path, {.from_ns = 10, .to_ns = 20}).has_value());
    const Bytes written = test::read_file(path);
    const auto check = check_slice(written);
    ASSERT_TRUE(check.has_value()) << check.error();
    EXPECT_EQ(check->messages, 7U);
    EXPECT_EQ(check->bytes, written.size());
    EXPECT_EQ(check->system_messages, 2U);
    EXPECT_EQ(check->directory_messages, 3U);
    EXPECT_EQ(check->sha256, sha256(written));
}

TEST_F(Slice, CheckRefusesAStreamWithATail) {
    EXPECT_FALSE(check_slice(test::source_with_tail()).has_value());
}

TEST_F(Slice, CheckRefusesARecordOffItsSpecification) {
    Bytes stream;
    append(stream, event(1));
    const Bytes bad = itch::test::framed(4, bytes({'z', 1, 2, 3}));
    stream.insert(stream.end(), bad.begin(), bad.end());
    const auto check = check_slice(stream);
    ASSERT_FALSE(check.has_value());
    EXPECT_TRUE(check.error().contains("record 1"));
}

class SliceCli : public Slice {
protected:
    SliceCli() { test::write_file(dir / "source", source_); }

    [[nodiscard]] int run(std::vector<std::string> arguments) {
        const std::vector<std::string_view> views{arguments.begin(), arguments.end()};
        return run_slice_cli(views, output);
    }

    std::string source_path = (dir / "source").string();
    std::string out_path = path.string();
    CliOutput output;
};

TEST_F(SliceCli, ReportsTheDefinitionItWroteWithNoPaths) {
    EXPECT_EQ(run({source_path, out_path, "--from", "00:00:00", "--to", "00:00:01"}), kExitPass)
        << output.err;
    const std::string pin = to_hex(sha256(test::read_file(path)));
    EXPECT_TRUE(output.out.contains(std::format(R"("sha256": "{}")", pin)));
    EXPECT_TRUE(output.out.contains(R"("from": "00:00:00")"));
    EXPECT_TRUE(output.out.contains(R"("messages": 10)"));
    EXPECT_TRUE(output.out.contains(R"("decodes_cleanly": true)"));
    EXPECT_FALSE(output.out.contains(dir.path().string()));
}

TEST_F(SliceCli, DefaultsToTheOpen) {
    EXPECT_EQ(run({source_path, out_path}), kExitPass) << output.err;
    EXPECT_TRUE(output.out.contains(R"("from": "09:29:30")"));
    EXPECT_TRUE(output.out.contains(R"("to": "09:34:30")"));
    EXPECT_TRUE(output.out.contains(R"("window_messages": 0)"));
}

TEST_F(SliceCli, ASourceThatIsNotCleanFailsTheCheckWithExitOne) {
    Bytes bad = source_;
    const Bytes raw = itch::test::framed(4, bytes({'S', 1, 2, 3}));  // S at the wrong length
    // Second in the file, so inside a window that the first record opens.
    const auto second = static_cast<std::ptrdiff_t>(test::frames_of(source_).frames[1].offset);
    bad.insert(bad.begin() + second, raw.begin(), raw.end());
    test::write_file(dir / "source", bad);
    EXPECT_EQ(run({source_path, out_path, "--from", "00:00:00", "--to", "00:00:01"}),
              kExitMismatch);
    EXPECT_TRUE(output.err.contains("does not check out"));
}

TEST_F(SliceCli, BadUsageAndFailedIoExitTwo) {
    const std::string absent = (dir / "absent").string();
    EXPECT_EQ(run({}), kExitUsageOrIo);
    EXPECT_EQ(run({source_path}), kExitUsageOrIo);
    EXPECT_EQ(run({source_path, out_path, "--from"}), kExitUsageOrIo);
    EXPECT_EQ(run({source_path, out_path, "--from", "9"}), kExitUsageOrIo);
    EXPECT_EQ(run({source_path, out_path, "--to", "99:00:00"}), kExitUsageOrIo);
    EXPECT_EQ(run({source_path, out_path, "--from", "10:00:00", "--to", "09:00:00"}),
              kExitUsageOrIo);
    EXPECT_EQ(run({source_path, out_path, "--frobnicate", "1"}), kExitUsageOrIo);
    EXPECT_TRUE(output.err.contains("usage: tick_slice"));
    EXPECT_EQ(run({absent, out_path}), kExitUsageOrIo);
    EXPECT_EQ(run({source_path, (dir / "no" / "dir").string()}), kExitUsageOrIo);
}

}  // namespace
}  // namespace tick_store::harness
