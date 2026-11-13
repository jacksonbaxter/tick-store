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
#include "itch_query_oracles.hpp"
#include "itch_store_fixture.hpp"
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
#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

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

TEST(FuzzRegression, ItchQueryCrashersStayFixed) {
    for (const auto& path : crashers("itch_query")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_itch_query(read_file(path))), "");
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

// A frame whose prefix has both bytes set, 0x0101, and so both halves of the length
// read (review A-1). The oracle passes the cursor's stops on it; that it refuses a
// zero-length stop there, which would drop a whole frame, is shown by the test below.
TEST(FuzzOracles, PassAFrameOf257Bytes) {
    const Bytes payload(257, std::byte{0x41});
    Bytes stream = test::framed(0x0101, payload);
    EXPECT_EQ(text(oracle::check_stream(exact_copy(stream))), "");
    stream.push_back(std::byte{0x01});
    EXPECT_EQ(text(oracle::check_stream(exact_copy(stream))), "");
}

// Stops no correct cursor makes, each over the tail it claims to describe: every one must
// be refused, by the check named, and the right stop over the same tail accepted. The
// cursor is inline and constexpr, so only check_stop can be handed a wrong stop (review
// RA-1): without these, deleting a stop check passed every test.
TEST(FuzzOracles, TheFrameOracleRefusesAStopTheBytesDoNotJustify) {
    using enum FrameStopKind;
    Bytes whole = test::framed(0x0101, Bytes(257, std::byte{0x41}));
    const Bytes short_body = test::bytes({0x00, 0x04, 'x', 'x', 'x'});
    const Bytes whole_body = test::bytes({0x00, 0x04, 'x', 'x', 'x', 'x'});
    const Bytes zero = test::bytes({0x00, 0x00, 'x'});
    const Bytes one = test::bytes({0x01});
    const Bytes two = test::bytes({0x00, 0x04});
    const Bytes none;

    struct Case {
        const char* label;
        FrameStop stop;
        const Bytes& tail;
        std::string refusal;
    };
    const Case cases[] = {
        {"zero-length on 01 01", {zero_length, 0, 0, 257}, whole,
         "a zero-length stop and a zero prefix do not go together"},
        {"body, wrong expected", {truncated_body, 0, 5, 3}, short_body,
         "a truncated body does not state its prefix's length, or that length is there"},
        {"body, wrong actual", {truncated_body, 0, 4, 2}, short_body,
         "a stop does not state the bytes after its prefix"},
        {"body, whole body there", {truncated_body, 0, 4, 4}, whole_body,
         "the cursor reported a truncated body with the whole body present"},
        {"prefix, 2 bytes left", {truncated_prefix, 0, 2, 1}, two,
         "the cursor reported a truncated prefix with a whole prefix left"},
        {"prefix, wrong numbers", {truncated_prefix, 0, 2, 2}, one,
         "a truncated prefix does not state 2 bytes needed and the 1 left"},
        {"zero-length, states bytes needed", {zero_length, 0, 2, 1}, zero,
         "a zero-length stop states bytes needed"},
        {"zero-length, wrong actual", {zero_length, 0, 0, 0}, zero,
         "a stop does not state the bytes after its prefix"},
        {"body over one byte", {truncated_body, 0, 2, 0}, one,
         "the cursor read a prefix that is not there"},
        {"end with bytes left", {end, 0, 0, 0}, one, "the cursor reported end with bytes left"},
        {"end, states bytes needed", {end, 0, 1, 0}, none,
         "an end stop states bytes needed or bytes left"},
        {"end, states bytes left", {end, 0, 0, 1}, none,
         "an end stop states bytes needed or bytes left"},
        {"fault with no bytes", {zero_length, 0, 0, 0}, none,
         "the cursor reported a fault with no bytes left"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.label);
        EXPECT_EQ(text(oracle::check_stop(c.stop, c.tail)), c.refusal);
    }

    const std::pair<FrameStop, const Bytes&> right[] = {
        {{zero_length, 0, 0, 1}, zero},          {{truncated_body, 0, 4, 3}, short_body},
        {{truncated_body, 0, 4, 0}, two},        {{truncated_prefix, 0, 2, 1}, one},
        {{end, 0, 0, 0}, none},
    };
    for (const auto& [stop, tail] : right) {
        EXPECT_EQ(text(oracle::check_stop(stop, tail)), "") << to_string(stop);
    }
    whole.pop_back();
    EXPECT_EQ(text(oracle::check_stop({truncated_body, 0, 257, 256}, whole)), "");
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

// The columns oracle's check of a frame from the parser refuses one too short to hold its
// own size without reading past it (B-5 of the final review, docs/REVIEW.md): no input gives
// such a frame today, but the oracle is there for the day the parser does. Empty, and one
// byte in a heap block of its own, so ASan sees a read past it.
TEST(FuzzOracles, TheColumnsFrameCheckRefusesAFrameTooShortToStateItsSize) {
    EXPECT_FALSE(store::oracle::frame_states_its_size(std::span<const std::byte>{}));
    EXPECT_FALSE(store::oracle::frame_states_its_size(exact_copy(Bytes(1, std::byte{0}))));
    EXPECT_FALSE(store::oracle::frame_states_its_size(exact_copy(Bytes(2, std::byte{0}))));
    EXPECT_TRUE(store::oracle::frame_states_its_size(
        exact_copy(Bytes{std::byte{0}, std::byte{1}, std::byte{0x41}})));
    EXPECT_FALSE(store::oracle::frame_states_its_size(
        exact_copy(Bytes{std::byte{0}, std::byte{2}, std::byte{0x41}})));
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
        read_file(std::filesystem::path{TICK_STORE_GOLDEN_DIR} / "itch-store-v3.tsc");
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

// The query oracle on the golden store cut and flipped throughout, and
// on a store of several seek groups a segment whose index columns each have a byte changed.
TEST(FuzzOracles, PassEveryCutAndFlipThroughTheQueryOracle) {
    const Bytes file =
        read_file(std::filesystem::path{TICK_STORE_GOLDEN_DIR} / "itch-store-v3.tsc");
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(text(store::oracle::check_itch_query(file)), "");
    // Every 11th length and every 5th byte: the whole decode, which most cuts and flips
    // still pass under recomputed CRCs, makes each call cost a full set of queries twice.
    for (std::size_t length = 0; length < file.size(); length += 11) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_itch_query(cut)), "") << "length " << length;
    }
    for (std::size_t byte = 0; byte < file.size(); byte += 5) {
        Bytes flipped = exact_copy(file);
        flipped[byte] ^= static_cast<std::byte>(1U << (byte % 8));
        EXPECT_EQ(text(store::oracle::check_itch_query(flipped)), "") << "byte " << byte;
    }
    std::vector<Message> messages;
    for (std::uint64_t i = 0; i < 9'000; ++i) {
        const auto locate = static_cast<std::uint16_t>(1 + (i % 4));
        const MessageHeader header{locate, 0, 34'200'000'000'000ULL + (i * 1'000)};
        if (i % 3 == 2) {
            messages.emplace_back(OrderDelete{header, 1'000 + i - 2});
        } else {
            messages.emplace_back(AddOrder{header, 1'000 + i, std::byte{'B'}, 100,
                                           store::test::stock_of("QRY"), Price4{100'000}});
        }
    }
    const Bytes store_file = store::test::store_of_messages(messages, 4'500);
    EXPECT_EQ(text(store::oracle::check_itch_query(store_file)), "");
    for (const store::Column column :
         {store::Column::seek, store::Column::locate_run, store::Column::locate_run_start,
          store::Column::locate_run_add_start, store::Column::locate_run_import_start,
          store::Column::locate, store::Column::time_base, store::Column::timestamp}) {
        for (const std::size_t at : {std::size_t{0}, std::size_t{12}, std::size_t{60}}) {
            store::test::StoreParts parts = store::test::parts_of(store_file);
            Bytes& payload = parts.block(0, column).bytes;
            if (at < payload.size()) {
                payload[at] ^= std::byte{0x04};
            }
            EXPECT_EQ(text(store::oracle::check_itch_query(parts.build())), "")
                << store::column_info(column).name << " byte " << at;
        }
    }
}

// A segment of raw records alone, between two typed ones: no typed record of it is in any
// window, so only the row before it, which its raw rows repeat the time of, says whether a
// query must read it. Both query paths choose segments with the same may_hold and seed
// them with the same seed_before, so comparing them cannot see either go wrong (review
// C-5); the oracle's column form from the whole decode can. A fault that skips such a
// segment (may_hold answering from its typed records alone) fails this test.
TEST(FuzzOracles, TheQueryOracleSeesASegmentBothPathsWouldSkip) {
    static const Bytes raw = test::bytes({'z', 1, 2, 3});
    std::vector<Message> messages;
    messages.reserve(12);
    for (std::uint64_t i = 0; i < 4; ++i) {
        messages.emplace_back(AddOrder{MessageHeader{1, 0, 1'000 + i}, 10 + i, std::byte{'B'}, 100,
                                       store::test::stock_of("QRY"), Price4{100'000}});
    }
    for (std::uint64_t i = 0; i < 4; ++i) {
        messages.emplace_back(RawMessage{.bytes = raw, .reason = RawReason::unknown_type});
    }
    for (std::uint64_t i = 0; i < 4; ++i) {
        messages.emplace_back(AddOrder{MessageHeader{2, 0, 2'000 + i}, 20 + i, std::byte{'S'}, 100,
                                       store::test::stock_of("QRY"), Price4{100'000}});
    }
    const Bytes file = store::test::store_of_messages(messages, 4);
    const auto reader = store::ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    ASSERT_EQ(reader->segment_count(), 3U);
    ASSERT_EQ(reader->segment(1).summary.typed_records, 0U);
    EXPECT_EQ(text(store::oracle::check_itch_query(file)), "");
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

    // One full vector of 1,024 values and a one-value tail, so the checks, kernel against
    // scalar among them, reach the interleaved vector path and AVX2's fused delta, not only
    // the linear tails the blocks of 16 above take (B-7 of the final review,
    // docs/REVIEW.md): timestamps by delta with no exceptions, and prices by frame of
    // reference with one exception in the vector. A cut at each of the first 24 bytes, which
    // hold the block's and the vector's fields, and a flip every 7 bits there, so each byte
    // gets one; then a cut every 97 bytes and a flip every 257 bits through the packed words.
    // Each check re-encodes 1,025 values in every option set, so this samples rather than
    // trying every bit as the columns above do.
    // The first delta is the first value, so it starts small enough to need no exception.
    std::vector<std::uint64_t> times{40};
    for (std::uint64_t i = 1; i < 1'025; ++i) {
        times.push_back(times.back() + 1 + ((i * 7'919U) % 61U));
    }
    std::vector<std::uint64_t> vector_prices;
    vector_prices.reserve(1'025);
    for (std::uint64_t i = 0; i < 1'025; ++i) {
        vector_prices.push_back(i == 500 ? 1'999'999'900U : 1'234'500U + ((i * 37U) % 11U * 100U));
    }
    Bytes time_vector;
    codec::PforStats time_stats;
    ASSERT_TRUE(codec::encode_pfor(times, 1'025,
                                   codec::PforOptions{.transform = codec::IntTransform::delta,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                   time_vector, &time_stats)
                    .has_value());
    Bytes price_vector;
    codec::PforStats price_stats;
    ASSERT_TRUE(codec::encode_pfor(vector_prices, 1'025,
                                   codec::PforOptions{.transform = codec::IntTransform::none,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                   price_vector, &price_stats)
                    .has_value());
    // A full vector and a one-value tail each, the timestamps' vector with no exceptions,
    // which AVX2 decodes fused, and the prices' with its one.
    for (const codec::PforStats& stats : {time_stats, price_stats}) {
        EXPECT_EQ(stats.full_vectors, 1U);
        EXPECT_EQ(stats.tail_values, 1U);
    }
    EXPECT_EQ(time_stats.exceptions, 0U);
    EXPECT_EQ(price_stats.exceptions, 1U);
    for (const Bytes& column : {time_vector, price_vector}) {
        EXPECT_EQ(text(codec::oracle::check_pfor_column(exact_copy(column))), "");
        for (std::size_t length = 0; length < column.size(); length += length < 24 ? 1 : 97) {
            const Bytes cut = exact_copy(std::span{column}.first(length));
            EXPECT_EQ(text(codec::oracle::check_pfor_column(cut)), "") << "length " << length;
        }
        constexpr std::size_t kFieldBits = std::size_t{24} * 8U;
        for (std::size_t bit = 0; bit < column.size() * 8; bit += bit < kFieldBits ? 7 : 257) {
            Bytes flipped = exact_copy(column);
            flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            EXPECT_EQ(text(codec::oracle::check_pfor_column(flipped)), "") << "bit " << bit;
        }
    }
}

}  // namespace
}  // namespace tick_store::itch
