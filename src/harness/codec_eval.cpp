#include "tick_store/harness/codec_eval.hpp"

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/harness/census.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::harness {

namespace {

constexpr std::string_view kToolVersion = "1";
constexpr std::size_t kLocates = std::size_t{1} << 16U;
// Bytes a reader needs per stream beyond its blocks: the value count, and for a
// per-locate stream the locate it belongs to.
constexpr std::uint64_t kStreamCountBytes = 8;
constexpr std::uint64_t kStreamLocateBytes = 2;

void add_dod(CodecCounters& counters, const codec::DodStats& stats) {
    for (std::size_t i = 0; i < stats.buckets.size(); ++i) {
        counters[i] += stats.buckets[i];
    }
}

std::expected<void, codec::CodecError> gorilla_dod_block(std::span<const std::uint64_t> values,
                                                         std::vector<std::byte>& out,
                                                         CodecCounters& counters) {
    codec::DodStats stats;
    auto done = codec::encode_timestamp_block(values, out, &stats);
    add_dod(counters, stats);
    return done;
}

std::expected<void, codec::CodecError> gorilla_xor_block(std::span<const std::uint64_t> words,
                                                         std::vector<std::byte>& out,
                                                         CodecCounters& counters) {
    codec::XorStats stats;
    auto done = codec::encode_xor_block(words, out, &stats);
    counters[0] += stats.zero;
    counters[1] += stats.reuse;
    counters[2] += stats.new_window;
    counters[3] += stats.data_bits;
    return done;
}

constexpr std::array<std::string_view, kCodecCounters> kDodCounters{
    "zero", "bucket_16_bits", "bucket_19_bits", "bucket_23_bits", "escape_64_bits"};
constexpr std::array<std::string_view, kCodecCounters> kXorCounters{"zero", "reuse",
                                                                    "new_window", "data_bits", ""};

// The codecs measured. A new codec is one more row here.
constexpr std::array kRegistry{
    CodecEntry{.name = "gorilla-dod",
               .what = "Gorilla delta-of-delta, nanosecond buckets of 16, 19 and 23 bits",
               .encode_block = &gorilla_dod_block,
               .decode_block = &codec::decode_timestamp_block,
               .representation = Representation::identity,
               .for_timestamps = true,
               .for_prices = false,
               .counter_names = kDodCounters},
    CodecEntry{.name = "gorilla-xor-f64-dollars",
               .what = "Gorilla XOR on the dollar value as a double, price / 10^4 or / 10^8",
               .encode_block = &gorilla_xor_block,
               .decode_block = &codec::decode_xor_block,
               .representation = Representation::dollars,
               .for_timestamps = false,
               .for_prices = true,
               .counter_names = kXorCounters},
    CodecEntry{.name = "gorilla-xor-f64-integer",
               .what = "Gorilla XOR on the integer price as a double",
               .encode_block = &gorilla_xor_block,
               .decode_block = &codec::decode_xor_block,
               .representation = Representation::integer_as_double,
               .for_timestamps = false,
               .for_prices = true,
               .counter_names = kXorCounters},
    CodecEntry{.name = "gorilla-xor-u64-integer",
               .what = "Gorilla XOR on the raw integer price",
               .encode_block = &gorilla_xor_block,
               .decode_block = &codec::decode_xor_block,
               .representation = Representation::identity,
               .for_timestamps = false,
               .for_prices = true,
               .counter_names = kXorCounters},
};

// The columns, in report order. kColumn* below index this table.
constexpr std::array kColumns{
    ColumnSpec{.name = "timestamp",
               .source = "every message's timestamp",
               .kind = ValueKind::timestamp_ns,
               .order = StreamOrder::file,
               .target = "CR-3"},
    ColumnSpec{.name = "timestamp_per_locate",
               .source = "every message's timestamp, one stream per stock locate",
               .kind = ValueKind::timestamp_ns,
               .order = StreamOrder::per_locate,
               .target = "CR-3"},
    ColumnSpec{.name = "add_price",
               .source = "A and F price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = "CR-4"},
    ColumnSpec{.name = "add_price_per_locate",
               .source = "A and F price, one stream per stock locate",
               .kind = ValueKind::price4,
               .order = StreamOrder::per_locate,
               .target = "CR-4"},
    ColumnSpec{.name = "replace_price",
               .source = "U price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = ""},
    ColumnSpec{.name = "executed_price",
               .source = "C execution price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = ""},
    ColumnSpec{.name = "trade_price",
               .source = "P price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = ""},
    ColumnSpec{.name = "mwcb_level",
               .source = "V levels 1, 2 and 3, in that order: the only Price(8) fields",
               .kind = ValueKind::price8,
               .order = StreamOrder::file,
               .target = ""},
};

constexpr std::size_t kColumnTimestamp = 0;
constexpr std::size_t kColumnTimestampPerLocate = 1;
constexpr std::size_t kColumnAddPrice = 2;
constexpr std::size_t kColumnAddPricePerLocate = 3;
constexpr std::size_t kColumnReplacePrice = 4;
constexpr std::size_t kColumnExecutedPrice = 5;
constexpr std::size_t kColumnTradePrice = 6;
constexpr std::size_t kColumnMwcbLevel = 7;
static_assert(kColumnMwcbLevel + 1 == kColumns.size());

[[nodiscard]] bool applies(const CodecEntry& entry, const ColumnSpec& spec) noexcept {
    return spec.kind == ValueKind::timestamp_ns ? entry.for_timestamps : entry.for_prices;
}

[[nodiscard]] std::uint64_t scale_of(ValueKind kind) noexcept {
    return kind == ValueKind::price8 ? codec::kPrice8Scale : codec::kPrice4Scale;
}

[[nodiscard]] std::uint64_t max_of(ValueKind kind) noexcept {
    return kind == ValueKind::price4 ? std::numeric_limits<std::uint32_t>::max()
                                     : std::numeric_limits<std::uint64_t>::max();
}

// The census's view of one stream: deltas from the second value, delta-of-deltas from
// the third, both as signed differences. Differences wrap like the codecs' do.
struct StreamShape {
    std::uint64_t previous = 0;
    std::uint64_t previous_delta = 0;
    std::uint8_t seen = 0;

    void observe(std::uint64_t value, Log2Histogram& delta, Log2Histogram& dod) noexcept {
        if (seen > 0) {
            const std::uint64_t step = value - previous;
            delta.add(static_cast<std::int64_t>(step));
            if (seen > 1) {
                dod.add(static_cast<std::int64_t>(step - previous_delta));
            }
            previous_delta = step;
        }
        seen = seen > 1 ? seen : static_cast<std::uint8_t>(seen + 1);
        previous = value;
    }
};

// One column through every codec registered for it, a block at a time.
class ColumnRun {
public:
    ColumnRun(const ColumnSpec& spec, const EvalOptions& options)
        : spec_(&spec),
          options_(options),
          streams_(spec.order == StreamOrder::file ? 1 : kLocates),
          shapes_(streams_.size()) {
        for (const CodecEntry& entry : kRegistry) {
            if (applies(entry, spec)) {
                CodecResult result;
                result.entry = &entry;
                results_.push_back(std::move(result));
            }
        }
    }

    void add(std::uint16_t locate, std::uint64_t value) {
        const std::size_t stream = spec_->order == StreamOrder::file ? 0 : locate;
        Stream& into = streams_[stream];
        into.pending.push_back(value);
        shapes_[stream].observe(value, delta_, dod_);
        ++values_;
        if (into.pending.size() == options_.block_values) {
            flush(into, static_cast<std::uint16_t>(stream));
        }
    }

    [[nodiscard]] ColumnResult finish() {
        std::uint64_t streams = 0;
        for (std::size_t stream = 0; stream < streams_.size(); ++stream) {
            Stream& from = streams_[stream];
            if (!from.pending.empty()) {
                flush(from, static_cast<std::uint16_t>(stream));
            }
            if (from.values > 0) {
                ++streams;
            }
        }
        const std::uint64_t per_stream =
            kStreamCountBytes + (spec_->order == StreamOrder::per_locate ? kStreamLocateBytes : 0);
        for (CodecResult& result : results_) {
            result.metadata_bytes = streams * per_stream;
        }
        return ColumnResult{.spec = spec_,
                            .values = values_,
                            .streams = streams,
                            .ideal_delta_bits = delta_.ideal_code_bits(),
                            .ideal_delta_of_delta_bits = dod_.ideal_code_bits(),
                            .codecs = std::move(results_)};
    }

private:
    struct Stream {
        std::vector<std::uint64_t> pending;
        // Values already flushed, so an example can name its position in the stream.
        std::uint64_t values = 0;
    };

    void note(CodecResult& result, const Example& example) const {
        if (result.examples.size() < options_.max_examples) {
            result.examples.push_back(example);
        }
    }

    void flush(Stream& stream, std::uint16_t locate) {
        const std::span<const std::uint64_t> block{stream.pending};
        for (CodecResult& result : results_) {
            check_block(result, block, stream.values, locate);
        }
        stream.values += block.size();
        stream.pending.clear();
    }

    void check_block(CodecResult& result, std::span<const std::uint64_t> block,
                     std::uint64_t first_index, std::uint16_t locate) {
        const CodecEntry& entry = *result.entry;
        words_.clear();
        for (const std::uint64_t value : block) {
            words_.push_back(to_stored_word(entry.representation, spec_->kind, value));
        }
        result.values += block.size();
        const auto refuse = [&] {
            ++result.blocks_refused;
            result.word_mismatches += block.size();
            note(result, Example{.index = first_index,
                                 .locate = locate,
                                 .value = block.front(),
                                 .stored_word = words_.front(),
                                 .decoded = std::nullopt});
        };
        encoded_.clear();
        if (!entry.encode_block(words_, encoded_, result.counters).has_value()) {
            refuse();
            return;
        }
        ++result.blocks;
        result.block_bytes += encoded_.size();
        result.header_bytes += codec::kBlockHeaderSize;

        decoded_.clear();
        const auto taken = entry.decode_block(encoded_, decoded_);
        if (!taken.has_value() || *taken != encoded_.size() || decoded_.size() != block.size()) {
            refuse();
            return;
        }
        for (std::size_t i = 0; i < block.size(); ++i) {
            const Example example{.index = first_index + i,
                                  .locate = locate,
                                  .value = block[i],
                                  .stored_word = words_[i],
                                  .decoded = std::nullopt};
            if (decoded_[i] != words_[i]) {
                ++result.word_mismatches;
                note(result, example);
                continue;
            }
            const auto back = from_stored_word(entry.representation, spec_->kind, decoded_[i]);
            if (!back.has_value() || *back != block[i]) {
                ++result.representation_failures;
                Example failed = example;
                if (back.has_value()) {
                    failed.decoded = *back;
                }
                note(result, failed);
            }
        }
    }

    const ColumnSpec* spec_;
    EvalOptions options_;
    std::vector<Stream> streams_;
    std::vector<StreamShape> shapes_;
    Log2Histogram delta_;
    Log2Histogram dod_;
    std::uint64_t values_ = 0;
    std::vector<CodecResult> results_;
    std::vector<std::uint64_t> words_;
    std::vector<std::byte> encoded_;
    std::vector<std::uint64_t> decoded_;
};

// Feeds each message's fields to the columns that take them.
class Walk {
public:
    explicit Walk(const EvalOptions& options) {
        runs_.reserve(kColumns.size());
        for (const ColumnSpec& spec : kColumns) {
            runs_.emplace_back(spec, options);
        }
    }

    void observe(const itch::Message& message) {
        ++messages_;
        std::visit([this]<class M>(const M& typed) { observe_typed(typed); }, message);
    }

    [[nodiscard]] EvalResult finish(std::string stop) {
        EvalResult result;
        result.messages = messages_;
        result.raw_skipped = raw_;
        result.stop = std::move(stop);
        for (ColumnRun& run : runs_) {
            result.columns.push_back(run.finish());
        }
        return result;
    }

private:
    template <class M>
    void observe_typed(const M& typed) {
        if constexpr (std::same_as<M, itch::RawMessage>) {
            ++raw_;
        } else {
            const std::uint16_t locate = typed.header.stock_locate;
            runs_[kColumnTimestamp].add(locate, typed.header.timestamp_ns);
            runs_[kColumnTimestampPerLocate].add(locate, typed.header.timestamp_ns);
            if constexpr (std::same_as<M, itch::AddOrder> ||
                          std::same_as<M, itch::AddOrderWithMpid>) {
                runs_[kColumnAddPrice].add(locate, price(typed.price));
                runs_[kColumnAddPricePerLocate].add(locate, price(typed.price));
            } else if constexpr (std::same_as<M, itch::OrderReplace>) {
                runs_[kColumnReplacePrice].add(locate, price(typed.price));
            } else if constexpr (std::same_as<M, itch::OrderExecutedWithPrice>) {
                runs_[kColumnExecutedPrice].add(locate, price(typed.execution_price));
            } else if constexpr (std::same_as<M, itch::Trade>) {
                runs_[kColumnTradePrice].add(locate, price(typed.price));
            } else if constexpr (std::same_as<M, itch::MwcbDeclineLevel>) {
                runs_[kColumnMwcbLevel].add(locate, static_cast<std::uint64_t>(typed.level_1));
                runs_[kColumnMwcbLevel].add(locate, static_cast<std::uint64_t>(typed.level_2));
                runs_[kColumnMwcbLevel].add(locate, static_cast<std::uint64_t>(typed.level_3));
            }
        }
    }

    [[nodiscard]] static std::uint64_t price(itch::Price4 value) noexcept {
        return static_cast<std::uint32_t>(value);
    }

    std::vector<ColumnRun> runs_;
    std::uint64_t messages_ = 0;
    std::uint64_t raw_ = 0;
};

[[nodiscard]] std::string_view name_of(ValueKind kind) noexcept {
    switch (kind) {
        case ValueKind::timestamp_ns:
            return "timestamp_ns";
        case ValueKind::price4:
            return "price4";
        case ValueKind::price8:
            return "price8";
    }
    return "unknown";
}

[[nodiscard]] std::string_view name_of(StreamOrder order) noexcept {
    return order == StreamOrder::file ? "file" : "per_locate";
}

[[nodiscard]] std::string_view name_of(Representation representation) noexcept {
    switch (representation) {
        case Representation::identity:
            return "identity";
        case Representation::dollars:
            return "dollars";
        case Representation::integer_as_double:
            return "integer_as_double";
    }
    return "unknown";
}

[[nodiscard]] double bits_per_value(const CodecResult& result) noexcept {
    return result.values == 0
               ? 0.0
               : static_cast<double>(result.stored_bytes()) * 8.0 /
                     static_cast<double>(result.values);
}

// BENCHMARKS.md's wording, unchanged.
struct Target {
    std::string_view id;
    std::string_view what;
    double limit;
};

constexpr std::array kTargets{
    Target{.id = "CR-3", .what = "Timestamp column at most 20.0 bits per value", .limit = 20.0},
    Target{.id = "CR-4",
           .what = "Add-order price column at most 7.9 bits per value",
           .limit = 7.9},
};

constexpr double kCr5Ratio = 1.53;

[[nodiscard]] const Target* target_of(std::string_view id) noexcept {
    const auto* found = std::ranges::find(kTargets, id, &Target::id);
    return found == kTargets.end() ? nullptr : found;
}

[[nodiscard]] const CodecResult* result_of(const ColumnResult& column, std::string_view codec) {
    const auto found = std::ranges::find_if(
        column.codecs, [&](const CodecResult& result) { return result.entry->name == codec; });
    return found == column.codecs.end() ? nullptr : &*found;
}

void write_example(JsonWriter& json, const Example& example) {
    json.begin_object();
    json.field("index", example.index);
    json.field("locate", example.locate);
    json.field("value", example.value);
    json.field("stored_word", std::format("{:#018x}", example.stored_word));
    json.field("decoded", example.decoded);
    json.end_object();
}

void write_codec(JsonWriter& json, const CodecResult& result) {
    const CodecEntry& entry = *result.entry;
    json.begin_object();
    json.field("codec", entry.name);
    json.field("what", entry.what);
    json.field("representation", name_of(entry.representation));
    json.field("values", result.values);
    json.field("blocks", result.blocks);
    json.field("block_bytes", result.block_bytes);
    json.field("header_bytes", result.header_bytes);
    json.field("metadata_bytes", result.metadata_bytes);
    json.field("stored_bytes", result.stored_bytes());
    json.field("bits_per_value", bits_per_value(result));
    json.field("exact", result.exact());
    json.field("blocks_refused", result.blocks_refused);
    json.field("word_mismatches", result.word_mismatches);
    json.field("representation_failures", result.representation_failures);
    json.key("examples");
    json.begin_array();
    for (const Example& example : result.examples) {
        write_example(json, example);
    }
    json.end_array();
    json.key("counters");
    json.begin_object();
    for (std::size_t i = 0; i < kCodecCounters; ++i) {
        if (!entry.counter_names[i].empty()) {
            json.field(entry.counter_names[i], result.counters[i]);
        }
    }
    json.end_object();
    json.end_object();
}

// The best codec on a column against its target, and for prices how XOR on the dollar
// doubles compares with XOR on the integers.
void write_comparison(JsonWriter& json, const ColumnResult& column) {
    json.key("comparison");
    json.begin_object();
    const CodecResult* best = nullptr;
    for (const CodecResult& result : column.codecs) {
        if (result.values > 0 && result.exact() &&
            (best == nullptr || result.stored_bytes() < best->stored_bytes())) {
            best = &result;
        }
    }
    if (best != nullptr) {
        json.field("best_exact_codec", best->entry->name);
        json.field("best_bits_per_value", bits_per_value(*best));
    }
    if (const Target* target = target_of(column.spec->target);
        target != nullptr && best != nullptr) {
        json.field("target", target->id);
        json.field("target_what", target->what);
        json.field("target_bits_per_value_at_most", target->limit);
        json.field("met", bits_per_value(*best) <= target->limit);
        json.field("gap_bits_per_value", bits_per_value(*best) - target->limit);
    }
    if (column.spec->kind == ValueKind::timestamp_ns) {
        json.field("census_ideal_delta_bits", column.ideal_delta_bits);
        json.field("census_ideal_delta_of_delta_bits", column.ideal_delta_of_delta_bits);
    }
    const CodecResult* dollars = result_of(column, "gorilla-xor-f64-dollars");
    const CodecResult* integer = result_of(column, "gorilla-xor-u64-integer");
    const CodecResult* as_double = result_of(column, "gorilla-xor-f64-integer");
    if (dollars != nullptr && integer != nullptr && as_double != nullptr &&
        integer->stored_bytes() > 0 && as_double->stored_bytes() > 0) {
        json.field("xor_dollars_over_xor_u64_integer",
                   static_cast<double>(dollars->stored_bytes()) /
                       static_cast<double>(integer->stored_bytes()));
        json.field("xor_dollars_over_xor_f64_integer",
                   static_cast<double>(dollars->stored_bytes()) /
                       static_cast<double>(as_double->stored_bytes()));
        json.field("cr5_bits_per_value_an_integer_scheme_must_reach",
                   bits_per_value(*dollars) / kCr5Ratio);
    }
    json.end_object();
}

void write_column(JsonWriter& json, const ColumnResult& column) {
    json.begin_object();
    json.field("name", column.spec->name);
    json.field("source", column.spec->source);
    json.field("kind", name_of(column.spec->kind));
    json.field("order", name_of(column.spec->order));
    json.field("values", column.values);
    json.field("streams", column.streams);
    json.field("ideal_delta_bits", column.ideal_delta_bits);
    json.field("ideal_delta_of_delta_bits", column.ideal_delta_of_delta_bits);
    json.key("codecs");
    json.begin_array();
    for (const CodecResult& result : column.codecs) {
        write_codec(json, result);
    }
    json.end_array();
    write_comparison(json, column);
    json.end_object();
}

[[nodiscard]] std::string host_label() {
    utsname name{};
    if (uname(&name) != 0) {
        return "unknown";
    }
    const std::string_view system{static_cast<const char*>(name.sysname)};
    return system == "Darwin" ? "macOS" : std::string{system};
}

struct CliOptions {
    std::string_view source;
    std::string_view output;
    std::string_view day;
    std::uint32_t block_values = codec::kDefaultBlockValues;
    bool prove_all_price4 = false;
};

constexpr std::string_view kUsage =
    "usage: tick_codec_eval <source> <output.json> --day LABEL [--block-values N] "
    "[--prove-all-price4]\n";

[[nodiscard]] std::optional<CliOptions> parse_options(std::span<const std::string_view> args) {
    if (args.size() < 2) {
        return std::nullopt;
    }
    CliOptions options;
    options.source = args[0];
    options.output = args[1];
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (flag == "--prove-all-price4") {
            options.prove_all_price4 = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            return std::nullopt;
        }
        const std::string_view value = args[++i];
        if (flag == "--day") {
            options.day = value;
        } else if (flag == "--block-values") {
            std::uint32_t parsed = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() ||
                !codec::check_block_values(parsed).has_value()) {
                return std::nullopt;
            }
            options.block_values = parsed;
        } else {
            return std::nullopt;
        }
    }
    if (options.day.empty()) {
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return !file.fail();
}

}  // namespace

std::span<const CodecEntry> codec_registry() noexcept { return kRegistry; }

std::span<const ColumnSpec> column_specs() noexcept { return kColumns; }

std::uint64_t to_stored_word(Representation representation, ValueKind kind,
                             std::uint64_t value) noexcept {
    switch (representation) {
        case Representation::identity:
            return value;
        case Representation::dollars:
            return std::bit_cast<std::uint64_t>(codec::scaled_to_double(value, scale_of(kind)));
        case Representation::integer_as_double:
            return std::bit_cast<std::uint64_t>(codec::scaled_to_double(value, 1));
    }
    return value;
}

std::expected<std::uint64_t, codec::CodecError> from_stored_word(Representation representation,
                                                                 ValueKind kind,
                                                                 std::uint64_t word) noexcept {
    switch (representation) {
        case Representation::identity:
            return word;
        case Representation::dollars:
            return codec::double_to_scaled(std::bit_cast<double>(word), scale_of(kind),
                                           max_of(kind));
        case Representation::integer_as_double:
            return codec::double_to_scaled(std::bit_cast<double>(word), 1, max_of(kind));
    }
    return word;
}

bool EvalResult::exact() const noexcept {
    return std::ranges::all_of(columns, [](const ColumnResult& column) {
        return std::ranges::all_of(column.codecs, &CodecResult::exact);
    });
}

std::expected<EvalResult, std::string> evaluate_codecs(std::span<const std::byte> file,
                                                       const EvalOptions& options) {
    if (!codec::check_block_values(options.block_values).has_value()) {
        return std::unexpected(std::format("block size {} is outside 1 to {}",
                                           options.block_values, codec::kMaxBlockValues));
    }
    Walk walk{options};
    itch::FrameCursor cursor{file};
    while (true) {
        const auto frame = cursor.next();
        if (!frame.has_value()) {
            return walk.finish(itch::to_string(frame.error()));
        }
        // The cursor returns no empty payload, and only that fails to decode.
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            return std::unexpected(itch::to_string(message.error()));
        }
        walk.observe(*message);
    }
}

Price4Proof prove_price4(Representation representation) noexcept {
    Price4Proof proof;
    proof.representation = representation;
    for (std::uint64_t price = 0; price <= std::numeric_limits<std::uint32_t>::max(); ++price) {
        const std::uint64_t word = to_stored_word(representation, ValueKind::price4, price);
        const auto back = from_stored_word(representation, ValueKind::price4, word);
        ++proof.checked;
        if (!back.has_value() || *back != price) {
            if (!proof.first_failure.has_value()) {
                proof.first_failure = static_cast<std::uint32_t>(price);
            }
            ++proof.failures;
        }
    }
    return proof;
}

int run_codec_eval_cli(std::span<const std::string_view> arguments,
                       const EvalProvenance& provenance, CliOutput& output) {
    const auto options = parse_options(arguments);
    if (!options.has_value()) {
        output.err += kUsage;
        return kExitUsageOrIo;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto source = store::MappedFile::open(options->source);
    if (!source.has_value()) {
        output.err += std::format("cannot read the source: {}\n", to_string(source.error()));
        return kExitUsageOrIo;
    }
    const auto result =
        evaluate_codecs(source->bytes(), EvalOptions{.block_values = options->block_values});
    if (!result.has_value()) {
        output.err += std::format("the source did not decode: {}\n", result.error());
        return kExitUsageOrIo;
    }
    std::vector<Price4Proof> proofs;
    if (options->prove_all_price4) {
        proofs.push_back(prove_price4(Representation::dollars));
        proofs.push_back(prove_price4(Representation::integer_as_double));
    }
    const double seconds = std::chrono::duration<double>{Clock::now() - start}.count();

    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_codec_eval"});
    json.field("schema", 1);
    json.field("tool_version", kToolVersion);
    json.field("day", options->day);
    json.field("source", std::filesystem::path{options->source}.filename().string());
    json.field("host", host_label());
    json.field("commit",
               provenance.commit.substr(0, std::min<std::size_t>(12, provenance.commit.size())));
    json.field("tree_dirty", provenance.tree_dirty);
    json.field("compiler", provenance.compiler);
    json.field("build_type", provenance.build_type);
    json.field("sanitizers", provenance.sanitizers);
    json.field("block_values", options->block_values);
    json.field("messages", result->messages);
    json.field("raw_messages_skipped", result->raw_skipped);
    json.field("stop", result->stop);
    json.field("every_value_exact", result->exact());
    json.field("not_counted",
               std::string_view{"A per-locate column also needs a stored permutation or "
                                "ordinal to rebuild file order. That cost is not in its "
                                "stored bytes."});
    json.key("price4_exhaustive_proofs");
    json.begin_array();
    for (const Price4Proof& proof : proofs) {
        json.begin_object();
        json.field("representation", name_of(proof.representation));
        json.field("checked", proof.checked);
        json.field("failures", proof.failures);
        json.field("first_failure", proof.first_failure);
        json.end_object();
    }
    json.end_array();
    json.key("targets");
    json.begin_object();
    for (const Target& target : kTargets) {
        json.key(target.id);
        json.begin_object();
        json.field("what", target.what);
        json.field("bits_per_value_at_most", target.limit);
        json.end_object();
    }
    json.key("CR-5");
    json.begin_object();
    json.field("what", std::string_view{"Best integer scheme at least 1.53x smaller than "
                                        "Gorilla XOR on the same price column"});
    json.field("ratio_at_least", kCr5Ratio);
    json.field("status", std::string_view{"no integer scheme is registered yet: the "
                                          "gorilla-xor-f64-dollars figures are its baseline"});
    json.end_object();
    json.end_object();
    json.key("columns");
    json.begin_array();
    for (const ColumnResult& column : result->columns) {
        write_column(json, column);
    }
    json.end_array();
    json.field("wall_seconds", seconds);
    json.end_object();
    if (!write_text(options->output, json.finish())) {
        output.err += "cannot write the results\n";
        return kExitUsageOrIo;
    }

    for (const ColumnResult& column : result->columns) {
        for (const CodecResult& codec_result : column.codecs) {
            output.out += std::format("{:<22} {:<24} {:>12} values {:>8.3f} bits/value {}\n",
                                      column.spec->name, codec_result.entry->name,
                                      codec_result.values, bits_per_value(codec_result),
                                      codec_result.exact() ? "exact" : "NOT EXACT");
        }
    }
    for (const Price4Proof& proof : proofs) {
        output.out += std::format("all 2^32 Price(4) through {}: {} failures\n",
                                  name_of(proof.representation), proof.failures);
    }
    const bool proofs_hold = std::ranges::all_of(
        proofs, [](const Price4Proof& proof) { return proof.failures == 0; });
    return result->exact() && proofs_hold ? kExitPass : kExitMismatch;
}

}  // namespace tick_store::harness
