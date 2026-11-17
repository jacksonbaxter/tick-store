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
// each tier. Each seed is built from the bound constants it stands for, its sizes found by
// search where they depend on them, so it follows a new value; a value its shape cannot
// follow fails a static_assert or the seed's build, saying so.
//
// Every block here comes from the real encoder or is a nested block of the real encoder's
// framed by hand where the encoder would choose another form, and every store comes from
// the real writer (Z4), one with its share columns' blocks replaced by such blocks and its
// container rebuilt by the real builder. A codec input ends with bytes for part 2 that start with a block
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
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/itch_store_writer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

// The dictionary target's costliest input: that block filling the cap, whose first values
// take the budget, then such blocks past the cap, each as many values as the compare limit and
// then the decode limit leave, at most a maximal block, while -max_len holds them (the last cut
// to the values that fit), so the fuzz target decodes each by both kernel sets and compares,
// then decodes the rest once; then the tail. The unit test takes them all whole.
[[nodiscard]] inline Bytes dictionary_seed_at(const codec::oracle::OracleBounds& bounds) {
    const std::size_t cap = std::min(std::size_t{codec::kMaxBlockValues}, bounds.decoded_values);
    Bytes column = move_to_front_block(cap);
    std::size_t seen = cap;
    const std::size_t room = kSmallMaxLen - codec::kBlockHeaderSize;
    for (const std::size_t limit : {bounds.compared_values, bounds.single_decoded_values}) {
        while (seen < limit && column.size() < room) {
            std::size_t count = std::min(std::size_t{codec::kMaxBlockValues}, limit - seen);
            if (column.size() + move_to_front_block(count).size() > room) {
                std::size_t low = 0;
                std::size_t high = count;
                while (low < high) {
                    const std::size_t middle = low + ((high - low + 1) / 2);
                    if (column.size() + move_to_front_block(middle).size() <= room) {
                        low = middle;
                    } else {
                        high = middle - 1;
                    }
                }
                count = low;
            }
            if (count == 0) {
                break;
            }
            column = joined(column, move_to_front_block(count));
            seen += count;
        }
    }
    return with_part_two_tail(std::move(column), kSmallMaxLen);
}
[[nodiscard]] inline Bytes dictionary_seed() { return dictionary_seed_at(codec::oracle::kDictFuzzBounds); }

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

// The PFOR target's costliest input: of the shapes timed at width 0 the two dearest maximal
// blocks, 7 i^2 under delta of delta with the scale (6,414 bytes), within the cap, then 2^20
// values rising by 7 under head delta with the scale (6,159 bytes), past it and so decoded
// by both kernel sets and compared; then, where the decode limit reaches past the compare
// limit, a block of the same rising values, as many as the bytes left hold beside the tail's
// header, decoded once; then the tail.
[[nodiscard]] inline Bytes pfor_seed() {
    constexpr codec::oracle::OracleBounds kBounds = codec::oracle::kPforFuzzBounds;
    static_assert(kBounds.decoded_values == codec::kMaxBlockValues &&
                      kBounds.compared_values == 2 * codec::kMaxBlockValues,
                  "pfor_seed assumes a cap and a compare tier of one maximal block each");
    const auto rising = [](std::size_t count) {
        Words values(count);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = 7 * (1'000 + i);
        }
        return nested_pfor(values, codec::IntTransform::head_delta, true);
    };
    Words square(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < square.size(); ++i) {
        square[i] = 7 * i * i;
    }
    Bytes column = joined(nested_pfor(square, codec::IntTransform::delta_of_delta, true),
                          rising(codec::kMaxBlockValues));
    if (kBounds.single_decoded_values > kBounds.compared_values) {
        const std::size_t room = kSmallMaxLen - codec::kBlockHeaderSize - column.size();
        std::size_t low = 0;
        std::size_t high = std::min(std::size_t{codec::kMaxBlockValues},
                                    kBounds.single_decoded_values - kBounds.compared_values);
        while (low < high) {
            const std::size_t middle = low + ((high - low + 1) / 2);
            if (rising(middle).size() <= room) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }
        if (low > 0) {
            column = joined(column, rising(low));
        }
    }
    return with_part_two_tail(std::move(column), kSmallMaxLen);
}

// The PFOR block of `count` zeros with no transform, each vector width 0, so no delta pass
// decodes it.
[[nodiscard]] inline Bytes pfor_zeros_block(std::size_t count) {
    return encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            Words(count, 0),
            codec::PforOptions{
                .transform = codec::IntTransform::none, .adaptive_scale = false, .patch = true},
            out);
    });
}

// A maximal block of values rising by one under delta, each vector width 0, so the delta pass
// sums every vector.
[[nodiscard]] inline Bytes pfor_rising_delta_block() {
    Words rising(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < rising.size(); ++i) {
        rising[i] = 1'000 + i;
    }
    return encoded([&](Bytes& out) {
        return codec::encode_pfor_block(
            rising,
            codec::PforOptions{
                .transform = codec::IntTransform::delta, .adaptive_scale = false, .patch = true},
            out);
    });
}

// For the faults the checks past the cap must see: a first block that takes fuzz_pfor's cap
// exactly, no transform, so no delta pass decodes it; then the maximal rising delta block,
// which the target decodes by both kernel sets and compares, its delta pass summing every
// vector, one of the decodes from an exact copy of its bytes; then the tail.
[[nodiscard]] inline Bytes pfor_delta_past_cap_seed() {
    const std::size_t first_values =
        std::min(std::size_t{codec::kMaxBlockValues}, codec::oracle::kPforFuzzDecodedValues);
    return with_part_two_tail(joined(pfor_zeros_block(first_values), pfor_rising_delta_block()),
                              kSmallMaxLen);
}

// Refused past the cap: the same, but the delta block's payload holds one byte more, which
// the decoder refuses only once it has decoded every value ("pfor block"), so a refusal that
// changed its output past the cap would show.
[[nodiscard]] inline Bytes pfor_refused_past_cap_seed() {
    const std::size_t first_values =
        std::min(std::size_t{codec::kMaxBlockValues}, codec::oracle::kPforFuzzDecodedValues);
    Bytes refused = pfor_rising_delta_block();
    refused.push_back(std::byte{0});
    constexpr std::size_t kPayloadSizeAt = 4;
    std::uint64_t payload = 0;
    for (unsigned i = 0; i < 4; ++i) {
        payload |= std::to_integer<std::uint64_t>(refused[kPayloadSizeAt + i]) << (8U * i);
    }
    ++payload;
    for (unsigned i = 0; i < 4; ++i) {
        refused[kPayloadSizeAt + i] = static_cast<std::byte>((payload >> (8U * i)) & 0xFFU);
    }
    return with_part_two_tail(joined(pfor_zeros_block(first_values), refused), kSmallMaxLen);
}

// Refused: a run-length block of one value a run, as many runs as fuzz_run_length's cap
// takes in one block, whose run values rise by 7 except that run 100,000 repeats the one
// before, so its runs are not maximal and the decoder refuses it ("rle run value"). The run
// values are one delta block with a scale, one exception patched, and the lengths, all one,
// a width-0 block: 11,438 bytes. With that refusal taken out, only the never-budgeted check
// of a block's run count against its values can see it, 100,000 values past the budget.
[[nodiscard]] inline Bytes run_length_repeat_seed() {
    constexpr std::size_t kRepeatAt = 100'000;
    static_assert(codec::oracle::kRleFuzzDecodedValues > kRepeatAt &&
                      codec::oracle::kRleFuzzCrossCheckedValues < kRepeatAt,
                  "the repeat must fall within run-length's cap and past its budget");
    const std::size_t runs =
        std::min(std::size_t{codec::kMaxBlockValues}, codec::oracle::kRleFuzzDecodedValues);
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
// segments of kFuzzMaxSegmentRecords, then that segment: 64 raw records of `raw_bytes` bytes,
// zstd holding them in a few hundred bytes. Typed records cost the most a value, but -max_len
// holds them to about 36,000 values; raw bytes cost far less a value and reach the cap in
// under 700 bytes (fuzz/README.md, "Bounds").
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

// The values a store file's segments state in all (store::oracle::detail::stated_values).
[[nodiscard]] inline std::uint64_t stated_store_values(const Bytes& file) {
    const auto reader = store::ItchStoreReader::open_bytes(file);
    if (!reader.has_value()) {
        throw std::runtime_error("a bound seed's store did not open");
    }
    return store::oracle::detail::stated_values(*reader);
}

// adds_then_raw_segment with the most raw bytes a record whose file states at most `limit`
// values in at most kLargeMaxLen bytes. Each raw byte is one value in each of the 64 records,
// so the stated values rise by 64 a raw byte: the count is solved from one and two raw bytes,
// then stepped to the exact edge. A limit that one segment of raw records cannot reach is
// refused, since the file would then not be the dearest the limit allows.
[[nodiscard]] inline Bytes adds_then_raw_to(std::uint64_t limit) {
    const auto fits = [&](std::size_t raw) {
        const Bytes file = adds_then_raw_segment(raw);
        return file.size() <= kLargeMaxLen && stated_store_values(file) <= limit;
    };
    if (fits(itch::kMaxPayloadSize)) {
        throw std::runtime_error("a store bound past what one raw segment states: extend the seed");
    }
    const std::uint64_t one = stated_store_values(adds_then_raw_segment(1));
    const std::uint64_t step = stated_store_values(adds_then_raw_segment(2)) - one;
    std::size_t raw = limit < one || step == 0 ? 1 : 1 + static_cast<std::size_t>((limit - one) / step);
    raw = std::min<std::size_t>(raw, itch::kMaxPayloadSize);
    while (raw > 1 && !fits(raw)) {
        --raw;
    }
    while (raw < itch::kMaxPayloadSize && fits(raw + 1)) {
        ++raw;
    }
    return adds_then_raw_segment(raw);
}

// Within the cap: as many raw bytes as the cap leaves room for.
[[nodiscard]] inline Bytes itch_store_seed() {
    return adds_then_raw_to(store::oracle::kFuzzMaxStoreValues);
}

// ITCH store, past the cap: the same add orders, then raw bytes to the compare limit. Past the
// cap a file is only read twice; raw bytes are the only way past the cap in -max_len, so the
// dearest file there holds as many typed records as the bytes left allow beside raw bytes to
// the limit.
[[nodiscard]] inline Bytes itch_store_compared_seed() {
    return adds_then_raw_to(store::oracle::kFuzzComparedStoreValues);
}

// The day in segments of `segment_records`, the query target's cap unless given, with a raw
// record of 9 bytes every `raw_every` records, so no segment is plain and the indexed path
// decodes every record it reaches, as much as the exact path does; with none if `raw_every`
// is 0. The raw records' bytes, a type no message has, live as long as the program, since a
// RawMessage only views them.
inline constexpr std::array<std::byte, 9> kUnknownRecord{std::byte{'z'},  std::byte{0xA5}, std::byte{0xA5},
                                                         std::byte{0xA5}, std::byte{0xA5}, std::byte{0xA5},
                                                         std::byte{0xA5}, std::byte{0xA5}, std::byte{0xA5}};
[[nodiscard]] inline std::vector<itch::Message> raw_spotted_messages(std::size_t records, std::size_t raw_every) {
    std::vector<itch::Message> day = day_messages(records);
    for (std::size_t i = 0; raw_every != 0 && i < day.size(); i += raw_every) {
        day[i] = itch::RawMessage{.bytes = kUnknownRecord, .reason = itch::RawReason::unknown_type};
    }
    return day;
}
[[nodiscard]] inline Bytes raw_spotted_day(
    std::size_t records, std::size_t raw_every,
    std::uint32_t segment_records = store::oracle::kQueryFuzzMaxSegmentRecords) {
    return written(raw_spotted_messages(records, raw_every), segment_records);
}

// `file`, a store from the real writer, with each block `edit` changes (given its column
// metadata, it may set the block's codec and bytes; it returns whether it changed them), the
// seek index's maxima for the columns of the changed blocks set by `seek_top` where it gives
// one (the exact path checks them), and every other block and section as it was; the
// container is the real builder's.
template <class Edit, class SeekTop>
[[nodiscard]] Bytes with_blocks_rewritten(const Bytes& file, Edit&& edit, SeekTop&& seek_top) {
    using store::Column;
    const auto parser = store::ContainerParser::open(file);
    if (!parser.has_value()) {
        throw std::runtime_error("a bound seed's store did not open");
    }
    std::vector<store::ContainerPart> parts;
    for (std::uint64_t b = 0; b < parser->block_count(); ++b) {
        const auto view = parser->block(b);
        if (!view.has_value()) {
            throw std::runtime_error("a bound seed's store block did not read");
        }
        parts.push_back({.section_name = {},
                         .codec = view->codec,
                         .metadata = Bytes(view->metadata.begin(), view->metadata.end()),
                         .bytes = Bytes(view->payload.begin(), view->payload.end())});
    }
    std::vector<std::pair<std::uint32_t, Column>> changed;
    for (store::ContainerPart& part : parts) {
        const auto metadata = store::parse_column_metadata(part.metadata, 0);
        if (metadata.has_value() && edit(*metadata, part)) {
            if (!store::decode_column(metadata->column, part.codec, part.bytes, metadata->values).has_value()) {
                throw std::runtime_error("a bound seed's rewritten column did not decode");
            }
            changed.emplace_back(metadata->segment, metadata->column);
        }
    }
    for (store::ContainerPart& part : parts) {
        const auto metadata = store::parse_column_metadata(part.metadata, 0);
        if (!metadata.has_value() || metadata->column != Column::seek) {
            continue;
        }
        auto maxima = store::decode_column(Column::seek, part.codec, part.bytes, metadata->values);
        if (!maxima.has_value()) {
            throw std::runtime_error("a bound seed's seek index did not decode");
        }
        bool moved = false;
        for (const auto& [segment, column] : changed) {
            const std::optional<std::uint64_t> top = seek_top(column);
            if (segment == metadata->segment && top.has_value() &&
                static_cast<std::size_t>(column) < maxima->size()) {
                (*maxima)[static_cast<std::size_t>(column)] = *top;
                moved = true;
            }
        }
        if (moved) {
            auto chunks = store::encode_column(Column::seek, *maxima);
            if (!chunks.has_value() || chunks->size() != 1) {
                throw std::runtime_error("a bound seed's seek index did not encode");
            }
            part.codec = (*chunks)[0].codec;
            part.bytes = std::move((*chunks)[0].bytes);
        }
    }
    for (std::uint64_t i = 0; i < parser->section_count(); ++i) {
        const auto info = parser->section_info(i);
        if (!info.has_value()) {
            throw std::runtime_error("a bound seed's store section did not read");
        }
        const auto view = parser->section(info->name);
        if (!view.has_value()) {
            throw std::runtime_error("a bound seed's store section did not read");
        }
        parts.push_back({.section_name = std::string{info->name},
                         .codec = codec::CodecId::raw,
                         .metadata = {},
                         .bytes = Bytes(view->bytes.begin(), view->bytes.end())});
    }
    auto rebuilt = store::build_container(parts);
    if (!rebuilt.has_value()) {
        throw std::runtime_error("a bound seed's store did not rebuild");
    }
    return std::move(*rebuilt);
}

// `file` with the pieces of the five share columns (tracking, add, reduce, replace and trade
// shares, the dictionary columns) rewritten as the dearest move-to-front blocks, the mode whose
// values cost a decode most, which the query measure weighs by the mode a block header states:
// each piece's values in blocks of kColumnBlockValues, each move_to_front_block where it
// has at least kDictMtfMaxEntries values and otherwise the real encoder's block of as many
// values cycling down from the top entry.
[[nodiscard]] inline Bytes with_move_to_front_shares(const Bytes& file) {
    using store::Column;
    constexpr std::array kShares{Column::tracking, Column::add_shares, Column::reduce_shares,
                                 Column::replace_shares, Column::trade_shares};
    constexpr std::uint64_t kTop = codec::kDictMtfMaxEntries - 1;
    const auto is_share = [&](Column column) { return std::ranges::find(kShares, column) != kShares.end(); };
    const auto edit = [&](const store::ColumnMetadata& metadata, store::ContainerPart& part) {
        if (!is_share(metadata.column)) {
            return false;
        }
        Bytes chunk;
        for (std::size_t done = 0; done < metadata.values; done += store::kColumnBlockValues) {
            const std::size_t count = std::min<std::size_t>(store::kColumnBlockValues, metadata.values - done);
            if (count >= codec::kDictMtfMaxEntries) {
                const Bytes block = move_to_front_block(count);
                chunk.insert(chunk.end(), block.begin(), block.end());
                continue;
            }
            Words cycling(count);
            for (std::size_t i = 0; i < count; ++i) {
                cycling[i] = kTop - i;
            }
            if (!codec::encode_dict(cycling, count, chunk).has_value()) {
                throw std::runtime_error("a bound seed's encoder refused its values");
            }
        }
        part.codec = codec::CodecId::dictionary;
        part.bytes = std::move(chunk);
        return true;
    };
    return with_blocks_rewritten(file, edit, [&](Column) { return std::optional<std::uint64_t>{kTop}; });
}

// `file` with every coded column's pieces written again by its codec's real encoder in blocks
// of `per` values, the dictionary columns held to ranked PFOR: many codec blocks a piece, each
// with its decoders' fixed cost, which the query measure counts a block. The values, and so
// the seek index, are as they were.
[[nodiscard]] inline Bytes with_small_blocks(const Bytes& file, std::size_t per) {
    using store::ColumnCodec;
    const auto edit = [&](const store::ColumnMetadata& metadata, store::ContainerPart& part) {
        const store::ColumnInfo& info = store::column_info(metadata.column);
        if (info.codec == ColumnCodec::raw_bytes) {
            return false;
        }
        const auto values = store::decode_column(metadata.column, part.codec, part.bytes, metadata.values);
        if (!values.has_value()) {
            throw std::runtime_error("a bound seed's column did not decode");
        }
        Bytes chunk;
        bool encoded = false;
        if (info.codec == ColumnCodec::dictionary) {
            encoded = codec::encode_dict(*values, per, chunk, nullptr, codec::kDictRankedPforOnly).has_value();
            part.codec = codec::CodecId::dictionary;
        } else if (info.codec == ColumnCodec::run_length) {
            encoded = codec::encode_rle(*values, per, chunk).has_value();
            part.codec = codec::CodecId::run_length;
        } else {
            const codec::PforOptions options{
                .transform = info.codec == ColumnCodec::pfor_head_delta ? codec::IntTransform::head_delta
                             : info.codec == ColumnCodec::pfor_for || info.codec == ColumnCodec::pfor_for_scaled
                                 ? codec::IntTransform::none
                                 : codec::IntTransform::delta,
                .adaptive_scale = info.codec == ColumnCodec::pfor_for_scaled ||
                                  info.codec == ColumnCodec::pfor_delta_scaled};
            encoded = codec::encode_pfor(*values, per, options, chunk).has_value();
            part.codec = codec::CodecId::pfor;
        }
        if (!encoded) {
            throw std::runtime_error("a bound seed's encoder refused its values");
        }
        part.bytes = std::move(chunk);
        return true;
    };
    return with_blocks_rewritten(file, edit, [](store::Column) { return std::optional<std::uint64_t>{}; });
}

// The time range the query oracle's requests span (requests_for): the least and the most time
// of the segments that hold typed records.
[[nodiscard]] inline std::pair<std::uint64_t, std::uint64_t> query_time_range(const Bytes& file) {
    const Bytes fixed = store::oracle::fixed_checksums(file);
    const auto reader = store::ItchStoreReader::open_bytes(fixed);
    if (!reader.has_value()) {
        throw std::runtime_error("a bound seed's store did not open");
    }
    std::uint64_t low = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t high = 0;
    for (std::uint64_t k = 0; k < reader->segment_count(); ++k) {
        const store::SegmentSummary& summary = reader->segment(k).summary;
        if (summary.typed_records != 0) {
            low = std::min(low, summary.min_time);
            high = std::max(high, summary.max_time);
        }
    }
    return {low, high};
}

// `records` written in segments of `segment_records`, then `finish` on the file, under the
// dearest steering a file's last bytes can give the query oracle. Its steered request
// (requests_for) starts at the least time plus the trailer's last 8 bytes, its magic, modulo
// the range, and runs for the 8 bytes before them, the footer CRC and the reserved zeros, modulo
// the range and 2. The oracle reads a file with its CRCs recomputed, so a fuzzed file may hold
// any CRC there, and any times: the dearest steering is a window over the whole range, one more
// set of requests asking every row. Where the range is under 2^32 ns, so the CRC can say it, the
// records get one more system event, at the time a little past the last that leaves the start
// nearest the least time, and the footer CRC field the width that reaches the last; the
// steered window is then checked. A wider range is left as written.
template <class Finish>
[[nodiscard]] Bytes dearest_steering(std::vector<itch::Message> records, std::uint32_t segment_records,
                                     Finish&& finish) {
    constexpr std::uint64_t kTried = std::uint64_t{1} << 16U;
    Bytes file = finish(written(records, segment_records));
    const auto [low, high] = query_time_range(file);
    if (low > high || high - low >= (std::uint64_t{1} << 32U) - kTried - 1) {
        return file;
    }
    const std::uint64_t magic = store::oracle::query_detail::tail_word(file, 0);
    std::uint64_t last = high;
    for (std::uint64_t time = high; time <= high + kTried; ++time) {
        if (magic % (time - low + 1) < magic % (last - low + 1)) {
            last = time;
        }
    }
    if (last != high) {
        records.emplace_back(itch::SystemEvent{itch::MessageHeader{0, 0, last}, std::byte{'Q'}});
        file = finish(written(records, segment_records));
    }
    const auto width = static_cast<std::uint32_t>(last - low + 1);
    const std::size_t crc_at = file.size() - store::kTrailerSize + store::kTrailerFooterCrc;
    for (std::size_t i = 0; i < 4; ++i) {
        file[crc_at + i] = static_cast<std::byte>((width >> (8U * i)) & 0xFFU);
    }
    const Bytes fixed = store::oracle::fixed_checksums(file);
    const auto reader = store::ItchStoreReader::open_bytes(fixed);
    if (!reader.has_value() || query_time_range(file) != std::pair{low, last}) {
        throw std::runtime_error("a bound seed's steering changed its time range");
    }
    const auto requests = store::oracle::query_detail::requests_for(*reader, file);
    const store::oracle::query_detail::Request& steered = requests.at((requests.size() / 4) * 3);
    if (steered.end <= last || steered.start - low > (last - low) / 1'000) {
        throw std::runtime_error("a bound seed's steered window does not span its times");
    }
    return file;
}

// The ITCH query seeds are files on the envelope of the query measures
// (itch_query_oracles.hpp): of the shapes timed, those whose time is the most of their work in
// the measure of their tier and that reach the tier's limit within -max_len, with as many
// records as the limit and -max_len allow (fuzz/README.md, "Bounds").
//
// The file `make` builds of the most records whose work in `weights` is at most `limit`, in
// at most kQueryMaxLen bytes; both grow with the records, so the search doubles the count
// until it fails and then halves the range.
template <class Make>
[[nodiscard]] Bytes grown_to(const store::oracle::WorkWeights& weights, std::uint64_t limit, Make&& make) {
    const auto fits = [&](const Bytes& file) {
        if (file.size() > kQueryMaxLen) {
            return false;
        }
        const Bytes fixed = store::oracle::fixed_checksums(file);
        const auto reader = store::ItchStoreReader::open_bytes(fixed);
        if (!reader.has_value()) {
            throw std::runtime_error("a bound seed's store did not open");
        }
        return store::oracle::query_work(*reader, weights) <= limit;
    };
    std::size_t low = 1;
    std::size_t high = 2;
    while (fits(make(high))) {
        low = high;
        high *= 2;
    }
    high -= 1;
    while (low < high) {
        const std::size_t middle = low + ((high - low + 1) / 2);
        if (fits(make(middle))) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return make(low);
}

// Within the cap and past the budget: add orders on locate 0 a millisecond apart, a segment
// of one record each, to the cap: of the shapes timed whose steered request can span them, the
// most of its work (0.990).
[[nodiscard]] inline std::vector<itch::Message> adds_on_locate_zero(std::size_t records) {
    std::vector<itch::Message> out;
    out.reserve(records);
    for (std::uint64_t i = 0; i < records; ++i) {
        out.emplace_back(itch::AddOrder{
            itch::MessageHeader{0, 0, 34'200'000'000'000ULL + (i * 1'000'000)}, 1 + i, std::byte{'B'}, 100,
            itch::Stock{std::byte{'A'}, std::byte{'A'}, std::byte{'P'}, std::byte{'L'},
                        std::byte{' '}, std::byte{' '}, std::byte{' '}, std::byte{' '}},
            itch::Price4{1'000'000}});
    }
    return out;
}
[[nodiscard]] inline Bytes itch_query_seed() {
    return grown_to(store::oracle::kWholeCheckWork, store::oracle::kQueryFuzzWork, [](std::size_t records) {
        return dearest_steering(adds_on_locate_zero(records), 1, [](Bytes file) { return file; });
    });
}

// Within the budget: the day with a raw record every 16 in one segment, every coded column in
// blocks of 8 values (with_small_blocks), whose whole check with the repeat ask on every
// request is the most above its work of the shapes timed that the real encoders write and whose
// steered request can span them (1.173 times), to the budget.
[[nodiscard]] inline Bytes itch_query_budget_seed() {
    return grown_to(store::oracle::kWholeCheckWork, store::oracle::kQueryFuzzCrossCheckedWork,
                    [](std::size_t records) {
                        return dearest_steering(raw_spotted_messages(records, 16),
                                                store::oracle::kQueryFuzzMaxSegmentRecords,
                                                [](const Bytes& file) { return with_small_blocks(file, 8); });
                    });
}

// Past the cap, within the compare limit: the day with a raw record every 3 in segments of one
// record, whose two indexed asks are the most of their work of the shapes timed that the real
// writer makes and whose steered request can span them (0.941), to the compare limit or
// -max_len, whichever it meets first; its whole-check work is past the cap.
[[nodiscard]] inline Bytes itch_query_compared_seed() {
    return grown_to(store::oracle::kComparedAsksWork, store::oracle::kQueryFuzzComparedWork,
                    [](std::size_t records) {
                        return dearest_steering(raw_spotted_messages(records, 3), 1, [](Bytes file) { return file; });
                    });
}

// Move-to-front within the budget, the dictionary mode dearest a value to decode: the day with
// a raw record every 4 in one segment, its share columns as move-to-front blocks over 2,048
// entries, to the budget.
[[nodiscard]] inline Bytes itch_query_move_to_front_seed() {
    return grown_to(store::oracle::kWholeCheckWork, store::oracle::kQueryFuzzCrossCheckedWork,
                    [](std::size_t records) {
                        return dearest_steering(raw_spotted_messages(records, 4),
                                                store::oracle::kQueryFuzzMaxSegmentRecords,
                                                &with_move_to_front_shares);
                    });
}

// Falling minutes, the order dearest to the minute aggregates: add orders on locate 0 in one
// plain segment, each a minute before the one before from the largest 48-bit time down, so
// every minute aggregate asked over them, the product's and the oracle's, opens each row's
// minute before every one already held: as many as 48 bits hold minutes (kMostMinutes), or
// the most whose whole-check work stays within the cap, whichever is fewer.
[[nodiscard]] inline Bytes falling_minutes(std::size_t minutes) {
    constexpr std::uint64_t kTop = (std::uint64_t{1} << 48U) - 1;
    constexpr std::uint64_t kMinute = 60'000'000'000ULL;
    static_assert(kTop / kMinute + 1 == store::oracle::kMostMinutes);
    std::vector<itch::Message> records;
    for (std::uint64_t i = 0; i < std::min<std::uint64_t>(minutes, store::oracle::kMostMinutes); ++i) {
        records.emplace_back(itch::AddOrder{
            itch::MessageHeader{0, 0, kTop - (i * kMinute)}, 1 + i, std::byte{'B'}, 100,
            itch::Stock{std::byte{'A'}, std::byte{'A'}, std::byte{'P'}, std::byte{'L'},
                        std::byte{' '}, std::byte{' '}, std::byte{' '}, std::byte{' '}},
            itch::Price4{1'000'000}});
    }
    return written(records, store::oracle::kQueryFuzzMaxSegmentRecords);
}
[[nodiscard]] inline Bytes itch_query_falling_minutes_seed() {
    Bytes all = falling_minutes(store::oracle::kMostMinutes);
    const auto reader = store::ItchStoreReader::open_bytes(all);
    if (!reader.has_value()) {
        throw std::runtime_error("a bound seed's store did not open");
    }
    if (store::oracle::query_work(*reader, store::oracle::kWholeCheckWork) <= store::oracle::kQueryFuzzWork) {
        return all;
    }
    return grown_to(store::oracle::kWholeCheckWork, store::oracle::kQueryFuzzWork, &falling_minutes);
}

// Run-length past the cap filled with the blocks dearest a byte to decode twice: the dearest
// maximal block within the cap, then maximal blocks of one run as many as the compare limit
// takes and one more past it, while -max_len holds them. Blocks past that cost the fuzz target
// nothing and the unit test's whole check over a second each, so they are left out.
inline constexpr std::size_t kOneRunBlocksCompared =
    (codec::oracle::kRleFuzzComparedValues - codec::oracle::kRleFuzzDecodedValues) / codec::kMaxBlockValues;
[[nodiscard]] inline Bytes run_length_one_run_blocks_seed() {
    Bytes column = run_length_runs_block(codec::kMaxBlockValues, codec::kMaxBlockValues);
    const Bytes one_run = run_length_runs_block(codec::kMaxBlockValues, 1);
    for (std::size_t i = 0; i <= kOneRunBlocksCompared &&
                            column.size() + one_run.size() + codec::kBlockHeaderSize <= kSmallMaxLen;
         ++i) {
        column = joined(column, one_run);
    }
    return with_part_two_tail(std::move(column), kSmallMaxLen);
}

// Small blocks after the block within the cap. The cap counts the values taken before a
// block twice (codec_oracles.hpp, OracleBounds::decoded_values), so a block of 64 values
// after a block near the cap falls past it and is compared, and the block before it is the
// last within the cap, which the wrong-count check does not decode again. These inputs are
// built from a first block of `first` values, `small` blocks of kSmallBlockValues after it,
// and past them the blocks the compare limit takes from the input's start; each builder
// returns nothing where -max_len does not hold them with the tail's header.
inline constexpr std::size_t kSmallBlockValues = codec::oracle::kOracleBlockSizes[1];

// The largest n up to `high` for which `fits(n)`, which holds for every n below one it holds
// for; 0 if none does.
template <class Fits>
[[nodiscard]] std::size_t largest_fitting(std::size_t high, Fits&& fits) {
    std::size_t low = 0;
    while (low < high) {
        const std::size_t middle = low + ((high - low + 1) / 2);
        if (fits(middle)) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return low;
}

[[nodiscard]] inline std::optional<Bytes> fitted(Bytes column) {
    if (column.size() + codec::kBlockHeaderSize > kSmallMaxLen) {
        return std::nullopt;
    }
    return with_part_two_tail(std::move(column), kSmallMaxLen);
}

// The dictionary target's: a move-to-front block of `first` values, `small` dictionary blocks
// of values cycling down (in the mode the encoder picks), then a move-to-front block of what
// the compare limit leaves, at most a maximal block; then the tail.
[[nodiscard]] inline std::optional<Bytes> dictionary_small_blocks_of(std::size_t first, std::size_t small) {
    constexpr codec::oracle::OracleBounds kBounds = codec::oracle::kDictFuzzBounds;
    Bytes column = move_to_front_block(first);
    for (std::size_t b = 0; b < small; ++b) {
        Words values(kSmallBlockValues);
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = codec::kDictMtfMaxEntries - 1 - (((b * kSmallBlockValues) + i) % codec::kDictMtfMaxEntries);
        }
        column = joined(column, encoded([&](Bytes& out) { return codec::encode_dict_block(values, out); }));
    }
    const std::size_t seen = first + (small * kSmallBlockValues);
    if (seen < kBounds.compared_values) {
        column = joined(column, move_to_front_block(std::min(std::size_t{codec::kMaxBlockValues},
                                                             kBounds.compared_values - seen)));
    }
    return fitted(std::move(column));
}

// The PFOR target's: the dearest block, 7 i^2 under delta of delta with the scale, of `first`
// values, `small` blocks carrying on its values the same way, then the maximal head-delta
// block with the scale, which the compare limit takes after a first block of up to the cap's
// values; then the tail.
[[nodiscard]] inline std::optional<Bytes> pfor_small_blocks_of(std::size_t first, std::size_t small) {
    const auto squares = [](std::size_t from, std::size_t count) {
        Words values(count);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = 7 * (from + i) * (from + i);
        }
        return values;
    };
    Bytes column = nested_pfor(squares(0, first), codec::IntTransform::delta_of_delta, true);
    for (std::size_t b = 0; b < small; ++b) {
        column = joined(column, nested_pfor(squares(first + (b * kSmallBlockValues), kSmallBlockValues),
                                            codec::IntTransform::delta_of_delta, true));
    }
    Words rising(codec::kMaxBlockValues);
    for (std::size_t i = 0; i < rising.size(); ++i) {
        rising[i] = 7 * (1'000 + i);
    }
    column = joined(column, nested_pfor(rising, codec::IntTransform::head_delta, true));
    return fitted(std::move(column));
}

// The run-length target's: a block of `first` runs of one value, `small` blocks of 64 such
// runs, then, for the maximal blocks the compare limit takes after them, one maximal block of
// as many runs as the bytes left hold and maximal one-run blocks for the rest; then the tail.
// A many-run block costs more a byte than one-run blocks, and each one-run block more than
// the runs its 48 bytes would add to it.
[[nodiscard]] inline std::optional<Bytes> run_length_small_blocks_of(std::size_t first, std::size_t small) {
    constexpr codec::oracle::OracleBounds kBounds = codec::oracle::kRleFuzzBounds;
    Bytes column = run_length_runs_block(first, first);
    for (std::size_t b = 0; b < small; ++b) {
        column = joined(column, run_length_runs_block(kSmallBlockValues, kSmallBlockValues));
    }
    const std::size_t seen = first + (small * kSmallBlockValues);
    const std::size_t maximal =
        seen < kBounds.compared_values ? (kBounds.compared_values - seen) / codec::kMaxBlockValues : 0;
    if (maximal == 0) {
        return fitted(std::move(column));
    }
    const Bytes one_run = run_length_runs_block(codec::kMaxBlockValues, 1);
    const std::size_t rest = column.size() + ((maximal - 1) * one_run.size()) + codec::kBlockHeaderSize;
    const std::size_t runs = largest_fitting(codec::kMaxBlockValues, [&](std::size_t runs_tried) {
        return rest + run_length_runs_block(codec::kMaxBlockValues, runs_tried).size() <= kSmallMaxLen;
    });
    if (runs == 0) {
        return std::nullopt;
    }
    column = joined(column, run_length_runs_block(codec::kMaxBlockValues, runs));
    for (std::size_t i = 1; i < maximal; ++i) {
        column = joined(column, one_run);
    }
    return fitted(std::move(column));
}

// The run-length target's costliest input: a block of kMaxBlockValues runs of one value, the
// dearest to decode (11,302 bytes), within the cap; then, for the maximal blocks the compare
// limit takes past it, a maximal block with as many runs as the bytes left hold and maximal
// one-run blocks for the rest, all compared; then the tail, here a header alone. The unit test
// takes them all whole.
[[nodiscard]] inline Bytes run_length_seed() {
    return run_length_small_blocks_of(codec::oracle::kRleFuzzDecodedValues, 0).value();
}

// The named seeds of these shapes: the first block shortened by the small blocks' values,
// so the compare limit still takes its maximal blocks after them, with as many small blocks as
// -max_len holds (the dictionary's and PFOR's) or one (run-length's, whose bytes go to the
// many-run block). Each builder checks that one more small block does not fit.
inline constexpr std::size_t kDictSmallBlocks = 189;
inline constexpr std::size_t kPforSmallBlocks = 128;
inline constexpr std::size_t kRleSmallBlocks = 1;
[[nodiscard]] inline Bytes dictionary_small_blocks_seed() {
    const std::size_t cap = codec::oracle::kDictFuzzDecodedValues;
    if (dictionary_small_blocks_of(cap - (kSmallBlockValues * (kDictSmallBlocks + 1)), kDictSmallBlocks + 1)) {
        throw std::runtime_error("kDictSmallBlocks is not the most small blocks -max_len holds");
    }
    return dictionary_small_blocks_of(cap - (kSmallBlockValues * kDictSmallBlocks), kDictSmallBlocks).value();
}
[[nodiscard]] inline Bytes pfor_small_blocks_seed() {
    const std::size_t cap = codec::oracle::kPforFuzzDecodedValues;
    if (pfor_small_blocks_of(cap - (kSmallBlockValues * (kPforSmallBlocks + 1)), kPforSmallBlocks + 1)) {
        throw std::runtime_error("kPforSmallBlocks is not the most small blocks -max_len holds");
    }
    return pfor_small_blocks_of(cap - (kSmallBlockValues * kPforSmallBlocks), kPforSmallBlocks).value();
}
[[nodiscard]] inline Bytes run_length_small_blocks_seed() {
    const std::size_t cap = codec::oracle::kRleFuzzDecodedValues;
    return run_length_small_blocks_of(cap - (kSmallBlockValues * kRleSmallBlocks), kRleSmallBlocks).value();
}
// The maximal blocks' values run-length's compare limit takes past its cap, in blocks.
inline constexpr std::size_t kRleComparedBlocks =
    (codec::oracle::kRleFuzzComparedValues - codec::oracle::kRleFuzzDecodedValues) / codec::kMaxBlockValues;

// The dictionary target's costliest input: one-value blocks, as many as -max_len holds beside
// the rest, then the dearest block of the cap's values less twice theirs, the last within the
// cap (counted twice, the values before a block take the cap's room twice: codec_oracles.hpp,
// OracleBounds::decoded_values), then a maximal block of it, compared; then the tail. Each
// one-value block costs the fuzz target the whole of part 1 for one value, dearer a byte than
// the cap's block. The builder checks that one more such block does not fit.
inline constexpr std::size_t kDictOneValueBlocks = 300;
[[nodiscard]] inline std::optional<Bytes> dictionary_one_value_blocks_of(std::size_t ones) {
    constexpr codec::oracle::OracleBounds kBounds = codec::oracle::kDictFuzzBounds;
    Bytes column;
    for (std::size_t b = 0; b < ones; ++b) {
        const Words value{codec::kDictMtfMaxEntries - 1 - (b % codec::kDictMtfMaxEntries)};
        column = joined(column, encoded([&](Bytes& out) { return codec::encode_dict_block(value, out); }));
    }
    column = joined(column, move_to_front_block(kBounds.decoded_values - (2 * ones)));
    column = joined(column, move_to_front_block(std::min(std::size_t{codec::kMaxBlockValues},
                                                         kBounds.compared_values - kBounds.decoded_values)));
    return fitted(std::move(column));
}
[[nodiscard]] inline Bytes dictionary_one_value_blocks_seed() {
    if (dictionary_one_value_blocks_of(kDictOneValueBlocks + 1)) {
        throw std::runtime_error("kDictOneValueBlocks is not the most one-value blocks -max_len holds");
    }
    return dictionary_one_value_blocks_of(kDictOneValueBlocks).value();
}

// Inputs that spend the cross-check budget and then the prefix past it within the cap, so the
// re-encodes and the encoder-side check take the most values they can (the count table,
// fuzz/README.md, "Bounds"): a first block of the budget's values, then
// OracleBounds::past_budget_blocks blocks of kPastBudgetValues values, each taken whole by the
// prefix, then a block of what the cap leaves, the last within it, which past the budget and
// the prefix gets the never-budgeted checks only; then the tail. `block(from, count)` gives a
// codec block of `count` values.
template <class Block>
[[nodiscard]] Bytes prefix_seed_of(const codec::oracle::OracleBounds& bounds, Block&& block) {
    constexpr std::size_t kSmall = codec::oracle::kPastBudgetValues;
    Bytes column = block(0, bounds.cross_checked_values);
    std::size_t seen = bounds.cross_checked_values;
    for (std::size_t b = 0; b < bounds.past_budget_blocks; ++b) {
        column = joined(column, block(seen, kSmall));
        seen += kSmall;
    }
    column = joined(column, block(seen, bounds.decoded_values - (2 * seen)));
    auto fits = fitted(std::move(column));
    if (!fits) {
        throw std::runtime_error("a prefix seed does not fit -max_len");
    }
    return std::move(*fits);
}
[[nodiscard]] inline Bytes dictionary_prefix_seed() {
    return prefix_seed_of(codec::oracle::kDictFuzzBounds, [](std::size_t from, std::size_t count) {
        Words values(count);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = codec::kDictMtfMaxEntries - 1 - ((from + i) % codec::kDictMtfMaxEntries);
        }
        return encoded([&](Bytes& out) { return codec::encode_dict_block(values, out); });
    });
}
[[nodiscard]] inline Bytes pfor_prefix_seed() {
    return prefix_seed_of(codec::oracle::kPforFuzzBounds, [](std::size_t from, std::size_t count) {
        Words values(count);
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = 7 * (from + i) * (from + i);
        }
        return nested_pfor(values, codec::IntTransform::delta_of_delta, true);
    });
}
[[nodiscard]] inline Bytes run_length_prefix_seed() {
    return prefix_seed_of(codec::oracle::kRleFuzzBounds, [](std::size_t /*from*/, std::size_t count) {
        return run_length_runs_block(count, std::min(count, kSmallBlockValues));
    });
}
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
    // Its last block states values and is refused past the cap and within the compare limit, so
    // the fuzz target decodes it by both kernel sets, compares the refusals and checks the output
    // was left alone. (The tail's header, which states none, is not counted.)
    bool refused_compared = false;

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

// The tiers a seed of a codec's dearest block within the cap and a maximal block after it
// falls in: the second compared where the compare limit reaches it, else not examined.
[[nodiscard]] constexpr CodecTiers maximal_after_cap(std::size_t cap, std::size_t compare_limit) {
    return compare_limit >= cap + codec::kMaxBlockValues ? CodecTiers{.within_cap = 1, .compared = 1}
                                                         : CodecTiers{.within_cap = 1, .not_examined = 1};
}

// The tiers of a seed of one block within the cap and a refused maximal block after it.
[[nodiscard]] constexpr CodecTiers refused_after_cap(std::size_t cap, std::size_t compare_limit) {
    return CodecTiers{.within_cap = 1, .refused_compared = compare_limit >= cap + codec::kMaxBlockValues};
}

// The tiers of run_length_one_run_blocks_seed's blocks: the dearest block within the cap, the
// one-run blocks the compare limit takes, then one not examined; -max_len holds 103 beside the
// dearest block.
[[nodiscard]] constexpr CodecTiers one_run_blocks_tiers() {
    constexpr std::size_t kFit = 103;
    static_assert(codec::oracle::kRleFuzzDecodedValues == codec::kMaxBlockValues &&
                      codec::oracle::kRleFuzzComparedValues ==
                          codec::oracle::kRleFuzzBounds.single_decoded_values,
                  "one_run_blocks_tiers assumes a cap of one maximal block and no decode-once tier");
    const std::size_t compared = std::min(kFit, kOneRunBlocksCompared);
    return CodecTiers{.within_cap = 1, .compared = compared, .not_examined = compared < kFit ? 1U : 0U};
}

// The tiers of dictionary_seed's blocks: the cap's block, then a block for each maximal
// block's values or part of one the compare limit and then the decode limit leave. -max_len
// holds the cap's block and two maximal blocks of it beside the tail, so the limits may reach
// no further past the cap.
[[nodiscard]] constexpr CodecTiers dictionary_tiers() {
    constexpr std::size_t kCap = codec::oracle::kDictFuzzDecodedValues;
    constexpr std::size_t kCompared = codec::oracle::kDictFuzzComparedValues;
    constexpr std::size_t kOnce = codec::oracle::kDictFuzzSingleDecodedValues;
    static_assert(kCap < codec::kMaxBlockValues && kOnce <= kCap + (2 * codec::kMaxBlockValues),
                  "dictionary_tiers assumes a cap below one maximal block and limits -max_len holds");
    const auto blocks = [](std::size_t values) {
        return (values + codec::kMaxBlockValues - 1) / codec::kMaxBlockValues;
    };
    return CodecTiers{.within_cap = 1, .compared = blocks(kCompared - kCap), .decoded_once = blocks(kOnce - kCompared)};
}

// The tiers of a prefix seed: the budget's block, the prefix's blocks and the cap's block.
inline constexpr CodecTiers kPrefixTiers{.within_cap = 1 + codec::oracle::kFuzzPastBudgetBlocks + 1};

// One line each in fuzz/bound_seeds.txt, in this order.
inline constexpr std::array<BoundSeed, 24> kBoundSeeds{{
    {"fuzz_dictionary", "dictionary", "bound-costliest", kSmallMaxLen, Side::codec_tiers,
     &dictionary_seed, &codec::oracle::check_dict_column, &codec::oracle::kDictCodec,
     &codec::oracle::kDictFuzzBounds, dictionary_tiers()},
    {"fuzz_dictionary", "dictionary", "bound-small-blocks", kSmallMaxLen, Side::codec_tiers,
     &dictionary_small_blocks_seed, &codec::oracle::check_dict_column, &codec::oracle::kDictCodec,
     &codec::oracle::kDictFuzzBounds, CodecTiers{.within_cap = 1, .compared = kDictSmallBlocks + 1}},
    {"fuzz_dictionary", "dictionary", "bound-one-value-blocks", kSmallMaxLen, Side::codec_tiers,
     &dictionary_one_value_blocks_seed, &codec::oracle::check_dict_column, &codec::oracle::kDictCodec,
     &codec::oracle::kDictFuzzBounds, CodecTiers{.within_cap = kDictOneValueBlocks + 1, .compared = 1}},
    {"fuzz_dictionary", "dictionary", "bound-prefix", kSmallMaxLen, Side::codec_tiers,
     &dictionary_prefix_seed, &codec::oracle::check_dict_column, &codec::oracle::kDictCodec,
     &codec::oracle::kDictFuzzBounds, kPrefixTiers},
    {"fuzz_run_length", "run_length", "bound-costliest", kSmallMaxLen, Side::codec_tiers,
     &run_length_seed, &codec::oracle::check_rle_column, &codec::oracle::kRleCodec,
     &codec::oracle::kRleFuzzBounds, CodecTiers{.within_cap = 1, .compared = kRleComparedBlocks}},
    {"fuzz_run_length", "run_length", "bound-one-run-blocks", kSmallMaxLen, Side::codec_tiers,
     &run_length_one_run_blocks_seed, &codec::oracle::check_rle_column, &codec::oracle::kRleCodec,
     &codec::oracle::kRleFuzzBounds, one_run_blocks_tiers()},
    {"fuzz_run_length", "run_length", "bound-small-blocks", kSmallMaxLen, Side::codec_tiers,
     &run_length_small_blocks_seed, &codec::oracle::check_rle_column, &codec::oracle::kRleCodec,
     &codec::oracle::kRleFuzzBounds, CodecTiers{.within_cap = 1, .compared = kRleSmallBlocks + kRleComparedBlocks}},
    {"fuzz_run_length", "run_length", "bound-prefix", kSmallMaxLen, Side::codec_tiers,
     &run_length_prefix_seed, &codec::oracle::check_rle_column, &codec::oracle::kRleCodec,
     &codec::oracle::kRleFuzzBounds, kPrefixTiers},
    {"fuzz_run_length", "run_length", "bound-repeated-run-value", kSmallMaxLen,
     Side::codec_tiers, &run_length_repeat_seed, &codec::oracle::check_rle_column,
     &codec::oracle::kRleCodec, &codec::oracle::kRleFuzzBounds,
     CodecTiers{.first_refused_within_cap = true}},
    {"fuzz_pfor", "pfor", "bound-costliest", kSmallMaxLen, Side::codec_tiers, &pfor_seed,
     &codec::oracle::check_pfor_column, &codec::oracle::kPforCodec,
     &codec::oracle::kPforFuzzBounds,
     CodecTiers{.within_cap = 1, .compared = 1, .decoded_once = 1}},
    {"fuzz_pfor", "pfor", "bound-small-blocks", kSmallMaxLen, Side::codec_tiers,
     &pfor_small_blocks_seed, &codec::oracle::check_pfor_column, &codec::oracle::kPforCodec,
     &codec::oracle::kPforFuzzBounds, CodecTiers{.within_cap = 1, .compared = kPforSmallBlocks + 1}},
    {"fuzz_pfor", "pfor", "bound-prefix", kSmallMaxLen, Side::codec_tiers, &pfor_prefix_seed,
     &codec::oracle::check_pfor_column, &codec::oracle::kPforCodec, &codec::oracle::kPforFuzzBounds,
     kPrefixTiers},
    {"fuzz_pfor", "pfor", "bound-delta-past-cap", kSmallMaxLen, Side::codec_tiers,
     &pfor_delta_past_cap_seed, &codec::oracle::check_pfor_column, &codec::oracle::kPforCodec,
     &codec::oracle::kPforFuzzBounds,
     maximal_after_cap(codec::oracle::kPforFuzzDecodedValues, codec::oracle::kPforFuzzComparedValues)},
    {"fuzz_pfor", "pfor", "bound-refused-past-cap", kSmallMaxLen, Side::codec_tiers,
     &pfor_refused_past_cap_seed, &codec::oracle::check_pfor_column, &codec::oracle::kPforCodec,
     &codec::oracle::kPforFuzzBounds,
     refused_after_cap(codec::oracle::kPforFuzzDecodedValues, codec::oracle::kPforFuzzComparedValues)},
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
    {"fuzz_itch_query", "itch_query", "bound-costliest-compared", kQueryMaxLen,
     Side::query_compared, &itch_query_compared_seed, &store::oracle::check_itch_query},
    {"fuzz_itch_query", "itch_query", "bound-falling-minutes", kQueryMaxLen,
     Side::past_query_budget, &itch_query_falling_minutes_seed, &store::oracle::check_itch_query},
    {"fuzz_itch_query", "itch_query", "bound-move-to-front", kQueryMaxLen,
     Side::within_query_budget, &itch_query_move_to_front_seed, &store::oracle::check_itch_query},
}};

}  // namespace tick_store::bound_seeds
