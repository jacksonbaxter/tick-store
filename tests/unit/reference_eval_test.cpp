// tick_reference_eval's accounting on a stream small enough to count by hand: references,
// imports and resolutions per segment size, and the state each encoding needs.

#include "file_size_limit.hpp"
#include "itch_store_fixture.hpp"
#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "tick_store/harness/codec_eval.hpp"
#include "tick_store/harness/reference_eval.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::harness {
namespace {

using store::test::add;
using store::test::deleted;
using store::test::executed;
using store::test::replaced;
using Bytes = std::vector<std::byte>;

Bytes stream() {
    const std::vector<itch::Message> messages{
        add(1, 1, 100),       add(1, 2, 101),          // segment 0 at N = 2
        executed(1, 3, 100),  deleted(1, 4, 101),       // segment 1: two imports
        add(2, 5, 200),       replaced(2, 6, 200, 201), // segment 2: one in-segment
        deleted(2, 7, 201),   executed(3, 8, 999),      // segment 3: two imports, 999 unresolved
    };
    Bytes out;
    std::array<std::byte, itch::kFramePrefixSize + itch::kMaxPayloadSize> frame{};
    for (const itch::Message& message : messages) {
        const auto written = itch::encode_frame(message, frame);
        EXPECT_TRUE(written.has_value());
        out.insert(out.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(*written));
    }
    return out;
}

const EncodingCost& encoding(const SegmentSizeCost& size, std::string_view name) {
    for (const EncodingCost& cost : size.encodings) {
        if (cost.name == name) {
            return cost;
        }
    }
    throw std::runtime_error("no such encoding");
}

TEST(ReferenceEval, CountsReferencesImportsAndStatePerSegmentSize) {
    const Bytes source = stream();
    const auto result = evaluate_references(
        source, {.segment_records = {2, 100}, .checkpoint_segment_records = {2}});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->records, 8U);
    EXPECT_EQ(result->references, 5U);
    EXPECT_EQ(result->unresolved, 1U);
    EXPECT_EQ(result->peak_live_orders, 2U);
    ASSERT_EQ(result->sizes.size(), 2U);

    const SegmentSizeCost& small = result->sizes[0];
    EXPECT_EQ(small.segment_records, 2U);
    EXPECT_EQ(small.segments, 4U);
    EXPECT_EQ(small.imports, 4U);
    EXPECT_EQ(small.in_segment, 1U);
    EXPECT_EQ(encoding(small, "within_stock_imports").state_values, 4U);
    EXPECT_EQ(encoding(small, "within_stock_imports").max_segment_state_values, 2U);
    EXPECT_GT(encoding(small, "within_stock_imports").state_bytes, 0U);
    // The live book at each segment's start: 0, then 100 and 101, then 100, then 100 and 201.
    EXPECT_TRUE(encoding(small, "day_distance_checkpoint").measured);
    EXPECT_EQ(encoding(small, "day_distance_checkpoint").state_values, 5U);
    EXPECT_EQ(encoding(small, "day_distance_checkpoint").max_segment_state_values, 2U);
    // The locates with creations before each segment: none, 1, 1, then 1 and 2.
    EXPECT_EQ(encoding(small, "day_distance_index").state_values, 4U);
    for (const std::string_view name : {"values_pfor_delta", "values_head_delta", "values_svb_delta"}) {
        EXPECT_EQ(encoding(small, name).state_bytes, 0U) << name;
        EXPECT_GT(encoding(small, name).column_bytes, 0U) << name;
    }

    const SegmentSizeCost& whole = result->sizes[1];
    EXPECT_EQ(whole.segments, 1U);
    EXPECT_EQ(whole.imports, 1U);
    EXPECT_EQ(whole.in_segment, 4U);
    EXPECT_FALSE(encoding(whole, "day_distance_checkpoint").measured);
    EXPECT_EQ(encoding(whole, "day_distance_checkpoint").state_bytes, 0U);
    // The same distances whatever the segment size.
    EXPECT_EQ(encoding(whole, "day_distance_index").column_bytes,
              encoding(whole, "day_distance_checkpoint").column_bytes);
}

TEST(ReferenceEval, AnEmptyStreamCostsNothing) {
    const auto result = evaluate_references({}, {.segment_records = {4}, .checkpoint_segment_records = {}});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->records, 0U);
    EXPECT_EQ(result->sizes[0].segments, 0U);
    for (const EncodingCost& cost : result->sizes[0].encodings) {
        EXPECT_EQ(cost.column_bytes + cost.state_bytes, 0U) << cost.name;
    }
}

// A source whose frames stop before its end is part of a day, and its figures would read as
// a whole day's.
TEST(ReferenceEval, RefusesASourceWhoseFramesStopBeforeItsEnd) {
    const Bytes clean = stream();
    Bytes cut = clean;
    cut.insert(cut.end(), {std::byte{0x00}, std::byte{0x30}, std::byte{'S'}});
    Bytes zero = clean;
    zero.insert(zero.end(), {std::byte{0x00}, std::byte{0x00}});
    zero.insert(zero.end(), clean.begin(), clean.end());
    Bytes one_byte = clean;
    one_byte.push_back(std::byte{0x00});
    for (const Bytes& source : {cut, zero, one_byte}) {
        const auto result = evaluate_references(source, {.segment_records = {4}, .checkpoint_segment_records = {}});
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_TRUE(result.error().contains("stop before its end")) << result.error();
        }
    }
}

TEST(ReferenceEval, RefusesACheckpointSizeThatIsNotASegmentSize) {
    const Bytes source = stream();
    const auto result =
        evaluate_references(source, {.segment_records = {2}, .checkpoint_segment_records = {2, 100}});
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().contains("100")) << result.error();
}

// The JSON is smaller than the stream's buffer, so its one write comes at close. A failure
// there has to reach the exit code, not be lost in a destructor.
TEST(ReferenceEval, AFailedWriteOfTheResultsIsAnError) {
    const tick_store::test::ScratchDir dir;
    tick_store::test::write_file(dir / "source", stream());
    const std::string source_path = (dir / "source").string();
    const std::string output_path = (dir / "out.json").string();
    const EvalProvenance provenance{.commit = "0123456789abcdef", .tree_dirty = false,
                                    .compiler = "test", .build_type = "Debug", .sanitizers = ""};
    const std::vector<std::string_view> arguments{source_path, output_path, "--day", "T",
                                                  "--segment-records", "2"};
    CliOutput output;
    int code = kExitPass;
    {
        const tick_store::test::FileSizeLimit limit{16};
        code = run_reference_eval_cli(arguments, provenance, output);
    }
    EXPECT_EQ(code, kExitUsageOrIo);
    EXPECT_TRUE(output.err.contains("cannot write the output")) << output.err;
    EXPECT_LE(std::filesystem::file_size(dir / "out.json"), 16U);
}

TEST(ReferenceEval, TheCommandLineWritesJsonWithoutPathsAndRefusesBadUsage) {
    const tick_store::test::ScratchDir dir;
    const Bytes source = stream();
    tick_store::test::write_file(dir / "source", source);
    // Named, because the argument lists below hold string_views into them.
    const std::string source_path = (dir / "source").string();
    const std::string output_path = (dir / "out.json").string();
    const std::string absent_path = (dir / "absent").string();
    const std::string no_dir_path = (dir / "no" / "dir" / "out.json").string();
    const EvalProvenance provenance{.commit = "0123456789abcdef", .tree_dirty = false,
                                    .compiler = "test", .build_type = "Debug", .sanitizers = ""};
    const auto run = [&](std::vector<std::string_view> arguments, CliOutput& output) {
        return run_reference_eval_cli(arguments, provenance, output);
    };
    CliOutput output;
    ASSERT_EQ(run({source_path, output_path, "--day", "T", "--segment-records", "2,100",
                   "--checkpoint-segment-records", "2"},
                  output),
              kExitPass)
        << output.err;
    std::ifstream in{dir / "out.json"};
    const std::string json{std::istreambuf_iterator<char>{in}, {}};
    EXPECT_TRUE(json.contains(R"("references": 5)"));
    EXPECT_TRUE(json.contains(R"("commit": "0123456789ab")"));
    EXPECT_TRUE(json.contains(R"("name": "within_stock_imports")"));
    EXPECT_FALSE(json.contains(dir.path().string()));
    EXPECT_TRUE(output.out.contains("N=2 within_stock_imports"));
    // At N = 100 the checkpoint was not asked for: its column alone is no cost, so neither a
    // total nor bits per reference is given for it.
    const std::size_t whole = json.find(R"("segment_records": 100,)");
    ASSERT_NE(whole, std::string::npos);
    const std::size_t unmeasured = json.find(R"("name": "day_distance_checkpoint")", whole);
    ASSERT_NE(unmeasured, std::string::npos);
    const std::string row = json.substr(unmeasured, json.find('}', unmeasured) - unmeasured);
    EXPECT_TRUE(row.contains(R"("measured": false)")) << row;
    EXPECT_TRUE(row.contains(R"("total_bytes": null)")) << row;
    EXPECT_TRUE(row.contains(R"("bits_per_reference": null)")) << row;

    const std::vector<std::vector<std::string_view>> bad{
        {source_path, output_path},
        {source_path, output_path, "--day", "T"},
        {source_path, output_path, "--segment-records", "2", "--day"},
        {source_path, output_path, "--day", "T", "--segment-records", "0"},
        {source_path, output_path, "--day", "T", "--segment-records", "2,"},
        {source_path, output_path, "--day", "T", "--segment-records", "16777217"},
        {source_path, output_path, "--day", "T", "--segment-records", "2", "--other", "1"},
        {source_path, output_path, "--day", "T", "--segment-records", "2",
         "--checkpoint-segment-records", "x"},
        // A flag given twice is refused, never "the last one wins".
        {source_path, output_path, "--day", "T", "--day", "U", "--segment-records", "2"},
        {source_path, output_path, "--day", "T", "--segment-records", "2", "--segment-records",
         "4"},
        {source_path, output_path, "--day", "T", "--segment-records", "2",
         "--checkpoint-segment-records", "2", "--checkpoint-segment-records", "2"},
        // A checkpoint size that is not a segment size would measure nothing.
        {source_path, output_path, "--day", "T", "--segment-records", "2",
         "--checkpoint-segment-records", "100"},
        {absent_path, output_path, "--day", "T", "--segment-records", "2"},
        {source_path, no_dir_path, "--day", "T", "--segment-records", "2"},
    };
    for (const auto& arguments : bad) {
        CliOutput refused;
        EXPECT_EQ(run(arguments, refused), kExitUsageOrIo);
        EXPECT_FALSE(refused.err.empty());
    }
}

}  // namespace
}  // namespace tick_store::harness
