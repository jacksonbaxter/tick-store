#include "tick_store/store/itch_segment.hpp"

#include "tick_store/itch/big_endian.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/itch_store.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::store {
namespace {

constexpr std::size_t kLocates = std::size_t{kMax16} + 1;

[[nodiscard]] std::uint8_t symbol_of(std::byte type) noexcept {
    return std::to_integer<std::uint8_t>(type);
}

[[nodiscard]] std::uint64_t mpid_value(const itch::Mpid& mpid) noexcept {
    std::uint64_t value = 0;
    for (const std::byte b : mpid) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(b);
    }
    return value;
}

[[nodiscard]] itch::Mpid mpid_of(std::uint64_t value) noexcept {
    itch::Mpid mpid{};
    for (std::size_t i = 0; i < mpid.size(); ++i) {
        mpid[mpid.size() - 1 - i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
    return mpid;
}

[[nodiscard]] std::byte byte_of(std::uint64_t value) noexcept {
    return static_cast<std::byte>(value & 0xFFU);
}

// The orders a segment knows, by reference: the locate whose list holds each, and where.
// Open addressing with linear probing, cleared slot by slot so a segment pays for the
// slots it used, not for the table.
class RefMap {
public:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t index = 0;
        std::uint16_t locate = 0;
        bool used = false;
    };

    RefMap() { slots_.resize(kInitialSlots); }

    [[nodiscard]] const Slot* find(std::uint64_t key) const noexcept {
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t at = hash(key) & mask;; at = (at + 1) & mask) {
            const Slot& slot = slots_[at];
            if (!slot.used) {
                return nullptr;
            }
            if (slot.key == key) {
                return &slot;
            }
        }
    }

    void assign(std::uint64_t key, std::uint16_t locate, std::uint32_t index) {
        if ((used_.size() + 1) * 2 > slots_.size()) {
            grow();
        }
        Slot& slot = probe(key);
        if (!slot.used) {
            slot.used = true;
            slot.key = key;
            used_.push_back(static_cast<std::size_t>(&slot - slots_.data()));
        }
        slot.locate = locate;
        slot.index = index;
    }

    void clear() noexcept {
        for (const std::size_t at : used_) {
            slots_[at] = Slot{};
        }
        used_.clear();
    }

private:
    static constexpr std::size_t kInitialSlots = std::size_t{1} << 12U;

    [[nodiscard]] static std::size_t hash(std::uint64_t key) noexcept {
        return static_cast<std::size_t>((key * 0x9E37'79B9'7F4A'7C15ULL) >> 20U);
    }

    [[nodiscard]] Slot& probe(std::uint64_t key) noexcept {
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t at = hash(key) & mask;; at = (at + 1) & mask) {
            Slot& slot = slots_[at];
            if (!slot.used || slot.key == key) {
                return slot;
            }
        }
    }

    void grow() {
        std::vector<Slot> old(slots_.size() * 2);
        std::swap(old, slots_);
        used_.clear();
        for (const Slot& slot : old) {
            if (slot.used) {
                Slot& moved = probe(slot.key);
                moved = slot;
                used_.push_back(static_cast<std::size_t>(&moved - slots_.data()));
            }
        }
    }

    std::vector<Slot> slots_;
    std::vector<std::size_t> used_;
};

// Values sorted by a byte key, stably: a counting sort.
[[nodiscard]] std::vector<std::uint64_t> group_by_byte(const std::vector<std::uint64_t>& values,
                                                       const std::vector<std::uint8_t>& keys) {
    std::array<std::size_t, 257> start{};
    for (const std::uint8_t key : keys) {
        ++start[std::size_t{key} + 1];
    }
    for (std::size_t i = 1; i < start.size(); ++i) {
        start[i] += start[i - 1];
    }
    std::vector<std::uint64_t> out(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        out[start[keys[i]]++] = values[i];
    }
    return out;
}

// The inverse: file order back from values grouped by byte key.
void ungroup_by_byte(std::vector<std::uint64_t>& values, const std::vector<std::uint8_t>& keys) {
    std::array<std::size_t, 257> start{};
    for (const std::uint8_t key : keys) {
        ++start[std::size_t{key} + 1];
    }
    for (std::size_t i = 1; i < start.size(); ++i) {
        start[i] += start[i - 1];
    }
    std::vector<std::uint64_t> out(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        out[i] = values[start[keys[i]]++];
    }
    values = std::move(out);
}

// The positions of values in order of a 32-bit key, stably: what the writer emits them in.
[[nodiscard]] std::vector<std::uint32_t> order_by_key(const std::vector<std::uint32_t>& keys) {
    std::vector<std::uint64_t> sorted(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        sorted[i] = (std::uint64_t{keys[i]} << 32U) | i;
    }
    std::ranges::sort(sorted);
    std::vector<std::uint32_t> order(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        order[i] = static_cast<std::uint32_t>(sorted[i] & 0xFFFF'FFFFU);
    }
    return order;
}

[[nodiscard]] std::vector<std::uint64_t> group_by_key(const std::vector<std::uint64_t>& values,
                                                      const std::vector<std::uint32_t>& keys) {
    const std::vector<std::uint32_t> order = order_by_key(keys);
    std::vector<std::uint64_t> out(values.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        out[i] = values[order[i]];
    }
    return out;
}

void ungroup_by_key(std::vector<std::uint64_t>& values, const std::vector<std::uint32_t>& keys) {
    const std::vector<std::uint32_t> order = order_by_key(keys);
    std::vector<std::uint64_t> out(values.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        out[order[i]] = values[i];
    }
    values = std::move(out);
}

void set_bit(std::vector<std::byte>& bitmap, std::uint16_t locate) {
    bitmap[locate / 8U] |= static_cast<std::byte>(1U << (locate % 8U));
}

[[nodiscard]] std::vector<std::byte> trimmed(const std::vector<std::byte>& bitmap) {
    std::size_t size = bitmap.size();
    while (size > 0 && bitmap[size - 1] == std::byte{0}) {
        --size;
    }
    return {bitmap.begin(), bitmap.begin() + static_cast<std::ptrdiff_t>(size)};
}

// The locate column as runs (ColumnOrder::locate_runs) and the run directory, from the
// stored-order locates and types of the typed records, each one's index in the segment,
// and the ungrouped reference distances.
struct LocateRuns {
    std::vector<std::uint64_t> gaps;
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> starts;
    std::vector<std::uint64_t> add_starts;
    std::vector<std::uint64_t> import_starts;
};

[[nodiscard]] bool names_an_order(std::uint64_t type) noexcept {
    return type == 'E' || type == 'C' || type == 'X' || type == 'D' || type == 'U';
}

[[nodiscard]] LocateRuns to_runs(const std::vector<std::uint64_t>& locates,
                                 const std::vector<std::uint64_t>& types,
                                 const std::vector<std::uint64_t>& positions,
                                 const std::vector<std::uint64_t>& ref_distance) {
    LocateRuns runs;
    runs.gaps.resize(locates.size());
    std::uint64_t adds = 0;
    std::uint64_t imports = 0;
    std::size_t named = 0;
    for (std::size_t k = 0; k < locates.size(); ++k) {
        const bool first = k == 0 || locates[k] != locates[k - 1];
        if (first) {
            runs.ids.push_back(locates[k]);
            runs.starts.push_back(k);
            runs.add_starts.push_back(adds);
            runs.import_starts.push_back(imports);
        }
        runs.gaps[k] = first ? positions[k] : positions[k] - positions[k - 1] - 1U;
        // The typed records come first in stored order, so types[k] is record k's.
        const std::uint64_t type = types[k];
        if (type == 'A' || type == 'F') {
            ++adds;
        }
        if (names_an_order(type)) {
            if (ref_distance[named] == 0) {
                ++imports;
            }
            ++named;
        }
    }
    return runs;
}

// The run directory against the types and the reference distances, both in stored order:
// each run's adds and imports. Returns each stored typed record's locate.
[[nodiscard]] std::expected<std::vector<std::uint64_t>, ItchStoreError> run_locates(
    std::uint64_t segment, const SegmentColumns& columns, std::uint64_t typed) {
    const std::vector<std::uint64_t>& types = columns[Column::type];
    const std::vector<std::uint64_t>& ids = columns[Column::locate_run];
    const std::vector<std::uint64_t>& starts = columns[Column::locate_run_start];
    const std::vector<std::uint64_t>& add_starts = columns[Column::locate_run_add_start];
    const std::vector<std::uint64_t>& import_starts = columns[Column::locate_run_import_start];
    const std::vector<std::uint64_t>& distances = columns[Column::ref_distance];
    const auto refuse = [&](Column column, std::string_view name, std::uint64_t index,
                            std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::bad_value;
        error.field = name;
        error.segment = segment;
        error.column = column_info(column).name;
        error.index = index;
        error.expected = expected;
        error.actual = actual;
        return std::unexpected(error);
    };
    std::vector<std::uint64_t> locates(static_cast<std::size_t>(typed));
    std::uint64_t adds = 0;
    std::uint64_t imports = 0;
    std::size_t named = 0;
    std::size_t run = 0;
    for (std::uint64_t k = 0; k < typed; ++k) {
        if (run < starts.size() && starts[run] == k) {
            if (add_starts[run] != adds) {
                return refuse(Column::locate_run_add_start, "locate run add start", run, adds,
                              add_starts[run]);
            }
            if (import_starts[run] != imports) {
                return refuse(Column::locate_run_import_start, "locate run import start", run,
                              imports, import_starts[run]);
            }
            ++run;
        }
        // check_run_directory() put the first start at 0, so run >= 1 here.
        locates[k] = ids[run - 1];
        const std::uint64_t type = types[k];
        if (type == 'A' || type == 'F') {
            ++adds;
        }
        if (names_an_order(type)) {
            if (distances[named] == 0) {
                ++imports;
            }
            ++named;
        }
    }
    return locates;
}

template <class T>
concept HasStock = requires(const T& message) {
    { message.stock } -> std::convertible_to<itch::Stock>;
};

}  // namespace

// --- SegmentBuilder ----------------------------------------------------------------------

struct SegmentBuilder::State {
    // The segment's records as added, a raw record's bytes copied into raw_bytes_held
    // (its RawMessage's span is rebuilt from raw_at when the segment is finished).
    std::vector<itch::Message> held;
    std::vector<std::byte> raw_bytes_held;
    std::vector<std::pair<std::size_t, std::size_t>> raw_at;
    SegmentColumns columns;
    // The locate and the index in the segment of each typed record, in stored order.
    std::vector<std::uint64_t> locates;
    std::vector<std::uint64_t> positions;
    std::uint64_t position = 0;
    std::uint64_t first_position = 0;
    std::uint64_t last_position = 0;
    std::vector<std::uint8_t> tracking_key;
    std::vector<std::uint8_t> ref_key;
    std::vector<std::uint8_t> reduce_key;
    std::vector<std::uint32_t> price_key;
    std::vector<std::uint32_t> shares_key;
    // How long each locate's list of known orders is.
    std::vector<std::uint32_t> known = std::vector<std::uint32_t>(kLocates, 0);
    std::vector<std::uint16_t> touched;
    RefMap refs;
    std::uint32_t records = 0;
    std::uint32_t typed = 0;
    bool plain = true;
    std::uint64_t first_time = 0;
    std::uint64_t last_time = 0;
    std::uint64_t min_time = 0;
    std::uint64_t max_time = 0;
    std::vector<std::byte> bitmap = std::vector<std::byte>(kMaxLocateBitmapBytes);
    std::array<std::byte, itch::kMaxPayloadSize> scratch{};
    SymbolTable* symbols = nullptr;

    void push(Column column, std::uint64_t value) { columns[column].push_back(value); }

    void bump(std::uint16_t locate) {
        if (known[locate] == 0) {
            touched.push_back(locate);
        }
        ++known[locate];
    }

    void header(std::byte type, const itch::MessageHeader& h) {
        const std::uint8_t symbol = symbol_of(type);
        push(Column::type, symbol);
        locates.push_back(h.stock_locate);
        positions.push_back(position);
        push(Column::timestamp, h.timestamp_ns);
        push(Column::tracking, h.tracking_number);
        tracking_key.push_back(symbol);
        // The first and last times are the file's first and last typed records', which the
        // stored order need not visit first and last.
        if (typed == 0) {
            first_time = h.timestamp_ns;
            last_time = h.timestamp_ns;
            min_time = h.timestamp_ns;
            max_time = h.timestamp_ns;
            first_position = position;
            last_position = position;
        }
        if (position < first_position) {
            first_position = position;
            first_time = h.timestamp_ns;
        }
        if (position > last_position) {
            last_position = position;
            last_time = h.timestamp_ns;
        }
        min_time = std::min(min_time, h.timestamp_ns);
        max_time = std::max(max_time, h.timestamp_ns);
        ++typed;
        set_bit(bitmap, h.stock_locate);
    }

    void create(std::uint16_t locate, std::uint64_t reference) {
        push(Column::creation_ref, reference);
        refs.assign(reference, locate, known[locate]);
        bump(locate);
    }

    void name(std::byte type, std::uint16_t locate, std::uint64_t reference) {
        ref_key.push_back(symbol_of(type));
        const RefMap::Slot* slot = refs.find(reference);
        if (slot != nullptr && slot->locate == locate) {
            push(Column::ref_distance, known[locate] - slot->index);
            return;
        }
        push(Column::ref_distance, 0);
        push(Column::import_ref, reference);
        refs.assign(reference, locate, known[locate]);
        bump(locate);
    }

    void reduce(std::byte type, std::uint32_t shares) {
        push(Column::reduce_shares, shares);
        reduce_key.push_back(symbol_of(type));
    }

    void derived_stock(std::uint16_t locate, const itch::Stock& stock) {
        if (symbols->learn(locate, stock) == stock) {
            return;
        }
        push(Column::stock_exception_record, records);
        for (const std::byte b : stock) {
            push(Column::stock_exception_value, std::to_integer<std::uint64_t>(b));
        }
    }

    void add_order(std::byte type, const itch::MessageHeader& h, std::uint64_t reference,
                   std::byte side, std::uint32_t shares, itch::Price4 price,
                   const itch::Stock& stock) {
        header(type, h);
        create(h.stock_locate, reference);
        const std::uint8_t side_value = symbol_of(side);
        push(Column::add_side, side_value);
        push(Column::add_price, static_cast<std::uint32_t>(price));
        push(Column::add_shares, shares);
        price_key.push_back((std::uint32_t{h.stock_locate} << 8U) | side_value);
        shares_key.push_back(h.stock_locate);
        derived_stock(h.stock_locate, stock);
    }

    void operator()(const itch::AddOrder& m) {
        add_order(itch::AddOrder::type_code, m.header, m.order_reference_number,
                  m.buy_sell_indicator, m.shares, m.price, m.stock);
    }
    void operator()(const itch::AddOrderWithMpid& m) {
        add_order(itch::AddOrderWithMpid::type_code, m.header, m.order_reference_number,
                  m.buy_sell_indicator, m.shares, m.price, m.stock);
        push(Column::mpid, mpid_value(m.attribution));
    }
    void operator()(const itch::OrderExecuted& m) {
        header(itch::OrderExecuted::type_code, m.header);
        name(itch::OrderExecuted::type_code, m.header.stock_locate, m.order_reference_number);
        reduce(itch::OrderExecuted::type_code, m.executed_shares);
        push(Column::match, m.match_number);
    }
    void operator()(const itch::OrderExecutedWithPrice& m) {
        header(itch::OrderExecutedWithPrice::type_code, m.header);
        name(itch::OrderExecutedWithPrice::type_code, m.header.stock_locate,
             m.order_reference_number);
        reduce(itch::OrderExecutedWithPrice::type_code, m.executed_shares);
        push(Column::match, m.match_number);
        push(Column::printable, symbol_of(m.printable));
        push(Column::exec_price, static_cast<std::uint32_t>(m.execution_price));
    }
    void operator()(const itch::OrderCancel& m) {
        header(itch::OrderCancel::type_code, m.header);
        name(itch::OrderCancel::type_code, m.header.stock_locate, m.order_reference_number);
        reduce(itch::OrderCancel::type_code, m.cancelled_shares);
    }
    void operator()(const itch::OrderDelete& m) {
        header(itch::OrderDelete::type_code, m.header);
        name(itch::OrderDelete::type_code, m.header.stock_locate, m.order_reference_number);
    }
    void operator()(const itch::OrderReplace& m) {
        header(itch::OrderReplace::type_code, m.header);
        name(itch::OrderReplace::type_code, m.header.stock_locate,
             m.original_order_reference_number);
        create(m.header.stock_locate, m.new_order_reference_number);
        push(Column::replace_shares, m.shares);
        push(Column::replace_price, static_cast<std::uint32_t>(m.price));
    }
    void operator()(const itch::Trade& m) {
        header(itch::Trade::type_code, m.header);
        push(Column::trade_ref, m.order_reference_number);
        push(Column::trade_side, symbol_of(m.buy_sell_indicator));
        push(Column::trade_shares, m.shares);
        push(Column::trade_price, static_cast<std::uint32_t>(m.price));
        push(Column::match, m.match_number);
        derived_stock(m.header.stock_locate, m.stock);
    }
    void operator()(const itch::CrossTrade& m) {
        header(itch::CrossTrade::type_code, m.header);
        push(Column::cross_shares, m.shares);
        push(Column::cross_price, static_cast<std::uint32_t>(m.cross_price));
        push(Column::match, m.match_number);
        push(Column::cross_type, symbol_of(m.cross_type));
        derived_stock(m.header.stock_locate, m.stock);
    }
    void operator()(const itch::BrokenTrade& m) {
        header(itch::BrokenTrade::type_code, m.header);
        push(Column::match, m.match_number);
    }
    void operator()(const itch::NetOrderImbalance& m) {
        header(itch::NetOrderImbalance::type_code, m.header);
        push(Column::imbalance_shares, m.paired_shares);
        push(Column::imbalance_shares, m.imbalance_shares);
        push(Column::imbalance_price, static_cast<std::uint32_t>(m.far_price));
        push(Column::imbalance_price, static_cast<std::uint32_t>(m.near_price));
        push(Column::imbalance_price, static_cast<std::uint32_t>(m.current_reference_price));
        push(Column::imbalance_flags, std::uint64_t{symbol_of(m.imbalance_direction)} |
                                          (std::uint64_t{symbol_of(m.cross_type)} << 8U) |
                                          (std::uint64_t{symbol_of(m.price_variation_indicator)}
                                           << 16U));
        derived_stock(m.header.stock_locate, m.stock);
    }
    void operator()(const itch::MwcbDeclineLevel& m) {
        header(itch::MwcbDeclineLevel::type_code, m.header);
        push(Column::mwcb_level, static_cast<std::uint64_t>(m.level_1));
        push(Column::mwcb_level, static_cast<std::uint64_t>(m.level_2));
        push(Column::mwcb_level, static_cast<std::uint64_t>(m.level_3));
    }
    void operator()(const itch::RawMessage& m) {
        push(Column::type, 0);
        push(Column::raw_length, m.bytes.size());
        for (const std::byte b : m.bytes) {
            push(Column::raw_bytes, std::to_integer<std::uint64_t>(b));
        }
    }
    // The rest: S, R, H, Y, L, W, K, J, h, N, O. Their bytes after the header are kept
    // whole, from the real encoder.
    template <class T>
        requires(!std::is_same_v<T, itch::RawMessage>)
    void operator()(const T& m) {
        header(T::type_code, m.header);
        const auto written = itch::encode(itch::Message{m}, scratch);
        // A typed message with an in-range timestamp always encodes, and add() checked it.
        const std::size_t size = written.value_or(kItchHeaderBytes);
        for (std::size_t i = kItchHeaderBytes; i < size; ++i) {
            push(Column::rare_bytes, std::to_integer<std::uint64_t>(scratch[i]));
        }
        if constexpr (HasStock<T>) {
            symbols->learn(m.header.stock_locate, m.stock);
        }
    }
};

SegmentBuilder::SegmentBuilder() : state_(std::make_unique<State>()) {}
SegmentBuilder::SegmentBuilder(SegmentBuilder&&) noexcept = default;
SegmentBuilder& SegmentBuilder::operator=(SegmentBuilder&&) noexcept = default;
SegmentBuilder::~SegmentBuilder() = default;

std::uint32_t SegmentBuilder::records() const noexcept { return state_->records; }

std::expected<void, ItchStoreError> SegmentBuilder::add(const itch::Message& message,
                                                        SymbolTable& symbols) {
    const auto refuse = [](std::string_view name, std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::invalid_argument;
        error.field = name;
        error.expected = expected;
        error.actual = actual;
        return std::unexpected(error);
    };
    if (const auto* raw = std::get_if<itch::RawMessage>(&message)) {
        if (raw->bytes.empty() || raw->bytes.size() > itch::kMaxPayloadSize) {
            return refuse("raw record size", itch::kMaxPayloadSize, raw->bytes.size());
        }
        const auto decoded = itch::decode(raw->bytes);
        if (!decoded.has_value() || !std::holds_alternative<itch::RawMessage>(*decoded)) {
            return refuse("raw record conforms", 0, 1);
        }
    } else {
        const std::uint64_t time = std::visit(
            [](const auto& m) -> std::uint64_t {
                if constexpr (std::is_same_v<std::decay_t<decltype(m)>, itch::RawMessage>) {
                    return 0;
                } else {
                    return m.header.timestamp_ns;
                }
            },
            message);
        if (time > kMax48) {
            return refuse("timestamp", kMax48, time);
        }
    }
    State& s = *state_;
    s.symbols = &symbols;
    if (const auto* raw = std::get_if<itch::RawMessage>(&message)) {
        s.raw_at.emplace_back(s.raw_bytes_held.size(), raw->bytes.size());
        s.raw_bytes_held.insert(s.raw_bytes_held.end(), raw->bytes.begin(), raw->bytes.end());
        s.held.emplace_back(itch::RawMessage{.bytes = {}, .reason = raw->reason});
    } else {
        s.held.push_back(message);
    }
    s.plain = s.plain && column_form_plain(message);
    ++s.records;
    return {};
}

SegmentColumns SegmentBuilder::finish(std::uint64_t first_record, SegmentSummary& summary) {
    State& s = *state_;
    // Stored order: the typed records by locate, stably, then the raw records, by a
    // counting sort on the locate with the raw records keyed after every locate.
    {
        const auto key_of = [](const itch::Message& m) -> std::size_t {
            return std::visit(
                [](const auto& r) -> std::size_t {
                    if constexpr (std::is_same_v<std::decay_t<decltype(r)>, itch::RawMessage>) {
                        return kLocates;
                    } else {
                        return r.header.stock_locate;
                    }
                },
                m);
        };
        std::vector<std::uint32_t> start(kLocates + 2, 0);
        for (const itch::Message& m : s.held) {
            ++start[key_of(m) + 1];
        }
        for (std::size_t k = 1; k < start.size(); ++k) {
            start[k] += start[k - 1];
        }
        std::vector<std::uint32_t> order(s.held.size());
        for (std::size_t i = 0; i < s.held.size(); ++i) {
            order[start[key_of(s.held[i])]++] = static_cast<std::uint32_t>(i);
        }
        std::size_t raw = 0;
        std::vector<std::size_t> raw_index(s.held.size(), 0);
        for (std::size_t i = 0; i < s.held.size(); ++i) {
            if (std::holds_alternative<itch::RawMessage>(s.held[i])) {
                raw_index[i] = raw++;
            }
        }
        s.records = 0;
        for (const std::uint32_t i : order) {
            itch::Message& m = s.held[i];
            if (auto* r = std::get_if<itch::RawMessage>(&m)) {
                const auto [at, size] = s.raw_at[raw_index[i]];
                r->bytes = std::span<const std::byte>{s.raw_bytes_held}.subspan(at, size);
            }
            s.position = i;
            std::visit(s, m);
            ++s.records;
        }
    }
    summary = SegmentSummary{};
    summary.first_record = first_record;
    summary.records = s.records;
    summary.typed_records = s.typed;
    summary.plain = s.plain;
    if (s.typed > 0) {
        summary.first_time = s.first_time;
        summary.last_time = s.last_time;
        summary.min_time = s.min_time;
        summary.max_time = s.max_time;
        summary.locates = trimmed(s.bitmap);
    }
    SegmentColumns out = std::move(s.columns);
    LocateRuns runs = to_runs(s.locates, out[Column::type], s.positions, out[Column::ref_distance]);
    out[Column::locate] = std::move(runs.gaps);
    out[Column::locate_run] = std::move(runs.ids);
    out[Column::locate_run_start] = std::move(runs.starts);
    out[Column::locate_run_add_start] = std::move(runs.add_starts);
    out[Column::locate_run_import_start] = std::move(runs.import_starts);
    const std::vector<std::uint64_t> ref_stored_order = out[Column::ref_distance];
    out[Column::tracking] = group_by_byte(out[Column::tracking], s.tracking_key);
    out[Column::ref_distance] = group_by_byte(out[Column::ref_distance], s.ref_key);
    out[Column::reduce_shares] = group_by_byte(out[Column::reduce_shares], s.reduce_key);
    out[Column::add_price] = group_by_key(out[Column::add_price], s.price_key);
    out[Column::add_shares] = group_by_key(out[Column::add_shares], s.shares_key);
    // The times as residuals over their bases; the seek's zone maps take the times.
    const std::vector<std::uint64_t> times = std::move(out[Column::timestamp]);
    out[Column::time_base] = time_bases(times, s.positions, s.records);
    out[Column::timestamp] = time_residuals(times, s.positions, out[Column::locate_run_start],
                                            out[Column::time_base]);
    out[Column::seek] = compute_seek(column_maxima(out), out[Column::type], times,
                                     ref_stored_order, s.positions);

    s.columns = SegmentColumns{};
    s.held.clear();
    s.raw_bytes_held.clear();
    s.raw_at.clear();
    s.locates.clear();
    s.positions.clear();
    s.tracking_key.clear();
    s.ref_key.clear();
    s.reduce_key.clear();
    s.price_key.clear();
    s.shares_key.clear();
    for (const std::uint16_t locate : s.touched) {
        s.known[locate] = 0;
    }
    s.touched.clear();
    s.refs.clear();
    s.records = 0;
    s.typed = 0;
    s.plain = true;
    std::ranges::fill(s.bitmap, std::byte{0});
    return out;
}

// --- SegmentAssembler ----------------------------------------------------------------------

struct SegmentAssembler::State {
    SegmentColumns columns;
    const SegmentSummary* summary = nullptr;
    const SymbolTable* symbols = nullptr;
    std::uint64_t segment = 0;
    std::uint64_t records = 0;
    // The stored record being decoded.
    std::uint64_t next_record = 0;
    // Each stored typed record's locate and index in the segment, and for each index in
    // the segment, the stored record.
    std::vector<std::uint64_t> locates;
    std::vector<std::uint64_t> positions;
    std::vector<std::uint32_t> stored_at;
    // The records in stored order, decoded at the first next(), and the raw records'
    // bytes they borrow; then the next index in the segment to hand out.
    std::vector<itch::Message> decoded;
    std::vector<std::byte> raw_held;
    bool done_decoding = false;
    std::uint64_t next_out = 0;
    // The error decoding stopped at, given again to any later call.
    std::optional<ItchStoreError> failure;
    std::array<std::size_t, kColumnCount> at{};
    // Each locate's known orders, as references, in the order they were appended.
    std::vector<std::vector<std::uint64_t>> lists = std::vector<std::vector<std::uint64_t>>(kLocates);
    std::vector<std::uint16_t> touched;
    std::array<std::byte, itch::kMaxPayloadSize> scratch{};
    // The seek index the columns give, against the stored one after the last record.
    std::vector<std::uint64_t> seek_want;
    // Whether every record read so far is plain, against the summary after the last.
    bool plain = true;

    [[nodiscard]] std::uint64_t take(Column column) {
        return columns[column][at[static_cast<std::size_t>(column)]++];
    }

    [[nodiscard]] std::unexpected<ItchStoreError> refuse(std::string_view name,
                                                         std::uint64_t expected,
                                                         std::uint64_t actual,
                                                         Column column) const {
        ItchStoreError error;
        error.kind = ItchStoreErrorKind::bad_value;
        error.field = name;
        error.segment = segment;
        error.column = column_info(column).name;
        error.record = summary->first_record + position_of(next_record);
        error.index = at[static_cast<std::size_t>(column)];
        error.expected = expected;
        error.actual = actual;
        return std::unexpected(error);
    }

    void append(std::uint16_t locate, std::uint64_t reference) {
        std::vector<std::uint64_t>& list = lists[locate];
        if (list.empty()) {
            touched.push_back(locate);
        }
        list.push_back(reference);
    }

    // A stored record's index in the segment.
    [[nodiscard]] std::uint64_t position_of(std::uint64_t stored) const noexcept {
        if (stored < positions.size()) {
            return positions[stored];
        }
        // A raw record: its index is the one stored_at maps back to it.
        for (std::uint64_t i = 0; i < stored_at.size(); ++i) {
            if (stored_at[i] == stored) {
                return i;
            }
        }
        return stored;
    }

    [[nodiscard]] std::expected<std::uint64_t, ItchStoreError> named(std::uint16_t locate) {
        const std::uint64_t distance = take(Column::ref_distance);
        if (distance == 0) {
            const std::uint64_t reference = take(Column::import_ref);
            append(locate, reference);
            return reference;
        }
        const std::vector<std::uint64_t>& list = lists[locate];
        if (distance > list.size()) {
            --at[static_cast<std::size_t>(Column::ref_distance)];
            return refuse("ref distance", list.size(), distance, Column::ref_distance);
        }
        return list[list.size() - distance];
    }

    [[nodiscard]] std::expected<itch::Stock, ItchStoreError> stock(std::uint16_t locate) {
        const itch::Stock* known = symbols->find(locate);
        const std::size_t exception = at[static_cast<std::size_t>(Column::stock_exception_record)];
        if (exception < columns[Column::stock_exception_record].size() &&
            columns[Column::stock_exception_record][exception] == next_record) {
            ++at[static_cast<std::size_t>(Column::stock_exception_record)];
            itch::Stock stock{};
            for (std::byte& b : stock) {
                b = byte_of(take(Column::stock_exception_value));
            }
            if (known != nullptr && *known == stock) {
                return refuse("stock exception equals symbol", 0, 1,
                              Column::stock_exception_value);
            }
            return stock;
        }
        if (known == nullptr) {
            return refuse("symbol for locate", 1, 0, Column::locate);
        }
        return *known;
    }

    [[nodiscard]] std::expected<itch::Message, ItchStoreError> rare(std::uint8_t type,
                                                                    const itch::MessageHeader& h) {
        const std::size_t size = itch::wire_size_of(std::byte{type}).value_or(kItchHeaderBytes);
        const std::span<std::byte> payload{scratch.data(), size};
        payload[0] = std::byte{type};
        itch::BigEndianWriter writer{payload};
        std::ignore = writer.write_uint<2>(1, h.stock_locate);
        std::ignore = writer.write_uint<2>(3, h.tracking_number);
        std::ignore = writer.write_uint<6>(5, h.timestamp_ns);
        for (std::size_t i = kItchHeaderBytes; i < size; ++i) {
            payload[i] = byte_of(take(Column::rare_bytes));
        }
        auto message = itch::decode(payload);
        if (!message.has_value() || std::holds_alternative<itch::RawMessage>(*message)) {
            return refuse("rare record", 0, 1, Column::rare_bytes);
        }
        return *message;
    }

    [[nodiscard]] std::expected<itch::Message, ItchStoreError> raw() {
        const std::uint64_t length = take(Column::raw_length);
        if (length == 0) {
            --at[static_cast<std::size_t>(Column::raw_length)];
            return refuse("raw length", 1, 0, Column::raw_length);
        }
        // Each raw record keeps its own bytes, which its RawMessage borrows until the next
        // segment; load() reserved room for them all, so none moves.
        const std::size_t held_at = raw_held.size();
        for (std::uint64_t i = 0; i < length; ++i) {
            raw_held.push_back(byte_of(take(Column::raw_bytes)));
        }
        const std::span<const std::byte> payload = std::span<const std::byte>{raw_held}.subspan(
            held_at, static_cast<std::size_t>(length));
        auto message = itch::decode(payload);
        if (!message.has_value() || !std::holds_alternative<itch::RawMessage>(*message)) {
            return refuse("raw record conforms", 0, 1, Column::raw_bytes);
        }
        return *message;
    }

    [[nodiscard]] std::expected<itch::Message, ItchStoreError> typed(std::uint8_t type) {
        itch::MessageHeader h;
        h.stock_locate = static_cast<std::uint16_t>(locates[next_record]);
        h.timestamp_ns = take(Column::timestamp);
        h.tracking_number = static_cast<std::uint16_t>(take(Column::tracking));
        const std::uint16_t locate = h.stock_locate;
        const auto price = [&](Column column) { return itch::Price4{static_cast<std::uint32_t>(take(column))}; };
        const auto u32 = [&](Column column) { return static_cast<std::uint32_t>(take(column)); };
        switch (type) {
            case 'A':
            case 'F': {
                const std::uint64_t reference = take(Column::creation_ref);
                append(locate, reference);
                const std::byte side = byte_of(take(Column::add_side));
                const std::uint32_t shares = u32(Column::add_shares);
                const itch::Price4 p = price(Column::add_price);
                const auto s = stock(locate);
                if (!s.has_value()) {
                    return std::unexpected(s.error());
                }
                if (type == 'A') {
                    return itch::AddOrder{.header = h, .order_reference_number = reference,
                                          .buy_sell_indicator = side, .shares = shares,
                                          .stock = *s, .price = p};
                }
                return itch::AddOrderWithMpid{.header = h, .order_reference_number = reference,
                                              .buy_sell_indicator = side, .shares = shares,
                                              .stock = *s, .price = p,
                                              .attribution = mpid_of(take(Column::mpid))};
            }
            case 'E': {
                const auto reference = named(locate);
                if (!reference.has_value()) {
                    return std::unexpected(reference.error());
                }
                const std::uint32_t shares = u32(Column::reduce_shares);
                return itch::OrderExecuted{.header = h, .order_reference_number = *reference,
                                           .executed_shares = shares,
                                           .match_number = take(Column::match)};
            }
            case 'C': {
                const auto reference = named(locate);
                if (!reference.has_value()) {
                    return std::unexpected(reference.error());
                }
                const std::uint32_t shares = u32(Column::reduce_shares);
                const std::uint64_t match = take(Column::match);
                const std::byte printable = byte_of(take(Column::printable));
                return itch::OrderExecutedWithPrice{
                    .header = h, .order_reference_number = *reference, .executed_shares = shares,
                    .match_number = match, .printable = printable,
                    .execution_price = price(Column::exec_price)};
            }
            case 'X': {
                const auto reference = named(locate);
                if (!reference.has_value()) {
                    return std::unexpected(reference.error());
                }
                return itch::OrderCancel{.header = h, .order_reference_number = *reference,
                                         .cancelled_shares = u32(Column::reduce_shares)};
            }
            case 'D': {
                const auto reference = named(locate);
                if (!reference.has_value()) {
                    return std::unexpected(reference.error());
                }
                return itch::OrderDelete{.header = h, .order_reference_number = *reference};
            }
            case 'U': {
                const auto original = named(locate);
                if (!original.has_value()) {
                    return std::unexpected(original.error());
                }
                const std::uint64_t created = take(Column::creation_ref);
                append(locate, created);
                const std::uint32_t shares = u32(Column::replace_shares);
                return itch::OrderReplace{.header = h,
                                          .original_order_reference_number = *original,
                                          .new_order_reference_number = created,
                                          .shares = shares,
                                          .price = price(Column::replace_price)};
            }
            case 'P': {
                const std::uint64_t reference = take(Column::trade_ref);
                const std::byte side = byte_of(take(Column::trade_side));
                const std::uint32_t shares = u32(Column::trade_shares);
                const itch::Price4 p = price(Column::trade_price);
                const std::uint64_t match = take(Column::match);
                const auto s = stock(locate);
                if (!s.has_value()) {
                    return std::unexpected(s.error());
                }
                return itch::Trade{.header = h, .order_reference_number = reference,
                                   .buy_sell_indicator = side, .shares = shares, .stock = *s,
                                   .price = p, .match_number = match};
            }
            case 'Q': {
                const std::uint64_t shares = take(Column::cross_shares);
                const itch::Price4 p = price(Column::cross_price);
                const std::uint64_t match = take(Column::match);
                const std::byte cross = byte_of(take(Column::cross_type));
                const auto s = stock(locate);
                if (!s.has_value()) {
                    return std::unexpected(s.error());
                }
                return itch::CrossTrade{.header = h, .shares = shares, .stock = *s,
                                        .cross_price = p, .match_number = match,
                                        .cross_type = cross};
            }
            case 'B':
                return itch::BrokenTrade{.header = h, .match_number = take(Column::match)};
            case 'I': {
                const std::uint64_t paired = take(Column::imbalance_shares);
                const std::uint64_t imbalance = take(Column::imbalance_shares);
                const itch::Price4 far = price(Column::imbalance_price);
                const itch::Price4 near = price(Column::imbalance_price);
                const itch::Price4 reference = price(Column::imbalance_price);
                const std::uint64_t flags = take(Column::imbalance_flags);
                const auto s = stock(locate);
                if (!s.has_value()) {
                    return std::unexpected(s.error());
                }
                return itch::NetOrderImbalance{
                    .header = h, .paired_shares = paired, .imbalance_shares = imbalance,
                    .imbalance_direction = byte_of(flags), .stock = *s, .far_price = far,
                    .near_price = near, .current_reference_price = reference,
                    .cross_type = byte_of(flags >> 8U),
                    .price_variation_indicator = byte_of(flags >> 16U)};
            }
            case 'V': {
                const std::uint64_t one = take(Column::mwcb_level);
                const std::uint64_t two = take(Column::mwcb_level);
                return itch::MwcbDeclineLevel{.header = h, .level_1 = itch::Price8{one},
                                              .level_2 = itch::Price8{two},
                                              .level_3 = itch::Price8{take(Column::mwcb_level)}};
            }
            default:
                return rare(type, h);
        }
    }
};

SegmentAssembler::SegmentAssembler() : state_(std::make_unique<State>()) {}
SegmentAssembler::SegmentAssembler(SegmentAssembler&&) noexcept = default;
SegmentAssembler& SegmentAssembler::operator=(SegmentAssembler&&) noexcept = default;
SegmentAssembler::~SegmentAssembler() = default;

void SegmentAssembler::fail(const ItchStoreError& error) {
    State& s = *state_;
    s.records = 0;
    s.decoded.clear();
    s.failure = error;
}

std::expected<void, ItchStoreError> SegmentAssembler::load(std::uint64_t segment,
                                                           SegmentColumns columns,
                                                           const SegmentSummary& summary,
                                                           const SymbolTable& symbols) {
    State& s = *state_;
    for (const std::uint16_t locate : s.touched) {
        s.lists[locate].clear();
    }
    s.touched.clear();
    s.at = {};
    s.segment = segment;
    s.summary = &summary;
    s.symbols = &symbols;
    s.next_record = 0;
    s.records = 0;
    s.seek_want.clear();
    s.plain = true;
    s.locates.clear();
    s.positions.clear();
    s.stored_at.clear();
    s.decoded.clear();
    s.raw_held.clear();
    s.done_decoding = false;
    s.next_out = 0;
    s.failure.reset();
    const auto refuse = [&](ItchStoreErrorKind kind, std::string_view name, Column column,
                            std::uint64_t index, std::uint64_t expected, std::uint64_t actual) {
        ItchStoreError error;
        error.kind = kind;
        error.field = name;
        error.segment = segment;
        error.column = column_info(column).name;
        error.index = index;
        error.expected = expected;
        error.actual = actual;
        return std::unexpected(error);
    };

    const std::vector<std::uint64_t>& types = columns[Column::type];
    if (types.size() != summary.records) {
        return refuse(ItchStoreErrorKind::bad_segment, "record count", Column::type, kNoIndex,
                      summary.records, types.size());
    }
    // The counts the type column fixes, then the three that follow from other columns.
    auto expected = counts_from_types(types);
    std::uint64_t typed = 0;
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (!valid_type_symbol(types[i])) {
            return refuse(ItchStoreErrorKind::bad_value, "type symbol", Column::type, i, 0,
                          types[i]);
        }
        // Stored order keeps every typed record before every raw one.
        if (types[i] != 0) {
            if (typed != i) {
                return refuse(ItchStoreErrorKind::bad_value, "typed record after a raw one",
                              Column::type, i, 0, types[i]);
            }
            ++typed;
        }
    }
    std::uint64_t raw_bytes = 0;
    for (const std::uint64_t length : columns[Column::raw_length]) {
        raw_bytes += length;
    }
    expected[static_cast<std::size_t>(Column::raw_bytes)] = raw_bytes;
    const std::uint64_t imports =
        static_cast<std::uint64_t>(std::ranges::count(columns[Column::ref_distance], 0U));
    expected[static_cast<std::size_t>(Column::import_ref)] = imports;
    expected[static_cast<std::size_t>(Column::stock_exception_record)] =
        columns[Column::stock_exception_record].size();
    expected[static_cast<std::size_t>(Column::stock_exception_value)] =
        columns[Column::stock_exception_record].size() * itch::Stock{}.size();
    // The runs are as many as the directory's ids, which are checked against the summary's
    // bitmap below.
    const std::uint64_t runs = columns[Column::locate_run].size();
    for (const Column run_column : {Column::locate_run, Column::locate_run_start,
                                    Column::locate_run_add_start, Column::locate_run_import_start}) {
        expected[static_cast<std::size_t>(run_column)] = runs;
    }
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        if (columns.values[c].size() != expected[c]) {
            return refuse(ItchStoreErrorKind::bad_segment, "column value count",
                          static_cast<Column>(c), kNoIndex, expected[c], columns.values[c].size());
        }
    }

    // The seek index's structure, and its maxima against the columns as stored, before
    // anything is regrouped.
    auto seek = parse_seek(columns[Column::seek], static_cast<std::uint32_t>(types.size()));
    if (!seek.has_value()) {
        ItchStoreError error = seek.error();
        error.segment = segment;
        return std::unexpected(error);
    }
    const std::array<std::uint64_t, kSeekMaxima> maxima = column_maxima(columns);

    // The run directory's own rules, then the locate column's indices.
    const std::uint64_t adds = columns[Column::add_side].size();
    if (auto checked = check_run_directory(
            columns[Column::locate_run], columns[Column::locate_run_start],
            columns[Column::locate_run_add_start], columns[Column::locate_run_import_start],
            typed, adds, imports);
        !checked.has_value()) {
        ItchStoreError error = checked.error();
        error.segment = segment;
        return std::unexpected(error);
    }
    auto positions = positions_from_runs(columns[Column::locate], columns[Column::locate_run_start],
                                         typed, types.size());
    if (!positions.has_value()) {
        ItchStoreError error = positions.error();
        error.segment = segment;
        return std::unexpected(error);
    }

    // The times from their residuals and bases, and the bases against the times.
    auto restored = times_from_residuals(columns[Column::timestamp], *positions,
                                         columns[Column::locate_run_start],
                                         columns[Column::time_base]);
    if (!restored.has_value()) {
        ItchStoreError error = restored.error();
        error.segment = segment;
        return std::unexpected(error);
    }
    const std::vector<std::uint64_t> bases = time_bases(*restored, *positions, types.size());
    for (std::size_t c = 0; c < bases.size(); ++c) {
        if (bases[c] != columns[Column::time_base][c]) {
            return refuse(ItchStoreErrorKind::bad_value, "time base", Column::time_base, c,
                          bases[c], columns[Column::time_base][c]);
        }
    }
    columns[Column::timestamp] = std::move(*restored);

    // The summary against the values it summarises: the first and last times are those of
    // the typed records first and last in the file.
    const std::vector<std::uint64_t>& times = columns[Column::timestamp];
    if (typed != summary.typed_records) {
        return refuse(ItchStoreErrorKind::bad_metadata, "typed records", Column::type, kNoIndex,
                      typed, summary.typed_records);
    }
    // A disagreement names the column summarised, the value's index in it, the summary's
    // value and the column's.
    if (typed > 0) {
        const auto first = std::ranges::min_element(*positions);
        const auto last = std::ranges::max_element(*positions);
        const auto first_at = static_cast<std::uint64_t>(first - positions->begin());
        const auto last_at = static_cast<std::uint64_t>(last - positions->begin());
        const auto low = std::ranges::min_element(times);
        const auto high = std::ranges::max_element(times);
        const std::array<std::tuple<std::string_view, std::uint64_t, std::uint64_t, std::uint64_t>, 4>
            agree{{
                {"first time", first_at, summary.first_time, times[first_at]},
                {"last time", last_at, summary.last_time, times[last_at]},
                {"min time", static_cast<std::uint64_t>(low - times.begin()), summary.min_time,
                 *low},
                {"max time", static_cast<std::uint64_t>(high - times.begin()), summary.max_time,
                 *high},
            }};
        for (const auto& [name, index, stated, have] : agree) {
            if (stated != have) {
                return refuse(ItchStoreErrorKind::bad_metadata, name, Column::timestamp, index,
                              stated, have);
            }
        }
    } else if (summary.first_time != 0 || summary.last_time != 0 || summary.min_time != 0 ||
               summary.max_time != 0) {
        return refuse(ItchStoreErrorKind::bad_metadata, "times without records", Column::type,
                      kNoIndex, 0, 1);
    }
    std::vector<std::byte> bitmap(kMaxLocateBitmapBytes);
    for (const std::uint64_t locate : columns[Column::locate_run]) {
        set_bit(bitmap, static_cast<std::uint16_t>(locate));
    }
    const std::vector<std::byte> locates = typed > 0 ? trimmed(bitmap) : std::vector<std::byte>{};
    if (locates != summary.locates) {
        // The first locate on one side only.
        std::uint64_t first = 0;
        const std::size_t bits = std::max(locates.size(), summary.locates.size()) * 8U;
        const auto bit_of = [](const std::vector<std::byte>& map, std::size_t b) {
            return b / 8U < map.size() &&
                   (std::to_integer<unsigned>(map[b / 8U]) >> (b % 8U) & 1U) != 0;
        };
        for (std::size_t b = 0; b < bits; ++b) {
            if (bit_of(locates, b) != bit_of(summary.locates, b)) {
                first = b;
                break;
            }
        }
        return refuse(ItchStoreErrorKind::bad_metadata, "locate bitmap", Column::locate_run, first,
                      bit_of(summary.locates, first) ? 1U : 0U, bit_of(locates, first) ? 1U : 0U);
    }

    // Stock exceptions name strictly later stored records of the segment.
    const std::vector<std::uint64_t>& exceptions = columns[Column::stock_exception_record];
    for (std::size_t i = 0; i < exceptions.size(); ++i) {
        const std::uint64_t floor = i == 0 ? 0 : exceptions[i - 1] + 1;
        if (exceptions[i] < floor || exceptions[i] >= types.size()) {
            return refuse(ItchStoreErrorKind::bad_value, "stock exception record",
                          Column::stock_exception_record, i, floor, exceptions[i]);
        }
    }

    // Stored order back for the columns grouped by type, from the type column.
    std::vector<std::uint8_t> tracking_key;
    std::vector<std::uint8_t> ref_key;
    std::vector<std::uint8_t> reduce_key;
    tracking_key.reserve(typed);
    for (const std::uint64_t type : types) {
        if (type == 0) {
            continue;
        }
        const auto symbol = static_cast<std::uint8_t>(type);
        tracking_key.push_back(symbol);
        switch (symbol) {
            case 'E':
            case 'C':
            case 'X':
                ref_key.push_back(symbol);
                reduce_key.push_back(symbol);
                break;
            case 'D':
            case 'U':
                ref_key.push_back(symbol);
                break;
            default:
                break;
        }
    }
    ungroup_by_byte(columns[Column::tracking], tracking_key);
    ungroup_by_byte(columns[Column::ref_distance], ref_key);
    ungroup_by_byte(columns[Column::reduce_shares], reduce_key);

    // Each run's adds and imports against the records, and each record's locate.
    auto run_locate = run_locates(segment, columns, typed);
    if (!run_locate.has_value()) {
        return std::unexpected(run_locate.error());
    }

    std::vector<std::uint32_t> price_key;
    std::vector<std::uint32_t> shares_key;
    price_key.reserve(columns[Column::add_side].size());
    shares_key.reserve(columns[Column::add_side].size());
    std::size_t add_index = 0;
    for (std::uint64_t k = 0; k < typed; ++k) {
        const std::uint64_t type = types[k];
        if (type == 'A' || type == 'F') {
            const auto locate = static_cast<std::uint32_t>((*run_locate)[k]);
            const auto side = static_cast<std::uint32_t>(columns[Column::add_side][add_index++]);
            price_key.push_back((locate << 8U) | side);
            shares_key.push_back(locate);
        }
    }
    ungroup_by_key(columns[Column::add_price], price_key);
    ungroup_by_key(columns[Column::add_shares], shares_key);

    // What every seek value must be, checked once the last record has been read, so that a
    // record a rule refuses is named by that rule first.
    s.seek_want = compute_seek(maxima, types, columns[Column::timestamp],
                               columns[Column::ref_distance], *positions);

    // For each index in the segment, its stored record: the typed records' from the locate
    // column, then the raw records', in order, at the indices no run names.
    const std::uint64_t records = types.size();
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    s.stored_at.assign(static_cast<std::size_t>(records), kUnset);
    for (std::uint64_t k = 0; k < typed; ++k) {
        s.stored_at[(*positions)[k]] = static_cast<std::uint32_t>(k);
    }
    std::uint64_t raw_next = typed;
    for (std::uint32_t& at : s.stored_at) {
        if (at == kUnset) {
            at = static_cast<std::uint32_t>(raw_next++);
        }
    }
    s.locates = std::move(*run_locate);
    s.positions = std::move(*positions);
    s.raw_held.reserve(static_cast<std::size_t>(raw_bytes));
    s.columns = std::move(columns);
    s.records = records;
    return {};
}

std::expected<std::optional<itch::Message>, ItchStoreError> SegmentAssembler::next() {
    State& s = *state_;
    if (s.failure.has_value()) {
        return std::unexpected(*s.failure);
    }
    // Nothing loaded yet: a segment of no records, as SegmentReader's header says (R-2).
    if (s.summary == nullptr) {
        return std::optional<itch::Message>{};
    }
    const auto fail = [&s](ItchStoreError error) {
        s.failure = error;
        return std::unexpected(error);
    };
    if (!s.done_decoding) {
        // Every record, in stored order, then the checks that need them all.
        s.decoded.reserve(static_cast<std::size_t>(s.records));
        const std::vector<std::uint64_t>& exceptions = s.columns[Column::stock_exception_record];
        for (s.next_record = 0; s.next_record < s.records; ++s.next_record) {
            const auto type = static_cast<std::uint8_t>(s.take(Column::type));
            auto message = type == 0 ? s.raw() : s.typed(type);
            if (!message.has_value()) {
                return fail(message.error());
            }
            // A derived stock takes its exception as it is read, so one still waiting at
            // this record names a record that has no stock to derive.
            const std::size_t exception =
                s.at[static_cast<std::size_t>(Column::stock_exception_record)];
            if (exception < exceptions.size() && exceptions[exception] == s.next_record) {
                return fail(s.refuse("stock exception record", 0, s.next_record,
                                     Column::stock_exception_record)
                                .error());
            }
            s.plain = s.plain && column_form_plain(*message);
            s.decoded.push_back(*message);
        }
        if (s.plain != s.summary->plain) {
            ItchStoreError error;
            error.kind = ItchStoreErrorKind::bad_metadata;
            error.field = "plain records";
            error.segment = s.segment;
            error.column = column_info(Column::type).name;
            error.expected = s.plain ? 1U : 0U;
            error.actual = s.summary->plain ? 1U : 0U;
            return fail(error);
        }
        const std::vector<std::uint64_t>& have = s.columns[Column::seek];
        for (std::size_t i = 0; i < s.seek_want.size(); ++i) {
            if (s.seek_want[i] != have[i]) {
                ItchStoreError error;
                error.kind = ItchStoreErrorKind::bad_value;
                error.field = "seek value";
                error.segment = s.segment;
                error.column = column_info(Column::seek).name;
                error.index = i;
                error.expected = s.seek_want[i];
                error.actual = have[i];
                return fail(error);
            }
        }
        s.done_decoding = true;
    }
    if (s.next_out == s.records) {
        return std::optional<itch::Message>{};
    }
    return std::optional<itch::Message>{s.decoded[s.stored_at[s.next_out++]]};
}

}  // namespace tick_store::store
