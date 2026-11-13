// The support library under the benchmarks. The guard is the point: a percentile the
// timer cannot resolve must never get out, however it is reached.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "tick_store/bench_support/affinity.hpp"
#include "tick_store/bench_support/build_info.hpp"
#include "tick_store/bench_support/counter_gate.hpp"
#include "tick_store/bench_support/latency_recorder.hpp"
#include "tick_store/bench_support/resolution_guard.hpp"
#include "tick_store/bench_support/stream_check.hpp"
#include "tick_store/bench_support/timer.hpp"
#include "tick_store/harness/sha256.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tick_store::bench_support {
namespace {

// The hosts' published figures, so these tests read as the rule they enforce.
constexpr double kLinuxStepNs = 10.0;
constexpr double kLinuxReadNs = 19.0;
constexpr double kMacStepNs = 41.667;
constexpr double kMacReadNs = 4.0;

TimerInfo linux_like_timer() {
    return {.source = "test",
            .ticks_per_second = 2e9,
            .resolution_ticks = 20,
            .resolution_ns = kLinuxStepNs,
            .read_cost_ns = kLinuxReadNs,
            .window_ns = 2e8};
}

TEST(ResolutionGuard, FloorIsTenStepsOnBothHosts) {
    EXPECT_DOUBLE_EQ(ResolutionGuard(kLinuxStepNs, kLinuxReadNs).floor_ns(), 100.0);
    EXPECT_DOUBLE_EQ(ResolutionGuard(kMacStepNs, kMacReadNs).floor_ns(), 416.67);
}

TEST(ResolutionGuard, ReadCostRaisesTheFloorWhenItDominates) {
    EXPECT_DOUBLE_EQ(ResolutionGuard(1.0, 30.0).floor_ns(), 150.0);
}

TEST(ResolutionGuard, RefusesAValueBelowTheFloor) {
    const ResolutionGuard guard{kLinuxStepNs, kLinuxReadNs};
    const auto refused = guard.publish(99.9, "p50");
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().find("refusing to publish p50"), std::string::npos);
    EXPECT_NE(refused.error().find("99.900"), std::string::npos);
}

TEST(ResolutionGuard, PublishesAtAndAboveTheFloor) {
    const ResolutionGuard guard{kLinuxStepNs, kLinuxReadNs};
    EXPECT_EQ(guard.publish(100.0, "p50"), 100.0);
    EXPECT_EQ(guard.publish(250.0, "p50"), 250.0);
}

TEST(ResolutionGuard, RefusesASingleTickOnTheMac) {
    const ResolutionGuard guard{kMacStepNs, kMacReadNs};
    EXPECT_FALSE(guard.publish(3.0 * kMacStepNs, "p99").has_value());
}

TEST(Resolution, StepIsTheCommonDivisorNotTheSmallestDelta) {
    // A read that costs two 20-tick steps never shows a delta of 20.
    constexpr std::array<std::uint64_t, 5> deltas{40, 60, 40, 80, 60};
    EXPECT_EQ(resolution_from_deltas(deltas), 20U);
}

TEST(Resolution, ZeroDeltasDoNotHideTheStep) {
    constexpr std::array<std::uint64_t, 6> deltas{0, 0, 1, 0, 1, 0};
    EXPECT_EQ(resolution_from_deltas(deltas), 1U);
}

TEST(Resolution, NoMovementGivesZero) {
    constexpr std::array<std::uint64_t, 3> deltas{0, 0, 0};
    EXPECT_EQ(resolution_from_deltas(deltas), 0U);
    EXPECT_EQ(resolution_from_deltas({}), 0U);
}

TEST(Timer, CalibrationMeasuresARateAStepAndACost) {
    const auto timer = calibrate_timer(std::chrono::milliseconds{20});
    ASSERT_TRUE(timer.has_value()) << timer.error();
    EXPECT_EQ(timer->source, timer_source());
    EXPECT_GT(timer->ticks_per_second, 1e6);
    EXPECT_GE(timer->resolution_ticks, 1U);
    EXPECT_GT(timer->resolution_ns, 0.0);
    EXPECT_GT(timer->read_cost_ns, 0.0);
    EXPECT_GE(timer->window_ns, 2e7);
}

TEST(Timer, TicksConvertToNanoseconds) {
    EXPECT_DOUBLE_EQ(linux_like_timer().to_ns(2000), 1000.0);
}

TEST(Timer, ReadsDoNotGoBackwards) {
    std::uint64_t previous = read_ticks();
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t now = read_ticks();
        EXPECT_GE(now, previous);
        previous = now;
    }
}

constexpr std::uint64_t kHighestTicks = 1ULL << 32U;
constexpr std::uint64_t kBatch = 1024;

BatchLatencyRecorder make_recorder() {
    auto recorder = BatchLatencyRecorder::create(kHighestTicks);
    EXPECT_TRUE(recorder.has_value());
    return std::move(*recorder);
}

TEST(BatchLatency, PublishesBatchUnitAndResolutionWithThePercentiles) {
    auto recorder = make_recorder();
    // 100,000 batches, the fewest that put 10 above p99.99, so all five are published.
    for (std::uint64_t ticks = 10'000; ticks < 110'000; ++ticks) {
        recorder.record(ticks);
    }
    const auto report = recorder.report(linux_like_timer(), kBatch);
    ASSERT_TRUE(report.has_value()) << report.error();
    EXPECT_EQ(report->unit, "ns per batch");
    EXPECT_EQ(report->batch_messages, kBatch);
    EXPECT_EQ(report->batches, 100'000U);
    EXPECT_DOUBLE_EQ(report->timer_resolution_ns, kLinuxStepNs);
    EXPECT_DOUBLE_EQ(report->floor_ns, 100.0);
    ASSERT_EQ(report->percentiles.size(), 5U);
    EXPECT_TRUE(report->omitted.empty());
    EXPECT_DOUBLE_EQ(report->percentiles.front().percentile, 50.0);
    // 3 significant figures of the 60,000 tick median, at 2 ticks a ns.
    EXPECT_NEAR(report->percentiles.front().value_ns, 30'000.0, 60.0);
    for (std::size_t i = 1; i < report->percentiles.size(); ++i) {
        EXPECT_GE(report->percentiles[i].value_ns, report->percentiles[i - 1].value_ns);
    }
    EXPECT_LE(report->min_ns, report->percentiles.front().value_ns);
    EXPECT_GE(report->max_ns, report->percentiles.back().value_ns);
}

// Ten batches and one slower: p99, p99.9 and p99.99 would each be the slowest batch, and
// p50 and p90 rest on fewer than 10 batches above them (review A-3).
// The published percentiles, as "p99=1000000 " pairs, for a failure message.
std::string published(const LatencyReport& report) {
    std::string out;
    for (const PublishedPercentile& point : report.percentiles) {
        out += std::format("p{}={} ", point.percentile, point.value_ns);
    }
    return out;
}

TEST(BatchLatency, LeavesOutAPercentileTooFewBatchesLieAbove) {
    auto recorder = make_recorder();
    for (int i = 0; i < 10; ++i) {
        recorder.record(1'000'000);
    }
    recorder.record(2'000'000);
    const auto report = recorder.report(linux_like_timer(), 1);
    ASSERT_TRUE(report.has_value()) << report.error();
    EXPECT_EQ(report->batches, 11U);
    EXPECT_TRUE(report->percentiles.empty()) << published(*report);
    ASSERT_EQ(report->omitted.size(), 5U);
    EXPECT_DOUBLE_EQ(report->omitted[2].percentile, 99.0);
    EXPECT_EQ(report->omitted[2].reason, "0.11 of 11 batches lie above it, and publishing needs 10");
    EXPECT_NEAR(report->min_ns, 500'000.0, 1'000.0);
    EXPECT_GE(report->max_ns, 1'000'000.0);
}

// The count of batches above each percentile is exact at the boundary: 20 batches put 10
// above p50 and 2 above p90, and 99,999 put 9.9999 above p99.99.
TEST(BatchLatency, PublishesAPercentileWithTenBatchesAboveItAndNotOneFewer) {
    auto twenty = make_recorder();
    for (int i = 0; i < 20; ++i) {
        twenty.record(1'000'000);
    }
    const auto small = twenty.report(linux_like_timer(), 1);
    ASSERT_TRUE(small.has_value()) << small.error();
    ASSERT_EQ(small->percentiles.size(), 1U) << published(*small);
    EXPECT_DOUBLE_EQ(small->percentiles[0].percentile, 50.0);
    ASSERT_EQ(small->omitted.size(), 4U);
    EXPECT_EQ(small->omitted[0].reason, "2 of 20 batches lie above it, and publishing needs 10");

    auto almost = make_recorder();
    for (int i = 0; i < 99'999; ++i) {
        almost.record(1'000'000);
    }
    const auto large = almost.report(linux_like_timer(), 1);
    ASSERT_TRUE(large.has_value()) << large.error();
    EXPECT_EQ(large->percentiles.size(), 4U) << published(*large);
    ASSERT_EQ(large->omitted.size(), 1U);
    EXPECT_DOUBLE_EQ(large->omitted[0].percentile, 99.99);
    EXPECT_EQ(large->omitted[0].reason,
              "9.9999 of 99999 batches lie above it, and publishing needs 10");
}

TEST(BatchLatency, RefusesASubResolutionPercentile) {
    auto recorder = make_recorder();
    // 50 ticks is 25 ns: a batch far too short for this timer.
    for (int i = 0; i < 1000; ++i) {
        recorder.record(50);
    }
    const auto report = recorder.report(linux_like_timer(), 1);
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().find("refusing to publish"), std::string::npos);
}

TEST(BatchLatency, OneResolvableTailDoesNotRescueAnUnresolvableMedian) {
    auto recorder = make_recorder();
    for (int i = 0; i < 1000; ++i) {
        recorder.record(50);
    }
    recorder.record(1'000'000);
    EXPECT_FALSE(recorder.report(linux_like_timer(), 1).has_value());
}

TEST(BatchLatency, RefusesEvenIfOnlyTheFastestBatchIsBelowTheFloor) {
    auto recorder = make_recorder();
    recorder.record(50);
    for (int i = 0; i < 100'000; ++i) {
        recorder.record(1'000'000);
    }
    EXPECT_FALSE(recorder.report(linux_like_timer(), kBatch).has_value());
}

TEST(BatchLatency, RefusesWhenABatchFellOutOfRange) {
    auto recorder = make_recorder();
    recorder.record(1'000'000);
    recorder.record(kHighestTicks * 4);
    const auto report = recorder.report(linux_like_timer(), kBatch);
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().find("outside the histogram"), std::string::npos);
}

TEST(BatchLatency, RefusesAnEmptyHistogram) {
    EXPECT_FALSE(make_recorder().report(linux_like_timer(), kBatch).has_value());
}

// Each pin runs on its own thread, so the test process keeps its affinity.
TEST(Affinity, PinsWhereSupportedAndSaysSoWhereNot) {
    std::thread pinned_thread{[] {
        const auto here = current_cpu();
        const PinResult result = pin_current_thread(here.value_or(0));
        if (here.has_value()) {
            EXPECT_EQ(result.status, PinStatus::pinned) << result.detail;
            EXPECT_EQ(result.observed_cpu, here);
        } else {
            EXPECT_EQ(result.status, PinStatus::not_supported);
            EXPECT_EQ(result.observed_cpu, std::nullopt);
            EXPECT_EQ(to_string(result.status), "not-supported");
        }
    }};
    pinned_thread.join();
}

TEST(Affinity, ACpuThatCannotExistIsNeverReportedPinned) {
    std::thread pinned_thread{[] {
        EXPECT_NE(pin_current_thread(-1).status, PinStatus::pinned);
        EXPECT_NE(pin_current_thread(100'000).status, PinStatus::pinned);
    }};
    pinned_thread.join();
}

TEST(Affinity, ARunThatAsksForNoCpuSaysWhetherItCouldHave) {
    const PinResult result = leave_unpinned();
    EXPECT_EQ(result.status,
              current_cpu().has_value() ? PinStatus::not_requested : PinStatus::not_supported);
}

// Stands in for perf at the far end of the two FIFOs.
class FakePerf {
public:
    explicit FakePerf(const test::ScratchDir& scratch)
        : control_(scratch / "control"), acknowledge_(scratch / "ack") {
        EXPECT_EQ(::mkfifo(control_.c_str(), 0600), 0);
        EXPECT_EQ(::mkfifo(acknowledge_.c_str(), 0600), 0);
    }

    [[nodiscard]] const std::filesystem::path& control() const { return control_; }
    [[nodiscard]] const std::filesystem::path& acknowledge() const { return acknowledge_; }

    // Reads `commands` commands, answering each with `reply`.
    [[nodiscard]] std::thread serve(int commands, std::string_view reply,
                                    std::string& seen) const {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic
        const int in = ::open(control_.c_str(), O_RDWR | O_CLOEXEC);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic
        const int out = ::open(acknowledge_.c_str(), O_RDWR | O_CLOEXEC);
        return std::thread{[=, &seen] {
            std::array<char, 32> buffer{};
            for (int i = 0; i < commands; ++i) {
                const auto got = ::read(in, buffer.data(), buffer.size());
                ASSERT_GT(got, 0);
                seen.append(buffer.data(), static_cast<std::size_t>(got));
                ASSERT_EQ(::write(out, reply.data(), reply.size()), std::ssize(reply));
            }
            ::close(in);
            ::close(out);
        }};
    }

private:
    std::filesystem::path control_;
    std::filesystem::path acknowledge_;
};

TEST(CounterGate, SendsEachCommandAndWaitsForTheAcknowledgement) {
    const test::ScratchDir scratch;
    const FakePerf perf{scratch};
    auto gate = CounterGate::open(perf.control(), perf.acknowledge());
    ASSERT_TRUE(gate.has_value()) << gate.error();
    EXPECT_EQ(gate->status(), CounterStatus::gated);

    std::string seen;
    std::thread server = perf.serve(2, "ack\n", seen);
    EXPECT_TRUE(gate->enable().has_value());
    EXPECT_TRUE(gate->disable().has_value());
    server.join();
    EXPECT_EQ(seen, "enable\ndisable\n");
}

TEST(CounterGate, ANonAcknowledgementIsAnError) {
    const test::ScratchDir scratch;
    const FakePerf perf{scratch};
    auto gate = CounterGate::open(perf.control(), perf.acknowledge());
    ASSERT_TRUE(gate.has_value()) << gate.error();

    std::string seen;
    std::thread server = perf.serve(1, "err\n", seen);
    EXPECT_FALSE(gate->enable().has_value());
    server.join();
}

TEST(CounterGate, ASilentPerfTimesOutInsteadOfHanging) {
    const test::ScratchDir scratch;
    const FakePerf perf{scratch};
    auto gate = CounterGate::open(perf.control(), perf.acknowledge(), std::chrono::milliseconds{20});
    ASSERT_TRUE(gate.has_value()) << gate.error();
    const auto enabled = gate->enable();
    ASSERT_FALSE(enabled.has_value());
    EXPECT_NE(enabled.error().find("did not acknowledge"), std::string::npos);
}

TEST(CounterGate, MissingFifosAreAnError) {
    const test::ScratchDir scratch;
    EXPECT_FALSE(CounterGate::open(scratch / "none", scratch / "neither").has_value());
}

TEST(CounterGate, WithoutFifosItIsInertAndSaysWhy) {
    auto gate = CounterGate::for_run(std::nullopt, std::nullopt);
    ASSERT_TRUE(gate.has_value());
    EXPECT_NE(gate->status(), CounterStatus::gated);
    EXPECT_TRUE(gate->enable().has_value());
    EXPECT_TRUE(gate->disable().has_value());
}

class StreamCheckTest : public ::testing::Test {
protected:
    test::Bytes source = test::source_records();
    std::uint64_t messages = test::kSourceRecordCount;
    std::vector<std::byte> scratch = std::vector<std::byte>(source.size());
    std::string pin = harness::to_hex(harness::sha256(source));
};

TEST_F(StreamCheckTest, APinnedStreamPasses) {
    const auto check = verify_round_trip(source, pin, messages, scratch);
    ASSERT_TRUE(check.has_value()) << check.error();
    EXPECT_EQ(check->messages, messages);
    EXPECT_EQ(check->bytes, source.size());
    EXPECT_EQ(check->input_sha256, pin);
    EXPECT_EQ(check->reencoded_sha256, pin);
}

TEST_F(StreamCheckTest, AWrongPinStopsIt) {
    pin[0] = pin[0] == '0' ? '1' : '0';
    const auto check = verify_round_trip(source, pin, messages, scratch);
    ASSERT_FALSE(check.has_value());
    EXPECT_NE(check.error().find("does not match the pin"), std::string::npos);
}

TEST_F(StreamCheckTest, OneFlippedBitStopsIt) {
    source[source.size() / 2] ^= std::byte{0x01};
    EXPECT_FALSE(verify_round_trip(source, pin, messages, scratch).has_value());
}

TEST_F(StreamCheckTest, AWrongCountStopsIt) {
    const auto check = verify_round_trip(source, pin, messages + 1, scratch);
    ASSERT_FALSE(check.has_value());
    EXPECT_NE(check.error().find("messages"), std::string::npos);
}

TEST_F(StreamCheckTest, AScratchBufferTooSmallStopsIt) {
    scratch.resize(source.size() / 2);
    EXPECT_FALSE(verify_round_trip(source, pin, messages, scratch).has_value());
}

TEST_F(StreamCheckTest, ATruncatedTailStopsIt) {
    const test::Bytes tail = test::truncated_tail();
    source.insert(source.end(), tail.begin(), tail.end());
    scratch.resize(source.size());
    const std::string tailed_pin = harness::to_hex(harness::sha256(source));
    const auto check = verify_round_trip(source, tailed_pin, messages, scratch);
    ASSERT_FALSE(check.has_value());
    EXPECT_NE(check.error().find("does not end cleanly"), std::string::npos);
}

TEST(BuildInfo, NamesTheCompilerAndCarriesNoPath) {
    const BuildInfo& info = build_info();
    EXPECT_FALSE(info.compiler_id.empty());
    EXPECT_FALSE(info.compiler_version.empty());
    EXPECT_FALSE(info.commit.empty());
    EXPECT_FALSE(info.isa.empty());
    EXPECT_EQ(info.compile_flags.find('/'), std::string_view::npos);
    EXPECT_EQ(info.compile_flags.find("-I"), std::string_view::npos);
}

}  // namespace
}  // namespace tick_store::bench_support
