#pragma once

// Helpers for the ITCH store's tests: stores written in memory from a source stream or
// from messages, the records a store gives back, and a store taken apart into its blocks
// and sections and put back together with valid CRCs, so a test can break one field of the
// store's own encoding and reach the check behind the checksums.

#include "test_bytes.hpp"
#include "tick_store/codec/registry.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/itch_store_writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tick_store::store::test {

using Bytes = std::vector<std::byte>;

// Writes every frame of `source` and its tail into an in-memory store.
inline Bytes store_of_source(std::span<const std::byte> source,
                             std::uint32_t segment_records = kDefaultSegmentRecords) {
    auto writer = ItchStoreWriter::in_memory({.segment_records = segment_records});
    if (!writer.has_value()) {
        throw std::runtime_error(to_string(writer.error()));
    }
    itch::FrameCursor cursor{source};
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            throw std::runtime_error("source frame does not decode");
        }
        if (auto added = writer->append(*message); !added.has_value()) {
            throw std::runtime_error(to_string(added.error()));
        }
    }
    if (auto finished = writer->finish(cursor.remaining()); !finished.has_value()) {
        throw std::runtime_error(to_string(finished.error()));
    }
    return writer->take_bytes();
}

// Writes messages into an in-memory store, with an empty tail.
inline Bytes store_of_messages(std::span<const itch::Message> messages,
                               std::uint32_t segment_records = kDefaultSegmentRecords) {
    auto writer = ItchStoreWriter::in_memory({.segment_records = segment_records});
    if (!writer.has_value()) {
        throw std::runtime_error(to_string(writer.error()));
    }
    for (const itch::Message& message : messages) {
        if (auto added = writer->append(message); !added.has_value()) {
            throw std::runtime_error(to_string(added.error()));
        }
    }
    if (auto finished = writer->finish({}); !finished.has_value()) {
        throw std::runtime_error(to_string(finished.error()));
    }
    return writer->take_bytes();
}

// A frame's payload as the encoder writes it, for comparing messages whatever they borrow.
inline Bytes payload_of(const itch::Message& message) {
    std::array<std::byte, itch::kMaxPayloadSize> scratch{};
    const auto written = itch::encode(message, scratch);
    if (!written.has_value()) {
        throw std::runtime_error("message does not encode");
    }
    return {scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(*written)};
}

// Every record of segment `segment`, as payloads, or the first error.
inline std::expected<std::vector<Bytes>, ItchStoreError> segment_payloads(
    const ItchStoreReader& reader, std::uint64_t segment) {
    SegmentReader records;
    if (auto sought = records.seek(reader, segment); !sought.has_value()) {
        return std::unexpected(sought.error());
    }
    std::vector<Bytes> out;
    for (;;) {
        auto message = records.next();
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        if (!message->has_value()) {
            return out;
        }
        out.push_back(payload_of(**message));
    }
}

// Every record of a store file in order, or the first error from opening or reading it.
inline std::expected<std::vector<Bytes>, ItchStoreError> all_payloads(
    std::span<const std::byte> file, ReadOptions options = {}) {
    auto reader = ItchStoreReader::open_bytes(file, options);
    if (!reader.has_value()) {
        return std::unexpected(reader.error());
    }
    std::vector<Bytes> out;
    for (std::uint64_t s = 0; s < reader->segment_count(); ++s) {
        auto records = segment_payloads(*reader, s);
        if (!records.has_value()) {
            return std::unexpected(records.error());
        }
        out.insert(out.end(), records->begin(), records->end());
    }
    return out;
}

// The payload of every message, in order.
inline std::vector<Bytes> payloads_of(std::span<const itch::Message> messages) {
    std::vector<Bytes> out;
    out.reserve(messages.size());
    for (const itch::Message& message : messages) {
        out.push_back(payload_of(message));
    }
    return out;
}

// A store file as its parts: blocks in file order, then sections in file order. A test
// changes one part and rebuilds; build_container() recomputes every CRC.
struct StoreParts {
    std::vector<ContainerPart> blocks;
    std::vector<ContainerPart> sections;

    [[nodiscard]] ContainerPart& section(std::string_view name) {
        for (ContainerPart& part : sections) {
            if (part.section_name == name) {
                return part;
            }
        }
        throw std::runtime_error("no such section");
    }

    // The block of `column` in `segment`, found from its metadata, if the segment has one.
    [[nodiscard]] std::optional<std::size_t> find_block(std::uint32_t segment,
                                                        Column column) const {
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const auto metadata = parse_column_metadata(blocks[b].metadata, 0);
            if (metadata.has_value() && metadata->segment == segment &&
                metadata->column == column) {
                return b;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t block_index(std::uint32_t segment, Column column) const {
        const auto found = find_block(segment, column);
        if (!found.has_value()) {
            throw std::runtime_error("no such block");
        }
        return *found;
    }

    [[nodiscard]] ContainerPart& block(std::uint32_t segment, Column column) {
        return blocks[block_index(segment, column)];
    }

    [[nodiscard]] Bytes build() const {
        std::vector<ContainerPart> parts = blocks;
        parts.insert(parts.end(), sections.begin(), sections.end());
        auto file = build_container(parts);
        if (!file.has_value()) {
            throw std::runtime_error(to_string(file.error()));
        }
        return std::move(*file);
    }
};

inline StoreParts parts_of(std::span<const std::byte> file) {
    auto parser = ContainerParser::open(file);
    if (!parser.has_value()) {
        throw std::runtime_error(to_string(parser.error()));
    }
    StoreParts parts;
    for (std::uint64_t b = 0; b < parser->block_count(); ++b) {
        const auto view = parser->block(b);
        if (!view.has_value()) {
            throw std::runtime_error(to_string(view.error()));
        }
        parts.blocks.push_back(ContainerPart{.section_name = {},
                                             .codec = view->codec,
                                             .metadata = {view->metadata.begin(), view->metadata.end()},
                                             .bytes = {view->payload.begin(), view->payload.end()}});
    }
    for (std::uint64_t s = 0; s < parser->section_count(); ++s) {
        const auto info = parser->section_info(s);
        if (!info.has_value()) {
            throw std::runtime_error(to_string(info.error()));
        }
        const auto view = parser->section(info->name);
        if (!view.has_value()) {
            throw std::runtime_error(to_string(view.error()));
        }
        parts.sections.push_back(ContainerPart{.section_name = std::string{info->name},
                                               .codec = codec::CodecId::raw,
                                               .metadata = {},
                                               .bytes = {view->bytes.begin(), view->bytes.end()}});
    }
    return parts;
}

// Writes `value` little-endian into `width` bytes at `at`, as every store field is stored.
inline void put_field(Bytes& bytes, std::size_t at, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.at(at + i) = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

inline std::uint64_t get_field(std::span<const std::byte> bytes, std::size_t at,
                               std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = width; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[at + i - 1]);
    }
    return value;
}

// Re-encodes one column of one segment from new values, keeping the column's codec choice.
inline void set_column(StoreParts& parts, std::uint32_t segment, Column column,
                       std::span<const std::uint64_t> values) {
    const std::size_t b = parts.block_index(segment, column);
    auto chunk = encode_column(column, values);
    if (!chunk.has_value()) {
        throw std::runtime_error(to_string(chunk.error()));
    }
    parts.blocks[b].codec = chunk->codec;
    parts.blocks[b].bytes = std::move(chunk->bytes);
    put_field(parts.blocks[b].metadata, kMetaValueCountOffset, 4, values.size());
}

// The decoded values of one column of one segment, as stored.
inline std::vector<std::uint64_t> column_values(std::span<const std::byte> file,
                                                std::uint64_t segment, Column column) {
    auto reader = ItchStoreReader::open_bytes(file);
    if (!reader.has_value()) {
        throw std::runtime_error(to_string(reader.error()));
    }
    auto columns = reader->read_columns(segment);
    if (!columns.has_value()) {
        throw std::runtime_error(to_string(columns.error()));
    }
    return (*columns)[column];
}

// --- Messages ---------------------------------------------------------------------------

inline itch::Stock stock_of(std::string_view text) {
    itch::Stock stock{};
    stock.fill(std::byte{' '});
    for (std::size_t i = 0; i < text.size() && i < stock.size(); ++i) {
        stock[i] = static_cast<std::byte>(text[i]);
    }
    return stock;
}

inline itch::MessageHeader header_at(std::uint16_t locate, std::uint64_t time) {
    return {.stock_locate = locate, .tracking_number = static_cast<std::uint16_t>(time % 7U),
            .timestamp_ns = time};
}

inline itch::Message add(std::uint16_t locate, std::uint64_t time, std::uint64_t reference,
                         std::string_view stock = "ZVZZT", char side = 'B',
                         std::uint32_t price = 100'0000, std::uint32_t shares = 100) {
    return itch::AddOrder{.header = header_at(locate, time),
                          .order_reference_number = reference,
                          .buy_sell_indicator = static_cast<std::byte>(side),
                          .shares = shares,
                          .stock = stock_of(stock),
                          .price = itch::Price4{price}};
}

inline itch::Message executed(std::uint16_t locate, std::uint64_t time, std::uint64_t reference,
                              std::uint32_t shares = 10, std::uint64_t match = 1) {
    return itch::OrderExecuted{.header = header_at(locate, time),
                               .order_reference_number = reference,
                               .executed_shares = shares,
                               .match_number = match};
}

inline itch::Message deleted(std::uint16_t locate, std::uint64_t time, std::uint64_t reference) {
    return itch::OrderDelete{.header = header_at(locate, time), .order_reference_number = reference};
}

inline itch::Message cancelled(std::uint16_t locate, std::uint64_t time, std::uint64_t reference,
                               std::uint32_t shares = 5) {
    return itch::OrderCancel{.header = header_at(locate, time),
                             .order_reference_number = reference,
                             .cancelled_shares = shares};
}

inline itch::Message replaced(std::uint16_t locate, std::uint64_t time, std::uint64_t original,
                              std::uint64_t created, std::uint32_t price = 101'0000) {
    return itch::OrderReplace{.header = header_at(locate, time),
                              .original_order_reference_number = original,
                              .new_order_reference_number = created,
                              .shares = 50,
                              .price = itch::Price4{price}};
}

inline itch::Message directory(std::uint16_t locate, std::uint64_t time, std::string_view stock) {
    itch::StockDirectory message{};
    message.header = header_at(locate, time);
    message.stock = stock_of(stock);
    message.market_category = std::byte{'Q'};
    message.financial_status_indicator = std::byte{'N'};
    message.round_lot_size = 100;
    message.round_lots_only = std::byte{'N'};
    message.issue_classification = std::byte{'C'};
    message.issue_sub_type = {std::byte{'Z'}, std::byte{' '}};
    message.authenticity = std::byte{'P'};
    message.short_sale_threshold_indicator = std::byte{'N'};
    message.ipo_flag = std::byte{'N'};
    message.luld_reference_price_tier = std::byte{'1'};
    message.etp_flag = std::byte{'N'};
    message.etp_leverage_factor = 0;
    message.inverse_indicator = std::byte{'N'};
    return message;
}

}  // namespace tick_store::store::test
