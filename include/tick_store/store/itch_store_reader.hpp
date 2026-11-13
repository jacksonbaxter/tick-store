#pragma once

// Reads an ITCH store (itch_store.hpp): opens the container, checks the store's sections
// and every block's metadata from the footer alone, then decodes any segment on demand.
//
// open() reads the file's two ends and its three sections. Nothing else is read until a
// segment is asked for, so a time index or symbol filter can choose segments from
// segment() without touching a payload. Block and section CRCs are checked as each is read,
// unless ReadOptions say Checksums::skip (container.hpp).
//
// Random access: SegmentReader::seek(k) reads segment k's blocks and nothing else, decodes
// its columns and hands its records back in order. The worst case for one segment is
// decoding all of its columns, every one at most N values for N records but imbalance
// prices and MWCB levels at 3 a record, and the byte columns at a record's payload size.

#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_segment.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/mapped_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace tick_store::store {

// One segment as the footer describes it.
struct SegmentEntry {
    SegmentSummary summary;
    // The container block holding each column's first piece, or kNoIndex where the segment
    // has none; its pieces are that block and the ones after it.
    std::array<std::uint64_t, kColumnCount> block{};
    // Each column's pieces, and its values over them all, from their metadata; 0 where
    // absent.
    std::array<std::uint32_t, kColumnCount> pieces{};
    std::array<std::uint32_t, kColumnCount> values{};
};

// What a file's bytes are spent on, from the footer alone.
struct StoreSizes {
    std::uint64_t file_bytes = 0;
    std::uint64_t header_bytes = 0;
    std::uint64_t trailer_bytes = 0;
    // The footer: fixed counts, block and section entries, and the block metadata.
    std::uint64_t footer_fixed_bytes = 0;
    std::uint64_t block_entry_bytes = 0;
    std::uint64_t section_entry_bytes = 0;
    std::uint64_t metadata_bytes = 0;
    // Per column: payload bytes, metadata bytes, blocks, values, and blocks under zstd.
    std::array<std::uint64_t, kColumnCount> column_payload_bytes{};
    std::array<std::uint64_t, kColumnCount> column_metadata_bytes{};
    std::array<std::uint64_t, kColumnCount> column_blocks{};
    std::array<std::uint64_t, kColumnCount> column_values{};
    std::array<std::uint64_t, kColumnCount> column_zstd_blocks{};
    std::uint64_t store_section_bytes = 0;
    std::uint64_t symbols_section_bytes = 0;
    std::uint64_t tail_section_bytes = 0;
};

// One piece of a column as stored, for a reader that decodes part of it.
struct ColumnView {
    codec::CodecId codec = codec::CodecId::none;
    std::span<const std::byte> payload;
    // The container block, kNoIndex where the segment has no values in the column.
    std::uint64_t block = kNoIndex;
    // The piece's value count, 0 where absent, and its first value's index in the column.
    std::uint64_t values = 0;
    std::uint64_t first_value = 0;
    // Where the payload starts in the file.
    std::uint64_t file_offset = 0;
};

class ItchStoreReader {
public:
    // Maps the file and opens it.
    [[nodiscard]] static std::expected<ItchStoreReader, ItchStoreError> open(
        const std::filesystem::path& path, ReadOptions options = {});

    // Opens bytes held in memory, which must outlive the reader and every view from it.
    [[nodiscard]] static std::expected<ItchStoreReader, ItchStoreError> open_bytes(
        std::span<const std::byte> file, ReadOptions options = {});

    [[nodiscard]] const StoreHeader& header() const noexcept { return header_; }
    [[nodiscard]] const SymbolTable& symbols() const noexcept { return *symbols_; }
    [[nodiscard]] std::uint64_t segment_count() const noexcept { return segments_.size(); }
    [[nodiscard]] const SegmentEntry& segment(std::uint64_t index) const {
        return segments_.at(index);
    }
    [[nodiscard]] const ContainerParser& container() const noexcept { return parser_; }

    // The source's bytes after its last whole frame, CRC checked unless skipped at open.
    [[nodiscard]] std::expected<std::span<const std::byte>, ItchStoreError> tail() const;

    // A segment's columns in their stored order, each decoded at the count its records
    // need, every block's CRC checked unless skipped at open.
    [[nodiscard]] std::expected<SegmentColumns, ItchStoreError> read_columns(
        std::uint64_t segment) const;

    // One piece of one column of one segment as stored, the block's CRC checked unless
    // skipped at open. The segment must exist, and the piece too where the column has
    // values; a column with none gives a view with block kNoIndex.
    [[nodiscard]] std::expected<ColumnView, ItchStoreError> column_view(std::uint64_t segment,
                                                                        Column column,
                                                                        std::uint64_t piece = 0) const;

    [[nodiscard]] StoreSizes sizes() const;

private:
    ItchStoreReader(std::shared_ptr<const MappedFile> mapping, std::span<const std::byte> file,
                    ContainerParser parser)
        : mapping_(std::move(mapping)), file_(file), parser_(std::move(parser)) {}

    [[nodiscard]] static std::expected<ItchStoreReader, ItchStoreError> build(
        std::shared_ptr<const MappedFile> mapping, std::span<const std::byte> file,
        ReadOptions options);
    [[nodiscard]] std::expected<void, ItchStoreError> read_sections();
    [[nodiscard]] std::expected<void, ItchStoreError> read_directory();
    [[nodiscard]] std::expected<void, ItchStoreError> close_column(const SegmentEntry& entry,
                                                                   Column column,
                                                                   std::uint64_t block) const;
    [[nodiscard]] std::expected<std::vector<std::uint64_t>, ItchStoreError> read_column(
        std::uint64_t segment, Column column, std::uint64_t count) const;

    std::shared_ptr<const MappedFile> mapping_;
    std::span<const std::byte> file_;
    ContainerParser parser_;
    StoreHeader header_;
    // Behind a pointer so the reader can move while SegmentReaders hold a reference.
    std::unique_ptr<SymbolTable> symbols_;
    std::vector<SegmentEntry> segments_;
};

// The records of one segment at a time, in file order.
class SegmentReader {
public:
    // Reads and decodes segment `segment`. The reader must outlive the records read. After
    // a failure, next() returns the same error until a seek succeeds.
    [[nodiscard]] std::expected<void, ItchStoreError> seek(const ItchStoreReader& reader,
                                                           std::uint64_t segment);

    // The next record of the segment, or nullopt after its last. A RawMessage borrows from
    // this reader until the next call. Before any seek, a segment of no records.
    [[nodiscard]] std::expected<std::optional<itch::Message>, ItchStoreError> next() {
        return assembler_.next();
    }

private:
    SegmentAssembler assembler_;
};

}  // namespace tick_store::store
