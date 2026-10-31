// The census against streams small enough to count by hand. Messages come through the
// encoder, which the golden tests hold to the specification.

#include "itch_fixtures.hpp"
#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/census.hpp"
#include "tick_store/harness/data_tools_cli.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::harness {
namespace {

using itch::test::bytes;
using test::Bytes;

itch::Stock stock(std::string_view name) {
    itch::Stock out{};
    std::ranges::fill(out, std::byte{' '});
    std::ranges::transform(name, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

void append(Bytes& out, const itch::Message& message) {
    std::array<std::byte, 128> frame{};
    const std::size_t size = itch::encode_frame(message, frame).value();
    out.insert(out.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
}

itch::MessageHeader header(std::uint16_t locate, std::uint64_t timestamp,
                           std::uint16_t tracking = 0) {
    return {.stock_locate = locate, .tracking_number = tracking, .timestamp_ns = timestamp};
}

itch::Message add(std::uint16_t locate, std::uint64_t timestamp, std::uint64_t reference,
                  std::uint32_t price, std::string_view name) {
    itch::AddOrder message;
    message.header = header(locate, timestamp);
    message.order_reference_number = reference;
    message.stock = stock(name);
    message.price = itch::Price4{price};
    return message;
}

itch::Message directory(std::uint16_t locate, std::string_view name) {
    itch::StockDirectory message;
    message.header = header(locate, 0);
    message.stock = stock(name);
    return message;
}

itch::Message event(std::uint64_t timestamp) {
    itch::SystemEvent message;
    message.header = header(0, timestamp, 7);
    return message;
}

TEST(Log2Histogram, SortsValuesIntoPowersOfTwoWithTheSignsApart) {
    Log2Histogram histogram;
    histogram.add(0);
    histogram.add(1);
    histogram.add(2);
    histogram.add(3);
    histogram.add(4);
    histogram.add(-1);
    histogram.add(-7);
    histogram.add(std::numeric_limits<std::int64_t>::max());
    histogram.add(std::numeric_limits<std::int64_t>::min());

    EXPECT_EQ(histogram.zero(), 1U);
    EXPECT_EQ(histogram.positive()[0], 1U);
    EXPECT_EQ(histogram.positive()[1], 2U);
    EXPECT_EQ(histogram.positive()[2], 1U);
    EXPECT_EQ(histogram.positive()[62], 1U);
    EXPECT_EQ(histogram.negative()[0], 1U);
    EXPECT_EQ(histogram.negative()[2], 1U);
    EXPECT_EQ(histogram.negative()[63], 1U);
    EXPECT_EQ(histogram.total(), 9U);
    EXPECT_EQ(histogram.positive_total(), 5U);
    EXPECT_EQ(histogram.negative_total(), 3U);
}

TEST(Log2Histogram, TakesDifferencesTooWideForASignedValue) {
    Log2Histogram histogram;
    const std::uint64_t top = std::numeric_limits<std::uint64_t>::max();
    histogram.add_difference(top, 0);
    histogram.add_difference(0, top);
    histogram.add_difference(5, 5);
    EXPECT_EQ(histogram.positive()[63], 1U);
    EXPECT_EQ(histogram.negative()[63], 1U);
    EXPECT_EQ(histogram.zero(), 1U);
}

TEST(Log2Histogram, MeanWidthAndIdealCodeAreRightOnAKnownSplit) {
    Log2Histogram histogram;
    histogram.add(0);
    histogram.add(0);
    histogram.add(1);  // bucket 0: one significant bit, no low bits
    histogram.add(4);  // bucket 2: three significant bits, two low bits
    EXPECT_DOUBLE_EQ(histogram.mean_bit_width(), 1.0);
    // Entropy of {1/2, 1/4, 1/4} is 1.5 bits, plus 1/4 of the 2 low bits.
    EXPECT_DOUBLE_EQ(histogram.ideal_code_bits(), 2.0);
    EXPECT_DOUBLE_EQ(Log2Histogram{}.mean_bit_width(), 0.0);
    EXPECT_DOUBLE_EQ(Log2Histogram{}.ideal_code_bits(), 0.0);
}

TEST(Census, ReconcilesCountsAndBytesPerTypeAgainstTheFile) {
    const Bytes source = test::source_with_tail();
    const Census census = take_census(source);

    EXPECT_EQ(census.frames, test::kSourceRecordCount);
    EXPECT_EQ(census.tail_bytes, test::truncated_tail().size());
    EXPECT_EQ(census.frame_bytes + census.tail_bytes, census.file_bytes);
    EXPECT_EQ(census.file_bytes, source.size());
    EXPECT_EQ(census.stop, itch::to_string(itch::FrameStopKind::truncated_body));

    std::uint64_t frames = 0;
    std::uint64_t frame_bytes = 0;
    for (const TypeCensus& type : census.types) {
        frames += type.frames;
        frame_bytes += type.frame_bytes;
    }
    EXPECT_EQ(frames, census.frames);
    EXPECT_EQ(frame_bytes, census.frame_bytes);

    for (const auto& fixture : itch::test::fixtures()) {
        const std::uint64_t extra = fixture.type == test::kWrongLengthType ? 1 : 0;
        EXPECT_EQ(census.type(fixture.type).frames, 1U + extra) << fixture.type;
    }
    // The raw-path records: two of a type that does not exist, one of the wrong length.
    EXPECT_EQ(census.type(test::kUnknownType).frames, 2U);
    EXPECT_EQ(census.type(test::kWrongLengthType).off_spec_length, 1U);
    EXPECT_EQ(census.type('A').off_spec_length, 0U);
    // Raw records have no fields, so only the 23 typed ones reach the header counters.
    EXPECT_EQ(census.tracking_checked, itch::kMessageTypeCount);
}

TEST(Census, ReportsAZeroLengthTerminatorAndEverythingAfterItAsTail) {
    Bytes source;
    append(source, event(5));
    const Bytes rest = bytes({0x00, 0x00, 0x41, 0x42});
    source.insert(source.end(), rest.begin(), rest.end());

    const Census census = take_census(source);
    EXPECT_EQ(census.frames, 1U);
    EXPECT_EQ(census.tail_bytes, rest.size());
    EXPECT_EQ(census.stop, itch::to_string(itch::FrameStopKind::zero_length));
    EXPECT_NE(to_json(census, 0.0).find(R"("zero_length_terminator": true)"), std::string::npos);
}

TEST(Census, CountsTrackingNumbersThatAreNotZero) {
    Bytes source;
    append(source, event(1));
    append(source, add(1, 2, 1, 100, "A"));
    const Census census = take_census(source);
    EXPECT_EQ(census.tracking_checked, 2U);
    EXPECT_EQ(census.tracking_nonzero, 1U);
    EXPECT_EQ(census.type('S').tracking_nonzero, 1U);
    EXPECT_EQ(census.type('A').tracking_nonzero, 0U);
}

TEST(Census, FollowsTimestampsGloballyAndPerLocate) {
    Bytes source;
    // Locate 1: 100, 100, 130, 110. Locate 2: 120, 125. In file order, interleaved.
    append(source, add(1, 100, 1, 100, "A"));
    append(source, add(1, 100, 2, 100, "A"));
    append(source, add(2, 120, 3, 100, "B"));
    append(source, add(2, 125, 4, 100, "B"));
    append(source, add(1, 130, 5, 100, "A"));
    append(source, add(1, 110, 6, 100, "A"));

    const Census census = take_census(source);
    EXPECT_EQ(census.first_timestamp_ns, 100U);
    EXPECT_EQ(census.last_timestamp_ns, 110U);

    // Global deltas: 0, 20, 5, 5, -20. Deltas of deltas: 20, -15, 0, -25.
    const TimestampCensus& global = census.timestamps_global;
    EXPECT_EQ(global.values, 6U);
    EXPECT_EQ(global.repeats, 1U);
    EXPECT_EQ(global.decreases, 1U);
    EXPECT_EQ(global.largest_decrease_ns, 20U);
    EXPECT_EQ(global.delta.total(), 5U);
    EXPECT_EQ(global.delta.zero(), 1U);
    EXPECT_EQ(global.delta.positive()[4], 1U);
    EXPECT_EQ(global.delta.positive()[2], 2U);
    EXPECT_EQ(global.delta.negative()[4], 1U);
    EXPECT_EQ(global.delta_of_delta.total(), 4U);
    EXPECT_EQ(global.delta_of_delta.zero(), 1U);
    EXPECT_EQ(global.delta_of_delta.positive()[4], 1U);
    EXPECT_EQ(global.delta_of_delta.negative()[3], 1U);
    EXPECT_EQ(global.delta_of_delta.negative()[4], 1U);

    // Locate 1 deltas: 0, 30, -20. Locate 2 delta: 5. Deltas of deltas, locate 1: 30, -50.
    const TimestampCensus& per_locate = census.timestamps_per_locate;
    EXPECT_EQ(per_locate.values, 6U);
    EXPECT_EQ(per_locate.repeats, 1U);
    EXPECT_EQ(per_locate.decreases, 1U);
    EXPECT_EQ(per_locate.delta.total(), 4U);
    EXPECT_EQ(per_locate.delta_of_delta.total(), 2U);
    EXPECT_EQ(census.locates.seen, 2U);
    EXPECT_EQ(census.locates.locates_with_a_decrease, 1U);
}

TEST(Census, SortsPricesByDivisibilityPerField) {
    Bytes source;
    append(source, add(1, 1, 1, 0, "A"));                // no price
    append(source, add(1, 2, 2, 1'234'500, "A"));        // a whole cent
    append(source, add(1, 3, 3, 1'234'550, "A"));        // half a cent
    append(source, add(1, 4, 4, 1'234'510, "A"));        // a tenth of a cent
    append(source, add(1, 5, 5, 1'234'507, "A"));        // none of them
    append(source, add(1, 6, 6, 9'900, "A"));            // whole cent, below a dollar
    append(source, add(1, 7, 7, 9'901, "A"));            // sub-penny, below a dollar
    append(source, add(1, 8, 8, 1'999'999'900, "A"));    // the stub-quote ceiling

    const Census census = take_census(source);
    const PriceCensus& price = census.price(PriceField::add_order);
    EXPECT_EQ(price.values, 8U);
    EXPECT_EQ(price.zero, 1U);
    EXPECT_EQ(price.multiples_of_100, 3U);
    EXPECT_EQ(price.multiples_of_50, 4U);
    EXPECT_EQ(price.multiples_of_10, 5U);
    EXPECT_EQ(price.below_one_dollar, 2U);
    EXPECT_EQ(price.below_one_dollar_multiples_of_100, 1U);
    EXPECT_EQ(price.minimum, 9'900U);
    EXPECT_EQ(price.maximum, 1'999'999'900U);
    EXPECT_EQ(census.price(PriceField::trade).values, 0U);
}

TEST(Census, ReachesEveryPriceAndReferenceFieldOfTheFixtures) {
    const Census census = take_census(test::source_records());
    for (std::size_t i = 0; i < kPriceFieldCount; ++i) {
        const auto field = static_cast<PriceField>(i);
        EXPECT_EQ(census.price(field).values, 1U) << name_of(field);
    }
    for (std::size_t i = 0; i < kReferenceFieldCount; ++i) {
        const auto field = static_cast<ReferenceField>(i);
        EXPECT_EQ(census.reference(field).values, 1U) << name_of(field);
    }
}

TEST(Census, TakesOrderReferenceDeltasPerFieldAndAgainstTheRunningMaximum) {
    Bytes source;
    append(source, add(1, 1, 10, 100, "A"));
    append(source, add(1, 2, 12, 100, "A"));
    append(source, add(1, 3, 11, 100, "A"));
    itch::OrderDelete gone;
    gone.header = header(1, 4);
    gone.order_reference_number = 10;
    append(source, gone);
    append(source, add(1, 5, 0, 100, "A"));

    const Census census = take_census(source);
    const ReferenceCensus& added = census.reference(ReferenceField::add_order);
    EXPECT_EQ(added.values, 4U);
    EXPECT_EQ(added.zero, 1U);
    EXPECT_EQ(added.delta_previous.total(), 3U);
    EXPECT_EQ(added.delta_previous.positive()[1], 1U);  // +2
    EXPECT_EQ(added.delta_previous.negative()[0], 1U);  // -1
    EXPECT_EQ(added.delta_previous.negative()[3], 1U);  // -11
    EXPECT_EQ(added.delta_running_maximum.positive()[1], 1U);  // 12 against 10
    EXPECT_EQ(added.delta_running_maximum.negative()[0], 1U);  // 11 against 12

    const ReferenceCensus& deleted = census.reference(ReferenceField::deleted);
    EXPECT_EQ(deleted.values, 1U);
    EXPECT_EQ(deleted.delta_previous.total(), 0U);
    EXPECT_EQ(deleted.delta_running_maximum.negative()[1], 1U);  // 10 against 12
}

TEST(Census, ComparesTheStockFieldWithTheDirectory) {
    Bytes source;
    append(source, directory(1, "AAPL"));
    append(source, directory(2, "MSFT"));
    append(source, directory(3, "MSFT"));  // a second locate for one stock
    append(source, add(1, 1, 1, 100, "AAPL"));
    append(source, add(1, 2, 2, 100, "MSFT"));  // the wrong stock for the locate
    append(source, add(9, 3, 3, 100, "TSLA"));  // a locate no R has named
    append(source, directory(1, "GOOG"));       // the directory itself changes
    append(source, add(1, 4, 4, 100, "GOOG"));

    const Census census = take_census(source);
    EXPECT_EQ(census.type('A').stock_checked, 3U);  // locate 9 had nothing to compare with
    EXPECT_EQ(census.type('A').stock_mismatches, 1U);
    EXPECT_EQ(census.type('A').locate_not_in_directory, 1U);
    EXPECT_EQ(census.type('R').stock_checked, 1U);
    EXPECT_EQ(census.type('R').stock_mismatches, 1U);
    EXPECT_EQ(census.locates.in_directory, 3U);
    EXPECT_EQ(census.locates.directory_changes, 1U);
    EXPECT_EQ(census.locates.stocks_with_several_locates, 1U);
}

// A record can name a locate before its `R` does, and the `R` must still be held to it.
TEST(Census, HoldsADirectoryEntryToTheStockAnEarlierRecordNamed) {
    Bytes source;
    append(source, add(1, 1, 1, 100, "AAPL"));
    append(source, directory(1, "AAPL"));
    append(source, add(2, 2, 2, 100, "MSFT"));
    append(source, directory(2, "TSLA"));

    const Census census = take_census(source);
    EXPECT_EQ(census.type('A').locate_not_in_directory, 2U);
    EXPECT_EQ(census.type('R').stock_checked, 2U);
    EXPECT_EQ(census.type('R').stock_mismatches, 1U);
    EXPECT_EQ(census.locates.directory_changes, 0U);
}

TEST(CensusCli, WritesJsonWithNoPathsAndExitsZero) {
    const test::ScratchDir dir;
    test::write_file(dir / "source", test::source_with_tail());
    const std::string source = (dir / "source").string();
    const std::string out = (dir / "census.json").string();
    const std::vector<std::string_view> arguments{source, out};

    CliOutput output;
    EXPECT_EQ(run_census_cli(arguments, output), kExitPass) << output.err;

    const Bytes written = test::read_file(out);
    const std::string_view text{reinterpret_cast<const char*>(written.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): reading bytes as text
                                written.size()};
    EXPECT_TRUE(text.contains(R"("frames": 26)"));
    EXPECT_TRUE(text.contains(R"("frames_and_tail_equal_file_size": true)"));
    EXPECT_TRUE(text.contains(R"("unknown_type_frames": 2)"));
    EXPECT_TRUE(text.contains(R"("off_spec_length_frames": 1)"));
    EXPECT_FALSE(text.contains(dir.path().string()));
}

TEST(CensusCli, FailsWithExitTwo) {
    const test::ScratchDir dir;
    const std::string absent = (dir / "absent").string();
    const std::string no_dir = (dir / "no" / "dir").string();
    test::write_file(dir / "source", test::source_records());
    const std::string source = (dir / "source").string();

    const auto run = [](std::vector<std::string_view> arguments) {
        CliOutput output;
        return run_census_cli(arguments, output);
    };
    EXPECT_EQ(run({}), kExitUsageOrIo);
    EXPECT_EQ(run({source}), kExitUsageOrIo);
    EXPECT_EQ(run({absent, no_dir}), kExitUsageOrIo);
    EXPECT_EQ(run({source, no_dir}), kExitUsageOrIo);
}

}  // namespace
}  // namespace tick_store::harness
