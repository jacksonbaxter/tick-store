// The tick_roundtrip command line, end to end on real files: exit codes, the summary
// and the failure report. No test starts a process, so each path runs under the
// sanitizers.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/store/stub_store.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
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
        const CliRun ingest = run({"ingest", source_path.string(), store_path.string()});
        EXPECT_EQ(ingest.code, kExitPass) << ingest.output.err;
    }

    ScratchDir dir;
    Bytes source = test::source_with_tail();
    std::filesystem::path source_path = dir / "source";
    std::filesystem::path store_path = dir / "store";
    std::string pin = to_hex(sha256(source));
};

TEST_F(RoundTripCli, IngestReportsWhatItStored) {
    const CliRun ingest = run({"ingest", source_path.string(), store_path.string()});
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
    EXPECT_EQ(run({"ingest", absent, store_path.string()}).code, kExitUsageOrIo);
    EXPECT_EQ(run({"ingest", source_path.string(), (dir / "no" / "dir").string()}).code,
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
        {"ingest", source_text, store_text, "more"},
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

}  // namespace
}  // namespace tick_store::harness
