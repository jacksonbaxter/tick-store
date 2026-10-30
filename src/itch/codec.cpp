#include "tick_store/itch/codec.hpp"

#include "layout.hpp"
#include "tick_store/itch/big_endian.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

namespace tick_store::itch {
namespace {

using detail::kAllFields;

// Errors are built out of line so that the decode path stays small (D2).

[[gnu::cold, gnu::noinline]] DecodeError truncated(std::string_view field, std::size_t offset,
                                                   std::size_t width,
                                                   std::size_t payload_size) noexcept {
    // A field past the end of the payload has nothing left at all, so the
    // subtraction only happens when it cannot wrap.
    const std::size_t left = offset < payload_size ? payload_size - offset : 0;
    return {.kind = DecodeErrorKind::truncated,
            .field = field,
            .offset = offset,
            .expected = width,
            .actual = left};
}

[[gnu::cold, gnu::noinline]] DecodeError unknown_type(std::byte type) noexcept {
    return {.kind = DecodeErrorKind::unknown_type,
            .field = "message_type",
            .offset = 0,
            .expected = 0,
            .actual = std::to_integer<std::size_t>(type)};
}

[[gnu::cold, gnu::noinline]] DecodeError trailing_bytes(std::size_t wire_size,
                                                        std::size_t payload_size) noexcept {
    return {.kind = DecodeErrorKind::trailing_bytes,
            .field = "end of message",
            .offset = wire_size,
            .expected = 0,
            .actual = payload_size - wire_size};
}

[[gnu::cold, gnu::noinline]] EncodeError buffer_too_small(std::size_t needed,
                                                          std::size_t available) noexcept {
    return {.kind = EncodeErrorKind::buffer_too_small,
            .field = "out",
            .expected = needed,
            .actual = available};
}

[[gnu::cold, gnu::noinline]] EncodeError out_of_range(std::string_view field,
                                                      std::uint64_t largest,
                                                      std::uint64_t value) noexcept {
    return {.kind = EncodeErrorKind::value_out_of_range,
            .field = field,
            .expected = largest,
            .actual = value};
}

// Reads one field through the checked reader. The dispatch on value type lives
// here and in write_value, so a new field type has exactly two places to go.
template <class V, std::size_t Width>
[[nodiscard]] constexpr std::optional<V> read_value(const BigEndianReader& reader,
                                                    std::size_t offset) noexcept {
    if constexpr (std::is_same_v<V, std::byte>) {
        return reader.read_byte(offset);
    } else if constexpr (std::is_enum_v<V> || std::is_integral_v<V>) {
        // Width never exceeds sizeof(V), so the narrowing loses nothing.
        const auto value = reader.read_uint<Width>(offset);
        return value ? std::optional{static_cast<V>(*value)} : std::nullopt;
    } else {
        return reader.read_bytes<Width>(offset);
    }
}

template <class V, std::size_t Width>
[[nodiscard]] constexpr bool write_value(BigEndianWriter& writer, std::size_t offset,
                                         const V& value) noexcept {
    if constexpr (std::is_same_v<V, std::byte>) {
        return writer.write_byte(offset, value);
    } else if constexpr (std::is_enum_v<V> || std::is_integral_v<V>) {
        return writer.write_uint<Width>(offset, static_cast<std::uint64_t>(value));
    } else {
        return writer.write_bytes(offset, value);
    }
}

// Decodes as type T. The caller has matched the type byte, and every other byte
// is read through the checked reader, so a short payload fails at the first
// field that does not fit and a read of zeros from past the end is impossible.
template <class T>
[[nodiscard]] std::expected<Message, DecodeError> decode_as(
    std::span<const std::byte> payload) noexcept {
    const BigEndianReader reader{payload};
    T message{};
    DecodeError error;

    const auto load = [&]<class F>(const F& field) noexcept {
        const auto value = read_value<typename F::Value, F::width>(reader, field.offset);
        if (!value.has_value()) [[unlikely]] {
            error = truncated(field.name, field.offset, F::width, payload.size());
            return false;
        }
        F::of(message) = *value;
        return true;
    };

    const bool complete =
        std::apply([&](const auto&... fields) { return (load(fields) && ...); }, kAllFields<T>);
    if (!complete) [[unlikely]] {
        return std::unexpected(error);
    }
    if (payload.size() != T::wire_size) [[unlikely]] {
        return std::unexpected(trailing_bytes(T::wire_size, payload.size()));
    }
    return message;
}

// Names a message type as a value, so a lambda can take it as an argument.
template <class T>
inline constexpr std::type_identity<T> tag{};

// The one whitelist of type bytes (D11): calls `known` with the tag of the message
// type, and `unknown` for any other byte. Case labels are the structs' own type
// codes, so two types sharing a code cannot compile.
template <class Known, class Unknown>
[[nodiscard]] constexpr auto with_message_type(std::byte type, Known&& known,
                                               Unknown&& unknown) noexcept {
    switch (type) {
        case SystemEvent::type_code:
            return known(tag<SystemEvent>);
        case StockDirectory::type_code:
            return known(tag<StockDirectory>);
        case StockTradingAction::type_code:
            return known(tag<StockTradingAction>);
        case RegShoRestriction::type_code:
            return known(tag<RegShoRestriction>);
        case MarketParticipantPosition::type_code:
            return known(tag<MarketParticipantPosition>);
        case MwcbDeclineLevel::type_code:
            return known(tag<MwcbDeclineLevel>);
        case MwcbStatus::type_code:
            return known(tag<MwcbStatus>);
        case IpoQuotingPeriodUpdate::type_code:
            return known(tag<IpoQuotingPeriodUpdate>);
        case LuldAuctionCollar::type_code:
            return known(tag<LuldAuctionCollar>);
        case OperationalHalt::type_code:
            return known(tag<OperationalHalt>);
        case AddOrder::type_code:
            return known(tag<AddOrder>);
        case AddOrderWithMpid::type_code:
            return known(tag<AddOrderWithMpid>);
        case OrderExecuted::type_code:
            return known(tag<OrderExecuted>);
        case OrderExecutedWithPrice::type_code:
            return known(tag<OrderExecutedWithPrice>);
        case OrderCancel::type_code:
            return known(tag<OrderCancel>);
        case OrderDelete::type_code:
            return known(tag<OrderDelete>);
        case OrderReplace::type_code:
            return known(tag<OrderReplace>);
        case Trade::type_code:
            return known(tag<Trade>);
        case CrossTrade::type_code:
            return known(tag<CrossTrade>);
        case BrokenTrade::type_code:
            return known(tag<BrokenTrade>);
        case NetOrderImbalance::type_code:
            return known(tag<NetOrderImbalance>);
        case RetailInterest::type_code:
            return known(tag<RetailInterest>);
        case DirectListingPriceDiscovery::type_code:
            return known(tag<DirectListingPriceDiscovery>);
        default:
            return unknown();
    }
}

template <class T>
[[nodiscard]] std::expected<std::size_t, EncodeError> encode_as(
    const T& message, std::span<std::byte> out) noexcept {
    // Checked first because nothing later could catch it: the writer would keep
    // the low 6 bytes of the timestamp and the round trip would silently differ.
    if (message.header.timestamp_ns > kMaxTimestampNs) [[unlikely]] {
        return std::unexpected(
            out_of_range("timestamp_ns", kMaxTimestampNs, message.header.timestamp_ns));
    }

    BigEndianWriter writer{out};
    const auto store = [&]<class F>(const F& field) noexcept {
        return write_value<typename F::Value, F::width>(writer, field.offset, F::of(message));
    };
    const bool written =
        writer.write_byte(0, T::type_code) &&
        std::apply([&](const auto&... fields) { return (store(fields) && ...); }, kAllFields<T>);
    if (!written) [[unlikely]] {
        return std::unexpected(buffer_too_small(T::wire_size, out.size()));
    }
    return T::wire_size;
}

[[nodiscard]] std::expected<std::size_t, EncodeError> encode_as(
    const RawMessage& message, std::span<std::byte> out) noexcept {
    if (BigEndianWriter writer{out}; !writer.write_bytes(0, message.bytes)) [[unlikely]] {
        return std::unexpected(buffer_too_small(message.bytes.size(), out.size()));
    }
    return message.bytes.size();
}

}  // namespace

std::string to_string(const DecodeError& error) {
    switch (error.kind) {
        case DecodeErrorKind::truncated:
            return std::format("truncated: field {} at offset {}: expected {} bytes, have {}",
                               error.field, error.offset, error.expected, error.actual);
        case DecodeErrorKind::unknown_type:
            return std::format("unknown type: field {} at offset {}: byte 0x{:02x}", error.field,
                               error.offset, error.actual);
        case DecodeErrorKind::trailing_bytes:
            return std::format("trailing bytes: {} at offset {}: expected {} more bytes, have {}",
                               error.field, error.offset, error.expected, error.actual);
    }
    return "invalid decode error";
}

std::string to_string(const EncodeError& error) {
    switch (error.kind) {
        case EncodeErrorKind::buffer_too_small:
            return std::format("buffer too small: field {}: need {} bytes, have {}", error.field,
                               error.expected, error.actual);
        case EncodeErrorKind::value_out_of_range:
            return std::format("value out of range: field {}: largest is {}, have {}", error.field,
                               error.expected, error.actual);
    }
    return "invalid encode error";
}

std::optional<std::size_t> wire_size_of(std::byte type) noexcept {
    return with_message_type(
        type,
        []<class T>(std::type_identity<T>) { return std::optional<std::size_t>{T::wire_size}; },
        [] { return std::optional<std::size_t>{}; });
}

std::expected<Message, DecodeError> decode_strict(std::span<const std::byte> payload) noexcept {
    const std::optional<std::byte> type = BigEndianReader{payload}.read_byte(0);
    if (!type.has_value()) [[unlikely]] {
        return std::unexpected(truncated("message_type", 0, detail::kTypeByteSize, 0));
    }
    return with_message_type(
        *type,
        [&]<class T>(std::type_identity<T>) { return decode_as<T>(payload); },
        [&] -> std::expected<Message, DecodeError> {
            return std::unexpected(unknown_type(*type));
        });
}

std::expected<Message, DecodeError> decode(std::span<const std::byte> payload) noexcept {
    auto result = decode_strict(payload);
    if (result.has_value()) [[likely]] {
        return result;
    }
    if (payload.empty()) {
        return result;
    }
    const RawReason reason = result.error().kind == DecodeErrorKind::unknown_type
                                 ? RawReason::unknown_type
                                 : RawReason::wrong_length;
    return RawMessage{.bytes = payload, .reason = reason};
}

// std::visit only throws on a valueless variant. Message holds trivially copyable
// alternatives, so it can never be valueless.
// NOLINTNEXTLINE(bugprone-exception-escape): see the comment above.
std::expected<std::size_t, EncodeError> encode(const Message& message,
                                               std::span<std::byte> out) noexcept {
    return std::visit([&](const auto& alternative) { return encode_as(alternative, out); },
                      message);
}

std::expected<std::size_t, EncodeError> encode_frame(const Message& message,
                                                     std::span<std::byte> out) noexcept {
    const std::span<std::byte> body =
        out.size() < kFramePrefixSize ? std::span<std::byte>{} : out.subspan(kFramePrefixSize);
    auto written = encode(message, body);
    if (!written.has_value()) [[unlikely]] {
        if (written.error().kind == EncodeErrorKind::buffer_too_small) {
            return std::unexpected(
                buffer_too_small(kFramePrefixSize + written.error().expected, out.size()));
        }
        return written;
    }
    if (*written > kMaxPayloadSize) [[unlikely]] {
        return std::unexpected(out_of_range("payload_size", kMaxPayloadSize, *written));
    }
    BigEndianWriter writer{out};
    if (!writer.write_uint<kFramePrefixSize>(0, *written)) [[unlikely]] {
        return std::unexpected(buffer_too_small(kFramePrefixSize + *written, out.size()));
    }
    return kFramePrefixSize + *written;
}

}  // namespace tick_store::itch
