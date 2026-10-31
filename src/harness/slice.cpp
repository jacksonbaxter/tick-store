#include "tick_store/harness/slice.hpp"

#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace tick_store::harness {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;

// The timestamp of a typed message. A raw-path record has none.
[[nodiscard]] std::optional<std::uint64_t> timestamp_of(const itch::Message& message) noexcept {
    return std::visit(
        []<class M>(const M& typed) -> std::optional<std::uint64_t> {
            if constexpr (std::same_as<M, itch::RawMessage>) {
                return std::nullopt;
            } else {
                return typed.header.timestamp_ns;
            }
        },
        message);
}

[[nodiscard]] std::optional<unsigned> two_digits(std::string_view text) noexcept {
    unsigned value = 0;
    const char* const end = text.data() + text.size();
    const auto [stopped, status] = std::from_chars(text.data(), end, value);
    if (text.size() != 2 || status != std::errc{} || stopped != end) {
        return std::nullopt;
    }
    return value;
}

enum class Phase : std::uint8_t { before, inside, after };

// Decides, record by record, what goes in the slice.
class Selector {
public:
    explicit Selector(SliceWindow window) noexcept : window_(window) {}

    // True if the record at `frame` belongs in the slice.
    [[nodiscard]] bool take(const itch::Frame& frame, const itch::Message& message) noexcept {
        const std::optional<std::uint64_t> timestamp = timestamp_of(message);
        if (timestamp.has_value()) {
            advance(*timestamp, frame.offset);
        }
        if (phase_ == Phase::inside) {
            ++report_.window_messages;
            if (timestamp.has_value() && !in_range(*timestamp)) {
                ++report_.in_window_outside_range;
            }
            return true;
        }
        if (timestamp.has_value() && in_range(*timestamp)) {
            ++report_.in_range_outside_window;
        }
        if (std::holds_alternative<itch::SystemEvent>(message)) {
            ++report_.system_messages_outside;
            return true;
        }
        if (std::holds_alternative<itch::StockDirectory>(message)) {
            ++report_.directory_messages_outside;
            return true;
        }
        if (!timestamp.has_value()) {
            ++report_.raw_outside_window;
        }
        return false;
    }

    // The file ended at `end`. Call once, after the last record.
    [[nodiscard]] SliceReport finish(std::uint64_t end) noexcept {
        if (phase_ == Phase::before) {
            report_.window_first_offset = end;
        }
        if (phase_ != Phase::after) {
            report_.window_end_offset = end;
        }
        return report_;
    }

private:
    void advance(std::uint64_t timestamp, std::uint64_t offset) noexcept {
        if (phase_ == Phase::before && timestamp >= window_.from_ns) {
            phase_ = Phase::inside;
            report_.window_first_offset = offset;
        }
        if (phase_ == Phase::inside && timestamp >= window_.to_ns) {
            phase_ = Phase::after;
            report_.window_end_offset = offset;
        }
    }

    [[nodiscard]] bool in_range(std::uint64_t timestamp) const noexcept {
        return timestamp >= window_.from_ns && timestamp < window_.to_ns;
    }

    SliceWindow window_;
    Phase phase_ = Phase::before;
    SliceReport report_;
};

}  // namespace

std::optional<std::uint64_t> parse_time_of_day(std::string_view text) noexcept {
    constexpr std::size_t kLength = 8;  // "HH:MM:SS"
    if (text.size() != kLength || text[2] != ':' || text[5] != ':') {
        return std::nullopt;
    }
    const auto hours = two_digits(text.substr(0, 2));
    const auto minutes = two_digits(text.substr(3, 2));
    const auto seconds = two_digits(text.substr(6, 2));
    if (!hours || !minutes || !seconds || *hours > 23 || *minutes > 59 || *seconds > 59) {
        return std::nullopt;
    }
    const std::uint64_t whole =
        std::uint64_t{*hours} * 3600U + std::uint64_t{*minutes} * 60U + *seconds;
    return whole * kNanosecondsPerSecond;
}

std::expected<SliceReport, std::string> write_slice(std::span<const std::byte> source,
                                                    const std::filesystem::path& path,
                                                    SliceWindow window) {
    if (window.from_ns >= window.to_ns) {
        return std::unexpected("the window is empty");
    }
    std::filesystem::path partial = path;
    partial += ".partial";

    std::ofstream out{partial, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::unexpected("cannot create the slice");
    }

    Selector selector{window};
    itch::FrameCursor cursor{source};
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        // The cursor returns no empty payload, and only that fails to decode.
        const auto message = itch::decode(frame->payload);
        if (!message.has_value() || !selector.take(*frame, *message)) {
            continue;
        }
        const auto whole =
            source.subspan(frame->offset, itch::kFramePrefixSize + frame->payload.size());
        const auto chars = std::span{
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): writing bytes to a stream.
            reinterpret_cast<const char*>(whole.data()), whole.size()};
        out.write(chars.data(), static_cast<std::streamsize>(chars.size()));
        ++messages;
        bytes += whole.size();
    }

    SliceReport report = selector.finish(cursor.offset());
    report.messages = messages;
    report.bytes = bytes;

    out.close();
    std::error_code error;
    if (out.fail()) {
        std::filesystem::remove(partial, error);
        return std::unexpected("cannot write the slice");
    }
    std::filesystem::rename(partial, path, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return std::unexpected("cannot publish the slice");
    }
    return report;
}

std::expected<SliceCheck, std::string> check_slice(std::span<const std::byte> slice) {
    SliceCheck check;
    itch::FrameCursor cursor{slice};
    auto frame = cursor.next();
    for (; frame.has_value(); frame = cursor.next()) {
        const auto message = itch::decode_strict(frame->payload);
        if (!message.has_value()) {
            return std::unexpected(std::format("record {} at byte {}: {}", check.messages,
                                               frame->offset, to_string(message.error())));
        }
        ++check.messages;
        check.system_messages += std::holds_alternative<itch::SystemEvent>(*message) ? 1U : 0U;
        check.directory_messages += std::holds_alternative<itch::StockDirectory>(*message) ? 1U : 0U;
    }
    if (frame.error().kind != itch::FrameStopKind::end) {
        return std::unexpected(std::format("the frames stop early: {}", to_string(frame.error())));
    }
    check.bytes = slice.size();
    check.sha256 = sha256(slice);
    return check;
}

}  // namespace tick_store::harness
