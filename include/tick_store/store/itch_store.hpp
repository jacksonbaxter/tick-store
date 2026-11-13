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
// Stored order. A segment keeps its records in locate order: the typed records grouped by
// locate, ascending, each locate's in file order, then the raw records in file order. So
// one stock's records are one run of every column, and a query about one stock reads that
// run. The locate column holds each run's records' indices in the segment, the way back to
// file order, and the run directory where each run starts in the record, add and import
// counts.
//
// Column orders. Most columns are in stored order. tracking, ref_distance and
// reduce_shares are grouped by type: their values sorted by type symbol, stably. add_price
// is grouped by (locate, side) and add_shares by locate, so each holds one run per stock,
// as docs/NOTES.md found smaller. The reader rebuilds stored order from the type, locate and side
// columns, which it decodes first, and file order from the locate column.

#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/dictionary.hpp"
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

inline constexpr std::uint32_t kItchStoreVersion = 3;

// Records per segment: the writer's default, chosen by the layout experiments in docs/NOTES.md,
// and the largest a file may hold.
inline constexpr std::uint32_t kDefaultSegmentRecords = std::uint32_t{1} << 22U;
inline constexpr std::uint32_t kMaxSegmentRecords = std::uint32_t{1} << 24U;

// Values per codec block inside a column's chunk.
inline constexpr std::uint32_t kColumnBlockValues = 4096;

// A coded column's values go into container blocks, its pieces, of this many values each
// but the last: a whole number of codec blocks, so that a read checks the CRC of, and
// inflates, only the pieces holding the values it needs. A byte column is one piece.
inline constexpr std::uint32_t kPieceCodecBlocks = 4;
inline constexpr std::uint32_t kPieceValues = kPieceCodecBlocks * kColumnBlockValues;

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
// The piece's index among its column's pieces, from 0.
inline constexpr std::size_t kMetaPieceOffset = 6;
inline constexpr std::size_t kMetaValueCountOffset = 8;
// Column 0 (type), piece 0 only: the segment summary follows.
inline constexpr std::size_t kMetaFirstRecordOffset = 12;
inline constexpr std::size_t kMetaFirstTimeOffset = 20;
inline constexpr std::size_t kMetaLastTimeOffset = 28;
inline constexpr std::size_t kMetaMinTimeOffset = 36;
inline constexpr std::size_t kMetaMaxTimeOffset = 44;
inline constexpr std::size_t kMetaRecordsOffset = 52;
inline constexpr std::size_t kMetaTypedOffset = 56;
inline constexpr std::size_t kMetaPlainOffset = 60;
inline constexpr std::size_t kMetaBitmapSizeOffset = 61;
inline constexpr std::size_t kMetaBitmapOffset = 63;
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
    time_base,
    locate_run,
    locate_run_start,
    locate_run_add_start,
    locate_run_import_start,
    seek,
};

inline constexpr std::size_t kColumnCount = 39;

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
    // The segment's stored order: its typed records by ascending locate, each locate's in
    // file order, then its raw records in file order.
    stored,
    by_type,
    by_locate_side,
    by_locate,
    // For each locate's run, ascending locate, its records' indices in the segment, the
    // file order: the first, then each later one less the one before less 1 (the locate
    // column).
    locate_runs,
    // One value for each locate the segment has, ascending locate (the run directory).
    per_run,
    // The seek index's own layout (below).
    seek,
    // Stored order, each typed record's timestamp less its base: the larger of the time
    // before it in its locate's run (none for a run's first) and the time base of the
    // kTimeBaseRecords records of the file its index falls in, modulo 2^64, so a time below
    // its base is still kept (time_residuals()).
    time_residual,
    // One value for each kTimeBaseRecords records of the segment in file order: the lowest
    // timestamp of its typed records, or the value before it where it has none, 0 for a
    // first with none (time_bases()).
    time_chunks,
};

struct ColumnInfo {
    Column id = Column::type;
    std::string_view name;
    ColumnCodec codec = ColumnCodec::dictionary;
    ColumnOrder order = ColumnOrder::stored;
    // Whether the writer tries zstd over the coded chunk, and keeps it where smaller.
    bool zstd = false;
    // The largest value the field holds.
    std::uint64_t max_value = 0;
    // For a dictionary column, the modes its writer may choose among (codec/dictionary.hpp).
    // The reader takes any mode in any column.
    codec::DictModeSet dict_modes = codec::kDictAllModes;
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
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax8,
     .dict_modes = codec::kDictRankedPforOnly},
    {.id = Column::locate,
     .name = "locate",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::locate_runs,
     .zstd = true,
     .max_value = kMaxSegmentRecords - 1},
    {.id = Column::timestamp,
     .name = "timestamp",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::time_residual,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::tracking,
     .name = "tracking",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::by_type,
     .zstd = false,
     .max_value = kMax16},
    {.id = Column::creation_ref,
     .name = "creation_ref",
     .codec = ColumnCodec::pfor_head_delta,
     .order = ColumnOrder::stored,
     .zstd = true,
     .max_value = kMax64},
    {.id = Column::ref_distance,
     .name = "ref_distance",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::by_type,
     .zstd = false,
     .max_value = kMax32,
     .dict_modes = codec::kDictRankedPforOnly},
    {.id = Column::import_ref,
     .name = "import_ref",
     .codec = ColumnCodec::pfor_reference,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::add_side,
     .name = "add_side",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax8,
     .dict_modes = codec::kDictRankedPforOnly},
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
     .max_value = kMax32,
     .dict_modes = codec::kDictRankedPforOnly},
    {.id = Column::mpid,
     .name = "mpid",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::reduce_shares,
     .name = "reduce_shares",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::by_type,
     .zstd = false,
     .max_value = kMax32,
     .dict_modes = codec::kDictRankedPforOnly},
    {.id = Column::match,
     .name = "match",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::printable,
     .name = "printable",
     .codec = ColumnCodec::run_length,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::exec_price,
     .name = "exec_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::replace_shares,
     .name = "replace_shares",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32,
     .dict_modes = codec::kDictRankedPforOnly},
    {.id = Column::replace_price,
     .name = "replace_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::trade_ref,
     .name = "trade_ref",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::trade_side,
     .name = "trade_side",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax8,
     .dict_modes = codec::kDictRankedPforOnly},
    {.id = Column::trade_shares,
     .name = "trade_shares",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32,
     .dict_modes = codec::kDictRankedPforOnly},
    {.id = Column::trade_price,
     .name = "trade_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::cross_shares,
     .name = "cross_shares",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::cross_price,
     .name = "cross_price",
     .codec = ColumnCodec::pfor_for_scaled,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::cross_type,
     .name = "cross_type",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::imbalance_shares,
     .name = "imbalance_shares",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::imbalance_price,
     .name = "imbalance_price",
     .codec = ColumnCodec::pfor_delta_scaled,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax32},
    {.id = Column::imbalance_flags,
     .name = "imbalance_flags",
     .codec = ColumnCodec::dictionary,
     .order = ColumnOrder::stored,
     .zstd = true,
     .max_value = kMax24},
    {.id = Column::mwcb_level,
     .name = "mwcb_level",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax64},
    {.id = Column::rare_bytes,
     .name = "rare_bytes",
     .codec = ColumnCodec::raw_bytes,
     .order = ColumnOrder::stored,
     .zstd = true,
     .max_value = kMax8},
    {.id = Column::raw_length,
     .name = "raw_length",
     .codec = ColumnCodec::pfor_for,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax16},
    {.id = Column::raw_bytes,
     .name = "raw_bytes",
     .codec = ColumnCodec::raw_bytes,
     .order = ColumnOrder::stored,
     .zstd = true,
     .max_value = kMax8},
    {.id = Column::stock_exception_record,
     .name = "stock_exception_record",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMaxSegmentRecords - 1},
    {.id = Column::stock_exception_value,
     .name = "stock_exception_value",
     .codec = ColumnCodec::raw_bytes,
     .order = ColumnOrder::stored,
     .zstd = false,
     .max_value = kMax8},
    {.id = Column::time_base,
     .name = "time_base",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::time_chunks,
     .zstd = false,
     .max_value = kMax48},
    {.id = Column::locate_run,
     .name = "locate_run",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::per_run,
     .zstd = false,
     .max_value = kMax16},
    {.id = Column::locate_run_start,
     .name = "locate_run_start",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::per_run,
     .zstd = false,
     .max_value = kMaxSegmentRecords - 1},
    {.id = Column::locate_run_add_start,
     .name = "locate_run_add_start",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::per_run,
     .zstd = false,
     .max_value = kMaxSegmentRecords - 1},
    {.id = Column::locate_run_import_start,
     .name = "locate_run_import_start",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::per_run,
     .zstd = false,
     .max_value = kMaxSegmentRecords - 1},
    {.id = Column::seek,
     .name = "seek",
     .codec = ColumnCodec::pfor_delta,
     .order = ColumnOrder::seek,
     .zstd = false,
     .max_value = kMax64},
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

// --- The seek index ---------------------------------------------------------------------
//
// Every segment's `seek` column is what a query needs to start decoding at a record group
// rather than at the segment's first record, and to skip what it cannot need. A group is
// kSeekGroupRecords consecutive records of the segment in stored order, the last one
// shorter: exactly one codec block of the type column. A file group is the same count of
// consecutive records in file order. The values, all u64, in this order:
//
//   maxima      kSeekMaxima values: the largest value of every column but seek, in column
//               order, 0 for a column the segment has no values in
//   then, for G groups, each of these G values long:
//   min time    the lowest timestamp of the group's typed records, 0 if it has none
//   max time    the highest, 0 if it has none
//   imports     the group's references named at distance 0, which take an import
//   counts      for each symbol of kSeekSymbols in turn, its records in each group
//   then, for the G file groups, each G values long:
//   file min    the lowest timestamp of the file group's typed records, 0 if it has none
//   file max    the highest, 0 if it has none
//
// Every value is checked against the columns when the segment is decoded whole, and the
// structure (counts summing to each group's records, times within 48 bits and ordered,
// imports within the group's naming records, maxima within each column's range) when the
// index is parsed.
inline constexpr std::uint32_t kSeekGroupRecords = kColumnBlockValues;
inline constexpr std::array<std::uint8_t, 24> kSeekSymbols{
    0, 'A', 'B', 'C', 'D', 'E', 'F', 'H', 'I', 'J', 'K', 'L',
    'N', 'O', 'P', 'Q', 'R', 'S', 'U', 'V', 'W', 'X', 'Y', 'h'};
inline constexpr std::size_t kSeekMaxima = kColumnCount - 1;
inline constexpr std::size_t kSeekPerGroup = 3 + kSeekSymbols.size() + 2;

[[nodiscard]] constexpr std::uint64_t seek_groups(std::uint64_t records) noexcept {
    return (records + kSeekGroupRecords - 1) / kSeekGroupRecords;
}

[[nodiscard]] constexpr std::uint64_t seek_value_count(std::uint64_t records) noexcept {
    return kSeekMaxima + (seek_groups(records) * kSeekPerGroup);
}

// Each byte's index in kSeekSymbols, or kSeekSymbols.size() for a byte that is none.
inline constexpr std::array<std::uint8_t, 256> kSeekSymbolIndex = [] {
    std::array<std::uint8_t, 256> table{};
    table.fill(static_cast<std::uint8_t>(kSeekSymbols.size()));
    for (std::size_t i = 0; i < kSeekSymbols.size(); ++i) {
        table[kSeekSymbols[i]] = static_cast<std::uint8_t>(i);
    }
    return table;
}();

// The index of `symbol` in kSeekSymbols, or kSeekSymbols.size() for none.
[[nodiscard]] constexpr std::size_t seek_symbol_index(std::uint64_t symbol) noexcept {
    return symbol < kSeekSymbolIndex.size() ? kSeekSymbolIndex[symbol] : kSeekSymbols.size();
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
// `expected` and `actual` are the values in conflict, set only where the check compared
// values: a container or codec error leaves them to `container` or `codec`, and a refused
// call such as "finish twice" has none. The text prints what is set, and a container
// error's offset once, in the container's own words. `field` points at static storage.
struct ItchStoreError {
    ItchStoreErrorKind kind = ItchStoreErrorKind::bad_value;
    std::string_view field;
    std::uint64_t segment = kNoIndex;
    std::uint64_t block = kNoIndex;
    std::string_view column;
    std::uint64_t offset = kNoIndex;
    std::uint64_t record = kNoIndex;
    std::uint64_t index = kNoIndex;
    std::optional<std::uint64_t> expected = std::nullopt;
    std::optional<std::uint64_t> actual = std::nullopt;
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
    // True if the column form (columns.hpp) keeps every record of the segment as a hot or
    // a rare row: no record the decoder returned raw, and every hot record's values fit
    // their signed columns with printable stock and MPID (column_form_plain()). A query
    // can then take each row's timestamp from its own record.
    bool plain = true;
    // Bit (l % 8) of byte l / 8 is set if locate l appears. No trailing zero byte.
    std::vector<std::byte> locates;

    friend bool operator==(const SegmentSummary&, const SegmentSummary&) = default;
};

struct ColumnMetadata {
    std::uint32_t segment = 0;
    Column column = Column::type;
    // The piece's index among its column's pieces.
    std::uint16_t piece = 0;
    std::uint32_t values = 0;
    // Column type only.
    std::optional<SegmentSummary> summary;

    friend bool operator==(const ColumnMetadata&, const ColumnMetadata&) = default;
};

[[nodiscard]] std::vector<std::byte> encode_column_metadata(const ColumnMetadata& metadata);

// Checks the size, the column id, a non-zero value count, and for a type column's first
// piece its summary's own rules: records at least the piece's values, typed records at most
// the records, the
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

// One piece of a column as stored: the container codec, its bytes and its value count.
struct ColumnChunk {
    codec::CodecId codec = codec::CodecId::none;
    std::vector<std::byte> bytes;
    std::uint32_t values = 0;
};

// The pieces a column of `values` values is stored in: kPieceValues each but the last, or
// one for a byte column.
[[nodiscard]] std::uint64_t pieces_of(Column column, std::uint64_t values) noexcept;

// Codes one column's values with its codec, a piece at a time, in blocks of
// kColumnBlockValues, and wraps each piece in zstd where the column allows it and that is
// smaller. `values` must not be empty.
[[nodiscard]] std::expected<std::vector<ColumnChunk>, ItchStoreError> encode_column(
    Column column, std::span<const std::uint64_t> values);

// A column's chunk with any zstd wrapper taken off: the payload as stored, or the coded
// chunk inside its zstd frame. bytes() is what the column's codec reads.
struct ChunkBytes {
    bool zstd = false;
    std::span<const std::byte> payload;
    std::vector<std::byte> inflated;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return zstd ? std::span<const std::byte>{inflated} : payload;
    }
};

// Checks the codec is one the column takes and inflates a zstd chunk, its inner size held
// to what `count` values could need. Errors carry the column but no segment or offset.
[[nodiscard]] std::expected<ChunkBytes, ItchStoreError> unwrap_column(
    Column column, codec::CodecId codec, std::span<const std::byte> payload, std::uint64_t count);

// The block decoder of a column's codec (codec/block.hpp), or nullptr for the byte columns,
// whose chunk is the values themselves.
[[nodiscard]] codec::BlockDecoder column_block_decoder(Column column) noexcept;

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

// The same from how many records of each type byte there are.
[[nodiscard]] std::array<std::uint64_t, kColumnCount> counts_from_type_counts(
    const std::array<std::uint64_t, 256>& per_type);

// --- The locate runs ------------------------------------------------------------------

// The locates a segment has: the bits set in its summary's bitmap, which is how many runs
// the locate column holds and how many values each run directory column holds.
[[nodiscard]] std::uint64_t locate_runs_of(const SegmentSummary& summary) noexcept;

// The run directory's own rules, for a segment of `typed` typed records holding `adds` A
// and F records and `imports` distance-0 references: as many values in each column; ids
// strictly ascending and at most 65,535; run starts from 0, strictly ascending and below
// `typed`, so every run has a record; add and import starts from 0, never falling, and at
// most the totals. Errors name the column and the value's index.
[[nodiscard]] std::expected<void, ItchStoreError> check_run_directory(
    std::span<const std::uint64_t> ids, std::span<const std::uint64_t> starts,
    std::span<const std::uint64_t> add_starts, std::span<const std::uint64_t> import_starts,
    std::uint64_t typed, std::uint64_t adds, std::uint64_t imports);

// Each stored typed record's index in a segment of `records` records, from the locate
// column (ColumnOrder::locate_runs) and the run starts, which check_run_directory()
// passed. Checks every index is below `records` and named once.
[[nodiscard]] std::expected<std::vector<std::uint64_t>, ItchStoreError> positions_from_runs(
    std::span<const std::uint64_t> gaps, std::span<const std::uint64_t> starts,
    std::uint64_t typed, std::uint64_t records);

// --- The time bases ------------------------------------------------------------------

// The file records each time_base value covers.
inline constexpr std::uint32_t kTimeBaseRecords = 64;

// The time_base values a segment of `records` records, `typed` of them typed, holds: one
// for each kTimeBaseRecords records, none where no record is typed.
[[nodiscard]] constexpr std::uint64_t time_base_count(std::uint64_t records,
                                                      std::uint64_t typed) noexcept {
    return typed == 0 ? 0 : (records + kTimeBaseRecords - 1) / kTimeBaseRecords;
}

// A segment's time bases (ColumnOrder::time_chunks), from its typed records' times in
// stored order and each one's index in the segment, of `records` records.
[[nodiscard]] std::vector<std::uint64_t> time_bases(std::span<const std::uint64_t> times,
                                                    std::span<const std::uint64_t> positions,
                                                    std::uint64_t records);

// The timestamp column's values (ColumnOrder::time_residual) from the times in stored order,
// each one's index in the segment, the run starts and the time bases.
[[nodiscard]] std::vector<std::uint64_t> time_residuals(std::span<const std::uint64_t> times,
                                                        std::span<const std::uint64_t> positions,
                                                        std::span<const std::uint64_t> starts,
                                                        std::span<const std::uint64_t> bases);

// The times back from the timestamp column's values, which positions_from_runs() and the
// run starts it was given place, and the time bases, of time_base_count() values. Refuses a
// time above 48 bits, naming its index in the column.
[[nodiscard]] std::expected<std::vector<std::uint64_t>, ItchStoreError> times_from_residuals(
    std::span<const std::uint64_t> residuals, std::span<const std::uint64_t> positions,
    std::span<const std::uint64_t> starts, std::span<const std::uint64_t> bases);

// --- The seek index, built and parsed ------------------------------------------------

// The largest value of each column but seek, in the order its values are stored (which
// does not change a maximum), 0 where a column is empty.
[[nodiscard]] std::array<std::uint64_t, kSeekMaxima> column_maxima(const SegmentColumns& stored);

// A segment's seek values, from its maxima, its stored-order type, timestamp and
// ref_distance values (ref_distance ungrouped), and each typed record's index in the
// segment (the file order), stored order.
[[nodiscard]] std::vector<std::uint64_t> compute_seek(
    const std::array<std::uint64_t, kSeekMaxima>& maxima, std::span<const std::uint64_t> types,
    std::span<const std::uint64_t> times, std::span<const std::uint64_t> ref_distance,
    std::span<const std::uint64_t> positions);

// A segment's seek index, parsed. Groups are numbered from 0 within the segment.
struct SeekIndex {
    std::uint32_t records = 0;
    std::uint64_t groups = 0;
    std::array<std::uint64_t, kSeekMaxima> maxima{};
    std::vector<std::uint64_t> min_time;
    std::vector<std::uint64_t> max_time;
    std::vector<std::uint64_t> imports;
    // The file groups' times.
    std::vector<std::uint64_t> file_min_time;
    std::vector<std::uint64_t> file_max_time;
    // before[s * (groups + 1) + g]: records of kSeekSymbols[s] before group g, so
    // g = groups gives the segment's total.
    std::vector<std::uint32_t> before;
    // imports_before[g]: imports in the groups before g.
    std::vector<std::uint64_t> imports_before;

    [[nodiscard]] std::uint32_t symbol_before(std::size_t symbol, std::uint64_t group) const {
        return before[(symbol * (groups + 1)) + group];
    }
    [[nodiscard]] std::uint32_t symbol_in(std::size_t symbol, std::uint64_t group) const {
        return symbol_before(symbol, group + 1) - symbol_before(symbol, group);
    }
    [[nodiscard]] std::uint32_t symbol_total(std::size_t symbol) const {
        return symbol_before(symbol, groups);
    }
    // Records of group g: kSeekGroupRecords, or what the last group holds.
    [[nodiscard]] std::uint32_t group_records(std::uint64_t group) const;
};

// Checks the count against `records` and every structural rule of the layout above,
// naming the value's index in the column for any it refuses.
[[nodiscard]] std::expected<SeekIndex, ItchStoreError> parse_seek(
    std::span<const std::uint64_t> values, std::uint32_t records);

// True for the type symbols a record may carry: 0, or one of the 23 ITCH type bytes.
[[nodiscard]] bool valid_type_symbol(std::uint64_t symbol) noexcept;

// True for the types whose payload after the header is kept whole in rare_bytes.
[[nodiscard]] bool is_rare_type(std::uint8_t type) noexcept;

// True for the types whose stock is derived from the symbol table.
[[nodiscard]] bool has_derived_stock(std::uint8_t type) noexcept;

}  // namespace tick_store::store
