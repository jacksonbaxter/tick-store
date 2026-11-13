#pragma once

// Writes an ITCH store (itch_store.hpp) one record at a time, a segment at a time, into a
// container file published in a named durability mode (container_writer.hpp), or into
// memory for the tests and the fuzzer.
//
// Memory is bounded by the segment, not the day: the builder's values and its table of the
// segment's known orders, cleared each segment; a symbol table of at most 65,536 stocks; and
// the container's footer directory, 32 bytes a block plus 12 bytes of metadata a column and
// up to 8,250 a segment. docs/store.md gives the measured figures.
//
// Records go in as Messages, so the writer stores what the ITCH decoder made of each frame,
// and the RawMessage of an unknown or off-spec frame keeps every byte. finish() takes the
// bytes after the last whole frame, the tail, verbatim.

#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/container_writer.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/itch_segment.hpp"
#include "tick_store/store/itch_store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace tick_store::store {

struct ItchStoreOptions {
    std::uint32_t segment_records = kDefaultSegmentRecords;
};

class ItchStoreWriter {
public:
    // Creates `<path>.partial` through ContainerWriter. Refuses a segment size outside 1 to
    // kMaxSegmentRecords.
    [[nodiscard]] static std::expected<ItchStoreWriter, ItchStoreError> create(
        const std::filesystem::path& path, Durability durability, ItchStoreOptions options = {},
        FileOps& ops = posix_file_ops());

    // Builds the file in memory; take_bytes() hands it over after finish().
    [[nodiscard]] static std::expected<ItchStoreWriter, ItchStoreError> in_memory(
        ItchStoreOptions options = {});

    ItchStoreWriter(const ItchStoreWriter&) = delete;
    ItchStoreWriter& operator=(const ItchStoreWriter&) = delete;
    ItchStoreWriter(ItchStoreWriter&&) noexcept;
    ItchStoreWriter& operator=(ItchStoreWriter&&) = delete;
    ~ItchStoreWriter();

    // Adds the next record, writing a segment's blocks each time one fills. A refused record
    // (SegmentBuilder::add) changes nothing and is not fatal; a failed write is, as in
    // ContainerWriter: every later append() and finish() returns the same error, and the
    // partial file is removed.
    [[nodiscard]] std::expected<void, ItchStoreError> append(const itch::Message& message);

    // Writes the last segment, then the store, symbols and tail sections, and commits. A
    // failure is fatal, as in append().
    [[nodiscard]] std::expected<void, ItchStoreError> finish(std::span<const std::byte> tail);

    // The whole file, for an in-memory writer after a finish() that succeeded; empty
    // otherwise.
    [[nodiscard]] std::vector<std::byte> take_bytes();

    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }

private:
    explicit ItchStoreWriter(ItchStoreOptions options);

    [[nodiscard]] std::expected<void, ItchStoreError> write_segment();
    // finish()'s work after its checks.
    [[nodiscard]] std::expected<void, ItchStoreError> write_last(std::span<const std::byte> tail);
    // Keeps a failure, so every later call returns it, and drops the partial file.
    [[nodiscard]] std::expected<void, ItchStoreError> latch(
        std::expected<void, ItchStoreError> result);
    [[nodiscard]] std::expected<void, ItchStoreError> add_block(codec::CodecId codec,
                                                                std::span<const std::byte> metadata,
                                                                std::span<const std::byte> payload);
    [[nodiscard]] std::expected<void, ItchStoreError> add_section(std::string_view name,
                                                                  std::span<const std::byte> bytes);

    ItchStoreOptions options_;
    SegmentBuilder builder_;
    // On the heap, so a move keeps its address: the builder holds it until the segment is
    // written (SegmentBuilder::add).
    std::unique_ptr<SymbolTable> symbols_ = std::make_unique<SymbolTable>();
    std::uint64_t records_ = 0;
    std::uint64_t segments_ = 0;
    std::optional<ContainerWriter> file_;
    // In memory: the header, then each part as it is added, then the footer at finish().
    ContainerLayout layout_;
    std::vector<std::byte> memory_;
    bool finished_ = false;
    std::optional<ItchStoreError> failed_;
};

}  // namespace tick_store::store
