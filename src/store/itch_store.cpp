#include "tick_store/store/itch_store.hpp"

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/registry.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/zstd_frame.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/store_error.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

using codec::CodecId;

[[nodiscard]] std::uint64_t field(std::span<const std::byte> bytes, std::size_t offset,
                                  std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = width; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + i - 1]);
    }
    return value;
}

void put(std::vector<std::byte>& out, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
    }
}

[[nodiscard]] ItchStoreError fail(ItchStoreErrorKind kind, std::string_view field_name,
                                  std::uint64_t offset, std::uint64_t expected,
                                  std::uint64_t actual) {
    ItchStoreError error;
    error.kind = kind;
    error.field = field_name;
    error.offset = offset;
    error.expected = expected;
    error.actual = actual;
    return error;
}

[[nodiscard]] std::string_view kind_name(ItchStoreErrorKind kind) noexcept {
    switch (kind) {
        case ItchStoreErrorKind::container:
            return "container";
        case ItchStoreErrorKind::codec:
            return "codec";
        case ItchStoreErrorKind::bad_section:
            return "bad section";
        case ItchStoreErrorKind::bad_metadata:
            return "bad block metadata";
        case ItchStoreErrorKind::bad_segment:
            return "bad segment";
        case ItchStoreErrorKind::bad_value:
            return "bad value";
        case ItchStoreErrorKind::invalid_argument:
            return "invalid argument";
    }
    return "unknown";
}

// The most bytes a column's coded chunk may take for `count` values, the bound a zstd
// payload's inner size is held to before anything is allocated. No codec here takes more
// than 9 bytes a value plus a few hundred bytes a block; dictionaries nest two PFOR blocks.
[[nodiscard]] std::uint64_t max_inner_bytes(Column column, std::uint64_t count) noexcept {
    if (column_info(column).codec == ColumnCodec::raw_bytes) {
        return count;
    }
    constexpr std::uint64_t per_value = 32;
    constexpr std::uint64_t per_block = 1024;
    const std::uint64_t blocks = (count / kColumnBlockValues) + 1;
    return (count * per_value) + (blocks * per_block);
}

[[nodiscard]] codec::PforOptions pfor_options(ColumnCodec kind) noexcept {
    switch (kind) {
        case ColumnCodec::pfor_head_delta:
            return {.transform = codec::IntTransform::head_delta};
        case ColumnCodec::pfor_for:
            return {.transform = codec::IntTransform::none};
        case ColumnCodec::pfor_for_scaled:
            return {.transform = codec::IntTransform::none, .adaptive_scale = true};
        case ColumnCodec::pfor_delta_scaled:
            return {.transform = codec::IntTransform::delta, .adaptive_scale = true};
        default:
            return {.transform = codec::IntTransform::delta};
    }
}

[[nodiscard]] std::expected<std::vector<std::byte>, codec::CodecError> code_pfor(
    ColumnCodec kind, std::span<const std::uint64_t> values) {
    std::vector<std::byte> out;
    if (auto done = codec::encode_pfor(values, kColumnBlockValues, pfor_options(kind), out);
        !done) {
        return std::unexpected(done.error());
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<std::byte>, codec::CodecError> code_values(
    ColumnCodec kind, std::span<const std::uint64_t> values) {
    std::vector<std::byte> out;
    switch (kind) {
        case ColumnCodec::dictionary: {
            if (auto done = codec::encode_dict(values, kColumnBlockValues, out); !done) {
                return std::unexpected(done.error());
            }
            return out;
        }
        case ColumnCodec::run_length: {
            if (auto done = codec::encode_rle(values, kColumnBlockValues, out); !done) {
                return std::unexpected(done.error());
            }
            return out;
        }
        case ColumnCodec::raw_bytes: {
            out.reserve(values.size());
            for (const std::uint64_t value : values) {
                out.push_back(static_cast<std::byte>(value & 0xFFU));
            }
            return out;
        }
        case ColumnCodec::pfor_reference: {
            auto head = code_pfor(ColumnCodec::pfor_head_delta, values);
            auto delta = code_pfor(ColumnCodec::pfor_delta, values);
            if (!head.has_value() || !delta.has_value()) {
                return !head.has_value() ? head : delta;
            }
            return head->size() < delta->size() ? std::move(head) : std::move(delta);
        }
        default:
            return code_pfor(kind, values);
    }
}

[[nodiscard]] codec::BlockDecoder block_decoder(CodecId id) noexcept {
    const codec::CodecInfo* info = codec::find_codec(static_cast<std::uint16_t>(id));
    return info != nullptr ? info->decoder : nullptr;
}

constexpr std::array<std::uint8_t, 11> kRareTypes{'S', 'R', 'H', 'Y', 'L', 'W',
                                                  'K', 'J', 'h', 'N', 'O'};
constexpr std::array<std::uint8_t, 5> kDerivedStockTypes{'A', 'F', 'P', 'Q', 'I'};

}  // namespace

codec::CodecId primary_codec(Column column) noexcept {
    switch (column_info(column).codec) {
        case ColumnCodec::dictionary:
            return CodecId::dictionary;
        case ColumnCodec::run_length:
            return CodecId::run_length;
        case ColumnCodec::raw_bytes:
            return CodecId::raw;
        default:
            return CodecId::pfor;
    }
}

std::string to_string(const ItchStoreError& error) {
    std::string text = std::format("{}: {}", kind_name(error.kind), error.field);
    if (error.segment != kNoIndex) {
        text += std::format(", segment {}", error.segment);
    }
    if (!error.column.empty()) {
        text += std::format(", column {}", error.column);
    }
    if (error.block != kNoIndex) {
        text += std::format(", block {}", error.block);
    }
    if (error.record != kNoIndex) {
        text += std::format(", record {}", error.record);
    }
    if (error.index != kNoIndex) {
        text += std::format(", value {}", error.index);
    }
    if (error.offset != kNoIndex) {
        text += std::format(", file byte {}", error.offset);
    }
    text += std::format(": expected {}, have {}", error.expected, error.actual);
    if (error.container.has_value()) {
        text += std::format(" ({})", to_string(*error.container));
    }
    if (error.codec.has_value()) {
        text += std::format(" ({})", to_string(*error.codec));
    }
    return text;
}

// --- Sections -------------------------------------------------------------------------

std::vector<std::byte> encode_store_header(const StoreHeader& header) {
    std::vector<std::byte> out;
    out.reserve(kStoreSectionSize);
    put(out, kItchStoreVersion, 4);
    put(out, header.segment_records, 4);
    put(out, header.record_count, 8);
    put(out, header.segment_count, 8);
    return out;
}

std::expected<StoreHeader, ItchStoreError> parse_store_header(std::span<const std::byte> bytes,
                                                              std::uint64_t section_offset) {
    if (bytes.size() != kStoreSectionSize) {
        return std::unexpected(fail(ItchStoreErrorKind::bad_section, "store section size",
                                    section_offset, kStoreSectionSize, bytes.size()));
    }
    const std::uint64_t version = field(bytes, kStoreVersionOffset, 4);
    if (version != kItchStoreVersion) {
        return std::unexpected(fail(ItchStoreErrorKind::bad_section, "store version",
                                    section_offset + kStoreVersionOffset, kItchStoreVersion,
                                    version));
    }
    StoreHeader header;
    const std::uint64_t segment_records = field(bytes, kStoreSegmentRecordsOffset, 4);
    if (segment_records == 0 || segment_records > kMaxSegmentRecords) {
        return std::unexpected(fail(ItchStoreErrorKind::bad_section, "segment records",
                                    section_offset + kStoreSegmentRecordsOffset,
                                    kMaxSegmentRecords, segment_records));
    }
    header.segment_records = static_cast<std::uint32_t>(segment_records);
    header.record_count = field(bytes, kStoreRecordCountOffset, 8);
    header.segment_count = field(bytes, kStoreSegmentCountOffset, 8);
    // Rounded up without the sum that could wrap.
    const std::uint64_t segments = (header.record_count / segment_records) +
                                   (header.record_count % segment_records != 0 ? 1U : 0U);
    if (header.segment_count != segments) {
        return std::unexpected(fail(ItchStoreErrorKind::bad_section, "segment count",
                                    section_offset + kStoreSegmentCountOffset, segments,
                                    header.segment_count));
    }
    return header;
}

SymbolTable::SymbolTable() : slot_(std::size_t{kMax16} + 1, kAbsent) {}

const itch::Stock* SymbolTable::find(std::uint16_t locate) const noexcept {
    const std::uint32_t slot = slot_[locate];
    return slot == kAbsent ? nullptr : &stocks_[slot];
}

const itch::Stock& SymbolTable::learn(std::uint16_t locate, const itch::Stock& stock) {
    std::uint32_t& slot = slot_[locate];
    if (slot == kAbsent) {
        slot = static_cast<std::uint32_t>(stocks_.size());
        stocks_.push_back(stock);
        locates_.push_back(locate);
    }
    return stocks_[slot];
}

std::vector<std::byte> SymbolTable::encode() const {
    std::vector<std::byte> out;
    out.reserve(kSymbolsCountSize + (locates_.size() * kSymbolEntrySize));
    put(out, locates_.size(), kSymbolsCountSize);
    for (std::size_t locate = 0; locate < slot_.size(); ++locate) {
        const std::uint32_t slot = slot_[locate];
        if (slot == kAbsent) {
            continue;
        }
        put(out, locate, 2);
        out.insert(out.end(), stocks_[slot].begin(), stocks_[slot].end());
    }
    return out;
}

std::expected<SymbolTable, ItchStoreError> SymbolTable::parse(std::span<const std::byte> bytes,
                                                              std::uint64_t section_offset) {
    if (bytes.size() < kSymbolsCountSize) {
        return std::unexpected(fail(ItchStoreErrorKind::bad_section, "symbols size",
                                    section_offset, kSymbolsCountSize, bytes.size()));
    }
    const std::uint64_t count = field(bytes, 0, kSymbolsCountSize);
    const std::uint64_t room = (bytes.size() - kSymbolsCountSize) / kSymbolEntrySize;
    if (count != room || (bytes.size() - kSymbolsCountSize) % kSymbolEntrySize != 0) {
        return std::unexpected(fail(ItchStoreErrorKind::bad_section, "symbols count",
                                    section_offset, room, count));
    }
    SymbolTable table;
    table.locates_.reserve(static_cast<std::size_t>(count));
    table.stocks_.reserve(static_cast<std::size_t>(count));
    std::optional<std::uint64_t> previous;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto at = static_cast<std::size_t>(kSymbolsCountSize + (i * kSymbolEntrySize));
        const std::uint64_t locate = field(bytes, at, 2);
        if (previous.has_value() && locate <= *previous) {
            return std::unexpected(fail(ItchStoreErrorKind::bad_section, "symbols locate order",
                                        section_offset + at, *previous + 1, locate));
        }
        previous = locate;
        itch::Stock stock{};
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at + 2), stock.size(),
                    stock.begin());
        table.learn(static_cast<std::uint16_t>(locate), stock);
    }
    return table;
}

bool operator==(const SymbolTable& a, const SymbolTable& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t locate = 0; locate < a.slot_.size(); ++locate) {
        const auto* left = a.find(static_cast<std::uint16_t>(locate));
        const auto* right = b.find(static_cast<std::uint16_t>(locate));
        if ((left == nullptr) != (right == nullptr) || (left != nullptr && *left != *right)) {
            return false;
        }
    }
    return true;
}

// --- Block metadata -------------------------------------------------------------------

std::vector<std::byte> encode_column_metadata(const ColumnMetadata& metadata) {
    std::vector<std::byte> out;
    put(out, metadata.segment, 4);
    put(out, static_cast<std::uint16_t>(metadata.column), 2);
    put(out, metadata.piece, 2);
    put(out, metadata.values, 4);
    if (metadata.summary.has_value()) {
        const SegmentSummary& s = *metadata.summary;
        put(out, s.first_record, 8);
        put(out, s.first_time, 8);
        put(out, s.last_time, 8);
        put(out, s.min_time, 8);
        put(out, s.max_time, 8);
        put(out, s.records, 4);
        put(out, s.typed_records, 4);
        put(out, s.plain ? 1U : 0U, 1);
        put(out, s.locates.size(), 2);
        out.insert(out.end(), s.locates.begin(), s.locates.end());
    }
    return out;
}

std::expected<ColumnMetadata, ItchStoreError> parse_column_metadata(
    std::span<const std::byte> bytes, std::uint64_t metadata_offset) {
    const auto refuse = [&](std::string_view name, std::size_t at, std::uint64_t expected,
                            std::uint64_t actual) {
        return std::unexpected(
            fail(ItchStoreErrorKind::bad_metadata, name, metadata_offset + at, expected, actual));
    };
    if (bytes.size() < kColumnMetadataSize) {
        return refuse("metadata size", 0, kColumnMetadataSize, bytes.size());
    }
    ColumnMetadata metadata;
    metadata.segment = static_cast<std::uint32_t>(field(bytes, kMetaSegmentOffset, 4));
    const std::uint64_t column = field(bytes, kMetaColumnOffset, 2);
    if (column >= kColumnCount) {
        return refuse("column id", kMetaColumnOffset, kColumnCount - 1, column);
    }
    metadata.column = static_cast<Column>(column);
    metadata.piece = static_cast<std::uint16_t>(field(bytes, kMetaPieceOffset, 2));
    metadata.values = static_cast<std::uint32_t>(field(bytes, kMetaValueCountOffset, 4));
    if (metadata.values == 0) {
        return refuse("value count", kMetaValueCountOffset, 1, 0);
    }
    if (metadata.column != Column::type || metadata.piece != 0) {
        if (bytes.size() != kColumnMetadataSize) {
            return refuse("metadata size", 0, kColumnMetadataSize, bytes.size());
        }
        return metadata;
    }
    if (bytes.size() < kSegmentMetadataFixedSize) {
        return refuse("metadata size", 0, kSegmentMetadataFixedSize, bytes.size());
    }
    SegmentSummary s;
    s.records = static_cast<std::uint32_t>(field(bytes, kMetaRecordsOffset, 4));
    s.first_record = field(bytes, kMetaFirstRecordOffset, 8);
    s.first_time = field(bytes, kMetaFirstTimeOffset, 8);
    s.last_time = field(bytes, kMetaLastTimeOffset, 8);
    s.min_time = field(bytes, kMetaMinTimeOffset, 8);
    s.max_time = field(bytes, kMetaMaxTimeOffset, 8);
    s.typed_records = static_cast<std::uint32_t>(field(bytes, kMetaTypedOffset, 4));
    const std::uint64_t plain = field(bytes, kMetaPlainOffset, 1);
    if (plain > 1) {
        return refuse("plain records", kMetaPlainOffset, 1, plain);
    }
    s.plain = plain == 1;
    const std::uint64_t bitmap = field(bytes, kMetaBitmapSizeOffset, 2);
    if (bitmap > kMaxLocateBitmapBytes) {
        return refuse("locate bitmap size", kMetaBitmapSizeOffset, kMaxLocateBitmapBytes, bitmap);
    }
    if (bytes.size() != kSegmentMetadataFixedSize + bitmap) {
        return refuse("metadata size", 0, kSegmentMetadataFixedSize + bitmap, bytes.size());
    }
    if (s.records < metadata.values || s.records > kMaxSegmentRecords) {
        return refuse("segment records", kMetaRecordsOffset, metadata.values, s.records);
    }
    if (s.typed_records > s.records) {
        return refuse("typed records", kMetaTypedOffset, s.records, s.typed_records);
    }
    if (s.max_time > kMax48) {
        return refuse("max time", kMetaMaxTimeOffset, kMax48, s.max_time);
    }
    if (s.min_time > s.max_time) {
        return refuse("min time", kMetaMinTimeOffset, s.max_time, s.min_time);
    }
    if (s.first_time < s.min_time || s.first_time > s.max_time) {
        return refuse("first time", kMetaFirstTimeOffset, s.max_time, s.first_time);
    }
    if (s.last_time < s.min_time || s.last_time > s.max_time) {
        return refuse("last time", kMetaLastTimeOffset, s.max_time, s.last_time);
    }
    if (s.typed_records == 0 && s.max_time != 0) {
        return refuse("max time", kMetaMaxTimeOffset, 0, s.max_time);
    }
    if ((s.typed_records == 0) != (bitmap == 0)) {
        return refuse("locate bitmap size", kMetaBitmapSizeOffset, s.typed_records == 0 ? 0 : 1,
                      bitmap);
    }
    s.locates.assign(bytes.begin() + kMetaBitmapOffset, bytes.end());
    if (!s.locates.empty() && s.locates.back() == std::byte{0}) {
        return refuse("locate bitmap last byte", kMetaBitmapOffset + bitmap - 1, 1, 0);
    }
    metadata.summary = std::move(s);
    return metadata;
}

// --- Column values --------------------------------------------------------------------

std::uint64_t pieces_of(Column column, std::uint64_t values) noexcept {
    if (values == 0) {
        return 0;
    }
    if (column_info(column).codec == ColumnCodec::raw_bytes) {
        return 1;
    }
    return (values + kPieceValues - 1) / kPieceValues;
}

std::expected<std::vector<ColumnChunk>, ItchStoreError> encode_column(
    Column column, std::span<const std::uint64_t> values) {
    const ColumnInfo& info = column_info(column);
    const auto refuse = [&](std::string_view name, std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error = fail(ItchStoreErrorKind::invalid_argument, name, kNoIndex,
                                    expected, actual);
        error.column = info.name;
        return std::unexpected(error);
    };
    if (values.empty()) {
        return refuse("column values", 1, 0);
    }
    for (const std::uint64_t value : values) {
        if (value > info.max_value) {
            return refuse("column value", info.max_value, value);
        }
    }
    const std::uint64_t pieces = pieces_of(column, values.size());
    const std::uint64_t per_piece =
        info.codec == ColumnCodec::raw_bytes ? values.size() : std::uint64_t{kPieceValues};
    std::vector<ColumnChunk> out;
    out.reserve(static_cast<std::size_t>(pieces));
    for (std::uint64_t p = 0; p < pieces; ++p) {
        const std::uint64_t first = p * per_piece;
        const std::span<const std::uint64_t> part =
            values.subspan(static_cast<std::size_t>(first),
                           static_cast<std::size_t>(std::min(per_piece, values.size() - first)));
        auto coded = code_values(info.codec, part);
        if (!coded.has_value()) {
            ItchStoreError error =
                fail(ItchStoreErrorKind::invalid_argument, "column encoder", kNoIndex, 0, 0);
            error.column = info.name;
            error.codec = coded.error();
            return std::unexpected(error);
        }
        // Every piece holds at most a column's values, which the writer holds to 32 bits.
        ColumnChunk chunk{.codec = primary_codec(column), .bytes = std::move(*coded),
                          .values = static_cast<std::uint32_t>(part.size())};
        if (info.zstd) {
            std::vector<std::byte> wrapped;
            if (codec::encode_zstd_payload(static_cast<std::uint16_t>(chunk.codec), chunk.bytes,
                                           wrapped)
                    .has_value() &&
                wrapped.size() < chunk.bytes.size()) {
                chunk.codec = CodecId::zstd;
                chunk.bytes = std::move(wrapped);
            }
        }
        out.push_back(std::move(chunk));
    }
    return out;
}

std::expected<ChunkBytes, ItchStoreError> unwrap_column(Column column, codec::CodecId codec_id,
                                                         std::span<const std::byte> payload,
                                                         std::uint64_t count) {
    const ColumnInfo& info = column_info(column);
    const CodecId primary = primary_codec(column);
    const auto refuse = [&](std::string_view name, std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error = fail(ItchStoreErrorKind::bad_segment, name, kNoIndex, expected, actual);
        error.column = info.name;
        return std::unexpected(error);
    };
    if (codec_id != primary && (!info.zstd || codec_id != CodecId::zstd)) {
        return refuse("column codec", static_cast<std::uint64_t>(primary),
                      static_cast<std::uint64_t>(codec_id));
    }
    ChunkBytes out;
    out.payload = payload;
    if (codec_id == CodecId::zstd) {
        auto unwrapped = codec::decode_zstd_payload(payload, max_inner_bytes(column, count));
        if (!unwrapped.has_value()) {
            ItchStoreError error =
                fail(ItchStoreErrorKind::codec, unwrapped.error().field, kNoIndex, 0, 0);
            error.column = info.name;
            error.codec = unwrapped.error();
            return std::unexpected(error);
        }
        if (unwrapped->inner_codec != static_cast<std::uint16_t>(primary)) {
            return refuse("zstd inner codec", static_cast<std::uint64_t>(primary),
                          unwrapped->inner_codec);
        }
        out.zstd = true;
        out.inflated = std::move(unwrapped->bytes);
    }
    return out;
}

codec::BlockDecoder column_block_decoder(Column column) noexcept {
    if (column_info(column).codec == ColumnCodec::raw_bytes) {
        return nullptr;
    }
    return block_decoder(primary_codec(column));
}

std::expected<std::vector<std::uint64_t>, ItchStoreError> decode_column(
    Column column, codec::CodecId codec_id, std::span<const std::byte> payload,
    std::uint64_t count) {
    const ColumnInfo& info = column_info(column);
    const CodecId primary = primary_codec(column);
    const auto refuse = [&](ItchStoreErrorKind kind, std::string_view name,
                            std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error = fail(kind, name, kNoIndex, expected, actual);
        error.column = info.name;
        return std::unexpected(error);
    };
    auto unwrapped = unwrap_column(column, codec_id, payload, count);
    if (!unwrapped.has_value()) {
        return std::unexpected(unwrapped.error());
    }
    const std::span<const std::byte> bytes = unwrapped->bytes();
    std::vector<std::uint64_t> values;
    if (primary == CodecId::raw) {
        if (bytes.size() != count) {
            return refuse(ItchStoreErrorKind::bad_segment, "column bytes", count, bytes.size());
        }
        values.reserve(bytes.size());
        for (const std::byte value : bytes) {
            values.push_back(std::to_integer<std::uint64_t>(value));
        }
        return values;
    }
    if (auto decoded = codec::decode_column(bytes, count, values, block_decoder(primary));
        !decoded.has_value()) {
        ItchStoreError error = fail(ItchStoreErrorKind::codec, decoded.error().field, kNoIndex, 0, 0);
        error.column = info.name;
        error.codec = decoded.error();
        return std::unexpected(error);
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] > info.max_value) {
            ItchStoreError error = fail(ItchStoreErrorKind::bad_value, "column value", kNoIndex,
                                        info.max_value, values[i]);
            error.column = info.name;
            error.index = i;
            return std::unexpected(error);
        }
    }
    return values;
}

bool valid_type_symbol(std::uint64_t symbol) noexcept {
    return symbol == 0 ||
           (symbol <= kMax8 && itch::wire_size_of(static_cast<std::byte>(symbol)).has_value());
}

bool is_rare_type(std::uint8_t type) noexcept {
    return std::ranges::find(kRareTypes, type) != kRareTypes.end();
}

bool has_derived_stock(std::uint8_t type) noexcept {
    return std::ranges::find(kDerivedStockTypes, type) != kDerivedStockTypes.end();
}

std::array<std::uint64_t, kColumnCount> counts_from_types(std::span<const std::uint64_t> types) {
    std::array<std::uint64_t, 256> per_type{};
    for (const std::uint64_t type : types) {
        ++per_type[type & 0xFFU];
    }
    return counts_from_type_counts(per_type);
}

std::array<std::uint64_t, kColumnCount> counts_from_type_counts(
    const std::array<std::uint64_t, 256>& per_type) {
    std::uint64_t records = 0;
    for (const std::uint64_t n : per_type) {
        records += n;
    }
    std::array<std::uint64_t, kColumnCount> counts{};
    const auto add = [&](Column column, std::uint64_t n) {
        counts[static_cast<std::size_t>(column)] += n;
    };
    const auto of = [&](char type) { return per_type[static_cast<unsigned char>(type)]; };
    const std::uint64_t raw = per_type[0];
    const std::uint64_t typed = records - raw;
    add(Column::type, records);
    add(Column::locate, typed);
    add(Column::timestamp, typed);
    add(Column::tracking, typed);
    const std::uint64_t adds = of('A') + of('F');
    add(Column::creation_ref, adds + of('U'));
    add(Column::ref_distance, of('E') + of('C') + of('X') + of('D') + of('U'));
    add(Column::add_side, adds);
    add(Column::add_price, adds);
    add(Column::add_shares, adds);
    add(Column::mpid, of('F'));
    add(Column::reduce_shares, of('E') + of('C') + of('X'));
    add(Column::match, of('E') + of('C') + of('P') + of('Q') + of('B'));
    add(Column::printable, of('C'));
    add(Column::exec_price, of('C'));
    add(Column::replace_shares, of('U'));
    add(Column::replace_price, of('U'));
    add(Column::trade_ref, of('P'));
    add(Column::trade_side, of('P'));
    add(Column::trade_shares, of('P'));
    add(Column::trade_price, of('P'));
    add(Column::cross_shares, of('Q'));
    add(Column::cross_price, of('Q'));
    add(Column::cross_type, of('Q'));
    add(Column::imbalance_shares, 2 * of('I'));
    add(Column::imbalance_price, 3 * of('I'));
    add(Column::imbalance_flags, of('I'));
    add(Column::mwcb_level, 3 * of('V'));
    for (const std::uint8_t type : kRareTypes) {
        const auto size = itch::wire_size_of(static_cast<std::byte>(type));
        add(Column::rare_bytes, per_type[type] * (size.value_or(kItchHeaderBytes) - kItchHeaderBytes));
    }
    add(Column::raw_length, raw);
    add(Column::time_base, time_base_count(records, typed));
    add(Column::seek, seek_value_count(records));
    return counts;
}

// --- The locate runs --------------------------------------------------------------------

std::uint64_t locate_runs_of(const SegmentSummary& summary) noexcept {
    std::uint64_t runs = 0;
    for (const std::byte b : summary.locates) {
        runs += static_cast<std::uint64_t>(std::popcount(std::to_integer<unsigned>(b)));
    }
    return runs;
}

std::expected<void, ItchStoreError> check_run_directory(
    std::span<const std::uint64_t> ids, std::span<const std::uint64_t> starts,
    std::span<const std::uint64_t> add_starts, std::span<const std::uint64_t> import_starts,
    std::uint64_t typed, std::uint64_t adds, std::uint64_t imports) {
    const auto refuse = [&](Column column, std::string_view name, std::uint64_t index,
                            std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error = fail(ItchStoreErrorKind::bad_value, name, kNoIndex, expected, actual);
        error.column = column_info(column).name;
        error.index = index;
        return std::unexpected(error);
    };
    const std::size_t runs = ids.size();
    for (const auto& [column, values] :
         {std::pair{Column::locate_run_start, starts}, std::pair{Column::locate_run_add_start, add_starts},
          std::pair{Column::locate_run_import_start, import_starts}}) {
        if (values.size() != runs) {
            return refuse(column, "locate run count", kNoIndex, runs, values.size());
        }
    }
    for (std::size_t r = 0; r < runs; ++r) {
        if (ids[r] > kMax16 || (r > 0 && ids[r] <= ids[r - 1])) {
            return refuse(Column::locate_run, "locate run order", r, r == 0 ? 0 : ids[r - 1] + 1,
                          ids[r]);
        }
        // The first run starts at 0, and each later one after the one before, below `typed`.
        const std::uint64_t floor = r == 0 ? 0 : starts[r - 1] + 1;
        if ((r == 0 && starts[r] != 0) || starts[r] < floor || starts[r] >= typed) {
            return refuse(Column::locate_run_start, "locate run start", r, floor, starts[r]);
        }
        const std::uint64_t add_floor = r == 0 ? 0 : add_starts[r - 1];
        if ((r == 0 && add_starts[r] != 0) || add_starts[r] < add_floor || add_starts[r] > adds) {
            return refuse(Column::locate_run_add_start, "locate run add start", r, add_floor,
                          add_starts[r]);
        }
        const std::uint64_t import_floor = r == 0 ? 0 : import_starts[r - 1];
        if ((r == 0 && import_starts[r] != 0) || import_starts[r] < import_floor ||
            import_starts[r] > imports) {
            return refuse(Column::locate_run_import_start, "locate run import start", r,
                          import_floor, import_starts[r]);
        }
    }
    return {};
}

std::expected<std::vector<std::uint64_t>, ItchStoreError> positions_from_runs(
    std::span<const std::uint64_t> gaps, std::span<const std::uint64_t> starts,
    std::uint64_t typed, std::uint64_t records) {
    const auto refuse = [&](std::string_view name, std::uint64_t index, std::uint64_t expected,
                            std::uint64_t actual) {
        ItchStoreError error = fail(ItchStoreErrorKind::bad_value, name, kNoIndex, expected, actual);
        error.column = column_info(Column::locate).name;
        error.index = index;
        return std::unexpected(error);
    };
    if (gaps.size() != typed) {
        return refuse("locate run indices", kNoIndex, typed, gaps.size());
    }
    if (typed > 0 && (starts.empty() || starts[0] != 0)) {
        return refuse("locate run indices", kNoIndex, 1, starts.size());
    }
    std::vector<std::uint64_t> positions(static_cast<std::size_t>(typed));
    std::vector<std::uint8_t> named(static_cast<std::size_t>(records), 0);
    std::size_t run = 0;
    std::uint64_t index = 0;
    for (std::uint64_t at = 0; at < typed; ++at) {
        const bool first = run < starts.size() && starts[run] == at;
        if (first) {
            ++run;
        }
        const std::uint64_t gap = gaps[at];
        // The first index of a run is the gap itself, each later one the last plus the gap
        // plus 1, and every one below `records`, checked before it is formed.
        const std::uint64_t room = first ? records : records - index - 1;
        if (gap >= room) {
            return refuse("locate run index", at, room - 1, gap);
        }
        index = first ? gap : index + gap + 1;
        if (named[index] != 0) {
            return refuse("locate run index repeated", at, 0, index);
        }
        named[index] = 1;
        positions[at] = index;
    }
    return positions;
}

// --- The time bases -----------------------------------------------------------------------

std::vector<std::uint64_t> time_bases(std::span<const std::uint64_t> times,
                                      std::span<const std::uint64_t> positions,
                                      std::uint64_t records) {
    constexpr std::uint64_t kNone = kMax64;
    std::vector<std::uint64_t> bases(static_cast<std::size_t>(time_base_count(records, times.size())),
                                     kNone);
    for (std::size_t k = 0; k < times.size(); ++k) {
        std::uint64_t& base = bases[positions[k] / kTimeBaseRecords];
        base = std::min(base, times[k]);
    }
    std::uint64_t before = 0;
    for (std::uint64_t& base : bases) {
        if (base == kNone) {
            base = before;
        }
        before = base;
    }
    return bases;
}

namespace {

// Visits each stored typed record with whether it starts its run and its time base.
template <class Visit>
void each_with_base(std::span<const std::uint64_t> positions, std::span<const std::uint64_t> starts,
                    std::span<const std::uint64_t> bases, Visit&& visit) {
    std::size_t run = 0;
    for (std::size_t k = 0; k < positions.size(); ++k) {
        const bool first = run < starts.size() && starts[run] == k;
        if (first) {
            ++run;
        }
        visit(k, first, bases[positions[k] / kTimeBaseRecords]);
    }
}

}  // namespace

std::vector<std::uint64_t> time_residuals(std::span<const std::uint64_t> times,
                                          std::span<const std::uint64_t> positions,
                                          std::span<const std::uint64_t> starts,
                                          std::span<const std::uint64_t> bases) {
    std::vector<std::uint64_t> out(times.size());
    std::uint64_t before = 0;
    each_with_base(positions, starts, bases, [&](std::size_t k, bool first, std::uint64_t base) {
        const std::uint64_t from = first ? base : std::max(before, base);
        // Modulo 2^64: a time below its base wraps, and adds back the same way.
        out[k] = times[k] - from;
        before = times[k];
    });
    return out;
}

std::expected<std::vector<std::uint64_t>, ItchStoreError> times_from_residuals(
    std::span<const std::uint64_t> residuals, std::span<const std::uint64_t> positions,
    std::span<const std::uint64_t> starts, std::span<const std::uint64_t> bases) {
    // A max-plus recurrence, t = r + max(t before, base), so each value waits on the one
    // before: no vector form, since the residuals wrap modulo 2^64 and a wrapped sum breaks
    // the associativity a parallel scan needs (NOTES.md, "SIMD"). The loop keeps only the
    // add and the max on that chain: the run start is a compare with the next start, and
    // the 48-bit check is an OR over all values, with the first offender found after.
    std::vector<std::uint64_t> out(residuals.size());
    std::uint64_t before = 0;
    std::uint64_t seen = 0;
    std::size_t run = 0;
    std::size_t next_start = starts.empty() ? positions.size() : starts[0];
    for (std::size_t k = 0; k < positions.size(); ++k) {
        const std::uint64_t base = bases[positions[k] / kTimeBaseRecords];
        std::uint64_t from_before = before;
        if (k == next_start) {
            // A run's first record takes its base alone.
            from_before = 0;
            ++run;
            next_start = run < starts.size() ? starts[run] : positions.size();
        }
        const std::uint64_t time = residuals[k] + std::max(from_before, base);
        out[k] = time;
        seen |= time;
        before = time;
    }
    if (seen > kMax48) {
        const auto refused = static_cast<std::size_t>(
            std::ranges::find_if(out, [](std::uint64_t time) { return time > kMax48; }) -
            out.begin());
        ItchStoreError error =
            fail(ItchStoreErrorKind::bad_value, "timestamp value", kNoIndex, kMax48, out[refused]);
        error.column = column_info(Column::timestamp).name;
        error.index = refused;
        return std::unexpected(error);
    }
    return out;
}

// --- The seek index -----------------------------------------------------------------------

namespace {

[[nodiscard]] bool names_an_order(std::uint64_t type) noexcept {
    return type == 'E' || type == 'C' || type == 'X' || type == 'D' || type == 'U';
}

}  // namespace

std::array<std::uint64_t, kSeekMaxima> column_maxima(const SegmentColumns& stored) {
    std::array<std::uint64_t, kSeekMaxima> maxima{};
    for (std::size_t c = 0; c < kSeekMaxima; ++c) {
        for (const std::uint64_t value : stored.values[c]) {
            maxima[c] = std::max(maxima[c], value);
        }
    }
    return maxima;
}

std::vector<std::uint64_t> compute_seek(const std::array<std::uint64_t, kSeekMaxima>& maxima,
                                        std::span<const std::uint64_t> types,
                                        std::span<const std::uint64_t> times,
                                        std::span<const std::uint64_t> ref_distance,
                                        std::span<const std::uint64_t> positions) {
    const std::uint64_t groups = seek_groups(types.size());
    std::vector<std::uint64_t> out(seek_value_count(types.size()), 0);
    std::ranges::copy(maxima, out.begin());
    const std::size_t min_at = kSeekMaxima;
    const std::size_t max_at = min_at + groups;
    const std::size_t imports_at = max_at + groups;
    const std::size_t counts_at = imports_at + groups;
    const std::size_t file_min_at = counts_at + (kSeekSymbols.size() * groups);
    const std::size_t file_max_at = file_min_at + groups;
    std::size_t named = 0;
    for (std::size_t i = 0; i < types.size(); ++i) {
        const std::size_t g = i / kSeekGroupRecords;
        const std::uint64_t type = types[i];
        const std::size_t symbol = seek_symbol_index(type);
        if (symbol < kSeekSymbols.size()) {
            ++out[counts_at + (symbol * groups) + g];
        }
        if (names_an_order(type)) {
            if (named < ref_distance.size() && ref_distance[named] == 0) {
                ++out[imports_at + g];
            }
            ++named;
        }
    }
    // The times: a group's first typed record sets both, and each later one widens them;
    // the same for the file groups, by each record's index in the segment.
    const auto widen = [&](std::size_t low_at, std::size_t high_at, std::vector<bool>& seen,
                           std::size_t g, std::uint64_t time) {
        if (!seen[g]) {
            seen[g] = true;
            out[low_at + g] = time;
            out[high_at + g] = time;
            return;
        }
        out[low_at + g] = std::min(out[low_at + g], time);
        out[high_at + g] = std::max(out[high_at + g], time);
    };
    std::size_t typed = 0;
    std::vector<bool> seen(groups, false);
    std::vector<bool> file_seen(groups, false);
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i] == 0) {
            continue;
        }
        const std::uint64_t time = typed < times.size() ? times[typed] : 0;
        widen(min_at, max_at, seen, i / kSeekGroupRecords, time);
        if (typed < positions.size() && positions[typed] < types.size()) {
            widen(file_min_at, file_max_at, file_seen,
                  static_cast<std::size_t>(positions[typed] / kSeekGroupRecords), time);
        }
        ++typed;
    }
    return out;
}

std::uint32_t SeekIndex::group_records(std::uint64_t group) const {
    const std::uint64_t first = group * kSeekGroupRecords;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(kSeekGroupRecords, records - first));
}

std::expected<SeekIndex, ItchStoreError> parse_seek(std::span<const std::uint64_t> values,
                                                    std::uint32_t records) {
    const auto refuse = [&](std::string_view name, std::uint64_t index, std::uint64_t expected,
                            std::uint64_t actual) {
        ItchStoreError error = fail(ItchStoreErrorKind::bad_value, name, kNoIndex, expected, actual);
        error.column = column_info(Column::seek).name;
        error.index = index;
        return std::unexpected(error);
    };
    const std::uint64_t want = seek_value_count(records);
    if (records == 0 || values.size() != want) {
        return refuse("seek value count", kNoIndex, want, values.size());
    }
    SeekIndex index;
    index.records = records;
    index.groups = seek_groups(records);
    const std::uint64_t groups = index.groups;
    for (std::size_t c = 0; c < kSeekMaxima; ++c) {
        if (values[c] > kColumns[c].max_value) {
            return refuse("seek column maximum", c, kColumns[c].max_value, values[c]);
        }
        index.maxima[c] = values[c];
    }
    const std::size_t min_at = kSeekMaxima;
    const std::size_t max_at = min_at + groups;
    const std::size_t imports_at = max_at + groups;
    const std::size_t counts_at = imports_at + groups;
    const std::size_t file_min_at = counts_at + (kSeekSymbols.size() * groups);
    const std::size_t file_max_at = file_min_at + groups;
    index.file_min_time.assign(values.begin() + static_cast<std::ptrdiff_t>(file_min_at),
                               values.begin() + static_cast<std::ptrdiff_t>(file_max_at));
    index.file_max_time.assign(values.begin() + static_cast<std::ptrdiff_t>(file_max_at),
                               values.begin() + static_cast<std::ptrdiff_t>(file_max_at + groups));
    index.min_time.assign(values.begin() + static_cast<std::ptrdiff_t>(min_at),
                          values.begin() + static_cast<std::ptrdiff_t>(max_at));
    index.max_time.assign(values.begin() + static_cast<std::ptrdiff_t>(max_at),
                          values.begin() + static_cast<std::ptrdiff_t>(imports_at));
    index.imports.assign(values.begin() + static_cast<std::ptrdiff_t>(imports_at),
                         values.begin() + static_cast<std::ptrdiff_t>(counts_at));
    index.before.assign(kSeekSymbols.size() * (groups + 1), 0);
    index.imports_before.assign(groups + 1, 0);
    const std::size_t raw = seek_symbol_index(0);
    for (std::uint64_t g = 0; g < groups; ++g) {
        const std::uint32_t size = index.group_records(g);
        std::uint64_t sum = 0;
        std::uint64_t naming = 0;
        for (std::size_t symbol = 0; symbol < kSeekSymbols.size(); ++symbol) {
            const std::size_t at = counts_at + (symbol * groups) + g;
            const std::uint64_t count = values[at];
            if (count > size) {
                return refuse("seek count", at, size, count);
            }
            sum += count;
            if (names_an_order(kSeekSymbols[symbol])) {
                naming += count;
            }
            const std::size_t slot = (symbol * (groups + 1)) + g;
            index.before[slot + 1] = index.before[slot] + static_cast<std::uint32_t>(count);
        }
        if (sum != size) {
            return refuse("seek group records", counts_at + g, size, sum);
        }
        const std::uint64_t typed = size - index.symbol_in(raw, g);
        if (index.max_time[g] > kMax48) {
            return refuse("seek max time", max_at + g, kMax48, index.max_time[g]);
        }
        if (index.min_time[g] > index.max_time[g]) {
            return refuse("seek min time", min_at + g, index.max_time[g], index.min_time[g]);
        }
        if (typed == 0 && index.max_time[g] != 0) {
            return refuse("seek max time", max_at + g, 0, index.max_time[g]);
        }
        if (index.imports[g] > naming) {
            return refuse("seek imports", imports_at + g, naming, index.imports[g]);
        }
        if (index.file_max_time[g] > kMax48) {
            return refuse("seek file max time", file_max_at + g, kMax48, index.file_max_time[g]);
        }
        if (index.file_min_time[g] > index.file_max_time[g]) {
            return refuse("seek file min time", file_min_at + g, index.file_max_time[g],
                          index.file_min_time[g]);
        }
        index.imports_before[g + 1] = index.imports_before[g] + index.imports[g];
    }
    return index;
}

}  // namespace tick_store::store
