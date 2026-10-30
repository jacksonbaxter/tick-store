#pragma once

// BinaryFILE framing: each message is a 2-byte big-endian payload length, then
// the payload. The length does not count itself.
//
// Over-read contract: FrameCursor reads only inside the span it was given, and
// needs no padding after it.
//
// Lifetime: Frame::payload and FrameCursor::remaining() borrow from that span,
// so they are valid only while the bytes behind it are.

#include "tick_store/itch/big_endian.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace tick_store::itch {

// Size of the length prefix in front of every frame.
inline constexpr std::size_t kFramePrefixSize = 2;

// One whole frame. `offset` is where its length prefix starts in the input.
struct Frame {
    std::span<const std::byte> payload;
    std::size_t offset = 0;
};

// Why a cursor stopped.
enum class FrameStopKind : std::uint8_t {
    // The input ended exactly on a frame boundary.
    end,
    // One byte remains, which is less than a length prefix.
    truncated_prefix,
    // The prefix promises more bytes than remain.
    truncated_body,
    // The prefix is zero. BinaryFILE defines this as the end of the session, but
    // no sample file has one, so it is reported, never skipped or treated as end.
    zero_length,
};

// A stop, located. `offset` is where the frame that could not be read starts.
// `expected` and `actual` are bytes needed and bytes available:
//
//   end               offset = input size; expected = 0, actual = 0
//   truncated_prefix  expected = 2, actual = 1
//   truncated_body    expected = declared payload length, actual = bytes after the prefix
//   zero_length       expected = 0, actual = bytes after the prefix
struct FrameStop {
    FrameStopKind kind = FrameStopKind::end;
    std::size_t offset = 0;
    std::size_t expected = 0;
    std::size_t actual = 0;

    friend bool operator==(const FrameStop&, const FrameStop&) = default;
};

[[nodiscard]] std::string to_string(FrameStopKind kind);
[[nodiscard]] std::string to_string(const FrameStop& stop);

// Walks a byte stream frame by frame.
//
// next() returns each whole frame, then a FrameStop. A stop is terminal: the
// cursor does not move, so calling next() again repeats it and remaining() is
// everything from the stopped frame on, the bytes a lossless copy must keep as
// the file tail. A caller who wants to read past a zero-length frame starts a
// new cursor after its prefix.
class FrameCursor {
public:
    constexpr explicit FrameCursor(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] constexpr std::expected<Frame, FrameStop> next() noexcept {
        const std::size_t left = bytes_.size() - offset_;
        if (left == 0) {
            return stop(FrameStopKind::end, 0, 0);
        }
        const auto length = BigEndianReader{bytes_}.read_uint<kFramePrefixSize>(offset_);
        if (!length.has_value()) {
            return stop(FrameStopKind::truncated_prefix, kFramePrefixSize, left);
        }
        const std::size_t after_prefix = left - kFramePrefixSize;
        const auto payload_size = static_cast<std::size_t>(*length);
        if (payload_size == 0) {
            return stop(FrameStopKind::zero_length, 0, after_prefix);
        }
        if (payload_size > after_prefix) {
            return stop(FrameStopKind::truncated_body, payload_size, after_prefix);
        }
        const Frame frame{.payload = bytes_.subspan(offset_ + kFramePrefixSize, payload_size),
                          .offset = offset_};
        offset_ += kFramePrefixSize + payload_size;
        return frame;
    }

    // Where the next frame starts, which is the end of the last whole frame.
    [[nodiscard]] constexpr std::size_t offset() const noexcept { return offset_; }

    // The bytes not yet returned as a frame.
    [[nodiscard]] constexpr std::span<const std::byte> remaining() const noexcept {
        return bytes_.subspan(offset_);
    }

private:
    [[nodiscard]] constexpr std::unexpected<FrameStop> stop(FrameStopKind kind, std::size_t expected,
                                                            std::size_t actual) const noexcept {
        return std::unexpected(
            FrameStop{.kind = kind, .offset = offset_, .expected = expected, .actual = actual});
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

}  // namespace tick_store::itch
