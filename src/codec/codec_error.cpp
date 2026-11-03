#include "tick_store/codec/codec_error.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace tick_store::codec {

CodecError codec_error(CodecErrorKind kind, std::string_view field, std::uint64_t offset,
                       std::uint64_t expected, std::uint64_t actual) noexcept {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

std::string to_string(const CodecError& error) {
    const auto located = [&](std::string_view what) {
        return std::format("{}: {} at bit {}: expected {}, have {}", what, error.field,
                           error.offset, error.expected, error.actual);
    };
    switch (error.kind) {
        case CodecErrorKind::truncated:
            return located("truncated");
        case CodecErrorKind::bad_width:
            return located("bad width");
        case CodecErrorKind::bad_count:
            return located("bad count");
        case CodecErrorKind::trailing_bits:
            return located("trailing bits");
        case CodecErrorKind::non_canonical:
            return located("non-canonical encoding");
        case CodecErrorKind::bad_window:
            return located("bad XOR window");
        case CodecErrorKind::bad_parameter:
            return located("bad parameter");
        case CodecErrorKind::not_exact_price:
            return located("not an exact price");
        case CodecErrorKind::bad_field:
            return located("field out of range");
    }
    return located("unknown codec error");
}

}  // namespace tick_store::codec
