#include "tick_store/bench_support/stream_check.hpp"

#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::bench_support {

std::expected<StreamCheck, std::string> verify_round_trip(std::span<const std::byte> source,
                                                          std::string_view expected_sha256,
                                                          std::uint64_t expected_messages,
                                                          std::span<std::byte> scratch) {
    const std::string input_sha256 = harness::to_hex(harness::sha256(source));
    if (input_sha256 != expected_sha256) {
        return std::unexpected(std::format("the input's SHA-256 {} does not match the pin {}",
                                           input_sha256, expected_sha256));
    }

    itch::FrameCursor cursor{source};
    std::uint64_t messages = 0;
    std::size_t written = 0;
    auto frame = cursor.next();
    for (; frame.has_value(); frame = cursor.next()) {
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            return std::unexpected(
                std::format("message {}: {}", messages, to_string(message.error())));
        }
        const auto size = itch::encode_frame(*message, scratch.subspan(written));
        if (!size.has_value()) {
            return std::unexpected(
                std::format("message {}: {}", messages, to_string(size.error())));
        }
        written += *size;
        ++messages;
    }
    if (frame.error().kind != itch::FrameStopKind::end) {
        return std::unexpected(std::format("the input does not end cleanly: {}",
                                           to_string(frame.error())));
    }
    if (messages != expected_messages) {
        return std::unexpected(std::format("the input holds {} messages, and the pin says {}",
                                           messages, expected_messages));
    }

    const auto rebuilt = std::span<const std::byte>{scratch}.first(written);
    const std::string reencoded_sha256 = harness::to_hex(harness::sha256(rebuilt));
    if (reencoded_sha256 != expected_sha256) {
        return std::unexpected(std::format(
            "decode then encode gave SHA-256 {}, which does not match the pin {}",
            reencoded_sha256, expected_sha256));
    }
    return StreamCheck{.messages = messages,
                       .bytes = source.size(),
                       .input_sha256 = input_sha256,
                       .reencoded_sha256 = reencoded_sha256};
}

}  // namespace tick_store::bench_support
