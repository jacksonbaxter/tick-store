#pragma once

// Pins the calling thread to one CPU, and proves it. Nothing above this file knows
// which platform it is on: where pinning does not exist the call reports that, and
// the caller records it the same way as any other outcome.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tick_store::bench_support {

enum class PinStatus : std::uint8_t {
    // The affinity call succeeded and the thread was then seen on the requested CPU.
    pinned,
    // This platform has no way to pin a thread.
    not_supported,
    // The platform can pin, and this run did not ask to.
    not_requested,
    // The call failed, or the thread was seen somewhere else afterwards.
    failed,
};

[[nodiscard]] constexpr std::string_view to_string(PinStatus status) noexcept {
    switch (status) {
        case PinStatus::pinned:
            return "pinned";
        case PinStatus::not_supported:
            return "not-supported";
        case PinStatus::not_requested:
            return "not-requested";
        case PinStatus::failed:
            return "failed";
    }
    return "failed";
}

struct PinResult {
    PinStatus status = PinStatus::failed;
    int requested_cpu = -1;
    // Where the thread was seen after the call, if the platform can tell.
    std::optional<int> observed_cpu;
    std::string detail;
};

[[nodiscard]] PinResult pin_current_thread(int cpu);

// The outcome for a run that asks for no CPU, so that a results file can tell "chose
// not to" from "cannot".
[[nodiscard]] PinResult leave_unpinned();

// The CPU this thread is on now, if the platform can tell.
[[nodiscard]] std::optional<int> current_cpu() noexcept;

}  // namespace tick_store::bench_support
