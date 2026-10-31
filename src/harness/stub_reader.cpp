#include "tick_store/harness/stub_reader.hpp"

#include "tick_store/harness/record_reader.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>

namespace tick_store::harness {

std::expected<RecordReader::Record, RecordReadError> StubRecordReader::next() {
    const auto message = parser_.next();
    if (!message.has_value()) [[unlikely]] {
        return std::unexpected(RecordReadError{.message = to_string(message.error()),
                                               .store_offset = message.error().offset});
    }
    if (!message->has_value()) {
        return Record{std::nullopt};
    }
    const auto written = itch::encode_frame(**message, frame_);
    if (!written.has_value()) [[unlikely]] {
        return std::unexpected(RecordReadError{.message = to_string(written.error())});
    }
    return Record{std::span<const std::byte>{frame_}.first(*written)};
}

std::expected<std::span<const std::byte>, RecordReadError> StubRecordReader::tail() {
    return parser_.tail();
}

}  // namespace tick_store::harness
