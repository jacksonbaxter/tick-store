#include "tick_store/bench_support/timer.hpp"

#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime and its clock ids are POSIX

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::bench_support {

namespace {

// Long enough to cross many timer steps at 4 ns a read, and short enough to fit
// between interrupts most of the time.
constexpr std::size_t kCostSamples = 1U << 16U;
constexpr int kCostRounds = 8;
constexpr int kBracketTries = 16;

[[nodiscard]] std::int64_t raw_clock_ns() noexcept {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return (static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000) + now.tv_nsec;
}

struct ClockPair {
    std::uint64_t ticks = 0;
    std::int64_t raw_ns = 0;
};

// Takes the tick reading with the tightest raw-clock bracket, so an interrupt during
// one try cannot bias the rate.
[[nodiscard]] ClockPair read_pair() noexcept {
    ClockPair best;
    std::int64_t tightest = std::numeric_limits<std::int64_t>::max();
    for (int i = 0; i < kBracketTries; ++i) {
        const std::int64_t before = raw_clock_ns();
        const std::uint64_t ticks = read_ticks();
        const std::int64_t after = raw_clock_ns();
        if (after - before < tightest) {
            tightest = after - before;
            best = {.ticks = ticks, .raw_ns = before + ((after - before) / 2)};
        }
    }
    return best;
}

}  // namespace

std::string_view timer_source() noexcept {
#if defined(__x86_64__)
    return "rdtscp+lfence";
#elif defined(__APPLE__)
    return "mach_absolute_time";
#else
    return "cntvct_el0";
#endif
}

std::uint64_t resolution_from_deltas(std::span<const std::uint64_t> deltas) noexcept {
    std::uint64_t step = 0;
    for (const std::uint64_t delta : deltas) {
        step = std::gcd(step, delta);
    }
    return step;
}

std::expected<TimerInfo, std::string> calibrate_timer(std::chrono::milliseconds window) {
    const ClockPair start = read_pair();
    const std::int64_t deadline =
        start.raw_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(window).count();
    while (raw_clock_ns() < deadline) {
    }
    const ClockPair end = read_pair();
    if (end.ticks <= start.ticks || end.raw_ns <= start.raw_ns) {
        return std::unexpected(std::string{"the timer did not advance during calibration"});
    }
    const auto window_ns = static_cast<double>(end.raw_ns - start.raw_ns);
    const double ticks_per_second = static_cast<double>(end.ticks - start.ticks) * 1e9 / window_ns;

    std::vector<std::uint64_t> readings(kCostSamples);
    std::vector<std::uint64_t> deltas(kCostSamples - 1);
    std::uint64_t step = 0;
    auto fewest_ticks = std::numeric_limits<std::uint64_t>::max();
    for (int round = 0; round < kCostRounds; ++round) {
        for (std::uint64_t& reading : readings) {
            reading = read_ticks();
        }
        std::adjacent_difference(readings.begin(), readings.end(), readings.begin());
        std::copy(readings.begin() + 1, readings.end(), deltas.begin());
        step = std::gcd(step, resolution_from_deltas(deltas));
        // The round least disturbed by interrupts gives the cost of a read.
        std::uint64_t round_ticks = 0;
        for (const std::uint64_t delta : deltas) {
            round_ticks += delta;
        }
        fewest_ticks = std::min(fewest_ticks, round_ticks);
    }
    if (step == 0) {
        return std::unexpected(std::string{"the timer never moved between reads"});
    }

    TimerInfo info{.source = timer_source(),
                   .ticks_per_second = ticks_per_second,
                   .resolution_ticks = step,
                   .resolution_ns = 0.0,
                   .read_cost_ns = 0.0,
                   .window_ns = window_ns};
    info.resolution_ns = info.to_ns(step);
    info.read_cost_ns = info.to_ns(fewest_ticks) / static_cast<double>(deltas.size());
    return info;
}

}  // namespace tick_store::bench_support
