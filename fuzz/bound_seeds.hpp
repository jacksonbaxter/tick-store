#pragma once

// The costliest valid input each bounded fuzz target, and each codec target, takes at its
// -max_len, in time and in memory, as fuzz/README.md, "Bounds", derives them, and one
// maximal PFOR delta block that puts the delta kernel past the budget. generate_seeds
// writes each into its target's seeds under its name here, fuzz/bound_seeds.txt lists them so
// a runner can time each one alone, and every replay of the seeds (scripts/check.sh's fuzz
// steps, CI, a G-3 run) shows the bound again. FuzzRegression/BoundSeeds.PassTheWholeCheck
// replays each through its oracle with no bounds, so what the bounds leave out is still
// checked on them, and holds each to the side of the bounds it was built for.
//
// Every valid block here comes from the real encoder, and every store from the real writer
// (Z4). Where the costliest codec input is two maximal blocks followed by bytes for part 2,
// the fuzz target takes the first and stops at the second, past its cap, and the unit test
// takes both; the bytes for part 2 start with a block header stating no values, which every
// decoder refuses at once.

#include "codec_oracles.hpp"
#include "itch_query_oracles.hpp"
#include "itch_store_oracles.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/stream_vbyte.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/itch_store_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace tick_store::bound_seeds {

using Bytes = std::vector<std::byte>;
using Words = std::vector<std::uint64_t>;

// The -max_len each target runs at (fuzz/launch.txt).
inline constexpr std::size_t kSmallMaxLen = 16'384;
inline constexpr std::size_t kLargeMaxLen = 70'000;
inline constexpr std::size_t kQueryMaxLen = 131'072;

template <class Encode>
[[nodiscard]] Bytes encoded(Encode&& encode) {
    Bytes out;
    if (!encode(out).has_value()) {
        throw std::runtime_error("a bound seed's encoder refused its values");
    }
    return out;
}

// `count` copies of `block`.
[[nodiscard]] inline Bytes copies(const Bytes& block, std::size_t count) {
    Bytes out;
    for (std::size_t i = 0; i < count; ++i) {
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

[[nodiscard]] inline Bytes joined(const Bytes& first, const Bytes& second) {
    Bytes out = first;
    out.insert(out.end(), second.begin(), second.end());
    return out;
}

// `column`, then a block header stating no values, then pseudo-random bytes up to `max_len`,
// so part 2 has the most values the length holds, few of them alike, the last byte 0 so it
// takes blocks of one value: the most blocks, each paying an encoder's and a decoder's fixed
// cost, which costs part 2 more than any longer block does.
[[nodiscard]] inline Bytes with_part_two_tail(Bytes column, std::size_t max_len) {
    column.resize(column.size() + codec::kBlockHeaderSize, std::byte{0});
    std::uint64_t state = 0x9E37'79B9'7F4A'7C15ULL;
    while (column.size() < max_len) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        for (unsigned i = 0; i < 8 && column.size() < max_len; ++i) {
            column.push_back(static_cast<std::byte>((state >> (8U * i)) & 0xFFU));
        }
    }
    column.back() = std::byte{0};
    return column;
}

// The fuzz target takes the first block whole and stops at the second, past the cap; the
// unit test takes both. The first is the dearest block to decode the format allows in the
// fewest bytes: kMaxBlockValues values cycling down through 2,048, which the encoder writes
// as a sorted move-to-front block of kDictMtfMaxEntries entries whose every rank is the
// last, so the decoder moves 2,047 entries for each value, its most (dictionary.hpp), and
// the ranks' PFOR block is one width-0 run: 7,342 bytes. The second is a block of
// kMaxBlockValues equal values, one entry with codes of no bits, 31 bytes. Then the tail.
[[nodiscard]] inline Bytes dictionary_seed() {
    Words cycling(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < cycling.size(); ++i) {
        cycling[i] = (codec::kDictMtfMaxEntries - 1) - (i % codec::kDictMtfMaxEntries);
    }
    const Words equal(codec::kMaxBlockValues, 42);
    const Bytes moving = encoded([&](Bytes& out) { return codec::encode_dict_block(cycling, out); });
    const Bytes still = encoded([&](Bytes& out) { return codec::encode_dict_block(equal, out); });
    return with_part_two_tail(joined(moving, still), kSmallMaxLen);
}

// Two blocks of one run of kMaxBlockValues values, 48 bytes each, then the tail.
[[nodiscard]] inline Bytes run_length_seed() {
    const Words equal(codec::kMaxBlockValues, 42);
    const Bytes block = encoded([&](Bytes& out) { return codec::encode_rle_block(equal, out); });
    return with_part_two_tail(copies(block, 2), kSmallMaxLen);
}

// Two blocks of kMaxBlockValues values at width 0 in every vector, which the decoder turns
// back into a rising sequence by the inverse head delta and a scale, the dearest of the
// shapes timed (fuzz/README.md, "Bounds"): 6,159 bytes each. Then the tail.
[[nodiscard]] inline Bytes pfor_seed() {
    Words rising(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < rising.size(); ++i) {
        rising[i] = 7 * (1'000 + i);
    }
    const Bytes block = encoded([&](Bytes& out) {
        return codec::encode_pfor_block(rising,
                                        codec::PforOptions{.transform = codec::IntTransform::head_delta,
                                                           .adaptive_scale = true,
                                                           .patch = true},
                                        out);
    });
    return with_part_two_tail(copies(block, 2), kSmallMaxLen);
}

// Not the costliest: two blocks of kMaxBlockValues rising values by delta, each vector width
// 0, so the delta kernel sums every vector of a maximal block, past the budget, where only
// the kernel-against-scalar check can see a kernel's fault. None of the other PFOR seeds
// holds an accepted block of more than a few thousand values. Then the tail.
[[nodiscard]] inline Bytes pfor_delta_seed() {
    Words rising(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < rising.size(); ++i) {
        rising[i] = 1'000 + i;
    }
    const Bytes block = encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            rising,
            codec::PforOptions{
                .transform = codec::IntTransform::delta, .adaptive_scale = false, .patch = true},
            out);
    });
    return with_part_two_tail(copies(block, 2), kSmallMaxLen);
}

// The most values one block of at most `max_len` bytes holds, `encode` writing `count` of
// them: the block grows with the count, so the search halves the range.
template <class Encode>
[[nodiscard]] Bytes largest_block(std::size_t max_len, std::size_t most, Encode&& encode) {
    std::size_t low = 1;
    std::size_t high = most;
    while (low < high) {
        const std::size_t middle = low + ((high - low + 1) / 2);
        if (encoded([&](Bytes& out) { return encode(middle, out); }).size() <= max_len) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return encoded([&](Bytes& out) { return encode(low, out); });
}

// Stream VByte: one byte a value at least, so one block of one small value repeated, one
// byte and two control bits each, without delta, which costs more than with it.
[[nodiscard]] inline Bytes stream_vbyte_seed() {
    return largest_block(kLargeMaxLen, codec::kMaxBlockValues, [](std::size_t count, Bytes& out) {
        return codec::encode_svb_block(Words(count, 5), false, out);
    });
}

// Gorilla: one bit a value at least, so one block of a regular clock, whose delta-of-deltas
// are all zero.
[[nodiscard]] inline Bytes gorilla_timestamps_seed() {
    return largest_block(kLargeMaxLen, codec::kMaxBlockValues, [](std::size_t count, Bytes& out) {
        Words clock(count);
        for (std::size_t i = 0; i < count; ++i) {
            clock[i] = 34'200'000'000'000ULL + (i * 1'000);
        }
        return codec::encode_timestamp_block(clock, out);
    });
}

// And one block of one price repeated, whose XORs are all zero.
[[nodiscard]] inline Bytes gorilla_xor_seed() {
    return largest_block(kLargeMaxLen, codec::kMaxBlockValues, [](std::size_t count, Bytes& out) {
        return codec::encode_xor_block(Words(count, 0x4059'0000'0000'0000ULL), out);
    });
}

// A day-like stream for the query engine: adds, executions, cancels, deletes, replaces and
// trades over a few locates, system events on locate 0, ties in time and orders named
// across segments, enough records for several seek groups. generate_seeds' query seeds are
// days of it too.
[[nodiscard]] inline std::vector<itch::Message> day_messages(std::size_t records) {
    using itch::MessageHeader;
    using itch::Price4;
    constexpr auto ch = [](char c) { return static_cast<std::byte>(c); };
    constexpr auto stock = [](std::string_view text) {
        itch::Stock out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::byte>(text[i]);
        }
        return out;
    };
    const itch::Stock usual = stock("ZVZZT   ");
    const itch::Stock other = stock("OTHER   ");
    std::vector<itch::Message> out;
    out.reserve(records);
    std::uint64_t state = 11;
    std::vector<std::vector<std::uint64_t>> live(6);
    std::uint64_t next = 9'000;
    std::uint64_t time = 34'199'000'000'000ULL;
    for (std::size_t i = 0; i < records; ++i) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        time += (state >> 61U) == 0 ? 0 : (state >> 40U) % 2'000'000;
        const auto locate = static_cast<std::uint16_t>(1 + ((state >> 33U) % 5));
        const MessageHeader h{locate, static_cast<std::uint16_t>(i % 3), time};
        std::vector<std::uint64_t>& orders = live[locate];
        const std::uint64_t pick = (state >> 20U) % 16;
        const auto price = Price4{static_cast<std::uint32_t>(100'000 + ((state >> 44U) % 900))};
        if (pick == 15) {
            out.emplace_back(itch::SystemEvent{MessageHeader{0, 0, time}, ch('Q')});
            continue;
        }
        if (orders.empty() || pick < 6) {
            out.emplace_back(itch::AddOrder{h, next, (state & 1U) != 0 ? ch('B') : ch('S'),
                                            static_cast<std::uint32_t>(100 * (1 + (state >> 58U))),
                                            i % 1'000 == 999 ? other : usual, price});
            orders.push_back(next++);
            continue;
        }
        const std::size_t at = (state >> 8U) % orders.size();
        const std::uint64_t named = (state >> 7U) % 23 == 0 ? next + 77 : orders[at];
        if (pick < 9) {
            out.emplace_back(itch::OrderExecuted{h, named, 10, 90'000 + i});
        } else if (pick < 10) {
            out.emplace_back(itch::OrderExecutedWithPrice{h, named, 5, 90'000 + i, ch('Y'), price});
        } else if (pick < 11) {
            out.emplace_back(itch::OrderReplace{h, named, next, 100, price});
            orders[at] = next++;
        } else if (pick < 12) {
            out.emplace_back(itch::OrderCancel{h, named, 5});
        } else if (pick < 14) {
            out.emplace_back(itch::OrderDelete{h, named});
            orders.erase(orders.begin() + static_cast<std::ptrdiff_t>(at));
        } else {
            out.emplace_back(itch::Trade{h, named, ch('B'), 100, usual, price, 70'000 + i});
        }
    }
    return out;
}

// `messages` through the real writer in segments of `segment_records`.
[[nodiscard]] inline Bytes written(std::span<const itch::Message> messages,
                                   std::uint32_t segment_records) {
    auto writer = store::ItchStoreWriter::in_memory({.segment_records = segment_records});
    if (!writer.has_value()) {
        throw std::runtime_error("a bound seed's writer refused its options");
    }
    for (const itch::Message& message : messages) {
        if (!writer->append(message).has_value()) {
            throw std::runtime_error("a bound seed's writer refused a record");
        }
    }
    if (!writer->finish({}).has_value()) {
        throw std::runtime_error("a bound seed's writer refused to finish");
    }
    return writer->take_bytes();
}

// ITCH store: the most add orders -max_len holds beside one more segment, 3,840 in 60
// segments of kFuzzMaxSegmentRecords, then that segment: 64 raw records of 15,831 bytes, as
// many raw bytes as the cap leaves room for, zstd holding them in a few hundred bytes. In
// all 1,048,419 values and 68,990 bytes. Typed records cost the most a value, about 4.4
// microseconds on the Mac, but -max_len holds them to about 36,000 values; raw bytes cost
// about 0.37 and reach the cap in under 700 bytes (fuzz/README.md, "Bounds").
[[nodiscard]] inline Bytes itch_store_seed() {
    std::vector<itch::Message> records;
    constexpr std::uint32_t kAdds = 60 * store::oracle::kFuzzMaxSegmentRecords;
    records.reserve(std::size_t{kAdds} + store::oracle::kFuzzMaxSegmentRecords);
    for (std::uint32_t i = 0; i < kAdds; ++i) {
        records.emplace_back(itch::AddOrder{
            itch::MessageHeader{1, 0, 34'200'000'000'000ULL + i}, 1 + std::uint64_t{i},
            std::byte{'B'}, 100,
            itch::Stock{std::byte{'A'}, std::byte{'A'}, std::byte{'P'}, std::byte{'L'},
                        std::byte{' '}, std::byte{' '}, std::byte{' '}, std::byte{' '}},
            itch::Price4{1'000'000}});
    }
    const Bytes payload(15'831, std::byte{0});
    for (std::uint32_t i = 0; i < store::oracle::kFuzzMaxSegmentRecords; ++i) {
        records.emplace_back(
            itch::RawMessage{.bytes = payload, .reason = itch::RawReason::unknown_type});
    }
    return written(records, store::oracle::kFuzzMaxSegmentRecords);
}

// The day with a raw record of 9 bytes every `raw_every` records, so no segment is plain and
// the indexed path decodes every record it reaches, as much as the exact path does.
[[nodiscard]] inline Bytes raw_spotted_day(std::size_t records, std::size_t raw_every) {
    std::vector<itch::Message> day = day_messages(records);
    Bytes unknown(9, std::byte{0xA5});
    unknown.front() = std::byte{'z'};
    for (std::size_t i = 0; i < day.size(); i += raw_every) {
        day[i] = itch::RawMessage{.bytes = unknown, .reason = itch::RawReason::unknown_type};
    }
    return written(day, store::oracle::kQueryFuzzMaxSegmentRecords);
}

// ITCH query, past the budget: the most of that day the cap holds, 18,342 records with a
// raw one every 64, in one segment, 135,165 values in 129,649 bytes. Typed records outside
// plain segments cost the most a value, so this is dearer than add orders, denser in bytes,
// to the same cap, or raw bytes to it (fuzz/README.md, "Bounds").
[[nodiscard]] inline Bytes itch_query_seed() { return raw_spotted_day(18'342, 64); }

// ITCH query, within the budget: the most of the day with a raw record every 4,500 whose
// file states no more values than the budget, 9,504 records and 69,632 values.
[[nodiscard]] inline Bytes itch_query_budget_seed() { return raw_spotted_day(9'504, 4'500); }

// Which side of its target's bounds a seed was built for, which the unit test holds it to.
enum class Side : std::uint8_t {
    // A codec input whose first block holds kMaxBlockValues values: taken whole, then capped.
    first_block_maximal,
    // Codec targets without bounds: nothing to hold but -max_len.
    unbounded,
    // A store file stating no more values than the store cap.
    within_store_cap,
    // A store file for the query target within the cap and past the cross-check budget.
    past_query_budget,
    // One within the budget.
    within_query_budget,
};

struct BoundSeed {
    // The fuzz target, and its seeds' folder under build/fuzz/fuzz-seeds (fuzz/launch.txt).
    std::string_view target;
    std::string_view seeds;
    std::string_view name;
    std::size_t max_len;
    Side side;
    Bytes (*make)();
    // Its oracle with no bounds, as the unit suite runs it.
    const char* (*check)(std::span<const std::byte>);
};

// One line each in fuzz/bound_seeds.txt, in this order.
inline constexpr std::array<BoundSeed, 10> kBoundSeeds{{
    {"fuzz_dictionary", "dictionary", "bound-costliest", kSmallMaxLen, Side::first_block_maximal,
     &dictionary_seed, &codec::oracle::check_dict_column},
    {"fuzz_run_length", "run_length", "bound-costliest", kSmallMaxLen, Side::first_block_maximal,
     &run_length_seed, &codec::oracle::check_rle_column},
    {"fuzz_pfor", "pfor", "bound-costliest", kSmallMaxLen, Side::first_block_maximal, &pfor_seed,
     &codec::oracle::check_pfor_column},
    {"fuzz_pfor", "pfor", "bound-maximal-delta", kSmallMaxLen, Side::first_block_maximal,
     &pfor_delta_seed, &codec::oracle::check_pfor_column},
    {"fuzz_stream_vbyte", "stream_vbyte", "bound-costliest", kLargeMaxLen, Side::unbounded,
     &stream_vbyte_seed, &codec::oracle::check_svb_column},
    {"fuzz_gorilla_timestamps", "gorilla_timestamps", "bound-costliest", kLargeMaxLen,
     Side::unbounded, &gorilla_timestamps_seed, &codec::oracle::check_timestamp_column},
    {"fuzz_gorilla_xor", "gorilla_xor", "bound-costliest", kLargeMaxLen, Side::unbounded,
     &gorilla_xor_seed, &codec::oracle::check_xor_column},
    {"fuzz_itch_store", "itch_store", "bound-costliest", kLargeMaxLen, Side::within_store_cap,
     &itch_store_seed, &store::oracle::check_itch_store},
    {"fuzz_itch_query", "itch_query", "bound-costliest", kQueryMaxLen, Side::past_query_budget,
     &itch_query_seed, &store::oracle::check_itch_query},
    {"fuzz_itch_query", "itch_query", "bound-costliest-within-budget", kQueryMaxLen,
     Side::within_query_budget, &itch_query_budget_seed, &store::oracle::check_itch_query},
}};

}  // namespace tick_store::bound_seeds
