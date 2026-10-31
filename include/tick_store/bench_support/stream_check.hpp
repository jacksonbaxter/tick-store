#pragma once

// Verify before measuring (B2): the input must be the pinned stream, and decoding then
// re-encoding it must rebuild that stream exactly. Until this passes, no time counts.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::bench_support {

struct StreamCheck {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::string input_sha256;
    std::string reencoded_sha256;
};

// Checks `source` against the pin, then re-encodes it into `scratch`, which needs to be
// as large as `source`, and checks that against the same pin. Any failure is an error
// naming what differed.
[[nodiscard]] std::expected<StreamCheck, std::string> verify_round_trip(
    std::span<const std::byte> source, std::string_view expected_sha256,
    std::uint64_t expected_messages, std::span<std::byte> scratch);

}  // namespace tick_store::bench_support
