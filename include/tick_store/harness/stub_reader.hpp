#pragma once

// Plugs the stub store into the oracle. Each record goes parser -> Message ->
// encode_frame, so what the oracle compares is the output of the real decoder and
// encoder. Phase 3 replaces this file along with the stub.

#include "tick_store/harness/record_reader.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/stub_store.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <span>

namespace tick_store::harness {

class StubRecordReader final : public RecordReader {
public:
    // The bytes behind `parser` must outlive this reader.
    explicit StubRecordReader(const store::StoreParser& parser) noexcept : parser_(parser) {}

    [[nodiscard]] std::expected<Record, RecordReadError> next() override;
    [[nodiscard]] std::expected<std::span<const std::byte>, RecordReadError> tail() override;

private:
    store::StoreParser parser_;
    std::array<std::byte, itch::kFramePrefixSize + itch::kMaxPayloadSize> frame_{};
};

}  // namespace tick_store::harness
