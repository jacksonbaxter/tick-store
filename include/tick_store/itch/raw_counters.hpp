#pragma once

// Counts how many messages took the raw path, by reason. decode() stays a pure
// function, so a walker keeps one of these and calls observe() on each message.

#include "tick_store/itch/message.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace tick_store::itch {

class RawPathCounters {
public:
    constexpr void record(RawReason reason) noexcept { ++counts_[index(reason)]; }

    // Counts `message` if it is a RawMessage, and otherwise does nothing.
    constexpr void observe(const Message& message) noexcept {
        if (const auto* raw = std::get_if<RawMessage>(&message)) [[unlikely]] {
            record(raw->reason);
        }
    }

    [[nodiscard]] constexpr std::uint64_t count(RawReason reason) const noexcept {
        return counts_[index(reason)];
    }

    [[nodiscard]] constexpr std::uint64_t total() const noexcept {
        std::uint64_t sum = 0;
        for (const std::uint64_t count : counts_) {
            sum += count;
        }
        return sum;
    }

private:
    [[nodiscard]] static constexpr std::size_t index(RawReason reason) noexcept {
        return static_cast<std::size_t>(reason);
    }

    std::array<std::uint64_t, kRawReasonCount> counts_{};
};

}  // namespace tick_store::itch
