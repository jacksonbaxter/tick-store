#pragma once

// The costliest valid input each bounded fuzz target, and each codec target, takes at its
// -max_len, in time and in memory, as fuzz/README.md, "Bounds", derives them: for each
// bounded target the dearest input its tiers allow, timed whole, and inputs that put a
// kernel or a check in a named tier. generate_seeds writes each into its target's seeds
// under its name here, fuzz/bound_seeds.txt lists them so a runner can time each one alone,
// and every replay of the seeds (scripts/check.sh's fuzz steps, CI, a G-3 run) shows the
// bound again. FuzzRegression/BoundSeeds.PassTheWholeCheck replays each through its oracle
// with no bounds, so what the bounds leave out is still checked on them, and holds each to
// the side of the bounds it was built for: for a codec seed, how many of its blocks fall in
// each tier.
//
// Every block here comes from the real encoder or is a nested block of the real encoder's
// framed by hand where the encoder would choose another form, and every store comes from
// the real writer (Z4). A codec input ends with bytes for part 2 that start with a block
// header stating no values, which every decoder refuses at once.

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

#include <algorithm>
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

// The block frame of block.hpp around `payload`: its count and payload size, u32
// little-endian, for blocks the encoders would never write.
[[nodiscard]] inline Bytes framed(std::uint64_t count, const Bytes& payload) {
    Bytes out;
    for (const std::uint64_t field : {count, std::uint64_t{payload.size()}}) {
        for (unsigned i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>((field >> (8U * i)) & 0xFFU));
        }
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// The nested PFOR block of `values` under `transform`, with the scale.
[[nodiscard]] inline Bytes nested_pfor(std::span<const std::uint64_t> values,
                                       codec::IntTransform transform, bool scale) {
    return encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            values, codec::PforOptions{.transform = transform, .adaptive_scale = scale, .patch = true},
            out);
    });
}

// The dearest dictionary block of `count` values to decode: values cycling down through
// 2,048, which the encoder writes as a sorted move-to-front block of kDictMtfMaxEntries
// entries whose every rank is the last, so the decoder moves 2,047 entries for each value,
// its most (dictionary.hpp). Its ranks block is then written again under head delta with the
// scale, which the decoder undoes through the inverse transform and the scale on top, the
// dearest of the ranks' shapes timed (fuzz/README.md, "Bounds").
[[nodiscard]] inline Bytes move_to_front_block(std::size_t count) {
    constexpr std::size_t kEntriesAt = 5;
    Words cycling(count);
    for (std::size_t i = 0; i < count; ++i) {
        cycling[i] = (codec::kDictMtfMaxEntries - 1) - (i % codec::kDictMtfMaxEntries);
    }
    const Bytes moving = encoded([&](Bytes& out) { return codec::encode_dict_block(cycling, out); });
    // The payload: mode, entry count, the dictionary's block, then the ranks' block.
    const std::span<const std::byte> payload = std::span{moving}.subspan(codec::kBlockHeaderSize);
    Words dictionary;
    const auto dictionary_bytes = codec::decode_pfor_block(payload.subspan(kEntriesAt), dictionary);
    if (!dictionary_bytes.has_value()) {
        throw std::runtime_error("a bound seed's dictionary block did not decode");
    }
    Bytes rewritten(payload.begin(),
                    payload.begin() + static_cast<std::ptrdiff_t>(kEntriesAt + *dictionary_bytes));
    const Words ranks(count, codec::kDictMtfMaxEntries - 1);
    const Bytes ranks_block = nested_pfor(ranks, codec::IntTransform::head_delta, true);
    rewritten.insert(rewritten.end(), ranks_block.begin(), ranks_block.end());
    return framed(count, rewritten);
}

// The dictionary target's costliest input: that block filling the cap, 2^18 values (1,458
// bytes), whose first 5,123 take the budget, then one of the 786,432 values left to 2^20
// (4,018 bytes), past the cap and so decoded by both kernel sets and compared; then the tail.
// The unit test takes both whole.
[[nodiscard]] inline Bytes dictionary_seed() {
    const std::size_t cap = codec::oracle::kDictFuzzBounds.decoded_values;
    return with_part_two_tail(
        joined(move_to_front_block(cap),
               move_to_front_block(codec::oracle::kDictFuzzBounds.compared_values - cap)),
        kSmallMaxLen);
}

// A run-length block of `values` values in `runs` maximal runs, the first runs - 1 of one value
// each and the last the rest, hand-built with the dearest nested blocks timed: run values
// rising by 7 under head delta with the scale, the lengths less one under delta of delta.
[[nodiscard]] inline Bytes run_length_runs_block(std::size_t values, std::size_t runs) {
    Words run_values(runs);
    for (std::size_t i = 0; i < runs; ++i) {
        run_values[i] = 7 * (1'000 + i);
    }
    Words lengths(runs, 0);
    lengths.back() = values - runs;
    Bytes payload;
    for (unsigned i = 0; i < 4; ++i) {
        payload.push_back(static_cast<std::byte>((runs >> (8U * i)) & 0xFFU));
    }
    const Bytes value_block = nested_pfor(run_values, codec::IntTransform::head_delta, true);
    const Bytes length_block = nested_pfor(lengths, codec::IntTransform::delta_of_delta, false);
    payload.insert(payload.end(), value_block.begin(), value_block.end());
    payload.insert(payload.end(), length_block.begin(), length_block.end());
    return framed(values, payload);
}

// The run-length target's costliest input: a block of kMaxBlockValues runs of one value,
// the dearest to decode (11,302 bytes), within the cap; then a maximal block with as many
// runs as the bytes left hold beside a block header, 467,968, past the cap and so decoded by
// both kernel sets and compared, dearer there than eight one-run blocks; then the tail, here
// a header alone.
[[nodiscard]] inline Bytes run_length_seed() {
    const Bytes dear = run_length_runs_block(codec::kMaxBlockValues, codec::kMaxBlockValues);
    std::size_t low = 1;
    std::size_t high = codec::kMaxBlockValues;
    while (low < high) {
        const std::size_t middle = low + ((high - low + 1) / 2);
        if (dear.size() + run_length_runs_block(codec::kMaxBlockValues, middle).size() +
                codec::kBlockHeaderSize <
            kSmallMaxLen) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return with_part_two_tail(joined(dear, run_length_runs_block(codec::kMaxBlockValues, low)),
                              kSmallMaxLen);
}

// The PFOR target's costliest input: of the shapes timed at width 0 the two dearest maximal
// blocks, 7 i^2 under delta of delta with the scale (6,414 bytes), within the cap, then 2^20
// values rising by 7 under head delta with the scale (6,159 bytes), past it and so decoded
// by both kernel sets and compared; then the tail.
[[nodiscard]] inline Bytes pfor_seed() {
    Words square(codec::kMaxBlockValues);
    Words rising(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < square.size(); ++i) {
        square[i] = 7 * i * i;
        rising[i] = 7 * (1'000 + i);
    }
    return with_part_two_tail(
        joined(nested_pfor(square, codec::IntTransform::delta_of_delta, true),
               nested_pfor(rising, codec::IntTransform::head_delta, true)),
        kSmallMaxLen);
}

// For the fault the kernel-against-scalar check past the cap must see: a first block that
// takes fuzz_pfor's cap exactly, no transform, so no delta pass decodes it; then a maximal
// block of rising values by delta, each vector width 0, which the target decodes by both
// kernel sets and compares, its delta pass summing every vector; then the tail.
[[nodiscard]] inline Bytes pfor_delta_past_cap_seed() {
    const std::size_t first_values =
        std::min(std::size_t{codec::kMaxBlockValues}, codec::oracle::kPforFuzzBounds.decoded_values);
    const Bytes zeros = encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            Words(first_values, 0),
            codec::PforOptions{
                .transform = codec::IntTransform::none, .adaptive_scale = false, .patch = true},
            out);
    });
    Words rising(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < rising.size(); ++i) {
        rising[i] = 1'000 + i;
    }
    const Bytes delta = encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            rising,
            codec::PforOptions{
                .transform = codec::IntTransform::delta, .adaptive_scale = false, .patch = true},
            out);
    });
    return with_part_two_tail(joined(zeros, delta), kSmallMaxLen);
}

// Refused: a run-length block of one value a run, as many runs as fuzz_run_length's cap
// takes in one block, whose run values rise by 7 except that run 100,000 repeats the one
// before, so its runs are not maximal and the decoder refuses it ("rle run value"). The run
// values are one delta block with a scale, one exception patched, and the lengths, all one,
// a width-0 block: 11,438 bytes. With that refusal taken out, only the never-budgeted check
// of a block's run count against its values can see it, 100,000 values past the budget.
[[nodiscard]] inline Bytes run_length_repeat_seed() {
    constexpr std::size_t kRepeatAt = 100'000;
    const std::size_t runs =
        std::min(std::size_t{codec::kMaxBlockValues}, codec::oracle::kRleFuzzBounds.decoded_values);
    Words values(runs);
    for (std::size_t i = 0; i < runs; ++i) {
        values[i] = 7 * (1'000 + i);
    }
    values.at(kRepeatAt) = values.at(kRepeatAt - 1);
    const Bytes run_values = encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            values,
            codec::PforOptions{
                .transform = codec::IntTransform::delta, .adaptive_scale = true, .patch = true},
            out);
    });
    const Bytes lengths = encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            Words(runs, 0),
            codec::PforOptions{
                .transform = codec::IntTransform::none, .adaptive_scale = false, .patch = true},
            out);
    });
    Bytes payload;
    for (unsigned i = 0; i < 4; ++i) {
        payload.push_back(static_cast<std::byte>((runs >> (8U * i)) & 0xFFU));
    }
    payload.insert(payload.end(), run_values.begin(), run_values.end());
    payload.insert(payload.end(), lengths.begin(), lengths.end());
    return framed(runs, payload);
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
[[nodiscard]] inline Bytes adds_then_raw_segment(std::size_t raw_bytes) {
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
    const Bytes payload(raw_bytes, std::byte{0});
    for (std::uint32_t i = 0; i < store::oracle::kFuzzMaxSegmentRecords; ++i) {
        records.emplace_back(
            itch::RawMessage{.bytes = payload, .reason = itch::RawReason::unknown_type});
    }
    return written(records, store::oracle::kFuzzMaxSegmentRecords);
}

[[nodiscard]] inline Bytes itch_store_seed() { return adds_then_raw_segment(15'831); }

// ITCH store, past the cap: the same add orders, then 64 raw records of 64,985 bytes, as many
// as the compare limit leaves room for, 4,194,275 values in all. Past the cap a file is only
// read twice; raw bytes are the only way past the cap in -max_len, so the dearest file there
// holds as many typed records as the bytes left allow beside raw bytes to the limit.
[[nodiscard]] inline Bytes itch_store_compared_seed() { return adds_then_raw_segment(64'985); }

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
    // A codec input of a bounded target: its blocks fall in the tiers `tiers` gives.
    codec_tiers,
    // Codec targets without bounds: nothing to hold but -max_len.
    unbounded,
    // A store file stating no more values than the store cap.
    within_store_cap,
    // A store file past the cap and within the compare limit.
    store_compared,
    // A query file within the cap and past the cross-check budget.
    past_query_budget,
    // One within the budget.
    within_query_budget,
    // One past the cap and within the compare limit.
    query_compared,
};

// How many of a codec seed's accepted blocks, taken in order, fall in each tier of its fuzz
// target's bounds (codec_oracles.hpp), or that its first block is refused, its stated count
// within the cap, so the fuzz target takes it as a block within the cap.
struct CodecTiers {
    std::size_t within_cap = 0;
    std::size_t compared = 0;
    std::size_t decoded_once = 0;
    std::size_t not_examined = 0;
    bool first_refused_within_cap = false;

    friend bool operator==(const CodecTiers&, const CodecTiers&) = default;
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
    // For Side::codec_tiers: the codec, the fuzz target's bounds and the tiers.
    const codec::oracle::CodecUnderTest* codec = nullptr;
    const codec::oracle::OracleBounds* bounds = nullptr;
    CodecTiers tiers{};
};

// One line each in fuzz/bound_seeds.txt, in this order.
inline constexpr std::array<BoundSeed, 12> kBoundSeeds{{
    {"fuzz_dictionary", "dictionary", "bound-costliest", kSmallMaxLen, Side::codec_tiers,
     &dictionary_seed, &codec::oracle::check_dict_column, &codec::oracle::kDictCodec,
     &codec::oracle::kDictFuzzBounds, CodecTiers{.within_cap = 1, .compared = 1}},
    {"fuzz_run_length", "run_length", "bound-costliest", kSmallMaxLen, Side::codec_tiers,
     &run_length_seed, &codec::oracle::check_rle_column, &codec::oracle::kRleCodec,
     &codec::oracle::kRleFuzzBounds, CodecTiers{.within_cap = 1, .compared = 1}},
    {"fuzz_run_length", "run_length", "bound-repeated-run-value", kSmallMaxLen,
     Side::codec_tiers, &run_length_repeat_seed, &codec::oracle::check_rle_column,
     &codec::oracle::kRleCodec, &codec::oracle::kRleFuzzBounds,
     CodecTiers{.first_refused_within_cap = true}},
    {"fuzz_pfor", "pfor", "bound-costliest", kSmallMaxLen, Side::codec_tiers, &pfor_seed,
     &codec::oracle::check_pfor_column, &codec::oracle::kPforCodec,
     &codec::oracle::kPforFuzzBounds, CodecTiers{.within_cap = 1, .compared = 1}},
    {"fuzz_pfor", "pfor", "bound-delta-past-cap", kSmallMaxLen, Side::codec_tiers,
     &pfor_delta_past_cap_seed, &codec::oracle::check_pfor_column, &codec::oracle::kPforCodec,
     &codec::oracle::kPforFuzzBounds, CodecTiers{.within_cap = 1, .compared = 1}},
    {"fuzz_stream_vbyte", "stream_vbyte", "bound-costliest", kLargeMaxLen, Side::unbounded,
     &stream_vbyte_seed, &codec::oracle::check_svb_column},
    {"fuzz_gorilla_timestamps", "gorilla_timestamps", "bound-costliest", kLargeMaxLen,
     Side::unbounded, &gorilla_timestamps_seed, &codec::oracle::check_timestamp_column},
    {"fuzz_gorilla_xor", "gorilla_xor", "bound-costliest", kLargeMaxLen, Side::unbounded,
     &gorilla_xor_seed, &codec::oracle::check_xor_column},
    {"fuzz_itch_store", "itch_store", "bound-costliest", kLargeMaxLen, Side::within_store_cap,
     &itch_store_seed, &store::oracle::check_itch_store},
    {"fuzz_itch_store", "itch_store", "bound-costliest-compared", kLargeMaxLen,
     Side::store_compared, &itch_store_compared_seed, &store::oracle::check_itch_store},
    {"fuzz_itch_query", "itch_query", "bound-costliest", kQueryMaxLen, Side::past_query_budget,
     &itch_query_seed, &store::oracle::check_itch_query},
    {"fuzz_itch_query", "itch_query", "bound-costliest-within-budget", kQueryMaxLen,
     Side::within_query_budget, &itch_query_budget_seed, &store::oracle::check_itch_query},
}};

}  // namespace tick_store::bound_seeds
