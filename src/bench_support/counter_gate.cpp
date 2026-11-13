#include "tick_store/bench_support/counter_gate.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tick_store::bench_support {

namespace {

#ifdef __linux__
constexpr CounterStatus kWithoutFifos = CounterStatus::not_requested;
constexpr bool kPlatformHasPerf = true;
#else
constexpr CounterStatus kWithoutFifos = CounterStatus::not_supported;
constexpr bool kPlatformHasPerf = false;
#endif

// perf opens both FIFOs read-write as well. A plain O_WRONLY or O_RDONLY open would
// block until the other side appeared, and a dead perf must fail here, not hang.
[[nodiscard]] int open_fifo(const std::filesystem::path& path) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic
    return ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

[[nodiscard]] std::string last_error() {
    return std::generic_category().message(errno);
}

}  // namespace

std::string_view to_string(CounterStatus status) noexcept {
    switch (status) {
        case CounterStatus::gated:
            return "gated";
        case CounterStatus::not_supported:
            return "not-supported";
        case CounterStatus::not_requested:
            return "not-requested";
    }
    return "not-supported";
}

std::expected<CounterGate, std::string> CounterGate::for_run(
    const std::optional<std::filesystem::path>& control,
    const std::optional<std::filesystem::path>& acknowledge) {
    // One without the other is a mistyped command line under perf, whose counters would
    // then never be enabled, not a run without perf (review A-6).
    if (control.has_value() != acknowledge.has_value()) {
        return std::unexpected(std::string{
            "the perf control FIFOs come as a pair, and only one was given"});
    }
    if (!kPlatformHasPerf || !control.has_value()) {
        return CounterGate{kWithoutFifos};
    }
    return open(*control, *acknowledge);
}

std::expected<CounterGate, std::string> CounterGate::open(
    const std::filesystem::path& control, const std::filesystem::path& acknowledge,
    std::chrono::milliseconds ack_timeout) {
    CounterGate gate{CounterStatus::gated};
    gate.ack_timeout_ = ack_timeout;
    gate.control_fd_ = open_fifo(control);
    gate.acknowledge_fd_ = open_fifo(acknowledge);
    if (gate.control_fd_ < 0 || gate.acknowledge_fd_ < 0) {
        return std::unexpected(std::format("cannot open the perf control FIFOs: {}", last_error()));
    }
    return gate;
}

CounterGate::CounterGate(CounterGate&& other) noexcept
    : status_(other.status_),
      control_fd_(std::exchange(other.control_fd_, -1)),
      acknowledge_fd_(std::exchange(other.acknowledge_fd_, -1)),
      ack_timeout_(other.ack_timeout_) {}

CounterGate& CounterGate::operator=(CounterGate&& other) noexcept {
    if (this != &other) {
        close_files();
        status_ = other.status_;
        control_fd_ = std::exchange(other.control_fd_, -1);
        acknowledge_fd_ = std::exchange(other.acknowledge_fd_, -1);
        ack_timeout_ = other.ack_timeout_;
    }
    return *this;
}

CounterGate::~CounterGate() { close_files(); }

void CounterGate::close_files() noexcept {
    if (control_fd_ >= 0) {
        ::close(control_fd_);
        control_fd_ = -1;
    }
    if (acknowledge_fd_ >= 0) {
        ::close(acknowledge_fd_);
        acknowledge_fd_ = -1;
    }
}

std::expected<void, std::string> CounterGate::enable() { return command("enable\n"); }

std::expected<void, std::string> CounterGate::disable() { return command("disable\n"); }

std::expected<void, std::string> CounterGate::command(std::string_view text) {
    if (status_ != CounterStatus::gated) {
        return {};
    }
    if (::write(control_fd_, text.data(), text.size()) != std::ssize(text)) {
        return std::unexpected(
            std::format("cannot write to the perf control FIFO: {}", last_error()));
    }
    // A signal ends poll() early with EINTR; the wait goes on for the time left (A-6).
    const auto deadline = std::chrono::steady_clock::now() + ack_timeout_;
    while (true) {
        const auto left = std::max(std::chrono::ceil<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now()),
                                   std::chrono::milliseconds{0});
        pollfd waiting{.fd = acknowledge_fd_, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&waiting, 1, static_cast<int>(left.count()));
        if (ready > 0) {
            break;
        }
        if (ready == 0) {
            return std::unexpected(
                std::format("perf did not acknowledge within {} ms", ack_timeout_.count()));
        }
        if (errno != EINTR) {
            return std::unexpected(
                std::format("waiting for perf's acknowledgement failed: {}", last_error()));
        }
    }
    std::array<char, 16> reply{};
    const auto got = ::read(acknowledge_fd_, reply.data(), reply.size());
    if (got < 3 || !std::string_view{reply.data(), static_cast<std::size_t>(got)}.starts_with("ack")) {
        return std::unexpected(std::string{"perf sent something other than an acknowledgement"});
    }
    return {};
}

}  // namespace tick_store::bench_support
