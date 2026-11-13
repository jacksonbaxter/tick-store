#include "tick_store/store/itch_store_writer.hpp"

#include "tick_store/codec/registry.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/container_writer.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/itch_segment.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
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
    error.container = cause;
    return std::unexpected(error);
}

[[nodiscard]] std::expected<void, ItchStoreError> check_options(const ItchStoreOptions& options) {
    if (options.segment_records == 0 || options.segment_records > kMaxSegmentRecords) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::invalid_argument;
        error.field = "segment records";
        error.expected = kMaxSegmentRecords;
        error.actual = options.segment_records;
        return std::unexpected(error);
    }
    return {};
}

}  // namespace

ItchStoreWriter::ItchStoreWriter(ItchStoreOptions options) : options_(options) {}
ItchStoreWriter::ItchStoreWriter(ItchStoreWriter&&) noexcept = default;
ItchStoreWriter::~ItchStoreWriter() = default;

std::expected<ItchStoreWriter, ItchStoreError> ItchStoreWriter::create(
    const std::filesystem::path& path, Durability durability, ItchStoreOptions options,
    FileOps& ops) {
    if (auto checked = check_options(options); !checked.has_value()) {
        return std::unexpected(checked.error());
    }
    auto file = ContainerWriter::create(path, durability, ops);
    if (!file.has_value()) {
        return container_failure("create", file.error());
    }
    ItchStoreWriter writer{options};
    writer.file_.emplace(std::move(*file));
    return writer;
}

std::expected<ItchStoreWriter, ItchStoreError> ItchStoreWriter::in_memory(
    ItchStoreOptions options) {
    if (auto checked = check_options(options); !checked.has_value()) {
        return std::unexpected(checked.error());
    }
    ItchStoreWriter writer{options};
    const auto header = encode_container_header();
    writer.memory_.assign(header.begin(), header.end());
    return writer;
}

std::expected<void, ItchStoreError> ItchStoreWriter::add_block(codec::CodecId codec,
                                                               std::span<const std::byte> metadata,
                                                               std::span<const std::byte> payload) {
    if (file_.has_value()) {
        if (auto added = file_->add_block(codec, metadata, payload); !added.has_value()) {
            return container_failure("add block", added.error());
        }
        return {};
    }
    if (auto added = layout_.add_block(codec, metadata, payload); !added.has_value()) {
        return container_failure("add block", added.error());
    }
    memory_.insert(memory_.end(), payload.begin(), payload.end());
    return {};
}

std::expected<void, ItchStoreError> ItchStoreWriter::add_section(std::string_view name,
                                                                 std::span<const std::byte> bytes) {
    if (file_.has_value()) {
        if (auto added = file_->add_section(name, bytes); !added.has_value()) {
            return container_failure("add section", added.error());
        }
        return {};
    }
    if (auto added = layout_.add_section(name, bytes); !added.has_value()) {
        return container_failure("add section", added.error());
    }
    memory_.insert(memory_.end(), bytes.begin(), bytes.end());
    return {};
}

std::expected<void, ItchStoreError> ItchStoreWriter::write_segment() {
    SegmentSummary summary;
    const std::uint64_t first_record = records_ - builder_.records();
    SegmentColumns columns = builder_.finish(first_record, summary);
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        const std::vector<std::uint64_t>& values = columns.values[c];
        if (values.empty()) {
            continue;
        }
        const auto column = static_cast<Column>(c);
        if (values.size() > kMax32) {
            ItchStoreError error;
            error.kind = ItchStoreErrorKind::invalid_argument;
            error.field = "column value count";
            error.segment = segments_;
            error.column = column_info(column).name;
            error.expected = kMax32;
            error.actual = values.size();
            return std::unexpected(error);
        }
        auto pieces = encode_column(column, values);
        if (!pieces.has_value()) {
            ItchStoreError error = pieces.error();
            error.segment = segments_;
            return std::unexpected(error);
        }
        for (std::size_t p = 0; p < pieces->size(); ++p) {
            const ColumnChunk& piece = (*pieces)[p];
            // At most kMax32 values, so at most 2^18 pieces of 2^14; the column's own count
            // keeps it far below that.
            ColumnMetadata metadata{.segment = static_cast<std::uint32_t>(segments_),
                                    .column = column,
                                    .piece = static_cast<std::uint16_t>(p),
                                    .values = piece.values,
                                    .summary = std::nullopt};
            if (column == Column::type && p == 0) {
                metadata.summary = summary;
            }
            const std::vector<std::byte> encoded = encode_column_metadata(metadata);
            if (auto added = add_block(piece.codec, encoded, piece.bytes); !added.has_value()) {
                return added;
            }
        }
    }
    ++segments_;
    return {};
}

std::expected<void, ItchStoreError> ItchStoreWriter::append(const itch::Message& message) {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    if (finished_) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::invalid_argument;
        error.field = "append after finish";
        return std::unexpected(error);
    }
    if (auto added = builder_.add(message, *symbols_); !added.has_value()) {
        ItchStoreError error = added.error();
        error.record = records_;
        return std::unexpected(error);
    }
    ++records_;
    if (builder_.records() == options_.segment_records) {
        return latch(write_segment());
    }
    return {};
}

std::expected<void, ItchStoreError> ItchStoreWriter::latch(
    std::expected<void, ItchStoreError> result) {
    if (!result.has_value()) {
        failed_ = result.error();
        // Unlinks the partial file now rather than when the writer goes.
        file_.reset();
    }
    return result;
}

std::expected<void, ItchStoreError> ItchStoreWriter::finish(std::span<const std::byte> tail) {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    if (finished_) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::invalid_argument;
        error.field = "finish twice";
        return std::unexpected(error);
    }
    finished_ = true;
    return latch(write_last(tail));
}

std::expected<void, ItchStoreError> ItchStoreWriter::write_last(std::span<const std::byte> tail) {
    if (builder_.records() > 0) {
        if (auto written = write_segment(); !written.has_value()) {
            return written;
        }
    }
    const StoreHeader header{.segment_records = options_.segment_records,
                             .record_count = records_,
                             .segment_count = segments_};
    if (auto added = add_section(kStoreSection, encode_store_header(header)); !added) {
        return added;
    }
    if (auto added = add_section(kSymbolsSection, symbols_->encode()); !added) {
        return added;
    }
    if (auto added = add_section(kTailSection, tail); !added) {
        return added;
    }
    if (file_.has_value()) {
        if (auto committed = file_->commit(); !committed.has_value()) {
            return container_failure("commit", committed.error());
        }
        return {};
    }
    const auto footer = layout_.encode_footer_and_trailer();
    memory_.insert(memory_.end(), footer.begin(), footer.end());
    return {};
}

std::vector<std::byte> ItchStoreWriter::take_bytes() {
    if (file_.has_value() || !finished_ || failed_.has_value()) {
        return {};
    }
    return std::move(memory_);
}

}  // namespace tick_store::store
