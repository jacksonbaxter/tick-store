#pragma once

// A RecordReader over lists of bytes, for making the oracle see anything at all.

#include "stub_fixture.hpp"
#include "tick_store/harness/record_reader.hpp"
#include "tick_store/itch/frame.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace tick_store::test {

class ListReader final : public harness::RecordReader {
public:
    ListReader(std::vector<Bytes> records_in, Bytes tail_in)
        : records(std::move(records_in)), tail_bytes(std::move(tail_in)) {}

    [[nodiscard]] std::expected<Record, harness::RecordReadError> next() override {
        if (fail_at.has_value() && next_ == *fail_at) {
            return std::unexpected(error);
        }
        if (next_ >= records.size()) {
            calls_after_end += ended_ ? 1U : 0U;
            ended_ = true;
            return Record{std::nullopt};
        }
        return Record{std::span<const std::byte>{records[next_++]}};
    }

    [[nodiscard]] std::expected<std::span<const std::byte>, harness::RecordReadError> tail()
        override {
        if (fail_tail) {
            return std::unexpected(error);
        }
        return std::span<const std::byte>{tail_bytes};
    }

    std::vector<Bytes> records;
    Bytes tail_bytes;
    std::optional<std::size_t> fail_at;
    bool fail_tail = false;
    harness::RecordReadError error{.message = "test error"};
    // The contract says nothing is called after the end, so tests can check that.
    std::size_t calls_after_end = 0;

private:
    std::size_t next_ = 0;
    bool ended_ = false;
};

// A reader holding a copy of each frame and the tail of `source`.
inline ListReader list_of(std::span<const std::byte> source) {
    const auto [frames, tail] = frames_of(source);
    std::vector<Bytes> records;
    for (const itch::Frame& frame : frames) {
        const auto whole = source.subspan(frame.offset, itch::kFramePrefixSize + frame.payload.size());
        records.emplace_back(whole.begin(), whole.end());
    }
    return ListReader{std::move(records), Bytes(tail.begin(), tail.end())};
}

}  // namespace tick_store::test
