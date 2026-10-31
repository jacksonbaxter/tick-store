#include "tick_store/bench_support/latency_recorder.hpp"

#include "tick_store/bench_support/resolution_guard.hpp"
#include "tick_store/bench_support/timer.hpp"

#include <hdr/hdr_histogram.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <string>

namespace tick_store::bench_support {

namespace {

constexpr std::array<double, 5> kPercentiles{50.0, 90.0, 99.0, 99.9, 99.99};

}  // namespace

void BatchLatencyRecorder::Close::operator()(hdr_histogram* histogram) const noexcept {
    hdr_close(histogram);
}

std::expected<BatchLatencyRecorder, std::string> BatchLatencyRecorder::create(
    std::uint64_t highest_ticks) {
    hdr_histogram* histogram = nullptr;
    const int status = hdr_init(1, static_cast<std::int64_t>(highest_ticks),
                                kHistogramSignificantFigures, &histogram);
    if (status != 0) {
        return std::unexpected(std::format("hdr_init failed with status {}", status));
    }
    return BatchLatencyRecorder{histogram};
}

void BatchLatencyRecorder::record(std::uint64_t ticks) noexcept {
    if (!hdr_record_value(histogram_.get(), static_cast<std::int64_t>(ticks))) {
        ++out_of_range_;
    }
}

std::expected<LatencyReport, std::string> BatchLatencyRecorder::report(
    const TimerInfo& timer, std::uint64_t batch_messages) const {
    if (out_of_range_ != 0) {
        return std::unexpected(
            std::format("{} batches fell outside the histogram's range", out_of_range_));
    }
    const hdr_histogram* histogram = histogram_.get();
    if (histogram->total_count == 0) {
        return std::unexpected(std::string{"no batches were recorded"});
    }

    const ResolutionGuard guard{timer.resolution_ns, timer.read_cost_ns};
    const auto publish = [&](std::int64_t ticks, const std::string& what) {
        return guard.publish(timer.to_ns(static_cast<std::uint64_t>(ticks)), what);
    };

    LatencyReport report{.unit = "ns per batch",
                         .batch_messages = batch_messages,
                         .batches = static_cast<std::uint64_t>(histogram->total_count),
                         .timer_resolution_ns = timer.resolution_ns,
                         .floor_ns = guard.floor_ns(),
                         .significant_figures = kHistogramSignificantFigures,
                         .min_ns = 0.0,
                         .max_ns = 0.0,
                         .percentiles = {}};
    const auto min_ns = publish(hdr_min(histogram), "the fastest batch");
    if (!min_ns.has_value()) {
        return std::unexpected(min_ns.error());
    }
    report.min_ns = *min_ns;
    for (const double percentile : kPercentiles) {
        const auto value = publish(hdr_value_at_percentile(histogram, percentile),
                                   std::format("p{}", percentile));
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        report.percentiles.push_back({.percentile = percentile, .value_ns = *value});
    }
    report.max_ns = timer.to_ns(static_cast<std::uint64_t>(hdr_max(histogram)));
    return report;
}

}  // namespace tick_store::bench_support
