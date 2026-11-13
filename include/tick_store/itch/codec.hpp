#pragma once

// Decode and re-serialise one ITCH 5.0 payload: the bytes of a frame after its
// length prefix, type byte first.
//
// There are two decoders because two different questions get asked of a payload:
//
//   decode         "Give me this message." Lossless: anything that is not a
//                  conforming message comes back as a RawMessage holding every
//                  byte, and encode() reproduces it exactly. It fails only on an
//                  empty payload, which has no bytes to preserve. Use this on a
//                  file: an unknown type or an unexpected length is data.
//   decode_strict  "Is this payload a known type, at exactly that type's
//                  length?" A payload that is not is a typed DecodeError naming
//                  where it departs. Field values are not checked (see
//                  message.hpp): a price above the specification's maximum or
//                  an alpha value outside its lists decodes. Use this to
//                  diagnose a payload, for example a RawMessage's bytes.
//
// Over-read contract: decoders read only inside [payload.data(), payload.data() +
// payload.size()), and encoders write only inside `out`. Neither needs padding.
//
// Lifetime: only a RawMessage borrows from the payload. See message.hpp.

#include "tick_store/itch/message.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::itch {

// The largest payload a frame prefix can declare.
inline constexpr std::size_t kMaxPayloadSize = 0xFFFF;

// The largest timestamp the 6-byte wire field can hold.
inline constexpr std::uint64_t kMaxTimestampNs = 0xFFFF'FFFF'FFFFULL;

enum class DecodeErrorKind : std::uint8_t {
    // The payload ended before `field`.
    truncated,
    // The first byte is not one of the 23 type codes.
    unknown_type,
    // Bytes remain after the last field of a complete message.
    trailing_bytes,
};

// What went wrong and where, as offsets into the payload:
//
//   truncated       `field` needs `expected` bytes at `offset`; `actual` remain
//   trailing_bytes  `field` is "end of message", `offset` is the type's length,
//                   `expected` is 0 and `actual` is how many bytes follow
//   unknown_type    `field` is "message_type", `offset` is 0, `expected` is 0
//                   and `actual` is the byte value
//
// `field` points at static storage, so an error outlives the payload.
struct DecodeError {
    DecodeErrorKind kind = DecodeErrorKind::truncated;
    std::string_view field;
    std::size_t offset = 0;
    std::size_t expected = 0;
    std::size_t actual = 0;

    friend bool operator==(const DecodeError&, const DecodeError&) = default;
};

[[nodiscard]] std::string to_string(const DecodeError& error);

enum class EncodeErrorKind : std::uint8_t {
    // `out` is smaller than the message. `expected` is the bytes needed.
    buffer_too_small,
    // The value of `field` does not fit its wire width, so writing it would
    // silently change it. `actual` is the value and `expected` the largest allowed;
    // for "raw bytes", a RawMessage with none, `expected` is 1, the fewest allowed.
    value_out_of_range,
};

// `field` points at static storage.
struct EncodeError {
    EncodeErrorKind kind = EncodeErrorKind::buffer_too_small;
    std::string_view field;
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;

    friend bool operator==(const EncodeError&, const EncodeError&) = default;
};

[[nodiscard]] std::string to_string(const EncodeError& error);

// The wire length of `type`, or nullopt if it is not one of the 23 type codes.
[[nodiscard]] std::optional<std::size_t> wire_size_of(std::byte type) noexcept;

// Lossless decode: see the top of this file.
[[nodiscard]] std::expected<Message, DecodeError> decode(
    std::span<const std::byte> payload) noexcept;

// Strict decode: see the top of this file. A success is never a RawMessage.
[[nodiscard]] std::expected<Message, DecodeError> decode_strict(
    std::span<const std::byte> payload) noexcept;

// Writes the payload bytes of `message` to the front of `out` and returns how
// many it wrote. On error, the contents of `out` are unspecified. `out` must not
// overlap the bytes of a RawMessage.
[[nodiscard]] std::expected<std::size_t, EncodeError> encode(const Message& message,
                                                             std::span<std::byte> out) noexcept;

// As encode(), with the 2-byte length prefix in front, so consecutive calls
// rebuild a BinaryFILE stream. Returns the bytes written, prefix included. Both refuse
// an empty RawMessage, whose frame would be the stream's end of session.
[[nodiscard]] std::expected<std::size_t, EncodeError> encode_frame(
    const Message& message, std::span<std::byte> out) noexcept;

}  // namespace tick_store::itch
