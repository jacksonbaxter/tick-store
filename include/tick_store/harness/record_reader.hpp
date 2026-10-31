#pragma once

// What the correctness oracle needs from a store, and all it can get: the store's
// records, re-serialised, one at a time. The stub store implements this today, and
// Phase 3's store implements it without the oracle changing.
//
// next() and tail() take no arguments, so the oracle gives a reader nothing of the
// source: no bytes, no offset, no expected length. A reader knows only what it was
// built from, and the tools build one from a store file alone. A reader that was
// handed the source anyway and returns views of it is caught: the oracle refuses any
// record that overlaps the source. One that copied the source first cannot be told
// apart by any comparison of bytes, which is why the only reader a tool can construct
// takes no source, and a source given in place of a store is refused by the parser.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

namespace tick_store::harness {

// A store that could not produce its next record: corrupt, or unreadable.
struct RecordReadError {
    std::string message;
    // Where in the store's own file, if it knows.
    std::uint64_t store_offset = 0;

    friend bool operator==(const RecordReadError&, const RecordReadError&) = default;
};

class RecordReader {
public:
    using Record = std::optional<std::span<const std::byte>>;

    RecordReader() = default;
    RecordReader(const RecordReader&) = delete;
    RecordReader& operator=(const RecordReader&) = delete;
    RecordReader(RecordReader&&) = delete;
    RecordReader& operator=(RecordReader&&) = delete;
    virtual ~RecordReader() = default;

    // The next record as one whole frame: the 2-byte length prefix, then the payload,
    // exactly as they would stand in the source. nullopt after the last record. The
    // span is valid until the next call. After an error the oracle calls nothing more.
    [[nodiscard]] virtual std::expected<Record, RecordReadError> next() = 0;

    // The stored bytes that follow the last record. Called once, after next() has
    // returned nullopt. The span is valid for the life of the reader.
    [[nodiscard]] virtual std::expected<std::span<const std::byte>, RecordReadError> tail() = 0;
};

}  // namespace tick_store::harness
