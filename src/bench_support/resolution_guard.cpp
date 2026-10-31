#include "tick_store/bench_support/resolution_guard.hpp"

#include <algorithm>
#include <expected>
#include <format>
#include <string>

namespace tick_store::bench_support {

ResolutionGuard::ResolutionGuard(double resolution_ns, double read_cost_ns) noexcept
    : floor_ns_(std::max(kMinResolutionSteps * resolution_ns, kMinReadCosts * read_cost_ns)) {}

std::expected<double, std::string> ResolutionGuard::publish(double value_ns,
                                                            const std::string& what) const {
    if (value_ns < floor_ns_) {
        return std::unexpected(
            std::format("refusing to publish {}: {:.3f} ns is below the {:.3f} ns the timer can "
                        "resolve; batch or average it",
                        what, value_ns, floor_ns_));
    }
    return value_ns;
}

}  // namespace tick_store::bench_support
