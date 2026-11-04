// The tick_roundtrip command line, end to end on real files: exit codes, the summary
// and the failure report. No test starts a process, so each path runs under the
// sanitizers.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/stub_store.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::harness {
namespace {

using test::Bytes;
using test::ScratchDir;
using test::write_file;

struct CliRun {
    int code = -1;
    CliOutput output;
};

CliRun run(std::initializer_list<std::string> arguments) {
    const std::vector<std::string> owned{arguments};
    const std::vector<std::string_view> views{owned.begin(), owned.end()};
    CliRun result;
    result.code = run_roundtrip_cli(views, result.output);
    return result;
}

bool contains(std::string_view text, std::string_view part) {
    return text.contains(part);
}

class RoundTripCli : public ::testing::Test {
protected:
    RoundTripCli() {
        write_file(source_path, source);
        const CliRun ingest = run({"ingest-stub", source_path.string(), store_path.string()});
        EXPECT_EQ(ingest.code, kExitPass) << ingest.output.err;
    }

    ScratchDir dir;
    Bytes source = test::source_with_tail();
    std::filesystem::path source_path = dir / "source";
    std::filesystem::path store_path = dir / "store";
    std::string pin = to_hex(sha256(source));
};

TEST_F(RoundTripCli, IngestReportsWhatItStored) {
    const CliRun ingest = run({"ingest-stub", source_path.string(), store_path.string()});
    EXPECT_EQ(ingest.code, kExitPass);
    EXPECT_TRUE(contains(ingest.output.out, R"("records": 26)"));
    EXPECT_TRUE(contains(ingest.output.out, R"("tail_bytes": 4)"));
    EXPECT_TRUE(contains(ingest.output.out, R"("raw_unknown_type": 2)"));
    EXPECT_TRUE(contains(ingest.output.out, R"("raw_wrong_length": 1)"));
    EXPECT_FALSE(contains(ingest.output.out, dir.path().string()));
}

TEST_F(RoundTripCli, VerifyPassesWithExitZeroAndASummary) {
    const CliRun verify = run({"verify", source_path.string(), store_path.string()});
    EXPECT_EQ(verify.code, kExitPass) << verify.output.err;
    EXPECT_TRUE(contains(verify.output.out, R"("verdict": "pass")"));
    EXPECT_TRUE(contains(verify.output.out, R"("records": 26)"));
    EXPECT_TRUE(contains(verify.output.out, std::format(R"("bytes": {})", source.size())));
    EXPECT_TRUE(contains(verify.output.out, std::format(R"("source_sha256": "{}")", pin)));
    EXPECT_TRUE(contains(verify.output.out, std::format(R"("stored_sha256": "{}")", pin)));
    EXPECT_TRUE(contains(verify.output.out, R"("wall_seconds": )"));
    EXPECT_FALSE(contains(verify.output.out, dir.path().string()));
    EXPECT_TRUE(verify.output.err.empty());
}

TEST_F(RoundTripCli, VerifyAgainstTheRightPinPasses) {
    const CliRun verify =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(verify.code, kExitPass) << verify.output.err;
    EXPECT_TRUE(contains(verify.output.out, std::format(R"("expected_sha256": "{}")", pin)));
}

TEST_F(RoundTripCli, VerifyAgainstAWrongPinExitsOne) {
    const std::string wrong(64, '0');
    const CliRun verify =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", wrong});
    EXPECT_EQ(verify.code, kExitMismatch);
    EXPECT_TRUE(contains(verify.output.out, R"("verdict": "mismatch")"));
    EXPECT_TRUE(contains(verify.output.err, "does not match the expected SHA-256"));
}

TEST_F(RoundTripCli, AFlippedSourceCopyFailsAgainstThePinWithAHashMismatch) {
    Bytes copy = source;
    copy[100] ^= std::byte{0x04};
    write_file(dir / "copy", copy);
    const CliRun verify =
        run({"verify", (dir / "copy").string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(verify.code, kExitMismatch);
    EXPECT_TRUE(contains(verify.output.out, R"("source_matches_expected": false)"));
    EXPECT_TRUE(contains(verify.output.err, "the source does not match the expected SHA-256"));
}

// G-2 as the real run does it: flip on disk, fail with the location, flip back, pass.
TEST_F(RoundTripCli, FlipThenVerifyNamesTheLocationAndFlippingBackRestoresThePass) {
    const std::size_t source_offset = 100;
    const std::size_t store_offset = store::kStubHeaderSize + source_offset;
    const unsigned bit = 5;
    const std::string offset_text = std::to_string(store_offset);
    const std::string bit_text = std::to_string(bit);

    const CliRun flip = run({"flip-bit", store_path.string(), offset_text, bit_text});
    ASSERT_EQ(flip.code, kExitPass) << flip.output.err;

    const CliRun failed =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(failed.code, kExitMismatch);
    EXPECT_TRUE(contains(failed.output.out, R"("mismatched_records": 1)"));
    EXPECT_TRUE(contains(failed.output.out,
                         std::format(R"("source_byte_offset": {})", source_offset)));
    EXPECT_TRUE(contains(failed.output.out, std::format(R"("bit": {})", bit)));
    EXPECT_TRUE(contains(failed.output.err, std::format("source byte {}, bit {}", source_offset, bit)));

    const CliRun back = run({"flip-bit", store_path.string(), offset_text, bit_text});
    ASSERT_EQ(back.code, kExitPass) << back.output.err;
    const CliRun passed =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(passed.code, kExitPass) << passed.output.err;
}

TEST_F(RoundTripCli, AStoreItsParserRefusesExitsOne) {
    const CliRun flip = run({"flip-bit", store_path.string(), "0", "0"});
    ASSERT_EQ(flip.code, kExitPass);
    const CliRun verify = run({"verify", source_path.string(), store_path.string()});
    EXPECT_EQ(verify.code, kExitMismatch);
    EXPECT_TRUE(contains(verify.output.out, R"("kind": "store_error")"));
    EXPECT_TRUE(contains(verify.output.err, "bad magic"));
}

TEST_F(RoundTripCli, AnEmptyStoreFileExitsOne) {
    write_file(dir / "empty", {});
    const CliRun verify = run({"verify", source_path.string(), (dir / "empty").string()});
    EXPECT_EQ(verify.code, kExitMismatch);
    EXPECT_TRUE(contains(verify.output.err, "file too small"));
}

TEST_F(RoundTripCli, FailedIoExitsTwo) {
    const std::string absent = (dir / "absent").string();
    EXPECT_EQ(run({"verify", absent, store_path.string()}).code, kExitUsageOrIo);
    EXPECT_EQ(run({"verify", source_path.string(), absent}).code, kExitUsageOrIo);
    EXPECT_EQ(run({"ingest-stub", absent, store_path.string()}).code, kExitUsageOrIo);
    EXPECT_EQ(run({"ingest-stub", source_path.string(), (dir / "no" / "dir").string()}).code,
              kExitUsageOrIo);
    EXPECT_EQ(run({"ingest", absent, store_path.string(), "--durability", "page_cache"}).code,
              kExitUsageOrIo);
    EXPECT_EQ(run({"ingest", source_path.string(), (dir / "no" / "dir").string(), "--durability",
                   "page_cache"})
                  .code,
              kExitUsageOrIo);
    EXPECT_EQ(run({"flip-bit", absent, "0", "0"}).code, kExitUsageOrIo);
    EXPECT_EQ(run({"flip-bit", store_path.string(), "999999", "0"}).code, kExitUsageOrIo);
}

TEST_F(RoundTripCli, BadUsageExitsTwoAndPrintsUsage) {
    const std::string source_text = source_path.string();
    const std::string store_text = store_path.string();
    const std::vector<std::vector<std::string>> cases{
        {},
        {"frobnicate"},
        {"ingest", source_text},
        {"ingest", source_text, store_text},
        {"ingest", source_text, store_text, "more"},
        {"ingest", source_text, store_text, "--durability"},
        {"ingest", source_text, store_text, "--durability", "sometimes"},
        {"ingest", source_text, store_text, "--segment-records", "16"},
        {"ingest", source_text, store_text, "--durability", "page_cache", "--segment-records",
         "0"},
        {"ingest", source_text, store_text, "--durability", "page_cache", "--segment-records",
         "16777217"},
        {"ingest", source_text, store_text, "--durability", "page_cache", "--segment-records",
         "x"},
        {"ingest-stub", source_text},
        {"ingest-stub", source_text, store_text, "more"},
        {"verify", source_text, store_text, "--no-checksums", "--no-checksums"},
        {"layout"},
        {"layout", store_text, "--other"},
        {"verify", source_text},
        {"verify", source_text, store_text, "--expect-sha256"},
        {"verify", source_text, store_text, "--expect-sha256", "not-hex"},
        {"verify", source_text, store_text, "--expect-sha256", pin.substr(1)},
        {"verify", source_text, store_text, "--unknown", pin},
        {"flip-bit", store_text},
        {"flip-bit", store_text, "x", "0"},
        {"flip-bit", store_text, "0", "8"},
        {"flip-bit", store_text, "0", "-1"},
        {"flip-bit", store_text, "-1", "0"},
        {"flip-bit", store_text, "1x", "0"},
    };
    for (const auto& arguments : cases) {
        const std::vector<std::string_view> views{arguments.begin(), arguments.end()};
        CliOutput output;
        EXPECT_EQ(run_roundtrip_cli(views, output), kExitUsageOrIo);
        EXPECT_TRUE(contains(output.err, "usage: tick_roundtrip"));
    }
}


// The real store: ingest with its durability class, verify with and without CRCs, and the
// G-2 drill the full days run: a flip in a data block and one in the footer.
class RoundTripCliStore : public ::testing::Test {
protected:
    RoundTripCliStore() {
        write_file(source_path, source);
        const CliRun ingest = run({"ingest", source_path.string(), store_path.string(),
                                   "--durability", "page_cache", "--segment-records", "8"});
        EXPECT_EQ(ingest.code, kExitPass) << ingest.output.err;
    }

    // The file offset of segment `segment`'s block of `column`, and its size.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> block_extent(std::uint64_t segment,
                                                                       store::Column column) {
        const auto reader = store::ItchStoreReader::open(store_path);
        EXPECT_TRUE(reader.has_value());
        const std::uint64_t block = reader->segment(segment).block[static_cast<std::size_t>(column)];
        const auto info = reader->container().block_info(block);
        EXPECT_TRUE(info.has_value());
        return {info->payload_offset, info->payload_size};
    }

    [[nodiscard]] std::uint64_t footer_offset() {
        const auto reader = store::ItchStoreReader::open(store_path);
        EXPECT_TRUE(reader.has_value());
        return reader->container().footer_offset();
    }

    ScratchDir dir;
    Bytes source = test::source_with_tail();
    std::filesystem::path source_path = dir / "source";
    std::filesystem::path store_path = dir / "store";
    std::string pin = to_hex(sha256(source));
};

TEST_F(RoundTripCliStore, IngestReportsTheStoreItWrote) {
    const CliRun ingest = run({"ingest", source_path.string(), store_path.string(),
                               "--durability", "device_flush"});
    ASSERT_EQ(ingest.code, kExitPass) << ingest.output.err;
    EXPECT_TRUE(contains(ingest.output.out, R"("store_format": "itch-store-v1")"));
    EXPECT_TRUE(contains(ingest.output.out, R"("durability": "device_flush")"));
    EXPECT_TRUE(contains(ingest.output.out,
                         std::format(R"("segment_records": {})", store::kDefaultSegmentRecords)));
    EXPECT_TRUE(contains(ingest.output.out, R"("records": 26)"));
    EXPECT_TRUE(contains(ingest.output.out, R"("tail_bytes": 4)"));
    EXPECT_TRUE(contains(ingest.output.out, R"("raw_unknown_type": 2)"));
    EXPECT_TRUE(contains(ingest.output.out, R"("raw_wrong_length": 1)"));
    EXPECT_TRUE(contains(ingest.output.out, R"("ratio": )"));
    EXPECT_TRUE(contains(ingest.output.out, R"("peak_rss_bytes": )"));
    EXPECT_FALSE(contains(ingest.output.out, dir.path().string()));
}

TEST_F(RoundTripCliStore, VerifyPassesAgainstThePinWithAndWithoutChecksums) {
    const CliRun verify =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(verify.code, kExitPass) << verify.output.err;
    EXPECT_TRUE(contains(verify.output.out, R"("store_format": "itch-store-v1")"));
    EXPECT_TRUE(contains(verify.output.out, R"("checksums": "verify")"));
    EXPECT_TRUE(contains(verify.output.out, R"("records": 26)"));
    EXPECT_TRUE(contains(verify.output.out, std::format(R"("stored_sha256": "{}")", pin)));

    const CliRun unchecked = run({"verify", source_path.string(), store_path.string(),
                                  "--no-checksums", "--expect-sha256", pin});
    EXPECT_EQ(unchecked.code, kExitPass) << unchecked.output.err;
    EXPECT_TRUE(contains(unchecked.output.out, R"("checksums": "skip")"));
}

TEST_F(RoundTripCliStore, ADataBlockFlipIsACrcFailureNamingTheBlockAndWithoutCrcsTheRecord) {
    // Segment 0's timestamps: a flip there changes a value, never the layout.
    const auto [offset, size] = block_extent(0, store::Column::timestamp);
    ASSERT_GT(size, 8U);
    const std::string offset_text = std::to_string(offset + size - 1);
    ASSERT_EQ(run({"flip-bit", store_path.string(), offset_text, "0"}).code, kExitPass);

    const CliRun checked =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(checked.code, kExitMismatch);
    EXPECT_TRUE(contains(checked.output.err, "bad checksum: block at offset"));
    EXPECT_TRUE(contains(checked.output.err, std::format("store byte {}", offset)));
    EXPECT_TRUE(contains(checked.output.err, "segment 0, column timestamp, block "));

    const CliRun unchecked = run({"verify", source_path.string(), store_path.string(),
                                  "--no-checksums", "--expect-sha256", pin});
    EXPECT_EQ(unchecked.code, kExitMismatch);
    // The last timestamp of the block no longer agrees with the segment's summary, so the
    // decoder names the column and the value's index in it.
    EXPECT_TRUE(contains(unchecked.output.err, "segment 0, column timestamp, value "))
        << unchecked.output.err;

    ASSERT_EQ(run({"flip-bit", store_path.string(), offset_text, "0"}).code, kExitPass);
    const CliRun passed =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(passed.code, kExitPass) << passed.output.err;
}

TEST_F(RoundTripCliStore, AFooterFlipIsRefusedAtOpenAndFlippingBackRestoresThePass) {
    const std::string offset_text = std::to_string(footer_offset() + 20);
    ASSERT_EQ(run({"flip-bit", store_path.string(), offset_text, "3"}).code, kExitPass);
    const CliRun failed =
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin});
    EXPECT_EQ(failed.code, kExitMismatch);
    EXPECT_TRUE(contains(failed.output.out, R"("kind": "store_error")"));
    EXPECT_TRUE(contains(failed.output.err, "bad checksum: footer"));
    ASSERT_EQ(run({"flip-bit", store_path.string(), offset_text, "3"}).code, kExitPass);
    EXPECT_EQ(
        run({"verify", source_path.string(), store_path.string(), "--expect-sha256", pin}).code,
        kExitPass);
}

TEST_F(RoundTripCliStore, LayoutCountsEveryByteOfTheFile) {
    const CliRun layout = run({"layout", store_path.string(), "--blocks"});
    ASSERT_EQ(layout.code, kExitPass) << layout.output.err;
    EXPECT_TRUE(contains(layout.output.out, R"("records": 26)"));
    EXPECT_TRUE(contains(layout.output.out, R"("segment_records": 8)"));
    EXPECT_TRUE(contains(layout.output.out, R"("segments": 4)"));
    EXPECT_TRUE(contains(layout.output.out, R"("column": "timestamp")"));
    EXPECT_TRUE(contains(layout.output.out, R"("block_list": [)"));
    EXPECT_TRUE(contains(layout.output.out,
                         std::format(R"("file_bytes": {})", std::filesystem::file_size(store_path))));
    EXPECT_FALSE(contains(layout.output.out, dir.path().string()));

    write_file(dir / "empty", {});
    const CliRun refused = run({"layout", (dir / "empty").string()});
    EXPECT_EQ(refused.code, kExitMismatch);
    EXPECT_TRUE(contains(refused.output.err, "cannot open the store"));
}

TEST_F(RoundTripCliStore, TheStubHasNoChecksumsToSkip) {
    ASSERT_EQ(run({"ingest-stub", source_path.string(), (dir / "stub").string()}).code, kExitPass);
    const CliRun verify =
        run({"verify", source_path.string(), (dir / "stub").string(), "--no-checksums"});
    EXPECT_EQ(verify.code, kExitUsageOrIo);
    EXPECT_TRUE(contains(verify.output.err, "no checksums to skip"));
}

}  // namespace
}  // namespace tick_store::harness
