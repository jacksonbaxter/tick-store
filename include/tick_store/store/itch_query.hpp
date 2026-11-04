#pragma once

// Queries over an ITCH store (itch_store_reader.hpp) with the comparison harness's exact
// semantics (compare/queries.py, compare/README.md): every answer is what the column form
// tick_export writes would give for the same records.
//
// A row is one record as the column form holds it: the record's number in the file, its
// timestamp in nanoseconds from midnight, its type byte, locate, order reference (U's
// original), side, shares, price and stock, each 0 or empty where the record's form does
// not use it. A record the form calls raw keeps its type byte, takes the timestamp of the
// row before it (0 for the file's first) and holds nothing else. A window is half-open,
// [start, end), on that timestamp.
//
// How a segment is answered:
//
//   - The footer's summary skips a segment whose times or locates cannot match.
//   - The seek index (docs/store.md) skips each group of 4,096 records whose time zone map
//     cannot match, and gives every count a column's value index needs at a group's start.
//   - A locate's records come from its run in the locate column, so a locate filter reads
//     that run, not the whole column.
//   - Only the columns a query needs are read, and in them only the codec blocks that hold
//     values it needs: values are reached by index, a block at a time.
//   - A segment holding a record the column form calls raw, which the seek index's maxima,
//     the symbols, the stock exceptions and the MPIDs rule out for every real day, is
//     answered exactly instead: decoded whole, each record re-encoded and mapped by
//     row_values() (columns.hpp), the function tick_export itself uses.
//
// Every block read is CRC-checked unless the reader skips checksums, and every value is
// checked against its field and against the seek index's maximum for its column. What a
// partial decode cannot check, the whole decode (SegmentReader) does; a file that it
// accepts gives the same answer by either path.
//
// Results are values, copied out of the mapping. The reader must outlive the ItchQuery.

#include "tick_store/itch/message.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace tick_store::store {

struct QueryRow {
    std::uint64_t seq = 0;
    std::uint64_t ts_ns = 0;
    std::uint16_t msg_type = 0;
    std::uint32_t locate = 0;
    std::uint64_t order_ref = 0;
    std::uint8_t side = 0;
    std::uint32_t shares = 0;
    std::uint32_t price = 0;
    // The 8 bytes as sent, for A, F and P; none for any other row.
    bool has_stock = false;
    itch::Stock stock{};

    friend bool operator==(const QueryRow&, const QueryRow&) = default;
};

// One minute of add orders: the minute's start in nanoseconds from midnight, the adds,
// their shares summed, and the least and greatest price.
struct MinuteBucket {
    std::uint64_t minute_ns = 0;
    std::uint64_t count = 0;
    std::uint64_t shares = 0;
    std::uint64_t min_price = 0;
    std::uint64_t max_price = 0;

    friend bool operator==(const MinuteBucket&, const MinuteBucket&) = default;
};

struct ColumnSum {
    std::uint64_t records = 0;
    std::uint64_t sum = 0;

    friend bool operator==(const ColumnSum&, const ColumnSum&) = default;
};

// What a query read. bytes_decoded is stored bytes: each codec block decoded, header
// included, and for a zstd chunk the whole compressed chunk, which inflates as one; plus
// every block of a segment answered exactly. values_decoded counts values those blocks
// gave.
struct QueryStats {
    std::uint64_t segments = 0;
    std::uint64_t exact_segments = 0;
    std::uint64_t blocks = 0;
    std::uint64_t bytes_decoded = 0;
    std::uint64_t values_decoded = 0;
    std::uint64_t rows = 0;

    friend bool operator==(const QueryStats&, const QueryStats&) = default;
};

enum class QueryPath : std::uint8_t {
    // The seek index, the locate runs and block-level reads, falling back to the exact
    // path only for a segment that needs it.
    indexed,
    // Every segment the summaries cannot skip, decoded whole and mapped record by record.
    exact,
};

class ItchQuery {
public:
    explicit ItchQuery(const ItchStoreReader& reader, QueryPath path = QueryPath::indexed);
    ItchQuery(const ItchQuery&) = delete;
    ItchQuery& operator=(const ItchQuery&) = delete;
    ItchQuery(ItchQuery&&) noexcept;
    ItchQuery& operator=(ItchQuery&&) noexcept;
    ~ItchQuery();

    // Rows with start_ns <= timestamp < end_ns, of one locate if `locate` is set, in file
    // order.
    [[nodiscard]] std::expected<std::vector<QueryRow>, ItchStoreError> rows(
        std::uint64_t start_ns, std::uint64_t end_ns, std::optional<std::uint16_t> locate);

    // For the add orders (A and F) of `locate` in the window, per minute of their
    // timestamp, the minutes in order.
    [[nodiscard]] std::expected<std::vector<MinuteBucket>, ItchStoreError> minute_aggregate(
        std::uint64_t start_ns, std::uint64_t end_ns, std::uint16_t locate);

    // Every record of the file, and the sum of the shares column over them.
    [[nodiscard]] std::expected<ColumnSum, ItchStoreError> shares_sum();

    // Rows per locate in the window, indexed by locate, 65,536 long.
    [[nodiscard]] std::expected<std::vector<std::uint64_t>, ItchStoreError> locate_counts(
        std::uint64_t start_ns, std::uint64_t end_ns);

    // What the last query read.
    [[nodiscard]] const QueryStats& stats() const noexcept { return stats_; }

    // The decoding state reused from query to query; defined with the implementation.
    struct Engine;

private:
    const ItchStoreReader* reader_;
    QueryPath path_;
    QueryStats stats_;
    std::unique_ptr<Engine> engine_;
};

}  // namespace tick_store::store
