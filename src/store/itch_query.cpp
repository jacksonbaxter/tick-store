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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <map>
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

[[nodiscard]] bool is_add(std::uint64_t type) noexcept { return type == 'A' || type == 'F'; }

// One column of one segment, decoded a codec block at a time as values are asked for. The
// container block is read and CRC-checked when the column is opened, and a zstd chunk
// inflated then, since zstd has no block boundaries inside it. Every value is checked
// against the column's range as its block is decoded.
class ColumnAccess {
public:
    // `most` bounds the values the segment's records can give the column, checked before
    // anything is inflated or allocated.
    [[nodiscard]] std::expected<void, ItchStoreError> open(const ItchStoreReader& reader,
                                                           std::uint64_t segment, Column column,
                                                           std::uint64_t most, QueryStats& stats) {
        column_ = column;
        segment_ = segment;
        stats_ = &stats;
        starts_.clear();
        offsets_.clear();
        decoded_.clear();
        count_ = 0;
        bytes_ = {};
        auto view = reader.column_view(segment, column);
        if (!view.has_value()) {
            return std::unexpected(view.error());
        }
        block_ = view->block;
        file_offset_ = view->file_offset;
        if (view->block == kNoIndex) {
            return {};
        }
        if (view->values > most) {
            return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column value count",
                                          kNoIndex, most, view->values));
        }
        count_ = view->values;
        auto unwrapped = unwrap_column(column, view->codec, view->payload, count_);
        if (!unwrapped.has_value()) {
            return std::unexpected(located(unwrapped.error(), 0));
        }
        bytes_ = std::move(*unwrapped);
        const std::span<const std::byte> bytes = bytes_.bytes();
        decoder_ = column_block_decoder(column);
        if (bytes_.zstd) {
            // The whole compressed chunk is read to reach any value of it.
            stats.bytes_decoded += view->payload.size();
        }
        if (decoder_ == nullptr) {
            // A byte column: the chunk is the values.
            if (bytes.size() != count_) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column bytes",
                                              kNoIndex, count_, bytes.size()));
            }
            reserve();
            for (std::uint64_t i = 0; i < count_; ++i) {
                values_[i] = std::to_integer<std::uint64_t>(bytes[i]);
            }
            starts_ = {0, count_};
            offsets_ = {0, bytes.size()};
            decoded_.assign(1, 1);
            ++stats.blocks;
            if (!bytes_.zstd) {
                stats.bytes_decoded += bytes.size();
            }
            stats.values_decoded += count_;
            return {};
        }
        // The codec blocks' headers, so a value index finds its block without decoding the
        // ones before it.
        std::uint64_t offset = 0;
        std::uint64_t total = 0;
        while (offset < bytes.size()) {
            if (total == count_) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column trailing bytes",
                                              kNoIndex, 0, bytes.size() - offset));
            }
            const auto values = read_le<4>(bytes, offset);
            const auto payload = read_le<4>(bytes, offset + 4);
            if (!values.has_value() || !payload.has_value()) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column block header",
                                              kNoIndex, codec::kBlockHeaderSize,
                                              bytes.size() - offset));
            }
            const std::uint64_t end = offset + codec::kBlockHeaderSize + *payload;
            if (*values == 0 || end > bytes.size()) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column block size",
                                              kNoIndex, bytes.size() - offset, end - offset));
            }
            starts_.push_back(total);
            offsets_.push_back(offset);
            total += *values;
            offset = end;
            if (total > count_) {
                return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column value count",
                                              kNoIndex, count_, total));
            }
        }
        if (total != count_) {
            return std::unexpected(
                refuse(ItchStoreErrorKind::bad_segment, "column value count", kNoIndex, count_, total));
        }
        starts_.push_back(total);
        offsets_.push_back(bytes.size());
        decoded_.assign(starts_.size() - 1, 0);
        reserve();
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
        const auto block_of = [&](std::uint64_t index) {
            return static_cast<std::size_t>(
                std::ranges::upper_bound(starts_, index) - starts_.begin() - 1);
        };
        const std::size_t from = block_of(first);
        const std::size_t to = block_of(last - 1);
        for (std::size_t b = from; b <= to; ++b) {
            if (decoded_[b] == 0) {
                if (auto done = decode_block(b); !done.has_value()) {
                    return done;
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
        if (index >= count_) {
            return std::unexpected(
                refuse(ItchStoreErrorKind::bad_value, "value index", index, count_, index));
        }
        if (decoded_[block_index(index)] == 0) {
            if (auto done = ensure(index, index + 1); !done.has_value()) {
                return std::unexpected(done.error());
            }
        }
        return values_[index];
    }

    // Every value, after all().
    [[nodiscard]] std::span<const std::uint64_t> values() const noexcept {
        return std::span<const std::uint64_t>{values_}.first(static_cast<std::size_t>(count_));
    }

private:
    // Room for the column's values, kept from segment to segment so it grows to the
    // largest column once.
    void reserve() {
        if (count_ > values_.size()) {
            values_.resize(count_);
        }
    }

    [[nodiscard]] std::size_t block_index(std::uint64_t index) const noexcept {
        // Blocks are almost always kColumnBlockValues long, so guess, then search.
        const std::uint64_t guess = index / kColumnBlockValues;
        if (guess + 1 < starts_.size() && starts_[guess] <= index && index < starts_[guess + 1]) {
            return static_cast<std::size_t>(guess);
        }
        return static_cast<std::size_t>(
            std::ranges::upper_bound(starts_, index) - starts_.begin() - 1);
    }

    [[nodiscard]] std::expected<void, ItchStoreError> decode_block(std::size_t b) {
        const std::span<const std::byte> bytes = bytes_.bytes();
        const std::span<const std::byte> block =
            bytes.subspan(offsets_[b], offsets_[b + 1] - offsets_[b]);
        const std::uint64_t first = starts_[b];
        const std::uint64_t n = starts_[b + 1] - first;
        scratch_.clear();
        const auto taken = decoder_(block, scratch_);
        if (!taken.has_value()) {
            ItchStoreError error = refuse(ItchStoreErrorKind::codec, taken.error().field, kNoIndex, 0, 0);
            error.codec = taken.error();
            error.offset = bytes_.zstd ? file_offset_
                                       : file_offset_ + offsets_[b] + (taken.error().offset / 8U);
            return std::unexpected(error);
        }
        if (*taken != block.size() || scratch_.size() != n) {
            return std::unexpected(refuse(ItchStoreErrorKind::bad_segment, "column block values",
                                          first, n, scratch_.size()));
        }
        const std::uint64_t max = column_info(column_).max_value;
        for (std::uint64_t i = 0; i < n; ++i) {
            if (scratch_[i] > max) {
                return std::unexpected(
                    refuse(ItchStoreErrorKind::bad_value, "column value", first + i, max, scratch_[i]));
            }
        }
        std::memcpy(values_.data() + first, scratch_.data(), n * sizeof(std::uint64_t));
        decoded_[b] = 1;
        ++stats_->blocks;
        if (!bytes_.zstd) {
            stats_->bytes_decoded += block.size();
        }
        stats_->values_decoded += n;
        return {};
    }

    [[nodiscard]] ItchStoreError located(ItchStoreError error, std::uint64_t at) const {
        error.segment = segment_;
        error.block = block_;
        error.offset = file_offset_ + at;
        return error;
    }

    [[nodiscard]] ItchStoreError refuse(ItchStoreErrorKind kind, std::string_view field,
                                        std::uint64_t index, std::uint64_t expected,
                                        std::uint64_t actual) const {
        ItchStoreError error = query_error(kind, field, segment_, column_, index, expected, actual);
        error.block = block_;
        return error;
    }

    Column column_ = Column::type;
    std::uint64_t segment_ = 0;
    std::uint64_t block_ = kNoIndex;
    std::uint64_t file_offset_ = 0;
    std::uint64_t count_ = 0;
    ChunkBytes bytes_;
    codec::BlockDecoder decoder_ = nullptr;
    std::vector<std::uint64_t> starts_;
    std::vector<std::uint64_t> offsets_;
    std::vector<std::uint8_t> decoded_;
    std::vector<std::uint64_t> values_;
    std::vector<std::uint64_t> scratch_;
    QueryStats* stats_ = nullptr;
};

// What a pass over a segment's records hands on: rows in full, or for the minute aggregate
// the add orders alone with their time, shares and price.
enum class Need : std::uint8_t { rows, adds };

// Indices into kSeekSymbols, which the type column's values are checked against.
const std::size_t kSymA = seek_symbol_index('A');
const std::size_t kSymC = seek_symbol_index('C');
const std::size_t kSymD = seek_symbol_index('D');
const std::size_t kSymE = seek_symbol_index('E');
const std::size_t kSymF = seek_symbol_index('F');
const std::size_t kSymP = seek_symbol_index('P');
const std::size_t kSymU = seek_symbol_index('U');
const std::size_t kSymX = seek_symbol_index('X');

}  // namespace

struct ItchQuery::Engine {
    std::array<ColumnAccess, kColumnCount> cols;
    // Each locate's known orders in the segment being read, as the assembler keeps them.
    std::vector<std::vector<std::uint64_t>> lists = std::vector<std::vector<std::uint64_t>>(kLocates);
    std::vector<std::uint16_t> touched;
    // The timestamp of each segment's last row, once known.
    std::vector<std::optional<std::uint64_t>> last_ts;
    std::array<std::byte, itch::kMaxPayloadSize> scratch{};

    ColumnAccess& col(Column column) { return cols[static_cast<std::size_t>(column)]; }

    void clear_lists() {
        for (const std::uint16_t locate : touched) {
            lists[locate].clear();
        }
        touched.clear();
    }

    void append(std::uint16_t locate, std::uint64_t reference) {
        std::vector<std::uint64_t>& list = lists[locate];
        if (list.empty()) {
            touched.push_back(locate);
        }
        list.push_back(reference);
    }
};

namespace {

// The bytes of every block of a segment, which the exact path reads whole.
void count_whole_segment(const ItchStoreReader& reader, std::uint64_t segment, QueryStats& stats) {
    const SegmentEntry& entry = reader.segment(segment);
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        if (entry.block[c] == kNoIndex) {
            continue;
        }
        if (const auto info = reader.container().block_info(entry.block[c]); info.has_value()) {
            stats.bytes_decoded += info->payload_size;
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

// What the indexed path keeps of one plain segment while it answers.
struct PlainSegment {
    const ItchStoreReader* reader = nullptr;
    ItchQuery::Engine* engine = nullptr;
    QueryStats* stats = nullptr;
    std::uint64_t segment = 0;
    const SegmentSummary* summary = nullptr;
    SeekIndex seek;
    // The record range the time zone maps leave, [lo, hi).
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    // Records of each type byte, once types() has run.
    std::array<std::uint64_t, 256> per_type{};

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

    // Parses the seek index and narrows [lo, hi) to the groups whose times can fall in
    // [start, end). Returns false if none can.
    [[nodiscard]] std::expected<bool, ItchStoreError> narrow(std::uint64_t start,
                                                             std::uint64_t end) {
        const std::uint32_t records = summary->records;
        if (summary->typed_records != records) {
            const ItchStoreError e = error(ItchStoreErrorKind::bad_metadata, "plain typed records",
                                     Column::type, kNoIndex, records, summary->typed_records);
            return std::unexpected(e);
        }
        if (auto opened = open(Column::seek); !opened.has_value()) {
            return std::unexpected(opened.error());
        }
        ColumnAccess& column = engine->col(Column::seek);
        if (auto done = column.all(); !done.has_value()) {
            return std::unexpected(done.error());
        }
        auto parsed = parse_seek(column.values(), records);
        if (!parsed.has_value()) {
            ItchStoreError e = parsed.error();
            e.segment = segment;
            return std::unexpected(e);
        }
        seek = std::move(*parsed);
        std::optional<std::uint64_t> first;
        std::uint64_t last = 0;
        for (std::uint64_t g = 0; g < seek.groups; ++g) {
            if (seek.max_time[g] >= start && seek.min_time[g] < end) {
                if (!first.has_value()) {
                    first = g;
                }
                last = g;
            }
        }
        if (!first.has_value()) {
            return false;
        }
        lo = *first * kSeekGroupRecords;
        hi = std::min<std::uint64_t>((last + 1) * kSeekGroupRecords, records);
        return true;
    }

    // The type column whole, every value a type symbol a typed record may carry.
    [[nodiscard]] std::expected<std::span<const std::uint64_t>, ItchStoreError> types() {
        if (auto opened = open(Column::type); !opened.has_value()) {
            return std::unexpected(opened.error());
        }
        ColumnAccess& column = engine->col(Column::type);
        if (column.size() != summary->records) {
            return std::unexpected(error(ItchStoreErrorKind::bad_segment, "column value count",
                                         Column::type, kNoIndex, summary->records, column.size()));
        }
        if (auto done = column.all(); !done.has_value()) {
            return std::unexpected(done.error());
        }
        const std::span<const std::uint64_t> values = column.values();
        per_type.fill(0);
        for (std::size_t i = 0; i < values.size(); ++i) {
            // The column's range is 8 bits, which decoding checked.
            if (values[i] == 0 || kSeekSymbolIndex[values[i]] == kSeekSymbols.size()) {
                return std::unexpected(error(ItchStoreErrorKind::bad_value, "type symbol",
                                             Column::type, i, 0, values[i]));
            }
            ++per_type[values[i]];
        }
        return values;
    }

    // Opens `column` and checks it holds the values the types give.
    [[nodiscard]] std::expected<void, ItchStoreError> open_counted(
        Column column, const std::array<std::uint64_t, kColumnCount>& counts) const {
        if (auto opened = open(column); !opened.has_value()) {
            return opened;
        }
        const std::uint64_t want = counts[static_cast<std::size_t>(column)];
        if (engine->col(column).size() != want) {
            return std::unexpected(error(ItchStoreErrorKind::bad_segment, "column value count",
                                         column, kNoIndex, want, engine->col(column).size()));
        }
        return {};
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

// One locate's records in a plain segment, from its run in the locate column.
struct LocateRun {
    bool found = false;
    std::vector<std::uint64_t> positions;
    // Where the locate's adds start in add_shares and add_price.
    std::uint64_t add_offset = 0;
    std::uint64_t adds = 0;
};

[[nodiscard]] std::expected<LocateRun, ItchStoreError> locate_run(const PlainSegment& seg,
                                                                  std::uint16_t locate) {
    for (const Column column : {Column::locate_run, Column::locate_run_records,
                                Column::locate_run_adds, Column::locate}) {
        if (auto opened = seg.open(column); !opened.has_value()) {
            return std::unexpected(opened.error());
        }
    }
    ColumnAccess& ids = seg.engine->col(Column::locate_run);
    ColumnAccess& records = seg.engine->col(Column::locate_run_records);
    ColumnAccess& adds = seg.engine->col(Column::locate_run_adds);
    ColumnAccess& gaps = seg.engine->col(Column::locate);
    for (ColumnAccess* column : {&ids, &records, &adds}) {
        if (auto done = column->all(); !done.has_value()) {
            return std::unexpected(done.error());
        }
    }
    const std::uint64_t runs = ids.size();
    if (records.size() != runs || adds.size() != runs) {
        return std::unexpected(seg.error(ItchStoreErrorKind::bad_segment, "locate run count",
                                         Column::locate_run_records, kNoIndex, runs,
                                         records.size() != runs ? records.size() : adds.size()));
    }
    LocateRun run;
    std::uint64_t record_offset = 0;
    for (std::uint64_t r = 0; r < runs; ++r) {
        if (r > 0 && ids[r] <= ids[r - 1]) {
            return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run order",
                                             Column::locate_run, r, ids[r - 1] + 1, ids[r]));
        }
        if (ids[r] == locate) {
            run.found = true;
            const std::uint64_t n = records[r];
            if (n == 0 || n > gaps.size() - record_offset) {
                return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run records",
                                                 Column::locate_run_records, r,
                                                 gaps.size() - record_offset, n));
            }
            run.adds = adds[r];
            if (auto done = gaps.ensure(record_offset, record_offset + n); !done.has_value()) {
                return std::unexpected(done.error());
            }
            run.positions.reserve(n);
            std::uint64_t at = gaps[record_offset];
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i > 0) {
                    at += gaps[record_offset + i] + 1U;
                }
                if (at >= seg.summary->records) {
                    return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run index",
                                                     Column::locate, record_offset + i,
                                                     seg.summary->records, at));
                }
                run.positions.push_back(at);
            }
            break;
        }
        record_offset += records[r];
        run.add_offset += adds[r];
        if (record_offset > gaps.size()) {
            return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run records",
                                             Column::locate_run_records, r, gaps.size(),
                                             record_offset));
        }
    }
    return run;
}

// Every record's locate in a plain segment, from all the runs.
[[nodiscard]] std::expected<std::vector<std::uint64_t>, ItchStoreError> all_locates(
    const PlainSegment& seg) {
    for (const Column column : {Column::locate_run, Column::locate_run_records, Column::locate}) {
        if (auto opened = seg.open(column); !opened.has_value()) {
            return std::unexpected(opened.error());
        }
        if (auto done = seg.engine->col(column).all(); !done.has_value()) {
            return std::unexpected(done.error());
        }
    }
    auto placed = locates_from_runs(seg.engine->col(Column::locate).values(),
                                    seg.engine->col(Column::locate_run).values(),
                                    seg.engine->col(Column::locate_run_records).values(),
                                    seg.summary->records);
    if (!placed.has_value()) {
        ItchStoreError e = placed.error();
        e.segment = seg.segment;
        return std::unexpected(e);
    }
    return std::move(*placed);
}

// The stock of a derived-stock record: its exception, or its locate's symbol.
struct Stocks {
    std::span<const std::uint64_t> records;
    std::span<const std::uint64_t> bytes;
    std::size_t next = 0;
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
                                         Column::locate, kNoIndex, 1, 0));
    }
    return *known;
}

// The indexed path over one plain segment: the rows in [start, end), of `locate` if set.
template <class Sink>
[[nodiscard]] std::expected<void, ItchStoreError> indexed_segment(
    const ItchStoreReader& reader, ItchQuery::Engine& engine, std::uint64_t segment,
    std::uint64_t start, std::uint64_t end, std::optional<std::uint16_t> locate, Need need,
    QueryStats& stats, Sink&& sink) {
    ++stats.segments;
    PlainSegment seg = plain_segment(&reader, &engine, &stats, segment);
    auto any = seg.narrow(start, end);
    if (!any.has_value()) {
        return std::unexpected(any.error());
    }
    if (!*any) {
        return {};
    }
    auto typed = seg.types();
    if (!typed.has_value()) {
        return std::unexpected(typed.error());
    }
    const std::span<const std::uint64_t> types = *typed;
    const std::uint64_t records = types.size();
    const auto counts = counts_from_type_counts(seg.per_type);

    // Which records the query is about.
    LocateRun run;
    std::vector<std::uint64_t> locates;
    if (locate.has_value()) {
        auto found = locate_run(seg, *locate);
        if (!found.has_value()) {
            return std::unexpected(found.error());
        }
        run = std::move(*found);
        if (!run.found) {
            return {};
        }
    } else {
        auto placed = all_locates(seg);
        if (!placed.has_value()) {
            return std::unexpected(placed.error());
        }
        locates = std::move(*placed);
    }
    const auto locate_at = [&](std::uint64_t record) -> std::uint16_t {
        return locate.has_value() ? *locate : static_cast<std::uint16_t>(locates[record]);
    };

    std::vector<Column> needed{Column::timestamp, Column::add_side, Column::add_shares,
                               Column::add_price};
    if (need == Need::rows) {
        needed.insert(needed.end(),
                      {Column::creation_ref, Column::ref_distance, Column::reduce_shares,
                       Column::exec_price, Column::replace_shares, Column::replace_price,
                       Column::trade_ref, Column::trade_side, Column::trade_shares,
                       Column::trade_price});
    }
    for (const Column column : needed) {
        if (auto opened = seg.open_counted(column, counts); !opened.has_value()) {
            return opened;
        }
    }
    ColumnAccess& times = engine.col(Column::timestamp);
    ColumnAccess& add_side = engine.col(Column::add_side);
    ColumnAccess& add_shares = engine.col(Column::add_shares);
    ColumnAccess& add_price = engine.col(Column::add_price);

    // Each add's place in add_shares, grouped by locate, and in add_price, grouped by
    // locate and side, both stable: what the writer sorted them into.
    struct AddPlace {
        std::uint64_t record;
        std::uint64_t side;
        std::uint64_t shares_at;
        std::uint64_t price_at;
    };
    std::vector<AddPlace> places;
    if (locate.has_value()) {
        // The locate's adds alone: its run against the types.
        std::uint64_t add_index = 0;
        std::size_t next = 0;
        for (std::uint64_t j = 0; j < records && next < run.positions.size(); ++j) {
            const bool mine = run.positions[next] == j;
            next += mine ? 1U : 0U;
            if (!is_add(types[j])) {
                continue;
            }
            if (mine) {
                auto side = add_side.get(add_index);
                if (!side.has_value()) {
                    return std::unexpected(side.error());
                }
                places.push_back({.record = j, .side = *side, .shares_at = 0, .price_at = 0});
            }
            ++add_index;
        }
        if (places.size() != run.adds) {
            return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "locate run adds",
                                             Column::locate_run_adds, kNoIndex, places.size(),
                                             run.adds));
        }
        std::array<std::uint64_t, 256> base{};
        for (const AddPlace& place : places) {
            ++base[place.side];
        }
        std::uint64_t sum = 0;
        for (std::uint64_t& slot : base) {
            const std::uint64_t n = slot;
            slot = sum;
            sum += n;
        }
        for (std::size_t i = 0; i < places.size(); ++i) {
            places[i].shares_at = run.add_offset + i;
            places[i].price_at = run.add_offset + base[places[i].side]++;
        }
    } else {
        if (auto done = add_side.all(); !done.has_value()) {
            return done;
        }
        places.reserve(add_side.size());
        std::vector<std::uint64_t> per_locate(kLocates, 0);
        for (std::uint64_t j = 0; j < records; ++j) {
            if (is_add(types[j])) {
                const std::uint64_t a = places.size();
                places.push_back({.record = j, .side = add_side[a], .shares_at = 0, .price_at = 0});
                ++per_locate[locates[j]];
            }
        }
        // Shares: by locate. Prices: by side, then stably by locate, two counting passes.
        std::vector<std::uint64_t> locate_base(kLocates, 0);
        for (std::size_t l = 1; l < kLocates; ++l) {
            locate_base[l] = locate_base[l - 1] + per_locate[l - 1];
        }
        std::vector<std::uint64_t> next = locate_base;
        for (AddPlace& place : places) {
            place.shares_at = next[locates[place.record]]++;
        }
        std::array<std::uint64_t, 257> side_base{};
        for (const AddPlace& place : places) {
            ++side_base[place.side + 1];
        }
        for (std::size_t s = 1; s < side_base.size(); ++s) {
            side_base[s] += side_base[s - 1];
        }
        std::vector<std::uint32_t> by_side(places.size());
        for (std::size_t a = 0; a < places.size(); ++a) {
            by_side[side_base[places[a].side]++] = static_cast<std::uint32_t>(a);
        }
        next = locate_base;
        for (const std::uint32_t a : by_side) {
            places[a].price_at = next[locates[places[a].record]]++;
        }
    }

    Stocks stocks;
    std::array<std::uint64_t, 6> ref_start{};
    std::array<std::uint64_t, 3> reduce_start{};
    if (need == Need::rows) {
        auto opened = open_stocks(seg);
        if (!opened.has_value()) {
            return std::unexpected(opened.error());
        }
        stocks = *opened;
        if (auto import = seg.open(Column::import_ref); !import.has_value()) {
            return import;
        }
        // ref_distance by type: C, D, E, U, X. reduce_shares: C, E, X.
        const std::array<std::size_t, 5> ref_order{kSymC, kSymD, kSymE, kSymU, kSymX};
        std::array<std::uint64_t, kSeekSymbols.size()> total{};
        for (std::size_t t = 0; t < seg.per_type.size(); ++t) {
            if (kSeekSymbolIndex[t] < total.size()) {
                total[kSeekSymbolIndex[t]] += seg.per_type[t];
            }
        }
        for (std::size_t i = 0; i < ref_order.size(); ++i) {
            ref_start[i + 1] = ref_start[i] + total[ref_order[i]];
        }
        reduce_start = {0, total[kSymC], total[kSymC] + total[kSymE]};
    }
    const auto ref_slot = [&](std::size_t symbol) -> std::uint64_t {
        if (symbol == kSymC) {
            return ref_start[0];
        }
        if (symbol == kSymD) {
            return ref_start[1];
        }
        if (symbol == kSymE) {
            return ref_start[2];
        }
        if (symbol == kSymU) {
            return ref_start[3];
        }
        return ref_start[4];
    };

    ColumnAccess& creation = engine.col(Column::creation_ref);
    ColumnAccess& distance = engine.col(Column::ref_distance);
    ColumnAccess& imports = engine.col(Column::import_ref);
    if (!locate.has_value()) {
        if (auto done = times.ensure(seg.lo, seg.hi); !done.has_value()) {
            return done;
        }
    }
    if (need == Need::rows) {
        // The pass reads every naming record's distance and every creation before hi, so
        // their blocks are decoded up front and read without a check each.
        std::array<std::uint64_t, 256> before{};
        for (std::uint64_t j = 0; j < seg.hi; ++j) {
            ++before[types[j]];
        }
        const std::array<std::pair<char, std::size_t>, 5> named{
            {{'C', 0}, {'D', 1}, {'E', 2}, {'U', 3}, {'X', 4}}};
        for (const auto& [type, slot] : named) {
            const std::uint64_t first = ref_start[slot];
            if (auto done = distance.ensure(first, first + before[static_cast<unsigned char>(type)]);
                !done.has_value()) {
                return done;
            }
        }
        if (auto done = creation.ensure(0, before['A'] + before['F'] + before['U']);
            !done.has_value()) {
            return done;
        }
    }
    engine.clear_lists();

    // One pass over [0, hi): the counters every column index needs, the locates' lists of
    // known orders, and the rows in the window.
    std::array<std::uint64_t, kSeekSymbols.size()> rank{};
    std::uint64_t add_index = 0;
    std::uint64_t created = 0;
    std::uint64_t imported = 0;
    std::size_t next_mine = 0;
    std::size_t next_add = 0;
    const std::uint64_t first_record = seg.summary->first_record;
    const auto value = [](ColumnAccess& column, std::uint64_t index,
                          std::uint64_t& out) -> std::expected<void, ItchStoreError> {
        auto got = column.get(index);
        if (!got.has_value()) {
            return std::unexpected(got.error());
        }
        out = *got;
        return {};
    };
    for (std::uint64_t j = 0; j < seg.hi; ++j) {
        const std::uint64_t type = types[j];
        const std::size_t symbol = kSeekSymbolIndex[type];
        const std::uint64_t r = rank[symbol]++;
        bool mine = true;
        if (locate.has_value()) {
            mine = next_mine < run.positions.size() && run.positions[next_mine] == j;
            next_mine += mine ? 1U : 0U;
            if (!mine && next_mine == run.positions.size()) {
                // Nothing after the locate's last record bears on it.
                break;
            }
        }
        const std::uint16_t loc = mine ? locate_at(j) : 0;
        std::optional<std::size_t> place;
        if (is_add(type)) {
            if (locate.has_value()) {
                if (mine) {
                    place = next_add++;
                }
            } else {
                place = add_index;
            }
            ++add_index;
        }
        const bool emit = mine && j >= seg.lo;
        if (need == Need::adds) {
            if (!emit || !place.has_value()) {
                continue;
            }
            std::uint64_t ts = 0;
            if (auto got = value(times, j, ts); !got.has_value()) {
                return got;
            }
            if (ts < start || ts >= end) {
                continue;
            }
            QueryRow row;
            row.seq = first_record + j;
            row.ts_ns = ts;
            row.msg_type = static_cast<std::uint16_t>(type);
            row.locate = loc;
            std::uint64_t shares = 0;
            std::uint64_t price = 0;
            if (auto got = value(add_shares, places[*place].shares_at, shares); !got.has_value()) {
                return got;
            }
            if (auto got = value(add_price, places[*place].price_at, price); !got.has_value()) {
                return got;
            }
            row.shares = static_cast<std::uint32_t>(shares);
            row.price = static_cast<std::uint32_t>(price);
            sink(row);
            continue;
        }

        // Need::rows: the order each record names or creates, kept for every locate the
        // query reads, and the imports counted over every locate.
        std::uint64_t order_ref = 0;
        const bool names = symbol == kSymC || symbol == kSymD || symbol == kSymE ||
                           symbol == kSymU || symbol == kSymX;
        if (names) {
            const std::uint64_t at = ref_slot(symbol) + r;
            const std::uint64_t d = distance[at];
            if (d == 0) {
                const std::uint64_t import = imported++;
                if (mine) {
                    if (auto got = value(imports, import, order_ref); !got.has_value()) {
                        return got;
                    }
                    engine.append(loc, order_ref);
                }
            } else if (mine) {
                const std::vector<std::uint64_t>& list = engine.lists[loc];
                if (d > list.size()) {
                    return std::unexpected(seg.error(ItchStoreErrorKind::bad_value, "ref distance",
                                                     Column::ref_distance, at, list.size(), d));
                }
                order_ref = list[list.size() - d];
            }
        }
        std::uint64_t created_ref = 0;
        if (is_add(type) || symbol == kSymU) {
            const std::uint64_t c = created++;
            if (mine) {
                created_ref = creation[c];
                engine.append(loc, created_ref);
            }
        }
        if (!emit) {
            continue;
        }
        std::uint64_t ts = 0;
        if (auto got = value(times, j, ts); !got.has_value()) {
            return got;
        }
        if (ts < start || ts >= end) {
            continue;
        }
        QueryRow row;
        row.seq = first_record + j;
        row.ts_ns = ts;
        row.msg_type = static_cast<std::uint16_t>(type);
        row.locate = loc;
        std::uint64_t shares = 0;
        std::uint64_t price = 0;
        std::uint64_t side = 0;
        const auto read = [&](Column column, std::uint64_t index,
                              std::uint64_t& out) { return value(engine.col(column), index, out); };
        std::expected<void, ItchStoreError> got{};
        if (is_add(type)) {
            row.order_ref = created_ref;
            side = places[*place].side;
            got = read(Column::add_shares, places[*place].shares_at, shares);
            if (got.has_value()) {
                got = read(Column::add_price, places[*place].price_at, price);
            }
        } else if (symbol == kSymE || symbol == kSymX) {
            row.order_ref = order_ref;
            got = read(Column::reduce_shares, reduce_start[symbol == kSymE ? 1 : 2] + r, shares);
        } else if (symbol == kSymC) {
            row.order_ref = order_ref;
            got = read(Column::reduce_shares, reduce_start[0] + r, shares);
            if (got.has_value()) {
                got = read(Column::exec_price, r, price);
            }
        } else if (symbol == kSymD) {
            row.order_ref = order_ref;
        } else if (symbol == kSymU) {
            row.order_ref = order_ref;
            got = read(Column::replace_shares, r, shares);
            if (got.has_value()) {
                got = read(Column::replace_price, r, price);
            }
        } else if (symbol == kSymP) {
            got = read(Column::trade_ref, r, row.order_ref);
            if (got.has_value()) {
                got = read(Column::trade_side, r, side);
            }
            if (got.has_value()) {
                got = read(Column::trade_shares, r, shares);
            }
            if (got.has_value()) {
                got = read(Column::trade_price, r, price);
            }
        }
        if (!got.has_value()) {
            return got;
        }
        row.side = static_cast<std::uint8_t>(side);
        row.shares = static_cast<std::uint32_t>(shares);
        row.price = static_cast<std::uint32_t>(price);
        if (is_add(type) || symbol == kSymP) {
            auto stock = stock_of(seg, stocks, j, loc);
            if (!stock.has_value()) {
                return std::unexpected(stock.error());
            }
            row.has_stock = true;
            row.stock = *stock;
        }
        sink(row);
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
            if (auto done = indexed_segment(*reader_, *engine_, k, start_ns, end_ns, locate,
                                            Need::rows, stats_, keep);
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
    std::map<std::uint64_t, MinuteBucket> minutes;
    if (start_ns >= end_ns) {
        return std::vector<MinuteBucket>{};
    }
    const auto add = [&](const QueryRow& row) {
        if (row.ts_ns < start_ns || row.ts_ns >= end_ns || row.locate != locate ||
            !is_add(row.msg_type)) {
            return;
        }
        const std::uint64_t minute = row.ts_ns / kNanosPerMinute;
        auto [slot, fresh] = minutes.try_emplace(minute);
        MinuteBucket& bucket = slot->second;
        if (fresh) {
            bucket.minute_ns = minute * kNanosPerMinute;
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
            if (auto done = indexed_segment(*reader_, *engine_, k, start_ns, end_ns, locate,
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
    std::vector<MinuteBucket> out;
    out.reserve(minutes.size());
    for (const auto& [minute, bucket] : minutes) {
        out.push_back(bucket);
    }
    stats_.rows = out.size();
    return out;
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
            auto any = seg.narrow(0, std::uint64_t{kMax48} + 1);
            if (!any.has_value()) {
                return std::unexpected(any.error());
            }
            const SeekIndex& seek = seg.seek;
            const auto total = [&](std::size_t symbol) -> std::uint64_t {
                return seek.symbol_total(symbol);
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
            auto any = seg.narrow(start_ns, end_ns);
            if (!any.has_value()) {
                return std::unexpected(any.error());
            }
            if (!*any) {
                continue;
            }
            auto locates = all_locates(seg);
            if (!locates.has_value()) {
                return std::unexpected(locates.error());
            }
            if (auto opened = seg.open(Column::timestamp); !opened.has_value()) {
                return std::unexpected(opened.error());
            }
            ColumnAccess& times = engine_->col(Column::timestamp);
            if (times.size() != summary.records) {
                return std::unexpected(seg.error(ItchStoreErrorKind::bad_segment,
                                                 "column value count", Column::timestamp,
                                                 kNoIndex, summary.records, times.size()));
            }
            if (auto done = times.ensure(seg.lo, seg.hi); !done.has_value()) {
                return std::unexpected(done.error());
            }
            for (std::uint64_t j = seg.lo; j < seg.hi; ++j) {
                if (times[j] >= start_ns && times[j] < end_ns) {
                    ++counts[(*locates)[j]];
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
