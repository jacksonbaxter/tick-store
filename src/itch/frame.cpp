#include "tick_store/itch/frame.hpp"

#include <format>
#include <string>

namespace tick_store::itch {

std::string to_string(FrameStopKind kind) {
    switch (kind) {
        case FrameStopKind::end:
            return "end";
        case FrameStopKind::truncated_prefix:
            return "truncated prefix";
        case FrameStopKind::truncated_body:
            return "truncated body";
        case FrameStopKind::zero_length:
            return "zero-length frame";
    }
    return "invalid frame stop";
}

std::string to_string(const FrameStop& stop) {
    return std::format("{} at offset {}: expected {} bytes, have {}", to_string(stop.kind),
                       stop.offset, stop.expected, stop.actual);
}

}  // namespace tick_store::itch
