// The ITCH store's column mappings, its order references, stock exceptions, segment
// summaries, random access and the writer's refusals. Corrupt files are in
// itch_store_corrupt_test.cpp.

#include "itch_fixtures.hpp"
#include "itch_store_fixture.hpp"
#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "tick_store/harness/itch_store_record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/itch_segment.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/itch_store_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace tick_store::store {
namespace {

using test::Bytes;
using test::column_values;
using test::payloads_of;

constexpr std::uint64_t kLocate = 0x1112;
constexpr std::uint64_t kTracking = 0x2122;
constexpr std::uint64_t kTime = 0x313233343536;

std::uint64_t byte_value(std::byte b) { return std::to_integer<std::uint64_t>(b); }

std::vector<std::uint64_t> bytes_after_header(const Bytes& payload) {
    std::vector<std::uint64_t> out;
    for (std::size_t i = kItchHeaderBytes; i < payload.size(); ++i) {
        out.push_back(byte_value(payload[i]));
    }
    return out;
}

// The columns each type fills beyond type, locate, timestamp and tracking.
const std::map<char, std::set<Column>>& columns_of_type() {
    static const std::map<char, std::set<Column>> table{
        {'A', {Column::creation_ref, Column::add_side, Column::add_price, Column::add_shares}},
        {'F',
         {Column::creation_ref, Column::add_side, Column::add_price, Column::add_shares,
          Column::mpid}},
        {'E', {Column::ref_distance, Column::import_ref, Column::reduce_shares, Column::match}},
        {'C',
         {Column::ref_distance, Column::import_ref, Column::reduce_shares, Column::match,
          Column::printable, Column::exec_price}},
        {'X', {Column::ref_distance, Column::import_ref, Column::reduce_shares}},
        {'D', {Column::ref_distance, Column::import_ref}},
        {'U',
         {Column::ref_distance, Column::import_ref, Column::creation_ref, Column::replace_shares,
          Column::replace_price}},
        {'P',
         {Column::trade_ref, Column::trade_side, Column::trade_shares, Column::trade_price,
          Column::match}},
        {'Q', {Column::cross_shares, Column::cross_price, Column::match, Column::cross_type}},
        {'B', {Column::match}},
        {'I', {Column::imbalance_shares, Column::imbalance_price, Column::imbalance_flags}},
        {'V', {Column::mwcb_level}},
    };
    return table;
}

using Expected = std::map<Column, std::vector<std::uint64_t>>;

// What each column of a one-record store must hold for this message.
Expected expected_columns(const itch::Message& message, const Bytes& payload) {
    Expected out;
    const auto u = [](auto value) { return static_cast<std::uint64_t>(value); };
    std::visit(
        [&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, itch::AddOrder> ||
                          std::is_same_v<T, itch::AddOrderWithMpid>) {
                out[Column::creation_ref] = {m.order_reference_number};
                out[Column::add_side] = {byte_value(m.buy_sell_indicator)};
                out[Column::add_price] = {u(m.price)};
                out[Column::add_shares] = {m.shares};
                if constexpr (std::is_same_v<T, itch::AddOrderWithMpid>) {
                    std::uint64_t mpid = 0;
                    for (const std::byte b : m.attribution) {
                        mpid = (mpid << 8U) | byte_value(b);
                    }
                    out[Column::mpid] = {mpid};
                }
            } else if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
                out[Column::ref_distance] = {0};
                out[Column::import_ref] = {m.order_reference_number};
                out[Column::reduce_shares] = {m.executed_shares};
                out[Column::match] = {m.match_number};
            } else if constexpr (std::is_same_v<T, itch::OrderExecutedWithPrice>) {
                out[Column::ref_distance] = {0};
                out[Column::import_ref] = {m.order_reference_number};
                out[Column::reduce_shares] = {m.executed_shares};
                out[Column::match] = {m.match_number};
                out[Column::printable] = {byte_value(m.printable)};
                out[Column::exec_price] = {u(m.execution_price)};
            } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
                out[Column::ref_distance] = {0};
                out[Column::import_ref] = {m.order_reference_number};
                out[Column::reduce_shares] = {m.cancelled_shares};
            } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
                out[Column::ref_distance] = {0};
                out[Column::import_ref] = {m.order_reference_number};
            } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
                out[Column::ref_distance] = {0};
                out[Column::import_ref] = {m.original_order_reference_number};
                out[Column::creation_ref] = {m.new_order_reference_number};
                out[Column::replace_shares] = {m.shares};
                out[Column::replace_price] = {u(m.price)};
            } else if constexpr (std::is_same_v<T, itch::Trade>) {
                out[Column::trade_ref] = {m.order_reference_number};
                out[Column::trade_side] = {byte_value(m.buy_sell_indicator)};
                out[Column::trade_shares] = {m.shares};
                out[Column::trade_price] = {u(m.price)};
                out[Column::match] = {m.match_number};
            } else if constexpr (std::is_same_v<T, itch::CrossTrade>) {
                out[Column::cross_shares] = {m.shares};
                out[Column::cross_price] = {u(m.cross_price)};
                out[Column::match] = {m.match_number};
                out[Column::cross_type] = {byte_value(m.cross_type)};
            } else if constexpr (std::is_same_v<T, itch::BrokenTrade>) {
                out[Column::match] = {m.match_number};
            } else if constexpr (std::is_same_v<T, itch::NetOrderImbalance>) {
                out[Column::imbalance_shares] = {m.paired_shares, m.imbalance_shares};
                out[Column::imbalance_price] = {u(m.far_price), u(m.near_price),
                                                u(m.current_reference_price)};
                out[Column::imbalance_flags] = {byte_value(m.imbalance_direction) |
                                                (byte_value(m.cross_type) << 8U) |
                                                (byte_value(m.price_variation_indicator) << 16U)};
            } else if constexpr (std::is_same_v<T, itch::MwcbDeclineLevel>) {
                out[Column::mwcb_level] = {u(m.level_1), u(m.level_2), u(m.level_3)};
            } else if constexpr (!std::is_same_v<T, itch::RawMessage>) {
                out[Column::rare_bytes] = bytes_after_header(payload);
            }
        },
        message);
    return out;
}

// --- Column mappings ------------------------------------------------------------------

TEST(ItchStoreColumns, EachTypeFillsExactlyItsColumnsWithItsFields) {
    for (const auto& fixture : itch::test::fixtures()) {
        SCOPED_TRACE(std::string{fixture.label});
        const auto message = itch::decode(fixture.bytes);
        ASSERT_TRUE(message.has_value());
        const std::vector<itch::Message> one{*message};
        const Bytes file = test::store_of_messages(one);
        auto reader = ItchStoreReader::open_bytes(file);
        ASSERT_TRUE(reader.has_value()) << to_string(reader.error());
        auto columns = reader->read_columns(0);
        ASSERT_TRUE(columns.has_value()) << to_string(columns.error());

        Expected expected = expected_columns(*message, fixture.bytes);
        expected[Column::type] = {static_cast<std::uint64_t>(fixture.type)};
        expected[Column::locate] = {kLocate};
        expected[Column::timestamp] = {kTime};
        expected[Column::tracking] = {kTracking};
        if (const auto known = columns_of_type().find(fixture.type);
            known != columns_of_type().end()) {
            for (const Column column : known->second) {
                EXPECT_TRUE(expected.contains(column)) << column_info(column).name;
            }
            EXPECT_EQ(expected.size(), known->second.size() + 4);
        } else {
            EXPECT_TRUE(is_rare_type(static_cast<std::uint8_t>(fixture.type)));
            EXPECT_EQ(expected.size(), 5U);
        }
        for (std::size_t c = 0; c < kColumnCount; ++c) {
            const auto column = static_cast<Column>(c);
            const auto want = expected.find(column);
            if (want == expected.end()) {
                EXPECT_TRUE((*columns)[column].empty()) << column_info(column).name;
            } else {
                EXPECT_EQ((*columns)[column], want->second) << column_info(column).name;
            }
        }
        // The stock, where the type has one, went to the symbol table, not to a column.
        const bool has_stock =
            std::visit([](const auto& m) { return requires { m.stock; }; }, *message);
        EXPECT_EQ(reader->symbols().size(), has_stock ? 1U : 0U);
    }
}

TEST(ItchStoreColumns, ARawRecordFillsOnlyItsTypeLengthAndBytes) {
    const Bytes payload = itch::test::bytes({'z', 0xA1, 0xA2, 0xA3});
    const auto message = itch::decode(payload);
    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(std::holds_alternative<itch::RawMessage>(*message));
    const std::vector<itch::Message> one{*message};
    const Bytes file = test::store_of_messages(one);
    auto reader = ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    auto columns = reader->read_columns(0);
    ASSERT_TRUE(columns.has_value());
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        const auto column = static_cast<Column>(c);
        switch (column) {
            case Column::type:
                EXPECT_EQ((*columns)[column], std::vector<std::uint64_t>{0});
                break;
            case Column::raw_length:
                EXPECT_EQ((*columns)[column], std::vector<std::uint64_t>{4});
                break;
            case Column::raw_bytes:
                EXPECT_EQ((*columns)[column], (std::vector<std::uint64_t>{'z', 0xA1, 0xA2, 0xA3}));
                break;
            default:
                EXPECT_TRUE((*columns)[column].empty()) << column_info(column).name;
        }
    }
    EXPECT_EQ(reader->segment(0).summary.typed_records, 0U);
    EXPECT_EQ(reader->segment(0).summary.max_time, 0U);
    EXPECT_TRUE(reader->segment(0).summary.locates.empty());
}

TEST(ItchStoreColumns, CountsFromTypesMatchWhatTheBuilderWrites) {
    const Bytes source = tick_store::test::source_records();
    const Bytes file = test::store_of_source(source);
    auto reader = ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    auto columns = reader->read_columns(0);
    ASSERT_TRUE(columns.has_value());
    const auto counts = counts_from_types((*columns)[Column::type]);
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        const auto column = static_cast<Column>(c);
        if (column == Column::import_ref || column == Column::raw_bytes ||
            column == Column::stock_exception_record || column == Column::stock_exception_value) {
            EXPECT_EQ(counts[c], 0U);
            continue;
        }
        EXPECT_EQ(counts[c], (*columns)[column].size()) << column_info(column).name;
    }
}

TEST(ItchStoreColumns, EveryColumnRoundTripsThroughItsCodecAtItsLimits) {
    for (const ColumnInfo& info : kColumns) {
        SCOPED_TRACE(std::string{info.name});
        std::vector<std::uint64_t> values;
        for (std::uint64_t i = 0; i < 3 * kColumnBlockValues + 17; ++i) {
            values.push_back(i % 5 == 0 ? info.max_value : (i * 2654435761U) & info.max_value);
        }
        auto chunk = encode_column(info.id, values);
        ASSERT_TRUE(chunk.has_value()) << to_string(chunk.error());
        EXPECT_TRUE(chunk->codec == primary_codec(info.id) ||
                    (info.zstd && chunk->codec == codec::CodecId::zstd));
        auto decoded = decode_column(info.id, chunk->codec, chunk->bytes, values.size());
        ASSERT_TRUE(decoded.has_value()) << to_string(decoded.error());
        EXPECT_EQ(*decoded, values);

        if (info.max_value != kMax64) {
            const std::vector<std::uint64_t> over{info.max_value + 1};
            const auto refused = encode_column(info.id, over);
            ASSERT_FALSE(refused.has_value());
            EXPECT_EQ(refused.error().field, "column value");
        }
    }
    EXPECT_FALSE(encode_column(Column::type, {}).has_value());
}

TEST(ItchStoreColumns, ZstdIsKeptOnlyWhereItIsSmaller) {
    // A byte column of one repeated value compresses; one of noise does not.
    const std::vector<std::uint64_t> same(10000, 'A');
    auto squeezed = encode_column(Column::rare_bytes, same);
    ASSERT_TRUE(squeezed.has_value());
    EXPECT_EQ(squeezed->codec, codec::CodecId::zstd);
    EXPECT_LT(squeezed->bytes.size(), same.size());

    std::vector<std::uint64_t> noise;
    std::uint64_t state = 1;
    for (int i = 0; i < 64; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        noise.push_back(state >> 56U);
    }
    auto plain = encode_column(Column::rare_bytes, noise);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->codec, codec::CodecId::raw);
    EXPECT_EQ(plain->bytes.size(), noise.size());

    // A column without zstd never gets it.
    auto timestamps = encode_column(Column::timestamp, std::vector<std::uint64_t>(10000, 7));
    ASSERT_TRUE(timestamps.has_value());
    EXPECT_EQ(timestamps->codec, codec::CodecId::pfor);
}

// --- Round trips ------------------------------------------------------------------------

TEST(ItchStoreRoundTrip, EveryTypeRawRecordAndTailPassesTheOracleAtEverySegmentSize) {
    const Bytes source = tick_store::test::source_with_tail();
    for (const std::uint32_t n : {1U, 2U, 3U, 7U, 25U, 26U, 27U, kDefaultSegmentRecords}) {
        SCOPED_TRACE(n);
        const Bytes file = test::store_of_source(source, n);
        for (const Checksums checksums : {Checksums::verify, Checksums::skip}) {
            const harness::VerifyReport report = harness::verify_itch_store(
                source, file, {.expected_sha256 = harness::sha256(source)},
                {.checksums = checksums});
            EXPECT_TRUE(report.passed());
            EXPECT_EQ(report.stored_records, tick_store::test::kSourceRecordCount);
            EXPECT_EQ(report.stored_tail_bytes, 4U);
        }
        auto reader = ItchStoreReader::open_bytes(file);
        ASSERT_TRUE(reader.has_value());
        EXPECT_EQ(reader->header().segment_records, n);
        EXPECT_EQ(reader->header().record_count, tick_store::test::kSourceRecordCount);
        EXPECT_EQ(reader->segment_count(), (tick_store::test::kSourceRecordCount + n - 1) / n);
    }
}

TEST(ItchStoreRoundTrip, AnEmptySourceIsAStoreOfNoSegments) {
    const Bytes file = test::store_of_source({});
    auto reader = ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader->segment_count(), 0U);
    EXPECT_EQ(reader->header().record_count, 0U);
    EXPECT_EQ(reader->container().block_count(), 0U);
    EXPECT_TRUE(harness::verify_itch_store({}, file).passed());

    // A tail with no whole frame before it.
    const Bytes tail = tick_store::test::truncated_tail();
    const Bytes tail_only = test::store_of_source(tail);
    EXPECT_TRUE(harness::verify_itch_store(tail, tail_only).passed());
}

TEST(ItchStoreRoundTrip, AStoreThatDropsOrChangesARecordFailsTheOracle) {
    const Bytes source = tick_store::test::source_with_tail();
    const Bytes other = tick_store::test::source_records();  // No tail.
    const Bytes file = test::store_of_source(other);
    EXPECT_FALSE(harness::verify_itch_store(source, file).passed());

    Bytes changed = source;
    changed[20] ^= std::byte{1};
    EXPECT_FALSE(harness::verify_itch_store(changed, test::store_of_source(source)).passed());
}

TEST(ItchStoreRoundTrip, AFileOnDiskReadsBackUnderEitherDurability) {
    const Bytes source = tick_store::test::source_with_tail();
    tick_store::test::ScratchDir dir;
    for (const Durability durability : {Durability::page_cache, Durability::device_flush}) {
        const auto path = dir / "store.tsc";
        auto writer = ItchStoreWriter::create(path, durability, {.segment_records = 5});
        ASSERT_TRUE(writer.has_value()) << to_string(writer.error());
        itch::FrameCursor cursor{source};
        for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
            ASSERT_TRUE(writer->append(*itch::decode(frame->payload)).has_value());
        }
        ASSERT_TRUE(writer->finish(cursor.remaining()).has_value());
        EXPECT_TRUE(writer->take_bytes().empty());
        EXPECT_EQ(writer->records(), tick_store::test::kSourceRecordCount);

        // The same bytes as the in-memory writer's.
        const auto mapped = MappedFile::open(path);
        ASSERT_TRUE(mapped.has_value());
        EXPECT_TRUE(std::ranges::equal(mapped->bytes(), test::store_of_source(source, 5)));
        auto reader = ItchStoreReader::open(path);
        ASSERT_TRUE(reader.has_value()) << to_string(reader.error());
        ItchStoreReader moved = std::move(*reader);
        harness::ItchStoreRecordReader records{moved};
        EXPECT_TRUE(harness::verify(source, records).passed());
    }
}

// --- Order references ---------------------------------------------------------------

TEST(ItchStoreReferences, AnOrderCreatedInTheSegmentIsADistanceInItsLocatesList) {
    const std::vector<itch::Message> messages{
        test::add(1, 10, 100),  // locate 1: [100]
        test::add(2, 11, 200),  // locate 2: [200]
        test::add(1, 12, 101),  // locate 1: [100, 101]
        test::executed(1, 13, 100),  // distance 2
        test::deleted(2, 14, 200),   // distance 1
        test::cancelled(1, 15, 101), // distance 1
        test::replaced(1, 16, 100, 102),  // distance 2; creates 102: [100, 101, 102]
        test::deleted(1, 17, 102),        // distance 1
    };
    const Bytes file = test::store_of_messages(messages);
    EXPECT_EQ(test::all_payloads(file).value(), payloads_of(messages));
    // Grouped by type, stably: D (14, 17), E (13), U (16), X (15).
    EXPECT_EQ(column_values(file, 0, Column::ref_distance),
              (std::vector<std::uint64_t>{1, 1, 2, 2, 1}));
    EXPECT_TRUE(column_values(file, 0, Column::import_ref).empty());
    EXPECT_EQ(column_values(file, 0, Column::creation_ref),
              (std::vector<std::uint64_t>{100, 200, 101, 102}));
}

TEST(ItchStoreReferences, AnOrderFromAnEarlierSegmentIsImportedOnceThenADistance) {
    const std::vector<itch::Message> messages{
        test::add(1, 10, 100),
        test::add(1, 11, 101),
        // Segment 1: 100 and 101 are not its own.
        test::executed(1, 12, 100),  // import 100: [100]
        test::executed(1, 13, 100),  // distance 1
        // Segment 2 knows nothing of segment 1's list: both are imports again.
        test::deleted(1, 14, 101),   // import 101: [101]
        test::deleted(1, 15, 100),   // import 100: [101, 100]
    };
    const Bytes file = test::store_of_messages(messages, 2);
    EXPECT_EQ(test::all_payloads(file).value(), payloads_of(messages));
    EXPECT_EQ(column_values(file, 1, Column::ref_distance), (std::vector<std::uint64_t>{0, 1}));
    EXPECT_EQ(column_values(file, 1, Column::import_ref), (std::vector<std::uint64_t>{100}));
    EXPECT_EQ(column_values(file, 2, Column::ref_distance), (std::vector<std::uint64_t>{0, 0}));
    EXPECT_EQ(column_values(file, 2, Column::import_ref), (std::vector<std::uint64_t>{101, 100}));
}

TEST(ItchStoreReferences, AnOrderUnderAnotherLocateOrNeverCreatedIsImported) {
    // The writer keeps one entry per reference, holding the locate that last named or
    // created it, so a reference named under alternating locates is imported each time.
    const std::vector<itch::Message> messages{
        test::add(1, 10, 100),
        test::executed(2, 11, 100),  // Created under locate 1: import on locate 2.
        test::executed(1, 12, 100),  // The entry now says locate 2: import on locate 1.
        test::executed(2, 13, 100),  // And back: import.
        test::deleted(3, 14, 999),   // Never created: import.
        test::deleted(3, 15, 999),   // distance 1
        test::executed(3, 16, 999),  // distance 1
    };
    const Bytes file = test::store_of_messages(messages);
    EXPECT_EQ(test::all_payloads(file).value(), payloads_of(messages));
    // D (14, 15), then E (11, 12, 13, 16).
    EXPECT_EQ(column_values(file, 0, Column::ref_distance),
              (std::vector<std::uint64_t>{0, 1, 0, 0, 0, 1}));
    EXPECT_EQ(column_values(file, 0, Column::import_ref),
              (std::vector<std::uint64_t>{100, 100, 100, 999}));
}

TEST(ItchStoreReferences, AReferenceCreatedTwiceNamesTheLatestCreation) {
    const std::vector<itch::Message> messages{
        test::add(1, 10, 100),
        test::add(1, 11, 100),
        test::deleted(1, 12, 100),
        test::add(2, 13, 100),
        test::deleted(2, 14, 100),
    };
    const Bytes file = test::store_of_messages(messages);
    EXPECT_EQ(test::all_payloads(file).value(), payloads_of(messages));
    EXPECT_EQ(column_values(file, 0, Column::ref_distance), (std::vector<std::uint64_t>{1, 1}));
}

// --- Stocks -------------------------------------------------------------------------------

TEST(ItchStoreStocks, ADerivedStockThatDiffersFromTheSymbolIsAnException) {
    const std::vector<itch::Message> messages{
        test::directory(1, 10, "AAPL"),
        test::add(1, 11, 100, "AAPL"),
        test::add(1, 12, 101, "MSFT"),  // Exception at record 2.
        test::add(2, 13, 102, "IBM"),   // First seen: learned.
        test::add(2, 14, 103, "IBM"),
        test::add(1, 15, 104, "AAPL"),
        test::add(2, 16, 105, "AAPL"),  // Exception at record 6.
    };
    const Bytes file = test::store_of_messages(messages);
    EXPECT_EQ(test::all_payloads(file).value(), payloads_of(messages));
    EXPECT_EQ(column_values(file, 0, Column::stock_exception_record),
              (std::vector<std::uint64_t>{2, 6}));
    std::vector<std::uint64_t> stocks;
    for (const std::string_view text : {"MSFT", "AAPL"}) {
        for (const std::byte b : test::stock_of(text)) {
            stocks.push_back(byte_value(b));
        }
    }
    EXPECT_EQ(column_values(file, 0, Column::stock_exception_value), stocks);
    auto reader = ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(*reader->symbols().find(1), test::stock_of("AAPL"));
    EXPECT_EQ(*reader->symbols().find(2), test::stock_of("IBM"));
}

TEST(ItchStoreStocks, TheSymbolTableEncodesInLocateOrderAndParsesBack) {
    SymbolTable table;
    EXPECT_EQ(table.learn(9, test::stock_of("B")), test::stock_of("B"));
    EXPECT_EQ(table.learn(3, test::stock_of("A")), test::stock_of("A"));
    EXPECT_EQ(table.learn(9, test::stock_of("C")), test::stock_of("B"));
    const auto encoded = table.encode();
    ASSERT_EQ(encoded.size(), kSymbolsCountSize + (2 * kSymbolEntrySize));
    EXPECT_EQ(test::get_field(encoded, 0, 4), 2U);
    EXPECT_EQ(test::get_field(encoded, 4, 2), 3U);
    EXPECT_EQ(test::get_field(encoded, 14, 2), 9U);
    auto parsed = SymbolTable::parse(encoded, 0);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(*parsed == table);
    EXPECT_EQ(parsed->find(4), nullptr);
}

// --- Segments -------------------------------------------------------------------------

TEST(ItchStoreSegments, TheSummaryHoldsTimesRecordsAndLocates) {
    const Bytes raw_payload = itch::test::bytes({'z', 1});
    const auto raw = itch::decode(raw_payload);
    ASSERT_TRUE(raw.has_value());
    const std::vector<itch::Message> messages{
        test::add(7, 500, 1), *raw, test::add(9, 300, 2), test::add(7, 900, 3),
        test::add(8, 400, 4),  test::add(16, 100, 5),
    };
    const Bytes file = test::store_of_messages(messages, 4);
    auto reader = ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    ASSERT_EQ(reader->segment_count(), 2U);
    const SegmentSummary& first = reader->segment(0).summary;
    EXPECT_EQ(first.first_record, 0U);
    EXPECT_EQ(first.records, 4U);
    EXPECT_EQ(first.typed_records, 3U);
    EXPECT_EQ(first.first_time, 500U);
    EXPECT_EQ(first.last_time, 900U);
    EXPECT_EQ(first.min_time, 300U);
    EXPECT_EQ(first.max_time, 900U);
    EXPECT_EQ(first.locates, (Bytes{std::byte{0x80}, std::byte{0x02}}));
    const SegmentSummary& second = reader->segment(1).summary;
    EXPECT_EQ(second.first_record, 4U);
    EXPECT_EQ(second.records, 2U);
    EXPECT_EQ(second.first_time, 400U);
    EXPECT_EQ(second.last_time, 100U);
    EXPECT_EQ(second.min_time, 100U);
    EXPECT_EQ(second.max_time, 400U);
    EXPECT_EQ(second.locates, (Bytes{std::byte{0}, std::byte{0x01}, std::byte{0x01}}));
}

TEST(ItchStoreSegments, MetadataEncodesAndParsesBack) {
    ColumnMetadata plain{.segment = 7, .column = Column::match, .values = 9, .summary = {}};
    auto parsed = parse_column_metadata(encode_column_metadata(plain), 0);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, plain);

    SegmentSummary summary{.first_record = 70,
                           .records = 9,
                           .typed_records = 8,
                           .first_time = 5,
                           .last_time = 6,
                           .min_time = 1,
                           .max_time = 9,
                           .locates = {std::byte{0}, std::byte{4}}};
    ColumnMetadata typed{.segment = 7, .column = Column::type, .values = 9, .summary = summary};
    const auto encoded = encode_column_metadata(typed);
    EXPECT_EQ(encoded.size(), kSegmentMetadataFixedSize + 2);
    parsed = parse_column_metadata(encoded, 0);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, typed);

    const StoreHeader header{.segment_records = 3, .record_count = 7, .segment_count = 3};
    auto store = parse_store_header(encode_store_header(header), 0);
    ASSERT_TRUE(store.has_value());
    EXPECT_EQ(*store, header);
}

TEST(ItchStoreSegments, RandomAccessToAnySegmentGivesWhatTheSequentialReadGives) {
    // Orders created, named and replaced across segment boundaries, on a few locates.
    std::vector<itch::Message> messages;
    std::vector<std::uint64_t> live;
    std::uint64_t next = 1000;
    std::uint64_t state = 42;
    for (std::uint64_t t = 1; t <= 3000; ++t) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto locate = static_cast<std::uint16_t>(1 + ((state >> 40U) % 5));
        const std::uint64_t pick = (state >> 20U) % 10;
        if (live.empty() || pick < 4) {
            messages.push_back(test::add(locate, t, next, "ZVZZT", (state & 1U) != 0 ? 'B' : 'S',
                                         static_cast<std::uint32_t>(100'0000 + (state >> 50U))));
            live.push_back(next++);
            continue;
        }
        const std::size_t at = (state >> 8U) % live.size();
        const std::uint64_t reference = live[at];
        // The locate an order was added under is not tracked: references across locates are
        // imports, and must come back the same.
        if (pick < 6) {
            messages.push_back(test::executed(locate, t, reference, 1, t));
        } else if (pick < 8) {
            messages.push_back(test::replaced(locate, t, reference, next));
            live[at] = next++;
        } else {
            messages.push_back(test::deleted(locate, t, reference));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
    const Bytes file = test::store_of_messages(messages, 64);
    const auto sequential = test::all_payloads(file);
    ASSERT_TRUE(sequential.has_value()) << to_string(sequential.error());
    ASSERT_EQ(*sequential, payloads_of(messages));

    auto reader = ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    ASSERT_EQ(reader->segment_count(), (3000U + 63U) / 64U);
    // Last to first, and one SegmentReader reused, so nothing carries from a previous seek.
    SegmentReader records;
    for (std::uint64_t k = reader->segment_count(); k-- > 0;) {
        ASSERT_TRUE(records.seek(*reader, k).has_value());
        const std::uint64_t first = reader->segment(k).summary.first_record;
        for (std::uint64_t i = 0;; ++i) {
            auto message = records.next();
            ASSERT_TRUE(message.has_value());
            if (!message->has_value()) {
                EXPECT_EQ(i, reader->segment(k).summary.records);
                break;
            }
            EXPECT_EQ(test::payload_of(**message), (*sequential)[first + i]);
        }
    }
    EXPECT_FALSE(records.seek(*reader, reader->segment_count()).has_value());
}

TEST(ItchStoreSegments, AddPriceIsGroupedByLocateAndSideAndAddSharesByLocate) {
    const std::vector<itch::Message> messages{
        test::add(2, 1, 1, "B", 'S', 30, 3), test::add(1, 2, 2, "A", 'B', 10, 1),
        test::add(2, 3, 3, "B", 'B', 20, 2), test::add(1, 4, 4, "A", 'B', 11, 4),
        test::add(2, 5, 5, "B", 'S', 31, 5),
    };
    const Bytes file = test::store_of_messages(messages);
    EXPECT_EQ(test::all_payloads(file).value(), payloads_of(messages));
    // Keys (locate << 8 | side): 1B, 1B, 2B, 2S, 2S.
    EXPECT_EQ(column_values(file, 0, Column::add_price),
              (std::vector<std::uint64_t>{10, 11, 20, 30, 31}));
    EXPECT_EQ(column_values(file, 0, Column::add_shares),
              (std::vector<std::uint64_t>{1, 4, 3, 2, 5}));
    // Side stays in file order.
    EXPECT_EQ(column_values(file, 0, Column::add_side),
              (std::vector<std::uint64_t>{'S', 'B', 'B', 'B', 'S'}));
}

// --- The writer's refusals ------------------------------------------------------------

TEST(ItchStoreWriter, RefusesWhatNoFileHolds) {
    EXPECT_FALSE(ItchStoreWriter::in_memory({.segment_records = 0}).has_value());
    EXPECT_FALSE(
        ItchStoreWriter::in_memory({.segment_records = kMaxSegmentRecords + 1}).has_value());
    EXPECT_TRUE(ItchStoreWriter::in_memory({.segment_records = kMaxSegmentRecords}).has_value());

    auto writer = ItchStoreWriter::in_memory({.segment_records = 4});
    ASSERT_TRUE(writer.has_value());
    // A timestamp past 48 bits.
    auto late = std::get<itch::AddOrder>(test::add(1, 1, 1));
    late.header.timestamp_ns = kMax48 + 1;
    const auto refused_time = writer->append(late);
    ASSERT_FALSE(refused_time.has_value());
    EXPECT_EQ(refused_time.error().field, "timestamp");
    EXPECT_EQ(refused_time.error().record, 0U);

    // An empty raw record, and one that decode() would never return raw.
    EXPECT_FALSE(writer->append(itch::RawMessage{}).has_value());
    const auto& fixture = itch::test::fixture('S');
    const auto conforming = writer->append(itch::RawMessage{.bytes = fixture.bytes});
    ASSERT_FALSE(conforming.has_value());
    EXPECT_EQ(conforming.error().field, "raw record conforms");
    EXPECT_EQ(writer->records(), 0U);

    ASSERT_TRUE(writer->append(test::add(1, 1, 1)).has_value());
    ASSERT_TRUE(writer->finish({}).has_value());
    EXPECT_FALSE(writer->append(test::add(1, 2, 2)).has_value());
    EXPECT_FALSE(writer->finish({}).has_value());
    // The refusals changed nothing: the store holds the one record.
    const Bytes file = writer->take_bytes();
    const auto records = test::all_payloads(file);
    ASSERT_TRUE(records.has_value());
    EXPECT_EQ(records->size(), 1U);
}

TEST(ItchStoreWriter, TheCreateErrorNamesTheContainersFailure) {
    tick_store::test::ScratchDir dir;
    const auto refused =
        ItchStoreWriter::create(dir / "no" / "such" / "dir", Durability::page_cache, {});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ItchStoreErrorKind::container);
    EXPECT_TRUE(refused.error().container.has_value());
}

// --- Sizes ------------------------------------------------------------------------------

TEST(ItchStoreSizes, EveryByteOfTheFileIsCountedOnce) {
    const Bytes source = tick_store::test::source_with_tail();
    const Bytes file = test::store_of_source(source, 5);
    auto reader = ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    const StoreSizes sizes = reader->sizes();
    std::uint64_t total = sizes.header_bytes + sizes.trailer_bytes + sizes.footer_fixed_bytes +
                          sizes.block_entry_bytes + sizes.section_entry_bytes +
                          sizes.metadata_bytes + sizes.store_section_bytes +
                          sizes.symbols_section_bytes + sizes.tail_section_bytes;
    std::uint64_t metadata = 0;
    std::uint64_t blocks = 0;
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        total += sizes.column_payload_bytes[c];
        metadata += sizes.column_metadata_bytes[c];
        blocks += sizes.column_blocks[c];
    }
    EXPECT_EQ(total, file.size());
    EXPECT_EQ(sizes.file_bytes, file.size());
    EXPECT_EQ(metadata, sizes.metadata_bytes);
    EXPECT_EQ(blocks, reader->container().block_count());
    EXPECT_EQ(sizes.column_blocks[0], reader->segment_count());
    EXPECT_EQ(sizes.column_values[0], tick_store::test::kSourceRecordCount);
    EXPECT_EQ(sizes.tail_section_bytes, 4U);
    EXPECT_EQ(sizes.store_section_bytes, kStoreSectionSize);
}

}  // namespace
}  // namespace tick_store::store
