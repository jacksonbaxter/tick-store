#pragma once

// The benchmark timer. read_ticks() is one call on every supported target, and the
// platform choice stays in this file.
//
//   x86-64        rdtscp then lfence, the pattern debian-host.md settled on. Ticks are
//                 TSC ticks at the fixed P0 rate: wall time, not core cycles. A perf
//                 `cycles` count and a tick count are different units and are never
//                 compared directly, because a boosted core runs more cycles per tick.
//   Apple arm64   mach_absolute_time. Raw cntvct_el0 is no finer: on the M4 Max it
//                 counts in 1 GHz units but moves in steps of about 42, with occasional
//                 smaller steps, so its unit hides its real resolution. mach ticks are
//                 the real 24 MHz steps.
//   Other arm64   cntvct_el0, between instruction barriers.
//
// Never assume a rate or a resolution: calibrate_timer() measures both, on every run.

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#if defined(__x86_64__)
#include <x86intrin.h>
#elif defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach_time.h>
#elif !defined(__aarch64__)
#error "tick-store benchmarks support x86-64 and arm64"
#endif

namespace tick_store::bench_support {

[[nodiscard]] inline std::uint64_t read_ticks() noexcept {
#if defined(__x86_64__)
    unsigned int processor = 0;
    const std::uint64_t ticks = __rdtscp(&processor);
    _mm_lfence();
    return ticks;
#elif defined(__APPLE__)
    return mach_absolute_time();
#else
    std::uint64_t ticks = 0;
    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(ticks));
    return ticks;
#endif
}

// The name of what read_ticks() reads.
[[nodiscard]] std::string_view timer_source() noexcept;

struct TimerInfo {
    std::string_view source;
    // read_ticks() rate, measured against CLOCK_MONOTONIC_RAW.
    double ticks_per_second = 0.0;
    // How far apart two different readings can be: the step, not the unit.
    std::uint64_t resolution_ticks = 0;
    double resolution_ns = 0.0;
    // Mean cost of one read_ticks().
    double read_cost_ns = 0.0;
    // Length of the calibration window.
    double window_ns = 0.0;

    [[nodiscard]] double to_ns(std::uint64_t ticks) const noexcept {
        return static_cast<double>(ticks) * 1e9 / ticks_per_second;
    }
};

// The step of a counter, from differences between consecutive readings: the greatest
// common divisor of the non-zero ones. The smallest difference would not do, because
// a read that costs two steps never shows one. Returns 0 if none is non-zero.
[[nodiscard]] std::uint64_t resolution_from_deltas(std::span<const std::uint64_t> deltas) noexcept;

// Rate of the timer over `window`, spinning on this thread, then its step and cost.
// Fails if the timer did not advance.
[[nodiscard]] std::expected<TimerInfo, std::string> calibrate_timer(
    std::chrono::milliseconds window);

}  // namespace tick_store::bench_support
