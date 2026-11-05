// tick_export and tick_roundtrip verify-columns, end to end on real files.

#include "columns_fixture.hpp"
#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/columns_reader.hpp"
#include "tick_store/harness/compare_tools_cli.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/mapped_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
using test::ColumnDirectory;
using test::ScratchDir;
using test::write_file;

struct CliRun {
    int code = -1;
    CliOutput output;
};

template <class Function>
CliRun run(Function function, std::initializer_list<std::string> arguments) {
    const std::vector<std::string> owned{arguments};
    const std::vector<std::string_view> views{owned.begin(), owned.end()};
    CliRun result;
    result.code = function(views, result.output);
    return result;
}

class ColumnsCli : public ::testing::Test {
protected:
    ColumnsCli() { write_file(source_path, source); }

    CliRun export_all() {
        return run(run_export_cli, {source_path.string(), directory.string()});
    }

    CliRun verify_all(std::initializer_list<std::string> extra = {}) {
        std::vector<std::string> owned{"verify-columns", source_path.string(), directory.string()};
        owned.insert(owned.end(), extra);
        const std::vector<std::string_view> views{owned.begin(), owned.end()};
        CliRun result;
        result.code = run_roundtrip_cli(views, result.output);
        return result;
    }

    ScratchDir scratch;
    std::filesystem::path source_path = scratch / "source";
    std::filesystem::path directory = scratch / "columns";
    Bytes source = test::source_with_tail();
};

TEST_F(ColumnsCli, ExportWritesEveryFileAndProvesThem) {
    const CliRun exported = export_all();
    ASSERT_EQ(exported.code, kExitPass) << exported.output.err;
    EXPECT_TRUE(exported.output.out.contains("\"verdict\": \"pass\""));
    EXPECT_TRUE(exported.output.out.contains("\"raw_rows\": 3"));
    EXPECT_TRUE(exported.output.out.contains("\"hot_rows\": 8"));
    EXPECT_TRUE(exported.output.out.contains("\"rare_rows\": 15"));
    EXPECT_TRUE(exported.output.out.contains("\"timestamps_sorted\":"));
    EXPECT_FALSE(exported.output.out.contains(scratch.path().string()));
    for (const std::string_view name : store::kColumnFileNames) {
        EXPECT_TRUE(std::filesystem::exists(directory / name)) << name;
    }
    EXPECT_TRUE(std::filesystem::exists(directory / store::kColumnsManifestName));
}

TEST_F(ColumnsCli, ExportRefusesToWriteOverADirectory) {
    ASSERT_EQ(export_all().code, kExitPass);
    const CliRun again = export_all();
    EXPECT_EQ(again.code, kExitUsageOrIo);
    EXPECT_TRUE(again.output.err.contains("cannot create the columns"));
}

TEST_F(ColumnsCli, ExportStatesItsUsageAndAMissingSource) {
    EXPECT_EQ(run(run_export_cli, {"only-one"}).code, kExitUsageOrIo);
    const CliRun missing = run(run_export_cli, {(scratch / "absent").string(), directory.string()});
    EXPECT_EQ(missing.code, kExitUsageOrIo);
    EXPECT_TRUE(missing.output.err.contains("cannot read the source"));
}

// The comparison harness reads tick-store back this way: the store's records as the column
// form, which must be the very files tick_export writes from the source, and pass the oracle.
TEST_F(ColumnsCli, ExportFromAStoreWritesWhatTheSourceExportWrites) {
    ASSERT_EQ(export_all().code, kExitPass);
    const std::filesystem::path store_path = scratch / "store.tsc";
    const std::vector<std::string> ingest{"ingest", source_path.string(), store_path.string(),
                                          "--durability", "page_cache", "--segment-records", "8"};
    const std::vector<std::string_view> views{ingest.begin(), ingest.end()};
    CliOutput ingested;
    ASSERT_EQ(run_roundtrip_cli(views, ingested), kExitPass) << ingested.err;

    const std::filesystem::path from_store = scratch / "from-store";
    const CliRun exported =
        run(run_export_cli, {"--store", store_path.string(), from_store.string()});
    ASSERT_EQ(exported.code, kExitPass) << exported.output.err;
    EXPECT_TRUE(exported.output.out.contains("\"command\": \"store\""));
    EXPECT_TRUE(exported.output.out.contains("\"records\": 26"));
    EXPECT_TRUE(exported.output.out.contains("\"tail_bytes\": 4"));
    EXPECT_FALSE(exported.output.out.contains(scratch.path().string()));
    std::vector<std::string> names{store::kColumnFileNames.begin(), store::kColumnFileNames.end()};
    names.emplace_back(store::kColumnsManifestName);
    for (const std::string& name : names) {
        const auto expected = store::MappedFile::open((directory / name).string());
        const auto actual = store::MappedFile::open((from_store / name).string());
        ASSERT_EQ(expected.has_value(), actual.has_value()) << name;
        if (expected.has_value()) {
            EXPECT_TRUE(std::ranges::equal(expected->bytes(), actual->bytes())) << name;
        }
    }
    const std::vector<std::string> verify{"verify-columns", source_path.string(),
                                          from_store.string()};
    const std::vector<std::string_view> verify_views{verify.begin(), verify.end()};
    CliOutput verified;
    EXPECT_EQ(run_roundtrip_cli(verify_views, verified), kExitPass) << verified.err;

    const CliRun refused = run(run_export_cli, {"--store", source_path.string(),
                                                (scratch / "not-a-store").string()});
    EXPECT_EQ(refused.code, kExitMismatch);
    EXPECT_TRUE(refused.output.err.contains("the store does not open"));
}

TEST_F(ColumnsCli, VerifyColumnsPassesAndChecksThePin) {
    ASSERT_EQ(export_all().code, kExitPass);
    const CliRun plain = verify_all();
    ASSERT_EQ(plain.code, kExitPass) << plain.output.err;
    EXPECT_TRUE(plain.output.out.contains("\"command\": \"verify-columns\""));
    EXPECT_TRUE(plain.output.out.contains("\"verdict\": \"pass\""));

    const std::string pin = to_hex(sha256(source));
    EXPECT_EQ(verify_all({"--expect-sha256", pin}).code, kExitPass);

    const std::string wrong(64, '0');
    const CliRun refused = verify_all({"--expect-sha256", wrong});
    EXPECT_EQ(refused.code, kExitMismatch);
    EXPECT_TRUE(refused.output.err.contains("does not match the expected SHA-256"));
    EXPECT_EQ(verify_all({"--expect-sha256", "nothex"}).code, kExitUsageOrIo);
}

TEST_F(ColumnsCli, VerifyColumnsReportsOneChangedValueWithItsRecord) {
    ASSERT_EQ(export_all().code, kExitPass);
    // Change the low bit of the first order reference there is.
    Bytes order_ref = test::read_file(directory / "order_ref.bin");
    std::size_t row = 0;
    while (order_ref[row * 8] == std::byte{0}) {
        ++row;
    }
    order_ref[row * 8] ^= std::byte{0x01};
    write_file(directory / "order_ref.bin", order_ref);

    const CliRun verified = verify_all();
    EXPECT_EQ(verified.code, kExitMismatch);
    EXPECT_TRUE(verified.output.out.contains("\"verdict\": \"mismatch\""));
    EXPECT_TRUE(verified.output.out.contains(std::format("\"record_index\": {}", row)));
    EXPECT_TRUE(verified.output.err.contains("first difference: bytes"));
}

TEST_F(ColumnsCli, VerifyColumnsRefusesADirectoryWithAMissingFileOrManifest) {
    ASSERT_EQ(export_all().code, kExitPass);
    std::filesystem::remove(directory / "price.bin");
    const CliRun no_file = verify_all();
    EXPECT_EQ(no_file.code, kExitMismatch);
    EXPECT_TRUE(no_file.output.out.contains("store_error"));
    EXPECT_TRUE(no_file.output.err.contains("store error"));

    std::filesystem::remove(directory / store::kColumnsManifestName);
    EXPECT_EQ(verify_all().code, kExitMismatch);
}

TEST_F(ColumnsCli, ASystemThatLostARowFailsLoudly) {
    // Every column consistently one row shorter, as a system dropping the last row
    // would give, so the parser has nothing to refuse and the oracle must see it.
    ColumnDirectory columns = test::export_columns(source);
    const store::ColumnsManifest manifest =
        *store::parse_columns_manifest(columns.manifest_bytes());
    Bytes shorter = source;
    const auto [frames, tail] = test::frames_of(source);
    shorter.resize(frames.back().offset);
    ColumnDirectory dropped = test::export_columns(shorter);
    dropped.files[test::index_of(store::ColumnFile::tail)] =
        columns.files[test::index_of(store::ColumnFile::tail)];
    store::ColumnsManifest fixed = *store::parse_columns_manifest(dropped.manifest_bytes());
    fixed.tail_bytes = manifest.tail_bytes;
    dropped.manifest = store::format_columns_manifest(fixed);
    dropped.write_to(directory);

    const CliRun verified = verify_all();
    EXPECT_EQ(verified.code, kExitMismatch);
    EXPECT_TRUE(verified.output.out.contains("\"missing_records\": 1"));
}

TEST_F(ColumnsCli, RowsOutOfOrderAreRefusedBySeq) {
    ColumnDirectory columns = test::export_columns(source);
    auto& seq = columns.files[test::index_of(store::ColumnFile::seq)];
    ASSERT_TRUE(store::write_le<8>(seq, 0, 1));
    ASSERT_TRUE(store::write_le<8>(seq, 8, 0));
    columns.write_to(directory);
    const CliRun verified = verify_all();
    EXPECT_EQ(verified.code, kExitMismatch);
    EXPECT_TRUE(verified.output.err.contains("seq.bin"));
}

TEST_F(ColumnsCli, ReaderOpensAWrittenDirectoryAndStatesWhyItCannot) {
    EXPECT_FALSE(ColumnsReader::open(directory).has_value());
    ASSERT_EQ(export_all().code, kExitPass);
    auto reader = ColumnsReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const auto source_map = store::MappedFile::open(source_path);
    ASSERT_TRUE(source_map.has_value());
    EXPECT_TRUE(verify(source_map->bytes(), **reader).passed());
}

}  // namespace
}  // namespace tick_store::harness
