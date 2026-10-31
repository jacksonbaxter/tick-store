// ITCH decode, and decode then encode, over the pinned slice of real data.
//
// One process runs one workload, so `perf stat` around it attributes its counters to
// that workload alone. run_bench.py drives one process per workload, and turns the
// output of each into a results file. Run by hand, this is for iteration only.
//
// How the bar in research/evidence/code-quality.md section 16 is met:
//
//   B1  The input is the pinned file. The one random choice, where batches start, is
//       drawn from a fixed seed owned by each workload, so adding a workload changes
//       nothing about another's data.
//   B2  verify_round_trip() must pass before a benchmark is registered, and each timed
//       pass is checked afterwards, outside its timing: the message count always, and
//       for decode then encode, its output against the source.
//   B3  DoNotOptimize on every decoded message, and ClobberMemory after every pass.
//   B4  Throughput here, counters from `perf stat` around the same loop. Where a counter
//       is unavailable, the results file says so.
//   B5  FrameCursor and the public decode and encode_frame over a read-only mapping, the
//       same path the store's ingest and read paths take.
//   B6  Every pass is the whole real stream in file order, 6.1 M messages of mixed types,
//       far past what a predictor can memorise. Spread across repetitions is reported.

#include "tick_store/bench_support/affinity.hpp"
#include "tick_store/bench_support/build_info.hpp"
#include "tick_store/bench_support/counter_gate.hpp"
#include "tick_store/bench_support/latency_recorder.hpp"
#include "tick_store/bench_support/resolution_guard.hpp"
#include "tick_store/bench_support/stream_check.hpp"
#include "tick_store/bench_support/timer.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/mapped_file.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <optional>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace bs = tick_store::bench_support;
namespace harness = tick_store::harness;
namespace itch = tick_store::itch;
namespace store = tick_store::store;

// Messages per latency sample. At 10 to 60 ns a message that is 10 to 60 us, far above
// either host's floor, and the guard refuses the run if it ever is not.
constexpr std::uint64_t kBatchMessages = 1024;
constexpr int kRepetitions = 10;
constexpr int kWarmupPasses = 2;
constexpr std::chrono::milliseconds kCalibrationWindow{200};
// About nine minutes at 2 GHz: longer than any batch, and small enough for a histogram
// of a few hundred kilobytes.
constexpr std::uint64_t kHighestBatchTicks = 1ULL << 40U;

// Decodes one payload. The result goes nowhere but the sink.
struct DecodeOnly {
    static constexpr std::string_view kName = "itch_decode";
    static constexpr std::uint64_t kBatchPhaseSeed = 0x1D0C0DE'0000'0001ULL;

    void begin_pass() noexcept {}
    [[nodiscard]] bool operator()(std::span<const std::byte> payload) noexcept {
        auto message = itch::decode(payload);
        benchmark::DoNotOptimize(message);
        return message.has_value();
    }
    [[nodiscard]] std::expected<void, std::string> check_output(
        std::span<const std::byte> /*source*/) const {
        return {};
    }
};

// Decodes one payload and writes it back as a frame, rebuilding the stream in `output`.
struct DecodeEncode {
    static constexpr std::string_view kName = "itch_decode_encode";
    static constexpr std::uint64_t kBatchPhaseSeed = 0x1D0C0DE'0000'0002ULL;

    std::span<std::byte> output;
    std::size_t written = 0;

    void begin_pass() noexcept { written = 0; }
    [[nodiscard]] bool operator()(std::span<const std::byte> payload) noexcept {
        auto message = itch::decode(payload);
        if (!message.has_value()) [[unlikely]] {
            return false;
        }
        benchmark::DoNotOptimize(message);
        const auto size = itch::encode_frame(*message, output.subspan(written));
        if (!size.has_value()) [[unlikely]] {
            return false;
        }
        written += *size;
        return true;
    }
    [[nodiscard]] std::expected<void, std::string> check_output(
        std::span<const std::byte> source) const {
        if (written != source.size() ||
            std::memcmp(output.data(), source.data(), source.size()) != 0) {
            return std::unexpected(std::string{"a timed pass did not rebuild the source"});
        }
        return {};
    }
};

// One whole stream, returning how many messages succeeded.
template <class Step>
[[nodiscard]] std::uint64_t run_pass(std::span<const std::byte> source, Step& step) noexcept {
    itch::FrameCursor cursor{source};
    std::uint64_t succeeded = 0;
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        succeeded += step(frame->payload) ? 1U : 0U;
    }
    return succeeded;
}

// The same stream, timing each run of kBatchMessages. The first `phase` messages run
// untimed so that batch boundaries differ between passes, and a short batch at the end
// is not recorded. Recording falls between the two reads of the next batch.
template <class Step>
[[nodiscard]] std::uint64_t run_timed_pass(std::span<const std::byte> source, Step& step,
                                           std::uint64_t phase,
                                           bs::BatchLatencyRecorder& recorder) noexcept {
    itch::FrameCursor cursor{source};
    std::uint64_t succeeded = 0;
    for (std::uint64_t i = 0; i < phase; ++i) {
        const auto frame = cursor.next();
        if (!frame.has_value()) {
            return succeeded;
        }
        succeeded += step(frame->payload) ? 1U : 0U;
    }
    while (true) {
        std::uint64_t in_batch = 0;
        const std::uint64_t start = bs::read_ticks();
        for (; in_batch < kBatchMessages; ++in_batch) {
            const auto frame = cursor.next();
            if (!frame.has_value()) {
                break;
            }
            succeeded += step(frame->payload) ? 1U : 0U;
        }
        const std::uint64_t stop = bs::read_ticks();
        if (in_batch < kBatchMessages) {
            return succeeded;
        }
        recorder.record(stop - start);
    }
}

// What one process measures, and what it learns on the way.
struct Session {
    std::span<const std::byte> source;
    std::uint64_t expected_messages = 0;
    bs::CounterGate& gate;
    bs::BatchLatencyRecorder& recorder;

    // Inside the counter gate, so counters can be divided by them.
    std::uint64_t gated_messages = 0;
    std::uint64_t gated_ticks = 0;
    // From the first repetition, so a reader can see what warming up did.
    std::vector<std::uint64_t> warmup_pass_ticks;
};

template <class Step>
void warm_up(Session& session, Step& step) {
    const bool record = session.warmup_pass_ticks.empty();
    for (int pass = 0; pass < kWarmupPasses; ++pass) {
        step.begin_pass();
        const std::uint64_t start = bs::read_ticks();
        benchmark::DoNotOptimize(run_pass(session.source, step));
        const std::uint64_t stop = bs::read_ticks();
        if (record) {
            session.warmup_pass_ticks.push_back(stop - start);
        }
    }
}

// Warm-up runs before the first state iteration, so Google Benchmark's clock has not
// started, and the gate is shut. Each repetition times one pass, so each timed pass
// gets its check, also outside the clock and the gate.
template <class Step>
void throughput(benchmark::State& state, Session& session, Step step) {
    warm_up(session, step);
    if (const auto opened = session.gate.enable(); !opened.has_value()) {
        state.SkipWithError(opened.error());
        return;
    }
    std::uint64_t succeeded = 0;
    const std::uint64_t start = bs::read_ticks();
    for (auto _ : state) {
        step.begin_pass();
        succeeded = run_pass(session.source, step);
        benchmark::DoNotOptimize(succeeded);
        benchmark::ClobberMemory();
    }
    const std::uint64_t stop = bs::read_ticks();
    if (const auto closed = session.gate.disable(); !closed.has_value()) {
        state.SkipWithError(closed.error());
        return;
    }

    if (succeeded != session.expected_messages) {
        state.SkipWithError(std::format("a timed pass handled {} of {} messages", succeeded,
                                        session.expected_messages));
        return;
    }
    if (const auto checked = step.check_output(session.source); !checked.has_value()) {
        state.SkipWithError(checked.error());
        return;
    }
    session.gated_messages += succeeded;
    session.gated_ticks += stop - start;
    state.SetItemsProcessed(static_cast<std::int64_t>(succeeded));
    state.SetBytesProcessed(static_cast<std::int64_t>(session.source.size()));
}

// The same work with a timer read at each batch edge. It is apart from throughput so
// that the reads and the histogram touch neither the throughput figure nor the counters.
template <class Step>
void batch_latency(benchmark::State& state, Session& session, Step step, std::mt19937_64& phases) {
    warm_up(session, step);
    std::uint64_t succeeded = 0;
    for (auto _ : state) {
        const std::uint64_t phase = phases() % kBatchMessages;
        step.begin_pass();
        succeeded = run_timed_pass(session.source, step, phase, session.recorder);
        benchmark::DoNotOptimize(succeeded);
        benchmark::ClobberMemory();
    }
    if (succeeded != session.expected_messages) {
        state.SkipWithError(std::format("a timed pass handled {} of {} messages", succeeded,
                                        session.expected_messages));
        return;
    }
    if (const auto checked = step.check_output(session.source); !checked.has_value()) {
        state.SkipWithError(checked.error());
    }
}

// True if a benchmark is seen to have failed.
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

template <class Step>
void register_workload(Session& session, Step step, std::mt19937_64& phases) {
    const auto configure = [](benchmark::Benchmark* benchmark) {
        benchmark->Iterations(1)->Repetitions(kRepetitions)->UseRealTime()->Unit(
            benchmark::kMillisecond);
    };
    const std::string name{Step::kName};
    configure(benchmark::RegisterBenchmark(
        name + "/throughput",
        [&session, step](benchmark::State& state) { throughput(state, session, step); }));
    configure(benchmark::RegisterBenchmark(
        name + "/latency", [&session, step, &phases](benchmark::State& state) {
            batch_latency(state, session, step, phases);
        }));
}

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
};

template <class Number>
[[nodiscard]] bool parse_number(std::string_view text, Number& number) noexcept {
    const auto* const end = text.data() + text.size();
    const auto [next, error] = std::from_chars(text.data(), end, number);
    return error == std::errc{} && next == end;
}

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
            return std::unexpected(std::format("unknown argument: {}", argument));
        }
    }
    if (options.list_cases) {
        return options;
    }
    if (options.slice.empty() || options.expected_sha256.empty() ||
        options.expected_messages == 0 || options.report.empty() ||
        (options.workload != DecodeOnly::kName && options.workload != DecodeEncode::kName)) {
        return std::unexpected(std::string{
            "usage: itch_codec_bench --slice=PATH --expect-sha256=HEX --expect-messages=N "
            "--case=NAME --report=PATH [--core=N] [--perf-control=FIFO --perf-ack=FIFO] "
            "[benchmark flags]\n       itch_codec_bench --list-cases"});
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
    std::string_view workload;
    std::uint64_t batch_phase_seed;
};

[[nodiscard]] std::string render_report(const ReportInputs& in) {
    const bs::BuildInfo& build = bs::build_info();
    harness::JsonWriter json;
    json.begin_object();
    json.field("case", in.workload);

    json.key("build");
    json.begin_object();
    json.field("compiler", std::format("{} {}", build.compiler_id, build.compiler_version));
    json.field("compile_flags", build.compile_flags);
    json.field("build_type", build.build_type);
    json.field("sanitizers", build.sanitizers);
    json.field("commit", build.commit);
    json.field("tree_dirty", build.tree_dirty);
    json.field("isa", build.isa);
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
    json.field("messages", in.session.gated_messages);
    json.field("wall_ns", in.timer_start.to_ns(in.session.gated_ticks));
    json.end_object();

    json.key("latency");
    json.begin_object();
    json.field("unit", in.latency.unit);
    json.field("batch_messages", in.latency.batch_messages);
    json.field("batches", in.latency.batches);
    json.field("batch_phase_seed", in.batch_phase_seed);
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
    std::println(stderr, "itch_codec_bench: {}", message);
    return kExitFailed;
}

template <class Step>
int run_workload(const Options& options, Step step, std::span<const std::byte> source,
                 bs::CounterGate& gate, const bs::PinResult& pin,
                 const bs::TimerInfo& timer_start, const bs::StreamCheck& check) {
    auto recorder = bs::BatchLatencyRecorder::create(kHighestBatchTicks);
    if (!recorder.has_value()) {
        return fail(recorder.error());
    }
    Session session{.source = source,
                    .expected_messages = options.expected_messages,
                    .gate = gate,
                    .recorder = *recorder,
                    .gated_messages = 0,
                    .gated_ticks = 0,
                    .warmup_pass_ticks = {}};
    std::mt19937_64 phases{Step::kBatchPhaseSeed};
    register_workload(session, step, phases);

    FailureWatch reporter;
    benchmark::RunSpecifiedBenchmarks(&reporter);
    benchmark::Shutdown();
    if (reporter.failed()) {
        return fail("a benchmark failed, so this run has no result");
    }

    // Still under the same process, so a clock that moved would show here.
    const auto timer_end = bs::calibrate_timer(kCalibrationWindow);
    if (!timer_end.has_value()) {
        return fail(timer_end.error());
    }
    const auto latency = recorder->report(timer_start, kBatchMessages);
    if (!latency.has_value()) {
        return fail(latency.error());
    }

    const std::string report = render_report({.pin = pin,
                                              .gate = gate,
                                              .check = check,
                                              .timer_start = timer_start,
                                              .timer_end = *timer_end,
                                              .latency = *latency,
                                              .session = session,
                                              .workload = Step::kName,
                                              .batch_phase_seed = Step::kBatchPhaseSeed});
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
        std::println("{}\n{}", DecodeOnly::kName, DecodeEncode::kName);
        return kExitOk;
    }

    // Pinned first, so that calibration, verification and warm-up all run where the
    // measurement will.
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
    std::vector<std::byte> output(source.size());

    const auto check = bs::verify_round_trip(source, options->expected_sha256,
                                             options->expected_messages, output);
    if (!check.has_value()) {
        return fail(check.error());
    }

    if (options->workload == DecodeOnly::kName) {
        return run_workload(*options, DecodeOnly{}, source, *gate, pin, *timer_start, *check);
    }
    return run_workload(*options, DecodeEncode{.output = output}, source, *gate, pin, *timer_start,
                        *check);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
