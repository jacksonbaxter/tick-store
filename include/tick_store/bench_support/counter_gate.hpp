#pragma once

// Limits hardware counters to the measured loop.
//
// `perf stat` counts a whole process, which here includes hashing the input twice and
// the warm-up passes. So run_bench.py starts perf with its events disabled and a pair
// of control FIFOs, and this class turns them on and off around the timed region. Each
// command waits for perf's acknowledgement, so the region is covered before it starts.
//
// Hardware counters come only from `perf stat`, which exists only on Linux. Elsewhere,
// or without the FIFOs, the gate is inert and status() says why, so callers do not
// branch on the platform.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tick_store::bench_support {

enum class CounterStatus : std::uint8_t {
    // Commands go to a live perf.
    gated,
    // This platform has no perf.
    not_supported,
    // The platform has perf, but this run was not started under it.
    not_requested,
};

[[nodiscard]] std::string_view to_string(CounterStatus status) noexcept;

class CounterGate {
public:
    static constexpr std::chrono::milliseconds kDefaultAckTimeout{5000};

    // What this run should use: FIFOs where given, otherwise an inert gate. On a
    // platform without perf, the status is not_supported even if FIFOs are given. One
    // FIFO without the other is an error.
    [[nodiscard]] static std::expected<CounterGate, std::string> for_run(
        const std::optional<std::filesystem::path>& control,
        const std::optional<std::filesystem::path>& acknowledge);

    // A gate on these FIFOs, whatever the platform.
    [[nodiscard]] static std::expected<CounterGate, std::string> open(
        const std::filesystem::path& control, const std::filesystem::path& acknowledge,
        std::chrono::milliseconds ack_timeout = kDefaultAckTimeout);

    CounterGate(const CounterGate&) = delete;
    CounterGate& operator=(const CounterGate&) = delete;
    CounterGate(CounterGate&& other) noexcept;
    CounterGate& operator=(CounterGate&& other) noexcept;
    ~CounterGate();

    [[nodiscard]] CounterStatus status() const noexcept { return status_; }

    // Both succeed without doing anything on an inert gate.
    [[nodiscard]] std::expected<void, std::string> enable();
    [[nodiscard]] std::expected<void, std::string> disable();

private:
    explicit CounterGate(CounterStatus status) noexcept : status_(status) {}

    [[nodiscard]] std::expected<void, std::string> command(std::string_view text);
    void close_files() noexcept;

    CounterStatus status_;
    int control_fd_ = -1;
    int acknowledge_fd_ = -1;
    std::chrono::milliseconds ack_timeout_ = kDefaultAckTimeout;
};

}  // namespace tick_store::bench_support
