#include "tick_store/store/itch_query.hpp"

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/little_endian.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

constexpr std::size_t kLocates = std::size_t{kMax16} + 1;
constexpr std::uint64_t kNanosPerMinute = 60ULL * 1'000'000'000ULL;
constexpr std::size_t kSymbols = kSeekSymbols.size();

[[nodiscard]] ItchStoreError query_error(ItchStoreErrorKind kind, std::string_view field,
                                         std::uint64_t segment, Column column,
                                         std::uint64_t index, std::uint64_t expected,
                                         std::uint64_t actual) {
    ItchStoreError error;
    error.kind = kind;
    error.field = field;
    error.segment = segment;
    error.column = column_info(column).name;
    error.index = index;
    error.expected = expected;
    error.actual = actual;
    return error;
}

[[nodiscard]] bool bit_of(std::span<const std::byte> bitmap, std::uint64_t locate) noexcept {
    const std::uint64_t byte = locate / 8U;
    return byte < bitmap.size() &&
           (std::to_integer<unsigned>(bitmap[byte]) & (1U << (locate % 8U))) != 0;
}

// The bits set below `locate`: its run's index among the segment's runs.
[[nodiscard]] std::uint64_t rank_of(std::span<const std::byte> bitmap, std::uint64_t locate) noexcept {
    std::uint64_t rank = 0;
    const std::uint64_t whole = std::min<std::uint64_t>(locate / 8U, bitmap.size());
    for (std::uint64_t b = 0; b < whole; ++b) {
        rank += static_cast<std::uint64_t>(std::popcount(std::to_integer<unsigned>(bitmap[b])));
    }
    if (whole < bitmap.size()) {
        const unsigned below = (1U << (locate % 8U)) - 1U;
        rank += static_cast<std::uint64_t>(
            std::popcount(std::to_integer<unsigned>(bitmap[whole]) & below));
    }
    return rank;
}

[[nodiscard]] bool is_add(std::uint64_t type) noexcept { return type == 'A' || type == 'F'; }

// Indices into kSeekSymbols, which the type column's values are checked against.
const std::size_t kSymA = seek_symbol_index('A');
const std::size_t kSymC = seek_symbol_index('C');
const std::size_t kSymD = seek_symbol_index('D');
const std::size_t kSymE = seek_symbol_index('E');
const std::size_t kSymF = seek_symbol_index('F');
const std::size_t kSymP = seek_symbol_index('P');
const std::size_t kSymU = seek_symbol_index('U');
const std::size_t kSymX = seek_symbol_index('X');
const std::size_t kSymRaw = seek_symbol_index(0);

[[nodiscard]] bool names(std::size_t symbol) noexcept {
    return symbol == kSymC || symbol == kSymD || symbol == kSymE || symbol == kSymU ||
           symbol == kSymX;
}

using TypeCounts = std::array<std::uint64_t, kSymbols>;

// One column of one segment, decoded a codec block at a time as values are asked for. A
// piece (a container block) is read, CRC-checked and, if zstd, inflated the first time a
// value in it is wanted, since zstd has no block boundaries inside a piece. Every value is
// checked against the column's range as its block is decoded.
class ColumnAccess {
public:
    // `most` bounds the values the segment's records can give the column, checked before
    // anything is read or allocated.
    [[nodiscard]] std::expected<void, ItchStoreError> open(const ItchStoreReader& reader,
                                                           std::uint64_t segment, Column column,
                                                           std::uint64_t most, QueryStats& stats) {
        reader_ = &reader;
        column_ = column;
        segment_ = segment;
        stats_ = &stats;
        count_ = 0;
        pieces_used_ = 0;
        const SegmentEntry& entry = reader.segment(segment);
        const auto c = static_cast<std::size_t>(column);
        block_ = entry.block[c];
        if (block_ == kNoIndex) {
            return {};
        }
        if (entry.values[c] > most) {
            return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column value count",
                                          kNoIndex, most, entry.values[c]));
        }
        count_ = entry.values[c];
        pieces_used_ = entry.pieces[c];
        bytes_column_ = column_info(column).codec == ColumnCodec::raw_bytes;
        decoder_ = column_block_decoder(column);
        if (pieces_.size() < pieces_used_) {
            pieces_.resize(pieces_used_);
        }
        for (std::size_t p = 0; p < pieces_used_; ++p) {
            Piece& piece = pieces_[p];
            piece.opened = false;
            piece.first = bytes_column_ ? 0 : p * std::uint64_t{kPieceValues};
            piece.count = p + 1 == pieces_used_ ? count_ - piece.first : std::uint64_t{kPieceValues};
        }
        if (count_ > values_.size()) {
            values_.resize(count_);
        }
        return {};
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return count_; }

    // Decodes every block holding a value in [first, last).
    [[nodiscard]] std::expected<void, ItchStoreError> ensure(std::uint64_t first,
                                                             std::uint64_t last) {
        if (first >= last) {
            return {};
        }
        if (last > count_) {
            return std::unexpected(
                refuse(ItchStoreErrorKind::bad_value, "value index", last - 1, count_, last - 1));
        }
        const std::size_t from = piece_of(first);
        const std::size_t to = piece_of(last - 1);
        for (std::size_t p = from; p <= to; ++p) {
            Piece& piece = pieces_[p];
            if (!piece.opened) {
                if (auto done = open_piece(p); !done.has_value()) {
                    return done;
                }
            }
            if (bytes_column_) {
                continue;
            }
            const std::uint64_t low = std::max(first, piece.first);
            const std::uint64_t high = std::min(last, piece.first + piece.count);
            const std::size_t b_from = block_in(piece, low);
            const std::size_t b_to = block_in(piece, high - 1);
            for (std::size_t b = b_from; b <= b_to; ++b) {
                if (piece.decoded[b] == 0) {
                    if (auto done = decode_block(piece, b); !done.has_value()) {
                        return done;
                    }
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ItchStoreError> all() { return ensure(0, count_); }

    // A value already decoded by ensure() or all().
    [[nodiscard]] std::uint64_t operator[](std::uint64_t index) const noexcept {
        return values_[index];
    }

    [[nodiscard]] std::expected<std::uint64_t, ItchStoreError> get(std::uint64_t index) {
        if (auto done = ensure(index, index + 1); !done.has_value()) {
            return std::unexpected(done.error());
        }
        return values_[index];
    }

    // Every value, after all().
    [[nodiscard]] std::span<const std::uint64_t> values() const noexcept {
        return std::span<const std::uint64_t>{values_}.first(static_cast<std::size_t>(count_));
    }

private:
    struct Piece {
        bool opened = false;
        std::uint64_t first = 0;
        std::uint64_t count = 0;
        std::uint64_t block = kNoIndex;
        std::uint64_t file_offset = 0;
        ChunkBytes bytes;
        // Each codec block's first value, in the column, and its offset in the piece's
        // coded bytes; one more entry closes the last.
        std::vector<std::uint64_t> starts;
        std::vector<std::uint64_t> offsets;
        std::vector<std::uint8_t> decoded;
    };

    [[nodiscard]] std::size_t piece_of(std::uint64_t index) const noexcept {
        return bytes_column_ ? 0 : static_cast<std::size_t>(index / kPieceValues);
    }

    [[nodiscard]] static std::size_t block_in(const Piece& piece, std::uint64_t index) noexcept {
        // Blocks are almost always kColumnBlockValues long, so guess, then search.
        const std::uint64_t guess = (index - piece.first) / kColumnBlockValues;
        if (guess + 1 < piece.starts.size() && piece.starts[guess] <= index &&
            index < piece.starts[guess + 1]) {
            return static_cast<std::size_t>(guess);
        }
        return static_cast<std::size_t>(std::ranges::upper_bound(piece.starts, index) -
                                        piece.starts.begin() - 1);
    }

    [[nodiscard]] std::expected<void, ItchStoreError> open_piece(std::size_t p) {
        Piece& piece = pieces_[p];
        auto view = reader_->column_view(segment_, column_, p);
        if (!view.has_value()) {
            return std::unexpected(view.error());
        }
        piece.block = view->block;
        piece.file_offset = view->file_offset;
        if (view->values != piece.count) {
            return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "piece value count",
                                          kNoIndex, piece.count, view->values));
        }
        auto unwrapped = unwrap_column(column_, view->codec, view->payload, piece.count);
        if (!unwrapped.has_value()) {
            ItchStoreError error = unwrapped.error();
            error.segment = segment_;
            error.block = piece.block;
            error.offset = piece.file_offset;
            return std::unexpected(error);
        }
        piece.bytes = std::move(*unwrapped);
        const std::span<const std::byte> bytes = piece.bytes.bytes();
        if (piece.bytes.zstd) {
            // The whole compressed piece is read to reach any value of it.
            stats_->bytes_decoded += view->payload.size();
        }
        piece.starts.clear();
        piece.offsets.clear();
        if (decoder_ == nullptr) {
            // A byte column: the piece is the values.
            if (bytes.size() != piece.count) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column bytes",
                                              kNoIndex, piece.count, bytes.size()));
            }
            for (std::uint64_t i = 0; i < piece.count; ++i) {
                values_[piece.first + i] = std::to_integer<std::uint64_t>(bytes[i]);
            }
            ++stats_->blocks;
            if (!piece.bytes.zstd) {
                stats_->bytes_decoded += bytes.size();
            }
            stats_->values_decoded += piece.count;
            piece.opened = true;
            return {};
        }
        // The codec blocks' headers, so a value index finds its block without decoding the
        // ones before it.
        std::uint64_t offset = 0;
        std::uint64_t total = 0;
        while (offset < bytes.size()) {
            if (total == piece.count) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment,
                                              "column trailing bytes", kNoIndex, 0,
                                              bytes.size() - offset));
            }
            const auto values = read_le<4>(bytes, offset);
            const auto payload = read_le<4>(bytes, offset + 4);
            if (!values.has_value() || !payload.has_value()) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment,
                                              "column block header", kNoIndex,
                                              codec::kBlockHeaderSize, bytes.size() - offset));
            }
            const std::uint64_t end = offset + codec::kBlockHeaderSize + *payload;
            if (*values == 0 || end > bytes.size()) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column block size",
                                              kNoIndex, bytes.size() - offset, end - offset));
            }
            piece.starts.push_back(piece.first + total);
            piece.offsets.push_back(offset);
            total += *values;
            offset = end;
            if (total > piece.count) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment,
                                              "column value count", kNoIndex, piece.count, total));
            }
        }
        if (total != piece.count) {
            return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column value count",
                                          kNoIndex, piece.count, total));
        }
        piece.starts.push_back(piece.first + total);
        piece.offsets.push_back(bytes.size());
        piece.decoded.assign(piece.starts.size() - 1, 0);
        piece.opened = true;
        return {};
    }

    [[nodiscard]] std::expected<void, ItchStoreError> decode_block(Piece& piece, std::size_t b) {
        const std::span<const std::byte> bytes = piece.bytes.bytes();
        const std::span<const std::byte> block =
            bytes.subspan(piece.offsets[b], piece.offsets[b + 1] - piece.offsets[b]);
        const std::uint64_t first = piece.starts[b];
        const std::uint64_t n = piece.starts[b + 1] - first;
        scratch_.clear();
        const auto taken = decoder_(block, scratch_);
        if (!taken.has_value()) {
            ItchStoreError error =
                refuse(ItchStoreErrorKind::codec, taken.error().field, kNoIndex, 0, 0);
            error.codec = taken.error();
            error.block = piece.block;
            error.offset = piece.bytes.zstd
                               ? piece.file_offset
                               : piece.file_offset + piece.offsets[b] + (taken.error().offset / 8U);
            return std::unexpected(error);
        }
        if (*taken != block.size() || scratch_.size() != n) {
            return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column block values",
                                          first, n, scratch_.size()));
        }
        const std::uint64_t max = column_info(column_).max_value;
        std::uint64_t highest = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            highest = std::max(highest, scratch_[i]);
        }
        if (highest > max) {
            for (std::uint64_t i = 0; i < n; ++i) {
                if (scratch_[i] > max) {
                    return std::unexpected(refuse(ItchStoreErrorKind::bad_value, "column value",
                                                  first + i, max, scratch_[i]));
                }
            }
        }
        std::memcpy(values_.data() + first, scratch_.data(), n * sizeof(std::uint64_t));
        piece.decoded[b] = 1;
        ++stats_->blocks;
        if (!piece.bytes.zstd) {
            stats_->bytes_decoded += block.size();
        }
        stats_->values_decoded += n;
        return {};
    }

    [[nodiscard]] ItchStoreError refuse(ItchStoreErrorKind kind, std::string_view field,
                                        std::uint64_t index, std::uint64_t expected,
                                        std::uint64_t actual) const {
        ItchStoreError error = query_error(kind, field, segment_, column_, index, expected, actual);
        error.block = block_;
        return error;
    }

    const ItchStoreReader* reader_ = nullptr;
    Column column_ = Column::type;
    std::uint64_t segment_ = 0;
    std::uint64_t block_ = kNoIndex;
    std::uint64_t count_ = 0;
    std::size_t pieces_used_ = 0;
    bool bytes_column_ = false;
    codec::BlockDecoder decoder_ = nullptr;
    std::vector<Piece> pieces_;
    std::vector<std::uint64_t> values_;
    std::vector<std::uint64_t> scratch_;
    QueryStats* stats_ = nullptr;
};

// What a scan of a run hands on: rows in full, or for the minute aggregate the add orders
// alone with their time, shares and price.
enum class Need : std::uint8_t { rows, adds };

}  // namespace

struct ItchQuery::Engine {
    std::array<ColumnAccess, kColumnCount> cols;
    // The known orders of the locate being scanned, as the assembler keeps them.
    std::vector<std::uint64_t> list;
    // The timestamp of each segment's last row, once known.
    std::vector<std::optional<std::uint64_t>> last_ts;
    std::array<std::byte, itch::kMaxPayloadSize> scratch{};
    // The all-locates path: each stored typed record's index in the segment, and the rows
    // of a segment by their index in it, with which are filled.
    std::vector<std::uint64_t> positions;
    std::vector<QueryRow> by_position;
    std::vector<std::uint8_t> filled;

    ColumnAccess& col(Column column) { return cols[static_cast<std::size_t>(column)]; }
};

namespace {

// The bytes of every block of a segment, which the exact path reads whole.
void count_whole_segment(const ItchStoreReader& reader, std::uint64_t segment, QueryStats& stats) {
    const SegmentEntry& entry = reader.segment(segment);
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        if (entry.block[c] == kNoIndex) {
            continue;
        }
        for (std::uint64_t p = 0; p < entry.pieces[c]; ++p) {
            if (const auto info = reader.container().block_info(entry.block[c] + p);
                info.has_value()) {
                stats.bytes_decoded += info->payload_size;
            }
        }
        stats.blocks += (entry.values[c] + kColumnBlockValues - 1) / kColumnBlockValues;
        stats.values_decoded += entry.values[c];
    }
}

// The exact path: segment `segment` decoded whole, each record mapped by row_values() as
// tick_export maps it. `seed` is the timestamp of the row before the segment's first, which
// a raw row repeats. Hands every row to `sink` and returns the last row's timestamp.
template <class Sink>
[[nodiscard]] std::expected<std::uint64_t, ItchStoreError> exact_segment(
    const ItchStoreReader& reader, ItchQuery::Engine& engine, std::uint64_t segment,
    std::uint64_t seed, QueryStats& stats, Sink&& sink) {
    ++stats.segments;
    ++stats.exact_segments;
    count_whole_segment(reader, segment, stats);
    SegmentReader records;
    if (auto opened = records.seek(reader, segment); !opened.has_value()) {
        return std::unexpected(opened.error());
    }
    const std::uint64_t first = reader.segment(segment).summary.first_record;
    std::uint64_t previous = seed;
    for (std::uint64_t i = 0;; ++i) {
        auto message = records.next();
        if (!message.has_value()) {
            return std::unexpected(message.error());
        }
        if (!message->has_value()) {
            break;
        }
        const auto size = itch::encode(**message, engine.scratch);
        std::optional<RowValues> values;
        if (size.has_value()) {
            values = row_values(std::span<const std::byte>{engine.scratch.data(), *size});
        }
        if (!values.has_value()) {
            ItchStoreError error;
            error.kind = ItchStoreErrorKind::bad_value;
            error.field = "record has no row";
            error.segment = segment;
            error.record = first + i;
            return std::unexpected(error);
        }
        QueryRow row;
        row.seq = first + i;
        row.msg_type = values->msg_type;
        if (values->form == RowForm::raw) {
            row.ts_ns = previous;
        } else {
            row.ts_ns = values->ts_ns;
            row.locate = static_cast<std::uint32_t>(values->locate);
            if (values->form == RowForm::hot) {
                // fits_columns() held, so each value fits the row's field.
                row.order_ref = values->order_ref;
                row.side = values->side;
                row.shares = static_cast<std::uint32_t>(values->shares);
                row.price = static_cast<std::uint32_t>(values->price);
                if (values->stock.size() == row.stock.size()) {
                    row.has_stock = true;
                    std::ranges::copy(values->stock, row.stock.begin());
                }
            }
        }
        previous = row.ts_ns;
        sink(row);
    }
    return previous;
}

// One locate's run in a plain segment: its stored records [start, end), and where its
// adds and imports start in their columns.
struct Run {
    std::uint16_t locate = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t add_start = 0;
    std::uint64_t add_end = 0;
    std::uint64_t import_start = 0;
    std::uint64_t import_end = 0;
};

// What the indexed path keeps of one plain segment while it answers.
struct PlainSegment {
    const ItchStoreReader* reader = nullptr;
    ItchQuery::Engine* engine = nullptr;
    QueryStats* stats = nullptr;
    std::uint64_t segment = 0;
    const SegmentSummary* summary = nullptr;
    SeekIndex seek;
    // Each kSeekSymbols entry's records in the segment, from the seek index.
    TypeCounts total{};
    // Where each type's values start in the columns grouped by type: ref_distance in the
    // order C, D, E, U, X and reduce_shares in the order C, E, X.
    TypeCounts ref_group{};
    TypeCounts reduce_group{};
    std::uint64_t adds = 0;
    std::uint64_t imports = 0;

    [[nodiscard]] ItchStoreError error(ItchStoreErrorKind kind, std::string_view field,
                                       Column column, std::uint64_t index, std::uint64_t expected,
                                       std::uint64_t actual) const {
        return query_error(kind, field, segment, column, index, expected, actual);
    }

    [[nodiscard]] std::expected<void, ItchStoreError> open(Column column) const {
        // What any record can give a column the queries read: one value, eight stock bytes
        // for an exception, and the seek index's own count.
        const std::uint64_t records = summary->records;
        std::uint64_t most = records;
        if (column == Column::stock_exception_value) {
            most = records * sizeof(itch::Stock);
        } else if (column == Column::seek) {
            most = seek_value_count(records);
        }
        return engine->col(column).open(*reader, segment, column, most, *stats);
    }

    // Opens `column` and checks it holds `want` values.
    [[nodiscard]] std::expected<void, ItchStoreError> open_counted(Column column,
                                                                   std::uint64_t want) const {
        if (auto opened = open(column); !opened.has_value()) {
            return opened;
        }
        if (engine->col(column).size() != want) {
            return std::unexpected(error(ItchStoreErrorKind::bad_segment, "column value count",
                                         column, kNoIndex, want, engine->col(column).size()));
        }
        return {};
    }

    // Parses the seek index, and from it each type's total and group starts.
    [[nodiscard]] std::expected<void, ItchStoreError> parse() {
        const std::uint32_t records = summary->records;
        if (summary->typed_records != records) {
            return std::unexpected(error(ItchStoreErrorKind::bad_metadata, "plain typed records",
                                         Column::type, kNoIndex, records,
                                         summary->typed_records));
        }
        if (auto opened = open(Column::seek); !opened.has_value()) {
            return opened;
        }
        ColumnAccess& column = engine->col(Column::seek);
        if (auto done = column.all(); !done.has_value()) {
            return done;
        }
        auto parsed = parse_seek(column.values(), records);
        if (!parsed.has_value()) {
            ItchStoreError e = parsed.error();
            e.segment = segment;
            return std::unexpected(e);
        }
        seek = std::move(*parsed);
        for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
            total[symbol] = seek.symbol_total(symbol);
        }
        if (total[kSymRaw] != 0) {
            return std::unexpected(error(ItchStoreErrorKind::bad_value, "plain raw records",
                                         Column::seek, kNoIndex, 0, total[kSymRaw]));
        }
        std::uint64_t at = 0;
        for (const std::size_t symbol : {kSymC, kSymD, kSymE, kSymU, kSymX}) {
            ref_group[symbol] = at;
            at += total[symbol];
        }
        at = 0;
        for (const std::size_t symbol : {kSymC, kSymE, kSymX}) {
            reduce_group[symbol] = at;
            at += total[symbol];
        }
        adds = total[kSymA] + total[kSymF];
        imports = seek.imports_before[seek.groups];
        return {};
    }

    // The type column's values in [first, last), each a typed record's symbol.
    [[nodiscard]] std::expected<void, ItchStoreError> types(std::uint64_t first,
                                                            std::uint64_t last) const {
        ColumnAccess& column = engine->col(Column::type);
        if (auto done = column.ensure(first, last); !done.has_value()) {
            return done;
        }
        for (std::uint64_t k = first; k < last; ++k) {
            // The column's range is 8 bits, which decoding checked.
            const std::uint64_t type = column[k];
            if (type == 0 || kSeekSymbolIndex[type] == kSymbols) {
                return std::unexpected(
                    error(ItchStoreErrorKind::bad_value, "type symbol", Column::type, k, 0, type));
            }
        }
        return {};
    }

    // Each type's stored records before `at`: the seek index's count before at's group,
    // plus those from the group's first record to `at`.
    [[nodiscard]] std::expected<TypeCounts, ItchStoreError> before(std::uint64_t at) const {
        const std::uint64_t group = at / kSeekGroupRecords;
        TypeCounts counts{};
        if (group >= seek.groups) {
            for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
                counts[symbol] = total[symbol];
            }
            return counts;
        }
        for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
            counts[symbol] = seek.symbol_before(symbol, group);
        }
        const std::uint64_t first = group * kSeekGroupRecords;
        if (auto done = types(first, at); !done.has_value()) {
            return std::unexpected(done.error());
        }
        const ColumnAccess& column = engine->col(Column::type);
        for (std::uint64_t k = first; k < at; ++k) {
            ++counts[kSeekSymbolIndex[column[k]]];
        }
        return counts;
    }

    // Run r of the segment's `runs`, from the directory, with its limits checked against the
    // totals. `locate` is the id it must have.
    [[nodiscard]] std::expected<Run, ItchStoreError> run(std::uint64_t r, std::uint64_t runs,
                                                         std::uint16_t locate) const {
        ColumnAccess& ids = engine->col(Column::locate_run);
        ColumnAccess& starts = engine->col(Column::locate_run_start);
        ColumnAccess& add_starts = engine->col(Column::locate_run_add_start);
        ColumnAccess& import_starts = engine->col(Column::locate_run_import_start);
        const std::uint64_t last = std::min(r + 2, runs);
        for (ColumnAccess* column : {&ids, &starts, &add_starts, &import_starts}) {
            if (auto done = column->ensure(r, last); !done.has_value()) {
                return std::unexpected(done.error());
            }
        }
        if (ids[r] != locate) {
            return std::unexpected(error(ItchStoreErrorKind::bad_value, "locate run id",
                                         Column::locate_run, r, locate, ids[r]));
        }
        Run out;
        out.locate = locate;
        out.start = starts[r];
        out.add_start = add_starts[r];
        out.import_start = import_starts[r];
        const bool final_run = r + 1 == runs;
        out.end = final_run ? summary->typed_records : starts[r + 1];
        out.add_end = final_run ? adds : add_starts[r + 1];
        out.import_end = final_run ? imports : import_starts[r + 1];
        if (out.start >= out.end || out.end > summary->typed_records) {
            return std::unexpected(error(ItchStoreErrorKind::bad_value, "locate run start",
                                         Column::locate_run_start, r, out.end, out.start));
        }
        if (out.add_start > out.add_end || out.add_end > adds) {
            return std::unexpected(error(ItchStoreErrorKind::bad_value, "locate run add start",
                                         Column::locate_run_add_start, r, out.add_end,
                                         out.add_start));
        }
        if (out.import_start > out.import_end || out.import_end > imports) {
            return std::unexpected(error(ItchStoreErrorKind::bad_value, "locate run import start",
                                         Column::locate_run_import_start, r, out.import_end,
                                         out.import_start));
        }
        return out;
    }

    // The directory columns, opened and checked to hold one value per run.
    [[nodiscard]] std::expected<std::uint64_t, ItchStoreError> open_directory() const {
        const std::uint64_t runs = locate_runs_of(*summary);
        for (const Column column : {Column::locate_run, Column::locate_run_start,
                                    Column::locate_run_add_start, Column::locate_run_import_start}) {
            if (auto opened = open_counted(column, runs); !opened.has_value()) {
                return std::unexpected(opened.error());
            }
        }
        return runs;
    }
};

[[nodiscard]] PlainSegment plain_segment(const ItchStoreReader* reader,
                                         ItchQuery::Engine* engine, QueryStats* stats,
                                         std::uint64_t segment) {
    PlainSegment seg;
    seg.reader = reader;
    seg.engine = engine;
    seg.stats = stats;
    seg.segment = segment;
    seg.summary = &reader->segment(segment).summary;
    return seg;
}

// The stock of a derived-stock record: its exception, or its locate's symbol.
struct Stocks {
    std::span<const std::uint64_t> records;
    std::span<const std::uint64_t> bytes;
};

[[nodiscard]] std::expected<Stocks, ItchStoreError> open_stocks(const PlainSegment& seg) {
    for (const Column column : {Column::stock_exception_record, Column::stock_exception_value}) {
        if (auto opened = seg.open(column); !opened.has_value()) {
            return std::unexpected(opened.error());
        }
        if (auto done = seg.engine->col(column).all(); !done.has_value()) {
            return std::unexpected(done.error());
        }
    }
    Stocks stocks{.records = seg.engine->col(Column::stock_exception_record).values(),
                  .bytes = seg.engine->col(Column::stock_exception_value).values()};
    if (stocks.bytes.size() != stocks.records.size() * sizeof(itch::Stock)) {
        return std::unexpected(seg.error(ItchStoreErrorKind::bad_segment, "column value count",
                                         Column::stock_exception_value, kNoIndex,
                                         stocks.records.size() * sizeof(itch::Stock),
                                         stocks.bytes.size()));
    }
    for (std::size_t i = 1; i < stocks.records.size(); ++i) {
        if (stocks.records[i] <= stocks.records[i - 1]) {
            return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "stock exception record",
                                             Column::stock_exception_record, i,
                                             stocks.records[i - 1] + 1, stocks.records[i]));
        }
    }
    return stocks;
}

[[nodiscard]] std::expected<itch::Stock, ItchStoreError> stock_of(const PlainSegment& seg,
                                                                  const Stocks& stocks,
                                                                  std::uint64_t record,
                                                                  std::uint16_t locate) {
    const auto at = std::ranges::lower_bound(stocks.records, record);
    if (at != stocks.records.end() && *at == record) {
        const auto e = static_cast<std::size_t>(at - stocks.records.begin());
        itch::Stock stock{};
        for (std::size_t i = 0; i < stock.size(); ++i) {
            stock[i] = static_cast<std::byte>(stocks.bytes[(e * stock.size()) + i]);
        }
        return stock;
    }
    const itch::Stock* known = seg.reader->symbols().find(locate);
    if (known == nullptr) {
        return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "symbol for locate",
                                         Column::locate_run, kNoIndex, 1, 0));
    }
    return *known;
}

// The columns a scan reads, opened for the segment: what rows need, or what the minute
// aggregate needs.
[[nodiscard]] std::expected<void, ItchStoreError> open_scan_columns(const PlainSegment& seg,
                                                                    Need need) {
    const auto counts = [&] {
        std::array<std::uint64_t, 256> per_type{};
        for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
            per_type[kSeekSymbols[symbol]] = seg.total[symbol];
        }
        return counts_from_type_counts(per_type);
    }();
    const auto open = [&](Column column) {
        return seg.open_counted(column, counts[static_cast<std::size_t>(column)]);
    };
    std::vector<Column> needed{Column::type, Column::locate, Column::timestamp, Column::add_side,
                               Column::add_shares, Column::add_price};
    if (need == Need::rows) {
        needed.insert(needed.end(),
                      {Column::creation_ref, Column::ref_distance, Column::reduce_shares,
                       Column::exec_price, Column::replace_shares, Column::replace_price,
                       Column::trade_ref, Column::trade_side, Column::trade_shares,
                       Column::trade_price});
    }
    for (const Column column : needed) {
        if (auto opened = open(column); !opened.has_value()) {
            return opened;
        }
    }
    if (need == Need::rows) {
        if (auto opened = seg.open_counted(Column::import_ref, seg.imports); !opened.has_value()) {
            return opened;
        }
    }
    // The time bases whole: a run's records fall anywhere in the segment.
    if (auto opened = seg.open_counted(
            Column::time_base, time_base_count(seg.summary->records, seg.summary->typed_records));
        !opened.has_value()) {
        return opened;
    }
    return seg.engine->col(Column::time_base).all();
}

// One run's records, scanned from its first stored record to `stop`: each one's place in
// every column, the locate's known orders, and the rows of [emit_from, stop) whose time is in
// [start, end), each handed to `emit` with its index in the segment. `before` holds each
// type's stored records before the run (rows only; the minute aggregate reads adds alone).
template <class Emit>
[[nodiscard]] std::expected<void, ItchStoreError> scan_run(
    const PlainSegment& seg, const Stocks& stocks, const Run& run, const TypeCounts& before,
    std::uint64_t emit_from, std::uint64_t stop, std::uint64_t start, std::uint64_t end,
    Need need, Emit&& emit) {
    ItchQuery::Engine& engine = *seg.engine;
    const ColumnAccess& types = engine.col(Column::type);
    ColumnAccess& times = engine.col(Column::timestamp);
    ColumnAccess& gaps = engine.col(Column::locate);
    ColumnAccess& add_side = engine.col(Column::add_side);
    ColumnAccess& add_shares = engine.col(Column::add_shares);
    ColumnAccess& add_price = engine.col(Column::add_price);
    const std::uint64_t first_record = seg.summary->first_record;
    const std::uint64_t first = need == Need::rows ? run.start : emit_from;

    // The run's types from where the scan starts, and its counts per type there.
    if (auto done = seg.types(run.start, stop); !done.has_value()) {
        return done;
    }
    TypeCounts in_run{};
    for (std::uint64_t k = run.start; k < stop; ++k) {
        ++in_run[kSeekSymbolIndex[types[k]]];
    }
    const std::uint64_t run_adds = run.add_end - run.add_start;
    if (in_run[kSymA] + in_run[kSymF] > run_adds) {
        return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run add start",
                                         Column::locate_run_add_start, kNoIndex, run_adds,
                                         in_run[kSymA] + in_run[kSymF]));
    }
    // Every add's side, so each add's place in add_price, grouped by side within the run,
    // follows; add_shares is grouped by locate alone.
    if (auto done = add_side.ensure(run.add_start, run.add_end); !done.has_value()) {
        return done;
    }
    std::array<std::uint64_t, 256> side_base{};
    for (std::uint64_t a = run.add_start; a < run.add_end; ++a) {
        ++side_base[add_side[a]];
    }
    {
        std::uint64_t sum = 0;
        for (std::uint64_t& slot : side_base) {
            const std::uint64_t n = slot;
            slot = run.add_start + sum;
            sum += n;
        }
    }
    if (auto done = add_price.ensure(run.add_start, run.add_end); !done.has_value()) {
        return done;
    }
    if (auto done = add_shares.ensure(run.add_start, run.add_end); !done.has_value()) {
        return done;
    }
    if (auto done = times.ensure(run.start, stop); !done.has_value()) {
        return done;
    }
    if (auto done = gaps.ensure(run.start, stop); !done.has_value()) {
        return done;
    }
    // From the run's first record: the index in the segment of each, as the gaps add up,
    // and its time, its residual over the larger of the time before it and its time base.
    const ColumnAccess& bases = engine.col(Column::time_base);
    std::uint64_t position = 0;
    std::uint64_t time = 0;
    const auto advance = [&](std::uint64_t k) -> std::expected<void, ItchStoreError> {
        const std::uint64_t gap = gaps[k];
        const std::uint64_t records = seg.summary->records;
        const bool first_of_run = k == run.start;
        const std::uint64_t room = first_of_run ? records : records - position - 1;
        if (gap >= room) {
            return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run index",
                                             Column::locate, k, room - 1, gap));
        }
        position = first_of_run ? gap : position + gap + 1;
        const std::uint64_t base = bases[position / kTimeBaseRecords];
        time = times[k] + (first_of_run ? base : std::max(time, base));
        if (time > kMax48) {
            return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "timestamp value",
                                             Column::timestamp, k, kMax48, time));
        }
        return {};
    };

    if (need == Need::adds) {
        // Adds alone: no reference to resolve, so the scan starts at emit_from, with the
        // adds before it counted from the run's types.
        std::uint64_t add_index = run.add_start;
        std::array<std::uint64_t, 256> side_next = side_base;
        for (std::uint64_t k = run.start; k < stop; ++k) {
            if (auto moved = advance(k); !moved.has_value()) {
                return moved;
            }
            const std::uint64_t type = types[k];
            if (!is_add(type)) {
                continue;
            }
            const std::uint64_t a = add_index++;
            const std::uint64_t side = add_side[a];
            const std::uint64_t price_at = side_next[side]++;
            if (k < first) {
                continue;
            }
            const std::uint64_t ts = time;
            if (ts < start || ts >= end) {
                continue;
            }
            QueryRow row;
            row.seq = first_record + position;
            row.ts_ns = ts;
            row.msg_type = static_cast<std::uint16_t>(type);
            row.locate = run.locate;
            row.side = static_cast<std::uint8_t>(side);
            row.shares = static_cast<std::uint32_t>(add_shares[a]);
            row.price = static_cast<std::uint32_t>(add_price[price_at]);
            emit(position, row);
        }
        return {};
    }

    // Rows: each column's first index for the run, then the ranges the scan reads.
    ColumnAccess& creation = engine.col(Column::creation_ref);
    ColumnAccess& distance = engine.col(Column::ref_distance);
    ColumnAccess& imports = engine.col(Column::import_ref);
    ColumnAccess& reduce = engine.col(Column::reduce_shares);
    ColumnAccess& exec_price = engine.col(Column::exec_price);
    ColumnAccess& replace_shares = engine.col(Column::replace_shares);
    ColumnAccess& replace_price = engine.col(Column::replace_price);
    ColumnAccess& trade_ref = engine.col(Column::trade_ref);
    ColumnAccess& trade_side = engine.col(Column::trade_side);
    ColumnAccess& trade_shares = engine.col(Column::trade_shares);
    ColumnAccess& trade_price = engine.col(Column::trade_price);
    if (before[kSymA] + before[kSymF] != run.add_start) {
        return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run add start",
                                         Column::locate_run_add_start, kNoIndex,
                                         before[kSymA] + before[kSymF], run.add_start));
    }
    TypeCounts next{};
    for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
        next[symbol] = before[symbol];
    }
    const std::uint64_t creation_first = before[kSymA] + before[kSymF] + before[kSymU];
    const auto range = [&](ColumnAccess& column, std::uint64_t from,
                           std::uint64_t n) -> std::expected<void, ItchStoreError> {
        return column.ensure(from, from + n);
    };
    std::expected<void, ItchStoreError> ready = range(
        creation, creation_first, in_run[kSymA] + in_run[kSymF] + in_run[kSymU]);
    for (const std::size_t symbol : {kSymC, kSymD, kSymE, kSymU, kSymX}) {
        if (ready.has_value()) {
            ready = range(distance, seg.ref_group[symbol] + before[symbol], in_run[symbol]);
        }
    }
    for (const std::size_t symbol : {kSymC, kSymE, kSymX}) {
        if (ready.has_value()) {
            ready = range(reduce, seg.reduce_group[symbol] + before[symbol], in_run[symbol]);
        }
    }
    if (ready.has_value()) {
        ready = range(exec_price, before[kSymC], in_run[kSymC]);
    }
    for (ColumnAccess* column : {&replace_shares, &replace_price}) {
        if (ready.has_value()) {
            ready = range(*column, before[kSymU], in_run[kSymU]);
        }
    }
    for (ColumnAccess* column : {&trade_ref, &trade_side, &trade_shares, &trade_price}) {
        if (ready.has_value()) {
            ready = range(*column, before[kSymP], in_run[kSymP]);
        }
    }
    if (!ready.has_value()) {
        return ready;
    }

    std::vector<std::uint64_t>& list = engine.list;
    list.clear();
    std::uint64_t created = creation_first;
    std::uint64_t imported = run.import_start;
    std::uint64_t add_index = run.add_start;
    std::array<std::uint64_t, 256> side_next = side_base;
    for (std::uint64_t k = run.start; k < stop; ++k) {
        if (auto moved = advance(k); !moved.has_value()) {
            return moved;
        }
        const std::uint64_t type = types[k];
        const std::size_t symbol = kSeekSymbolIndex[type];
        const std::uint64_t r = next[symbol]++;
        std::uint64_t order_ref = 0;
        if (names(symbol)) {
            const std::uint64_t at = seg.ref_group[symbol] + r;
            const std::uint64_t d = distance[at];
            if (d == 0) {
                if (imported >= run.import_end) {
                    return std::unexpected(seg.error(ItchStoreErrorKind::bad_value,
                                                     "locate run import start",
                                                     Column::locate_run_import_start, kNoIndex,
                                                     run.import_end, imported + 1));
                }
                auto got = imports.get(imported++);
                if (!got.has_value()) {
                    return std::unexpected(got.error());
                }
                order_ref = *got;
                list.push_back(order_ref);
            } else {
                if (d > list.size()) {
                    return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "ref distance",
                                                     Column::ref_distance, at, list.size(), d));
                }
                order_ref = list[list.size() - d];
            }
        }
        std::uint64_t created_ref = 0;
        if (is_add(type) || symbol == kSymU) {
            created_ref = creation[created++];
            list.push_back(created_ref);
        }
        std::uint64_t add = 0;
        std::uint64_t price_at = 0;
        if (is_add(type)) {
            add = add_index++;
            price_at = side_next[add_side[add]]++;
        }
        if (k < first || k < emit_from) {
            continue;
        }
        const std::uint64_t ts = time;
        if (ts < start || ts >= end) {
            continue;
        }
        QueryRow row;
        row.seq = first_record + position;
        row.ts_ns = ts;
        row.msg_type = static_cast<std::uint16_t>(type);
        row.locate = run.locate;
        if (is_add(type)) {
            row.order_ref = created_ref;
            row.side = static_cast<std::uint8_t>(add_side[add]);
            row.shares = static_cast<std::uint32_t>(add_shares[add]);
            row.price = static_cast<std::uint32_t>(add_price[price_at]);
        } else if (symbol == kSymE || symbol == kSymX) {
            row.order_ref = order_ref;
            row.shares = static_cast<std::uint32_t>(reduce[seg.reduce_group[symbol] + r]);
        } else if (symbol == kSymC) {
            row.order_ref = order_ref;
            row.shares = static_cast<std::uint32_t>(reduce[seg.reduce_group[symbol] + r]);
            row.price = static_cast<std::uint32_t>(exec_price[r]);
        } else if (symbol == kSymD) {
            row.order_ref = order_ref;
        } else if (symbol == kSymU) {
            row.order_ref = order_ref;
            row.shares = static_cast<std::uint32_t>(replace_shares[r]);
            row.price = static_cast<std::uint32_t>(replace_price[r]);
        } else if (symbol == kSymP) {
            row.order_ref = trade_ref[r];
            row.side = static_cast<std::uint8_t>(trade_side[r]);
            row.shares = static_cast<std::uint32_t>(trade_shares[r]);
            row.price = static_cast<std::uint32_t>(trade_price[r]);
        }
        if (is_add(type) || symbol == kSymP) {
            auto stock = stock_of(seg, stocks, k, run.locate);
            if (!stock.has_value()) {
                return std::unexpected(stock.error());
            }
            row.has_stock = true;
            row.stock = *stock;
        }
        emit(position, row);
    }
    return {};
}

// The stored records [first, last) of a run whose seek groups can hold a time in
// [start, end), or nothing if none can: the first and last such group, clipped to the run.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> zone_range(
    const SeekIndex& seek, const Run& run, std::uint64_t start, std::uint64_t end) {
    std::optional<std::uint64_t> first;
    std::uint64_t last = 0;
    for (std::uint64_t g = run.start / kSeekGroupRecords;
         g < seek.groups && g * kSeekGroupRecords < run.end; ++g) {
        if (seek.max_time[g] >= start && seek.min_time[g] < end) {
            if (!first.has_value()) {
                first = g;
            }
            last = g;
        }
    }
    if (!first.has_value()) {
        return std::nullopt;
    }
    return std::pair{std::max(run.start, *first * kSeekGroupRecords),
                     std::min(run.end, (last + 1) * kSeekGroupRecords)};
}

// The indexed path for one locate in one plain segment: the rows of the locate in
// [start, end), or its adds for the minute aggregate, in file order.
template <class Sink>
[[nodiscard]] std::expected<void, ItchStoreError> locate_segment(
    const ItchStoreReader& reader, ItchQuery::Engine& engine, std::uint64_t segment,
    std::uint64_t start, std::uint64_t end, std::uint16_t locate, Need need, QueryStats& stats,
    Sink&& sink) {
    ++stats.segments;
    PlainSegment seg = plain_segment(&reader, &engine, &stats, segment);
    if (auto parsed = seg.parse(); !parsed.has_value()) {
        return parsed;
    }
    auto runs = seg.open_directory();
    if (!runs.has_value()) {
        return std::unexpected(runs.error());
    }
    // The summary's bitmap held the locate, so it has a run, whose index is its rank.
    auto run = seg.run(rank_of(seg.summary->locates, locate), *runs, locate);
    if (!run.has_value()) {
        return std::unexpected(run.error());
    }
    const auto zone = zone_range(seg.seek, *run, start, end);
    if (!zone.has_value()) {
        return {};
    }
    if (auto opened = open_scan_columns(seg, need); !opened.has_value()) {
        return opened;
    }
    Stocks stocks;
    TypeCounts before{};
    if (need == Need::rows) {
        auto opened = open_stocks(seg);
        if (!opened.has_value()) {
            return std::unexpected(opened.error());
        }
        stocks = *opened;
        auto counted = seg.before(run->start);
        if (!counted.has_value()) {
            return std::unexpected(counted.error());
        }
        before = *counted;
    }
    // A locate's records are in file order within its run, so its rows go out as found.
    return scan_run(seg, stocks, *run, before, zone->first, zone->second, start, end, need,
                    [&](std::uint64_t, const QueryRow& row) { sink(row); });
}

// The file groups whose times can fall in [start, end), as the records [first, last) of
// the segment in file order, or nothing if none can.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> file_range(
    const SeekIndex& seek, std::uint64_t start, std::uint64_t end) {
    std::optional<std::uint64_t> first;
    std::uint64_t last = 0;
    for (std::uint64_t g = 0; g < seek.groups; ++g) {
        if (seek.file_max_time[g] >= start && seek.file_min_time[g] < end) {
            if (!first.has_value()) {
                first = g;
            }
            last = g;
        }
    }
    if (!first.has_value()) {
        return std::nullopt;
    }
    return std::pair{*first * kSeekGroupRecords,
                     std::min<std::uint64_t>((last + 1) * kSeekGroupRecords, seek.records)};
}

// Every stored typed record's index in the segment, from the whole locate column and the
// run starts.
[[nodiscard]] std::expected<void, ItchStoreError> all_positions(const PlainSegment& seg,
                                                                std::uint64_t runs) {
    ColumnAccess& gaps = seg.engine->col(Column::locate);
    ColumnAccess& starts = seg.engine->col(Column::locate_run_start);
    if (auto opened = seg.open_counted(Column::locate, seg.summary->typed_records);
        !opened.has_value()) {
        return opened;
    }
    for (ColumnAccess* column : {&gaps, &starts}) {
        if (auto done = column->all(); !done.has_value()) {
            return done;
        }
    }
    const std::span<const std::uint64_t> start_values = starts.values().first(runs);
    auto placed = positions_from_runs(gaps.values(), start_values, seg.summary->typed_records,
                                      seg.summary->records);
    if (!placed.has_value()) {
        ItchStoreError e = placed.error();
        e.segment = seg.segment;
        return std::unexpected(e);
    }
    seg.engine->positions = std::move(*placed);
    return {};
}

// The indexed path for every locate in one plain segment: the rows in [start, end), in
// file order. Each run with a record in the file groups the window can reach is scanned
// from its first record to its last such record, and the rows are put back in file order
// by their index in the segment.
template <class Sink>
[[nodiscard]] std::expected<void, ItchStoreError> all_segment(
    const ItchStoreReader& reader, ItchQuery::Engine& engine, std::uint64_t segment,
    std::uint64_t start, std::uint64_t end, QueryStats& stats, Sink&& sink) {
    ++stats.segments;
    PlainSegment seg = plain_segment(&reader, &engine, &stats, segment);
    if (auto parsed = seg.parse(); !parsed.has_value()) {
        return parsed;
    }
    const auto window = file_range(seg.seek, start, end);
    if (!window.has_value()) {
        return {};
    }
    auto runs = seg.open_directory();
    if (!runs.has_value()) {
        return std::unexpected(runs.error());
    }
    for (const Column column : {Column::locate_run, Column::locate_run_start,
                                Column::locate_run_add_start, Column::locate_run_import_start}) {
        if (auto done = engine.col(column).all(); !done.has_value()) {
            return done;
        }
    }
    if (auto checked = check_run_directory(
            engine.col(Column::locate_run).values(), engine.col(Column::locate_run_start).values(),
            engine.col(Column::locate_run_add_start).values(),
            engine.col(Column::locate_run_import_start).values(), seg.summary->typed_records,
            seg.adds, seg.imports);
        !checked.has_value()) {
        ItchStoreError e = checked.error();
        e.segment = segment;
        return std::unexpected(e);
    }
    if (auto placed = all_positions(seg, *runs); !placed.has_value()) {
        return placed;
    }
    if (auto opened = open_scan_columns(seg, Need::rows); !opened.has_value()) {
        return opened;
    }
    auto opened = open_stocks(seg);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    const Stocks stocks = *opened;
    const std::vector<std::uint64_t>& positions = engine.positions;
    const auto [lo, hi] = *window;
    engine.by_position.resize(hi - lo);
    engine.filled.assign(hi - lo, 0);

    // Each run's records in the file groups, by a search of its ascending indices; then the
    // types up to the last such record, counted at each run's first.
    struct Wanted {
        Run run;
        std::uint64_t from = 0;
        std::uint64_t stop = 0;
    };
    std::vector<Wanted> wanted;
    for (std::uint64_t r = 0; r < *runs; ++r) {
        auto run = seg.run(r, *runs, static_cast<std::uint16_t>(engine.col(Column::locate_run)[r]));
        if (!run.has_value()) {
            return std::unexpected(run.error());
        }
        const auto first = positions.begin() + static_cast<std::ptrdiff_t>(run->start);
        const auto last = positions.begin() + static_cast<std::ptrdiff_t>(run->end);
        const auto from = std::lower_bound(first, last, lo);
        const auto stop = std::lower_bound(from, last, hi);
        if (from == stop) {
            continue;
        }
        wanted.push_back({.run = *run,
                          .from = static_cast<std::uint64_t>(from - positions.begin()),
                          .stop = static_cast<std::uint64_t>(stop - positions.begin())});
    }
    if (wanted.empty()) {
        return {};
    }
    const std::uint64_t reach = wanted.back().stop;
    if (auto done = seg.types(0, reach); !done.has_value()) {
        return done;
    }
    TypeCounts counts{};
    std::uint64_t k = 0;
    const ColumnAccess& types = engine.col(Column::type);
    for (const Wanted& w : wanted) {
        for (; k < w.run.start; ++k) {
            ++counts[kSeekSymbolIndex[types[k]]];
        }
        if (auto done = scan_run(seg, stocks, w.run, counts, w.from, w.stop, start, end,
                                 Need::rows,
                                 [&](std::uint64_t position, const QueryRow& row) {
                                     engine.by_position[position - lo] = row;
                                     engine.filled[position - lo] = 1;
                                 });
            !done.has_value()) {
            return done;
        }
    }
    for (std::uint64_t i = 0; i < hi - lo; ++i) {
        if (engine.filled[i] != 0) {
            sink(engine.by_position[i]);
        }
    }
    return {};
}

}  // namespace

ItchQuery::ItchQuery(const ItchStoreReader& reader, QueryPath path)
    : reader_(&reader), path_(path), engine_(std::make_unique<Engine>()) {
    engine_->last_ts.resize(reader.segment_count());
}

ItchQuery::ItchQuery(ItchQuery&&) noexcept = default;
ItchQuery& ItchQuery::operator=(ItchQuery&&) noexcept = default;
ItchQuery::~ItchQuery() = default;

namespace {

// The timestamp of the row before segment `segment`'s first: 0 for the file's first, the
// last time of a plain segment before it, or what decoding the segments since the last
// plain one exactly gives.
[[nodiscard]] std::expected<std::uint64_t, ItchStoreError> seed_before(
    const ItchStoreReader& reader, ItchQuery::Engine& engine, std::uint64_t segment,
    QueryStats& stats) {
    const auto known = [&](std::uint64_t k) {
        return reader.segment(k).summary.plain || engine.last_ts[k].has_value();
    };
    const auto last_of = [&](std::uint64_t k) {
        return engine.last_ts[k].has_value() ? *engine.last_ts[k]
                                             : reader.segment(k).summary.last_time;
    };
    std::uint64_t from = segment;
    while (from > 0 && !known(from - 1)) {
        --from;
    }
    std::uint64_t seed = from == 0 ? 0 : last_of(from - 1);
    for (std::uint64_t k = from; k < segment; ++k) {
        auto last = exact_segment(reader, engine, k, seed, stats, [](const QueryRow&) {});
        if (!last.has_value()) {
            return std::unexpected(last.error());
        }
        engine.last_ts[k] = *last;
        seed = *last;
    }
    return seed;
}

// Whether a segment can hold a row of the window (and of the locate), from its summary,
// and if it is not plain, the row before it.
[[nodiscard]] std::expected<bool, ItchStoreError> may_hold(
    const ItchStoreReader& reader, ItchQuery::Engine& engine, std::uint64_t segment,
    std::uint64_t start, std::uint64_t end, std::optional<std::uint16_t> locate,
    QueryStats& stats) {
    const SegmentSummary& summary = reader.segment(segment).summary;
    const bool typed_hit =
        summary.typed_records > 0 && summary.max_time >= start && summary.min_time < end;
    if (summary.plain) {
        return typed_hit && (!locate.has_value() || bit_of(summary.locates, *locate));
    }
    // Not plain: a raw row has locate 0 and repeats the timestamp of the row before it,
    // which is one of the segment's own times or the row before the segment.
    if (locate.has_value() && *locate != 0) {
        return typed_hit && bit_of(summary.locates, *locate);
    }
    if (typed_hit) {
        return true;
    }
    auto seed = seed_before(reader, engine, segment, stats);
    if (!seed.has_value()) {
        return std::unexpected(seed.error());
    }
    return *seed >= start && *seed < end;
}

}  // namespace

std::expected<std::vector<QueryRow>, ItchStoreError> ItchQuery::rows(
    std::uint64_t start_ns, std::uint64_t end_ns, std::optional<std::uint16_t> locate) {
    stats_ = {};
    std::vector<QueryRow> out;
    if (start_ns >= end_ns) {
        return out;
    }
    const auto keep = [&](const QueryRow& row) {
        if (row.ts_ns >= start_ns && row.ts_ns < end_ns &&
            (!locate.has_value() || row.locate == *locate)) {
            out.push_back(row);
        }
    };
    for (std::uint64_t k = 0; k < reader_->segment_count(); ++k) {
        auto hold = may_hold(*reader_, *engine_, k, start_ns, end_ns, locate, stats_);
        if (!hold.has_value()) {
            return std::unexpected(hold.error());
        }
        if (!*hold) {
            continue;
        }
        const SegmentSummary& summary = reader_->segment(k).summary;
        if (summary.plain && path_ == QueryPath::indexed) {
            auto done = locate.has_value()
                            ? locate_segment(*reader_, *engine_, k, start_ns, end_ns, *locate,
                                             Need::rows, stats_, keep)
                            : all_segment(*reader_, *engine_, k, start_ns, end_ns, stats_, keep);
            if (!done.has_value()) {
                return std::unexpected(done.error());
            }
            continue;
        }
        auto seed = summary.plain ? std::expected<std::uint64_t, ItchStoreError>{0}
                                  : seed_before(*reader_, *engine_, k, stats_);
        if (!seed.has_value()) {
            return std::unexpected(seed.error());
        }
        auto last = exact_segment(*reader_, *engine_, k, *seed, stats_, keep);
        if (!last.has_value()) {
            return std::unexpected(last.error());
        }
        engine_->last_ts[k] = *last;
    }
    stats_.rows = out.size();
    return out;
}

std::expected<std::vector<MinuteBucket>, ItchStoreError> ItchQuery::minute_aggregate(
    std::uint64_t start_ns, std::uint64_t end_ns, std::uint16_t locate) {
    stats_ = {};
    // The minutes in order. Rows come in file order, whose times rarely fall, so a row's
    // minute is almost always the last one or a new last one.
    std::vector<MinuteBucket> minutes;
    if (start_ns >= end_ns) {
        return minutes;
    }
    const auto add = [&](const QueryRow& row) {
        if (row.ts_ns < start_ns || row.ts_ns >= end_ns || row.locate != locate ||
            !is_add(row.msg_type)) {
            return;
        }
        const std::uint64_t minute_ns = (row.ts_ns / kNanosPerMinute) * kNanosPerMinute;
        auto slot = minutes.end();
        if (minutes.empty() || minutes.back().minute_ns < minute_ns) {
            slot = minutes.insert(minutes.end(), MinuteBucket{});
        } else if (minutes.back().minute_ns == minute_ns) {
            slot = minutes.end() - 1;
        } else {
            slot = std::ranges::lower_bound(minutes, minute_ns, {}, &MinuteBucket::minute_ns);
            if (slot->minute_ns != minute_ns) {
                slot = minutes.insert(slot, MinuteBucket{});
            }
        }
        MinuteBucket& bucket = *slot;
        if (bucket.count == 0) {
            bucket.minute_ns = minute_ns;
            bucket.min_price = row.price;
            bucket.max_price = row.price;
        }
        ++bucket.count;
        bucket.shares += row.shares;
        bucket.min_price = std::min<std::uint64_t>(bucket.min_price, row.price);
        bucket.max_price = std::max<std::uint64_t>(bucket.max_price, row.price);
    };
    for (std::uint64_t k = 0; k < reader_->segment_count(); ++k) {
        auto hold = may_hold(*reader_, *engine_, k, start_ns, end_ns, locate, stats_);
        if (!hold.has_value()) {
            return std::unexpected(hold.error());
        }
        if (!*hold) {
            continue;
        }
        const SegmentSummary& summary = reader_->segment(k).summary;
        if (summary.plain && path_ == QueryPath::indexed) {
            if (auto done = locate_segment(*reader_, *engine_, k, start_ns, end_ns, locate,
                                           Need::adds, stats_, add);
                !done.has_value()) {
                return std::unexpected(done.error());
            }
            continue;
        }
        auto seed = summary.plain ? std::expected<std::uint64_t, ItchStoreError>{0}
                                  : seed_before(*reader_, *engine_, k, stats_);
        if (!seed.has_value()) {
            return std::unexpected(seed.error());
        }
        auto last = exact_segment(*reader_, *engine_, k, *seed, stats_, add);
        if (!last.has_value()) {
            return std::unexpected(last.error());
        }
        engine_->last_ts[k] = *last;
    }
    stats_.rows = minutes.size();
    return minutes;
}

std::expected<ColumnSum, ItchStoreError> ItchQuery::shares_sum() {
    stats_ = {};
    ColumnSum sum;
    for (std::uint64_t k = 0; k < reader_->segment_count(); ++k) {
        const SegmentSummary& summary = reader_->segment(k).summary;
        sum.records += summary.records;
        if (summary.plain && path_ == QueryPath::indexed) {
            // The shares column is add_shares, reduce_shares, replace_shares and
            // trade_shares; every other row holds 0.
            ++stats_.segments;
            PlainSegment seg = plain_segment(reader_, engine_.get(), &stats_, k);
            if (auto parsed = seg.parse(); !parsed.has_value()) {
                return std::unexpected(parsed.error());
            }
            const auto total = [&](std::size_t symbol) -> std::uint64_t {
                return seg.total[symbol];
            };
            const std::array<std::pair<Column, std::uint64_t>, 4> parts{{
                {Column::add_shares, total(kSymA) + total(kSymF)},
                {Column::reduce_shares, total(kSymC) + total(kSymE) + total(kSymX)},
                {Column::replace_shares, total(kSymU)},
                {Column::trade_shares, total(kSymP)},
            }};
            for (const auto& [column, want] : parts) {
                if (auto opened = seg.open(column); !opened.has_value()) {
                    return std::unexpected(opened.error());
                }
                ColumnAccess& access = engine_->col(column);
                if (access.size() != want) {
                    return std::unexpected(seg.error(ItchStoreErrorKind::bad_segment,
                                                     "column value count", column, kNoIndex,
                                                     want, access.size()));
                }
                if (auto done = access.all(); !done.has_value()) {
                    return std::unexpected(done.error());
                }
                for (const std::uint64_t shares : access.values()) {
                    sum.sum += shares;
                }
            }
            continue;
        }
        auto seed = summary.plain ? std::expected<std::uint64_t, ItchStoreError>{0}
                                  : seed_before(*reader_, *engine_, k, stats_);
        if (!seed.has_value()) {
            return std::unexpected(seed.error());
        }
        auto last = exact_segment(*reader_, *engine_, k, *seed, stats_,
                                  [&](const QueryRow& row) { sum.sum += row.shares; });
        if (!last.has_value()) {
            return std::unexpected(last.error());
        }
        engine_->last_ts[k] = *last;
    }
    stats_.rows = 1;
    return sum;
}

std::expected<std::vector<std::uint64_t>, ItchStoreError> ItchQuery::locate_counts(
    std::uint64_t start_ns, std::uint64_t end_ns) {
    stats_ = {};
    std::vector<std::uint64_t> counts(kLocates, 0);
    if (start_ns >= end_ns) {
        return counts;
    }
    for (std::uint64_t k = 0; k < reader_->segment_count(); ++k) {
        auto hold = may_hold(*reader_, *engine_, k, start_ns, end_ns, std::nullopt, stats_);
        if (!hold.has_value()) {
            return std::unexpected(hold.error());
        }
        if (!*hold) {
            continue;
        }
        const SegmentSummary& summary = reader_->segment(k).summary;
        if (summary.plain && path_ == QueryPath::indexed) {
            ++stats_.segments;
            PlainSegment seg = plain_segment(reader_, engine_.get(), &stats_, k);
            if (auto parsed = seg.parse(); !parsed.has_value()) {
                return std::unexpected(parsed.error());
            }
            if (!file_range(seg.seek, start_ns, end_ns).has_value()) {
                continue;
            }
            // Every record's time, which its run's earlier ones fix, and its locate from the
            // run it falls in.
            auto runs = seg.open_directory();
            if (!runs.has_value()) {
                return std::unexpected(runs.error());
            }
            for (const Column column :
                 {Column::locate_run, Column::locate_run_start, Column::locate_run_add_start,
                  Column::locate_run_import_start}) {
                if (auto done = engine_->col(column).all(); !done.has_value()) {
                    return std::unexpected(done.error());
                }
            }
            const ColumnAccess& ids = engine_->col(Column::locate_run);
            const ColumnAccess& starts = engine_->col(Column::locate_run_start);
            if (auto checked = check_run_directory(
                    ids.values(), starts.values(),
                    engine_->col(Column::locate_run_add_start).values(),
                    engine_->col(Column::locate_run_import_start).values(),
                    summary.typed_records, seg.adds, seg.imports);
                !checked.has_value()) {
                ItchStoreError e = checked.error();
                e.segment = k;
                return std::unexpected(e);
            }
            if (auto placed = all_positions(seg, *runs); !placed.has_value()) {
                return std::unexpected(placed.error());
            }
            const std::uint64_t bases = time_base_count(summary.records, summary.typed_records);
            for (const auto& [column, want] :
                 {std::pair{Column::timestamp, std::uint64_t{summary.typed_records}},
                  std::pair{Column::time_base, bases}}) {
                if (auto opened = seg.open_counted(column, want); !opened.has_value()) {
                    return std::unexpected(opened.error());
                }
                if (auto done = engine_->col(column).all(); !done.has_value()) {
                    return std::unexpected(done.error());
                }
            }
            auto times = times_from_residuals(engine_->col(Column::timestamp).values(),
                                              engine_->positions, starts.values(),
                                              engine_->col(Column::time_base).values());
            if (!times.has_value()) {
                ItchStoreError e = times.error();
                e.segment = k;
                return std::unexpected(e);
            }
            std::uint64_t r = 0;
            for (std::uint64_t j = 0; j < times->size(); ++j) {
                while (r + 1 < *runs && starts[r + 1] <= j) {
                    ++r;
                }
                if ((*times)[j] >= start_ns && (*times)[j] < end_ns) {
                    ++counts[ids[r]];
                }
            }
            continue;
        }
        auto seed = summary.plain ? std::expected<std::uint64_t, ItchStoreError>{0}
                                  : seed_before(*reader_, *engine_, k, stats_);
        if (!seed.has_value()) {
            return std::unexpected(seed.error());
        }
        auto last = exact_segment(*reader_, *engine_, k, *seed, stats_, [&](const QueryRow& row) {
            if (row.ts_ns >= start_ns && row.ts_ns < end_ns) {
                ++counts[row.locate];
            }
        });
        if (!last.has_value()) {
            return std::unexpected(last.error());
        }
        engine_->last_ts[k] = *last;
    }
    return counts;
}

}  // namespace tick_store::store
