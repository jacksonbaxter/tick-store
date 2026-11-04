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
// file-order locates of the typed records and the segment's types.
struct LocateRuns {
    std::vector<std::uint64_t> gaps;
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> records;
    std::vector<std::uint64_t> adds;
};

[[nodiscard]] LocateRuns to_runs(const std::vector<std::uint64_t>& locates,
                                 const std::vector<std::uint64_t>& types) {
    std::vector<std::uint32_t> count(kLocates, 0);
    std::vector<std::uint32_t> adds(kLocates, 0);
    std::size_t typed = 0;
    for (const std::uint64_t type : types) {
        if (type == 0) {
            continue;
        }
        const std::uint64_t locate = locates[typed++];
        ++count[locate];
        if (type == 'A' || type == 'F') {
            ++adds[locate];
        }
    }
    LocateRuns runs;
    std::vector<std::uint32_t> start(kLocates, 0);
    std::uint32_t at = 0;
    for (std::size_t l = 0; l < kLocates; ++l) {
        if (count[l] == 0) {
            continue;
        }
        runs.ids.push_back(l);
        runs.records.push_back(count[l]);
        runs.adds.push_back(adds[l]);
        start[l] = at;
        at += count[l];
    }
    std::vector<std::uint32_t> positions(locates.size());
    std::vector<std::uint32_t> next = start;
    for (std::size_t i = 0; i < locates.size(); ++i) {
        positions[next[locates[i]]++] = static_cast<std::uint32_t>(i);
    }
    runs.gaps.resize(positions.size());
    for (std::size_t r = 0, first = 0; r < runs.ids.size(); ++r) {
        const std::size_t end = first + runs.records[r];
        runs.gaps[first] = positions[first];
        for (std::size_t i = first + 1; i < end; ++i) {
            runs.gaps[i] = positions[i] - positions[i - 1] - 1U;
        }
        first = end;
    }
    return runs;
}

// Puts the locate column back in file order from its runs, and checks each run's add count
// against the types.
[[nodiscard]] std::expected<void, ItchStoreError> place_locates(
    std::uint64_t segment, SegmentColumns& columns, const std::vector<std::uint64_t>& types) {
    auto placed = locates_from_runs(columns[Column::locate], columns[Column::locate_run],
                                    columns[Column::locate_run_records],
                                    columns[Column::timestamp].size());
    if (!placed.has_value()) {
        ItchStoreError error = placed.error();
        error.segment = segment;
        return std::unexpected(error);
    }
    std::vector<std::uint32_t> adds(kLocates, 0);
    std::size_t typed = 0;
    for (const std::uint64_t type : types) {
        if (type == 0) {
            continue;
        }
        const std::uint64_t locate = (*placed)[typed++];
        if (type == 'A' || type == 'F') {
            ++adds[locate];
        }
    }
    const std::vector<std::uint64_t>& ids = columns[Column::locate_run];
    const std::vector<std::uint64_t>& stated = columns[Column::locate_run_adds];
    for (std::size_t r = 0; r < ids.size(); ++r) {
        if (stated[r] != adds[ids[r]]) {
            ItchStoreError error;
            error.kind = ItchStoreErrorKind::bad_value;
            error.field = "locate run adds";
            error.segment = segment;
            error.column = column_info(Column::locate_run_adds).name;
            error.index = r;
            error.expected = adds[ids[r]];
            error.actual = stated[r];
            return std::unexpected(error);
        }
    }
    columns[Column::locate] = std::move(*placed);
    return {};
}

template <class T>
concept HasStock = requires(const T& message) {
    { message.stock } -> std::convertible_to<itch::Stock>;
};

}  // namespace

// --- SegmentBuilder ----------------------------------------------------------------------

struct SegmentBuilder::State {
    SegmentColumns columns;
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
        push(Column::locate, h.stock_locate);
        push(Column::timestamp, h.timestamp_ns);
        push(Column::tracking, h.tracking_number);
        tracking_key.push_back(symbol);
        if (typed == 0) {
            first_time = h.timestamp_ns;
            min_time = h.timestamp_ns;
            max_time = h.timestamp_ns;
        }
        last_time = h.timestamp_ns;
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
    state_->symbols = &symbols;
    std::visit(*state_, message);
    state_->plain = state_->plain && column_form_plain(message);
    ++state_->records;
    return {};
}

SegmentColumns SegmentBuilder::finish(std::uint64_t first_record, SegmentSummary& summary) {
    State& s = *state_;
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
    LocateRuns runs = to_runs(out[Column::locate], out[Column::type]);
    out[Column::locate] = std::move(runs.gaps);
    out[Column::locate_run] = std::move(runs.ids);
    out[Column::locate_run_records] = std::move(runs.records);
    out[Column::locate_run_adds] = std::move(runs.adds);
    const std::vector<std::uint64_t> ref_file_order = out[Column::ref_distance];
    out[Column::tracking] = group_by_byte(out[Column::tracking], s.tracking_key);
    out[Column::ref_distance] = group_by_byte(out[Column::ref_distance], s.ref_key);
    out[Column::reduce_shares] = group_by_byte(out[Column::reduce_shares], s.reduce_key);
    out[Column::add_price] = group_by_key(out[Column::add_price], s.price_key);
    out[Column::add_shares] = group_by_key(out[Column::add_shares], s.shares_key);
    out[Column::seek] = compute_seek(column_maxima(out), out[Column::type],
                                     out[Column::timestamp], ref_file_order);

    s.columns = SegmentColumns{};
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
    std::uint64_t next_record = 0;
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
        error.record = summary->first_record + next_record;
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
        const std::span<std::byte> payload{scratch.data(), static_cast<std::size_t>(length)};
        for (std::byte& b : payload) {
            b = byte_of(take(Column::raw_bytes));
        }
        auto message = itch::decode(payload);
        if (!message.has_value() || !std::holds_alternative<itch::RawMessage>(*message)) {
            return refuse("raw record conforms", 0, 1, Column::raw_bytes);
        }
        return *message;
    }

    [[nodiscard]] std::expected<itch::Message, ItchStoreError> typed(std::uint8_t type) {
        itch::MessageHeader h;
        h.stock_locate = static_cast<std::uint16_t>(take(Column::locate));
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
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (!valid_type_symbol(types[i])) {
            return refuse(ItchStoreErrorKind::bad_value, "type symbol", Column::type, i, 0,
                          types[i]);
        }
    }
    std::uint64_t raw_bytes = 0;
    for (const std::uint64_t length : columns[Column::raw_length]) {
        raw_bytes += length;
    }
    expected[static_cast<std::size_t>(Column::raw_bytes)] = raw_bytes;
    expected[static_cast<std::size_t>(Column::import_ref)] =
        static_cast<std::uint64_t>(std::ranges::count(columns[Column::ref_distance], 0U));
    expected[static_cast<std::size_t>(Column::stock_exception_record)] =
        columns[Column::stock_exception_record].size();
    expected[static_cast<std::size_t>(Column::stock_exception_value)] =
        columns[Column::stock_exception_record].size() * itch::Stock{}.size();
    // The runs are as many as the directory's ids, which the locates placed from them are
    // checked against the summary's bitmap below.
    const std::uint64_t runs = columns[Column::locate_run].size();
    for (const Column run_column :
         {Column::locate_run, Column::locate_run_records, Column::locate_run_adds}) {
        expected[static_cast<std::size_t>(run_column)] = runs;
    }
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        if (columns.values[c].size() != expected[c]) {
            return refuse(ItchStoreErrorKind::bad_segment, "column value count",
                          static_cast<Column>(c), kNoIndex, expected[c], columns.values[c].size());
        }
    }

    // The seek index's structure, and its maxima against the columns as stored, before
    // anything is put back in file order.
    auto seek = parse_seek(columns[Column::seek], static_cast<std::uint32_t>(types.size()));
    if (!seek.has_value()) {
        ItchStoreError error = seek.error();
        error.segment = segment;
        return std::unexpected(error);
    }
    const std::array<std::uint64_t, kSeekMaxima> maxima = column_maxima(columns);

    // The locate column back in file order, from its runs.
    if (auto placed = place_locates(segment, columns, types); !placed.has_value()) {
        return std::unexpected(placed.error());
    }

    // The summary against the values it summarises.
    const std::vector<std::uint64_t>& times = columns[Column::timestamp];
    const std::uint64_t typed = times.size();
    if (typed != summary.typed_records) {
        return refuse(ItchStoreErrorKind::bad_metadata, "typed records", Column::type, kNoIndex,
                      typed, summary.typed_records);
    }
    // A disagreement names the column summarised, the value's index in it, the summary's
    // value and the column's.
    if (typed > 0) {
        const auto low = std::ranges::min_element(times);
        const auto high = std::ranges::max_element(times);
        const std::array<std::tuple<std::string_view, std::uint64_t, std::uint64_t, std::uint64_t>, 4>
            agree{{
                {"first time", 0, summary.first_time, times.front()},
                {"last time", typed - 1, summary.last_time, times.back()},
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
    if (typed > 0) {
        for (const std::uint64_t locate : columns[Column::locate]) {
            set_bit(bitmap, static_cast<std::uint16_t>(locate));
        }
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
        return refuse(ItchStoreErrorKind::bad_metadata, "locate bitmap", Column::locate, first,
                      bit_of(summary.locates, first) ? 1U : 0U, bit_of(locates, first) ? 1U : 0U);
    }

    // Stock exceptions name strictly later records of the segment.
    const std::vector<std::uint64_t>& exceptions = columns[Column::stock_exception_record];
    for (std::size_t i = 0; i < exceptions.size(); ++i) {
        const std::uint64_t floor = i == 0 ? 0 : exceptions[i - 1] + 1;
        if (exceptions[i] < floor || exceptions[i] >= types.size()) {
            return refuse(ItchStoreErrorKind::bad_value, "stock exception record",
                          Column::stock_exception_record, i, floor, exceptions[i]);
        }
    }

    // File order back for the grouped columns, from the keys the earlier columns give.
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

    std::vector<std::uint32_t> price_key;
    std::vector<std::uint32_t> shares_key;
    price_key.reserve(columns[Column::add_side].size());
    shares_key.reserve(columns[Column::add_side].size());
    std::size_t typed_index = 0;
    std::size_t add_index = 0;
    for (const std::uint64_t type : types) {
        if (type == 0) {
            continue;
        }
        const auto locate = static_cast<std::uint32_t>(columns[Column::locate][typed_index++]);
        if (type == 'A' || type == 'F') {
            const auto side = static_cast<std::uint32_t>(columns[Column::add_side][add_index++]);
            price_key.push_back((locate << 8U) | side);
            shares_key.push_back(locate);
        }
    }
    ungroup_by_key(columns[Column::add_price], price_key);
    ungroup_by_key(columns[Column::add_shares], shares_key);

    // What every seek value must be, checked once the last record has been read, so that a
    // record a rule refuses is named by that rule first.
    s.seek_want =
        compute_seek(maxima, types, columns[Column::timestamp], columns[Column::ref_distance]);

    const std::uint64_t records = types.size();
    s.columns = std::move(columns);
    s.records = records;
    return {};
}

std::expected<std::optional<itch::Message>, ItchStoreError> SegmentAssembler::next() {
    State& s = *state_;
    if (s.next_record == s.records) {
        if (s.plain != s.summary->plain) {
            ItchStoreError error;
            error.kind = ItchStoreErrorKind::bad_metadata;
            error.field = "plain records";
            error.segment = s.segment;
            error.column = column_info(Column::type).name;
            error.expected = s.plain ? 1U : 0U;
            error.actual = s.summary->plain ? 1U : 0U;
            return std::unexpected(error);
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
                return std::unexpected(error);
            }
        }
        return std::optional<itch::Message>{};
    }
    const auto type = static_cast<std::uint8_t>(s.take(Column::type));
    auto message = type == 0 ? s.raw() : s.typed(type);
    if (!message.has_value()) {
        return std::unexpected(message.error());
    }
    // A derived stock takes its exception as it is read, so one still waiting at this
    // record names a record that has no stock to derive.
    const std::vector<std::uint64_t>& exceptions = s.columns[Column::stock_exception_record];
    const std::size_t exception = s.at[static_cast<std::size_t>(Column::stock_exception_record)];
    if (exception < exceptions.size() && exceptions[exception] == s.next_record) {
        return s.refuse("stock exception record", 0, s.next_record,
                        Column::stock_exception_record);
    }
    s.plain = s.plain && column_form_plain(*message);
    ++s.next_record;
    return std::optional<itch::Message>{*message};
}

}  // namespace tick_store::store
