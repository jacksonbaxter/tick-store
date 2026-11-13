#pragma once

// One segment of the ITCH store, between records and column values (itch_store.hpp).
//
// SegmentBuilder takes records one at a time and keeps each column's values, in file
// order, with the keys the grouped columns sort by. finish() hands back the columns in
// their stored order and the segment's summary. Its memory is the segment's values plus a
// table of the orders the segment knows, which it clears for the next segment: nothing it
// holds grows with the day.
//
// SegmentAssembler takes the decoded columns of one segment back, checks everything that
// ties them together, rebuilds file order, and gives the records back one at a time. It
// needs nothing from any other segment: the orders a segment's messages name come from its
// own creations and its own import list.

#include "tick_store/itch/message.hpp"
#include "tick_store/store/itch_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace tick_store::store {

class SegmentBuilder {
public:
    SegmentBuilder();
    SegmentBuilder(const SegmentBuilder&) = delete;
    SegmentBuilder& operator=(const SegmentBuilder&) = delete;
    SegmentBuilder(SegmentBuilder&&) noexcept;
    SegmentBuilder& operator=(SegmentBuilder&&) noexcept;
    ~SegmentBuilder();

    // Adds the next record. `symbols` learns the first stock seen for each locate, which
    // derived stocks are compared with. It learns them in finish(), so the builder keeps
    // a pointer to it: it must stay at the same address until then. Refuses, changing
    // nothing, a timestamp over 48 bits
    // and a RawMessage that is empty, over 65,535 bytes, or a conforming message that
    // decode() would never return raw: none of them is a frame of any file.
    [[nodiscard]] std::expected<void, ItchStoreError> add(const itch::Message& message,
                                                          SymbolTable& symbols);

    [[nodiscard]] std::uint32_t records() const noexcept;

    // The columns in their stored order and the summary, with first_record set from
    // `first_record`. Leaves the builder empty for the next segment.
    [[nodiscard]] SegmentColumns finish(std::uint64_t first_record, SegmentSummary& summary);

private:
    struct State;
    std::unique_ptr<State> state_;
};

class SegmentAssembler {
public:
    SegmentAssembler();
    SegmentAssembler(const SegmentAssembler&) = delete;
    SegmentAssembler& operator=(const SegmentAssembler&) = delete;
    SegmentAssembler(SegmentAssembler&&) noexcept;
    SegmentAssembler& operator=(SegmentAssembler&&) noexcept;
    ~SegmentAssembler();

    // Takes segment `segment`'s decoded columns, in stored order and each at the count
    // counts_from_types() and the import and exception rules give, and checks what ties
    // them together: the summary against the values, the stock exceptions' records, and
    // the import list's length. The summary and symbols must outlive the records read.
    [[nodiscard]] std::expected<void, ItchStoreError> load(std::uint64_t segment,
                                                           SegmentColumns columns,
                                                           const SegmentSummary& summary,
                                                           const SymbolTable& symbols);

    // The next record, or nullopt after the last. A RawMessage borrows from the assembler
    // and is valid until the next call. After fail(), its error, until a load() succeeds.
    [[nodiscard]] std::expected<std::optional<itch::Message>, ItchStoreError> next();

    // Drops the segment held: next() returns `error` until a load() succeeds.
    void fail(const ItchStoreError& error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace tick_store::store
