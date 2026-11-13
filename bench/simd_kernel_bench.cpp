// The decode kernels of codec/simd.hpp, one kernel and one set per case, on L1-resident
// seeded input: one interleaved vector of 1,024 values, decoded over and over, the setup
// DS-3 and DS-4's FastLanes figures use. Case names say the kernel, the width where it
// has one, and the set, as pfor_unpack/w=13/neon.
//
// What BENCHMARKS.md's decode targets read here: DS-3 and DS-4 the two delta forms,
// pfor_delta_two_pass and pfor_delta_fused, listed at every width 0 to 64 so that the
// median across widths is the source's own statistic; DS-8 dod_reconstruct/scalar, the
// delta-of-delta reconstruct the PFOR decoder runs, with no bit parsing. Real-data
// decodes are in column_decode_bench.
//
// Two ways to run it:
//
//   run_bench.py drives one process per case, --case=NAME --report=PATH, through the
//   measurement wrapper: ten repetitions of one pass each, a pass kPassCalls calls, then
//   the same passes timed in batches for the latency histogram.
//
//   By hand, with no --case: every case of every width 0 to 64, under Google Benchmark's
//   own iteration counts, for iteration only. --benchmark_filter picks cases.
//
// How the bar in research/evidence/code-quality.md section 16 is met:
//
//   B1  Every input comes from a fixed seed owned by its case, so a new case changes
//       nothing about another's data.
//   B2  Before any timing each case's output is compared by memcmp with its reference,
//       and again after the timed passes: the generated values for an unpack, the
//       original sequence for the delta-of-delta reconstruct, its second differences for
//       restore_copy, and the scalar set's output on the same input for the rest. Each
//       case names its reference in the results (review A-7). The output is filled with a
//       poison value after warm-up, outside every timed region, so the check after them
//       sees a timed pass that wrote nothing (review A-4).
//   B3  DoNotOptimize on the output after every call and ClobberMemory after every pass.
//   B4  Values per second here; counters from `perf stat` around the same passes on
//       Linux, marked unavailable on macOS.
//   B5  The kernels are called through the same table entries the codecs call.
//   B6  The input is one vector, decoded repeatedly: a predictor can learn it, which is
//       the point of an L1-resident kernel figure and why the store's own decode and the
//       queries are measured separately, on the real day (docs/NOTES.md, "SIMD").

#include "tick_store/bench_support/affinity.hpp"
#include "tick_store/bench_support/build_info.hpp"
#include "tick_store/bench_support/counter_gate.hpp"
#include "tick_store/bench_support/latency_recorder.hpp"
#include "tick_store/bench_support/resolution_guard.hpp"
#include "tick_store/bench_support/timer.hpp"
#include "tick_store/codec/int_transform.hpp"
#include "tick_store/codec/lane_pack.hpp"
#include "tick_store/codec/simd.hpp"
#include "tick_store/harness/json_writer.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
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
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace bs = tick_store::bench_support;
namespace codec = tick_store::codec;
namespace harness = tick_store::harness;
namespace simd = tick_store::codec::simd;

using Vector = std::array<std::uint64_t, codec::kVectorValues>;

// Calls per timed pass: about 8.4 M values, some milliseconds at any rate here.
constexpr std::size_t kPassCalls = 8192;
// Calls per latency batch: microseconds, far above either host's timer floor.
constexpr std::size_t kBatchCalls = 16;
constexpr int kRepetitions = 10;
constexpr int kWarmupPasses = 2;
constexpr std::chrono::milliseconds kCalibrationWindow{200};
constexpr std::uint64_t kHighestBatchTicks = 1ULL << 40U;
// The widths run_bench lists for the unpack, linear and narrow cases; by hand, every width
// runs. The two 64-bit delta forms are listed at every width 0 to 64 (DS-3, DS-4).
constexpr std::array<unsigned, 8> kListedWidths{1, 8, 13, 16, 21, 32, 45, 64};
constexpr unsigned kWidest = 64;
constexpr unsigned kNarrowWidest = 21;

// One kernel on one set: what it runs, how many values a call yields, and whether its
// output matches its reference, which `reference` names.
struct Case {
    std::string name;
    std::uint64_t seed = 0;
    std::size_t values_per_call = codec::kVectorValues;
    std::function<void(std::size_t calls)> run;
    std::function<bool()> verify;
    // Where run() writes, so poison() can overwrite what warm-up left there.
    std::shared_ptr<Vector> out;
    // What verify() compares the output with, for the results.
    std::string_view reference;
};

constexpr std::string_view kGenerated = "the generated values the input packs";
constexpr std::string_view kScalarSet = "the scalar set's output on the same input";

// No case's answer is this value in all 1,024 places.
constexpr std::uint64_t kPoison = 0xA5A5'A5A5'A5A5'A5A5ULL;

void poison(const Case& c) { std::ranges::fill(*c.out, kPoison); }

Case named(std::string name, std::uint64_t seed) {
    Case c;
    c.name = std::move(name);
    c.seed = seed;
    return c;
}

std::uint64_t mask_of(unsigned width) {
    return width >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1U;
}

// The packed bytes of a vector of random values at `width`, held in an exact buffer.
struct PackedVector {
    std::vector<std::byte> bytes;
    Vector values{};
};

std::shared_ptr<PackedVector> packed_vector(std::uint64_t seed, unsigned width) {
    auto packed = std::make_shared<PackedVector>();
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed): fixed for B1
    std::mt19937_64 rng{seed};
    for (std::uint64_t& value : packed->values) {
        value = rng() & mask_of(width);
    }
    std::vector<std::uint64_t> words(codec::vector_words(width));
    codec::pack_vector(packed->values, width, words);
    codec::append_words_le(words, packed->bytes);
    packed->bytes.shrink_to_fit();
    return packed;
}

bool same(std::span<const std::uint64_t> a, std::span<const std::uint64_t> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

const simd::Kernels& scalar() { return *simd::kernels_for(simd::KernelLevel::scalar); }

std::string level_name(const simd::Kernels& set) { return std::string{simd::to_string(set.level)}; }

// --- The cases -------------------------------------------------------------------------

Case unpack_case(const simd::Kernels& set, unsigned width) {
    const std::uint64_t seed = 0x5EED'0100ULL + width;
    const auto in = packed_vector(seed, width);
    const auto out = std::make_shared<Vector>();
    Case c = named(std::format("pfor_unpack/w={}/{}", width, level_name(set)), seed);
    c.out = out;
    c.reference = kGenerated;
    c.run = [&set, in, out, width](std::size_t calls) {
        for (std::size_t i = 0; i < calls; ++i) {
            set.unpack_vector(in->bytes, width, *out);
            benchmark::DoNotOptimize(out->data());
        }
    };
    c.verify = [in, out] { return same(*out, in->values); };
    return c;
}

// The linear layout, which tails and patch lists take: 1,024 values at `width`, packed
// end to end, in an exact buffer.
Case linear_case(const simd::Kernels& set, unsigned width) {
    const std::uint64_t seed = 0x5EED'0600ULL + width;
    const auto in = std::make_shared<PackedVector>();
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed): fixed for B1
    std::mt19937_64 rng{seed};
    for (std::uint64_t& value : in->values) {
        value = rng() & mask_of(width);
    }
    codec::pack_linear(in->values, width, in->bytes);
    in->bytes.shrink_to_fit();
    const auto out = std::make_shared<Vector>();
    Case c = named(std::format("linear_unpack/w={}/{}", width, level_name(set)), seed);
    c.out = out;
    c.reference = kGenerated;
    c.run = [&set, in, out, width](std::size_t calls) {
        for (std::size_t i = 0; i < calls; ++i) {
            set.unpack_linear(in->bytes, width, *out);
            benchmark::DoNotOptimize(out->data());
        }
    };
    c.verify = [in, out] { return same(*out, in->values); };
    return c;
}

// The base is small and positive, as for timestamps and prices, and the same for every
// form, so the narrow form may run at widths up to kNarrowWidest.
constexpr std::uint64_t kDeltaBase = 3;

Vector delta_reference(const PackedVector& in, std::uint64_t carry) {
    Vector reference = in.values;
    (void)scalar().delta_pass(reference, kDeltaBase, false, carry);
    return reference;
}

enum class DeltaForm : std::uint8_t { two_pass, fused, narrow };

Case delta_case(const simd::Kernels& set, unsigned width, DeltaForm form) {
    const std::uint64_t seed = 0x5EED'0200ULL + width;
    const auto in = packed_vector(seed, width);
    const auto out = std::make_shared<Vector>();
    const auto reference = std::make_shared<Vector>(delta_reference(*in, 0));
    constexpr std::array<std::string_view, 3> kNames{"pfor_delta_two_pass", "pfor_delta_fused",
                                                     "pfor_delta_narrow"};
    Case c = named(std::format("{}/w={}/{}", kNames[static_cast<std::size_t>(form)], width,
                               level_name(set)), seed);
    c.out = out;
    c.reference = kScalarSet;
    c.run = [&set, in, out, width, form](std::size_t calls) {
        for (std::size_t i = 0; i < calls; ++i) {
            switch (form) {
                case DeltaForm::two_pass:
                    set.unpack_vector(in->bytes, width, *out);
                    benchmark::DoNotOptimize(set.delta_pass(*out, kDeltaBase, false, 0));
                    break;
                case DeltaForm::fused:
                    benchmark::DoNotOptimize(set.unpack_delta(in->bytes, width, kDeltaBase, false, 0, *out));
                    break;
                case DeltaForm::narrow:
                    benchmark::DoNotOptimize(
                        set.unpack_delta_narrow(in->bytes, width, kDeltaBase, false, 0, *out));
                    break;
            }
            benchmark::DoNotOptimize(out->data());
        }
    };
    c.verify = [out, reference] { return same(*out, *reference); };
    return c;
}

// The inverse delta alone, in place on a copy restored before each call, so every call
// sums the same deltas; the copy is timed too, and the same for both forms.
Case prefix_case(const simd::Kernels& set, std::string_view form_name,
                 std::uint64_t (*form)(std::span<std::uint64_t>, std::uint64_t) noexcept) {
    const std::uint64_t seed = 0x5EED'0300ULL;
    const auto in = packed_vector(seed, 20);
    const auto out = std::make_shared<Vector>();
    const auto reference = std::make_shared<Vector>(in->values);
    (void)scalar().prefix_sum(*reference, 0);
    Case c = named(std::format("prefix_sum/{}/{}", form_name, level_name(set)), seed);
    c.out = out;
    c.reference = kScalarSet;
    c.run = [in, out, form](std::size_t calls) {
        for (std::size_t i = 0; i < calls; ++i) {
            *out = in->values;
            benchmark::DoNotOptimize(form(*out, 0));
            benchmark::DoNotOptimize(out->data());
        }
    };
    c.verify = [out, reference] { return same(*out, *reference); };
    return c;
}

Case for_case(const simd::Kernels& set, bool zigzag) {
    const std::uint64_t seed = zigzag ? 0x5EED'0401ULL : 0x5EED'0400ULL;
    const auto in = packed_vector(seed, 20);
    const auto out = std::make_shared<Vector>();
    const auto reference = std::make_shared<Vector>(in->values);
    scalar().add_base(*reference, 12345, zigzag);
    Case c = named(std::format("{}/{}", zigzag ? "for_add_zigzag" : "for_add", level_name(set)), seed);
    c.out = out;
    c.reference = kScalarSet;
    c.run = [&set, in, out, zigzag](std::size_t calls) {
        for (std::size_t i = 0; i < calls; ++i) {
            *out = in->values;
            set.add_base(*out, 12345, zigzag);
            benchmark::DoNotOptimize(out->data());
        }
    };
    c.verify = [out, reference] { return same(*out, *reference); };
    return c;
}

// Stream VByte: 1,024 values whose bit widths are drawn evenly from 1 to 32.
struct SvbInput {
    std::vector<std::byte> control;
    std::vector<std::byte> data;
    Vector values{};
};

// The bytes Stream VByte stores a value below 2^32 in.
unsigned svb_length(std::uint64_t value) {
    unsigned length = 1;
    while (length < 4U && (value >> (8U * length)) != 0) {
        ++length;
    }
    return length;
}

std::shared_ptr<SvbInput> svb_input(std::uint64_t seed) {
    auto in = std::make_shared<SvbInput>();
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed): fixed for B1
    std::mt19937_64 rng{seed};
    in->control.assign(codec::kVectorValues / 4U, std::byte{0});
    for (std::size_t i = 0; i < codec::kVectorValues; ++i) {
        const unsigned bits = 1U + static_cast<unsigned>(rng() % 32U);
        const std::uint64_t value = rng() & mask_of(bits);
        in->values[i] = value;
        const unsigned length = svb_length(value);
        in->control[i / 4U] |= static_cast<std::byte>((length - 1U) << (2U * (i % 4U)));
        for (unsigned b = 0; b < length; ++b) {
            in->data.push_back(static_cast<std::byte>((value >> (8U * b)) & 0xFFU));
        }
    }
    in->data.shrink_to_fit();
    return in;
}

Case svb_case(const simd::Kernels& set) {
    const std::uint64_t seed = 0x5EED'0500ULL;
    const auto in = svb_input(seed);
    const auto out = std::make_shared<Vector>();
    const auto ok = std::make_shared<bool>(true);
    Case c = named(std::format("svb_decode/{}", level_name(set)), seed);
    c.out = out;
    c.reference = kGenerated;
    c.run = [&set, in, out, ok](std::size_t calls) {
        for (std::size_t i = 0; i < calls; ++i) {
            *ok = set.svb_decode(in->control, in->data, *out) && *ok;
            benchmark::DoNotOptimize(out->data());
        }
    };
    c.verify = [in, out, ok] { return *ok && same(*out, in->values); };
    return c;
}

// DS-8: the delta-of-delta reconstruct PFOR runs once a block's second differences are
// unpacked, codec::delta_of_delta_decode, with no bit parsing: two running sums over 1,024
// values in place. It has no kernel of its own, so it is the same code in every set, and
// runs once, under the scalar set's name. Each call first restores the second differences
// from a copy, so every call sums the same input, and that copy is timed too:
// restore_copy/scalar times the copy alone. The input is a rising sequence like
// timestamps, its steps below 2^20, and the check is against that sequence itself, which
// the encoder's inverse turned into the second differences.
Case dod_case(bool copy_only) {
    const std::uint64_t seed = copy_only ? 0x5EED'0701ULL : 0x5EED'0700ULL;
    const auto values = std::make_shared<Vector>();
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed): fixed for B1
    std::mt19937_64 rng{seed};
    constexpr unsigned kStartBits = 40;
    constexpr unsigned kStepBits = 20;
    std::uint64_t time = rng() & mask_of(kStartBits);
    for (std::uint64_t& value : *values) {
        time += rng() & mask_of(kStepBits);
        value = time;
    }
    const auto second = std::make_shared<Vector>();
    codec::delta_of_delta_encode(*values, *second);
    const auto out = std::make_shared<Vector>();
    Case c = named(copy_only ? "restore_copy/scalar" : "dod_reconstruct/scalar", seed);
    c.out = out;
    c.reference = copy_only ? std::string_view{"the second differences it copies"}
                            : std::string_view{"the sequence the second differences encode"};
    c.run = [second, out, copy_only](std::size_t calls) {
        for (std::size_t i = 0; i < calls; ++i) {
            *out = *second;
            if (!copy_only) {
                codec::delta_of_delta_decode(*out);
            }
            benchmark::DoNotOptimize(out->data());
        }
    };
    c.verify = [values, second, out, copy_only] {
        return same(*out, copy_only ? *second : *values);
    };
    return c;
}

std::vector<Case> all_cases(bool every_width) {
    std::vector<unsigned> widths;
    if (every_width) {
        for (unsigned w = 0; w <= 64; ++w) {
            widths.push_back(w);
        }
    } else {
        widths.assign(kListedWidths.begin(), kListedWidths.end());
    }
    std::vector<Case> cases;
    for (const simd::KernelLevel level : simd::kAllKernelLevels) {
        const simd::Kernels* set = simd::kernels_for(level);
        if (set == nullptr) {
            continue;
        }
        for (const unsigned w : widths) {
            cases.push_back(unpack_case(*set, w));
        }
        for (const unsigned w : widths) {
            cases.push_back(linear_case(*set, w));
        }
        for (unsigned w = 0; w <= kWidest; ++w) {
            cases.push_back(delta_case(*set, w, DeltaForm::two_pass));
            cases.push_back(delta_case(*set, w, DeltaForm::fused));
        }
        for (const unsigned w : widths) {
            if (w <= kNarrowWidest) {
                cases.push_back(delta_case(*set, w, DeltaForm::narrow));
            }
        }
        const simd::PrefixSumForms forms = simd::prefix_sum_forms(level);
        cases.push_back(prefix_case(*set, "vector", forms.vector));
        cases.push_back(prefix_case(*set, "tree", forms.tree));
        cases.push_back(for_case(*set, false));
        cases.push_back(for_case(*set, true));
        cases.push_back(svb_case(*set));
        if (level == simd::KernelLevel::scalar) {
            cases.push_back(dod_case(false));
            cases.push_back(dod_case(true));
        }
    }
    return cases;
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
        session.c->run(kPassCalls);
        const std::uint64_t stop = bs::read_ticks();
        if (record) {
            session.warmup_pass_ticks.push_back(stop - start);
        }
    }
}

void throughput(benchmark::State& state, Session& session) {
    warm_up(session);
    poison(*session.c);
    if (const auto opened = session.gate->enable(); !opened.has_value()) {
        state.SkipWithError(opened.error());
        return;
    }
    const std::uint64_t start = bs::read_ticks();
    for (const auto iteration : state) {
        static_cast<void>(iteration);
        session.c->run(kPassCalls);
        benchmark::ClobberMemory();
    }
    const std::uint64_t stop = bs::read_ticks();
    if (const auto closed = session.gate->disable(); !closed.has_value()) {
        state.SkipWithError(closed.error());
        return;
    }
    if (!session.c->verify()) {
        // The case's own reference, not the scalar set for every case (review RA-4).
        state.SkipWithError(std::format("a timed pass did not match {}", session.c->reference));
        return;
    }
    const std::uint64_t items = kPassCalls * session.c->values_per_call;
    session.gated_items += items * static_cast<std::uint64_t>(state.iterations());
    session.gated_ticks += stop - start;
    state.SetItemsProcessed(static_cast<std::int64_t>(items) * state.iterations());
    state.SetBytesProcessed(static_cast<std::int64_t>(items * sizeof(std::uint64_t)) *
                            state.iterations());
}

void batch_latency(benchmark::State& state, Session& session) {
    warm_up(session);
    poison(*session.c);
    for (const auto iteration : state) {
        static_cast<void>(iteration);
        for (std::size_t done = 0; done < kPassCalls; done += kBatchCalls) {
            const std::uint64_t start = bs::read_ticks();
            session.c->run(kBatchCalls);
            const std::uint64_t stop = bs::read_ticks();
            session.recorder->record(stop - start);
        }
        benchmark::ClobberMemory();
    }
    if (!session.c->verify()) {
        // The case's own reference, not the scalar set for every case (review RA-4).
        state.SkipWithError(std::format("a timed pass did not match {}", session.c->reference));
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
            return std::unexpected(std::format(
                "unknown argument: {}\nusage: simd_kernel_bench [--case=NAME --report=PATH "
                "[--core=N] [--perf-control=FIFO --perf-ack=FIFO]] [benchmark flags]\n"
                "       simd_kernel_bench --list-cases | --input-kind | --verify-all",
                argument));
        }
    }
    if (!options.workload.empty() && options.report.empty()) {
        return std::unexpected(std::string{"--case needs --report"});
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
    json.field("commit_source", build.commit_source);
    // Each case names the kernel set it ran last, as in pfor_unpack/w=13/neon.
    json.field("isa", std::format("{}, {} kernels", build.isa,
                                  std::string_view{c.name}.substr(c.name.rfind('/') + 1)));
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
    json.field("input_seed", c.seed);
    json.field("items_per_call", static_cast<std::uint64_t>(c.values_per_call));
    json.field("calls_per_pass", static_cast<std::uint64_t>(kPassCalls));
    json.field("against", std::format("{}, memcmp, before timing and after the timed passes",
                                      c.reference));
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
    json.field("batch_items", static_cast<std::uint64_t>(kBatchCalls * c.values_per_call));
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
    // The percentiles the batch count cannot support, each with why (review A-3).
    json.key("omitted_percentiles");
    json.begin_array();
    for (const bs::OmittedPercentile& point : in.latency.omitted) {
        json.begin_object();
        json.field("percentile", point.percentile);
        json.field("reason", point.reason);
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
    std::println(stderr, "simd_kernel_bench: {}", message);
    return kExitFailed;
}

// B2 before anything is timed: one call, then the check against its reference.
[[nodiscard]] std::expected<void, std::string> verify_before(Case& c) {
    c.run(1);
    if (!c.verify()) {
        return std::unexpected(std::format("{} does not match its reference", c.name));
    }
    return {};
}

int run_by_hand(std::vector<Case>& cases) {
    for (Case& c : cases) {
        if (auto checked = verify_before(c); !checked.has_value()) {
            return fail(checked.error());
        }
        benchmark::RegisterBenchmark(c.name, [&c](benchmark::State& state) {
            poison(c);
            for (const auto iteration : state) {
        static_cast<void>(iteration);
                c.run(1);
            }
            if (!c.verify()) {
                state.SkipWithError(std::format("a timed call did not match {}", c.reference));
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(c.values_per_call) * state.iterations());
        });
    }
    FailureWatch reporter;
    benchmark::RunSpecifiedBenchmarks(&reporter);
    benchmark::Shutdown();
    return reporter.failed() ? fail("a benchmark failed") : kExitOk;
}

int run_case(const Options& options, Case& c) {
    const bs::PinResult pin = options.core.has_value() ? bs::pin_current_thread(*options.core)
                                                       : bs::leave_unpinned();
    if (pin.status == bs::PinStatus::failed) {
        return fail(pin.detail);
    }
    auto gate = bs::CounterGate::for_run(options.perf_control, options.perf_acknowledge);
    if (!gate.has_value()) {
        return fail(gate.error());
    }
    const auto timer_start = bs::calibrate_timer(kCalibrationWindow);
    if (!timer_start.has_value()) {
        return fail(timer_start.error());
    }
    if (auto checked = verify_before(c); !checked.has_value()) {
        return fail(checked.error());
    }
    auto recorder = bs::BatchLatencyRecorder::create(kHighestBatchTicks);
    if (!recorder.has_value()) {
        return fail(recorder.error());
    }
    Session session{.c = &c, .gate = &*gate, .recorder = &*recorder, .gated_items = 0,
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
    const auto latency = recorder->report(*timer_start, kBatchCalls * c.values_per_call);
    if (!latency.has_value()) {
        return fail(latency.error());
    }
    const std::string report = render_report({.pin = pin,
                                              .gate = *gate,
                                              .timer_start = *timer_start,
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
    if (options->input_kind) {
        std::println("seeded");
        return kExitOk;
    }
    if (options->list_cases) {
        for (const Case& c : all_cases(false)) {
            std::println("{}", c.name);
        }
        return kExitOk;
    }
    // B2 for every case at every width, with nothing timed: what ctest runs.
    if (options->verify_all) {
        std::vector<Case> cases = all_cases(true);
        for (Case& c : cases) {
            if (auto checked = verify_before(c); !checked.has_value()) {
                return fail(checked.error());
            }
        }
        std::println("{} cases match their reference", cases.size());
        return kExitOk;
    }
    if (options->workload.empty()) {
        std::vector<Case> cases = all_cases(true);
        return run_by_hand(cases);
    }
    std::vector<Case> cases = all_cases(true);
    const auto found = std::ranges::find(cases, options->workload, &Case::name);
    if (found == cases.end()) {
        return fail(std::format("no case {}: --list-cases names them", options->workload));
    }
    return run_case(*options, *found);
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
