#pragma once

// The check fuzz_itch_query runs, kept apart from libFuzzer so the unit suite replays
// crashers through the same code. It returns nullptr if the input passed, or a
// description of the broken property.
//
// The input is a whole store file, read with every CRC its container structure points at
// recomputed (fixed_checksums from container_oracles.hpp), so the bytes reach the index
// decoders below the CRCs: the seek index, the locate runs and their directory, the plain
// flag, and every codec block the indexed path reads on its own. Whatever the bytes, every
// query must return an answer or an error, never crash or read out of bounds (the
// sanitizers judge that), and asking twice must give the same result.
//
// The oracle for answers: the indexed path may accept a file the whole decode refuses,
// since it checks only what it reads, but on a file the whole decode accepts it must give
// exactly what the exact path gives, which maps each record as tick_export does. So when
// SegmentReader accepts every segment, every rows query, minute aggregate, locate count and
// the share sum from the indexed path must equal the exact path's. The indexed answers
// must also be the same with the scalar kernel set forced on this thread.
//
// Both paths choose segments with the same summary test (may_hold) and seed a segment's
// raw rows with the same seed_before, so comparing them cannot see either go wrong. Each
// answer is therefore also checked against the column form itself, built here from the
// whole decode with no segment skipped: every record re-encoded and mapped by row_values(),
// a raw row taking the time of the row before it, and each request filtered from that.
//
// What that column form shares with the paths it checks (review RD-6): SegmentReader and
// the decoders below it, itch::encode, and row_values() with hot_fields() and
// fits_columns(), which the exact path maps through everywhere and the indexed path in
// every segment that is not plain. Only the type byte is read here from the payload
// itself, and a raw row's time from the row before it. So a fault in which form a typed
// record takes, or in a hot or rare row's fields, that shows only in segments that are not
// plain moves every answer alike and passes here; in a plain segment the indexed path
// reads its own columns and sees it. The store fuzzer's decode, re-encode and compare
// covers the decoders, and the column export's round-trip and golden tests rebuild typed
// records from a hot or rare row's fields; a raw row's type byte is not read back there,
// so it is read here.
//
// Requests come from the file: windows over its summaries' times, a one-nanosecond window
// at a segment's first time, and a window and a locate read from the input's last bytes,
// so the fuzzer can steer them; locates: every one, locate 0, the first locate of the
// first segment's bitmap, and the steered one.
//
// The segment size is capped at kQueryFuzzMaxSegmentRecords, a piece and a seek group and a
// few records, so a file reaches several groups, a short last one and a column's second
// piece while one input stays within libFuzzer's allocation limit.
//
// That cap leaves the work unbounded: 1,431 bytes hold a segment of 400 raw records of
// 65,535 bytes, 26 million values, and both paths decode every segment a request reaches
// for each request. And a day of typed records with a few raw ones, 131,072 bytes and
// 18,334 records, takes about 2.5 s on the Mac, every segment not plain, so the indexed path
// decodes as much as the exact one, three times over. So the fuzz target passes
// kQueryFuzzBounds: a cap on the values a file's segments state, a file stating more opened
// but not queried, and a budget on the repeat ask, the indexed path asked a second time on
// the kernel set. The ask with the scalar set forced takes every request on every file
// within the cap. Called without bounds, as the unit suite calls it,
// the check queries every file within the segment cap with every request on every check
// (fuzz/README.md, "Bounds").

#include "container_oracles.hpp"
#include "itch_store_oracles.hpp"
#include "tick_store/codec/simd.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_query.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace tick_store::store::oracle {

inline constexpr std::uint32_t kQueryFuzzMaxSegmentRecords = kPieceValues + kSeekGroupRecords + 7;

// How much of one file the check queries, in the work its directory alone says the check
// will take (query_work). The defaults query every file within the segment cap, with every
// request on every check.
struct QueryBounds {
    // The cap: a file whose work is at most this takes the whole check.
    std::uint64_t work = std::numeric_limits<std::uint64_t>::max();
    // The repeat ask, the indexed path asked again on the kernel set, takes every request
    // while the work is at most this, and the first kQueryPastBudgetRequests past it. Never
    // budgeted within the cap: each path's answers to every request, once, the indexed
    // path's answers with the scalar kernel set forced, and their comparisons with each
    // other and with the column form.
    std::uint64_t cross_checked_work = std::numeric_limits<std::uint64_t>::max();
    // The compare limit: past the cap, a file whose work is at most this is still asked
    // every request by the indexed path on the chosen kernel set and with the scalar set
    // forced, and the answers compared. Nothing else is checked on it, and past this
    // nothing is asked.
    std::uint64_t compared_work = std::numeric_limits<std::uint64_t>::max();
};

// The requests the repeat ask takes past the budget: the first window's, the whole file,
// with no locate and at locate 0, so every kind of answer (rows, locate counts, minute
// aggregates and the share sum) is still asked twice over every row.
inline constexpr std::size_t kQueryPastBudgetRequests = 2;

// What the check costs a file, from its directory: each segment a fixed amount and an amount
// for each value its columns state, more for a segment that is not plain, where both paths
// map every record and seed each raw row's time from the rows before it. In microseconds of
// the whole check (the repeat ask on two requests) on the Mac's fuzz build: the least
// weights above every file timed (fuzz/README.md, "Bounds"). Values are what the decoders
// may be asked for at most, since a column is decoded only to its stated count. Saturates.
inline constexpr std::uint64_t kQueryPlainSegmentWork = 8'000;
inline constexpr std::uint64_t kQueryPlainValueWork = 4;
inline constexpr std::uint64_t kQuerySegmentWork = 24'000;
inline constexpr std::uint64_t kQueryValueWork = 12;

[[nodiscard]] inline std::uint64_t query_work(const ItchStoreReader& reader) {
    constexpr std::uint64_t kMost = std::numeric_limits<std::uint64_t>::max();
    const auto add = [](std::uint64_t a, std::uint64_t b) { return b > kMost - a ? kMost : a + b; };
    const auto times = [](std::uint64_t a, std::uint64_t b) {
        return a != 0 && b > kMost / a ? kMost : a * b;
    };
    std::uint64_t work = 0;
    for (std::uint64_t s = 0; s < reader.segment_count(); ++s) {
        const SegmentEntry& segment = reader.segment(s);
        std::uint64_t values = 0;
        for (const std::uint32_t column : segment.values) {
            values = add(values, column);
        }
        work = segment.summary.plain
                   ? add(work, add(kQueryPlainSegmentWork, times(kQueryPlainValueWork, values)))
                   : add(work, add(kQuerySegmentWork, times(kQueryValueWork, values)));
    }
    return work;
}

// The fuzz target's bounds. PROVISIONAL: set from the measurements.
inline constexpr std::uint64_t kQueryFuzzWork = 500'000;
inline constexpr std::uint64_t kQueryFuzzCrossCheckedWork = 400'000;
inline constexpr std::uint64_t kQueryFuzzComparedWork = 1'000'000;
inline constexpr QueryBounds kQueryFuzzBounds{.work = kQueryFuzzWork,
                                              .cross_checked_work = kQueryFuzzCrossCheckedWork,
                                              .compared_work = kQueryFuzzComparedWork};

namespace query_detail {

struct Request {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::optional<std::uint16_t> locate;
};

struct Answers {
    std::vector<std::expected<std::vector<QueryRow>, ItchStoreError>> rows;
    std::vector<std::expected<std::vector<MinuteBucket>, ItchStoreError>> minutes;
    std::vector<std::expected<std::vector<std::uint64_t>, ItchStoreError>> counts;
    std::optional<std::expected<ColumnSum, ItchStoreError>> sum;

    friend bool operator==(const Answers&, const Answers&) = default;
};

[[nodiscard]] inline std::uint64_t tail_word(std::span<const std::byte> file, std::size_t from_end) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const std::size_t back = from_end + i + 1;
        if (back <= file.size()) {
            value = (value << 8U) | std::to_integer<std::uint64_t>(file[file.size() - back]);
        }
    }
    return value;
}

[[nodiscard]] inline std::vector<Request> requests_for(const ItchStoreReader& reader,
                                                       std::span<const std::byte> file) {
    std::uint64_t low = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t high = 0;
    std::optional<std::uint64_t> first_time;
    for (std::uint64_t k = 0; k < reader.segment_count(); ++k) {
        const SegmentSummary& summary = reader.segment(k).summary;
        if (summary.typed_records == 0) {
            continue;
        }
        low = std::min(low, summary.min_time);
        high = std::max(high, summary.max_time);
        if (!first_time.has_value() || k == reader.segment_count() / 2) {
            first_time = summary.first_time;
        }
    }
    if (low > high) {
        low = 0;
        high = 0;
    }
    const std::uint64_t steer = tail_word(file, 0);
    const std::uint64_t steered_start = low + (steer % (high - low + 1));
    const std::uint64_t steered_end = steered_start + (tail_word(file, 8) % ((high - low) + 2));
    std::vector<std::optional<std::uint16_t>> locates{std::nullopt, std::uint16_t{0},
                                                      static_cast<std::uint16_t>(steer >> 48U)};
    if (reader.segment_count() > 0) {
        const std::vector<std::byte>& bitmap = reader.segment(0).summary.locates;
        for (std::size_t byte = 0; byte < bitmap.size(); ++byte) {
            const auto bits = std::to_integer<unsigned>(bitmap[byte]);
            if (bits != 0) {
                unsigned bit = 0;
                while ((bits & (1U << bit)) == 0) {
                    ++bit;
                }
                locates.emplace_back(static_cast<std::uint16_t>((byte * 8) + bit));
                break;
            }
        }
    }
    const std::uint64_t at = first_time.value_or(low);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> windows{
        {0, std::numeric_limits<std::uint64_t>::max()},
        {at, at + 1},
        {low + ((high - low) / 2), high},
        {steered_start, steered_end},
    };
    std::vector<Request> out;
    for (const auto& [start, end] : windows) {
        for (const auto& locate : locates) {
            out.push_back({.start = start, .end = end, .locate = locate});
        }
    }
    return out;
}

[[nodiscard]] inline Answers answer(const ItchStoreReader& reader, QueryPath path,
                                    std::span<const Request> requests) {
    ItchQuery query{reader, path};
    Answers out;
    for (const Request& request : requests) {
        out.rows.push_back(query.rows(request.start, request.end, request.locate));
        if (request.locate.has_value()) {
            out.minutes.push_back(query.minute_aggregate(request.start, request.end, *request.locate));
        } else {
            out.counts.push_back(query.locate_counts(request.start, request.end));
        }
    }
    out.sum = query.shares_sum();
    return out;
}

// What the whole decode makes of the file: nothing if SegmentReader refuses a segment,
// else the column form of every record, or a record with no row.
struct ColumnForm {
    bool accepted = false;
    bool every_record_has_a_row = true;
    std::vector<QueryRow> rows;
};

// Every segment through SegmentReader, none skipped, each record mapped as tick_export
// maps it, a raw row repeating the time of the row before it (0 for the file's first).
[[nodiscard]] inline ColumnForm column_form(const ItchStoreReader& reader) {
    ColumnForm out;
    SegmentReader records;
    std::array<std::byte, itch::kMaxPayloadSize> scratch{};
    std::uint64_t previous = 0;
    for (std::uint64_t k = 0; k < reader.segment_count(); ++k) {
        if (!records.seek(reader, k).has_value()) {
            return {};
        }
        for (;;) {
            auto message = records.next();
            if (!message.has_value()) {
                return {};
            }
            if (!message->has_value()) {
                break;
            }
            const auto size = itch::encode(**message, scratch);
            const auto values = size.has_value()
                                    ? row_values(std::span<const std::byte>{scratch.data(), *size})
                                    : std::nullopt;
            if (!values.has_value()) {
                out.every_record_has_a_row = false;
                continue;
            }
            QueryRow row;
            row.seq = out.rows.size();
            // The type byte as sent, read here rather than from row_values(), which every
            // path maps through in a segment that is not plain (review RD-6).
            row.msg_type = std::to_integer<std::uint16_t>(scratch.front());
            if (values->form == RowForm::raw) {
                row.ts_ns = previous;
            } else {
                row.ts_ns = values->ts_ns;
                row.locate = values->locate;
                if (values->form == RowForm::hot) {
                    row.order_ref = values->order_ref;
                    row.side = values->side;
                    row.shares = static_cast<std::uint32_t>(values->shares);
                    row.price = static_cast<std::uint32_t>(values->price);
                    if (values->stock.size() == row.stock.size()) {
                        row.has_stock = true;
                        std::ranges::copy(values->stock, row.stock.begin());
                    }
                }
            }
            previous = row.ts_ns;
            out.rows.push_back(row);
        }
    }
    out.accepted = true;
    return out;
}

// The answers the column form gives each request, as the query semantics define them
// (itch_query.hpp): rows of a half-open window, a locate's add orders per minute, rows per
// locate, and every record's shares summed.
[[nodiscard]] inline Answers answer_from(const std::vector<QueryRow>& all,
                                        std::span<const Request> requests) {
    constexpr std::uint64_t kNanosPerMinute = 60ULL * 1'000'000'000ULL;
    Answers out;
    for (const Request& request : requests) {
        const auto in_window = [&](const QueryRow& row) {
            return row.ts_ns >= request.start && row.ts_ns < request.end;
        };
        std::vector<QueryRow> rows;
        for (const QueryRow& row : all) {
            if (in_window(row) && (!request.locate.has_value() || request.locate == row.locate)) {
                rows.push_back(row);
            }
        }
        out.rows.emplace_back(std::move(rows));
        if (request.locate.has_value()) {
            std::vector<MinuteBucket> minutes;
            for (const QueryRow& row : all) {
                if (!in_window(row) || request.locate != row.locate ||
                    (row.msg_type != 'A' && row.msg_type != 'F')) {
                    continue;
                }
                const std::uint64_t minute = (row.ts_ns / kNanosPerMinute) * kNanosPerMinute;
                auto slot = std::ranges::lower_bound(minutes, minute, {}, &MinuteBucket::minute_ns);
                if (slot == minutes.end() || slot->minute_ns != minute) {
                    slot = minutes.insert(slot, MinuteBucket{.minute_ns = minute,
                                                             .count = 0,
                                                             .shares = 0,
                                                             .min_price = row.price,
                                                             .max_price = row.price});
                }
                ++slot->count;
                slot->shares += row.shares;
                slot->min_price = std::min<std::uint64_t>(slot->min_price, row.price);
                slot->max_price = std::max<std::uint64_t>(slot->max_price, row.price);
            }
            out.minutes.emplace_back(std::move(minutes));
        } else {
            std::vector<std::uint64_t> counts(std::size_t{kMax16} + 1, 0);
            for (const QueryRow& row : all) {
                if (in_window(row)) {
                    ++counts[row.locate];
                }
            }
            out.counts.emplace_back(std::move(counts));
        }
    }
    ColumnSum sum{.records = all.size(), .sum = 0};
    for (const QueryRow& row : all) {
        sum.sum += row.shares;
    }
    out.sum = sum;
    return out;
}

// The answers to the first `count` requests, cut from the answers to them all.
[[nodiscard]] inline Answers first_answers(const Answers& all, std::span<const Request> requests,
                                           std::size_t count) {
    Answers out;
    std::size_t minutes = 0;
    std::size_t counts = 0;
    for (std::size_t i = 0; i < count; ++i) {
        out.rows.push_back(all.rows[i]);
        if (requests[i].locate.has_value()) {
            out.minutes.push_back(all.minutes[minutes++]);
        } else {
            out.counts.push_back(all.counts[counts++]);
        }
    }
    out.sum = all.sum;
    return out;
}

template <class T>
[[nodiscard]] bool all_answered(const std::vector<std::expected<T, ItchStoreError>>& answers) {
    return std::ranges::all_of(answers, [](const auto& one) { return one.has_value(); });
}

}  // namespace query_detail

[[nodiscard]] inline const char* check_itch_query(std::span<const std::byte> file,
                                                 const QueryBounds& bounds) {
    const detail::Bytes fixed = fixed_checksums(file);
    auto reader = ItchStoreReader::open_bytes(fixed, {.checksums = Checksums::verify});
    if (!reader.has_value()) {
        return nullptr;
    }
    if (reader->header().segment_records > kQueryFuzzMaxSegmentRecords) {
        return nullptr;
    }
    const std::uint64_t work = query_work(*reader);
    const auto requests = query_detail::requests_for(*reader, file);
    const std::span<const query_detail::Request> all{requests};
    if (work > bounds.work) {
        // Past the cap, up to the compare limit: every request asked of the indexed path by
        // the chosen kernel set and with the scalar set forced, and the answers compared.
        if (work > bounds.compared_work) {
            return nullptr;
        }
        const query_detail::Answers kernels = query_detail::answer(*reader, QueryPath::indexed, all);
        const codec::simd::ScalarKernelsOnThisThread scalar;
        return query_detail::answer(*reader, QueryPath::indexed, all) != kernels
                   ? "past the cap, the kernel set and the scalar set answered the same queries differently"
                   : nullptr;
    }
    const query_detail::Answers indexed = query_detail::answer(*reader, QueryPath::indexed, all);
    {
        // The kernel-against-scalar oracle, on every request: the same answers with the
        // scalar set forced. Never budgeted.
        const codec::simd::ScalarKernelsOnThisThread scalar;
        if (query_detail::answer(*reader, QueryPath::indexed, all) != indexed) {
            return "the kernel set and the scalar set answered the same queries differently";
        }
    }
    // The repeat ask takes every request within the budget, and the first few past it.
    const std::size_t crossed = work <= bounds.cross_checked_work
                                    ? requests.size()
                                    : std::min(requests.size(), kQueryPastBudgetRequests);
    if (query_detail::answer(*reader, QueryPath::indexed, all.first(crossed)) !=
        query_detail::first_answers(indexed, all, crossed)) {
        return "the indexed path answered the same queries differently twice";
    }
    const query_detail::ColumnForm form = query_detail::column_form(*reader);
    if (!form.accepted) {
        return nullptr;
    }
    if (!form.every_record_has_a_row) {
        return "a record the whole decode accepts has no row in the column form";
    }
    const query_detail::Answers exact = query_detail::answer(*reader, QueryPath::exact, all);
    if (!query_detail::all_answered(exact.rows) || !query_detail::all_answered(exact.minutes) ||
        !query_detail::all_answered(exact.counts) || !exact.sum.has_value() ||
        !exact.sum->has_value()) {
        return "the exact path refused a file the whole decode accepts";
    }
    const query_detail::Answers want = query_detail::answer_from(form.rows, all);
    if (exact.rows != want.rows) {
        return "the exact path's rows differ from the column form of the whole decode";
    }
    if (exact.minutes != want.minutes) {
        return "the exact path's minute aggregate differs from the column form's";
    }
    if (exact.counts != want.counts) {
        return "the exact path's locate counts differ from the column form's";
    }
    if (exact.sum != want.sum) {
        return "the exact path's share sum differs from the column form's";
    }
    if (indexed.rows != exact.rows) {
        return "the indexed path's rows differ from the exact path's on a file the whole decode accepts";
    }
    if (indexed.minutes != exact.minutes) {
        return "the indexed path's minute aggregate differs from the exact path's";
    }
    if (indexed.counts != exact.counts) {
        return "the indexed path's locate counts differ from the exact path's";
    }
    if (indexed.sum != exact.sum) {
        return "the indexed path's share sum differs from the exact path's";
    }
    return nullptr;
}

// The whole check, as the unit suite runs it.
[[nodiscard]] inline const char* check_itch_query(std::span<const std::byte> file) {
    return check_itch_query(file, QueryBounds{});
}

}  // namespace tick_store::store::oracle
