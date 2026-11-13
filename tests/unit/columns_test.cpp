// The column form, both ways: every row form, every range fallback, every field of the
// parser's validation (T1), and every length of every file cut short (D10).

#include "columns_fixture.hpp"
#include "itch_fixtures.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

using test::Bytes;
using test::ColumnDirectory;
using test::export_columns;
using test::export_payload;
using test::index_of;
using test::source_with_tail;

// The frames a directory gives, or the error that stopped it.
std::expected<std::vector<Bytes>, StoreError> frames_from(const ColumnDirectory& directory) {
    const auto manifest = parse_columns_manifest(directory.manifest_bytes());
    if (!manifest.has_value()) {
        return std::unexpected(manifest.error());
    }
    auto parser = ColumnsParser::open(*manifest, directory.bytes());
    if (!parser.has_value()) {
        return std::unexpected(parser.error());
    }
    std::vector<Bytes> frames;
    while (true) {
        const auto frame = parser->next();
        if (!frame.has_value()) {
            return std::unexpected(frame.error());
        }
        if (!frame->has_value()) {
            return frames;
        }
        frames.emplace_back((*frame)->begin(), (*frame)->end());
    }
}

std::string text_of(std::span<const std::byte> bytes) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): viewing bytes as chars.
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

Bytes from_ascii(std::string_view text) {
    Bytes out;
    itch::test::append_ascii(out, text);
    return out;
}

void set_text(ColumnDirectory& directory, ColumnFile offsets, ColumnFile bytes, std::string_view value,
              std::size_t text_index) {
    // For a one-row directory: replace the whole column with one value.
    directory.files[index_of(bytes)] = from_ascii(value);
    ASSERT_TRUE(write_le<8>(directory.files[index_of(offsets)], 8, value.size()));
    ColumnsManifest manifest = *parse_columns_manifest(directory.manifest_bytes());
    manifest.text_bytes[text_index] = value.size();
    directory.manifest = format_columns_manifest(manifest);
}

void expect_bad_row(const ColumnDirectory& directory, std::string_view field) {
    const auto result = frames_from(directory);
    ASSERT_FALSE(result.has_value()) << field;
    EXPECT_EQ(result.error().kind, StoreErrorKind::bad_row);
    EXPECT_EQ(result.error().field, field);
    EXPECT_EQ(result.error().offset, 0U);
}

template <std::size_t Width>
void set_value(ColumnDirectory& directory, ColumnFile file, std::uint64_t value) {
    ASSERT_TRUE(write_le<Width>(directory.files[index_of(file)], 0, value));
}

Bytes add_order_payload() { return itch::test::fixture('A').bytes; }
Bytes system_event_payload() { return itch::test::fixture('S').bytes; }

Bytes raw_payload() {
    return itch::test::bytes({'z', 0xA1, 0xA2, 0xA3});
}

TEST(Columns, EveryFrameAndTheTailComeBack) {
    const Bytes source = source_with_tail();
    const auto [frames, tail] = test::frames_of(source);
    const ColumnDirectory directory = export_columns(source);
    const auto rebuilt = frames_from(directory);
    ASSERT_TRUE(rebuilt.has_value()) << to_string(rebuilt.error());
    ASSERT_EQ(rebuilt->size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto whole = std::span{source}.subspan(frames[i].offset,
                                                     frames[i].payload.size() + itch::kFramePrefixSize);
        EXPECT_TRUE(std::ranges::equal((*rebuilt)[i], whole)) << "record " << i;
    }
    const auto& stored_tail = directory.files[index_of(ColumnFile::tail)];
    EXPECT_TRUE(std::ranges::equal(stored_tail, tail));
}

TEST(Columns, NoRecordsIsAValidDirectory) {
    const ColumnDirectory directory = export_columns({});
    EXPECT_EQ(directory.manifest,
              "tick-columns 1\nrecords 0\ntail_bytes 0\ntext_bytes stock 0\ntext_bytes mpid 0\n"
              "text_bytes rest 0\n");
    const auto rebuilt = frames_from(directory);
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_TRUE(rebuilt->empty());
}

TEST(Columns, GoldenAddOrderColumns) {
    // Field values are the hand-built fixture's, written out here, so a change to the
    // format has to change this test (T4).
    const ColumnDirectory directory = export_payload(add_order_payload());
    const auto value = [&](ColumnFile file, std::size_t width) {
        std::uint64_t out = 0;
        for (std::size_t i = width; i > 0; --i) {
            out = (out << 8U) | std::to_integer<std::uint64_t>(directory.files[index_of(file)][i - 1]);
        }
        EXPECT_EQ(directory.files[index_of(file)].size(), width);
        return out;
    };
    const auto decoded = itch::decode(add_order_payload()).value();
    const auto& add = std::get<itch::AddOrder>(decoded);
    EXPECT_EQ(value(ColumnFile::seq, 8), 0U);
    EXPECT_EQ(value(ColumnFile::ts_ns, 8), add.header.timestamp_ns);
    EXPECT_EQ(value(ColumnFile::msg_type, 2), 'A');
    EXPECT_EQ(value(ColumnFile::locate, 4), add.header.stock_locate);
    EXPECT_EQ(value(ColumnFile::tracking, 4), add.header.tracking_number);
    EXPECT_EQ(value(ColumnFile::order_ref, 8), add.order_reference_number);
    EXPECT_EQ(value(ColumnFile::new_ref, 8), 0U);
    EXPECT_EQ(value(ColumnFile::side, 1), std::to_integer<std::uint64_t>(add.buy_sell_indicator));
    EXPECT_EQ(value(ColumnFile::shares, 4), add.shares);
    EXPECT_EQ(value(ColumnFile::price, 4), static_cast<std::uint64_t>(add.price));
    EXPECT_EQ(value(ColumnFile::match, 8), 0U);
    EXPECT_EQ(value(ColumnFile::printable, 1), 0U);
    EXPECT_TRUE(std::ranges::equal(directory.files[index_of(ColumnFile::stock_dat)], add.stock));
    EXPECT_TRUE(directory.files[index_of(ColumnFile::mpid_dat)].empty());
    EXPECT_TRUE(directory.files[index_of(ColumnFile::rest_dat)].empty());
}

TEST(Columns, EachHotTypeUsesNoRestAndEachRareTypeOnlyRest) {
    std::size_t hot = 0;
    std::size_t rare = 0;
    for (const auto& fixture : itch::test::fixtures()) {
        SCOPED_TRACE(fixture.label);
        const ColumnDirectory directory = export_payload(fixture.bytes);
        const auto rest = text_of(directory.files[index_of(ColumnFile::rest_dat)]);
        const std::string_view hot_types = "AFECXDUP";
        if (hot_types.contains(static_cast<char>(fixture.bytes.front()))) {
            EXPECT_TRUE(rest.empty());
            ++hot;
        } else {
            EXPECT_EQ(rest.size(), (fixture.bytes.size() - kMessageHeaderSize) * 2);
            EXPECT_TRUE(directory.files[index_of(ColumnFile::order_ref)] == Bytes(8));
            ++rare;
        }
        const auto rebuilt = frames_from(directory);
        ASSERT_TRUE(rebuilt.has_value()) << to_string(rebuilt.error());
        ASSERT_EQ(rebuilt->size(), 1U);
        EXPECT_TRUE(std::ranges::equal(std::span{rebuilt->front()}.subspan(2), fixture.bytes));
    }
    EXPECT_EQ(hot, 8U);
    EXPECT_EQ(rare, 15U);
}

TEST(Columns, RawRowsKeepTheWholePayloadAndRepeatTheTimestamp) {
    Bytes source;
    const auto add = [&](const Bytes& payload) {
        const Bytes frame = itch::test::framed(static_cast<std::uint16_t>(payload.size()), payload);
        source.insert(source.end(), frame.begin(), frame.end());
    };
    add(add_order_payload());
    add(raw_payload());
    const ColumnDirectory directory = export_columns(source);
    EXPECT_EQ(text_of(directory.files[index_of(ColumnFile::rest_dat)]), "raw:7aa1a2a3");
    const auto& ts = directory.files[index_of(ColumnFile::ts_ns)];
    EXPECT_TRUE(std::ranges::equal(std::span{ts}.subspan(0, 8), std::span{ts}.subspan(8, 8)));
    EXPECT_TRUE(frames_from(directory).has_value());

    ColumnsEncoder encoder;
    ASSERT_TRUE(encoder.append(raw_payload()).has_value());
    EXPECT_EQ(encoder.summary().raw_rows, 1U);
    EXPECT_EQ(encoder.summary().hot_rows, 0U);
}

TEST(Columns, AHotFieldOutsideItsColumnTakesTheRawForm) {
    const auto with_fields = [](auto change) {
        auto decoded = itch::decode(add_order_payload()).value();
        change(std::get<itch::AddOrder>(decoded));
        Bytes payload(itch::AddOrder::wire_size);
        EXPECT_TRUE(itch::encode(decoded, payload).has_value());
        return payload;
    };
    const std::vector<Bytes> cases{
        with_fields([](itch::AddOrder& add) { add.order_reference_number = 1ULL << 63U; }),
        with_fields([](itch::AddOrder& add) { add.shares = 1U << 31U; }),
        with_fields([](itch::AddOrder& add) { add.price = itch::Price4{1U << 31U}; }),
        with_fields([](itch::AddOrder& add) { add.buy_sell_indicator = std::byte{0x80}; }),
        with_fields([](itch::AddOrder& add) { add.stock[0] = std::byte{0x00}; }),
        with_fields([](itch::AddOrder& add) { add.stock[7] = std::byte{0x7F}; }),
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        SCOPED_TRACE(i);
        ColumnsEncoder encoder;
        ASSERT_TRUE(encoder.append(cases[i]).has_value());
        EXPECT_EQ(encoder.summary().raw_rows, 1U);
        const ColumnDirectory directory = export_payload(cases[i]);
        const auto rebuilt = frames_from(directory);
        ASSERT_TRUE(rebuilt.has_value()) << to_string(rebuilt.error());
        EXPECT_TRUE(std::ranges::equal(std::span{rebuilt->front()}.subspan(2), cases[i]));
    }

    // The largest values that do fit stay in their columns.
    const Bytes largest = with_fields([](itch::AddOrder& add) {
        add.order_reference_number = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        add.shares = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
        add.price = itch::Price4{static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())};
        add.buy_sell_indicator = std::byte{0x7F};
        add.stock[0] = std::byte{0x7E};
        add.stock[1] = std::byte{0x20};
    });
    ColumnsEncoder encoder;
    ASSERT_TRUE(encoder.append(largest).has_value());
    EXPECT_EQ(encoder.summary().hot_rows, 1U);
    const auto rebuilt = frames_from(export_payload(largest));
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_TRUE(std::ranges::equal(std::span{rebuilt->front()}.subspan(2), largest));
}

TEST(Columns, TimestampsOutOfOrderAreReported) {
    auto later = itch::decode(add_order_payload()).value();
    std::get<itch::AddOrder>(later).header.timestamp_ns = 2000;
    auto earlier = later;
    std::get<itch::AddOrder>(earlier).header.timestamp_ns = 1000;
    Bytes first(itch::AddOrder::wire_size);
    Bytes second(itch::AddOrder::wire_size);
    ASSERT_TRUE(itch::encode(later, first).has_value());
    ASSERT_TRUE(itch::encode(earlier, second).has_value());

    ColumnsEncoder encoder;
    ASSERT_TRUE(encoder.append(first).has_value());
    EXPECT_TRUE(encoder.summary().timestamps_sorted);
    ASSERT_TRUE(encoder.append(second).has_value());
    EXPECT_FALSE(encoder.summary().timestamps_sorted);
}

TEST(Columns, TheEncoderRefusesAnEmptyPayload) {
    ColumnsEncoder encoder;
    const auto refused = encoder.append({});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, StoreErrorKind::unencodable_record);
}

TEST(ColumnsManifestText, FormatsAndParsesBack) {
    const ColumnsManifest manifest{.records = 12, .tail_bytes = 3, .text_bytes = {96, 8, 1000}};
    const std::string text = format_columns_manifest(manifest);
    EXPECT_EQ(text,
              "tick-columns 1\nrecords 12\ntail_bytes 3\ntext_bytes stock 96\ntext_bytes mpid 8\n"
              "text_bytes rest 1000\n");
    const auto parsed = parse_columns_manifest(std::as_bytes(std::span{text}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, manifest);
}

TEST(ColumnsManifestText, RefusesEveryLengthCutShort) {
    const std::string text = format_columns_manifest({.records = 12, .tail_bytes = 3});
    for (std::size_t length = 0; length < text.size(); ++length) {
        const auto cut = std::as_bytes(std::span{text}).first(length);
        const auto parsed = parse_columns_manifest(cut);
        ASSERT_FALSE(parsed.has_value()) << "length " << length;
        EXPECT_EQ(parsed.error().kind, StoreErrorKind::bad_manifest);
    }
}

TEST(ColumnsManifestText, RefusesEachFormOfBadLine) {
    const std::vector<std::pair<std::string, std::string_view>> cases{
        {"", "magic and version"},
        {"tick-columns 2\n", "magic and version"},
        {"tick-columns 1\nrows 1\n", "records"},
        {"tick-columns 1\nrecords 01\n", "records"},
        {"tick-columns 1\nrecords +1\n", "records"},
        {"tick-columns 1\nrecords -1\n", "records"},
        {"tick-columns 1\nrecords 1.0\n", "records"},
        {"tick-columns 1\nrecords \n", "records"},
        {"tick-columns 1\nrecords 18446744073709551616\n", "records"},
        {"tick-columns 1\nrecords 1\ntail 0\n", "tail_bytes"},
        {"tick-columns 1\nrecords 1\ntail_bytes 0\ntext_bytes mpid 0\n", "text_bytes stock"},
        {"tick-columns 1\nrecords 1\ntail_bytes 0\ntext_bytes stock 0\ntext_bytes rest 0\n",
         "text_bytes mpid"},
        {"tick-columns 1\nrecords 1\ntail_bytes 0\ntext_bytes stock 0\ntext_bytes mpid 0\n",
         "text_bytes rest"},
        {"tick-columns 1\nrecords 1\ntail_bytes 0\ntext_bytes stock 0\ntext_bytes mpid 0\n"
         "text_bytes rest 0\nextra\n",
         "end of manifest"},
    };
    for (const auto& [text, field] : cases) {
        SCOPED_TRACE(text);
        const auto parsed = parse_columns_manifest(std::as_bytes(std::span{text}));
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().kind, StoreErrorKind::bad_manifest);
        EXPECT_EQ(parsed.error().field, field);
    }
}

TEST(ColumnsOpen, RefusesEveryFileCutAtEveryLengthAndOneByteLonger) {
    const ColumnDirectory directory = export_columns(source_with_tail());
    const ColumnsManifest manifest = *parse_columns_manifest(directory.manifest_bytes());
    std::size_t cases = 0;
    for (std::size_t file = 0; file < kColumnFileCount; ++file) {
        SCOPED_TRACE(kColumnFileNames[file]);
        ColumnBytes bytes = directory.bytes();
        for (std::size_t length = 0; length < directory.files[file].size(); ++length) {
            bytes[file] = std::span{directory.files[file]}.first(length);
            const auto parser = ColumnsParser::open(manifest, bytes);
            ASSERT_FALSE(parser.has_value()) << "length " << length;
            EXPECT_EQ(parser.error().kind, StoreErrorKind::bad_column_size);
            EXPECT_EQ(parser.error().field, kColumnFileNames[file]);
            ++cases;
        }
        Bytes longer = directory.files[file];
        longer.push_back(std::byte{0});
        bytes[file] = longer;
        const auto parser = ColumnsParser::open(manifest, bytes);
        ASSERT_FALSE(parser.has_value());
        EXPECT_EQ(parser.error().kind, StoreErrorKind::bad_column_size);
        ++cases;
    }
    EXPECT_GT(cases, 2000U);
}

TEST(ColumnsOpen, RefusesAManifestLargerThanAnyFileCouldBe) {
    const ColumnDirectory directory = export_columns(source_with_tail());
    ColumnsManifest manifest = *parse_columns_manifest(directory.manifest_bytes());
    manifest.records = std::numeric_limits<std::uint64_t>::max();
    const auto parser = ColumnsParser::open(manifest, directory.bytes());
    ASSERT_FALSE(parser.has_value());
    EXPECT_EQ(parser.error().kind, StoreErrorKind::bad_column_size);
}

TEST(ColumnsOpen, RefusesOffsetsThatDoNotStartAtZeroOrEndAtTheBytes) {
    ColumnDirectory start = export_payload(add_order_payload());
    ASSERT_TRUE(write_le<8>(start.files[index_of(ColumnFile::stock_off)], 0, 1));
    const auto first = frames_from(start);
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error().kind, StoreErrorKind::bad_text_offsets);
    EXPECT_EQ(first.error().field, "stock.off");

    ColumnDirectory end = export_payload(add_order_payload());
    ASSERT_TRUE(write_le<8>(end.files[index_of(ColumnFile::stock_off)], 8, 7));
    const auto last = frames_from(end);
    ASSERT_FALSE(last.has_value());
    EXPECT_EQ(last.error().kind, StoreErrorKind::bad_text_offsets);
    EXPECT_EQ(last.error().offset, 1U);
}

TEST(ColumnsRows, RefusesOffsetsThatRunBackwardsOrPastTheEnd) {
    Bytes source;
    for (int i = 0; i < 2; ++i) {
        const Bytes frame = itch::test::framed(itch::AddOrder::wire_size, add_order_payload());
        source.insert(source.end(), frame.begin(), frame.end());
    }
    ColumnDirectory backwards = export_columns(source);
    ASSERT_TRUE(write_le<8>(backwards.files[index_of(ColumnFile::rest_off)], 8, 1));
    backwards.files[index_of(ColumnFile::rest_dat)] = from_ascii("x");
    ASSERT_TRUE(write_le<8>(backwards.files[index_of(ColumnFile::rest_off)], 16, 1));
    backwards.manifest = format_columns_manifest({.records = 2,
                                                  .tail_bytes = 0,
                                                  .text_bytes = {16, 0, 1}});
    ASSERT_TRUE(write_le<8>(backwards.files[index_of(ColumnFile::stock_off)], 8, 100));
    const auto bad_walk = frames_from(backwards);
    ASSERT_FALSE(bad_walk.has_value());

    ColumnDirectory past = export_columns(source);
    ASSERT_TRUE(write_le<8>(past.files[index_of(ColumnFile::stock_off)], 8, 20));
    const auto too_far = frames_from(past);
    ASSERT_FALSE(too_far.has_value());
    EXPECT_EQ(too_far.error().kind, StoreErrorKind::bad_text_offsets);
}

TEST(ColumnsRows, EachValidatedFieldOfAHotRow) {
    const auto corrupt = [](auto change, std::string_view field) {
        ColumnDirectory directory = export_payload(add_order_payload());
        change(directory);
        expect_bad_row(directory, field);
    };
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::seq, 1); }, "seq.bin");
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::ts_ns, 1ULL << 48U); }, "ts_ns.bin");
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::ts_ns, ~0ULL); }, "ts_ns.bin");
    corrupt([](auto& d) { set_value<2>(d, ColumnFile::msg_type, 256); }, "msg_type.bin");
    corrupt([](auto& d) { set_value<2>(d, ColumnFile::msg_type, 0xFFFF); }, "msg_type.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::locate, 65536); }, "locate.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::locate, 0xFFFFFFFF); }, "locate.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::tracking, 65536); }, "tracking.bin");
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::order_ref, ~0ULL); }, "order_ref.bin");
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::new_ref, 1); }, "new_ref.bin");
    corrupt([](auto& d) { set_value<1>(d, ColumnFile::side, 0xFF); }, "side.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::shares, 0xFFFFFFFF); }, "shares.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::price, 0xFFFFFFFF); }, "price.bin");
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::match, 1); }, "match.bin");
    corrupt([](auto& d) { set_value<1>(d, ColumnFile::printable, 'Y'); }, "printable.bin");
    corrupt([](auto& d) { set_text(d, ColumnFile::stock_off, ColumnFile::stock_dat, "ABC", 0); },
            "stock.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::mpid_off, ColumnFile::mpid_dat, "MPID", 1); },
            "mpid.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "00", 2); },
            "rest.dat");
}

TEST(ColumnsRows, EachValidatedFieldOfARareRow) {
    const auto corrupt = [](auto change, std::string_view field) {
        ColumnDirectory directory = export_payload(system_event_payload());
        change(directory);
        expect_bad_row(directory, field);
    };
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::order_ref, 1); }, "order_ref.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::shares, 1); }, "shares.bin");
    corrupt([](auto& d) { set_value<2>(d, ColumnFile::msg_type, 'z'); }, "msg_type.bin");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "4f4f", 2); },
            "rest.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "4", 2); },
            "rest.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "4F", 2); },
            "rest.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "g0", 2); },
            "rest.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::stock_off, ColumnFile::stock_dat, "ABCDEFGH", 0); },
            "stock.dat");
}

TEST(ColumnsRows, EachValidatedFieldOfARawRow) {
    const auto corrupt = [](auto change, std::string_view field) {
        ColumnDirectory directory = export_payload(raw_payload());
        change(directory);
        expect_bad_row(directory, field);
    };
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::ts_ns, 1); }, "ts_ns.bin");
    corrupt([](auto& d) { set_value<2>(d, ColumnFile::msg_type, 'y'); }, "msg_type.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::locate, 1); }, "locate.bin");
    corrupt([](auto& d) { set_value<4>(d, ColumnFile::tracking, 1); }, "tracking.bin");
    corrupt([](auto& d) { set_value<8>(d, ColumnFile::order_ref, 1); }, "order_ref.bin");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "raw:", 2); },
            "rest.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "raw:7", 2); },
            "rest.dat");
    corrupt([](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, "raw:7G", 2); },
            "rest.dat");
    const std::string too_long = "raw:" + std::string((itch::kMaxPayloadSize + 1) * 2, '7');
    corrupt([&](auto& d) { set_text(d, ColumnFile::rest_off, ColumnFile::rest_dat, too_long, 2); },
            "rest.dat");
}

TEST(ColumnsRows, TheLargestRawPayloadComesBack) {
    Bytes payload(itch::kMaxPayloadSize, std::byte{0x5A});
    const auto rebuilt = frames_from(export_payload(payload));
    ASSERT_TRUE(rebuilt.has_value()) << to_string(rebuilt.error());
    ASSERT_EQ(rebuilt->size(), 1U);
    EXPECT_TRUE(std::ranges::equal(std::span{rebuilt->front()}.subspan(2), payload));
}

TEST(ColumnsRows, ARefusalAndTheEndBothRepeat) {
    ColumnDirectory directory = export_payload(add_order_payload());
    const ColumnsManifest manifest = *parse_columns_manifest(directory.manifest_bytes());
    {
        auto parser = ColumnsParser::open(manifest, directory.bytes());
        ASSERT_TRUE(parser.has_value());
        ASSERT_TRUE(parser->next().has_value());
        for (int i = 0; i < 3; ++i) {
            const auto end = parser->next();
            ASSERT_TRUE(end.has_value());
            EXPECT_FALSE(end->has_value());
        }
    }
    set_value<8>(directory, ColumnFile::seq, 7);
    auto parser = ColumnsParser::open(manifest, directory.bytes());
    ASSERT_TRUE(parser.has_value());
    const auto first = parser->next();
    ASSERT_FALSE(first.has_value());
    const auto second = parser->next();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(first.error(), second.error());
}

// An error with no values set prints none (gate finding 3); one with both prints both.
TEST(ColumnsErrors, EachNewKindHasAMessage) {
    EXPECT_EQ(to_string(StoreError{.kind = StoreErrorKind::bad_manifest, .field = "records"}),
              "bad manifest: records at offset 0");
    EXPECT_EQ(to_string(StoreError{.kind = StoreErrorKind::bad_column_size, .field = "seq.bin"}),
              "bad column size: seq.bin at offset 0");
    EXPECT_EQ(to_string(StoreError{.kind = StoreErrorKind::bad_text_offsets, .field = "rest.off"}),
              "bad text offsets: rest.off at offset 0");
    EXPECT_EQ(to_string(StoreError{.kind = StoreErrorKind::bad_row, .field = "seq.bin"}),
              "bad row: seq.bin at offset 0");
    EXPECT_EQ(to_string(StoreError{.kind = StoreErrorKind::bad_row,
                                   .field = "seq.bin",
                                   .offset = 3,
                                   .expected = 0,
                                   .actual = 1}),
              "bad row: seq.bin at offset 3: expected 0, have 1");
}

}  // namespace
}  // namespace tick_store::store
