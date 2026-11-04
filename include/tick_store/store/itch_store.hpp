#pragma once

// The ITCH store: a day of ITCH frames as columns, in segments, inside a container file
// (container.hpp). docs/store.md has the byte layout, the reasons and the measured costs;
// this file holds the parts the writer, the reader, the tests and the fuzzer share.
//
// A segment is N consecutive records of the source, N fixed per file (the `store`
// section). Each segment's columns are blocks of the container, one block per column
// that has values, in column order, and each segment decodes from its own blocks and
// the file's `symbols` section alone: no state crosses a segment boundary. The one
// thing a segment would otherwise need from earlier ones, the orders its messages name
// that were created before it, it carries as its own import list (the import_ref column),
// so random access to segment k reads segment k's blocks and nothing else.
//
// Records, values and columns:
//
//   - Every record has a type symbol: its ITCH type byte, or 0 for a record the decoder
//     returned raw (an unknown type or a known type at the wrong length), whose payload
//     goes whole into raw_length and raw_bytes. No other column holds anything of it.
//   - Every typed record has a locate, a timestamp and a tracking number.
//   - The fields of each type go to the columns in kColumns. Stocks in A, F, P, Q and I
//     are derived from the symbol table, the first stock any message carried for that
//     locate, and stored only where they differ (stock_exception_*). The payload after
//     the common header of S, R, H, Y, L, W, K, J, h, N and O goes whole into rare_bytes.
//   - Order references. A, F and U's new reference are creations, in creation_ref. E, C,
//     X, D and U's original reference name an order, stored in ref_distance as a distance
//     back in that locate's list of known orders: the orders created in this segment on
//     that locate, and the imports it has introduced, in the order they were appended.
//     Distance 0 introduces the next import: the next value of import_ref, appended to
//     the locate's list. So an order created in an earlier segment, or never created, or
//     created under another locate, costs one import the first time a segment names it,
//     and a short distance after that.
//
// Column orders. Most columns are in file order. tracking, ref_distance and
// reduce_shares are grouped by type: their values sorted by type symbol, stably. add_price
// is grouped by (locate, side) and add_shares by locate, so each holds one run per stock,
// as NOTES.md found smaller. The reader rebuilds file order from the type, locate and side
// columns, which it decodes first.

#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/registry.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/store_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::store {

// --- Layout constants ---------------------------------------------------------------

inline constexpr std::uint32_t kItchStoreVersion = 1;

// Records per segment: the writer's default, and the largest a file may hold.
inline constexpr std::uint32_t kDefaultSegmentRecords = std::uint32_t{1} << 20U;
inline constexpr std::uint32_t kMaxSegmentRecords = std::uint32_t{1} << 24U;

// Values per codec block inside a column's chunk.
inline constexpr std::uint32_t kColumnBlockValues = 4096;

// The common header of every ITCH message: type, locate, tracking, timestamp.
inline constexpr std::size_t kItchHeaderBytes = 11;

inline constexpr std::string_view kStoreSection = "store";
inline constexpr std::string_view kSymbolsSection = "symbols";
inline constexpr std::string_view kTailSection = "tail";

// `store` section, 24 bytes.
inline constexpr std::size_t kStoreSectionSize = 24;
inline constexpr std::size_t kStoreVersionOffset = 0;
inline constexpr std::size_t kStoreSegmentRecordsOffset = 4;
inline constexpr std::size_t kStoreRecordCountOffset = 8;
inline constexpr std::size_t kStoreSegmentCountOffset = 16;

// `symbols` section: a u32 count, then that many 10-byte entries.
inline constexpr std::size_t kSymbolsCountSize = 4;
inline constexpr std::size_t kSymbolEntrySize = 10;

// Block metadata, every column.
inline constexpr std::size_t kColumnMetadataSize = 12;
inline constexpr std::size_t kMetaSegmentOffset = 0;
inline constexpr std::size_t kMetaColumnOffset = 4;
inline constexpr std::size_t kMetaReservedOffset = 6;
inline constexpr std::size_t kMetaValueCountOffset = 8;
// Column 0 (type) only: the segment summary follows.
inline constexpr std::size_t kMetaFirstRecordOffset = 12;
inline constexpr std::size_t kMetaFirstTimeOffset = 20;
inline constexpr std::size_t kMetaLastTimeOffset = 28;
inline constexpr std::size_t kMetaMinTimeOffset = 36;
inline constexpr std::size_t kMetaMaxTimeOffset = 44;
inline constexpr std::size_t kMetaTypedOffset = 52;
inline constexpr std::size_t kMetaBitmapSizeOffset = 56;
inline constexpr std::size_t kMetaBitmapOffset = 58;
inline constexpr std::size_t kSegmentMetadataFixedSize = kMetaBitmapOffset;
// A bitmap over every 16-bit locate.
inline constexpr std::size_t kMaxLocateBitmapBytes = 8192;

inline constexpr std::uint64_t kNoIndex = std::numeric_limits<std::uint64_t>::max();

// --- Columns --------------------------------------------------------------------------

// NOLINTNEXTLINE(performance-enum-size): the id is a 16-bit field in the file format.
enum class Column : std::uint16_t {
    type = 0,
    locate,
    timestamp,
    tracking,
    creation_ref,
    ref_distance,
    import_ref,
    add_side,
    add_price,
    add_shares,
    mpid,
    reduce_shares,
    match,
    printable,
    exec_price,
    replace_shares,
    replace_price,
    trade_ref,
    trade_side,
    trade_shares,
    trade_price,
    cross_shares,
    cross_price,
    cross_type,
    imbalance_shares,
    imbalance_price,
    imbalance_flags,
    mwcb_level,
    rare_bytes,
    raw_length,
    raw_bytes,
    stock_exception_record,
    stock_exception_value,
};

inline constexpr std::size_t kColumnCount = 33;

// How a column's values are coded. Every PFOR variant is codec id pfor, and a block names
// its own transform, so the reader accepts any; the writer's choice is what is listed.
enum class ColumnCodec : std::uint8_t {
    dictionary,
    pfor_delta,
    pfor_head_delta,
    pfor_for,
    pfor_for_scaled,
    pfor_delta_scaled,
    // delta and head delta both tried, the smaller kept.
    pfor_reference,
    run_length,
    // One byte per value, stored as they are.
    raw_bytes,
};

enum class ColumnOrder : std::uint8_t {
    file,
    by_type,
    by_locate_side,
    by_locate,
};

struct ColumnInfo {
    Column id = Column::type;
    std::string_view name;
    ColumnCodec codec = ColumnCodec::dictionary;
    ColumnOrder order = ColumnOrder::file;
    // Whether the writer tries zstd over the coded chunk, and keeps it where smaller.
    bool zstd = false;
    // The largest value the field holds.
    std::uint64_t max_value = 0;
};

inline constexpr std::uint64_t kMax8 = 0xFFU;
inline constexpr std::uint64_t kMax16 = 0xFFFFU;
inline constexpr std::uint64_t kMax24 = 0xFF'FFFFU;
inline constexpr std::uint64_t kMax32 = 0xFFFF'FFFFU;
inline constexpr std::uint64_t kMax48 = 0xFFFF'FFFF'FFFFULL;
inline constexpr std::uint64_t kMax64 = std::numeric_limits<std::uint64_t>::max();

inline constexpr std::array<ColumnInfo, kColumnCount> kColumns{{
    {.id = Column::type,
     .name = "type",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::locate,
     .name = "locate",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = true,
     .max_value = kMax16},
    {.id = Column::timestamp,
     .name = "timestamp",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax48},
    {.id = Column::tracking,
     .name = "tracking",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::by_type,
     .zstd = false,
     .max_value = kMax16},
    {.id = Column::creation_ref,
     .name = "creation_ref",
     .codec = ColumnCodec::pfor_head_delta,
     .order = ColumnOrder::file,
     .zstd = true,
     .max_value = kMax64},
    {.id = Column::ref_distance,
     .name = "ref_distance",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::by_type,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::import_ref,
     .name = "import_ref",
     .codec = ColumnCodec::pfor_reference,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::add_side,
     .name = "add_side",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::add_price,
     .name = "add_price",
     .codec = ColumnCodec::pfor_delta_scaled,
     .order = ColumnOrder::by_locate_side,
     .zstd = true,
     .max_value = kMax32},
    {.id = Column::add_shares,
     .name = "add_shares",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::by_locate,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::mpid,
     .name = "mpid",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::reduce_shares,
     .name = "reduce_shares",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::by_type,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::match,
     .name = "match",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::printable,
     .name = "printable",
     .codec = ColumnCodec::run_length,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::exec_price,
     .name = "exec_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::replace_shares,
     .name = "replace_shares",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::replace_price,
     .name = "replace_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::trade_ref,
     .name = "trade_ref",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::trade_side,
     .name = "trade_side",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::trade_shares,
     .name = "trade_shares",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::trade_price,
     .name = "trade_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::cross_shares,
     .name = "cross_shares",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::cross_price,
     .name = "cross_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::cross_type,
     .name = "cross_type",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::imbalance_shares,
     .name = "imbalance_shares",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::imbalance_price,
     .name = "imbalance_price",
     .codec = ColumnCodec::pfor_delta_scaled,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::imbalance_flags,
     .name = "imbalance_flags",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::file,
     .zstd = true,
     .max_value = kMax24},
    {.id = Column::mwcb_level,
     .name = "mwcb_level",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::rare_bytes,
     .name = "rare_bytes",
     .codec = ColumnCodec::raw_bytes,
     .order = ColumnOrder::file,
     .zstd = true,
     .max_value = kMax8},
    {.id = Column::raw_length,
     .name = "raw_length",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax16},
    {.id = Column::raw_bytes,
     .name = "raw_bytes",
     .codec = ColumnCodec::raw_bytes,
     .order = ColumnOrder::file,
     .zstd = true,
     .max_value = kMax8},
    {.id = Column::stock_exception_record,
     .name = "stock_exception_record",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMaxSegmentRecords - 1},
    {.id = Column::stock_exception_value,
     .name = "stock_exception_value",
     .codec = ColumnCodec::raw_bytes,
     .order = ColumnOrder::file,
     .zstd = false,
     .max_value = kMax8},
}};

static_assert(
    [] {
        for (std::size_t i = 0; i < kColumns.size(); ++i) {
            if (static_cast<std::size_t>(kColumns[i].id) != i) {
                return false;
            }
        }
        return true;
    }(),
    "kColumns must list every column in id order");

[[nodiscard]] constexpr const ColumnInfo& column_info(Column column) noexcept {
    return kColumns[static_cast<std::size_t>(column)];
}

// The container codec a column's chunk is stored under when zstd is not used.
[[nodiscard]] codec::CodecId primary_codec(Column column) noexcept;

// --- Errors ----------------------------------------------------------------------------

enum class ItchStoreErrorKind : std::uint8_t {
    // The container refused: `container` holds its error.
    container,
    // A column's codec refused its bytes: `codec` holds the error, whose offset is in bits
    // from the chunk's first byte (or the zstd payload's, for "zstd ..." fields).
    codec,
    // A store section breaks a rule.
    bad_section,
    // A block's metadata breaks a rule.
    bad_metadata,
    // A segment's blocks are not the ones its records need: a column missing, extra, or
    // with the wrong count, or under a codec the column does not take.
    bad_segment,
    // A decoded value is outside its field, or breaks a rule tying columns together.
    bad_value,
    // The writer was given something it cannot store.
    invalid_argument,
};

// Where and what. Every location field is kNoIndex, or "" for `column`, where it does
// not apply:
//
//   segment   the segment's index
//   block     the container block's index
//   column    the column's name
//   offset    a byte offset in the file: the field, or the block's payload, or for a
//             codec error the byte the codec stopped at
//   record    the record's index in the file, from 0
//   index     the value's index in its column's chunk
//
// `expected` and `actual` are the values in conflict. `field` points at static storage.
struct ItchStoreError {
    ItchStoreErrorKind kind = ItchStoreErrorKind::bad_value;
    std::string_view field;
    std::uint64_t segment = kNoIndex;
    std::uint64_t block = kNoIndex;
    std::string_view column;
    std::uint64_t offset = kNoIndex;
    std::uint64_t record = kNoIndex;
    std::uint64_t index = kNoIndex;
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;
    std::optional<StoreError> container;
    std::optional<codec::CodecError> codec;

    friend bool operator==(const ItchStoreError&, const ItchStoreError&) = default;
};

[[nodiscard]] std::string to_string(const ItchStoreError& error);

// --- Sections ---------------------------------------------------------------------------

struct StoreHeader {
    std::uint32_t segment_records = kDefaultSegmentRecords;
    std::uint64_t record_count = 0;
    std::uint64_t segment_count = 0;

    friend bool operator==(const StoreHeader&, const StoreHeader&) = default;
};

[[nodiscard]] std::vector<std::byte> encode_store_header(const StoreHeader& header);

// Checks the version, the segment size (1 to kMaxSegmentRecords) and that the segment count
// is the record count over the segment size, rounded up. `section_offset` is where the
// section starts in the file, for errors.
[[nodiscard]] std::expected<StoreHeader, ItchStoreError> parse_store_header(
    std::span<const std::byte> bytes, std::uint64_t section_offset);

// The stock each locate stands for: the first stock any message carried for it.
class SymbolTable {
public:
    SymbolTable();

    [[nodiscard]] const itch::Stock* find(std::uint16_t locate) const noexcept;

    // Records `stock` for `locate` if it has none yet, and returns the one it has.
    const itch::Stock& learn(std::uint16_t locate, const itch::Stock& stock);

    [[nodiscard]] std::size_t size() const noexcept { return locates_.size(); }

    // Entries in ascending locate order.
    [[nodiscard]] std::vector<std::byte> encode() const;

    // Checks the count against the size and that locates strictly ascend.
    [[nodiscard]] static std::expected<SymbolTable, ItchStoreError> parse(
        std::span<const std::byte> bytes, std::uint64_t section_offset);

    friend bool operator==(const SymbolTable& a, const SymbolTable& b);

private:
    static constexpr std::uint32_t kAbsent = std::numeric_limits<std::uint32_t>::max();
    // Index into stocks_ for every locate, or kAbsent.
    std::vector<std::uint32_t> slot_;
    std::vector<std::uint16_t> locates_;
    std::vector<itch::Stock> stocks_;
};

// --- Block metadata ------------------------------------------------------------------

// What the footer says about a segment, from its type block's metadata: enough for a time
// index or a symbol filter to choose segments without reading a payload.
struct SegmentSummary {
    std::uint64_t first_record = 0;
    std::uint32_t records = 0;
    // Records with a type, which carry a timestamp and a locate. The four times are 0 when
    // there are none.
    std::uint32_t typed_records = 0;
    std::uint64_t first_time = 0;
    std::uint64_t last_time = 0;
    std::uint64_t min_time = 0;
    std::uint64_t max_time = 0;
    // Bit (l % 8) of byte l / 8 is set if locate l appears. No trailing zero byte.
    std::vector<std::byte> locates;

    friend bool operator==(const SegmentSummary&, const SegmentSummary&) = default;
};

struct ColumnMetadata {
    std::uint32_t segment = 0;
    Column column = Column::type;
    std::uint32_t values = 0;
    // Column type only.
    std::optional<SegmentSummary> summary;

    friend bool operator==(const ColumnMetadata&, const ColumnMetadata&) = default;
};

[[nodiscard]] std::vector<std::byte> encode_column_metadata(const ColumnMetadata& metadata);

// Checks the size, the column id, the reserved field, a non-zero value count, and for a
// type block its summary's own rules: records = values, typed records at most that, the
// times within 48 bits with the minimum and maximum bounding the first and last, all 0 when
// no record is typed, and a bitmap with no trailing zero byte. `metadata_offset` is where the
// metadata sits in the file.
[[nodiscard]] std::expected<ColumnMetadata, ItchStoreError> parse_column_metadata(
    std::span<const std::byte> bytes, std::uint64_t metadata_offset);

// --- Column values ------------------------------------------------------------------

// One segment's values, column by column, each in its column's order (kColumns). Byte
// columns hold one byte per value.
struct SegmentColumns {
    std::array<std::vector<std::uint64_t>, kColumnCount> values;

    [[nodiscard]] std::vector<std::uint64_t>& operator[](Column column) noexcept {
        return values[static_cast<std::size_t>(column)];
    }
    [[nodiscard]] const std::vector<std::uint64_t>& operator[](Column column) const noexcept {
        return values[static_cast<std::size_t>(column)];
    }

    friend bool operator==(const SegmentColumns&, const SegmentColumns&) = default;
};

// A column's chunk as stored: the container codec and its bytes.
struct ColumnChunk {
    codec::CodecId codec = codec::CodecId::none;
    std::vector<std::byte> bytes;
};

// Codes one column's values with its codec, in blocks of kColumnBlockValues, then wraps
// them in zstd where the column allows it and that is smaller. `values` must not be empty.
[[nodiscard]] std::expected<ColumnChunk, ItchStoreError> encode_column(
    Column column, std::span<const std::uint64_t> values);

// Decodes a chunk that must hold exactly `count` values, checking the codec is one the
// column takes and every value is within the column's range. Errors carry the column but no
// segment, block or offset; the caller adds them.
[[nodiscard]] std::expected<std::vector<std::uint64_t>, ItchStoreError> decode_column(
    Column column, codec::CodecId codec, std::span<const std::byte> payload, std::uint64_t count);

// The values each column must hold in a segment, from the type column. Columns whose count
// depends on other columns (raw_bytes, import_ref, stock_exception_value) and
// stock_exception_record, whose count is free, are left at 0.
[[nodiscard]] std::array<std::uint64_t, kColumnCount> counts_from_types(
    std::span<const std::uint64_t> types);

// True for the type symbols a record may carry: 0, or one of the 23 ITCH type bytes.
[[nodiscard]] bool valid_type_symbol(std::uint64_t symbol) noexcept;

// True for the types whose payload after the header is kept whole in rare_bytes.
[[nodiscard]] bool is_rare_type(std::uint8_t type) noexcept;

// True for the types whose stock is derived from the symbol table.
[[nodiscard]] bool has_derived_stock(std::uint8_t type) noexcept;

}  // namespace tick_store::store
