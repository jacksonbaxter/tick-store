#pragma once

// Refuses to publish a latency the timer cannot resolve.
//
// A value is publishable only if it is at least 10 timer steps, so one step of error
// is at most 10%, and at least 5 read costs, so the timer is not most of what was
// measured. On the debian host, a 10 ns step and a 19 ns read put the floor near
// 100 ns; on the Mac, a 41.667 ns step puts it near 417 ns. Anything below the floor
// has to be batched or averaged until it clears it.

#include <expected>
#include <string>

namespace tick_store::bench_support {

inline constexpr double kMinResolutionSteps = 10.0;
inline constexpr double kMinReadCosts = 5.0;

class ResolutionGuard {
public:
    ResolutionGuard(double resolution_ns, double read_cost_ns) noexcept;

    [[nodiscard]] double floor_ns() const noexcept { return floor_ns_; }

    // `value_ns` if it is publishable, otherwise why it is not. `what` names the value
    // in that message.
    [[nodiscard]] std::expected<double, std::string> publish(double value_ns,
                                                             const std::string& what) const;

private:
    double floor_ns_;
};

}  // namespace tick_store::bench_support
