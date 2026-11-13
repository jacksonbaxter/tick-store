// tools/tick_codec_eval's evaluator on streams small enough to follow by hand. The sizes
// it reports are held to the codecs' own column encoders run on the same values, so the
// evaluator's block cutting, per-locate split and byte accounting are checked against
// code it does not share.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/stream_vbyte.hpp"
#include "tick_store/harness/codec_eval.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::harness {
namespace {

using test::Bytes;

constexpr std::uint32_t kBlock = 2;

EvalOptions options_of(std::uint32_t block_values, std::size_t max_examples = 8) {
    return EvalOptions{.block_values = block_values,
                       .max_examples = max_examples,
                       .columns = {},
                       .observe_block = {}};
}

void append(Bytes& out, const itch::Message& message) {
    std::array<std::byte, 128> frame{};
    const std::size_t size = itch::encode_frame(message, frame).value();
    out.insert(out.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
}

itch::MessageHeader header(std::uint16_t locate, std::uint64_t timestamp) {
    return {.stock_locate = locate, .tracking_number = 0, .timestamp_ns = timestamp};
}

// One message of every type a column reads, from two locates, with the timestamps and
// prices of each locate kept apart so the per-locate streams differ from file order.
struct Day {
    Bytes bytes;
    std::vector<std::uint64_t> timestamps;
    std::map<std::uint16_t, std::vector<std::uint64_t>> timestamps_by_locate;
    std::vector<std::uint64_t> add_prices;
    std::map<std::uint16_t, std::vector<std::uint64_t>> add_prices_by_locate;
    std::map<std::pair<std::uint16_t, bool>, std::vector<std::uint64_t>> add_prices_by_side;
    // Each message's locate in file order, and each add's locate and side.
    std::vector<std::uint16_t> locates;
    std::vector<std::pair<std::uint16_t, bool>> add_sides;
    std::vector<std::uint64_t> replace_prices;
    std::vector<std::uint64_t> executed_prices;
    std::vector<std::uint64_t> trade_prices;
    std::vector<std::uint64_t> mwcb_levels;

    void stamp(std::uint16_t locate, std::uint64_t timestamp) {
        locates.push_back(locate);
        timestamps.push_back(timestamp);
        timestamps_by_locate[locate].push_back(timestamp);
    }

    void add(std::uint16_t locate, std::uint64_t timestamp, std::uint32_t price, bool mpid,
             bool sell = false) {
        stamp(locate, timestamp);
        add_prices.push_back(price);
        add_prices_by_locate[locate].push_back(price);
        add_prices_by_side[{locate, sell}].push_back(price);
        add_sides.emplace_back(locate, sell);
        const std::byte side{sell ? std::uint8_t{'S'} : std::uint8_t{'B'}};
        if (mpid) {
            itch::AddOrderWithMpid message;
            message.header = header(locate, timestamp);
            message.buy_sell_indicator = side;
            message.price = itch::Price4{price};
            append(bytes, message);
        } else {
            itch::AddOrder message;
            message.header = header(locate, timestamp);
            message.buy_sell_indicator = side;
            message.price = itch::Price4{price};
            append(bytes, message);
        }
    }

    void replace(std::uint16_t locate, std::uint64_t timestamp, std::uint32_t price) {
        stamp(locate, timestamp);
        replace_prices.push_back(price);
        itch::OrderReplace message;
        message.header = header(locate, timestamp);
        message.price = itch::Price4{price};
        append(bytes, message);
    }

    void execute(std::uint16_t locate, std::uint64_t timestamp, std::uint32_t price) {
        stamp(locate, timestamp);
        executed_prices.push_back(price);
        itch::OrderExecutedWithPrice message;
        message.header = header(locate, timestamp);
        message.execution_price = itch::Price4{price};
        append(bytes, message);
    }

    void trade(std::uint16_t locate, std::uint64_t timestamp, std::uint32_t price) {
        stamp(locate, timestamp);
        trade_prices.push_back(price);
        itch::Trade message;
        message.header = header(locate, timestamp);
        message.price = itch::Price4{price};
        append(bytes, message);
    }

    void mwcb(std::uint64_t timestamp, std::array<std::uint64_t, 3> levels) {
        stamp(0, timestamp);
        mwcb_levels.insert(mwcb_levels.end(), levels.begin(), levels.end());
        itch::MwcbDeclineLevel message;
        message.header = header(0, timestamp);
        message.level_1 = itch::Price8{levels[0]};
        message.level_2 = itch::Price8{levels[1]};
        message.level_3 = itch::Price8{levels[2]};
        append(bytes, message);
    }

    void event(std::uint64_t timestamp) {
        stamp(0, timestamp);
        itch::SystemEvent message;
        message.header = header(0, timestamp);
        append(bytes, message);
    }
};

Day small_day(std::uint64_t top_level) {
    Day day;
    day.event(1'000);
    day.mwcb(1'500, {350'000'000'000, 330'000'000'000, top_level});
    day.add(7, 2'000, 1'234'500, false);
    day.add(9, 2'100, 99'990'000, true, true);
    day.add(7, 2'100, 1'234'600, false, true);
    day.add(7, 2'500, 1'234'500, true);
    day.replace(9, 3'000, 99'980'000);
    day.execute(7, 3'001, 1'234'500);
    day.add(9, 3'002, 4'294'967'295, false);
    day.trade(9, 4'000, 100);
    day.add(7, 3'000, 0, false, true);
    day.event(5'000);
    return day;
}

std::uint64_t timestamp_bytes(const std::vector<std::uint64_t>& values) {
    std::vector<std::byte> column;
    EXPECT_TRUE(codec::encode_timestamps(values, kBlock, column).has_value());
    return column.size();
}

std::uint64_t xor_bytes(const std::vector<std::uint64_t>& words) {
    std::vector<std::byte> column;
    EXPECT_TRUE(codec::encode_xor(words, kBlock, column).has_value());
    return column.size();
}

std::vector<std::uint64_t> as_words(Representation representation, ValueKind kind,
                                    const std::vector<std::uint64_t>& values) {
    std::vector<std::uint64_t> words;
    words.reserve(values.size());
    for (const std::uint64_t value : values) {
        words.push_back(to_stored_word(representation, kind, value));
    }
    return words;
}

const ColumnResult& column(const EvalResult& result, std::string_view name) {
    const auto found = std::ranges::find_if(
        result.columns, [&](const ColumnResult& c) { return c.spec->name == name; });
    EXPECT_NE(found, result.columns.end()) << name;
    return *found;
}

const CodecResult& codec(const ColumnResult& column, std::string_view name) {
    const auto found = std::ranges::find_if(
        column.codecs, [&](const CodecResult& c) { return c.entry->name == name; });
    EXPECT_NE(found, column.codecs.end()) << name;
    return *found;
}

// What the integer codecs take for a column of `values`, from their own column encoders,
// with the options each name stands for written out here rather than taken from the
// registry. nullopt for Stream VByte.
struct IntegerCodec {
    std::string_view name;
    std::optional<codec::PforOptions> pfor;
    bool for_timestamps;
    bool for_prices;
};

constexpr codec::PforOptions pfor_options(codec::IntTransform transform, bool scale, bool patch) {
    return {.transform = transform, .adaptive_scale = scale, .patch = patch};
}

const std::vector<IntegerCodec>& integer_codecs() {
    using codec::IntTransform;
    static const std::vector<IntegerCodec> value{
        {"pfor-delta", pfor_options(IntTransform::delta, false, true), true, true},
        {"pfor-delta-unpatched", pfor_options(IntTransform::delta, false, false), true, true},
        {"pfor-dod", pfor_options(IntTransform::delta_of_delta, false, true), true, false},
        {"pfor-dod-unpatched", pfor_options(IntTransform::delta_of_delta, false, false), true,
         false},
        {"svb-delta", std::nullopt, true, true},
        {"pfor-for", pfor_options(IntTransform::none, false, true), false, true},
        {"pfor-for-scaled", pfor_options(IntTransform::none, true, true), false, true},
        {"pfor-delta-scaled", pfor_options(IntTransform::delta, true, true), false, true},
        {"pfor-delta-scaled-unpatched", pfor_options(IntTransform::delta, true, false), false,
         true},
    };
    return value;
}

std::uint64_t integer_bytes(const IntegerCodec& integer, const std::vector<std::uint64_t>& values) {
    std::vector<std::byte> column;
    if (integer.pfor.has_value()) {
        EXPECT_TRUE(codec::encode_pfor(values, kBlock, *integer.pfor, column).has_value());
    } else {
        EXPECT_TRUE(codec::encode_svb(values, kBlock, true, column).has_value());
    }
    return column.size();
}

// Every integer codec for this kind of column, on its streams, against the evaluator.
void expect_integer_sizes(const ColumnResult& got,
                          const std::vector<std::vector<std::uint64_t>>& streams,
                          std::uint64_t metadata) {
    const bool timestamps = got.spec->kind == ValueKind::timestamp_ns;
    for (const IntegerCodec& integer : integer_codecs()) {
        if (!(timestamps ? integer.for_timestamps : integer.for_prices)) {
            continue;
        }
        const CodecResult& result = codec(got, integer.name);
        EXPECT_TRUE(result.exact()) << got.spec->name << " " << integer.name;
        EXPECT_NE(result.entry->family, "gorilla");
        std::uint64_t bytes = 0;
        std::uint64_t values = 0;
        std::uint64_t blocks = 0;
        for (const std::vector<std::uint64_t>& stream : streams) {
            bytes += integer_bytes(integer, stream);
            values += stream.size();
            blocks += (stream.size() + kBlock - 1) / kBlock;
        }
        EXPECT_EQ(result.block_bytes, bytes) << got.spec->name << " " << integer.name;
        EXPECT_EQ(result.values, values);
        EXPECT_EQ(result.blocks, blocks);
        EXPECT_EQ(result.metadata_bytes, metadata) << got.spec->name << " " << integer.name;
    }
}

template <class Key>
std::vector<std::vector<std::uint64_t>> streams_of(
    const std::map<Key, std::vector<std::uint64_t>>& by_key) {
    std::vector<std::vector<std::uint64_t>> streams;
    streams.reserve(by_key.size());
    for (const auto& [key, values] : by_key) {
        streams.push_back(values);
    }
    return streams;
}

// Order-0 entropy in bits per symbol, from a count of each symbol: written out here, apart
// from the evaluator's own table.
template <class Symbol>
double entropy_of(const std::vector<Symbol>& symbols) {
    std::map<Symbol, double> counts;
    for (const Symbol& symbol : symbols) {
        counts[symbol] += 1;
    }
    const auto total = static_cast<double>(symbols.size());
    double bits = 0;
    for (const auto& [symbol, count] : counts) {
        bits -= count / total * std::log2(count / total);
    }
    return bits;
}

template <class Symbol>
std::size_t distinct_of(const std::vector<Symbol>& symbols) {
    return std::set<Symbol>(symbols.begin(), symbols.end()).size();
}

// The deltas of each stream from its second value, wrapping, pooled.
std::vector<std::uint64_t> pooled_deltas(const std::vector<std::vector<std::uint64_t>>& streams) {
    std::vector<std::uint64_t> deltas;
    for (const std::vector<std::uint64_t>& stream : streams) {
        for (std::size_t i = 1; i < stream.size(); ++i) {
            deltas.push_back(stream[i] - stream[i - 1]);
        }
    }
    return deltas;
}

std::vector<std::uint64_t> pooled_delta_of_deltas(
    const std::vector<std::vector<std::uint64_t>>& streams) {
    std::vector<std::uint64_t> dods;
    for (const std::vector<std::uint64_t>& stream : streams) {
        for (std::size_t i = 2; i < stream.size(); ++i) {
            dods.push_back((stream[i] - stream[i - 1]) - (stream[i - 1] - stream[i - 2]));
        }
    }
    return dods;
}

// What a column of `values` costs in each price codec, from the codecs' column encoders.
void expect_price_sizes(const ColumnResult& got, ValueKind kind,
                        const std::vector<std::uint64_t>& values, std::uint64_t metadata) {
    ASSERT_EQ(got.values, values.size()) << got.spec->name;
    const std::array<std::pair<std::string_view, Representation>, 3> codecs{{
        {"gorilla-xor-f64-dollars", Representation::dollars},
        {"gorilla-xor-f64-integer", Representation::integer_as_double},
        {"gorilla-xor-u64-integer", Representation::identity},
    }};
    for (const auto& [name, representation] : codecs) {
        const CodecResult& result = codec(got, name);
        EXPECT_TRUE(result.exact()) << got.spec->name << " " << name;
        EXPECT_EQ(result.values, values.size());
        EXPECT_EQ(result.blocks, (values.size() + kBlock - 1) / kBlock);
        EXPECT_EQ(result.header_bytes, result.blocks * codec::kBlockHeaderSize);
        EXPECT_EQ(result.block_bytes, xor_bytes(as_words(representation, kind, values)))
            << got.spec->name << " " << name;
        EXPECT_EQ(result.metadata_bytes, metadata);
        EXPECT_EQ(result.stored_bytes(), result.block_bytes + metadata);
    }
}

TEST(CodecEval, ReportsTheSizesTheColumnEncodersGiveOnEveryColumn) {
    const Day day = small_day(310'000'000'000);
    const auto result = evaluate_codecs(day.bytes, options_of(kBlock));
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->exact());
    EXPECT_EQ(result->messages, day.timestamps.size());
    EXPECT_EQ(result->raw_skipped, 0U);
    EXPECT_EQ(result->columns.size(), column_specs().size());

    const ColumnResult& timestamps = column(*result, "timestamp");
    EXPECT_EQ(timestamps.values, day.timestamps.size());
    EXPECT_EQ(timestamps.streams, 1U);
    const CodecResult& dod = codec(timestamps, "gorilla-dod");
    EXPECT_TRUE(dod.exact());
    const auto timestamp_codecs = static_cast<std::size_t>(std::ranges::count_if(
        codec_registry(),
        [](const CodecEntry& entry) { return applies_to(entry, ValueKind::timestamp_ns); }));
    EXPECT_EQ(timestamps.codecs.size(), timestamp_codecs);
    EXPECT_EQ(timestamp_codecs, 1U + 5U);
    EXPECT_EQ(dod.block_bytes, timestamp_bytes(day.timestamps));
    EXPECT_EQ(dod.metadata_bytes, 8U);
    // Every timestamp is counted once: zero or one of the four buckets, except each
    // block's first value, which is stored whole.
    std::uint64_t counted = 0;
    for (const std::uint64_t n : dod.counters) {
        counted += n;
    }
    EXPECT_EQ(counted, day.timestamps.size() - dod.blocks);

    const ColumnResult& per_locate = column(*result, "timestamp_per_locate");
    EXPECT_EQ(per_locate.streams, day.timestamps_by_locate.size());
    std::uint64_t per_locate_bytes = 0;
    for (const auto& [locate, values] : day.timestamps_by_locate) {
        per_locate_bytes += timestamp_bytes(values);
    }
    EXPECT_EQ(codec(per_locate, "gorilla-dod").block_bytes, per_locate_bytes);
    EXPECT_EQ(codec(per_locate, "gorilla-dod").metadata_bytes, per_locate.streams * (8U + 2U));
    expect_integer_sizes(timestamps, {day.timestamps}, 8);
    expect_integer_sizes(per_locate, streams_of(day.timestamps_by_locate),
                         per_locate.streams * (8U + 2U));

    expect_price_sizes(column(*result, "add_price"), ValueKind::price4, day.add_prices, 8);
    expect_price_sizes(column(*result, "replace_price"), ValueKind::price4, day.replace_prices,
                       8);
    expect_price_sizes(column(*result, "executed_price"), ValueKind::price4,
                       day.executed_prices, 8);
    expect_price_sizes(column(*result, "trade_price"), ValueKind::price4, day.trade_prices, 8);
    expect_price_sizes(column(*result, "mwcb_level"), ValueKind::price8, day.mwcb_levels, 8);

    const ColumnResult& add_per_locate = column(*result, "add_price_per_locate");
    EXPECT_EQ(add_per_locate.streams, day.add_prices_by_locate.size());
    std::uint64_t add_bytes = 0;
    for (const auto& [locate, values] : day.add_prices_by_locate) {
        add_bytes += xor_bytes(values);
    }
    EXPECT_EQ(codec(add_per_locate, "gorilla-xor-u64-integer").block_bytes, add_bytes);

    expect_integer_sizes(column(*result, "add_price"), {day.add_prices}, 8);
    expect_integer_sizes(column(*result, "replace_price"), {day.replace_prices}, 8);
    expect_integer_sizes(column(*result, "executed_price"), {day.executed_prices}, 8);
    expect_integer_sizes(column(*result, "trade_price"), {day.trade_prices}, 8);
    expect_integer_sizes(column(*result, "mwcb_level"), {day.mwcb_levels}, 8);
    expect_integer_sizes(add_per_locate, streams_of(day.add_prices_by_locate),
                         add_per_locate.streams * (8U + 2U));

    // Per locate and side: locate 7 buys and sells, locate 9 buys and sells, so four
    // streams, each with its count, locate and side.
    const ColumnResult& by_side = column(*result, "add_price_per_locate_side");
    EXPECT_EQ(by_side.streams, day.add_prices_by_side.size());
    EXPECT_EQ(by_side.streams, 4U);
    std::uint64_t side_bytes = 0;
    for (const auto& [key, values] : day.add_prices_by_side) {
        side_bytes += xor_bytes(values);
    }
    EXPECT_EQ(codec(by_side, "gorilla-xor-u64-integer").block_bytes, side_bytes);
    constexpr std::uint64_t kSideMetadata = std::uint64_t{4} * (8U + 2U + 1U);
    EXPECT_EQ(codec(by_side, "gorilla-xor-u64-integer").metadata_bytes, kSideMetadata);
    expect_integer_sizes(by_side, streams_of(day.add_prices_by_side), kSideMetadata);
}

TEST(CodecEval, ExactEntropiesAreThoseOfTheDeltasEachColumnSees) {
    const Day day = small_day(310'000'000'000);
    const auto result = evaluate_codecs(day.bytes, options_of(kBlock));
    ASSERT_TRUE(result.has_value()) << result.error();

    const ColumnResult& timestamps = column(*result, "timestamp");
    const auto deltas = pooled_deltas({day.timestamps});
    const auto dods = pooled_delta_of_deltas({day.timestamps});
    ASSERT_TRUE(timestamps.entropy_delta_bits.has_value());
    ASSERT_TRUE(timestamps.entropy_delta_of_delta_bits.has_value());
    EXPECT_NEAR(*timestamps.entropy_delta_bits, entropy_of(deltas), 1e-12);
    EXPECT_NEAR(*timestamps.entropy_delta_of_delta_bits, entropy_of(dods), 1e-12);
    EXPECT_EQ(timestamps.distinct_deltas, distinct_of(deltas));
    EXPECT_EQ(timestamps.distinct_delta_of_deltas, distinct_of(dods));
    EXPECT_FALSE(timestamps.entropy_value_bits.has_value());
    // By hand: 11 deltas, 500 three times and 1 twice, the other six once each.
    EXPECT_EQ(deltas.size(), 11U);
    EXPECT_EQ(timestamps.distinct_deltas, 8U);
    EXPECT_NEAR(*timestamps.entropy_delta_bits,
                std::log2(11.0) - (((3.0 * std::log2(3.0)) + 2.0) / 11.0), 1e-12);

    // The per-locate timestamps count none: their distinct values could be too many.
    EXPECT_FALSE(column(*result, "timestamp_per_locate").entropy_delta_bits.has_value());

    // Prices count deltas only, pooled over each column's streams.
    const auto expect_price_entropy = [&](std::string_view name,
                                          const std::vector<std::vector<std::uint64_t>>& streams) {
        const ColumnResult& got = column(*result, name);
        const auto price_deltas = pooled_deltas(streams);
        ASSERT_TRUE(got.entropy_delta_bits.has_value()) << name;
        EXPECT_NEAR(*got.entropy_delta_bits, entropy_of(price_deltas), 1e-12) << name;
        EXPECT_EQ(got.distinct_deltas, distinct_of(price_deltas)) << name;
        EXPECT_FALSE(got.entropy_delta_of_delta_bits.has_value()) << name;
        // The values' own entropy, the same whatever the order.
        std::vector<std::uint64_t> values;
        for (const std::vector<std::uint64_t>& stream : streams) {
            values.insert(values.end(), stream.begin(), stream.end());
        }
        ASSERT_TRUE(got.entropy_value_bits.has_value()) << name;
        EXPECT_NEAR(*got.entropy_value_bits, entropy_of(values), 1e-12) << name;
        EXPECT_EQ(got.distinct_values, distinct_of(values)) << name;
    };
    expect_price_entropy("add_price", {day.add_prices});
    expect_price_entropy("add_price_per_locate", streams_of(day.add_prices_by_locate));
    expect_price_entropy("add_price_per_locate_side", streams_of(day.add_prices_by_side));
    expect_price_entropy("trade_price", {day.trade_prices});
    // By hand: six add prices, 1,234,500 twice and four others once each.
    const std::optional<double> add_values = column(*result, "add_price").entropy_value_bits;
    ASSERT_TRUE(add_values.has_value());
    EXPECT_NEAR(*add_values, std::log2(6.0) - (2.0 / 6.0), 1e-12);
}

TEST(CodecEval, PermutationEstimateIsTheEntropyOfTheLocateSequence) {
    const Day day = small_day(310'000'000'000);
    const auto result = evaluate_codecs(day.bytes, options_of(kBlock));
    ASSERT_TRUE(result.has_value()) << result.error();
    const PermutationEstimate& permutation = result->permutation;
    EXPECT_EQ(permutation.messages, day.locates.size());
    EXPECT_NEAR(permutation.locate_entropy_bits, entropy_of(day.locates), 1e-12);
    EXPECT_EQ(permutation.add_messages, day.add_sides.size());
    std::vector<std::uint16_t> add_locates;
    add_locates.reserve(day.add_sides.size());
    for (const auto& [locate, sell] : day.add_sides) {
        add_locates.push_back(locate);
    }
    EXPECT_NEAR(permutation.add_locate_entropy_bits, entropy_of(add_locates), 1e-12);
    EXPECT_NEAR(permutation.add_locate_side_entropy_bits, entropy_of(day.add_sides), 1e-12);
    EXPECT_EQ(permutation.largest_locate, 9U);
    // By hand: adds from 7, 9, 7, 7, 9, 7, so 4 and 2 of 6.
    EXPECT_NEAR(permutation.add_locate_entropy_bits,
                -((4.0 / 6.0) * std::log2(4.0 / 6.0)) - ((2.0 / 6.0) * std::log2(2.0 / 6.0)),
                1e-12);
}

TEST(CodecEval, IdealBitsFollowTheCensusDefinition) {
    const Day day = small_day(310'000'000'000);
    const auto result = evaluate_codecs(day.bytes, options_of(kBlock));
    ASSERT_TRUE(result.has_value());
    const ColumnResult& timestamps = column(*result, "timestamp");
    EXPECT_GT(timestamps.ideal_delta_bits, 0.0);
    EXPECT_GT(timestamps.ideal_delta_of_delta_bits, 0.0);
    // A column of one value has neither.
    Day one;
    one.event(42);
    const auto single = evaluate_codecs(one.bytes, options_of(kBlock));
    ASSERT_TRUE(single.has_value());
    EXPECT_EQ(column(*single, "timestamp").ideal_delta_bits, 0.0);
    EXPECT_EQ(column(*single, "add_price").values, 0U);
    EXPECT_EQ(codec(column(*single, "add_price"), "gorilla-xor-u64-integer").stored_bytes(), 0U);
}

TEST(CodecEval, CountsAndReportsAValueADoubleCannotHold) {
    // 2^60 + 1 has no double: through either double it comes back as another integer,
    // or not at all, and the evaluator must say which value, not round it away.
    constexpr std::uint64_t kWide = (std::uint64_t{1} << 60U) + 1;
    const Day day = small_day(kWide);
    const auto result = evaluate_codecs(day.bytes, options_of(kBlock));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->exact());
    const ColumnResult& levels = column(*result, "mwcb_level");
    for (const std::string_view name : {"gorilla-xor-f64-dollars", "gorilla-xor-f64-integer"}) {
        const CodecResult& failed = codec(levels, name);
        EXPECT_FALSE(failed.exact()) << name;
        EXPECT_EQ(failed.representation_failures, 1U) << name;
        EXPECT_EQ(failed.word_mismatches, 0U) << name;
        ASSERT_EQ(failed.examples.size(), 1U) << name;
        EXPECT_EQ(failed.examples[0].value, kWide);
        EXPECT_EQ(failed.examples[0].index, 2U);
        EXPECT_NE(failed.examples[0].decoded, kWide);
    }
    EXPECT_TRUE(codec(levels, "gorilla-xor-u64-integer").exact());
    EXPECT_TRUE(codec(column(*result, "add_price"), "gorilla-xor-f64-dollars").exact());
}

TEST(CodecEval, KeepsOnlyTheFirstFewExamples) {
    Day day;
    for (std::uint64_t i = 0; i < 5; ++i) {
        day.mwcb(100 + i, {(std::uint64_t{1} << 60U) + 1 + (2 * i), 0, 0});
    }
    const auto result =
        evaluate_codecs(day.bytes, options_of(kBlock, 3));
    ASSERT_TRUE(result.has_value());
    const CodecResult& failed = codec(column(*result, "mwcb_level"), "gorilla-xor-f64-integer");
    EXPECT_EQ(failed.representation_failures, 5U);
    EXPECT_EQ(failed.examples.size(), 3U);
}

TEST(CodecEval, RefusesABlockSizeTheFormatCannotHold) {
    const Day day = small_day(1);
    EXPECT_FALSE(evaluate_codecs(day.bytes, options_of(0)).has_value());
    EXPECT_FALSE(
        evaluate_codecs(day.bytes, options_of(codec::kMaxBlockValues + 1))
            .has_value());
    EXPECT_TRUE(
        evaluate_codecs(day.bytes, options_of(codec::kMaxBlockValues))
            .has_value());
}

TEST(CodecEval, SkipsRawRecordsAndStopsAtATruncatedTail) {
    const auto result = evaluate_codecs(test::source_with_tail(), EvalOptions{});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->messages, test::kSourceRecordCount);
    EXPECT_GT(result->raw_skipped, 0U);
    EXPECT_FALSE(result->stop.empty());
    EXPECT_EQ(column(*result, "timestamp").values,
              test::kSourceRecordCount - result->raw_skipped);
    // The fixture's MWCB levels are byte patterns far above 2^53, which no double holds
    // exactly. Those three, and only those, fail, and only through a double.
    for (const ColumnResult& got : result->columns) {
        for (const CodecResult& codec_result : got.codecs) {
            const bool through_double =
                codec_result.entry->representation != Representation::identity;
            const bool levels = got.spec->name == "mwcb_level";
            EXPECT_EQ(codec_result.representation_failures, levels && through_double ? 3U : 0U)
                << got.spec->name << " " << codec_result.entry->name;
            EXPECT_EQ(codec_result.word_mismatches, 0U);
            EXPECT_EQ(codec_result.blocks_refused, 0U);
        }
    }
    EXPECT_FALSE(result->exact());
}

TEST(CodecEval, StoredWordsAreThePriceRepresentations) {
    EXPECT_EQ(to_stored_word(Representation::identity, ValueKind::price4, 1'234'500), 1'234'500U);
    EXPECT_EQ(to_stored_word(Representation::dollars, ValueKind::price4, 1'234'500),
              std::bit_cast<std::uint64_t>(123.45));
    EXPECT_EQ(to_stored_word(Representation::dollars, ValueKind::price8, 12'345'000'000),
              std::bit_cast<std::uint64_t>(123.45));
    EXPECT_EQ(to_stored_word(Representation::integer_as_double, ValueKind::price4, 7),
              std::bit_cast<std::uint64_t>(7.0));
    EXPECT_EQ(from_stored_word(Representation::dollars, ValueKind::price4,
                               std::bit_cast<std::uint64_t>(123.45)),
              1'234'500U);
    // A dollar value above the Price(4) range is refused, not wrapped.
    EXPECT_FALSE(from_stored_word(Representation::dollars, ValueKind::price4,
                                  std::bit_cast<std::uint64_t>(429'496.7296))
                     .has_value());
    EXPECT_FALSE(from_stored_word(Representation::integer_as_double, ValueKind::price4,
                                  std::bit_cast<std::uint64_t>(0.5))
                     .has_value());
}

TEST(CodecEval, EveryCodecHasAUniqueNameAndAColumn) {
    std::set<std::string_view> names;
    for (const CodecEntry& entry : codec_registry()) {
        EXPECT_TRUE(names.insert(entry.name).second) << entry.name;
        EXPECT_NE(entry.kinds, 0U) << entry.name;
        EXPECT_EQ(entry.kinds >> kValueKinds, 0U) << entry.name;
        EXPECT_NE(entry.encode_block, nullptr);
        EXPECT_NE(entry.decode_block, nullptr);
    }
    std::set<std::string_view> columns;
    for (const ColumnSpec& spec : column_specs()) {
        EXPECT_TRUE(columns.insert(spec.name).second) << spec.name;
    }
}

// A day that exercises the book: adds, executions, a cancel, a replace, deletes, one of
// them of an order never added and one of an order already executed in full, and the
// other columns' fields, with every value each new column should see written out by hand.
itch::Stock stock_of(std::string_view name) {
    itch::Stock stock{};
    stock.fill(std::byte{' '});
    for (std::size_t i = 0; i < name.size(); ++i) {
        stock[i] = static_cast<std::byte>(name[i]);
    }
    return stock;
}

Bytes book_day() {
    Bytes bytes;
    itch::StockDirectory directory;
    directory.header = header(7, 100);
    directory.stock = stock_of("AAPL");
    append(bytes, directory);
    const auto add = [&](std::uint64_t ref, std::uint16_t locate, char side, std::uint32_t shares,
                         std::string_view stock) {
        itch::AddOrder message;
        message.header = header(locate, 200);
        message.order_reference_number = ref;
        message.buy_sell_indicator = static_cast<std::byte>(side);
        message.shares = shares;
        message.stock = stock_of(stock);
        message.price = itch::Price4{1'000};
        append(bytes, message);
    };
    add(100, 7, 'B', 100, "AAPL");
    itch::AddOrderWithMpid with_mpid;
    with_mpid.header = header(9, 300);
    with_mpid.order_reference_number = 104;
    with_mpid.buy_sell_indicator = std::byte{'S'};
    with_mpid.shares = 200;
    with_mpid.stock = stock_of("MSFT");
    with_mpid.price = itch::Price4{2'000};
    with_mpid.attribution = {std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'D'}};
    append(bytes, with_mpid);
    add(103, 7, 'S', 300, "AAPL");
    const auto executed = [&](std::uint64_t ref, std::uint16_t locate, std::uint32_t shares,
                              std::uint64_t match) {
        itch::OrderExecuted message;
        message.header = header(locate, 400);
        message.order_reference_number = ref;
        message.executed_shares = shares;
        message.match_number = match;
        append(bytes, message);
    };
    executed(100, 7, 40, 5'000);
    itch::OrderExecutedWithPrice with_price;
    with_price.header = header(9, 500);
    with_price.order_reference_number = 104;
    with_price.executed_shares = 200;
    with_price.match_number = 5'001;
    with_price.printable = std::byte{'Y'};
    with_price.execution_price = itch::Price4{2'000};
    append(bytes, with_price);
    itch::OrderCancel cancel;
    cancel.header = header(7, 600);
    cancel.order_reference_number = 100;
    cancel.cancelled_shares = 60;
    append(bytes, cancel);
    itch::OrderReplace replace;
    replace.header = header(7, 700);
    replace.original_order_reference_number = 103;
    replace.new_order_reference_number = 110;
    replace.shares = 50;
    replace.price = itch::Price4{1'100};
    append(bytes, replace);
    const auto del = [&](std::uint64_t ref) {
        itch::OrderDelete message;
        message.header = header(7, 800);
        message.order_reference_number = ref;
        append(bytes, message);
    };
    del(110);
    del(999);
    itch::Trade trade;
    trade.header = header(9, 900);
    trade.buy_sell_indicator = std::byte{'B'};
    trade.shares = 10;
    trade.stock = stock_of("MSFT");
    trade.match_number = 5'002;
    append(bytes, trade);
    executed(104, 9, 5, 5'003);
    return bytes;
}

using Streams = std::map<std::size_t, std::vector<std::uint64_t>>;

// Every block each column cut, rejoined per stream.
std::map<std::string, Streams, std::less<>> blocks_of(const Bytes& day, EvalOptions options) {
    std::map<std::string, Streams, std::less<>> seen;
    options.observe_block = [&](std::string_view name, std::size_t stream,
                                std::span<const std::uint64_t> block) {
        auto& values = seen[std::string{name}][stream];
        values.insert(values.end(), block.begin(), block.end());
    };
    const auto result = evaluate_codecs(day, options);
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result.has_value() && result->exact());
    return seen;
}

std::vector<std::uint64_t> bytes_of(std::string_view text) {
    std::vector<std::uint64_t> values;
    for (const char c : text) {
        values.push_back(static_cast<unsigned char>(c));
    }
    return values;
}

TEST(CodecEval, BookColumnsHoldWhatEachMessageNames) {
    const auto seen = blocks_of(book_day(), options_of(kBlock));
    const auto file = [&](std::string_view name) {
        const auto found = seen.find(name);
        EXPECT_NE(found, seen.end()) << name;
        if (found == seen.end()) {
            return std::vector<std::uint64_t>{};
        }
        EXPECT_EQ(found->second.size(), 1U) << name;
        return found->second.begin()->second;
    };
    using V = std::vector<std::uint64_t>;
    EXPECT_EQ(file("add_ref"), (V{100, 104, 103}));
    // zigzag(100 - 0), zigzag(104 - 100), zigzag(103 - 104).
    EXPECT_EQ(file("add_ref_vs_running_max"), (V{200, 8, 1}));
    EXPECT_EQ(file("creation_ref"), (V{100, 104, 103, 110}));
    EXPECT_EQ(file("replace_new_ref"), (V{110}));
    EXPECT_EQ(file("replace_new_minus_original"), (V{14}));
    EXPECT_EQ(file("referenced_ref"), (V{100, 104, 100, 103, 110, 999, 104}));
    // Creations so far less the order's: 0 for an order not on the book, as 999 never was
    // and 104 no longer is once C executed all its shares.
    EXPECT_EQ(file("ref_distance_E"), (V{3, 0}));
    EXPECT_EQ(file("ref_locate_distance_E"), (V{2, 0}));
    EXPECT_EQ(file("ref_distance_C"), (V{2}));
    EXPECT_EQ(file("ref_locate_distance_C"), (V{1}));
    EXPECT_EQ(file("ref_distance_X"), (V{3}));
    EXPECT_EQ(file("ref_locate_distance_X"), (V{2}));
    EXPECT_EQ(file("ref_distance_D"), (V{1, 0}));
    EXPECT_EQ(file("ref_locate_distance_D"), (V{1, 0}));
    EXPECT_EQ(file("ref_distance_U"), (V{1}));
    EXPECT_EQ(file("ref_locate_distance_U"), (V{1}));
    EXPECT_EQ(file("add_shares"), (V{100, 200, 300}));
    EXPECT_EQ(file("executed_shares"), (V{40, 5}));
    EXPECT_EQ(file("executed_with_price_shares"), (V{200}));
    EXPECT_EQ(file("cancelled_shares"), (V{60}));
    EXPECT_EQ(file("replace_shares"), (V{50}));
    EXPECT_EQ(file("trade_shares"), (V{10}));
    EXPECT_EQ(file("match_number"), (V{5'000, 5'001, 5'002, 5'003}));
    EXPECT_EQ(file("message_type"), bytes_of("RAFAECXUDDPE"));
    EXPECT_EQ(file("stock_locate"), (V{7, 7, 9, 7, 7, 9, 7, 7, 7, 7, 9, 9}));
    EXPECT_EQ(file("stock_locate_unreferenced"), (V{7, 7, 9, 7, 9}));
    EXPECT_EQ(file("add_side"), bytes_of("BSS"));
    EXPECT_EQ(file("mpid"), (V{0x41424344}));
    EXPECT_EQ(file("printable"), bytes_of("Y"));
    EXPECT_EQ(file("trade_ref"), (V{0}));
    EXPECT_EQ(file("trade_side"), bytes_of("B"));
    // R's bytes after the common header, as the wire holds them.
    EXPECT_EQ(file("rare_rest_bytes").size(), itch::StockDirectory::wire_size - 11);

    // Reductions as 0 where they take all the order has left, else shares + 1, one stream
    // per type: E is alternative 12, C 13, X 14.
    const Streams& reductions = seen.at("reduction_shares_vs_remaining");
    EXPECT_EQ(reductions.at(12), (V{41, 6}));
    EXPECT_EQ(reductions.at(13), (V{0}));
    EXPECT_EQ(reductions.at(14), (V{0}));
    const Streams& by_type = seen.at("referenced_ref_per_type");
    EXPECT_EQ(by_type.at(12), (V{100, 104}));
    EXPECT_EQ(by_type.at(15), (V{110, 999}));
    EXPECT_EQ(by_type.at(16), (V{103}));
    const Streams& by_locate = seen.at("add_ref_per_locate");
    EXPECT_EQ(by_locate.at(7), (V{100, 103}));
    EXPECT_EQ(by_locate.at(9), (V{104}));
}

TEST(CodecEval, AnalysisCountsTheBookAndTheDerivableFields) {
    const auto result = evaluate_codecs(book_day(), options_of(kBlock));
    ASSERT_TRUE(result.has_value()) << result.error();
    const Analysis& a = result->analysis;
    ASSERT_EQ(a.references.size(), 5U);
    const auto locality = [&](char type) {
        return *std::ranges::find(a.references, type, &ReferenceLocality::type);
    };
    EXPECT_EQ(locality('E').messages, 2U);
    EXPECT_EQ(locality('E').unresolved, 1U);
    EXPECT_EQ(locality('E').shares_equal_remaining, 0U);
    EXPECT_EQ(locality('C').shares_equal_remaining, 1U);
    EXPECT_EQ(locality('X').shares_equal_remaining, 1U);
    EXPECT_EQ(locality('D').messages, 2U);
    EXPECT_EQ(locality('D').unresolved, 1U);
    EXPECT_EQ(locality('U').unresolved, 0U);
    for (const ReferenceLocality& r : a.references) {
        EXPECT_EQ(r.locate_mismatches, 0U) << r.type;
        // Every resolved order is a few creations back, so in every window but the
        // aligned blocks, which hold all four creations too at these sizes.
        EXPECT_EQ(r.within_distance[0], r.messages - r.unresolved) << r.type;
        EXPECT_EQ(r.same_block[0], r.messages - r.unresolved) << r.type;
    }
    EXPECT_EQ(a.peak_live_orders, 3U);
    EXPECT_EQ(a.live_orders_at_end, 0U);
    EXPECT_EQ(a.duplicate_creations, 0U);

    // The heads: 100 escapes, 104 and 103 extend it or escape, as the encoder decides.
    EXPECT_EQ(a.add_heads.values, 3U);
    std::uint64_t tokens = 0;
    for (const std::uint64_t count : a.add_heads.tokens) {
        tokens += count;
    }
    EXPECT_EQ(tokens, 3U);
    EXPECT_EQ(a.add_heads.consecutive, 2U);
    EXPECT_EQ(a.creation_heads.values, 4U);
    // Creations 100 B, 104 S, 103 S, then U's 110, which keeps 103's side, S: two of the
    // three consecutive pairs are on one side. A U carries no side of its own.
    EXPECT_EQ(a.creation_heads.consecutive, 3U);
    EXPECT_EQ(a.creation_heads.consecutive_side_pairs, 3U);
    EXPECT_EQ(a.creation_heads.consecutive_same_side, 2U);
    // Plain deltas 104 - 100 = 4 and 103 - 104 = -1.
    EXPECT_EQ(a.add_heads.small_deltas[16 + 4], 1U);
    EXPECT_EQ(a.add_heads.small_deltas[16 - 1], 1U);

    // Recency ranks of the locates 7 7 9 7 7 9 7 7 7 7 9 9: two first uses, then 0 and 1.
    const std::vector<int> ranks{-1, 0, -1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
    EXPECT_NEAR(a.locate_recency_entropy_bits, entropy_of(ranks), 1e-12);
    EXPECT_EQ(a.locate_first_uses, 2U);
    // The type sequence R A F A E C X U D D P E.
    const std::vector<char> types{'R', 'A', 'F', 'A', 'E', 'C', 'X', 'U', 'D', 'D', 'P', 'E'};
    EXPECT_NEAR(a.type_entropy_bits, entropy_of(types), 1e-12);

    // R sets AAPL on locate 7. F and P carry MSFT on locate 9, which has no entry.
    EXPECT_EQ(a.stock_fields[1], 1U);
    EXPECT_EQ(a.stock_mismatches[1], 0U);
    EXPECT_EQ(a.stock_fields[10], 2U);
    EXPECT_EQ(a.stock_mismatches[10], 0U);
    EXPECT_EQ(a.stock_mismatches[11], 1U);
    EXPECT_EQ(a.stock_mismatches[17], 1U);
}

// A U whose original is not on the book has no known side, so its steps are left out of
// the side counts, and so are the steps of a U that replaces it in turn.
TEST(CodecEval, AReplaceOfAnOrderNotOnTheBookHasNoSide) {
    Bytes bytes;
    itch::AddOrder add;
    add.header = header(7, 100);
    add.order_reference_number = 100;
    add.buy_sell_indicator = std::byte{'S'};
    add.shares = 10;
    add.price = itch::Price4{1'000};
    append(bytes, add);
    const auto replace = [&](std::uint64_t original, std::uint64_t replacement) {
        itch::OrderReplace message;
        message.header = header(7, 200);
        message.original_order_reference_number = original;
        message.new_order_reference_number = replacement;
        message.shares = 10;
        message.price = itch::Price4{1'000};
        append(bytes, message);
    };
    replace(100, 101);  // keeps S
    replace(500, 102);  // 500 was never added: no side
    replace(102, 103);  // 102's side is still unknown
    const auto result = evaluate_codecs(bytes, options_of(kBlock));
    ASSERT_TRUE(result.has_value()) << result.error();
    const HeadModes& heads = result->analysis.creation_heads;
    EXPECT_EQ(heads.values, 4U);
    EXPECT_EQ(heads.consecutive, 3U);
    EXPECT_EQ(heads.consecutive_side_pairs, 1U);
    EXPECT_EQ(heads.consecutive_same_side, 1U);
    // Each value extends head 0: 100 against the empty head's owner, 101 against 100's, both
    // with sides; 102 and 103 have none.
    EXPECT_EQ(heads.head_steps, 4U);
    EXPECT_EQ(heads.side_steps, 2U);
    EXPECT_EQ(heads.same_side, 1U);
}

// More messages than the recency ranks have use times, so they renumber several times,
// against move-to-front over a plain list.
TEST(CodecEval, LocateRecencyRanksSurviveRenumbering) {
    Bytes bytes;
    std::vector<std::int64_t> ranks;
    std::vector<std::uint16_t> list;
    constexpr std::uint64_t kMessages = 300'000;
    for (std::uint64_t i = 0; i < kMessages; ++i) {
        const auto locate = static_cast<std::uint16_t>(
            i % 9'973 == 0 ? 1'000 + (i % 7'000) : 1 + ((i * i) % 37));
        itch::SystemEvent event;
        event.header = header(locate, 1'000 + i);
        append(bytes, event);
        const auto found = std::ranges::find(list, locate);
        if (found == list.end()) {
            ranks.push_back(-1);
            list.insert(list.begin(), locate);
        } else {
            ranks.push_back(found - list.begin());
            list.erase(found);
            list.insert(list.begin(), locate);
        }
    }
    EvalOptions options = options_of(codec::kDefaultBlockValues);
    options.columns = {"message_type"};
    const auto result = evaluate_codecs(bytes, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->analysis.locate_recency_entropy_bits, entropy_of(ranks), 1e-9);
    EXPECT_EQ(result->analysis.locate_first_uses,
              static_cast<std::uint64_t>(std::ranges::count(ranks, -1)));
}

TEST(CodecEval, ZstdSeesEveryRawAndEncodedBlockAndGivesItBack) {
    const auto result = evaluate_codecs(book_day(), options_of(kBlock));
    ASSERT_TRUE(result.has_value());
    for (const ColumnResult& got : result->columns) {
        EXPECT_EQ(got.raw_bytes, got.values * got.spec->raw_bytes) << got.spec->name;
        EXPECT_EQ(got.raw_zstd_mismatches, 0U);
        EXPECT_EQ(got.raw_zstd_bytes > 0, got.values > 0) << got.spec->name;
        for (const CodecResult& c : got.codecs) {
            EXPECT_EQ(c.zstd_mismatches, 0U);
            EXPECT_EQ(c.zstd_bytes > 0, c.blocks > 0) << got.spec->name << " " << c.entry->name;
            // Each block costs the smaller of its frame and itself, and a byte to say which.
            EXPECT_LE(c.zstd_or_plain_bytes, c.block_bytes + c.blocks);
            EXPECT_LE(c.zstd_or_plain_bytes, c.zstd_bytes + c.blocks);
        }
    }
}

TEST(CodecEval, ASubsetRunsOnlyItsColumnsAndRefusesAnUnknownName) {
    EvalOptions options = options_of(kBlock);
    options.columns = {"add_ref", "message_type"};
    const auto result = evaluate_codecs(book_day(), options);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->columns.size(), 2U);
    EXPECT_EQ(result->columns[0].spec->name, "add_ref");
    EXPECT_EQ(result->columns[1].spec->name, "message_type");
    EXPECT_EQ(result->analysis.references.size(), 5U);
    options.columns = {"no_such_column"};
    EXPECT_FALSE(evaluate_codecs(book_day(), options).has_value());
}

TEST(CodecEval, EachKindOfColumnGetsItsCodecs) {
    const auto result = evaluate_codecs(book_day(), options_of(kBlock));
    ASSERT_TRUE(result.has_value());
    const auto names = [&](std::string_view column_name) {
        std::set<std::string_view> out;
        for (const CodecResult& c : column(*result, column_name).codecs) {
            out.insert(c.entry->name);
        }
        return out;
    };
    EXPECT_TRUE(names("add_ref").contains("pfor-head-delta"));
    EXPECT_FALSE(names("add_ref").contains("dict"));
    EXPECT_TRUE(names("add_shares").contains("dict"));
    EXPECT_TRUE(names("add_shares").contains("rle"));
    EXPECT_TRUE(names("message_type").contains("dict"));
    EXPECT_FALSE(names("message_type").contains("pfor-head-delta"));
    EXPECT_FALSE(names("timestamp").contains("dict"));
}

std::string read_text(const std::filesystem::path& path) {
    const Bytes bytes = test::read_file(path);
    return {reinterpret_cast<const char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): reading bytes as text
            bytes.size()};
}

constexpr EvalProvenance kProvenance{.commit = "0123456789abcdef0123",
                                     .tree_dirty = false,
                                     .compiler = "Clang 0",
                                     .build_type = "Release",
                                     .sanitizers = "none"};

TEST(CodecEvalCli, WritesJsonWithNoPathsAndExitsZero) {
    const test::ScratchDir dir;
    test::write_file(dir / "day", small_day(310'000'000'000).bytes);
    const std::string source = (dir / "day").string();
    const std::string out = (dir / "eval.json").string();
    const std::vector<std::string_view> arguments{source, out, "--day", "D1", "--block-values",
                                                  "2"};
    CliOutput output;
    EXPECT_EQ(run_codec_eval_cli(arguments, kProvenance, output), kExitPass) << output.err;
    const std::string text = read_text(out);
    EXPECT_TRUE(text.contains(R"("day": "D1")"));
    EXPECT_TRUE(text.contains(R"("source": "day")"));
    EXPECT_TRUE(text.contains(R"("commit": "0123456789ab")"));
    EXPECT_TRUE(text.contains(R"("block_values": 2)"));
    EXPECT_TRUE(text.contains(R"("every_value_exact": true)"));
    EXPECT_TRUE(text.contains(R"("codec": "gorilla-dod")"));
    EXPECT_TRUE(text.contains(R"("target": "CR-3")"));
    EXPECT_TRUE(text.contains(R"("target": "CR-4")"));
    EXPECT_TRUE(text.contains(R"("codec": "pfor-delta")"));
    EXPECT_TRUE(text.contains(R"("family": "gorilla")"));
    EXPECT_TRUE(text.contains(R"("family": "pfor")"));
    EXPECT_TRUE(text.contains(R"("against_gorilla_xor_f64_dollars")"));
    EXPECT_TRUE(text.contains(R"("against_best_gorilla")"));
    EXPECT_TRUE(text.contains(R"("permutation_estimate")"));
    EXPECT_TRUE(text.contains(R"("entropy_delta_bits")"));
    EXPECT_TRUE(text.contains(R"("entropy_value_bits")"));
    EXPECT_TRUE(text.contains(R"("target": "CR-6")"));
    EXPECT_TRUE(text.contains(R"("target": "CR-7")"));
    EXPECT_TRUE(text.contains(R"("analysis")"));
    EXPECT_TRUE(text.contains(R"("reference_locality")"));
    EXPECT_TRUE(text.contains(R"("zstd1_bytes")"));
    EXPECT_TRUE(text.contains(R"("raw_zstd1_bytes")"));
    EXPECT_TRUE(text.contains(R"("columns_subset": null)"));
    EXPECT_TRUE(text.contains(R"("price4_exhaustive_proofs": [])"));
    EXPECT_FALSE(text.contains(dir.path().string()));
    EXPECT_TRUE(output.out.contains("timestamp"));
}

TEST(CodecEvalCli, ExitsOneWhenAValueDoesNotComeBack) {
    const test::ScratchDir dir;
    test::write_file(dir / "day", small_day((std::uint64_t{1} << 60U) + 1).bytes);
    const std::string source = (dir / "day").string();
    const std::string out = (dir / "eval.json").string();
    const std::vector<std::string_view> arguments{source, out, "--day", "D1"};
    CliOutput output;
    EXPECT_EQ(run_codec_eval_cli(arguments, kProvenance, output), kExitMismatch);
    const std::string text = read_text(out);
    EXPECT_TRUE(text.contains(R"("every_value_exact": false)"));
    EXPECT_TRUE(text.contains(R"("representation_failures": 1)"));
    EXPECT_TRUE(text.contains(R"("value": 1152921504606846977)"));
    EXPECT_TRUE(output.out.contains("NOT EXACT"));
}

TEST(CodecEvalCli, FailsWithExitTwo) {
    const test::ScratchDir dir;
    test::write_file(dir / "day", small_day(1).bytes);
    const std::string source = (dir / "day").string();
    const std::string out = (dir / "eval.json").string();
    const std::string absent = (dir / "absent").string();
    const std::string no_dir = (dir / "no" / "dir").string();
    const auto run = [](std::vector<std::string_view> arguments) {
        CliOutput output;
        return run_codec_eval_cli(arguments, kProvenance, output);
    };
    EXPECT_EQ(run({}), kExitUsageOrIo);
    EXPECT_EQ(run({source}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D", "--block-values", "0"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D", "--block-values", "2x"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D", "--block-values", "1048577"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D", "--colour", "red"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D", "--columns", ""}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D", "--columns", "add_ref,"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D", "--columns", "no_such_column"}), kExitUsageOrIo);
    EXPECT_EQ(run({absent, out, "--day", "D"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, no_dir, "--day", "D"}), kExitUsageOrIo);
    EXPECT_EQ(run({source, out, "--day", "D"}), kExitPass);
    EXPECT_EQ(run({source, out, "--day", "D", "--columns", "timestamp,add_ref"}), kExitPass);
    const std::string text = read_text(out);
    EXPECT_TRUE(text.contains(R"("columns_subset": [)"));
    EXPECT_TRUE(text.contains(R"("name": "add_ref")"));
    EXPECT_FALSE(text.contains(R"("name": "add_price")"));
}

}  // namespace
}  // namespace tick_store::harness
