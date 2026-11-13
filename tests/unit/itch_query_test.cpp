// Queries over the ITCH store (store/itch_query.hpp) against the column form computed
// straight from the messages with row_values(), the mapping tick_export uses: windows at
// every boundary, locate filters, segments the exact path must answer, the minute aggregate,
// the share sum, index fields broken below the checksum, and the query worker.

#include "itch_store_fixture.hpp"
#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "tick_store/codec/simd.hpp"
#include "tick_store/harness/query_cli.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/itch_query.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <ostream>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::store {
namespace {

using test::Bytes;
using test::payload_of;
using test::stock_of;

constexpr std::uint64_t kStart = 34'199'000'000'000ULL;
constexpr std::uint64_t kNanosPerMinute = 60'000'000'000ULL;

// A stream of messages that looks like a day: adds, executions, cancels, deletes,
// replaces, trades and system events over a few locates, orders named across segments,
// stocks that change under a locate, ties in time. Raw payloads live in `raw`.
struct Day {
    std::vector<itch::Message> messages;
    std::deque<Bytes> raw;
};

struct DayOptions {
    std::size_t records = 30'000;
    std::uint64_t seed = 7;
    // Records to replace with something the column form keeps raw.
    std::vector<std::size_t> raw_at;
    std::vector<std::size_t> wide_shares_at;
    std::vector<std::size_t> unprintable_stock_at;
};

DayOptions day_options(std::size_t records, std::uint64_t seed) {
    DayOptions options;
    options.records = records;
    options.seed = seed;
    return options;
}

Day make_day(const DayOptions& options) {
    Day day;
    std::mt19937_64 random{options.seed};
    const auto below = [&](std::uint64_t n) { return random() % n; };
    std::map<std::uint16_t, std::vector<std::uint64_t>> live;
    std::uint64_t time = kStart;
    std::uint64_t next_ref = 1000;
    for (std::size_t i = 0; i < options.records; ++i) {
        // Ties now and then, and a burst at the minute boundary.
        time += below(4) == 0 ? 0 : below(30'000'000);
        const auto locate = static_cast<std::uint16_t>(1 + below(9));
        const auto stock = std::format("SYM{}", locate);
        std::vector<std::uint64_t>& orders = live[locate];
        const std::uint64_t pick = below(100);
        const auto shares = static_cast<std::uint32_t>(1 + below(5000));
        const auto price = static_cast<std::uint32_t>(10'0000 + below(5'000'000));
        const auto take_order = [&] -> std::uint64_t {
            if (orders.empty() || below(20) == 0) {
                // An order never seen, or created under another locate: an import.
                return 1 + below(next_ref);
            }
            return orders[below(orders.size())];
        };
        if (pick < 35 || orders.empty()) {
            const std::uint64_t ref = next_ref++;
            orders.push_back(ref);
            // Now and then the add carries a stock other than its locate's first.
            const std::string carried = below(200) == 0 ? "ODD" : stock;
            const char side = below(2) == 0 ? 'B' : 'S';
            if (below(8) == 0) {
                day.messages.emplace_back(itch::AddOrderWithMpid{
                    .header = test::header_at(locate, time),
                    .order_reference_number = ref,
                    .buy_sell_indicator = static_cast<std::byte>(side),
                    .shares = shares,
                    .stock = stock_of(carried),
                    .price = itch::Price4{price},
                    .attribution = {std::byte{'G'}, std::byte{'S'}, std::byte{'C'}, std::byte{'O'}}});
            } else {
                day.messages.push_back(test::add(locate, time, ref, carried, side, price, shares));
            }
        } else if (pick < 47) {
            day.messages.push_back(test::executed(locate, time, take_order(), shares % 300, i));
        } else if (pick < 51) {
            day.messages.emplace_back(itch::OrderExecutedWithPrice{
                .header = test::header_at(locate, time),
                .order_reference_number = take_order(),
                .executed_shares = shares % 200,
                .match_number = i,
                .printable = std::byte{'Y'},
                .execution_price = itch::Price4{price}});
        } else if (pick < 62) {
            day.messages.push_back(test::cancelled(locate, time, take_order(), shares % 100));
        } else if (pick < 78) {
            const std::uint64_t ref = take_order();
            day.messages.push_back(test::deleted(locate, time, ref));
            std::erase(orders, ref);
        } else if (pick < 88) {
            const std::uint64_t original = take_order();
            const std::uint64_t created = next_ref++;
            std::erase(orders, original);
            orders.push_back(created);
            day.messages.push_back(test::replaced(locate, time, original, created, price));
        } else if (pick < 94) {
            day.messages.emplace_back(itch::Trade{.header = test::header_at(locate, time),
                                               .order_reference_number = below(3) == 0 ? 0 : take_order(),
                                               .buy_sell_indicator = std::byte{'B'},
                                               .shares = shares,
                                               .stock = stock_of(stock),
                                               .price = itch::Price4{price},
                                               .match_number = i});
        } else if (pick < 97) {
            day.messages.emplace_back(itch::SystemEvent{.header = test::header_at(0, time),
                                                     .event_code = std::byte{'Q'}});
        } else {
            day.messages.push_back(test::directory(locate, time, stock));
        }
    }
    const auto raw_payload = [&](std::size_t i) {
        Bytes bytes = payload_of(day.messages[i]);
        bytes[0] = std::byte{'z'};  // no ITCH type
        day.raw.push_back(std::move(bytes));
        day.messages[i] = itch::RawMessage{.bytes = day.raw.back(),
                                           .reason = itch::RawReason::unknown_type};
    };
    for (const std::size_t i : options.raw_at) {
        raw_payload(i);
    }
    for (const std::size_t i : options.wide_shares_at) {
        const auto time_of = std::visit(
            [](const auto& m) -> std::uint64_t {
                if constexpr (requires { m.header; }) {
                    return m.header.timestamp_ns;
                } else {
                    return 0;
                }
            },
            day.messages[i]);
        day.messages[i] = test::add(3, time_of, next_ref++, "SYM3", 'S', 99'0000, 0x8000'0001U);
    }
    for (const std::size_t i : options.unprintable_stock_at) {
        auto message = test::add(4, kStart + i, next_ref++, "SYM4");
        std::get<itch::AddOrder>(message).stock[2] = std::byte{0x01};
        std::get<itch::AddOrder>(message).header.timestamp_ns =
            std::visit([](const auto& m) -> std::uint64_t {
                if constexpr (requires { m.header; }) {
                    return m.header.timestamp_ns;
                } else {
                    return kStart;
                }
            }, day.messages[i]);
        day.messages[i] = message;
    }
    return day;
}

// The column form of every message, as tick_export writes it: row_values(), with a raw
// row repeating the time before it.
std::vector<QueryRow> column_form(const Day& day) {
    std::vector<QueryRow> rows;
    std::uint64_t previous = 0;
    for (std::size_t i = 0; i < day.messages.size(); ++i) {
        const Bytes payload = payload_of(day.messages[i]);
        const auto values = row_values(payload);
        if (!values.has_value()) {
            ADD_FAILURE() << "message " << i << " has no row";
            continue;
        }
        QueryRow row;
        row.seq = i;
        row.msg_type = values->msg_type;
        if (values->form == RowForm::raw) {
            row.ts_ns = previous;
        } else {
            row.ts_ns = values->ts_ns;
            row.locate = static_cast<std::uint32_t>(values->locate);
            if (values->form == RowForm::hot) {
                row.order_ref = values->order_ref;
                row.side = values->side;
                row.shares = static_cast<std::uint32_t>(values->shares);
                row.price = static_cast<std::uint32_t>(values->price);
                if (!values->stock.empty()) {
                    row.has_stock = true;
                    std::ranges::copy(values->stock, row.stock.begin());
                }
            }
        }
        previous = row.ts_ns;
        rows.push_back(row);
    }
    return rows;
}

std::vector<QueryRow> window_of(const std::vector<QueryRow>& all, std::uint64_t start,
                                std::uint64_t end, std::optional<std::uint16_t> locate) {
    std::vector<QueryRow> out;
    for (const QueryRow& row : all) {
        if (row.ts_ns >= start && row.ts_ns < end && (!locate || row.locate == *locate)) {
            out.push_back(row);
        }
    }
    return out;
}

std::vector<MinuteBucket> minutes_of(const std::vector<QueryRow>& all, std::uint64_t start,
                                     std::uint64_t end, std::uint16_t locate) {
    std::map<std::uint64_t, MinuteBucket> buckets;
    for (const QueryRow& row : window_of(all, start, end, locate)) {
        if (row.msg_type != 'A' && row.msg_type != 'F') {
            continue;
        }
        const std::uint64_t minute = row.ts_ns / kNanosPerMinute;
        auto [at, fresh] = buckets.try_emplace(minute);
        if (fresh) {
            at->second = {.minute_ns = minute * kNanosPerMinute, .count = 0, .shares = 0,
                          .min_price = row.price, .max_price = row.price};
        }
        ++at->second.count;
        at->second.shares += row.shares;
        at->second.min_price = std::min<std::uint64_t>(at->second.min_price, row.price);
        at->second.max_price = std::max<std::uint64_t>(at->second.max_price, row.price);
    }
    std::vector<MinuteBucket> out;
    out.reserve(buckets.size());
    for (const auto& [minute, bucket] : buckets) {
        out.push_back(bucket);
    }
    return out;
}

struct Window {
    std::uint64_t start;
    std::uint64_t end;
};

// Windows at every edge the index has: segment and group boundaries, the first and last
// record, ties, empty and reversed windows, the whole day, and short windows throughout.
std::vector<Window> windows_of(const std::vector<QueryRow>& all, std::uint32_t segment_records) {
    std::vector<Window> out{{0, 0}, {kStart + 5, kStart + 5}, {kStart + 9, kStart + 3},
                            {0, ~std::uint64_t{0}}, {0, kStart}, {0, all.front().ts_ns + 1}};
    const std::uint64_t last = all.back().ts_ns;
    out.push_back({last, last + 1});
    out.push_back({last, last});
    out.push_back({last + 1, last + 1000});
    for (std::size_t i = 0; i < all.size(); ++i) {
        const bool segment_edge = i % segment_records == 0 || (i + 1) % segment_records == 0;
        const bool group_edge = (i % segment_records) % kSeekGroupRecords == 0 ||
                                ((i % segment_records) + 1) % kSeekGroupRecords == 0;
        if (segment_edge || group_edge) {
            const std::uint64_t t = all[i].ts_ns;
            out.push_back({t, t + 1});
            out.push_back({t - 1, t});
            out.push_back({t, t + 50'000'000});
            out.push_back({t > 40'000'000 ? t - 40'000'000 : 0, t + 1});
        }
    }
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint64_t t = all[(i * 7919U) % all.size()].ts_ns;
        out.push_back({t, t + 1'000'000});
        out.push_back({t, t + (60 * 1'000'000'000ULL)});
    }
    return out;
}

ItchStoreReader open_store(const Bytes& file) {
    auto reader = ItchStoreReader::open_bytes(file);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : to_string(reader.error()));
    return std::move(*reader);
}

// Every query of both paths against the column form, over all the windows.
void expect_every_query_matches(const Day& day, std::uint32_t segment_records,
                                const std::vector<std::optional<std::uint16_t>>& locates) {
    const Bytes file = test::store_of_messages(day.messages, segment_records);
    const ItchStoreReader reader = open_store(file);
    const std::vector<QueryRow> all = column_form(day);
    ItchQuery indexed{reader, QueryPath::indexed};
    ItchQuery exact{reader, QueryPath::exact};
    for (const Window& w : windows_of(all, segment_records)) {
        for (const auto& locate : locates) {
            const auto want = window_of(all, w.start, w.end, locate);
            const auto label = std::format("[{}, {}) locate {}", w.start, w.end,
                                           locate ? std::to_string(*locate) : "all");
            auto got = indexed.rows(w.start, w.end, locate);
            ASSERT_TRUE(got.has_value()) << label << ": " << to_string(got.error());
            ASSERT_EQ(*got, want) << "indexed " << label;
            EXPECT_EQ(indexed.stats().rows, want.size());
            auto plain = exact.rows(w.start, w.end, locate);
            ASSERT_TRUE(plain.has_value()) << label << ": " << to_string(plain.error());
            ASSERT_EQ(*plain, want) << "exact " << label;
            if (locate.has_value()) {
                const auto minutes = minutes_of(all, w.start, w.end, *locate);
                auto a = indexed.minute_aggregate(w.start, w.end, *locate);
                ASSERT_TRUE(a.has_value()) << label << ": " << to_string(a.error());
                ASSERT_EQ(*a, minutes) << "indexed minutes " << label;
                auto b = exact.minute_aggregate(w.start, w.end, *locate);
                ASSERT_TRUE(b.has_value()) << label;
                ASSERT_EQ(*b, minutes) << "exact minutes " << label;
            }
        }
        std::vector<std::uint64_t> counts(std::size_t{kMax16} + 1, 0);
        for (const QueryRow& row : window_of(all, w.start, w.end, std::nullopt)) {
            ++counts[row.locate];
        }
        auto a = indexed.locate_counts(w.start, w.end);
        ASSERT_TRUE(a.has_value());
        ASSERT_EQ(*a, counts);
        auto b = exact.locate_counts(w.start, w.end);
        ASSERT_TRUE(b.has_value());
        ASSERT_EQ(*b, counts);
    }
    ColumnSum want;
    want.records = all.size();
    for (const QueryRow& row : all) {
        want.sum += row.shares;
    }
    auto a = indexed.shares_sum();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, want);
    auto b = exact.shares_sum();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, want);
}

TEST(ItchQuery, BothPathsEqualTheColumnFormAtEveryBoundary) {
    const Day day = make_day({});
    // Three segments of three groups, the last group short, and a short last segment.
    expect_every_query_matches(day, (3 * kSeekGroupRecords) - 77,
                               {std::nullopt, std::uint16_t{0}, std::uint16_t{1},
                                std::uint16_t{5}, std::uint16_t{9}, std::uint16_t{4000}});
}

TEST(ItchQuery, OneRecordSegmentsAndOneSegmentFiles) {
    const Day small = make_day(day_options(300, 11));
    expect_every_query_matches(small, 1, {std::nullopt, std::uint16_t{0}, std::uint16_t{2}});
    expect_every_query_matches(small, kDefaultSegmentRecords,
                               {std::nullopt, std::uint16_t{0}, std::uint16_t{2}});
}

TEST(ItchQuery, SegmentsWithRawRowsTakeTheExactPath) {
    // A raw record first in the file (its time is 0), first in a segment (its time is the
    // last of the segment before), and in the middle; a hot record whose shares do not fit
    // their column, and one with a stock that is not printable.
    constexpr std::uint32_t kSegment = (2 * kSeekGroupRecords) + 5;
    DayOptions options = day_options((6 * kSegment) - 3, 3);
    options.raw_at = {0, kSegment, kSegment + 1, (3 * kSegment) + 17};
    options.wide_shares_at = {(5 * kSegment) + 100};
    options.unprintable_stock_at = {(4 * kSegment) - 1};
    const Day day = make_day(options);
    const Bytes file = test::store_of_messages(day.messages, kSegment);
    const ItchStoreReader reader = open_store(file);
    std::vector<bool> plain;
    plain.reserve(reader.segment_count());
    for (std::uint64_t k = 0; k < reader.segment_count(); ++k) {
        plain.push_back(reader.segment(k).summary.plain);
    }
    // Segment 2 is plain between two that are not, so the time a raw row of segment 3
    // repeats comes from its summary, and segment 1's from decoding segment 0.
    EXPECT_EQ(plain, (std::vector<bool>{false, false, true, false, true, false}));
    const std::vector<QueryRow> all = column_form(day);
    EXPECT_EQ(all[0].ts_ns, 0U);
    EXPECT_EQ(all[kSegment].ts_ns, all[kSegment - 1].ts_ns);
    expect_every_query_matches(day, kSegment, {std::nullopt, std::uint16_t{0}, std::uint16_t{3},
                                               std::uint16_t{4}});
    // A window holding only the time a raw row repeats from the segment before.
    ItchQuery query{reader};
    const std::uint64_t t = all[kSegment - 1].ts_ns;
    auto rows = query.rows(t, t + 1, std::uint16_t{0});
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(*rows, window_of(all, t, t + 1, std::uint16_t{0}));
    EXPECT_GT(query.stats().exact_segments, 0U);
}

TEST(ItchQuery, PlainFlagFollowsTheColumnForm) {
    const auto one = [](const itch::Message& message) {
        const std::vector<itch::Message> messages{message};
        const Bytes file = test::store_of_messages(messages);
        return open_store(file).segment(0).summary.plain;
    };
    EXPECT_TRUE(one(test::add(1, 5, 9, "AAPL")));
    EXPECT_FALSE(one(test::add(1, 5, 9, "AAPL", 'B', 0x8000'0000U)));
    EXPECT_FALSE(one(test::add(1, 5, 9, "AAPL", 'B', 100, 0x8000'0000U)));
    EXPECT_FALSE(one(test::add(1, 5, 0x8000'0000'0000'0000ULL, "AAPL")));
    EXPECT_FALSE(one(test::add(1, 5, 9, "AAPL", static_cast<char>(0x80))));
    EXPECT_FALSE(one(test::add(1, 5, 9, std::string_view{"A\x7f", 2})));
    EXPECT_TRUE(one(test::deleted(1, 5, 0x7FFF'FFFF'FFFF'FFFFULL)));
    EXPECT_FALSE(one(test::deleted(1, 5, 0x8000'0000'0000'0000ULL)));
}

TEST(ItchQuery, LocateZeroAndBusyLocatesAcrossGroups) {
    // Locate 0 holds only system events; locate 1 holds most of every group.
    std::vector<itch::Message> messages;
    std::uint64_t time = kStart;
    for (std::uint64_t i = 0; i < (3 * kSeekGroupRecords) + 10; ++i) {
        time += i % 3;
        if (i % 1000 == 0) {
            messages.emplace_back(itch::SystemEvent{.header = test::header_at(0, time),
                                                 .event_code = std::byte{'O'}});
        } else if (i % 2 == 0) {
            messages.push_back(test::add(1, time, 100 + i, "ONE", i % 4 == 0 ? 'B' : 'S',
                                         static_cast<std::uint32_t>(1000 + (i % 97)),
                                         static_cast<std::uint32_t>(i % 500)));
        } else {
            messages.push_back(test::deleted(1, time, 100 + i - 1));
        }
    }
    Day day;
    day.messages = std::move(messages);
    expect_every_query_matches(day, 2 * kSeekGroupRecords,
                               {std::nullopt, std::uint16_t{0}, std::uint16_t{1}});
}

TEST(ItchQuery, IndexedReadsLessThanExactForAShortWindow) {
    const Day day = make_day(day_options(40'000, 5));
    const Bytes file = test::store_of_messages(day.messages, 20'000);
    const ItchStoreReader reader = open_store(file);
    const std::vector<QueryRow> all = column_form(day);
    const std::uint64_t t = all[25'000].ts_ns;
    ItchQuery indexed{reader};
    ItchQuery exact{reader, QueryPath::exact};
    ASSERT_TRUE(indexed.rows(t, t + 1'000'000, std::nullopt).has_value());
    ASSERT_TRUE(exact.rows(t, t + 1'000'000, std::nullopt).has_value());
    EXPECT_EQ(indexed.stats().segments, 1U);
    EXPECT_EQ(indexed.stats().exact_segments, 0U);
    EXPECT_EQ(exact.stats().exact_segments, 1U);
    EXPECT_GT(indexed.stats().bytes_decoded, 0U);
    // Every locate reads each run from its first record to the window, which in a day of
    // nine locates is most of the segment; one locate reads its own run, though a zstd
    // piece inflates whole and here a piece is most of a column.
    const auto locate = static_cast<std::uint16_t>(all[25'000].locate);
    ASSERT_TRUE(indexed.rows(t, t + 1'000'000, locate).has_value());
    ASSERT_TRUE(exact.rows(t, t + 1'000'000, locate).has_value());
    EXPECT_EQ(indexed.stats().exact_segments, 0U);
    EXPECT_GT(indexed.stats().bytes_decoded, 0U);
    EXPECT_LT(indexed.stats().bytes_decoded, exact.stats().bytes_decoded);
    // A window before the day reads nothing.
    ASSERT_TRUE(indexed.rows(0, 1, std::nullopt).has_value());
    EXPECT_EQ(indexed.stats(), QueryStats{});
}

// --- Index fields broken below the checksum --------------------------------------------

Bytes store_for_corruption() {
    const Day day = make_day(day_options(9'000, 13));
    return test::store_of_messages(day.messages, 9'000);
}

// Every query on a file whose whole decode refuses it must fail or answer, never crash;
// these are run under the sanitizers.
void expect_queries_survive(const Bytes& file) {
    auto reader = ItchStoreReader::open_bytes(file);
    if (!reader.has_value()) {
        return;
    }
    ItchQuery query{*reader};
    std::ignore = query.rows(0, ~std::uint64_t{0}, std::nullopt);
    std::ignore = query.rows(0, ~std::uint64_t{0}, std::uint16_t{2});
    std::ignore = query.minute_aggregate(0, ~std::uint64_t{0}, 2);
    std::ignore = query.shares_sum();
    std::ignore = query.locate_counts(0, ~std::uint64_t{0});
}

std::expected<void, ItchStoreError> query_all(const Bytes& file, std::optional<std::uint16_t> locate) {
    auto reader = ItchStoreReader::open_bytes(file);
    if (!reader.has_value()) {
        return std::unexpected(reader.error());
    }
    ItchQuery query{*reader};
    auto rows = query.rows(0, ~std::uint64_t{0}, locate);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    return {};
}

TEST(ItchQueryCorrupt, SeekIndexRefusedByTheQuery) {
    const Bytes file = store_for_corruption();
    auto seek = test::column_values(file, 0, Column::seek);
    // A group count that does not sum to its records: parse_seek's rule, which the query
    // runs before it trusts any count.
    test::StoreParts parts = test::parts_of(file);
    seek[kSeekMaxima + (3 * seek_groups(9'000)) + 1] += 1;
    test::set_column(parts, 0, Column::seek, seek);
    const Bytes broken = parts.build();
    auto refused = query_all(broken, std::nullopt);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().column, "seek");
    expect_queries_survive(broken);
}

TEST(ItchQueryCorrupt, SeekTimesThatLieOnlyMissRowsTheWholeDecodeRefuses) {
    const Bytes file = store_for_corruption();
    auto seek = test::column_values(file, 0, Column::seek);
    // Group 1's maximum time pulled down to its minimum: still well formed, so the query
    // skips what the zone map rules out, and the whole decode names the lie.
    const std::uint64_t groups = seek_groups(9'000);
    seek[kSeekMaxima + groups + 1] = seek[kSeekMaxima + 1];
    test::StoreParts parts = test::parts_of(file);
    test::set_column(parts, 0, Column::seek, seek);
    const Bytes broken = parts.build();
    EXPECT_FALSE(test::all_payloads(broken).has_value());
    expect_queries_survive(broken);
}

TEST(ItchQueryCorrupt, LocateRunFieldsRefusedByTheQuery) {
    const Bytes file = store_for_corruption();
    const auto ids = test::column_values(file, 0, Column::locate_run);
    const auto starts = test::column_values(file, 0, Column::locate_run_start);
    const auto add_starts = test::column_values(file, 0, Column::locate_run_add_start);
    const auto import_starts = test::column_values(file, 0, Column::locate_run_import_start);
    const auto gaps = test::column_values(file, 0, Column::locate);
    ASSERT_GE(ids.size(), 3U);
    struct Case {
        std::string_view field;
        Column column;
        std::vector<std::uint64_t> values;
        std::optional<std::uint16_t> locate;
        // Ask for the locate's minute aggregate, which reads its adds alone, not its rows.
        bool minutes = false;
    };
    std::vector<Case> cases;
    {
        // Ids out of order: the all-locates path checks the directory whole.
        auto v = ids;
        std::swap(v[1], v[2]);
        cases.push_back({"locate run order", Column::locate_run, v, std::nullopt});
        // One locate's run read through a directory that names another there.
        cases.push_back({"locate run id", Column::locate_run, v, static_cast<std::uint16_t>(ids[1])});
    }
    {
        // A run ending past the typed records, by either path.
        auto v = starts;
        v[1] = gaps.size() + 1;
        cases.push_back({"locate run start", Column::locate_run_start, v,
                         static_cast<std::uint16_t>(ids[0])});
        cases.push_back({"locate run start", Column::locate_run_start, v, std::nullopt});
    }
    {
        // A run's adds starting one late: fewer than its records hold, refused before their
        // sides are read, for its rows and for its minute aggregate.
        auto v = add_starts;
        v[1] += 1;
        cases.push_back({"locate run add start", Column::locate_run_add_start, v,
                         static_cast<std::uint16_t>(ids[1])});
        cases.push_back({"locate run add start", Column::locate_run_add_start, v,
                         static_cast<std::uint16_t>(ids[1]), true});
    }
    {
        // A run's adds starting one early: room enough for its records, but not where the
        // records before the run put them.
        std::size_t r = 1;
        while (r + 1 < add_starts.size() && add_starts[r] == add_starts[r - 1]) {
            ++r;
        }
        ASSERT_LT(add_starts[r - 1], add_starts[r]);
        auto v = add_starts;
        v[r] -= 1;
        cases.push_back({"locate run add start", Column::locate_run_add_start, v,
                         static_cast<std::uint16_t>(ids[r])});
    }
    {
        // A run's imports ending one early, so its last import has no place: the next run's
        // start still ascends, so only the scan finds it.
        std::size_t r = 0;
        while (r + 2 < import_starts.size() && import_starts[r + 1] == import_starts[r]) {
            ++r;
        }
        ASSERT_LT(import_starts[r], import_starts[r + 1]);
        auto v = import_starts;
        v[r + 1] -= 1;
        cases.push_back({"locate run import start", Column::locate_run_import_start, v,
                         static_cast<std::uint16_t>(ids[r])});
    }
    {
        // Imports past the segment's.
        auto v = import_starts;
        v.back() += gaps.size();
        cases.push_back({"locate run import start", Column::locate_run_import_start, v,
                         std::nullopt});
    }
    {
        auto v = gaps;
        v[0] = 9'000;
        cases.push_back({"locate run index", Column::locate, v, static_cast<std::uint16_t>(ids[0])});
    }
    {
        auto v = gaps;
        v[0] = 9'000;
        cases.push_back({"locate run index", Column::locate, v, std::nullopt});
    }
    {
        // A time past 48 bits, by either path.
        auto v = test::column_values(file, 0, Column::timestamp);
        v[starts[1]] = kMax48;
        cases.push_back({"timestamp value", Column::timestamp, v,
                         static_cast<std::uint16_t>(ids[1])});
        cases.push_back({"timestamp value", Column::timestamp, v, std::nullopt});
    }
    for (const Case& c : cases) {
        test::StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, c.column, c.values);
        const Bytes broken = parts.build();
        if (c.minutes) {
            auto reader = ItchStoreReader::open_bytes(broken);
            ASSERT_TRUE(reader.has_value()) << c.field;
            ItchQuery query{*reader};
            ASSERT_TRUE(c.locate.has_value()) << c.field;
            auto refused = query.minute_aggregate(0, ~std::uint64_t{0}, c.locate.value());
            ASSERT_FALSE(refused.has_value()) << c.field;
            EXPECT_EQ(refused.error().field, c.field) << to_string(refused.error());
        } else {
            auto refused = query_all(broken, c.locate);
            ASSERT_FALSE(refused.has_value()) << c.field;
            EXPECT_EQ(refused.error().field, c.field) << to_string(refused.error());
        }
        expect_queries_survive(broken);
    }
}

TEST(ItchQueryCorrupt, PlainSegmentWithARawRecordRefused) {
    // The summary says plain, but the type column holds a raw record's 0: the indexed path
    // names it, and the whole decode refuses the counts.
    const Bytes file = store_for_corruption();
    auto types = test::column_values(file, 0, Column::type);
    types[17] = 0;
    test::StoreParts parts = test::parts_of(file);
    test::set_column(parts, 0, Column::type, types);
    const Bytes broken = parts.build();
    const auto refused = query_all(broken, std::nullopt);
    ASSERT_FALSE(refused.has_value());
    EXPECT_FALSE(test::all_payloads(broken).has_value());
    expect_queries_survive(broken);
}

TEST(ItchQueryCorrupt, EveryColumnTruncatedOrEmptiedSurvives) {
    const Bytes file = store_for_corruption();
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        const auto column = static_cast<Column>(c);
        test::StoreParts parts = test::parts_of(file);
        const auto found = parts.find_block(0, column);
        if (!found.has_value()) {
            continue;
        }
        auto values = test::column_values(file, 0, column);
        if (values.size() > 1) {
            values.pop_back();
            test::set_column(parts, 0, column, values);
            expect_queries_survive(parts.build());
        }
        test::StoreParts cut = test::parts_of(file);
        cut.blocks[*found].bytes.resize(cut.blocks[*found].bytes.size() / 2);
        expect_queries_survive(cut.build());
    }
}

// --- The query worker ---------------------------------------------------------------

TEST(QueryCli, MidnightOfADayLabel) {
    EXPECT_EQ(harness::midnight_ns_of("12302019"), 1'577'664'000'000'000'000LL);
    EXPECT_EQ(harness::midnight_ns_of("01302020"), 1'580'342'400'000'000'000LL);
    EXPECT_FALSE(harness::midnight_ns_of("13302019").has_value());
    EXPECT_FALSE(harness::midnight_ns_of("02302019").has_value());
    EXPECT_FALSE(harness::midnight_ns_of("1230201").has_value());
    EXPECT_FALSE(harness::midnight_ns_of("12x02019").has_value());
}

// compare/queries.py's digest of rows, written out again here from its definition.
std::string expected_digest(const std::vector<QueryRow>& rows, std::int64_t midnight) {
    harness::Sha256 hash;
    const auto name = [&](std::string_view n) { hash.update(std::as_bytes(std::span{n.data(), n.size()})); };
    const auto column = [&](std::string_view n, auto field) {
        name(n);
        for (const QueryRow& row : rows) {
            const auto value = static_cast<std::int64_t>(field(row));
            std::array<std::byte, 8> le{};
            for (std::size_t i = 0; i < 8; ++i) {
                le[i] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFFU);
            }
            hash.update(le);
        }
    };
    column("seq", [](const QueryRow& r) { return r.seq; });
    column("ts", [&](const QueryRow& r) { return static_cast<std::int64_t>(r.ts_ns) + midnight; });
    column("msg_type", [](const QueryRow& r) { return r.msg_type; });
    column("locate", [](const QueryRow& r) { return r.locate; });
    column("order_ref", [](const QueryRow& r) { return r.order_ref; });
    column("side", [](const QueryRow& r) { return r.side; });
    column("shares", [](const QueryRow& r) { return r.shares; });
    column("price", [](const QueryRow& r) { return r.price; });
    name("stock");
    for (const QueryRow& row : rows) {
        if (row.has_stock) {
            hash.update(row.stock);
        }
        hash.update(std::array<std::byte, 1>{std::byte{0}});
    }
    return harness::to_hex(hash.finish());
}

std::string json_field(std::string_view line, std::string_view key) {
    const std::string token = std::format("\"{}\":", key);
    const auto at = line.find(token);
    if (at == std::string_view::npos) {
        return {};
    }
    auto start = at + token.size();
    std::size_t end = 0;
    if (line[start] == '"') {
        ++start;
        end = line.find('"', start);
    } else if (line[start] == '[') {
        int depth = 0;
        for (end = start; end < line.size(); ++end) {
            if (line[end] == '[') {
                ++depth;
            } else if (line[end] == ']') {
                --depth;
            }
            if (depth == 0) {
                ++end;
                break;
            }
        }
    } else {
        end = line.find_first_of(",}", start);
    }
    return std::string{line.substr(start, end - start)};
}

TEST(QueryCli, ServesEveryKindWithTheHarnessDigests) {
    const ::tick_store::test::ScratchDir dir;
    const Day day = make_day(day_options(12'000, 21));
    const Bytes file = test::store_of_messages(day.messages, 5'000);
    const auto path = dir / "day.tsc";
    ::tick_store::test::write_file(path, file);
    const std::vector<QueryRow> all = column_form(day);
    const std::uint64_t t = all[6'000].ts_ns;
    const std::int64_t midnight = 1'577'664'000'000'000'000LL;
    std::istringstream in{std::format(
        "{{\"name\":\"Q1\",\"kind\":\"rows\",\"start_ns\":{},\"end_ns\":{},\"locate\":-1,\"text\":\"a \\\"b\\\" \\u00e9\"}}\n"
        "\n"
        "{{\"name\":\"Q2\",\"kind\":\"rows\",\"start_ns\":0,\"end_ns\":{},\"locate\":3}}\n"
        "{{\"name\":\"Q3\",\"kind\":\"minute_aggregate\",\"start_ns\":0,\"end_ns\":{},\"locate\":3}}\n"
        "{{\"name\":\"Q5\",\"kind\":\"column_sum\",\"start_ns\":0,\"end_ns\":0,\"locate\":-1}}\n"
        "not json\n"
        "{{\"kind\":\"busiest_locate\",\"start_ns\":0,\"end_ns\":{}}}\n"
        "{{\"kind\":\"quit\"}}\n"
        "{{\"name\":\"after\",\"kind\":\"column_sum\"}}\n",
        t, t + 200'000'000, t, t, std::numeric_limits<std::int64_t>::max())};
    std::ostringstream out;
    harness::CliOutput output;
    const std::string store_path = path.string();
    const std::vector<std::string_view> arguments{store_path, "--day", "12302019"};
    ASSERT_EQ(harness::run_query_cli(arguments, in, out, output), harness::kExitPass) << output.err;
    std::vector<std::string> lines;
    std::istringstream answers{out.str()};
    for (std::string line; std::getline(answers, line);) {
        lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 8U) << out.str();
    EXPECT_EQ(json_field(lines[0], "ready"), "true");
    EXPECT_EQ(lines[0].find('/'), std::string::npos);
    // What the harness reads back as settings in force.
    EXPECT_EQ(json_field(lines[0], "store_version"), "3");
    EXPECT_EQ(json_field(lines[0], "segment_records"), "5000");
    EXPECT_EQ(json_field(lines[0], "records"), std::to_string(day.messages.size()));
    const codec::simd::KernelLevel level = codec::simd::active_kernels().level;
    EXPECT_EQ(json_field(lines[0], "kernel_level"), codec::simd::to_string(level));
    EXPECT_EQ(json_field(lines[0], "kernel_best"),
              codec::simd::to_string(codec::simd::best_kernel_level()));
    // Each answer counts the kernel calls of its own request: only the chosen set's.
    const auto kernel_runs = [](std::string_view line) {
        const auto at = line.find("\"kernel_runs\":{");
        return at == std::string_view::npos
                   ? std::string{}
                   : std::string{line.substr(at, line.find('}', at) + 1 - at)};
    };
    for (const std::size_t answer : {1U, 2U, 3U, 4U}) {
        const std::string runs = kernel_runs(lines[answer]);
        ASSERT_FALSE(runs.empty()) << lines[answer];
        for (const codec::simd::KernelLevel other : codec::simd::kAllKernelLevels) {
            if (other != level) {
                EXPECT_TRUE(runs.contains(std::format("\"{}\":0", codec::simd::to_string(other))))
                    << lines[answer];
            }
        }
    }
    EXPECT_FALSE(kernel_runs(lines[1])
                     .contains(std::format("\"{}\":0", codec::simd::to_string(level))))
        << "Q1 decodes PFOR timestamps, so the chosen set ran: " << lines[1];

    const auto q1 = window_of(all, t, t + 200'000'000, std::nullopt);
    EXPECT_EQ(json_field(lines[1], "rows"), std::to_string(q1.size()));
    EXPECT_EQ(json_field(lines[1], "sha256"), expected_digest(q1, midnight));
    const auto q2 = window_of(all, 0, t, std::uint16_t{3});
    EXPECT_EQ(json_field(lines[2], "rows"), std::to_string(q2.size()));
    EXPECT_EQ(json_field(lines[2], "sha256"), expected_digest(q2, midnight));

    std::string minutes = "[";
    for (const MinuteBucket& b : minutes_of(all, 0, t, 3)) {
        minutes += std::format("{}[{},{},{},{},{}]", minutes.size() > 1 ? "," : "",
                               static_cast<std::int64_t>(b.minute_ns) + midnight, b.count, b.shares,
                               b.min_price, b.max_price);
    }
    minutes += ']';
    EXPECT_EQ(json_field(lines[3], "answer"), minutes);
    EXPECT_EQ(json_field(lines[3], "sha256"),
              harness::to_hex(harness::sha256(std::as_bytes(std::span{minutes.data(), minutes.size()}))));
    std::uint64_t shares = 0;
    for (const QueryRow& row : all) {
        shares += row.shares;
    }
    EXPECT_EQ(json_field(lines[4], "answer"), std::format("[[{},{}]]", all.size(), shares));
    EXPECT_EQ(json_field(lines[4], "rows"), "1");
    EXPECT_NE(json_field(lines[5], "error"), "");

    std::vector<std::uint64_t> counts(65'536, 0);
    for (const QueryRow& row : all) {
        ++counts[row.locate];
    }
    counts[0] = 0;
    const auto best = static_cast<std::size_t>(std::ranges::max_element(counts) - counts.begin());
    EXPECT_EQ(json_field(lines[6], "locate"), std::to_string(best));
    EXPECT_EQ(json_field(lines[7], "kind"), "quit");
    for (const std::string& line : lines) {
        EXPECT_EQ(line.find(dir.path().string()), std::string::npos);
    }
}

// With "ack", the line that stops the harness's clock comes before the digest, and agrees with
// the full answer after it.
TEST(QueryCli, AckComesFirstAndAgreesWithTheAnswer) {
    const ::tick_store::test::ScratchDir dir;
    const Day day = make_day(day_options(6'000, 5));
    const Bytes file = test::store_of_messages(day.messages, 2'000);
    const auto path = dir / "day.tsc";
    ::tick_store::test::write_file(path, file);
    std::istringstream in{
        "{\"name\":\"r\",\"kind\":\"rows\",\"start_ns\":0,\"end_ns\":9000000000000000,\"ack\":true}\n"
        "{\"name\":\"m\",\"kind\":\"minute_aggregate\",\"start_ns\":0,\"end_ns\":9000000000000000,"
        "\"locate\":3,\"ack\":true}\n"
        "{\"name\":\"s\",\"kind\":\"column_sum\",\"ack\":true}\n"
        "{\"name\":\"n\",\"kind\":\"column_sum\",\"ack\":false}\n"
        "{\"name\":\"e\",\"kind\":\"minute_aggregate\",\"ack\":true}\n"};
    std::ostringstream out;
    harness::CliOutput output;
    const std::string store_path = path.string();
    const std::vector<std::string_view> arguments{store_path, "--day", "12302019"};
    ASSERT_EQ(harness::run_query_cli(arguments, in, out, output), harness::kExitPass) << output.err;
    std::vector<std::string> lines;
    std::istringstream answers{out.str()};
    for (std::string line; std::getline(answers, line);) {
        lines.push_back(line);
    }
    // ready, then two lines for each acknowledged request, one for the rest.
    ASSERT_EQ(lines.size(), 1U + 2U + 2U + 2U + 1U + 1U) << out.str();
    for (const std::size_t first : {1U, 3U, 5U}) {
        EXPECT_EQ(json_field(lines[first], "answered"), "true") << lines[first];
        EXPECT_EQ(json_field(lines[first], "sha256"), "") << lines[first];
        EXPECT_EQ(json_field(lines[first], "name"), json_field(lines[first + 1], "name"));
        EXPECT_EQ(json_field(lines[first], "rows"), json_field(lines[first + 1], "rows"));
        EXPECT_EQ(json_field(lines[first], "engine_ns"), json_field(lines[first + 1], "engine_ns"));
        EXPECT_NE(json_field(lines[first + 1], "sha256"), "") << lines[first + 1];
    }
    EXPECT_EQ(std::to_string(column_form(day).size()), json_field(lines[1], "rows"));
    EXPECT_EQ(json_field(lines[7], "name"), "n");
    EXPECT_NE(json_field(lines[7], "sha256"), "");
    EXPECT_EQ(json_field(lines[8], "answered"), "");
    EXPECT_NE(json_field(lines[8], "error"), "");
}

TEST(QueryCli, BusiestLocateTakesTheLowestOnATie) {
    const ::tick_store::test::ScratchDir dir;
    // Locates 7 and 5 tie with two rows each, 9 has one, and locate 0 has three, which
    // does not count.
    const std::vector<itch::Message> messages{
        itch::SystemEvent{.header = test::header_at(0, 10), .event_code = std::byte{'O'}},
        test::add(7, 11, 1, "S"), test::add(9, 12, 2, "N"), test::add(5, 13, 3, "F"),
        itch::SystemEvent{.header = test::header_at(0, 14), .event_code = std::byte{'S'}},
        test::deleted(7, 15, 1), test::deleted(5, 16, 3),
        itch::SystemEvent{.header = test::header_at(0, 17), .event_code = std::byte{'Q'}}};
    const Bytes file = test::store_of_messages(messages);
    const auto path = dir / "tie.tsc";
    ::tick_store::test::write_file(path, file);
    for (const std::string_view which : {"indexed", "exact"}) {
        std::istringstream in{"{\"kind\":\"busiest_locate\",\"start_ns\":0,\"end_ns\":100}\n"
                              "{\"kind\":\"busiest_locate\",\"start_ns\":16,\"end_ns\":17}\n"
                              "{\"kind\":\"busiest_locate\",\"start_ns\":0,\"end_ns\":0}\n"};
        std::ostringstream out;
        harness::CliOutput output;
        const std::string store_path = path.string();
        const std::vector<std::string_view> arguments{store_path, "--day", "12302019", "--path", which};
        ASSERT_EQ(harness::run_query_cli(arguments, in, out, output), harness::kExitPass);
        std::istringstream answers{out.str()};
        std::vector<std::string> lines;
        for (std::string line; std::getline(answers, line);) {
            lines.push_back(line);
        }
        ASSERT_EQ(lines.size(), 4U);
        EXPECT_EQ(json_field(lines[1], "locate"), "5") << which;
        EXPECT_EQ(json_field(lines[2], "locate"), "5") << which;
        EXPECT_EQ(json_field(lines[3], "locate"), "0") << which;
        EXPECT_EQ(json_field(lines[3], "locate_rows"), "0") << which;
    }
}

TEST(QueryCli, UsageAndStoresThatDoNotOpen) {
    const ::tick_store::test::ScratchDir dir;
    const auto run = [](std::vector<std::string_view> arguments) {
        std::istringstream in;
        std::ostringstream out;
        harness::CliOutput output;
        return harness::run_query_cli(arguments, in, out, output);
    };
    EXPECT_EQ(run({}), harness::kExitUsageOrIo);
    EXPECT_EQ(run({"x.tsc"}), harness::kExitUsageOrIo);
    EXPECT_EQ(run({"x.tsc", "--day", "2019-12-30"}), harness::kExitUsageOrIo);
    EXPECT_EQ(run({"x.tsc", "--day", "12302019", "--path", "fast"}), harness::kExitUsageOrIo);
    EXPECT_EQ(run({"x.tsc", "y.tsc", "--day", "12302019"}), harness::kExitUsageOrIo);
    const std::string missing = (dir / "missing.tsc").string();
    EXPECT_EQ(run({missing, "--day", "12302019"}), harness::kExitMismatch);
}

// A stream every write to which fails, as a closed pipe or a full disk would.
class FailingBuffer : public std::streambuf {
protected:
    int_type overflow(int_type /*c*/) override { return traits_type::eof(); }
    std::streamsize xsputn(const char* /*s*/, std::streamsize /*n*/) override { return 0; }
};

// The worker's answers are its output: if they cannot be written, it must not exit 0.
TEST(QueryCli, AnswersThatCannotBeWrittenAreAnError) {
    const ::tick_store::test::ScratchDir dir;
    const std::vector<itch::Message> messages{test::add(1, 5, 9, "AAPL")};
    const auto path = dir / "one.tsc";
    ::tick_store::test::write_file(path, test::store_of_messages(messages));
    std::istringstream in{"{\"kind\":\"column_sum\"}\n{\"kind\":\"column_sum\"}\n"};
    FailingBuffer failing;
    std::ostream out{&failing};
    harness::CliOutput output;
    const std::string store_path = path.string();
    const std::vector<std::string_view> arguments{store_path, "--day", "12302019"};
    EXPECT_EQ(harness::run_query_cli(arguments, in, out, output), harness::kExitUsageOrIo);
    EXPECT_TRUE(output.err.contains("cannot write")) << output.err;
}

TEST(QueryCli, RequestsOutOfRangeAnswerWithAnError) {
    const ::tick_store::test::ScratchDir dir;
    const std::vector<itch::Message> messages{test::add(1, 5, 9, "AAPL")};
    const Bytes file = test::store_of_messages(messages);
    const auto path = dir / "one.tsc";
    ::tick_store::test::write_file(path, file);
    std::istringstream in{
        "{\"kind\":\"rows\",\"start_ns\":-1,\"end_ns\":5}\n"
        "{\"kind\":\"rows\",\"start_ns\":0,\"end_ns\":5,\"locate\":70000}\n"
        "{\"kind\":\"minute_aggregate\",\"start_ns\":0,\"end_ns\":5}\n"
        "{\"kind\":\"histogram\"}\n"
        "{\"kind\":\"rows\",\"start_ns\":99999999999999999999}\n"
        "{\"kind\":\"rows\",\"start_ns\":5,\"end_ns\":6,\"locate\":1} trailing\n"
        // A field present with the wrong type, or a window bound missing, is an error, never
        // a default: 0 for a time, or -1, every locate.
        "{\"kind\":\"rows\",\"start_ns\":\"34200000000000\",\"end_ns\":5}\n"
        "{\"kind\":\"rows\",\"start_ns\":0,\"end_ns\":5,\"locate\":true}\n"
        "{\"kind\":\"rows\",\"start_ns\":0,\"end_ns\":5,\"locate\":null}\n"
        "{\"kind\":\"rows\",\"start_ns\":0,\"end_ns\":5,\"locate\":\"1\"}\n"
        "{\"kind\":\"rows\",\"end_ns\":5}\n"
        "{\"kind\":\"busiest_locate\",\"start_ns\":0}\n"
        "{\"kind\":\"minute_aggregate\",\"start_ns\":0,\"locate\":1}\n"
        "{\"kind\":\"rows\",\"start_ns\":5,\"end_ns\":6,\"locate\":1,\"flag\":true,\"none\":null}\n"};
    std::ostringstream out;
    harness::CliOutput output;
    const std::string store_path = path.string();
    const std::vector<std::string_view> arguments{store_path, "--day", "12302019"};
    ASSERT_EQ(harness::run_query_cli(arguments, in, out, output), harness::kExitPass);
    std::istringstream answers{out.str()};
    std::vector<std::string> lines;
    for (std::string line; std::getline(answers, line);) {
        lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 15U);
    for (std::size_t i = 1; i <= 13; ++i) {
        EXPECT_NE(json_field(lines[i], "error"), "") << i << ": " << lines[i];
    }
    EXPECT_EQ(json_field(lines[14], "rows"), "1");
}

}  // namespace
}  // namespace tick_store::store
