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
// the share sum from the indexed path must equal the exact path's.
//
// Requests come from the file: windows over its summaries' times, a one-nanosecond window
// at a segment's first time, and a window and a locate read from the input's last bytes,
// so the fuzzer can steer them; locates: every one, locate 0, the first locate of the
// first segment's bitmap, and the steered one.
//
// The segment size is capped at kQueryFuzzMaxSegmentRecords, three seek groups and a few
// records, so a file reaches several groups and a short last one while one input stays
// within libFuzzer's limits.

#include "container_oracles.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_query.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace tick_store::store::oracle {

inline constexpr std::uint32_t kQueryFuzzMaxSegmentRecords = (3 * kSeekGroupRecords) + 7;

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
                                    const std::vector<Request>& requests) {
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

// Whether SegmentReader accepts every segment of the file.
[[nodiscard]] inline bool whole_decode_accepts(const ItchStoreReader& reader) {
    SegmentReader records;
    for (std::uint64_t k = 0; k < reader.segment_count(); ++k) {
        if (!records.seek(reader, k).has_value()) {
            return false;
        }
        for (;;) {
            auto message = records.next();
            if (!message.has_value()) {
                return false;
            }
            if (!message->has_value()) {
                break;
            }
        }
    }
    return true;
}

template <class T>
[[nodiscard]] bool all_answered(const std::vector<std::expected<T, ItchStoreError>>& answers) {
    return std::ranges::all_of(answers, [](const auto& one) { return one.has_value(); });
}

}  // namespace query_detail

[[nodiscard]] inline const char* check_itch_query(std::span<const std::byte> file) {
    const detail::Bytes fixed = fixed_checksums(file);
    auto reader = ItchStoreReader::open_bytes(fixed, {.checksums = Checksums::verify});
    if (!reader.has_value()) {
        return nullptr;
    }
    if (reader->header().segment_records > kQueryFuzzMaxSegmentRecords) {
        return nullptr;
    }
    const auto requests = query_detail::requests_for(*reader, file);
    const query_detail::Answers indexed = query_detail::answer(*reader, QueryPath::indexed, requests);
    if (query_detail::answer(*reader, QueryPath::indexed, requests) != indexed) {
        return "the indexed path answered the same queries differently twice";
    }
    if (!query_detail::whole_decode_accepts(*reader)) {
        return nullptr;
    }
    const query_detail::Answers exact = query_detail::answer(*reader, QueryPath::exact, requests);
    if (!query_detail::all_answered(exact.rows) || !query_detail::all_answered(exact.minutes) ||
        !query_detail::all_answered(exact.counts) || !exact.sum.has_value() ||
        !exact.sum->has_value()) {
        return "the exact path refused a file the whole decode accepts";
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

}  // namespace tick_store::store::oracle
