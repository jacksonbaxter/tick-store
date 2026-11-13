#pragma once

// The checks the fuzz targets run, kept apart from libFuzzer so that the unit
// suite replays crashers through the same code. Each returns nullptr if the input
// passed, or a description of the broken property.

#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::itch::oracle {

// One payload through both decoders. decode() must accept any non-empty input,
// keep every byte, and agree with decode_strict(); what it returns must encode to
// the input exactly, into a buffer of exactly that size, and fail cleanly into a
// shorter one; and decoding the encoded bytes must give the same message.
[[nodiscard]] inline const char* check_message(std::span<const std::byte> payload) {
    const auto strict = decode_strict(payload);
    const auto lossless = decode(payload);

    if (payload.empty()) {
        return strict.has_value() || lossless.has_value() ? "an empty payload decoded" : nullptr;
    }
    if (!lossless.has_value()) {
        return "decode failed on a non-empty payload";
    }
    if (strict.has_value()) {
        if (*strict != *lossless) {
            return "decode and decode_strict disagree on a conforming payload";
        }
        if (std::holds_alternative<RawMessage>(*strict)) {
            return "decode_strict returned a raw message";
        }
    } else {
        const auto* raw = std::get_if<RawMessage>(&*lossless);
        if (raw == nullptr) {
            return "decode_strict refused a payload that decode called conforming";
        }
        const bool unknown = strict.error().kind == DecodeErrorKind::unknown_type;
        if ((raw->reason == RawReason::unknown_type) != unknown) {
            return "the raw reason disagrees with the strict error";
        }
    }

    // Exact size, so ASan sees any write past the message.
    std::vector<std::byte> out(payload.size());
    const auto written = encode(*lossless, out);
    if (!written.has_value()) {
        return "encode failed on a decoded message";
    }
    if (*written != payload.size() || !std::ranges::equal(out, payload)) {
        return "re-encoded bytes differ from the input";
    }

    const auto again = decode(out);
    if (!again.has_value() || *again != *lossless) {
        return "decoding the re-encoded bytes gave a different message";
    }

    // Every prefix of the output is too short, so one derived from the input
    // varies the chunk size from run to run without looping over all of them.
    const std::size_t short_size = std::to_integer<std::size_t>(payload.back()) % payload.size();
    const auto refused = encode(*lossless, std::span{out}.first(short_size));
    if (refused.has_value() || refused.error().kind != EncodeErrorKind::buffer_too_small) {
        return "encode accepted a buffer shorter than the message";
    }
    return nullptr;
}

// A byte stream through the cursor. Each frame must lie inside the input, directly
// after the one before; re-encoding every frame and appending the tail the cursor
// reports must rebuild the input exactly; and a cursor over any shorter prefix must
// agree on every frame that fits and stop where the cut lands. A stop must also be
// justified by the bytes: a whole frame must never be reported as truncated, a
// zero-length stop must sit on a prefix of 00 00 and a truncated body on a prefix
// declaring more than is left, and each stop's `expected` and `actual` must be the
// numbers frame.hpp gives for it, read from the tail.
[[nodiscard]] inline const char* check_stream(std::span<const std::byte> stream) {
    std::vector<std::byte> rebuilt(stream.size());
    std::size_t out = 0;
    std::vector<Frame> frames;

    FrameCursor cursor{stream};
    auto step = cursor.next();
    for (; step.has_value(); step = cursor.next()) {
        const Frame& frame = *step;
        if (frame.offset != out) {
            return "a frame does not start where the last one ended";
        }
        if (frame.payload.empty()) {
            return "the cursor returned an empty payload";
        }
        const auto message = decode(frame.payload);
        if (!message.has_value()) {
            return "decode failed on a frame";
        }
        const auto written = encode_frame(*message, std::span{rebuilt}.subspan(out));
        if (!written.has_value()) {
            return "encode_frame failed on a decoded frame";
        }
        if (*written != kFramePrefixSize + frame.payload.size()) {
            return "encode_frame wrote the wrong size";
        }
        out += *written;
        frames.push_back(frame);
    }

    const FrameStop stop = step.error();
    if (stop.offset != out || cursor.offset() != out) {
        return "the stop is not at the end of the last whole frame";
    }
    const auto repeat = cursor.next();
    if (repeat.has_value() || repeat.error() != stop) {
        return "a stop did not repeat";
    }
    const auto tail = cursor.remaining();
    if (tail.size() != stream.size() - out) {
        return "the tail does not cover the unread bytes";
    }
    if (stop.kind == FrameStopKind::end && !tail.empty()) {
        return "the cursor reported end with bytes left";
    }
    if (stop.kind != FrameStopKind::end && tail.empty()) {
        return "the cursor reported a fault with no bytes left";
    }
    if (stop.kind == FrameStopKind::truncated_prefix && tail.size() >= kFramePrefixSize) {
        return "the cursor reported a truncated prefix with a whole prefix left";
    }
    if (stop.kind == FrameStopKind::truncated_body && stop.actual >= stop.expected) {
        return "the cursor reported a truncated body with the whole body present";
    }
    // Each stop's numbers, read from the bytes themselves (review A-1).
    if (stop.kind == FrameStopKind::truncated_prefix &&
        (tail.size() != 1 || stop.expected != kFramePrefixSize || stop.actual != 1)) {
        return "a truncated prefix does not state 2 bytes needed and the 1 left";
    }
    if (stop.kind == FrameStopKind::zero_length || stop.kind == FrameStopKind::truncated_body) {
        if (tail.size() < kFramePrefixSize) {
            return "the cursor read a prefix that is not there";
        }
        const std::size_t declared =
            (std::to_integer<std::size_t>(tail[0]) << 8U) | std::to_integer<std::size_t>(tail[1]);
        const std::size_t after_prefix = tail.size() - kFramePrefixSize;
        if (stop.actual != after_prefix) {
            return "a stop does not state the bytes after its prefix";
        }
        if ((stop.kind == FrameStopKind::zero_length) != (declared == 0)) {
            return "a zero-length stop and a zero prefix do not go together";
        }
        if (stop.kind == FrameStopKind::zero_length && stop.expected != 0) {
            return "a zero-length stop states bytes needed";
        }
        if (stop.kind == FrameStopKind::truncated_body &&
            (stop.expected != declared || declared <= after_prefix)) {
            return "a truncated body does not state its prefix's length, or that length is there";
        }
    }
    std::ranges::copy(tail, std::span{rebuilt}.subspan(out).begin());
    if (!std::ranges::equal(rebuilt, stream)) {
        return "the rebuilt stream differs from the input";
    }

    // The same stream cut short, at a point the input chooses.
    if (stream.empty()) {
        return nullptr;
    }
    const std::size_t cut = std::to_integer<std::size_t>(stream.front()) % stream.size();
    FrameCursor short_cursor{stream.first(cut)};
    std::size_t index = 0;
    auto short_step = short_cursor.next();
    for (; short_step.has_value(); short_step = short_cursor.next()) {
        if (index >= frames.size() || short_step->offset != frames[index].offset ||
            !std::ranges::equal(short_step->payload, frames[index].payload)) {
            return "a shorter input gave a different frame";
        }
        ++index;
    }
    const auto fits = [cut](const Frame& frame) {
        return frame.offset + kFramePrefixSize + frame.payload.size() <= cut;
    };
    if (std::cmp_not_equal(index, std::ranges::count_if(frames, fits))) {
        return "a shorter input gave a different number of frames";
    }
    if (short_step.error().offset > cut) {
        return "a stop was reported past the end of a shorter input";
    }
    return nullptr;
}

}  // namespace tick_store::itch::oracle
