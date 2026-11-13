#pragma once

// A distribution of batch times, kept in timer ticks in an HdrHistogram and turned
// into nanoseconds only when published. The report travels with what a reader needs
// to judge it: batch size, unit, timer step and the floor. Publishing goes through
// ResolutionGuard and is all or nothing: one value below the floor refuses the report,
// so nothing can quote the good half of a distribution the timer could not resolve.
// A percentile is published only when at least kMinBatchesAbovePercentile batches lie
// above it; otherwise it is left out with the reason, so a p99 of 10 batches, which
// would be the slowest of them, is never labelled a tail percentile (review A-3).

#include "tick_store/bench_support/timer.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct hdr_histogram;

namespace tick_store::bench_support {

// Fixed, so percentiles from different runs are comparable.
inline constexpr int kHistogramSignificantFigures = 3;

// The fewest batches that must lie above a percentile, (100 - p)% of them, to publish it:
// p99 needs 1,000 batches, p99.9 10,000 and p99.99 100,000.
inline constexpr std::uint64_t kMinBatchesAbovePercentile = 10;

struct PublishedPercentile {
    double percentile = 0.0;
    double value_ns = 0.0;
};

// A percentile the batch count cannot support, and why.
struct OmittedPercentile {
    double percentile = 0.0;
    std::string reason;
};

struct LatencyReport {
    std::string_view unit;
    std::uint64_t batch_messages = 0;
    std::uint64_t batches = 0;
    double timer_resolution_ns = 0.0;
    double floor_ns = 0.0;
    int significant_figures = kHistogramSignificantFigures;
    double min_ns = 0.0;
    double max_ns = 0.0;
    std::vector<PublishedPercentile> percentiles;
    std::vector<OmittedPercentile> omitted;
};

class BatchLatencyRecorder {
public:
    // `highest_ticks` is the largest batch time the histogram can hold.
    [[nodiscard]] static std::expected<BatchLatencyRecorder, std::string> create(
        std::uint64_t highest_ticks);

    // Records one batch. A value out of range is counted, and refuses the report.
    void record(std::uint64_t ticks) noexcept;

    // The report at p50, p90, p99, p99.9 and p99.99, each the batch count supports, with
    // min and max, or why not.
    [[nodiscard]] std::expected<LatencyReport, std::string> report(
        const TimerInfo& timer, std::uint64_t batch_messages) const;

private:
    struct Close {
        void operator()(hdr_histogram* histogram) const noexcept;
    };

    explicit BatchLatencyRecorder(hdr_histogram* histogram) noexcept : histogram_(histogram) {}

    std::unique_ptr<hdr_histogram, Close> histogram_;
    std::uint64_t out_of_range_ = 0;
};

}  // namespace tick_store::bench_support
