#include "tick_store/bench_support/affinity.hpp"

#include <pthread.h>
#include <sched.h>

#include <format>
#include <optional>
#include <system_error>

namespace tick_store::bench_support {

PinResult pin_current_thread(int cpu) {
    PinResult result{
        .status = PinStatus::failed, .requested_cpu = cpu, .observed_cpu = std::nullopt, .detail = {}};
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        result.detail = std::format("cpu {} is out of range", cpu);
        return result;
    }
    cpu_set_t wanted;
    CPU_ZERO(&wanted);
    CPU_SET(static_cast<unsigned>(cpu), &wanted);
    if (const int error = ::pthread_setaffinity_np(::pthread_self(), sizeof(wanted), &wanted);
        error != 0) {
        result.detail = std::format("pthread_setaffinity_np: {}", std::generic_category().message(error));
        return result;
    }
    // The call moves the thread before it returns, so this reads the new CPU.
    result.observed_cpu = current_cpu();
    if (result.observed_cpu != cpu) {
        result.detail = std::format("sched_getcpu reports cpu {} after pinning to cpu {}",
                                    result.observed_cpu.value_or(-1), cpu);
        return result;
    }
    result.status = PinStatus::pinned;
    return result;
}

PinResult leave_unpinned() {
    return {.status = PinStatus::not_requested,
            .requested_cpu = -1,
            .observed_cpu = current_cpu(),
            .detail = "no CPU was requested"};
}

std::optional<int> current_cpu() noexcept {
    const int cpu = ::sched_getcpu();
    if (cpu < 0) {
        return std::nullopt;
    }
    return cpu;
}

}  // namespace tick_store::bench_support
