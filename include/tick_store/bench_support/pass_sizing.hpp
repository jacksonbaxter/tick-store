#pragma once

// How long a kernel benchmark's pass is (DS-4, docs/NOTES.md): whole rounds of a fixed
// number of calls, enough to last at least a set time at the speed of the fastest of the
// rounds timed once per case before its warm-up; the timed passes can run a few percent shorter.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace tick_store::bench_support {

// Calls per round: about 8.4 M values of the kernel bench's vector, 1 to 20 ms at any rate.
inline constexpr std::size_t kKernelRoundCalls = 8192;
// The least a pass lasts at the speed it is sized at, so a kernel case lasts about 10 s.
inline constexpr std::chrono::milliseconds kKernelMinPassTime{150};
// Timed rounds a pass is sized from.
inline constexpr int kKernelSizingRounds = 3;

// The fewest whole rounds of `round_calls` calls that last at least `least_ns` at
// `round_ns` a round, and never fewer than one. A round measured at 0 ns or less counts as
// 1 ns.
[[nodiscard]] constexpr std::size_t calls_per_pass(std::int64_t round_ns, std::int64_t least_ns,
                                                   std::size_t round_calls) noexcept {
    const std::int64_t one = std::max<std::int64_t>(round_ns, 1);
    const std::int64_t rounds = std::max<std::int64_t>((least_ns + one - 1) / one, 1);
    return round_calls * static_cast<std::size_t>(rounds);
}

// A kernel case's calls per pass, from the fastest of kKernelSizingRounds timed rounds:
// `time_one_round` runs one round of kKernelRoundCalls calls and returns its nanoseconds. A
// round cannot be measured faster than it runs, so the fastest is the one least slowed by
// anything else on the host.
template <typename TimeOneRound>
[[nodiscard]] std::size_t size_kernel_pass(const TimeOneRound& time_one_round) {
    std::int64_t fastest = std::numeric_limits<std::int64_t>::max();
    for (int round = 0; round < kKernelSizingRounds; ++round) {
        fastest = std::min<std::int64_t>(fastest, time_one_round());
    }
    return calls_per_pass(fastest, std::chrono::nanoseconds{kKernelMinPassTime}.count(),
                          kKernelRoundCalls);
}

}  // namespace tick_store::bench_support
