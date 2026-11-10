// Real-data column decodes, the decode targets of BENCHMARKS.md that are not a kernel on
// L1-resident seeded input: two columns of the pinned slice, each decoded the way a target
// defines, one case per process. Case names say the codec and the column.
//
//   gorilla_xor/add_price       DS-1, and DS-6's Gorilla side for prices: the store's
//                               add-order price column as dollar doubles, price / 10^4, one
//                               sequential Gorilla XOR stream in blocks of 4,096
//   gorilla_xor_l1/add_price    DS-2: the same coding, 1,024-value vectors of that column,
//                               few enough to stay in a 32 KiB L1, decoded in turn
//   store_decode/add_price      DS-5, and DS-6's layout side for prices: the store's own
//                               chunks of that column, decoded as the reader does
//   gorilla_dod/timestamp       DS-6 and DS-7's Gorilla side, DS-9: every message's
//                               timestamp in file order, Gorilla delta-of-delta
//   pfor_delta/timestamp        DS-6 and DS-7's layout side: the same values, PFOR delta
//   pfor_dod/timestamp          DS-9 on the vector layout: PFOR delta-of-delta
//
// The price column is the store's: add-order prices grouped by (locate, side), as each
// segment of kDefaultSegmentRecords records stores them, the segments one after another.
// So every price case decodes the same values in the same order. The timestamp column is
// every typed message's timestamp in file order; the store keeps residuals against a time
// base instead, which no Gorilla stream would code, so both timestamp codecs get the
// timestamps themselves.
//
// run_bench.py drives one process per case, --case=NAME --report=PATH, input kind
// "column": the pinned slice, counted in values.
//
// How the bar in research/evidence/code-quality.md section 16 is met:
//
//   B1  The input is the pinned file, and nothing is random.
//   B2  The slice must match its pin and re-encode to itself, and each case's decode must
//       give back its column exactly, bit for bit, before anything is timed; the timed
//       passes' output is checked again after them.
//   B3  DoNotOptimize on the output after every call and ClobberMemory after every pass.
//   B4  Values per second, the column's encoded size, and counters from `perf stat`
//       around the same passes on Linux, marked unavailable on macOS.
//   B5  The codecs' public decoders, and for the store the reader's own decode_column on
//       the chunks encode_column writes, piece by piece into one vector as the reader does.
//   B6  Every whole-column pass is millions of real values. The L1 case is not, by its
//       definition, so it decodes several real vectors in turn rather than one, and its
//       branch misses per value are in the results file beside it.

#include "tick_store/bench_support/affinity.hpp"
#include "tick_store/bench_support/build_info.hpp"
#include "tick_store/bench_support/counter_gate.hpp"
#include "tick_store/bench_support/latency_recorder.hpp"
#include "tick_store/bench_support/resolution_guard.hpp"
#include "tick_store/bench_support/stream_check.hpp"
#include "tick_store/bench_support/timer.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/codec/simd.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/itch_segment.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace bs = tick_store::bench_support;
namespace codec = tick_store::codec;
namespace harness = tick_store::harness;
namespace itch = tick_store::itch;
namespace store = tick_store::store;

constexpr int kRepetitions = 10;
constexpr int kWarmupPasses = 2;
constexpr std::chrono::milliseconds kCalibrationWindow{200};
constexpr std::uint64_t kHighestBatchTicks = 1ULL << 40U;
// The codecs' block, the store's own.
constexpr std::size_t kBlockValues = store::kColumnBlockValues;
// The L1 case: ALP's vector, and at most this many encoded bytes of them in all, so that
// they and the 8 KiB output vector fit in Zen 1's 32 KiB L1 data cache with room to spare.
constexpr std::size_t kVectorValues = 1024;
constexpr std::size_t kL1EncodedBudget = std::size_t{16} * 1024U;
constexpr std::size_t kL1MostVectors = 16;
constexpr std::size_t kL1PassCalls = 8192;
constexpr std::size_t kL1BatchCalls = 16;

// The two columns, from the slice.
struct Columns {
    std::vector<std::uint64_t> add_price;
    std::vector<std::uint64_t> timestamp;
};

// One decode on one column: what it runs, how many values a call yields, and whether
// the last call's output is the column.
struct Case {
    std::string name;
    std::string column;
    std::string encoding;
    std::uint64_t values_per_call = 0;
    std::size_t calls_per_pass = 1;
    std::size_t batch_calls = 1;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t encoded_values = 0;
    // False if a decoder refused its input.
    std::function<bool(std::size_t calls)> run;
    std::function<bool()> verify;
};

using CaseMaker = std::expected<Case, std::string> (*)(const Columns&);

std::vector<double> dollars_of(std::span<const std::uint64_t> prices) {
    std::vector<double> dollars;
    dollars.reserve(prices.size());
    for (const std::uint64_t price : prices) {
        dollars.push_back(codec::price4_to_dollars(static_cast<std::uint32_t>(price)));
    }
    return dollars;
}

bool same_words(std::span<const std::uint64_t> a, std::span<const std::uint64_t> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

// Bit for bit, so a NaN or a -0.0 could not pass for its neighbour.
bool same_doubles(std::span<const double> a, std::span<const double> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

std::expected<Case, std::string> gorilla_xor_case(const Columns& columns) {
    const auto dollars = std::make_shared<std::vector<double>>(dollars_of(columns.add_price));
    const auto encoded = std::make_shared<std::vector<std::byte>>();
    if (const auto done = codec::encode_xor_doubles(*dollars, kBlockValues, *encoded); !done) {
        return std::unexpected(std::string{"encode_xor_doubles refused the price column"});
    }
    const auto out = std::make_shared<std::vector<double>>();
    out->reserve(dollars->size());
    Case c;
    c.name = "gorilla_xor/add_price";
    c.column = "add_price";
    c.encoding = "Gorilla XOR on price / 10^4 as a double, blocks of 4,096, one stream";
    c.values_per_call = dollars->size();
    c.encoded_bytes = encoded->size();
    c.encoded_values = dollars->size();
    c.run = [encoded, out, count = dollars->size()](std::size_t calls) {
        bool ok = true;
        for (std::size_t i = 0; i < calls; ++i) {
            out->clear();
            ok = codec::decode_xor_doubles(*encoded, count, *out).has_value() && ok;
            benchmark::DoNotOptimize(out->data());
        }
        return ok;
    };
    c.verify = [dollars, out] { return same_doubles(*out, *dollars); };
    return c;
}

std::expected<Case, std::string> gorilla_xor_l1_case(const Columns& columns) {
    const std::vector<double> dollars = dollars_of(columns.add_price);
    const std::size_t whole = dollars.size() / kVectorValues;
    if (whole == 0) {
        return std::unexpected(std::string{"the price column is shorter than one vector"});
    }
    // Vectors evenly spaced through the column: the most, up to kL1MostVectors, whose
    // encoded bytes fit the budget together.
    struct Vectors {
        std::vector<std::vector<std::byte>> encoded;
        std::vector<std::vector<double>> values;
    };
    const auto vectors = std::make_shared<Vectors>();
    std::uint64_t total = 0;
    for (std::size_t count = std::min(kL1MostVectors, whole); count > 0; --count) {
        vectors->encoded.clear();
        vectors->values.clear();
        total = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const std::size_t at = (k * whole / count) * kVectorValues;
            const std::span<const double> values{dollars.data() + at, kVectorValues};
            std::vector<std::uint64_t> words(kVectorValues);
            std::ranges::transform(values, words.begin(), [](double value) {
                return std::bit_cast<std::uint64_t>(value);
            });
            std::vector<std::byte> block;
            if (const auto done = codec::encode_xor_block(words, block); !done) {
                return std::unexpected(std::string{"encode_xor_block refused a price vector"});
            }
            total += block.size();
            block.shrink_to_fit();
            vectors->encoded.push_back(std::move(block));
            vectors->values.emplace_back(values.begin(), values.end());
        }
        if (total <= kL1EncodedBudget) {
            break;
        }
    }
    if (total > kL1EncodedBudget) {
        vectors->encoded.clear();
    }
    if (vectors->encoded.empty()) {
        return std::unexpected(std::string{"no price vector fits the L1 budget"});
    }
    const auto out = std::make_shared<std::vector<std::uint64_t>>();
    out->reserve(kVectorValues);
    const auto last = std::make_shared<std::size_t>(0);
    Case c;
    c.name = "gorilla_xor_l1/add_price";
    c.column = "add_price";
    c.encoding = std::format(
        "Gorilla XOR on price / 10^4 as a double, {} vectors of 1,024 evenly spaced through the "
        "column, one block each, {} encoded bytes in all, decoded in turn",
        vectors->encoded.size(), total);
    c.values_per_call = kVectorValues;
    c.calls_per_pass = kL1PassCalls;
    c.batch_calls = kL1BatchCalls;
    c.encoded_bytes = total;
    c.encoded_values = vectors->encoded.size() * kVectorValues;
    c.run = [vectors, out, last](std::size_t calls) {
        bool ok = true;
        const std::size_t n = vectors->encoded.size();
        for (std::size_t i = 0; i < calls; ++i) {
            const std::size_t k = i % n;
            out->clear();
            ok = codec::decode_xor_block(vectors->encoded[k], *out).has_value() && ok;
            benchmark::DoNotOptimize(out->data());
            *last = k;
        }
        return ok;
    };
    c.verify = [vectors, out, last] {
        const std::vector<double>& want = vectors->values[*last];
        return out->size() == want.size() &&
               std::memcmp(out->data(), want.data(), want.size() * sizeof(double)) == 0;
    };
    return c;
}

// The store's chunks of one segment's column, and how many values each holds.
struct StoredColumn {
    std::vector<std::vector<store::ColumnChunk>> segments;
};

std::expected<Case, std::string> store_decode_case(const Columns& columns,
                                                   std::span<const std::uint64_t> segment_sizes) {
    const auto stored = std::make_shared<StoredColumn>();
    std::uint64_t bytes = 0;
    std::size_t at = 0;
    for (const std::uint64_t size : segment_sizes) {
        if (size == 0) {
            continue;
        }
        const std::span<const std::uint64_t> values{columns.add_price.data() + at,
                                                    static_cast<std::size_t>(size)};
        at += static_cast<std::size_t>(size);
        auto pieces = store::encode_column(store::Column::add_price, values);
        if (!pieces.has_value()) {
            return std::unexpected(std::format("encode_column refused add_price: {}",
                                               store::to_string(pieces.error())));
        }
        for (const store::ColumnChunk& piece : *pieces) {
            bytes += piece.bytes.size();
        }
        stored->segments.push_back(std::move(*pieces));
    }
    const auto want = std::make_shared<std::vector<std::uint64_t>>(columns.add_price);
    const auto out = std::make_shared<std::vector<std::uint64_t>>();
    out->reserve(want->size());
    Case c;
    c.name = "store_decode/add_price";
    c.column = "add_price";
    c.encoding = "the store's add_price chunks from encode_column, PFOR delta with an "
                 "adaptive scale, zstd where smaller, decoded by decode_column piece by piece";
    c.values_per_call = want->size();
    c.encoded_bytes = bytes;
    c.encoded_values = want->size();
    c.run = [stored, out](std::size_t calls) {
        bool ok = true;
        for (std::size_t i = 0; i < calls; ++i) {
            out->clear();
            for (const auto& pieces : stored->segments) {
                for (const store::ColumnChunk& piece : pieces) {
                    auto values = store::decode_column(store::Column::add_price, piece.codec,
                                                       piece.bytes, piece.values);
                    if (!values.has_value()) [[unlikely]] {
                        ok = false;
                        continue;
                    }
                    out->insert(out->end(), values->begin(), values->end());
                }
            }
            benchmark::DoNotOptimize(out->data());
        }
        return ok;
    };
    c.verify = [want, out] { return same_words(*out, *want); };
    return c;
}

enum class TimeCodec : std::uint8_t { gorilla, pfor_delta, pfor_dod };

std::expected<Case, std::string> timestamp_case(const Columns& columns, TimeCodec kind) {
    const auto want = std::make_shared<std::vector<std::uint64_t>>(columns.timestamp);
    const auto encoded = std::make_shared<std::vector<std::byte>>();
    Case c;
    c.column = "timestamp";
    switch (kind) {
        case TimeCodec::gorilla:
            if (const auto done = codec::encode_timestamps(*want, kBlockValues, *encoded); !done) {
                return std::unexpected(std::string{"encode_timestamps refused the timestamps"});
            }
            c.name = "gorilla_dod/timestamp";
            c.encoding = "Gorilla delta-of-delta, buckets 16, 19 and 23 bits, blocks of 4,096";
            break;
        case TimeCodec::pfor_delta:
        case TimeCodec::pfor_dod: {
            const bool dod = kind == TimeCodec::pfor_dod;
            const codec::PforOptions options{
                .transform = dod ? codec::IntTransform::delta_of_delta : codec::IntTransform::delta,
                .adaptive_scale = false,
                .patch = true};
            if (const auto done = codec::encode_pfor(*want, kBlockValues, options, *encoded); !done) {
                return std::unexpected(std::string{"encode_pfor refused the timestamps"});
            }
            c.name = dod ? "pfor_dod/timestamp" : "pfor_delta/timestamp";
            c.encoding = dod ? "PFOR delta-of-delta, patched, no scale, blocks of 4,096"
                             : "PFOR delta, patched, no scale, blocks of 4,096, the store's "
                               "pfor_delta options";
            break;
        }
    }
    const auto out = std::make_shared<std::vector<std::uint64_t>>();
    out->reserve(want->size());
    c.values_per_call = want->size();
    c.encoded_bytes = encoded->size();
    c.encoded_values = want->size();
    const bool gorilla = kind == TimeCodec::gorilla;
    c.run = [encoded, out, gorilla, count = want->size()](std::size_t calls) {
        bool ok = true;
        for (std::size_t i = 0; i < calls; ++i) {
            out->clear();
            ok = (gorilla ? codec::decode_timestamps(*encoded, count, *out).has_value()
                          : codec::decode_pfor(*encoded, count, *out).has_value()) &&
                 ok;
            benchmark::DoNotOptimize(out->data());
        }
        return ok;
    };
    c.verify = [want, out] { return same_words(*out, *want); };
    return c;
}

constexpr std::array<std::string_view, 6> kCaseNames{
    "gorilla_xor/add_price", "gorilla_xor_l1/add_price", "store_decode/add_price",
    "gorilla_dod/timestamp", "pfor_delta/timestamp",     "pfor_dod/timestamp"};

// The slice's two columns: add-order prices as the store's segments keep them, and every
// typed message's timestamp in file order. Also the add_price count of each segment.
struct SliceColumns {
    Columns columns;
    std::vector<std::uint64_t> segment_prices;
};

std::expected<SliceColumns, std::string> columns_of(std::span<const std::byte> source) {
    SliceColumns out;
    store::SymbolTable symbols;
    store::SegmentBuilder builder;
    std::uint64_t records = 0;
    const auto finish = [&] {
        store::SegmentSummary summary;
        store::SegmentColumns segment = builder.finish(records - builder.records(), summary);
        const std::vector<std::uint64_t>& prices = segment[store::Column::add_price];
        out.columns.add_price.insert(out.columns.add_price.end(), prices.begin(), prices.end());
        out.segment_prices.push_back(prices.size());
    };
    itch::FrameCursor cursor{source};
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            return std::unexpected(std::format("message {} does not decode", records));
        }
        std::visit(
            [&](const auto& m) {
                if constexpr (requires { m.header.timestamp_ns; }) {
                    out.columns.timestamp.push_back(m.header.timestamp_ns);
                }
            },
            *message);
        if (auto added = builder.add(*message, symbols); !added.has_value()) {
            return std::unexpected(std::format("message {}: {}", records,
                                               store::to_string(added.error())));
        }
        ++records;
        if (builder.records() == store::kDefaultSegmentRecords) {
            finish();
        }
    }
    if (builder.records() > 0) {
        finish();
    }
    return out;
}

std::expected<Case, std::string> make_case(std::string_view name, const SliceColumns& slice) {
    if (name == kCaseNames[0]) {
        return gorilla_xor_case(slice.columns);
    }
    if (name == kCaseNames[1]) {
        return gorilla_xor_l1_case(slice.columns);
    }
    if (name == kCaseNames[2]) {
        return store_decode_case(slice.columns, slice.segment_prices);
    }
    if (name == kCaseNames[3]) {
        return timestamp_case(slice.columns, TimeCodec::gorilla);
    }
    if (name == kCaseNames[4]) {
        return timestamp_case(slice.columns, TimeCodec::pfor_delta);
    }
    if (name == kCaseNames[5]) {
        return timestamp_case(slice.columns, TimeCodec::pfor_dod);
    }
    return std::unexpected(std::format("no case {}: --list-cases names them", name));
}

// --- Running a case ------------------------------------------------------------------------

struct Session {
    Case* c = nullptr;
    bs::CounterGate* gate = nullptr;
    bs::BatchLatencyRecorder* recorder = nullptr;
    std::uint64_t gated_items = 0;
    std::uint64_t gated_ticks = 0;
    std::vector<std::uint64_t> warmup_pass_ticks;
};

void warm_up(Session& session) {
    const bool record = session.warmup_pass_ticks.empty();
    for (int pass = 0; pass < kWarmupPasses; ++pass) {
        const std::uint64_t start = bs::read_ticks();
        benchmark::DoNotOptimize(session.c->run(session.c->calls_per_pass));
        const std::uint64_t stop = bs::read_ticks();
        if (record) {
            session.warmup_pass_ticks.push_back(stop - start);
        }
    }
}

void throughput(benchmark::State& state, Session& session) {
    warm_up(session);
    if (const auto opened = session.gate->enable(); !opened.has_value()) {
        state.SkipWithError(opened.error());
        return;
    }
    bool ok = true;
    const std::uint64_t start = bs::read_ticks();
    for (const auto iteration : state) {
        static_cast<void>(iteration);
        ok = session.c->run(session.c->calls_per_pass) && ok;
        benchmark::ClobberMemory();
    }
    const std::uint64_t stop = bs::read_ticks();
    if (const auto closed = session.gate->disable(); !closed.has_value()) {
        state.SkipWithError(closed.error());
        return;
    }
    if (!ok || !session.c->verify()) {
        state.SkipWithError("a timed pass did not give back its column");
        return;
    }
    const std::uint64_t items = session.c->calls_per_pass * session.c->values_per_call;
    session.gated_items += items * static_cast<std::uint64_t>(state.iterations());
    session.gated_ticks += stop - start;
    state.SetItemsProcessed(static_cast<std::int64_t>(items) * state.iterations());
    state.SetBytesProcessed(static_cast<std::int64_t>(items * sizeof(std::uint64_t)) *
                            state.iterations());
}

void batch_latency(benchmark::State& state, Session& session) {
    warm_up(session);
    bool ok = true;
    const std::size_t batch = session.c->batch_calls;
    for (const auto iteration : state) {
        static_cast<void>(iteration);
        for (std::size_t done = 0; done < session.c->calls_per_pass; done += batch) {
            const std::uint64_t start = bs::read_ticks();
            ok = session.c->run(batch) && ok;
            const std::uint64_t stop = bs::read_ticks();
            session.recorder->record(stop - start);
        }
        benchmark::ClobberMemory();
    }
    if (!ok || !session.c->verify()) {
        state.SkipWithError("a timed pass did not give back its column");
    }
}

class FailureWatch final : public benchmark::ConsoleReporter {
public:
    void ReportRuns(const std::vector<Run>& runs) override {
        failed_ = failed_ || std::ranges::any_of(runs, [](const Run& run) {
                      return run.skipped != benchmark::internal::NotSkipped;
                  });
        ConsoleReporter::ReportRuns(runs);
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    bool failed_ = false;
};

struct Options {
    std::filesystem::path slice;
    std::string expected_sha256;
    std::uint64_t expected_messages = 0;
    std::string workload;
    std::filesystem::path report;
    std::optional<int> core;
    std::optional<std::filesystem::path> perf_control;
    std::optional<std::filesystem::path> perf_acknowledge;
    bool list_cases = false;
    bool input_kind = false;
    bool verify_all = false;
};

template <class Number>
[[nodiscard]] bool parse_number(std::string_view text, Number& number) noexcept {
    const auto* const end = text.data() + text.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): from_chars takes the end too
    const auto [next, error] = std::from_chars(text.data(), end, number);
    return error == std::errc{} && next == end;
}

constexpr std::string_view kUsage =
    "usage: column_decode_bench --slice=PATH --expect-sha256=HEX --expect-messages=N "
    "--case=NAME --report=PATH [--core=N] [--perf-control=FIFO --perf-ack=FIFO] "
    "[benchmark flags]\n"
    "       column_decode_bench --slice=PATH --expect-sha256=HEX --expect-messages=N "
    "--verify-all\n"
    "       column_decode_bench --list-cases | --input-kind";

[[nodiscard]] std::expected<Options, std::string> parse_options(std::span<char*> arguments) {
    Options options;
    for (const std::string_view argument : arguments.subspan(1)) {
        const auto value_of = [&](std::string_view flag) -> std::optional<std::string_view> {
            if (argument.starts_with(flag)) {
                return argument.substr(flag.size());
            }
            return std::nullopt;
        };
        if (argument == "--list-cases") {
            options.list_cases = true;
        } else if (argument == "--input-kind") {
            options.input_kind = true;
        } else if (argument == "--verify-all") {
            options.verify_all = true;
        } else if (const auto slice = value_of("--slice=")) {
            options.slice = *slice;
        } else if (const auto sha = value_of("--expect-sha256=")) {
            options.expected_sha256 = *sha;
        } else if (const auto messages = value_of("--expect-messages=")) {
            if (!parse_number(*messages, options.expected_messages)) {
                return std::unexpected(std::format("bad message count: {}", *messages));
            }
        } else if (const auto workload = value_of("--case=")) {
            options.workload = *workload;
        } else if (const auto report = value_of("--report=")) {
            options.report = *report;
        } else if (const auto core = value_of("--core=")) {
            int number = 0;
            if (!parse_number(*core, number)) {
                return std::unexpected(std::format("bad core: {}", *core));
            }
            options.core = number;
        } else if (const auto control = value_of("--perf-control=")) {
            options.perf_control = *control;
        } else if (const auto acknowledge = value_of("--perf-ack=")) {
            options.perf_acknowledge = *acknowledge;
        } else {
            return std::unexpected(std::format("unknown argument: {}\n{}", argument, kUsage));
        }
    }
    if (options.list_cases || options.input_kind) {
        return options;
    }
    if (options.slice.empty() || options.expected_sha256.empty() ||
        options.expected_messages == 0 ||
        (!options.verify_all && (options.workload.empty() || options.report.empty()))) {
        return std::unexpected(std::string{kUsage});
    }
    return options;
}

void write_timer(harness::JsonWriter& json, std::string_view name, const bs::TimerInfo& timer) {
    const bs::ResolutionGuard guard{timer.resolution_ns, timer.read_cost_ns};
    json.key(name);
    json.begin_object();
    json.field("source", timer.source);
    json.field("calibrated_against", std::string_view{"CLOCK_MONOTONIC_RAW"});
    json.field("ticks_per_second", timer.ticks_per_second);
    json.field("resolution_ticks", timer.resolution_ticks);
    json.field("resolution_ns", timer.resolution_ns);
    json.field("read_cost_ns", timer.read_cost_ns);
    json.field("min_publishable_ns", guard.floor_ns());
    json.field("calibration_window_ns", timer.window_ns);
    json.end_object();
}

struct ReportInputs {
    const bs::PinResult& pin;
    const bs::CounterGate& gate;
    const bs::StreamCheck& check;
    const bs::TimerInfo& timer_start;
    const bs::TimerInfo& timer_end;
    const bs::LatencyReport& latency;
    const Session& session;
};

[[nodiscard]] std::string render_report(const ReportInputs& in) {
    const bs::BuildInfo& build = bs::build_info();
    const Case& c = *in.session.c;
    harness::JsonWriter json;
    json.begin_object();
    json.field("case", std::string_view{c.name});

    json.key("build");
    json.begin_object();
    json.field("compiler", std::format("{} {}", build.compiler_id, build.compiler_version));
    json.field("compile_flags", build.compile_flags);
    json.field("build_type", build.build_type);
    json.field("sanitizers", build.sanitizers);
    json.field("commit", build.commit);
    json.field("tree_dirty", build.tree_dirty);
    // The decode kernel set this process dispatched to, after any TICK_STORE_SIMD cap.
    json.field("isa", std::format("{}, {} kernels", build.isa,
                                  codec::simd::to_string(codec::simd::active_kernels().level)));
    json.end_object();

    json.key("pinning");
    json.begin_object();
    json.field("status", bs::to_string(in.pin.status));
    json.field("requested_core", in.pin.requested_cpu);
    json.field("observed_core", in.pin.observed_cpu);
    json.end_object();

    json.field("counters", bs::to_string(in.gate.status()));
    write_timer(json, "timer", in.timer_start);
    write_timer(json, "timer_end", in.timer_end);

    json.key("verification");
    json.begin_object();
    json.field("messages", in.check.messages);
    json.field("bytes", in.check.bytes);
    json.field("input_sha256", std::string_view{in.check.input_sha256});
    json.field("reencoded_sha256", std::string_view{in.check.reencoded_sha256});
    json.field("column", std::string_view{c.column});
    json.field("encoding", std::string_view{c.encoding});
    json.field("values", c.encoded_values);
    json.field("encoded_bytes", c.encoded_bytes);
    json.field("items_per_call", c.values_per_call);
    json.field("calls_per_pass", static_cast<std::uint64_t>(c.calls_per_pass));
    json.field("against", std::string_view{"the column's own values, bit for bit, before timing "
                                           "and after the timed passes"});
    json.field("matched", true);
    json.end_object();

    json.key("warmup");
    json.begin_object();
    json.field("passes_per_repetition", kWarmupPasses);
    json.key("first_repetition_pass_ms");
    json.begin_array();
    for (const std::uint64_t ticks : in.session.warmup_pass_ticks) {
        json.value(in.timer_start.to_ns(ticks) / 1e6);
    }
    json.end_array();
    json.end_object();

    json.key("gated");
    json.begin_object();
    json.field("items", in.session.gated_items);
    json.field("wall_ns", in.timer_start.to_ns(in.session.gated_ticks));
    json.end_object();

    json.key("latency");
    json.begin_object();
    json.field("unit", in.latency.unit);
    json.field("batch_items", static_cast<std::uint64_t>(c.batch_calls * c.values_per_call));
    json.field("batches", in.latency.batches);
    json.field("timer_resolution_ns", in.latency.timer_resolution_ns);
    json.field("min_publishable_ns", in.latency.floor_ns);
    json.field("significant_figures", in.latency.significant_figures);
    json.field("min_ns", in.latency.min_ns);
    json.field("max_ns", in.latency.max_ns);
    json.key("percentiles");
    json.begin_array();
    for (const bs::PublishedPercentile& point : in.latency.percentiles) {
        json.begin_object();
        json.field("percentile", point.percentile);
        json.field("value_ns", point.value_ns);
        json.end_object();
    }
    json.end_array();
    json.end_object();

    json.end_object();
    return json.finish();
}

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;

int fail(std::string_view message) {
    std::println(stderr, "column_decode_bench: {}", message);
    return kExitFailed;
}

// B2 before anything is timed: one call, then the column's own values.
[[nodiscard]] std::expected<void, std::string> verify_before(Case& c) {
    if (!c.run(1) || !c.verify()) {
        return std::unexpected(std::format("{} does not give back its column", c.name));
    }
    return {};
}

int run_case(const Options& options, Case& c, const bs::PinResult& pin, bs::CounterGate& gate,
             const bs::TimerInfo& timer_start, const bs::StreamCheck& check) {
    auto recorder = bs::BatchLatencyRecorder::create(kHighestBatchTicks);
    if (!recorder.has_value()) {
        return fail(recorder.error());
    }
    Session session{.c = &c, .gate = &gate, .recorder = &*recorder, .gated_items = 0,
                    .gated_ticks = 0, .warmup_pass_ticks = {}};
    const auto configure = [](benchmark::Benchmark* benchmark) {
        benchmark->Iterations(1)->Repetitions(kRepetitions)->UseRealTime()->Unit(
            benchmark::kMillisecond);
    };
    configure(benchmark::RegisterBenchmark(
        c.name + "/throughput", [&session](benchmark::State& state) { throughput(state, session); }));
    configure(benchmark::RegisterBenchmark(
        c.name + "/latency", [&session](benchmark::State& state) { batch_latency(state, session); }));
    FailureWatch reporter;
    benchmark::RunSpecifiedBenchmarks(&reporter);
    benchmark::Shutdown();
    if (reporter.failed()) {
        return fail("a benchmark failed, so this run has no result");
    }
    const auto timer_end = bs::calibrate_timer(kCalibrationWindow);
    if (!timer_end.has_value()) {
        return fail(timer_end.error());
    }
    const auto latency = recorder->report(timer_start, c.batch_calls * c.values_per_call);
    if (!latency.has_value()) {
        return fail(latency.error());
    }
    const std::string report = render_report({.pin = pin,
                                              .gate = gate,
                                              .check = check,
                                              .timer_start = timer_start,
                                              .timer_end = *timer_end,
                                              .latency = *latency,
                                              .session = session});
    std::ofstream file{options.report, std::ios::binary};
    file << report;
    file.close();
    if (!file) {
        return fail(std::format("cannot write {}", options.report.string()));
    }
    return kExitOk;
}

int run(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    const auto options = parse_options(std::span{argv, static_cast<std::size_t>(argc)});
    if (!options.has_value()) {
        return fail(options.error());
    }
    if (options->list_cases) {
        for (const std::string_view name : kCaseNames) {
            std::println("{}", name);
        }
        return kExitOk;
    }
    // run_bench.py asks what the binary reads: columns of the pinned slice, in values.
    if (options->input_kind) {
        std::println("column");
        return kExitOk;
    }
    if (!options->verify_all && std::ranges::find(kCaseNames, options->workload) == kCaseNames.end()) {
        return fail(std::format("no case {}: --list-cases names them", options->workload));
    }

    const bs::PinResult pin = options->core.has_value() ? bs::pin_current_thread(*options->core)
                                                        : bs::leave_unpinned();
    if (pin.status == bs::PinStatus::failed) {
        return fail(pin.detail);
    }
    auto gate = bs::CounterGate::for_run(options->perf_control, options->perf_acknowledge);
    if (!gate.has_value()) {
        return fail(gate.error());
    }
    const auto timer_start = bs::calibrate_timer(kCalibrationWindow);
    if (!timer_start.has_value()) {
        return fail(timer_start.error());
    }

    auto mapped = store::MappedFile::open(options->slice);
    if (!mapped.has_value()) {
        return fail(std::format("cannot open the slice: {}", to_string(mapped.error())));
    }
    const std::span<const std::byte> source = mapped->bytes();
    std::vector<std::byte> scratch(source.size());
    const auto check = bs::verify_round_trip(source, options->expected_sha256,
                                             options->expected_messages, scratch);
    if (!check.has_value()) {
        return fail(check.error());
    }
    scratch = {};
    const auto slice = columns_of(source);
    if (!slice.has_value()) {
        return fail(slice.error());
    }

    if (options->verify_all) {
        for (const std::string_view name : kCaseNames) {
            auto c = make_case(name, *slice);
            if (!c.has_value()) {
                return fail(c.error());
            }
            if (auto checked = verify_before(*c); !checked.has_value()) {
                return fail(checked.error());
            }
            std::println("{}: {} values, {} encoded bytes, given back exactly", c->name,
                         c->encoded_values, c->encoded_bytes);
        }
        return kExitOk;
    }
    auto c = make_case(options->workload, *slice);
    if (!c.has_value()) {
        return fail(c.error());
    }
    if (auto checked = verify_before(*c); !checked.has_value()) {
        return fail(checked.error());
    }
    return run_case(*options, *c, pin, *gate, *timer_start, *check);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        (void)std::fputs(error.what(), stderr);
        return kExitFailed;
    }
}
