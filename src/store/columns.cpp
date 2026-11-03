#include "tick_store/store/columns.hpp"

#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::store {
namespace {

using itch::AddOrder;
using itch::AddOrderWithMpid;
using itch::Message;
using itch::MessageHeader;
using itch::OrderCancel;
using itch::OrderDelete;
using itch::OrderExecuted;
using itch::OrderExecutedWithPrice;
using itch::OrderReplace;
using itch::Price4;
using itch::Stock;
using itch::Trade;

constexpr std::size_t kLargestFrame = itch::kFramePrefixSize + itch::kMaxPayloadSize;
constexpr std::size_t kStockSize = 8;
constexpr std::size_t kMpidSize = 4;
// Each writer buffer is drained once it reaches this.
constexpr std::size_t kDrainThreshold = std::size_t{1} << 18U;

constexpr std::size_t index_of(ColumnFile file) noexcept { return static_cast<std::size_t>(file); }

constexpr std::size_t width_of(ColumnFile file) noexcept {
    return kFixedColumnWidths[index_of(file)];
}

constexpr std::string_view name_of(ColumnFile file) noexcept {
    return kColumnFileNames[index_of(file)];
}

[[gnu::cold, gnu::noinline]] StoreError bad(StoreErrorKind kind, std::string_view field,
                                            std::uint64_t offset, std::uint64_t expected,
                                            std::uint64_t actual) noexcept {
    return {.kind = kind,
            .field = field,
            .offset = offset,
            .expected = expected,
            .actual = actual};
}

[[gnu::cold, gnu::noinline]] StoreError io_error(std::string_view call) noexcept {
    return {.kind = StoreErrorKind::io,
            .field = call,
            .actual = static_cast<std::uint64_t>(errno)};
}

// Which typed columns a hot row uses. Everything else must be 0 or empty.
constexpr unsigned kUsesOrderRef = 1U << 0U;
constexpr unsigned kUsesNewRef = 1U << 1U;
constexpr unsigned kUsesSide = 1U << 2U;
constexpr unsigned kUsesShares = 1U << 3U;
constexpr unsigned kUsesPrice = 1U << 4U;
constexpr unsigned kUsesMatch = 1U << 5U;
constexpr unsigned kUsesPrintable = 1U << 6U;
constexpr unsigned kUsesStock = 1U << 7U;
constexpr unsigned kUsesMpid = 1U << 8U;

constexpr std::uint16_t kAddColumns =
    kUsesOrderRef | kUsesSide | kUsesShares | kUsesStock | kUsesPrice;

// The columns of a hot type, or nullopt for any other byte.
[[nodiscard]] constexpr std::optional<std::uint16_t> hot_columns(std::byte type) noexcept {
    if (type == AddOrder::type_code) {
        return kAddColumns;
    }
    if (type == AddOrderWithMpid::type_code) {
        return std::uint16_t{kAddColumns | kUsesMpid};
    }
    if (type == OrderExecuted::type_code) {
        return std::uint16_t{kUsesOrderRef | kUsesShares | kUsesMatch};
    }
    if (type == OrderExecutedWithPrice::type_code) {
        return std::uint16_t{kUsesOrderRef | kUsesShares | kUsesMatch | kUsesPrintable | kUsesPrice};
    }
    if (type == OrderCancel::type_code) {
        return std::uint16_t{kUsesOrderRef | kUsesShares};
    }
    if (type == OrderDelete::type_code) {
        return std::uint16_t{kUsesOrderRef};
    }
    if (type == OrderReplace::type_code) {
        return std::uint16_t{kUsesOrderRef | kUsesNewRef | kUsesShares | kUsesPrice};
    }
    if (type == Trade::type_code) {
        return std::uint16_t{kAddColumns | kUsesMatch};
    }
    return std::nullopt;
}

// The typed values of one row. Unsigned wire values are held as they are, and the
// range check on the way in decides whether they fit their signed column.
struct HotFields {
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

[[nodiscard]] std::uint64_t price_of(Price4 price) noexcept {
    return static_cast<std::uint64_t>(price);
}

[[nodiscard]] std::uint8_t byte_of(std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

// The header and typed fields of a hot message, or nullopt for any other. The returned
// spans point into `message`.
[[nodiscard]] std::optional<std::pair<MessageHeader, HotFields>> hot_fields(
    const Message& message) noexcept {
    HotFields fields;
    if (const auto* add = std::get_if<AddOrder>(&message)) {
        fields.order_ref = add->order_reference_number;
        fields.shares = add->shares;
        fields.price = price_of(add->price);
        fields.side = byte_of(add->buy_sell_indicator);
        fields.stock = add->stock;
        return std::pair{add->header, fields};
    }
    if (const auto* add = std::get_if<AddOrderWithMpid>(&message)) {
        fields.order_ref = add->order_reference_number;
        fields.shares = add->shares;
        fields.price = price_of(add->price);
        fields.side = byte_of(add->buy_sell_indicator);
        fields.stock = add->stock;
        fields.mpid = add->attribution;
        return std::pair{add->header, fields};
    }
    if (const auto* executed = std::get_if<OrderExecuted>(&message)) {
        fields.order_ref = executed->order_reference_number;
        fields.shares = executed->executed_shares;
        fields.match = executed->match_number;
        return std::pair{executed->header, fields};
    }
    if (const auto* executed = std::get_if<OrderExecutedWithPrice>(&message)) {
        fields.order_ref = executed->order_reference_number;
        fields.shares = executed->executed_shares;
        fields.price = price_of(executed->execution_price);
        fields.match = executed->match_number;
        fields.printable = byte_of(executed->printable);
        return std::pair{executed->header, fields};
    }
    if (const auto* cancel = std::get_if<OrderCancel>(&message)) {
        fields.order_ref = cancel->order_reference_number;
        fields.shares = cancel->cancelled_shares;
        return std::pair{cancel->header, fields};
    }
    if (const auto* deleted = std::get_if<OrderDelete>(&message)) {
        fields.order_ref = deleted->order_reference_number;
        return std::pair{deleted->header, fields};
    }
    if (const auto* replace = std::get_if<OrderReplace>(&message)) {
        fields.order_ref = replace->original_order_reference_number;
        fields.new_ref = replace->new_order_reference_number;
        fields.shares = replace->shares;
        fields.price = price_of(replace->price);
        return std::pair{replace->header, fields};
    }
    if (const auto* trade = std::get_if<Trade>(&message)) {
        fields.order_ref = trade->order_reference_number;
        fields.shares = trade->shares;
        fields.price = price_of(trade->price);
        fields.match = trade->match_number;
        fields.side = byte_of(trade->buy_sell_indicator);
        fields.stock = trade->stock;
        return std::pair{trade->header, fields};
    }
    return std::nullopt;
}

[[nodiscard]] bool printable_ascii(std::span<const std::byte> text) noexcept {
    return std::ranges::all_of(text, [](std::byte value) {
        return value >= std::byte{0x20} && value <= std::byte{0x7E};
    });
}

// True if every value fits its signed column with its exact value, which leaves
// QuestDB's null sentinels, the minimum of each width, unreachable.
[[nodiscard]] bool fits_columns(const HotFields& fields) noexcept {
    constexpr auto kInt64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr auto kInt32Max = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    constexpr std::uint8_t kInt8Max = 127;
    return fields.order_ref <= kInt64Max && fields.new_ref <= kInt64Max &&
           fields.match <= kInt64Max && fields.shares <= kInt32Max && fields.price <= kInt32Max &&
           fields.side <= kInt8Max && fields.printable <= kInt8Max &&
           printable_ascii(fields.stock) && printable_ascii(fields.mpid);
}

template <std::size_t Width>
void put(std::vector<std::byte>& out, std::uint64_t value) {
    const std::size_t at = out.size();
    out.resize(at + Width);
    // The space was made just above, so the checked write cannot fail.
    std::ignore = write_le<Width>(out, at, value);
}

constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void append_hex(std::string& out, std::span<const std::byte> bytes) {
    for (const std::byte value : bytes) {
        const auto high = std::to_integer<std::size_t>(value >> 4U);
        const auto low = std::to_integer<std::size_t>(value & std::byte{0x0F});
        out.push_back(kHexDigits[high]);
        out.push_back(kHexDigits[low]);
    }
}

// Lowercase only: a digit has one spelling, so a flipped case bit cannot go unseen.
[[nodiscard]] constexpr std::optional<std::uint8_t> hex_value(std::byte digit) noexcept {
    const std::uint8_t value = byte_of(digit);
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return std::nullopt;
}

// Decodes `hex` to the end of `out`. Returns the offset of the first bad digit if any.
[[nodiscard]] std::optional<std::size_t> append_unhex(std::vector<std::byte>& out,
                                                      std::span<const std::byte> hex) {
    out.reserve(out.size() + (hex.size() / 2));
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto high = hex_value(hex[i]);
        if (!high.has_value()) {
            return i;
        }
        const auto low = hex_value(hex[i + 1]);
        if (!low.has_value()) {
            return i + 1;
        }
        out.push_back(static_cast<std::byte>((static_cast<unsigned>(*high) << 4U) | *low));
    }
    return std::nullopt;
}

[[nodiscard]] std::span<const std::byte> text_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span{text});
}

// Reads a fixed column's value at `row`, sign-extended. Sizes were proven at open.
template <ColumnFile File>
[[nodiscard]] std::int64_t fixed_value(const ColumnBytes& files, std::uint64_t row) noexcept {
    constexpr std::size_t kWidth = width_of(File);
    const auto raw = read_le<kWidth>(files[index_of(File)], row * kWidth).value_or(0);
    if constexpr (kWidth == 8) {
        return std::bit_cast<std::int64_t>(raw);
    } else {
        const std::uint64_t sign_bit = std::uint64_t{1} << ((kWidth * 8U) - 1U);
        return std::bit_cast<std::int64_t>((raw ^ sign_bit) - sign_bit);
    }
}

[[nodiscard]] std::uint64_t offset_at(const ColumnBytes& files, ColumnFile offsets,
                                      std::uint64_t row) noexcept {
    return read_le<8>(files[index_of(offsets)], row * 8).value_or(0);
}

// The value of a text column at `row`, proven to lie inside its bytes.
[[nodiscard]] std::expected<std::span<const std::byte>, StoreError> text_value(
    const ColumnBytes& files, ColumnFile offsets, ColumnFile bytes, std::uint64_t row) noexcept {
    const std::uint64_t start = offset_at(files, offsets, row);
    const std::uint64_t end = offset_at(files, offsets, row + 1);
    const std::span<const std::byte> all = files[index_of(bytes)];
    if (end < start) {
        return std::unexpected(
            bad(StoreErrorKind::bad_text_offsets, name_of(offsets), row, start, end));
    }
    if (end > all.size()) {
        return std::unexpected(
            bad(StoreErrorKind::bad_text_offsets, name_of(offsets), row, all.size(), end));
    }
    return all.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
}

// A column value that must lie in [0, maximum] for the row to rebuild.
template <ColumnFile File>
[[nodiscard]] std::expected<std::uint64_t, StoreError> bounded(const ColumnBytes& files,
                                                               std::uint64_t row,
                                                               std::uint64_t maximum) noexcept {
    const std::int64_t value = fixed_value<File>(files, row);
    if (value < 0 || std::bit_cast<std::uint64_t>(value) > maximum) {
        return std::unexpected(bad(StoreErrorKind::bad_row, name_of(File), row, maximum,
                                   std::bit_cast<std::uint64_t>(value)));
    }
    return std::bit_cast<std::uint64_t>(value);
}

// A column a row's form leaves unused must be exactly 0.
template <ColumnFile File>
[[nodiscard]] std::expected<void, StoreError> unused(const ColumnBytes& files,
                                                     std::uint64_t row) noexcept {
    const std::int64_t value = fixed_value<File>(files, row);
    if (value != 0) {
        return std::unexpected(bad(StoreErrorKind::bad_row, name_of(File), row, 0,
                                   std::bit_cast<std::uint64_t>(value)));
    }
    return {};
}

// A value of a hot column if `columns` includes it, else the proof that it is 0.
template <ColumnFile File>
[[nodiscard]] std::expected<std::uint64_t, StoreError> hot_value(const ColumnBytes& files,
                                                                 std::uint64_t row,
                                                                 std::uint16_t columns,
                                                                 std::uint16_t bit) noexcept {
    if ((columns & bit) != 0U) {
        constexpr std::uint64_t kMaximum =
            (std::uint64_t{1} << ((width_of(File) * 8U) - 1U)) - 1U;
        return bounded<File>(files, row, kMaximum);
    }
    if (auto checked = unused<File>(files, row); !checked.has_value()) {
        return std::unexpected(checked.error());
    }
    return std::uint64_t{0};
}

template <std::size_t N>
void copy_field(std::array<std::byte, N>& out, std::span<const std::byte> value) noexcept {
    std::ranges::copy(value.first(N), out.begin());
}

struct RebuiltFields {
    HotFields fields;
    Stock stock{};
    itch::Mpid mpid{};
};

// A text column of a hot row: `size` bytes if the type uses it, otherwise empty.
[[nodiscard]] std::expected<std::span<const std::byte>, StoreError> hot_text(
    const ColumnBytes& files, ColumnFile offsets, ColumnFile bytes, std::uint64_t row, bool used,
    std::size_t size) noexcept {
    const auto value = text_value(files, offsets, bytes, row);
    if (!value.has_value()) {
        return value;
    }
    const std::size_t wanted = used ? size : 0;
    if (value->size() != wanted) {
        return std::unexpected(
            bad(StoreErrorKind::bad_row, name_of(bytes), row, wanted, value->size()));
    }
    return value;
}

[[nodiscard]] std::expected<RebuiltFields, StoreError> rebuild_fields(
    const ColumnBytes& files, std::uint64_t row, std::uint16_t columns) noexcept {
    RebuiltFields out;
    HotFields& fields = out.fields;
    const auto order_ref = hot_value<ColumnFile::order_ref>(files, row, columns, kUsesOrderRef);
    const auto new_ref = hot_value<ColumnFile::new_ref>(files, row, columns, kUsesNewRef);
    const auto side = hot_value<ColumnFile::side>(files, row, columns, kUsesSide);
    const auto shares = hot_value<ColumnFile::shares>(files, row, columns, kUsesShares);
    const auto price = hot_value<ColumnFile::price>(files, row, columns, kUsesPrice);
    const auto match = hot_value<ColumnFile::match>(files, row, columns, kUsesMatch);
    const auto printable = hot_value<ColumnFile::printable>(files, row, columns, kUsesPrintable);
    for (const auto* value : {&order_ref, &new_ref, &side, &shares, &price, &match, &printable}) {
        if (!value->has_value()) {
            return std::unexpected(value->error());
        }
    }
    fields.order_ref = *order_ref;
    fields.new_ref = *new_ref;
    fields.side = static_cast<std::uint8_t>(*side);
    fields.shares = *shares;
    fields.price = *price;
    fields.match = *match;
    fields.printable = static_cast<std::uint8_t>(*printable);

    const auto stock = hot_text(files, ColumnFile::stock_off, ColumnFile::stock_dat, row,
                                (columns & kUsesStock) != 0U, kStockSize);
    if (!stock.has_value()) {
        return std::unexpected(stock.error());
    }
    if (!stock->empty()) {
        copy_field(out.stock, *stock);
    }
    const auto mpid = hot_text(files, ColumnFile::mpid_off, ColumnFile::mpid_dat, row,
                               (columns & kUsesMpid) != 0U, kMpidSize);
    if (!mpid.has_value()) {
        return std::unexpected(mpid.error());
    }
    if (!mpid->empty()) {
        copy_field(out.mpid, *mpid);
    }
    return out;
}

[[nodiscard]] Message hot_message(std::byte type, const MessageHeader& header,
                                  const RebuiltFields& rebuilt) noexcept {
    const HotFields& f = rebuilt.fields;
    const auto price = static_cast<Price4>(static_cast<std::uint32_t>(f.price));
    const auto shares = static_cast<std::uint32_t>(f.shares);
    const auto side = static_cast<std::byte>(f.side);
    if (type == AddOrder::type_code) {
        return AddOrder{.header = header,
                        .order_reference_number = f.order_ref,
                        .buy_sell_indicator = side,
                        .shares = shares,
                        .stock = rebuilt.stock,
                        .price = price};
    }
    if (type == AddOrderWithMpid::type_code) {
        return AddOrderWithMpid{.header = header,
                                .order_reference_number = f.order_ref,
                                .buy_sell_indicator = side,
                                .shares = shares,
                                .stock = rebuilt.stock,
                                .price = price,
                                .attribution = rebuilt.mpid};
    }
    if (type == OrderExecuted::type_code) {
        return OrderExecuted{.header = header,
                             .order_reference_number = f.order_ref,
                             .executed_shares = shares,
                             .match_number = f.match};
    }
    if (type == OrderExecutedWithPrice::type_code) {
        return OrderExecutedWithPrice{.header = header,
                                      .order_reference_number = f.order_ref,
                                      .executed_shares = shares,
                                      .match_number = f.match,
                                      .printable = static_cast<std::byte>(f.printable),
                                      .execution_price = price};
    }
    if (type == OrderCancel::type_code) {
        return OrderCancel{.header = header,
                           .order_reference_number = f.order_ref,
                           .cancelled_shares = shares};
    }
    if (type == OrderDelete::type_code) {
        return OrderDelete{.header = header, .order_reference_number = f.order_ref};
    }
    if (type == OrderReplace::type_code) {
        return OrderReplace{.header = header,
                            .original_order_reference_number = f.order_ref,
                            .new_order_reference_number = f.new_ref,
                            .shares = shares,
                            .price = price};
    }
    return Trade{.header = header,
                 .order_reference_number = f.order_ref,
                 .buy_sell_indicator = side,
                 .shares = shares,
                 .stock = rebuilt.stock,
                 .price = price,
                 .match_number = f.match};
}

// A raw row's columns other than `seq`, `ts_ns`, `msg_type` and `rest` must be empty.
[[nodiscard]] std::expected<void, StoreError> check_raw_row_empty(const ColumnBytes& files,
                                                                  std::uint64_t row) noexcept {
    const auto fields = rebuild_fields(files, row, 0);
    if (!fields.has_value()) {
        return std::unexpected(fields.error());
    }
    if (auto locate = unused<ColumnFile::locate>(files, row); !locate.has_value()) {
        return locate;
    }
    return unused<ColumnFile::tracking>(files, row);
}

void write_frame_prefix(std::vector<std::byte>& frame, std::size_t payload_size) noexcept {
    frame[0] = static_cast<std::byte>((payload_size >> 8U) & 0xFFU);
    frame[1] = static_cast<std::byte>(payload_size & 0xFFU);
}

// One manifest line's number: the text up to `stop` at `*at`, which moves past it.
[[nodiscard]] std::optional<std::uint64_t> take_number(std::string_view text, std::size_t& at) {
    const std::size_t stop = text.find('\n', at);
    if (stop == std::string_view::npos || stop == at) {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(at, stop - at);
    if (digits.size() > 1 && digits.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* const start = digits.data();
    const char* const end = start + digits.size();
    const auto [stopped, status] = std::from_chars(start, end, value);
    if (status != std::errc{} || stopped != end) {
        return std::nullopt;
    }
    at = stop + 1;
    return value;
}

[[nodiscard]] bool take_label(std::string_view text, std::size_t& at, std::string_view label) {
    if (text.substr(at, label.size()) != label) {
        return false;
    }
    at += label.size();
    return true;
}

// A file's exact size, or the error naming it.
[[nodiscard]] std::expected<void, StoreError> require_size(const ColumnBytes& files,
                                                           std::size_t index,
                                                           std::uint64_t size) noexcept {
    const std::uint64_t actual = files[index].size();
    if (actual != size) {
        return std::unexpected(
            bad(StoreErrorKind::bad_column_size, kColumnFileNames[index], 0, size, actual));
    }
    return {};
}

// The first offset must be 0 and the last must be the end of the bytes. The walk
// between them is checked row by row.
[[nodiscard]] std::expected<void, StoreError> check_text_ends(const ColumnBytes& files,
                                                              ColumnFile offsets,
                                                              ColumnFile bytes,
                                                              std::uint64_t records) noexcept {
    const std::uint64_t first = offset_at(files, offsets, 0);
    if (first != 0) {
        return std::unexpected(bad(StoreErrorKind::bad_text_offsets, name_of(offsets), 0, 0, first));
    }
    const std::uint64_t last = offset_at(files, offsets, records);
    const std::uint64_t size = files[index_of(bytes)].size();
    if (last != size) {
        return std::unexpected(
            bad(StoreErrorKind::bad_text_offsets, name_of(offsets), records, size, last));
    }
    return {};
}

}  // namespace

std::expected<ColumnsManifest, StoreError> parse_columns_manifest(
    std::span<const std::byte> bytes) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): viewing bytes as chars is allowed.
    const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    std::size_t at = 0;
    const auto fail = [&](std::string_view line) {
        return std::unexpected(bad(StoreErrorKind::bad_manifest, line, at, 0, text.size()));
    };

    const std::string first = std::format("{} {}\n", kColumnsMagic, kColumnsVersion);
    if (!take_label(text, at, first)) {
        return fail("magic and version");
    }

    ColumnsManifest manifest;
    const auto field = [&](std::string_view label, std::uint64_t& value) {
        if (!take_label(text, at, label)) {
            return false;
        }
        const auto number = take_number(text, at);
        if (!number.has_value()) {
            return false;
        }
        value = *number;
        return true;
    };

    if (!field("records ", manifest.records)) {
        return fail("records");
    }
    if (!field("tail_bytes ", manifest.tail_bytes)) {
        return fail("tail_bytes");
    }
    if (!field("text_bytes stock ", manifest.text_bytes[0])) {
        return fail("text_bytes stock");
    }
    if (!field("text_bytes mpid ", manifest.text_bytes[1])) {
        return fail("text_bytes mpid");
    }
    if (!field("text_bytes rest ", manifest.text_bytes[2])) {
        return fail("text_bytes rest");
    }
    if (at != text.size()) {
        return fail("end of manifest");
    }
    return manifest;
}

std::string format_columns_manifest(const ColumnsManifest& manifest) {
    return std::format(
        "{} {}\nrecords {}\ntail_bytes {}\ntext_bytes stock {}\ntext_bytes mpid {}\n"
        "text_bytes rest {}\n",
        kColumnsMagic, kColumnsVersion, manifest.records, manifest.tail_bytes,
        manifest.text_bytes[0], manifest.text_bytes[1], manifest.text_bytes[2]);
}

ColumnsEncoder::ColumnsEncoder() {
    // Every offsets file starts with the 0 that the first row's value begins at.
    put<8>(buffers_[index_of(ColumnFile::stock_off)], 0);
    put<8>(buffers_[index_of(ColumnFile::mpid_off)], 0);
    put<8>(buffers_[index_of(ColumnFile::rest_off)], 0);
}

void ColumnsEncoder::append_text(ColumnFile offsets, ColumnFile bytes,
                                 std::span<const std::byte> value, std::uint64_t& total) {
    std::vector<std::byte>& out = buffers_[index_of(bytes)];
    out.insert(out.end(), value.begin(), value.end());
    total += value.size();
    put<8>(buffers_[index_of(offsets)], total);
}

std::expected<void, StoreError> ColumnsEncoder::append(std::span<const std::byte> payload) {
    const auto message = itch::decode(payload);
    if (!message.has_value()) {
        return std::unexpected(bad(StoreErrorKind::unencodable_record, "empty payload",
                                   summary_.records, 1, 0));
    }

    const auto hot = hot_fields(*message);
    const auto* raw = std::get_if<itch::RawMessage>(&*message);
    const bool keep_raw = raw != nullptr || (hot.has_value() && !fits_columns(hot->second));

    MessageHeader header{};
    HotFields fields;
    std::string rest;
    if (keep_raw) {
        rest.reserve(kRawRestPrefix.size() + (payload.size() * 2));
        rest += kRawRestPrefix;
        append_hex(rest, payload);
        header.timestamp_ns = previous_ts_ns_;
        ++summary_.raw_rows;
    } else if (hot.has_value()) {
        header = hot->first;
        fields = hot->second;
        ++summary_.hot_rows;
    } else {
        // A conforming rare type: the header is its first 11 bytes, by the wire layout.
        const auto rebuilt_header = std::visit(
            [](const auto& alternative) -> MessageHeader {
                if constexpr (requires { alternative.header; }) {
                    return alternative.header;
                } else {
                    return {};
                }
            },
            *message);
        header = rebuilt_header;
        rest.reserve((payload.size() - kMessageHeaderSize) * 2);
        append_hex(rest, payload.subspan(kMessageHeaderSize));
        ++summary_.rare_rows;
    }

    if (header.timestamp_ns < previous_ts_ns_) {
        summary_.timestamps_sorted = false;
    }
    previous_ts_ns_ = header.timestamp_ns;

    put<8>(buffers_[index_of(ColumnFile::seq)], summary_.records);
    put<8>(buffers_[index_of(ColumnFile::ts_ns)], header.timestamp_ns);
    put<2>(buffers_[index_of(ColumnFile::msg_type)], byte_of(payload.front()));
    put<4>(buffers_[index_of(ColumnFile::locate)], header.stock_locate);
    put<4>(buffers_[index_of(ColumnFile::tracking)], header.tracking_number);
    put<8>(buffers_[index_of(ColumnFile::order_ref)], fields.order_ref);
    put<8>(buffers_[index_of(ColumnFile::new_ref)], fields.new_ref);
    put<1>(buffers_[index_of(ColumnFile::side)], fields.side);
    put<4>(buffers_[index_of(ColumnFile::shares)], fields.shares);
    put<4>(buffers_[index_of(ColumnFile::price)], fields.price);
    put<8>(buffers_[index_of(ColumnFile::match)], fields.match);
    put<1>(buffers_[index_of(ColumnFile::printable)], fields.printable);
    append_text(ColumnFile::stock_off, ColumnFile::stock_dat, fields.stock, text_totals_[0]);
    append_text(ColumnFile::mpid_off, ColumnFile::mpid_dat, fields.mpid, text_totals_[1]);
    append_text(ColumnFile::rest_off, ColumnFile::rest_dat, text_bytes(rest), text_totals_[2]);

    ++summary_.records;
    summary_.text_rest_bytes = text_totals_[2];
    summary_.max_ts_ns = std::max(summary_.max_ts_ns, header.timestamp_ns);
    summary_.max_locate = std::max<std::uint64_t>(summary_.max_locate, header.stock_locate);
    summary_.max_tracking = std::max<std::uint64_t>(summary_.max_tracking, header.tracking_number);
    summary_.max_order_ref = std::max({summary_.max_order_ref, fields.order_ref, fields.new_ref});
    summary_.max_shares = std::max(summary_.max_shares, fields.shares);
    summary_.max_price = std::max(summary_.max_price, fields.price);
    summary_.max_match = std::max(summary_.max_match, fields.match);
    return {};
}

ColumnsManifest ColumnsEncoder::manifest(std::uint64_t tail_bytes) const noexcept {
    return {.records = summary_.records, .tail_bytes = tail_bytes, .text_bytes = text_totals_};
}

std::expected<ColumnsWriter, StoreError> ColumnsWriter::create(
    const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        errno = error.value();
        return std::unexpected(io_error("create_directories"));
    }
    if (std::filesystem::exists(directory / kColumnsManifestName, error)) {
        errno = EEXIST;
        return std::unexpected(io_error("manifest already there"));
    }
    std::array<int, kColumnFileCount> fds{};
    fds.fill(-1);
    for (std::size_t i = 0; i < kColumnFileCount; ++i) {
        const std::filesystem::path path = directory / kColumnFileNames[i];
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        fds[i] = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fds[i] < 0) {
            const StoreError failed = io_error("open");
            for (const int fd : fds) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
            return std::unexpected(failed);
        }
    }
    return ColumnsWriter{directory, fds};
}

ColumnsWriter::ColumnsWriter(std::filesystem::path directory,
                             std::array<int, kColumnFileCount> fds)
    : directory_(std::move(directory)), fds_(fds) {}

ColumnsWriter::ColumnsWriter(ColumnsWriter&& other) noexcept
    : directory_(std::move(other.directory_)),
      fds_(std::exchange(other.fds_, {})),
      encoder_(std::move(other.encoder_)) {
    other.fds_.fill(-1);
}

ColumnsWriter::~ColumnsWriter() { close_all(); }

void ColumnsWriter::close_all() noexcept {
    for (int& fd : fds_) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

std::expected<void, StoreError> ColumnsWriter::drain(std::size_t threshold) {
    for (std::size_t i = 0; i < kColumnFileCount; ++i) {
        std::vector<std::byte>& buffer = encoder_.buffers()[i];
        if (buffer.size() < threshold || buffer.empty()) {
            continue;
        }
        std::span<const std::byte> left = buffer;
        while (!left.empty()) {
            const auto written = ::write(fds_[i], left.data(), left.size());
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(io_error("write"));
            }
            left = left.subspan(static_cast<std::size_t>(written));
        }
        buffer.clear();
    }
    return {};
}

std::expected<void, StoreError> ColumnsWriter::append(std::span<const std::byte> payload) {
    if (auto added = encoder_.append(payload); !added.has_value()) {
        return added;
    }
    return drain(kDrainThreshold);
}

std::expected<ColumnsSummary, StoreError> ColumnsWriter::finish(std::span<const std::byte> tail) {
    std::vector<std::byte>& tail_buffer = encoder_.buffers()[index_of(ColumnFile::tail)];
    tail_buffer.assign(tail.begin(), tail.end());
    if (auto drained = drain(0); !drained.has_value()) {
        return std::unexpected(drained.error());
    }
    close_all();

    const std::string manifest = format_columns_manifest(encoder_.manifest(tail.size()));
    const std::filesystem::path path = directory_ / kColumnsManifestName;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return std::unexpected(io_error("open"));
    }
    std::span<const std::byte> left = text_bytes(manifest);
    while (!left.empty()) {
        const auto written = ::write(fd, left.data(), left.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const StoreError failed = io_error("write");
            ::close(fd);
            return std::unexpected(failed);
        }
        left = left.subspan(static_cast<std::size_t>(written));
    }
    if (::close(fd) != 0) {
        return std::unexpected(io_error("close"));
    }
    ColumnsSummary summary = encoder_.summary();
    summary.tail_bytes = tail.size();
    return summary;
}

std::expected<ColumnsParser, StoreError> ColumnsParser::open(const ColumnsManifest& manifest,
                                                             const ColumnBytes& files) {
    const std::uint64_t records = manifest.records;
    for (std::size_t i = 0; i < kFixedColumnCount; ++i) {
        const std::uint64_t width = kFixedColumnWidths[i];
        // The bound comes first, so the product below cannot overflow.
        if (records > files[i].size() / width) {
            return std::unexpected(bad(StoreErrorKind::bad_column_size, kColumnFileNames[i], 0,
                                       records, files[i].size() / width));
        }
        if (auto sized = require_size(files, i, records * width); !sized.has_value()) {
            return std::unexpected(sized.error());
        }
    }

    constexpr std::array<std::pair<ColumnFile, ColumnFile>, kTextColumnCount> kTexts{{
        {ColumnFile::stock_off, ColumnFile::stock_dat},
        {ColumnFile::mpid_off, ColumnFile::mpid_dat},
        {ColumnFile::rest_off, ColumnFile::rest_dat},
    }};
    for (std::size_t i = 0; i < kTextColumnCount; ++i) {
        const auto [offsets, bytes] = kTexts[i];
        // `records` fits a file of 8-byte values, so + 1 and * 8 cannot overflow here.
        if (auto sized = require_size(files, index_of(offsets), (records + 1) * 8);
            !sized.has_value()) {
            return std::unexpected(sized.error());
        }
        if (auto sized = require_size(files, index_of(bytes), manifest.text_bytes[i]);
            !sized.has_value()) {
            return std::unexpected(sized.error());
        }
        if (auto ends = check_text_ends(files, offsets, bytes, records); !ends.has_value()) {
            return std::unexpected(ends.error());
        }
    }
    if (auto sized = require_size(files, index_of(ColumnFile::tail), manifest.tail_bytes);
        !sized.has_value()) {
        return std::unexpected(sized.error());
    }
    return ColumnsParser{manifest, files};
}

ColumnsParser::ColumnsParser(const ColumnsManifest& manifest, const ColumnBytes& files)
    : manifest_(manifest), files_(files), frame_(kLargestFrame) {}

std::span<const std::byte> ColumnsParser::tail() const noexcept {
    return files_[index_of(ColumnFile::tail)];
}

std::expected<std::span<const std::byte>, StoreError> ColumnsParser::rebuild_row() {
    const std::uint64_t row = row_;
    if (const auto seq = fixed_value<ColumnFile::seq>(files_, row);
        std::bit_cast<std::uint64_t>(seq) != row) {
        return std::unexpected(bad(StoreErrorKind::bad_row, name_of(ColumnFile::seq), row, row,
                                   std::bit_cast<std::uint64_t>(seq)));
    }
    const auto ts_ns = bounded<ColumnFile::ts_ns>(files_, row, itch::kMaxTimestampNs);
    if (!ts_ns.has_value()) {
        return std::unexpected(ts_ns.error());
    }
    const auto type_value = bounded<ColumnFile::msg_type>(files_, row, 0xFF);
    if (!type_value.has_value()) {
        return std::unexpected(type_value.error());
    }
    const auto type = static_cast<std::byte>(*type_value);
    const auto rest =
        text_value(files_, ColumnFile::rest_off, ColumnFile::rest_dat, row);
    if (!rest.has_value()) {
        return std::unexpected(rest.error());
    }

    if (rest->size() >= kRawRestPrefix.size() &&
        std::ranges::equal(rest->first(kRawRestPrefix.size()), text_bytes(kRawRestPrefix))) {
        if (auto empty = check_raw_row_empty(files_, row); !empty.has_value()) {
            return std::unexpected(empty.error());
        }
        // Nothing rebuilds from this value, so without this check it could differ
        // unseen. The form fixes it: a raw row repeats the row before.
        if (*ts_ns != previous_ts_ns_) {
            return std::unexpected(bad(StoreErrorKind::bad_row, name_of(ColumnFile::ts_ns), row,
                                       previous_ts_ns_, *ts_ns));
        }
        const auto hex = rest->subspan(kRawRestPrefix.size());
        if (hex.empty() || (hex.size() % 2) != 0 || hex.size() / 2 > itch::kMaxPayloadSize) {
            return std::unexpected(bad(StoreErrorKind::bad_row, name_of(ColumnFile::rest_dat), row,
                                       itch::kMaxPayloadSize * 2, hex.size()));
        }
        payload_.clear();
        if (const auto digit = append_unhex(payload_, hex); digit.has_value()) {
            return std::unexpected(
                bad(StoreErrorKind::bad_row, name_of(ColumnFile::rest_dat), row, 0, *digit));
        }
        if (payload_.front() != type) {
            return std::unexpected(bad(StoreErrorKind::bad_row, name_of(ColumnFile::msg_type), row,
                                       byte_of(payload_.front()), byte_of(type)));
        }
        write_frame_prefix(frame_, payload_.size());
        std::ranges::copy(payload_, frame_.begin() + itch::kFramePrefixSize);
        return std::span<const std::byte>{frame_}.first(itch::kFramePrefixSize + payload_.size());
    }

    const auto locate = bounded<ColumnFile::locate>(files_, row, 0xFFFF);
    if (!locate.has_value()) {
        return std::unexpected(locate.error());
    }
    const auto tracking = bounded<ColumnFile::tracking>(files_, row, 0xFFFF);
    if (!tracking.has_value()) {
        return std::unexpected(tracking.error());
    }

    if (const auto columns = hot_columns(type); columns.has_value()) {
        if (!rest->empty()) {
            return std::unexpected(
                bad(StoreErrorKind::bad_row, name_of(ColumnFile::rest_dat), row, 0, rest->size()));
        }
        const auto rebuilt = rebuild_fields(files_, row, *columns);
        if (!rebuilt.has_value()) {
            return std::unexpected(rebuilt.error());
        }
        const MessageHeader header{.stock_locate = static_cast<std::uint16_t>(*locate),
                                   .tracking_number = static_cast<std::uint16_t>(*tracking),
                                   .timestamp_ns = *ts_ns};
        const auto written = itch::encode_frame(hot_message(type, header, *rebuilt), frame_);
        if (!written.has_value()) {
            return std::unexpected(
                bad(StoreErrorKind::unencodable_record, written.error().field, row,
                    written.error().expected, written.error().actual));
        }
        return std::span<const std::byte>{frame_}.first(*written);
    }

    // A rare row: the header from its columns, then the body. Nothing else may be set.
    const auto empty = rebuild_fields(files_, row, 0);
    if (!empty.has_value()) {
        return std::unexpected(empty.error());
    }
    const auto wire_size = itch::wire_size_of(type);
    if (!wire_size.has_value()) {
        return std::unexpected(
            bad(StoreErrorKind::bad_row, name_of(ColumnFile::msg_type), row, 0, byte_of(type)));
    }
    const std::size_t body_size = *wire_size - kMessageHeaderSize;
    if (rest->size() != body_size * 2) {
        return std::unexpected(bad(StoreErrorKind::bad_row, name_of(ColumnFile::rest_dat), row,
                                   body_size * 2, rest->size()));
    }
    payload_.clear();
    payload_.push_back(type);
    const auto put_be = [&](std::uint64_t value, std::size_t bytes) {
        for (std::size_t i = bytes; i > 0; --i) {
            payload_.push_back(static_cast<std::byte>((value >> ((i - 1) * 8U)) & 0xFFU));
        }
    };
    put_be(*locate, 2);
    put_be(*tracking, 2);
    put_be(*ts_ns, 6);
    if (const auto digit = append_unhex(payload_, *rest); digit.has_value()) {
        return std::unexpected(
            bad(StoreErrorKind::bad_row, name_of(ColumnFile::rest_dat), row, 0, *digit));
    }
    const auto message = itch::decode_strict(payload_);
    if (!message.has_value()) {
        return std::unexpected(bad(StoreErrorKind::bad_row, message.error().field, row,
                                   message.error().expected, message.error().actual));
    }
    const auto written = itch::encode_frame(*message, frame_);
    if (!written.has_value()) {
        return std::unexpected(bad(StoreErrorKind::unencodable_record, written.error().field, row,
                                   written.error().expected, written.error().actual));
    }
    return std::span<const std::byte>{frame_}.first(*written);
}

std::expected<std::optional<std::span<const std::byte>>, StoreError> ColumnsParser::next() {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    if (row_ == manifest_.records) {
        return std::optional<std::span<const std::byte>>{std::nullopt};
    }
    const auto frame = rebuild_row();
    if (!frame.has_value()) {
        failed_ = frame.error();
        return std::unexpected(frame.error());
    }
    previous_ts_ns_ = static_cast<std::uint64_t>(fixed_value<ColumnFile::ts_ns>(files_, row_));
    ++row_;
    return std::optional{*frame};
}

}  // namespace tick_store::store
