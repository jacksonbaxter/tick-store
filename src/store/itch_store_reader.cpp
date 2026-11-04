#include "tick_store/store/itch_store_reader.hpp"

#include "tick_store/codec/registry.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_segment.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

[[nodiscard]] std::unexpected<ItchStoreError> container_failure(std::string_view what,
                                                                const StoreError& cause) {
    ItchStoreError error;
    error.kind = ItchStoreErrorKind::container;
    error.field = what;
    error.offset = cause.offset;
    error.container = cause;
    return std::unexpected(error);
}

[[nodiscard]] std::uint64_t offset_in(std::span<const std::byte> file,
                                      std::span<const std::byte> part) noexcept {
    return part.empty() ? 0 : static_cast<std::uint64_t>(part.data() - file.data());
}

constexpr std::size_t kStoreSections = 3;

}  // namespace

std::expected<ItchStoreReader, ItchStoreError> ItchStoreReader::open(
    const std::filesystem::path& path, ReadOptions options) {
    auto mapped = MappedFile::open(path, Access::normal);
    if (!mapped.has_value()) {
        return container_failure("open", mapped.error());
    }
    auto mapping = std::make_shared<const MappedFile>(std::move(*mapped));
    const std::span<const std::byte> bytes = mapping->bytes();
    return build(std::move(mapping), bytes, options);
}

std::expected<ItchStoreReader, ItchStoreError> ItchStoreReader::open_bytes(
    std::span<const std::byte> file, ReadOptions options) {
    return build(nullptr, file, options);
}

std::expected<ItchStoreReader, ItchStoreError> ItchStoreReader::build(
    std::shared_ptr<const MappedFile> mapping, std::span<const std::byte> file,
    ReadOptions options) {
    auto parser = ContainerParser::open(file, options);
    if (!parser.has_value()) {
        return container_failure("container", parser.error());
    }
    ItchStoreReader reader{std::move(mapping), file, std::move(*parser)};
    if (auto read = reader.read_sections(); !read.has_value()) {
        return std::unexpected(read.error());
    }
    if (auto read = reader.read_directory(); !read.has_value()) {
        return std::unexpected(read.error());
    }
    return reader;
}

std::expected<void, ItchStoreError> ItchStoreReader::read_sections() {
    if (parser_.section_count() != kStoreSections) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::bad_section;
        error.field = "section count";
        error.offset = parser_.footer_offset();
        error.expected = kStoreSections;
        error.actual = parser_.section_count();
        return std::unexpected(error);
    }
    auto store = parser_.section(kStoreSection);
    if (!store.has_value()) {
        return container_failure("store section", store.error());
    }
    auto header = parse_store_header(store->bytes, offset_in(file_, store->bytes));
    if (!header.has_value()) {
        return std::unexpected(header.error());
    }
    header_ = *header;
    auto symbols = parser_.section(kSymbolsSection);
    if (!symbols.has_value()) {
        return container_failure("symbols section", symbols.error());
    }
    auto table = SymbolTable::parse(symbols->bytes, offset_in(file_, symbols->bytes));
    if (!table.has_value()) {
        return std::unexpected(table.error());
    }
    symbols_ = std::make_unique<SymbolTable>(std::move(*table));
    // The tail's CRC is checked when it is read; here it only has to exist.
    for (std::uint64_t i = 0; i < parser_.section_count(); ++i) {
        auto info = parser_.section_info(i);
        if (info.has_value() && info->name == kTailSection) {
            return {};
        }
    }
    return container_failure("tail section",
                             StoreError{.kind = StoreErrorKind::not_found, .field = "section"});
}

std::expected<void, ItchStoreError> ItchStoreReader::read_directory() {
    const std::uint64_t n = header_.segment_records;
    // Every segment has a type block, so the blocks bound the segments; the header's count,
    // not yet checked against them, does not size anything.
    segments_.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(header_.segment_count, parser_.block_count())));
    std::uint64_t records = 0;
    Column previous = Column::type;
    for (std::uint64_t b = 0; b < parser_.block_count(); ++b) {
        auto info = parser_.block_info(b);
        if (!info.has_value()) {
            return container_failure("block", info.error());
        }
        const std::uint64_t metadata_offset = offset_in(file_, info->metadata);
        auto metadata = parse_column_metadata(info->metadata, metadata_offset);
        if (!metadata.has_value()) {
            ItchStoreError error = metadata.error();
            error.block = b;
            return std::unexpected(error);
        }
        const auto refuse = [&](std::string_view name, std::size_t at, std::uint64_t expected,
                                std::uint64_t actual) {
            ItchStoreError error;
            error.kind = ItchStoreErrorKind::bad_metadata;
            error.field = name;
            error.block = b;
            error.segment = metadata->segment;
            error.column = column_info(metadata->column).name;
            error.offset = metadata_offset + at;
            error.expected = expected;
            error.actual = actual;
            return std::unexpected(error);
        };
        if (metadata->summary.has_value()) {
            // A type block, the only one with a summary, opens the next segment.
            const std::uint64_t segment = segments_.size();
            if (metadata->segment != segment) {
                return refuse("segment index", kMetaSegmentOffset, segment, metadata->segment);
            }
            if (segment >= header_.segment_count) {
                return refuse("segment index", kMetaSegmentOffset, header_.segment_count - 1,
                              segment);
            }
            const SegmentSummary& summary = *metadata->summary;
            if (summary.first_record != records) {
                return refuse("first record", kMetaFirstRecordOffset, records,
                              summary.first_record);
            }
            const bool last = segment + 1 == header_.segment_count;
            const std::uint64_t want = last ? header_.record_count - records : n;
            if (summary.records != want) {
                return refuse("value count", kMetaValueCountOffset, want, summary.records);
            }
            records += summary.records;
            SegmentEntry entry;
            entry.block.fill(kNoIndex);
            entry.summary = summary;
            segments_.push_back(std::move(entry));
        } else {
            if (segments_.empty()) {
                return refuse("column id", kMetaColumnOffset, 0,
                              static_cast<std::uint64_t>(metadata->column));
            }
            const std::uint64_t segment = segments_.size() - 1;
            if (metadata->segment != segment) {
                return refuse("segment index", kMetaSegmentOffset, segment, metadata->segment);
            }
            if (metadata->column <= previous) {
                return refuse("column id", kMetaColumnOffset,
                              static_cast<std::uint64_t>(previous) + 1,
                              static_cast<std::uint64_t>(metadata->column));
            }
        }
        const ColumnInfo& column = column_info(metadata->column);
        const codec::CodecId primary = primary_codec(metadata->column);
        if (info->codec != primary && (!column.zstd || info->codec != codec::CodecId::zstd)) {
            ItchStoreError error;
            error.kind = ItchStoreErrorKind::bad_segment;
            error.field = "column codec";
            error.block = b;
            error.segment = metadata->segment;
            error.column = column.name;
            error.offset = info->payload_offset;
            error.expected = static_cast<std::uint64_t>(primary);
            error.actual = static_cast<std::uint64_t>(info->codec);
            return std::unexpected(error);
        }
        SegmentEntry& entry = segments_.back();
        entry.block[static_cast<std::size_t>(metadata->column)] = b;
        entry.values[static_cast<std::size_t>(metadata->column)] = metadata->values;
        previous = metadata->column;
    }
    if (segments_.size() != header_.segment_count) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::bad_segment;
        error.field = "segment count";
        error.offset = parser_.footer_offset();
        error.expected = header_.segment_count;
        error.actual = segments_.size();
        return std::unexpected(error);
    }
    return {};
}

std::expected<std::span<const std::byte>, ItchStoreError> ItchStoreReader::tail() const {
    auto section = parser_.section(kTailSection);
    if (!section.has_value()) {
        return container_failure("tail section", section.error());
    }
    return section->bytes;
}

std::expected<std::vector<std::uint64_t>, ItchStoreError> ItchStoreReader::read_column(
    std::uint64_t segment, Column column, std::uint64_t count) const {
    const SegmentEntry& entry = segments_[segment];
    const auto c = static_cast<std::size_t>(column);
    const std::uint64_t block = entry.block[c];
    const auto refuse = [&](std::string_view name, std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::bad_segment;
        error.field = name;
        error.segment = segment;
        error.block = block;
        error.column = column_info(column).name;
        error.expected = expected;
        error.actual = actual;
        return std::unexpected(error);
    };
    if (block == kNoIndex) {
        if (count != 0) {
            return refuse("column missing", count, 0);
        }
        return std::vector<std::uint64_t>{};
    }
    if (entry.values[c] != count) {
        return refuse(count == 0 ? "column not expected" : "column value count", count,
                      entry.values[c]);
    }
    auto view = parser_.block(block);
    if (!view.has_value()) {
        ItchStoreError error = container_failure("block", view.error()).error();
        error.segment = segment;
        error.block = block;
        error.column = column_info(column).name;
        error.offset = view.error().offset;
        return std::unexpected(error);
    }
    auto values = decode_column(column, view->codec, view->payload, count);
    if (!values.has_value()) {
        ItchStoreError error = values.error();
        error.segment = segment;
        error.block = block;
        const std::uint64_t payload = offset_in(file_, view->payload);
        error.offset = payload;
        // A codec's offset is in bits from the chunk it read, which is the payload itself
        // unless zstd stood in between.
        if (error.codec.has_value() && view->codec != codec::CodecId::zstd) {
            error.offset = payload + (error.codec->offset / 8U);
        }
        return std::unexpected(error);
    }
    return values;
}

std::expected<ColumnView, ItchStoreError> ItchStoreReader::column_view(std::uint64_t segment,
                                                                      Column column) const {
    const SegmentEntry& entry = segments_.at(segment);
    const auto c = static_cast<std::size_t>(column);
    ColumnView out;
    out.block = entry.block[c];
    if (out.block == kNoIndex) {
        return out;
    }
    out.values = entry.values[c];
    auto view = parser_.block(out.block);
    if (!view.has_value()) {
        ItchStoreError error = container_failure("block", view.error()).error();
        error.segment = segment;
        error.block = out.block;
        error.column = column_info(column).name;
        error.offset = view.error().offset;
        return std::unexpected(error);
    }
    out.codec = view->codec;
    out.payload = view->payload;
    out.file_offset = offset_in(file_, view->payload);
    return out;
}

std::expected<SegmentColumns, ItchStoreError> ItchStoreReader::read_columns(
    std::uint64_t segment) const {
    if (segment >= segments_.size()) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::invalid_argument;
        error.field = "segment index";
        error.expected = segments_.size();
        error.actual = segment;
        return std::unexpected(error);
    }
    const SegmentEntry& entry = segments_[segment];
    SegmentColumns columns;
    auto types = read_column(segment, Column::type, entry.summary.records);
    if (!types.has_value()) {
        return std::unexpected(types.error());
    }
    columns[Column::type] = std::move(*types);
    auto counts = counts_from_types(columns[Column::type]);
    for (std::size_t c = 1; c < kColumnCount; ++c) {
        const auto column = static_cast<Column>(c);
        std::uint64_t count = counts[c];
        switch (column) {
            case Column::import_ref:
                count = static_cast<std::uint64_t>(
                    std::ranges::count(columns[Column::ref_distance], 0U));
                break;
            case Column::raw_bytes:
                count = 0;
                for (const std::uint64_t length : columns[Column::raw_length]) {
                    count += length;
                }
                break;
            case Column::stock_exception_record:
                // Free, but never more than the records.
                count = entry.values[c];
                if (count > entry.summary.records) {
                    ItchStoreError error;
                    error.kind = ItchStoreErrorKind::bad_segment;
                    error.field = "column value count";
                    error.segment = segment;
                    error.block = entry.block[c];
                    error.column = column_info(column).name;
                    error.expected = entry.summary.records;
                    error.actual = count;
                    return std::unexpected(error);
                }
                break;
            case Column::stock_exception_value:
                count = columns[Column::stock_exception_record].size() * itch::Stock{}.size();
                break;
            case Column::locate_run:
                // Free, but never more than the typed records: one run for each locate,
                // which the segment's decoder checks against the summary's bitmap.
                count = entry.values[c];
                if (count > entry.summary.typed_records) {
                    ItchStoreError error;
                    error.kind = ItchStoreErrorKind::bad_segment;
                    error.field = "column value count";
                    error.segment = segment;
                    error.block = entry.block[c];
                    error.column = column_info(column).name;
                    error.expected = entry.summary.typed_records;
                    error.actual = count;
                    return std::unexpected(error);
                }
                break;
            case Column::locate_run_records:
            case Column::locate_run_adds:
                count = columns[Column::locate_run].size();
                break;
            default:
                break;
        }
        auto values = read_column(segment, column, count);
        if (!values.has_value()) {
            return std::unexpected(values.error());
        }
        columns[column] = std::move(*values);
    }
    return columns;
}

StoreSizes ItchStoreReader::sizes() const {
    StoreSizes sizes;
    sizes.file_bytes = file_.size();
    sizes.header_bytes = kContainerHeaderSize;
    sizes.trailer_bytes = kTrailerSize;
    sizes.footer_fixed_bytes = kFooterFixedSize;
    sizes.block_entry_bytes = parser_.block_count() * kBlockEntrySize;
    sizes.section_entry_bytes = parser_.section_count() * kSectionEntrySize;
    for (std::uint64_t b = 0; b < parser_.block_count(); ++b) {
        const auto info = parser_.block_info(b);
        if (!info.has_value()) {
            continue;
        }
        const auto metadata = parse_column_metadata(info->metadata, 0);
        sizes.metadata_bytes += info->metadata.size();
        if (!metadata.has_value()) {
            continue;
        }
        const auto c = static_cast<std::size_t>(metadata->column);
        sizes.column_payload_bytes[c] += info->payload_size;
        sizes.column_metadata_bytes[c] += info->metadata.size();
        sizes.column_values[c] += metadata->values;
        ++sizes.column_blocks[c];
        if (info->codec == codec::CodecId::zstd) {
            ++sizes.column_zstd_blocks[c];
        }
    }
    for (std::uint64_t s = 0; s < parser_.section_count(); ++s) {
        const auto info = parser_.section_info(s);
        if (!info.has_value()) {
            continue;
        }
        if (info->name == kStoreSection) {
            sizes.store_section_bytes = info->size;
        } else if (info->name == kSymbolsSection) {
            sizes.symbols_section_bytes = info->size;
        } else if (info->name == kTailSection) {
            sizes.tail_section_bytes = info->size;
        }
    }
    return sizes;
}

std::expected<void, ItchStoreError> SegmentReader::seek(const ItchStoreReader& reader,
                                                        std::uint64_t segment) {
    auto columns = reader.read_columns(segment);
    if (!columns.has_value()) {
        return std::unexpected(columns.error());
    }
    return assembler_.load(segment, std::move(*columns), reader.segment(segment).summary,
                           reader.symbols());
}

}  // namespace tick_store::store
