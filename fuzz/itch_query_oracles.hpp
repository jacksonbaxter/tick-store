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
// decodes as much as the exact one, three times over; and a file of small segments states
// a few thousand values and still takes half a second, each segment's columns decoded on
// their own for every request. So the fuzz target passes kQueryFuzzBounds, in the work a
// file's directory says each part of the check will take (query_work): a cap, within which
// a file takes the whole check; a budget on the repeat ask, the indexed path asked a second
// time on the kernel set; and past the cap, a compare limit, within which the indexed path
// is asked every request on the kernel set and with the scalar set forced and the answers
// compared, and past which the file is opened and not queried. Within the cap the ask with
// the scalar set forced takes every request. Called without bounds, as the unit suite calls
// it, the check queries every file within the segment cap with every request on every
// check (fuzz/README.md, "Bounds").

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

// What a part of the check costs a file, estimated from its directory alone: an amount for
// each piece (one column's codec block in one segment) and for each value the pieces state,
// more in a segment that is not plain, where both paths map every record through the column
// form and seed each raw row's time from the rows before it. Values are what the decoders may
// be asked for at most, since a column is decoded only to its stated count. Saturates.
//
// The weights are nanoseconds of one execution through fuzz_itch_query on the Mac (fuzz build,
// -detect_leaks=0, load1 under 5, the median of three runs), fitted to 70 files timed through
// builds of the target at other bounds: days of typed records with a raw one every 4 or 64
// records or none, at segment sizes from 1 to the cap and at full and a quarter -max_len, raw
// records of 1,000 bytes, and files built against an earlier measure, days of one record type
// each (deletes, cancels, executions, system events), the types stating the fewest values a
// record. For each pair (plain, other) the weights are the pair with the least total over the
// files that puts every file's time at or below its work, rounded up to two figures; the files
// on that envelope are the seeds bound_seeds.hpp builds.
struct WorkWeights {
    std::uint64_t plain_piece;
    std::uint64_t plain_value;
    std::uint64_t piece;
    std::uint64_t value;
};

// The whole check with the repeat ask on two requests: on the envelope, a day of system events
// in segments of 16 (1,074 ms, 0.996 of its work), the day of 4,587 typed records in one plain
// segment (205 ms, 0.993) and a day of identical system events in segments of one (0.990).
inline constexpr WorkWeights kWholeCheckWork{
    .plain_piece = 610'000, .plain_value = 5'600, .piece = 1'600'000, .value = 15'000};
// The indexed path asked every request on the kernel set and with the scalar set forced: on
// the envelope, the day with a raw record every 4 in segments of one record (452 ms, 0.999)
// and the plain day of 4,587 records (33 ms, 0.999).
inline constexpr WorkWeights kComparedAsksWork{
    .plain_piece = 24'000, .plain_value = 960, .piece = 2'600'000, .value = 8'100};

[[nodiscard]] inline std::uint64_t query_work(const ItchStoreReader& reader, const WorkWeights& weights) {
    constexpr std::uint64_t kMost = std::numeric_limits<std::uint64_t>::max();
    const auto add = [](std::uint64_t a, std::uint64_t b) { return b > kMost - a ? kMost : a + b; };
    const auto times = [](std::uint64_t a, std::uint64_t b) {
        return a != 0 && b > kMost / a ? kMost : a * b;
    };
    std::uint64_t work = 0;
    for (std::uint64_t s = 0; s < reader.segment_count(); ++s) {
        const SegmentEntry& segment = reader.segment(s);
        std::uint64_t pieces = 0;
        std::uint64_t values = 0;
        for (std::size_t c = 0; c < segment.values.size(); ++c) {
            pieces = add(pieces, segment.pieces[c]);
            values = add(values, segment.values[c]);
        }
        const bool plain = segment.summary.plain;
        work = add(work, add(times(plain ? weights.plain_piece : weights.piece, pieces),
                             times(plain ? weights.plain_value : weights.value, values)));
    }
    return work;
}

// How much of one file the check queries, in the work its directory says each part will take.
// The defaults query every file within the segment cap, with every request on every check.
struct QueryBounds {
    // The cap, in kWholeCheckWork: a file whose work is at most this takes the whole check.
    std::uint64_t work = std::numeric_limits<std::uint64_t>::max();
    // The budget, in kWholeCheckWork: the repeat ask, the indexed path asked again on the
    // kernel set, takes every request while the work is at most this, and the first
    // kQueryPastBudgetRequests past it. Never budgeted within the cap: each path's answers to
    // every request, once, the indexed path's answers with the scalar kernel set forced, and
    // their comparisons with each other and with the column form.
    std::uint64_t cross_checked_work = std::numeric_limits<std::uint64_t>::max();
    // The compare limit, in kComparedAsksWork: past the cap, a file whose work in that
    // measure is at most this is still asked every request by the indexed path on the chosen
    // kernel set and with the scalar set forced, and the answers compared. Nothing else is
    // checked on it, and past this nothing is asked.
    std::uint64_t compared_work = std::numeric_limits<std::uint64_t>::max();
};

// The requests the repeat ask takes past the budget: the first window's, the whole file,
// with no locate and at locate 0, so every kind of answer (rows, locate counts, minute
// aggregates and the share sum) is still asked twice over every row.
inline constexpr std::size_t kQueryPastBudgetRequests = 2;

// The fuzz target's bounds. The bar is the ruling's: the costliest input takes at most 5 s an
// execution on the Linux host with its sibling cpu busy (fuzz/README.md, "Bounds"). These
// values were set to a design proxy for it, 0.5 s an execution on the Mac, and are to be set
// again from timings on the Linux host: the cap and the compare limit are 0.45 s of their
// measures, so a file whose time is up to a ninth above the envelope of the files timed still
// meets the proxy. Within the budget the whole check takes the repeat ask on every request, at
// most 1.19 times the work on the files timed (the day with a raw record every 64 in one
// segment), so the budget is 0.375 s: 445 ms at most.
inline constexpr std::uint64_t kQueryFuzzWork = 450'000'000;
inline constexpr std::uint64_t kQueryFuzzCrossCheckedWork = 375'000'000;
inline constexpr std::uint64_t kQueryFuzzComparedWork = 450'000'000;
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
            // The adds in the window, sorted by minute and then folded, so the cost is the
            // sort's whatever order the times come in (REVIEW-2, B5 (b)).
            std::vector<const QueryRow*> adds;
            for (const QueryRow& row : all) {
                if (in_window(row) && request.locate == row.locate &&
                    (row.msg_type == 'A' || row.msg_type == 'F')) {
                    adds.push_back(&row);
                }
            }
            const auto minute_of = [](const QueryRow* row) {
                return (row->ts_ns / kNanosPerMinute) * kNanosPerMinute;
            };
            std::ranges::stable_sort(adds, {}, minute_of);
            std::vector<MinuteBucket> minutes;
            for (const QueryRow* row : adds) {
                const std::uint64_t minute = minute_of(row);
                if (minutes.empty() || minutes.back().minute_ns != minute) {
                    minutes.push_back(MinuteBucket{.minute_ns = minute,
                                                   .count = 0,
                                                   .shares = 0,
                                                   .min_price = row->price,
                                                   .max_price = row->price});
                }
                MinuteBucket& bucket = minutes.back();
                ++bucket.count;
                bucket.shares += row->shares;
                bucket.min_price = std::min<std::uint64_t>(bucket.min_price, row->price);
                bucket.max_price = std::max<std::uint64_t>(bucket.max_price, row->price);
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
    const std::uint64_t work = query_work(*reader, kWholeCheckWork);
    const auto requests = query_detail::requests_for(*reader, file);
    const std::span<const query_detail::Request> all{requests};
    if (work > bounds.work) {
        // Past the cap, up to the compare limit: every request asked of the indexed path by
        // the chosen kernel set and with the scalar set forced, and the answers compared.
        if (query_work(*reader, kComparedAsksWork) > bounds.compared_work) {
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
