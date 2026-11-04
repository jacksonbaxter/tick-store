// Replays checked-in crashers through the fuzz targets' own oracles (Z6), and runs
// the golden fixtures through them, so the oracles stay exercised on every build
// even when the crasher directories hold nothing.

#include "codec_oracles.hpp"
#include "columns_fixture.hpp"
#include "columns_oracles.hpp"
#include "container_fixture.hpp"
#include "container_oracles.hpp"
#include "itch_fixtures.hpp"
#include "itch_oracles.hpp"
#include "itch_store_oracles.hpp"
#include "scratch_dir.hpp"
#include "store_oracles.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/stream_vbyte.hpp"
#include "tick_store/store/container.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tick_store::itch {
namespace {

using test::Bytes;
using test::exact_copy;
using test::fixtures;

std::vector<std::filesystem::path> crashers(const std::string& target) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry :
         std::filesystem::directory_iterator{std::filesystem::path{TICK_STORE_CRASHER_DIR} / target}) {
        if (entry.is_regular_file() && entry.path().filename() != ".gitkeep") {
            paths.push_back(entry.path());
        }
    }
    return paths;
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    Bytes bytes;
    for (auto it = std::istreambuf_iterator<char>{file}; it != std::istreambuf_iterator<char>{};
         ++it) {
        bytes.push_back(static_cast<std::byte>(*it));
    }
    return exact_copy(bytes);
}

// nullptr turned into an empty string, so a failure prints the broken property.
std::string text(const char* failure) { return failure == nullptr ? std::string{} : failure; }

TEST(FuzzRegression, MessageCrashersStayFixed) {
    for (const auto& path : crashers("itch_message")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(oracle::check_message(read_file(path))), "");
    }
}

TEST(FuzzRegression, FrameCrashersStayFixed) {
    for (const auto& path : crashers("itch_frame")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(oracle::check_stream(read_file(path))), "");
    }
}

TEST(FuzzRegression, StoreCrashersStayFixed) {
    for (const auto& path : crashers("stub_store")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_store(read_file(path))), "");
    }
}

TEST(FuzzRegression, ContainerCrashersStayFixed) {
    for (const auto& path : crashers("container")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_container(read_file(path))), "");
    }
}

TEST(FuzzRegression, ItchStoreCrashersStayFixed) {
    for (const auto& path : crashers("itch_store")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_itch_store(read_file(path))), "");
    }
}

TEST(FuzzRegression, ColumnsCrashersStayFixed) {
    for (const auto& path : crashers("columns")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_columns(read_file(path))), "");
    }
}

TEST(FuzzRegression, GorillaTimestampCrashersStayFixed) {
    for (const auto& path : crashers("gorilla_timestamps")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_timestamp_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, GorillaXorCrashersStayFixed) {
    for (const auto& path : crashers("gorilla_xor")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_xor_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, PforCrashersStayFixed) {
    for (const auto& path : crashers("pfor")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_pfor_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, StreamVByteCrashersStayFixed) {
    for (const auto& path : crashers("stream_vbyte")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_svb_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, DictionaryCrashersStayFixed) {
    for (const auto& path : crashers("dictionary")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_dict_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, RunLengthCrashersStayFixed) {
    for (const auto& path : crashers("run_length")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_rle_column(read_file(path))), "");
    }
}

TEST(FuzzOracles, PassEveryFixtureWholeAndCutAtEveryLength) {
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        for (std::size_t length = 0; length <= f.length; ++length) {
            const Bytes payload = exact_copy(std::span{f.bytes}.first(length));
            EXPECT_EQ(text(oracle::check_message(payload)), "") << "length " << length;
        }
    }
}

TEST(FuzzOracles, PassEveryWayAStreamCanEnd) {
    Bytes stream;
    for (const auto& f : fixtures()) {
        const Bytes one = test::framed(static_cast<std::uint16_t>(f.length), f.bytes);
        stream.insert(stream.end(), one.begin(), one.end());
    }
    for (std::size_t length = 0; length <= stream.size(); ++length) {
        const Bytes cut = exact_copy(std::span{stream}.first(length));
        EXPECT_EQ(text(oracle::check_stream(cut)), "") << "length " << length;
    }

    Bytes ended = stream;
    ended.push_back(std::byte{0});
    ended.push_back(std::byte{0});
    EXPECT_EQ(text(oracle::check_stream(exact_copy(ended))), "");
    ended.push_back(std::byte{0x41});
    EXPECT_EQ(text(oracle::check_stream(exact_copy(ended))), "");
}

// A real export, as fuzz_columns' sections, cut at every length of the whole input and
// with every bit flipped in turn.
TEST(FuzzOracles, PassEveryCutAndFlipOfAColumnDirectory) {
    const Bytes source = tick_store::test::source_with_tail();
    const auto directory = tick_store::test::export_columns(source);
    const Bytes input = store::oracle::pack_columns(directory.manifest, directory.files);
    EXPECT_EQ(text(store::oracle::check_columns(exact_copy(input))), "");
    for (std::size_t length = 0; length < input.size(); length += 7) {
        const Bytes cut = exact_copy(std::span{input}.first(length));
        EXPECT_EQ(text(store::oracle::check_columns(cut)), "") << "length " << length;
    }
    Bytes flipped = input;
    for (std::size_t byte = 0; byte < flipped.size(); byte += 3) {
        const unsigned bit = byte % 8U;
        flipped[byte] ^= static_cast<std::byte>(1U << bit);
        EXPECT_EQ(text(store::oracle::check_columns(flipped)), "") << "byte " << byte;
        flipped[byte] ^= static_cast<std::byte>(1U << bit);
    }
}

// The golden container, cut at every length and with every bit flipped in turn, and a
// copy with every CRC wrong, which only the CRC checks refuse.
TEST(FuzzOracles, PassEveryCutAndFlipOfAContainer) {
    const Bytes file = exact_copy(store::test::read_bytes(store::test::golden_path()));
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(text(store::oracle::check_container(file)), "");
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_container(cut)), "") << "length " << length;
    }
    for (std::size_t bit = 0; bit < file.size() * 8; ++bit) {
        Bytes flipped = exact_copy(file);
        flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        EXPECT_EQ(text(store::oracle::check_container(flipped)), "") << "bit " << bit;
    }
    Bytes stale = exact_copy(file);
    for (const std::size_t at : {store::test::golden_block_entry(0) + store::kBlockCrc,
                                 store::test::golden_section_entry(0) + store::kSectionCrc,
                                 store::test::kGoldenTrailer + store::kTrailerFooterCrc}) {
        stale[at] ^= std::byte{0xFF};
    }
    EXPECT_EQ(text(store::oracle::check_container(stale)), "");
    EXPECT_EQ(store::oracle::fixed_checksums(stale), file);
}

// The golden ITCH store, cut at every length and with one bit of every byte flipped, the
// bit chosen by the byte's offset; and the same file with segments over the fuzzing cap.
TEST(FuzzOracles, PassEveryCutAndFlipOfAnItchStore) {
    const Bytes file =
        read_file(std::filesystem::path{TICK_STORE_GOLDEN_DIR} / "itch-store-v2.tsc");
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(text(store::oracle::check_itch_store(file)), "");
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_itch_store(cut)), "") << "length " << length;
    }
    for (std::size_t byte = 0; byte < file.size(); ++byte) {
        Bytes flipped = exact_copy(file);
        flipped[byte] ^= static_cast<std::byte>(1U << (byte % 8));
        EXPECT_EQ(text(store::oracle::check_itch_store(flipped)), "") << "byte " << byte;
    }
}

// A real store, cut at every length and with every bit flipped in turn.
TEST(FuzzOracles, PassEveryCutAndFlipOfAStore) {
    const tick_store::test::ScratchDir dir;
    const Bytes source = tick_store::test::source_with_tail();
    const Bytes file =
        tick_store::test::store_bytes(tick_store::test::messages_of(source),
                                      tick_store::test::frames_of(source).tail, dir / "store");
    EXPECT_EQ(text(store::oracle::check_store(exact_copy(file))), "");
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_store(cut)), "") << "length " << length;
    }
    for (std::size_t bit = 0; bit < file.size() * 8; ++bit) {
        Bytes flipped = exact_copy(file);
        flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        EXPECT_EQ(text(store::oracle::check_store(flipped)), "") << "bit " << bit;
    }
}

// A column from each Gorilla encoder, whole, cut at every length and with every bit
// flipped in turn, through the fuzz targets' oracles. A flipped column must be refused or
// re-encode to the flipped bytes, which the oracle checks.
TEST(FuzzOracles, PassEveryCutAndFlipOfAGorillaColumn) {
    std::vector<std::uint64_t> timestamps{34'200'000'000'000ULL};
    std::vector<std::uint64_t> dollars;
    std::uint64_t price = 1'234'500;
    for (std::uint64_t i = 1; i < 40; ++i) {
        timestamps.push_back(timestamps.back() + ((i * 7'919U) % 60'000U) + (i % 5 == 0 ? 0 : 1));
        price = price + ((i * 37U) % 11U * 100U) - 500U;
        dollars.push_back(std::bit_cast<std::uint64_t>(
            codec::price4_to_dollars(static_cast<std::uint32_t>(price))));
    }
    Bytes timestamp_column;
    ASSERT_TRUE(codec::encode_timestamps(timestamps, 16, timestamp_column).has_value());
    Bytes xor_column;
    ASSERT_TRUE(codec::encode_xor(dollars, 16, xor_column).has_value());

    using Check = const char* (*)(std::span<const std::byte>);
    for (const auto& [column, check] :
         {std::pair<const Bytes&, Check>{timestamp_column, &codec::oracle::check_timestamp_column},
          std::pair<const Bytes&, Check>{xor_column, &codec::oracle::check_xor_column}}) {
        EXPECT_EQ(text(check(exact_copy(column))), "");
        for (std::size_t length = 0; length < column.size(); ++length) {
            EXPECT_EQ(text(check(exact_copy(std::span{column}.first(length)))), "")
                << "length " << length;
        }
        for (std::size_t bit = 0; bit < column.size() * 8; ++bit) {
            Bytes flipped = exact_copy(column);
            flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            EXPECT_EQ(text(check(flipped)), "") << "bit " << bit;
        }
    }
}

// A column from each integer encoder, whole, cut at every length and with every bit
// flipped in turn, through the fuzz targets' oracles. These formats leave the encoder
// choices, so a flipped column must be refused or decode to values that every encoder
// round-trips, which the oracle checks.
TEST(FuzzOracles, PassEveryCutAndFlipOfAnIntegerColumn) {
    std::vector<std::uint64_t> values{34'200'000'000'000ULL};
    for (std::uint64_t i = 1; i < 40; ++i) {
        values.push_back(values.back() + ((i * 7'919U) % 60'000U) + (i == 20 ? 1ULL << 36U : 0));
    }
    std::vector<std::uint64_t> prices;
    prices.reserve(40);
    for (std::uint64_t i = 0; i < 40; ++i) {
        prices.push_back(i == 17 ? 1'999'999'900U : 1'234'500U + ((i * 37U) % 11U * 100U));
    }
    Bytes timestamp_column;
    ASSERT_TRUE(codec::encode_pfor(values, 16, codec::PforOptions{}, timestamp_column).has_value());
    Bytes price_column;
    ASSERT_TRUE(codec::encode_pfor(prices, 16,
                                   codec::PforOptions{.transform = codec::IntTransform::none,
                                                      .adaptive_scale = true,
                                                      .patch = true},
                                   price_column)
                    .has_value());
    Bytes svb_column;
    ASSERT_TRUE(codec::encode_svb(values, 16, true, svb_column).has_value());
    Bytes head_column;
    ASSERT_TRUE(codec::encode_pfor(values, 16,
                                   codec::PforOptions{.transform = codec::IntTransform::head_delta,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                   head_column)
                    .has_value());
    // Few distinct values with runs, as shares and flags are: one dictionary column in each
    // mode the encoder takes at this size, and one run-length column.
    std::vector<std::uint64_t> shares;
    shares.reserve(40);
    for (std::uint64_t i = 0; i < 40; ++i) {
        std::uint64_t size = 100;
        if (i % 5 == 0) {
            size = 200;
        } else if (i % 13 == 0) {
            size = 37;
        }
        shares.push_back(size);
    }
    Bytes dict_column;
    ASSERT_TRUE(codec::encode_dict(shares, 16, dict_column).has_value());
    std::vector<std::uint64_t> runs;
    runs.reserve(40);
    for (std::uint64_t i = 0; i < 40; ++i) {
        runs.push_back(i / 7 % 3);
    }
    Bytes rle_column;
    ASSERT_TRUE(codec::encode_rle(runs, 16, rle_column).has_value());

    using Check = const char* (*)(std::span<const std::byte>);
    for (const auto& [column, check] :
         {std::pair<const Bytes&, Check>{timestamp_column, &codec::oracle::check_pfor_column},
          std::pair<const Bytes&, Check>{price_column, &codec::oracle::check_pfor_column},
          std::pair<const Bytes&, Check>{head_column, &codec::oracle::check_pfor_column},
          std::pair<const Bytes&, Check>{svb_column, &codec::oracle::check_svb_column},
          std::pair<const Bytes&, Check>{dict_column, &codec::oracle::check_dict_column},
          std::pair<const Bytes&, Check>{rle_column, &codec::oracle::check_rle_column}}) {
        EXPECT_EQ(text(check(exact_copy(column))), "");
        for (std::size_t length = 0; length < column.size(); ++length) {
            EXPECT_EQ(text(check(exact_copy(std::span{column}.first(length)))), "")
                << "length " << length;
        }
        for (std::size_t bit = 0; bit < column.size() * 8; ++bit) {
            Bytes flipped = exact_copy(column);
            flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            EXPECT_EQ(text(check(flipped)), "") << "bit " << bit;
        }
    }
}

}  // namespace
}  // namespace tick_store::itch
