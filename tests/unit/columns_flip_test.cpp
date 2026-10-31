// G-2 on the column form, exhaustively: every bit of every file of a real export, flipped
// in turn, must be refused by the parser or caught by the oracle. A bit that could be
// flipped undetected would be a value a system could return wrong and still pass, so
// nothing here is relaxed to suit the data. Runs in memory, because it runs the oracle
// once per bit.

#include "columns_fixture.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/store/columns.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <span>
#include <string>

namespace tick_store::harness {
namespace {

using test::Bytes;
using test::BufferReader;
using test::ColumnDirectory;

struct FlipCounts {
    std::uint64_t flips = 0;
    std::uint64_t refused_at_open = 0;
    std::uint64_t refused_in_a_row = 0;
    std::uint64_t caught_by_the_oracle = 0;
};

void check(const ColumnDirectory& directory, std::span<const std::byte> source, FlipCounts& counts) {
    ++counts.flips;
    const auto manifest = store::parse_columns_manifest(directory.manifest_bytes());
    if (!manifest.has_value()) {
        ++counts.refused_at_open;
        return;
    }
    const auto parser = store::ColumnsParser::open(*manifest, directory.bytes());
    if (!parser.has_value()) {
        ++counts.refused_at_open;
        return;
    }
    BufferReader reader{*parser};
    const VerifyReport report = verify(source, reader);
    ASSERT_FALSE(report.passed());
    if (report.store_error.has_value()) {
        ++counts.refused_in_a_row;
    } else {
        ++counts.caught_by_the_oracle;
    }
}

TEST(ColumnsFlip, EveryBitOfEveryFileIsCaught) {
    const Bytes source = test::source_with_tail();
    const ColumnDirectory clean = test::export_columns(source);
    FlipCounts counts;
    std::uint64_t bits = 0;

    ColumnDirectory directory = clean;
    for (std::size_t byte = 0; byte < directory.manifest.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            SCOPED_TRACE(std::format("manifest byte {} bit {}", byte, bit));
            directory.manifest[byte] = static_cast<char>(
                static_cast<unsigned char>(clean.manifest[byte]) ^ (1U << bit));
            check(directory, source, counts);
            ++bits;
        }
        directory.manifest[byte] = clean.manifest[byte];
    }

    for (std::size_t file = 0; file < store::kColumnFileCount; ++file) {
        for (std::size_t byte = 0; byte < directory.files[file].size(); ++byte) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                SCOPED_TRACE(std::format("{} byte {} bit {}", store::kColumnFileNames[file], byte, bit));
                directory.files[file][byte] = clean.files[file][byte] ^ static_cast<std::byte>(1U << bit);
                check(directory, source, counts);
                ++bits;
            }
            directory.files[file][byte] = clean.files[file][byte];
        }
    }

    // A silently empty loop cannot pass.
    EXPECT_GT(bits, 20'000U);
    EXPECT_EQ(counts.flips, bits);
    EXPECT_GT(counts.refused_at_open, 0U);
    EXPECT_GT(counts.refused_in_a_row, 0U);
    EXPECT_GT(counts.caught_by_the_oracle, 0U);
    std::println("flips {}: refused at open {}, refused in a row {}, caught by the oracle {}",
                 counts.flips, counts.refused_at_open, counts.refused_in_a_row,
                 counts.caught_by_the_oracle);

    // The same fixture with nothing flipped passes, so the failures above are the flips.
    const auto manifest = store::parse_columns_manifest(clean.manifest_bytes());
    ASSERT_TRUE(manifest.has_value());
    const auto parser = store::ColumnsParser::open(*manifest, clean.bytes());
    ASSERT_TRUE(parser.has_value());
    BufferReader reader{*parser};
    EXPECT_TRUE(verify(source, reader).passed());
}

}  // namespace
}  // namespace tick_store::harness
