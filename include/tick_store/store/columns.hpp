#pragma once

// The lossless column form every comparison system stores, as plain little-endian
// files: one table, one row per record. tools/tick_export writes it from an ITCH file.
// The comparison harness writes the same form back from what a system returned, and
// ColumnsReader rebuilds the source from it, so the correctness oracle judges each
// system's stored form with no knowledge of how the system works. compare/README.md
// gives the field mapping and the reasons for it.
//
// A directory holds, for N records:
//
//   manifest.txt      the only description of sizes, written last
//   <name>.bin        one fixed-width column of N signed values
//   <name>.off        one text column's byte offsets, N + 1 unsigned 8-byte values
//   <name>.dat        that column's bytes
//   tail.bin          the source's bytes after its last frame, verbatim
//
// A row takes one of three forms, and every row has exactly one:
//
//   hot    A F E C X D U P: every field is in a typed column and `rest` is empty.
//   rare   any other conforming type: `rest` is the body after the 11-byte header,
//          as lowercase hex.
//   raw    `rest` is "raw:" and the whole payload, type byte first, as lowercase hex.
//          Used for anything the other two cannot hold exactly: a payload the decoder
//          calls raw, a hot field outside its column's range, or a stock or MPID that
//          is not printable ASCII. `seq` and `msg_type` stay set, and `ts_ns` repeats the
//          previous row's, or is 0 on the first row, so the column stays sorted. All else
//          is empty.
//
// Columns a row's form does not use must hold 0 or be empty. That is checked, since a
// stored value the rebuild would ignore is still a stored value that came back wrong.
//
// Manifest, exact bytes, every number in decimal with no sign or leading zero:
//
//   tick-columns 1
//   records <N>
//   tail_bytes <bytes of tail.bin>
//   text_bytes stock <bytes of stock.dat>
//   text_bytes mpid <bytes of mpid.dat>
//   text_bytes rest <bytes of rest.dat>
//
// Over-read contract: the parser reads only inside the spans it was given and needs no
// padding after them.
//
// Lifetime: ColumnsParser borrows its spans. A frame it returns is valid until the next
// call to next().

#include "tick_store/itch/message.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/store_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::store {

inline constexpr std::string_view kColumnsMagic = "tick-columns";
inline constexpr std::uint32_t kColumnsVersion = 1;
inline constexpr std::string_view kColumnsManifestName = "manifest.txt";
// Marks a raw row's `rest`. ':' is not a hex digit, so no rare row can begin with it.
inline constexpr std::string_view kRawRestPrefix = "raw:";
// The bytes every conforming message starts with: type, locate, tracking, timestamp.
inline constexpr std::size_t kMessageHeaderSize = 11;

// Every file of a column directory but the manifest, in the order of kColumnFileNames.
enum class ColumnFile : std::uint8_t {
    seq,
    ts_ns,
    msg_type,
    locate,
    tracking,
    order_ref,
    new_ref,
    side,
    shares,
    price,
    match,
    printable,
    stock_off,
    stock_dat,
    mpid_off,
    mpid_dat,
    rest_off,
    rest_dat,
    tail,
};

inline constexpr std::size_t kColumnFileCount = 19;
inline constexpr std::size_t kFixedColumnCount = 12;
inline constexpr std::size_t kTextColumnCount = 3;

inline constexpr std::array<std::string_view, kColumnFileCount> kColumnFileNames{
    "seq.bin",       "ts_ns.bin", "msg_type.bin", "locate.bin", "tracking.bin",
    "order_ref.bin", "new_ref.bin", "side.bin",   "shares.bin", "price.bin",
    "match.bin",     "printable.bin", "stock.off", "stock.dat", "mpid.off",
    "mpid.dat",      "rest.off",  "rest.dat",     "tail.bin",
};

// Value width of the fixed columns, in file order.
inline constexpr std::array<std::size_t, kFixedColumnCount> kFixedColumnWidths{
    8, 8, 2, 4, 4, 8, 8, 1, 4, 4, 8, 1};

using ColumnBytes = std::array<std::span<const std::byte>, kColumnFileCount>;
using ColumnBuffers = std::array<std::vector<std::byte>, kColumnFileCount>;

struct ColumnsManifest {
    std::uint64_t records = 0;
    std::uint64_t tail_bytes = 0;
    std::array<std::uint64_t, kTextColumnCount> text_bytes{};

    friend bool operator==(const ColumnsManifest&, const ColumnsManifest&) = default;
};

// Parses a manifest, refusing anything but the exact form above.
[[nodiscard]] std::expected<ColumnsManifest, StoreError> parse_columns_manifest(
    std::span<const std::byte> bytes);

[[nodiscard]] std::string format_columns_manifest(const ColumnsManifest& manifest);

// How one payload becomes a row, the rest text aside: its form and, for a hot or rare row,
// the header and the typed values exactly as the files hold them. A raw row repeats the
// previous row's timestamp, which only the caller knows, so it is 0 here, and every value
// but the type byte is 0. `stock` and `mpid` point into the payload. ColumnsEncoder writes
// what this returns, and a query that must answer as the files would uses it too.
enum class RowForm : std::uint8_t { hot, rare, raw };

struct RowValues {
    RowForm form = RowForm::raw;
    std::uint64_t ts_ns = 0;
    std::uint16_t msg_type = 0;
    std::uint32_t locate = 0;
    std::uint32_t tracking = 0;
    std::uint64_t order_ref = 0;
    std::uint64_t new_ref = 0;
    std::uint64_t shares = 0;
    std::uint64_t price = 0;
    std::uint64_t match = 0;
    std::uint8_t side = 0;
    std::uint8_t printable = 0;
    std::span<const std::byte> stock;
    std::span<const std::byte> mpid;
};

// nullopt only for an empty payload, which no frame can hold.
[[nodiscard]] std::optional<RowValues> row_values(std::span<const std::byte> payload);

// True if the column form keeps `message` as a hot or a rare row, false if it keeps it raw:
// a RawMessage, or a hot message with a value outside its signed column or a stock or MPID
// that is not printable ASCII. row_values() decides the same for a payload.
[[nodiscard]] bool column_form_plain(const itch::Message& message) noexcept;

// What ColumnsEncoder has seen. The largest value of each range-limited column is
// here so that the harness can show the types chosen hold the real data.
struct ColumnsSummary {
    std::uint64_t records = 0;
    std::uint64_t hot_rows = 0;
    std::uint64_t rare_rows = 0;
    std::uint64_t raw_rows = 0;
    std::uint64_t text_rest_bytes = 0;
    std::uint64_t tail_bytes = 0;
    // True if no row's timestamp is below the previous row's.
    bool timestamps_sorted = true;
    std::uint64_t max_ts_ns = 0;
    std::uint64_t max_locate = 0;
    std::uint64_t max_tracking = 0;
    std::uint64_t max_order_ref = 0;
    std::uint64_t max_shares = 0;
    std::uint64_t max_price = 0;
    std::uint64_t max_match = 0;
};

// Turns payloads into rows. Bytes accumulate in buffers() until the caller takes them,
// which is all ColumnsWriter adds, so tests and fuzz targets run this with no files.
class ColumnsEncoder {
public:
    ColumnsEncoder();

    // Refuses only an empty payload, which no frame can hold.
    [[nodiscard]] std::expected<void, StoreError> append(std::span<const std::byte> payload);

    [[nodiscard]] ColumnBuffers& buffers() noexcept { return buffers_; }
    [[nodiscard]] const ColumnsSummary& summary() const noexcept { return summary_; }
    [[nodiscard]] ColumnsManifest manifest(std::uint64_t tail_bytes) const noexcept;

private:
    void append_text(ColumnFile offsets, ColumnFile bytes, std::span<const std::byte> value,
                     std::uint64_t& total);

    ColumnBuffers buffers_;
    ColumnsSummary summary_;
    std::array<std::uint64_t, kTextColumnCount> text_totals_{};
    std::uint64_t previous_ts_ns_ = 0;
};

// Writes a column directory. `directory` must not hold a manifest. The manifest goes
// last, so a directory without one is incomplete and every reader refuses it. These
// files can be regenerated from the source, so nothing here is flushed to the device.
// Every file's bytes go through `ops`'s write, by write_all, which must outlive the writer.
class ColumnsWriter {
public:
    [[nodiscard]] static std::expected<ColumnsWriter, StoreError> create(
        const std::filesystem::path& directory, FileOps& ops = posix_file_ops());

    ColumnsWriter(const ColumnsWriter&) = delete;
    ColumnsWriter& operator=(const ColumnsWriter&) = delete;
    ColumnsWriter(ColumnsWriter&& other) noexcept;
    ColumnsWriter& operator=(ColumnsWriter&& other) = delete;
    ~ColumnsWriter();

    [[nodiscard]] std::expected<void, StoreError> append(std::span<const std::byte> payload);

    // Stores `tail` and publishes the directory by writing the manifest.
    [[nodiscard]] std::expected<ColumnsSummary, StoreError> finish(std::span<const std::byte> tail);

private:
    ColumnsWriter(std::filesystem::path directory, std::array<int, kColumnFileCount> fds,
                  FileOps& ops);

    [[nodiscard]] std::expected<void, StoreError> drain(std::size_t threshold);
    void close_all() noexcept;

    std::filesystem::path directory_;
    std::array<int, kColumnFileCount> fds_{};
    ColumnsEncoder encoder_;
    FileOps* ops_;
};

// Rebuilds frames from columns held in memory.
class ColumnsParser {
public:
    // Proves every file's size against the manifest, and the ends of each offsets
    // file. A row's own content is checked as next() reaches it.
    [[nodiscard]] static std::expected<ColumnsParser, StoreError> open(
        const ColumnsManifest& manifest, const ColumnBytes& files);

    // The next record as a whole frame, or nullopt after the last. Once it has
    // reported an error or the end, it keeps reporting the same thing.
    [[nodiscard]] std::expected<std::optional<std::span<const std::byte>>, StoreError> next();

    [[nodiscard]] std::span<const std::byte> tail() const noexcept;
    [[nodiscard]] const ColumnsManifest& manifest() const noexcept { return manifest_; }

private:
    ColumnsParser(const ColumnsManifest& manifest, const ColumnBytes& files);

    [[nodiscard]] std::expected<std::span<const std::byte>, StoreError> rebuild_row();

    ColumnsManifest manifest_;
    ColumnBytes files_;
    std::uint64_t row_ = 0;
    std::uint64_t previous_ts_ns_ = 0;
    std::optional<StoreError> failed_;
    std::vector<std::byte> frame_;
    std::vector<std::byte> payload_;
};

}  // namespace tick_store::store
