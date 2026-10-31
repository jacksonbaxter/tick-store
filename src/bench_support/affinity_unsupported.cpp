// Built where no thread affinity exists. On macOS, THREAD_AFFINITY_POLICY is rejected
// with KERN_NOT_SUPPORTED and QoS hints do not choose a core (machine-probe.md Q4).

#include "tick_store/bench_support/affinity.hpp"

#include <optional>

namespace tick_store::bench_support {

PinResult pin_current_thread(int cpu) {
    return {.status = PinStatus::not_supported,
            .requested_cpu = cpu,
            .observed_cpu = std::nullopt,
            .detail = "this platform has no thread affinity"};
}

PinResult leave_unpinned() { return pin_current_thread(-1); }

std::optional<int> current_cpu() noexcept { return std::nullopt; }

}  // namespace tick_store::bench_support
