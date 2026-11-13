#include "tick_store/harness/reference_eval.hpp"

#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/stream_vbyte.hpp"
#include "tick_store/harness/codec_eval.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::harness {
namespace {

constexpr std::size_t kLocates = 65536;
constexpr std::uint32_t kBlock = store::kColumnBlockValues;

enum Encoding : std::uint8_t {
    within_stock_imports,
    all_creations_imports,
    values_pfor_delta,
    values_head_delta,
    values_svb_delta,
    day_distance_checkpoint,
    day_distance_index,
    kEncodings,
};

constexpr std::array<std::string_view, kEncodings> kEncodingNames{
    "within_stock_imports", "all_creations_imports", "values_pfor_delta", "values_head_delta",
    "values_svb_delta",     "day_distance_checkpoint", "day_distance_index"};

// The first encoder error of a run. Every figure is the real codecs' output, so an encoding
// that fails fails the run rather than count short. The encoders refuse only a bad block size
// or transform, and both are constants here.
struct EncodeErrors {
    std::optional<std::string> first;

    void note(const std::expected<void, codec::CodecError>& result) {
        if (!result.has_value() && !first.has_value()) {
            first = codec::to_string(result.error());
        }
    }
};

[[nodiscard]] std::uint64_t pfor_bytes(std::span<const std::uint64_t> values,
                                       codec::IntTransform transform, EncodeErrors& errors) {
    if (values.empty()) {
        return 0;
    }
    std::vector<std::byte> out;
    errors.note(codec::encode_pfor(values, kBlock, {.transform = transform}, out));
    return out.size();
}

[[nodiscard]] std::uint64_t dict_bytes(std::span<const std::uint64_t> values, EncodeErrors& errors) {
    if (values.empty()) {
        return 0;
    }
    std::vector<std::byte> out;
    errors.note(codec::encode_dict(values, kBlock, out));
    return out.size();
}

[[nodiscard]] std::uint64_t svb_bytes(std::span<const std::uint64_t> values, EncodeErrors& errors) {
    if (values.empty()) {
        return 0;
    }
    std::vector<std::byte> out;
    errors.note(codec::encode_svb(values, kBlock, true, out));
    return out.size();
}

// The store's own coding of an import list: the smaller of PFOR delta and head delta.
[[nodiscard]] std::uint64_t import_bytes(std::span<const std::uint64_t> values,
                                         EncodeErrors& errors) {
    if (values.empty()) {
        return 0;
    }
    return std::min(pfor_bytes(values, codec::IntTransform::delta, errors),
                    pfor_bytes(values, codec::IntTransform::head_delta, errors));
}

// Distances grouped by message type, stably, as the store keeps them.
[[nodiscard]] std::uint64_t grouped_dict_bytes(const std::vector<std::uint64_t>& values,
                                               const std::vector<std::uint8_t>& types,
                                               EncodeErrors& errors) {
    std::array<std::vector<std::uint64_t>, 256> groups;
    for (std::size_t i = 0; i < values.size(); ++i) {
        groups[types[i]].push_back(values[i]);
    }
    std::vector<std::uint64_t> grouped;
    grouped.reserve(values.size());
    for (const auto& group : groups) {
        grouped.insert(grouped.end(), group.begin(), group.end());
    }
    return dict_bytes(grouped, errors);
}

// A segment's lists of known orders, by reference: per locate or all in one.
class SegmentLists {
public:
    explicit SegmentLists(bool by_locate) : by_locate_(by_locate), length_(kLocates, 0) {}

    // The distance back to `reference`, or 0 with the reference imported.
    std::uint64_t name(std::uint16_t locate, std::uint64_t reference) {
        const std::uint16_t list = by_locate_ ? locate : 0;
        const auto found = where_.find(reference);
        if (found != where_.end() && found->second.first == list) {
            return length_[list] - found->second.second;
        }
        imports_.push_back(reference);
        append(list, reference);
        return 0;
    }

    void create(std::uint16_t locate, std::uint64_t reference) {
        append(by_locate_ ? locate : 0, reference);
    }

    [[nodiscard]] const std::vector<std::uint64_t>& imports() const noexcept { return imports_; }

    void clear() {
        where_.clear();
        for (const std::uint16_t list : touched_) {
            length_[list] = 0;
        }
        touched_.clear();
        imports_.clear();
    }

private:
    void append(std::uint16_t list, std::uint64_t reference) {
        if (length_[list] == 0) {
            touched_.push_back(list);
        }
        where_[reference] = {list, length_[list]};
        ++length_[list];
    }

    bool by_locate_;
    std::vector<std::uint64_t> length_;
    std::vector<std::uint16_t> touched_;
    std::unordered_map<std::uint64_t, std::pair<std::uint16_t, std::uint64_t>> where_;
    std::vector<std::uint64_t> imports_;
};

struct LiveOrder {
    std::uint16_t locate = 0;
    std::uint64_t ordinal = 0;
    std::uint64_t shares = 0;
};

// The whole day's book, by reference, and each locate's creation count.
struct DayBook {
    std::unordered_map<std::uint64_t, LiveOrder> live;
    std::vector<std::uint64_t> created = std::vector<std::uint64_t>(kLocates, 0);
    std::uint64_t peak = 0;

    void create(std::uint16_t locate, std::uint64_t reference, std::uint64_t shares) {
        live[reference] = LiveOrder{.locate = locate, .ordinal = created[locate], .shares = shares};
        ++created[locate];
        peak = std::max<std::uint64_t>(peak, live.size());
    }

    // The distance back in the locate's creations, or nullopt.
    [[nodiscard]] std::optional<std::uint64_t> distance(std::uint16_t locate,
                                                        std::uint64_t reference) const {
        const auto found = live.find(reference);
        if (found == live.end() || found->second.locate != locate) {
            return std::nullopt;
        }
        return created[locate] - found->second.ordinal;
    }

    void reduce(std::uint64_t reference, std::uint64_t shares) {
        const auto found = live.find(reference);
        if (found == live.end()) {
            return;
        }
        if (shares >= found->second.shares) {
            live.erase(found);
        } else {
            found->second.shares -= shares;
        }
    }

    void remove(std::uint64_t reference) { live.erase(reference); }

    // A checkpoint of the live book: by locate, each order's ordinal and reference. The
    // encodings: locates (PFOR delta), live counts and creation counts (PFOR), and per order
    // the gap from the locate's creation count back to its ordinal (PFOR) and its reference in
    // creation order (the smaller of delta and head delta).
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> checkpoint_bytes(EncodeErrors& errors) const {
        std::vector<std::tuple<std::uint16_t, std::uint64_t, std::uint64_t>> orders;
        orders.reserve(live.size());
        for (const auto& [reference, order] : live) {
            orders.emplace_back(order.locate, order.ordinal, reference);
        }
        std::ranges::sort(orders);
        std::vector<std::uint64_t> locates;
        std::vector<std::uint64_t> counts;
        std::vector<std::uint64_t> creations;
        std::vector<std::uint64_t> gaps;
        std::vector<std::uint64_t> references;
        gaps.reserve(orders.size());
        references.reserve(orders.size());
        for (std::size_t i = 0; i < orders.size(); ++i) {
            const auto& [locate, ordinal, reference] = orders[i];
            if (i == 0 || std::get<0>(orders[i - 1]) != locate) {
                locates.push_back(locate);
                counts.push_back(0);
                creations.push_back(created[locate]);
            }
            ++counts.back();
            gaps.push_back(created[locate] - ordinal);
            references.push_back(reference);
        }
        const std::uint64_t bytes = pfor_bytes(locates, codec::IntTransform::delta, errors) +
                                    pfor_bytes(counts, codec::IntTransform::none, errors) +
                                    pfor_bytes(creations, codec::IntTransform::none, errors) +
                                    pfor_bytes(gaps, codec::IntTransform::none, errors) +
                                    import_bytes(references, errors);
        return {bytes, orders.size()};
    }

    // Each locate's creation count so far, for the locates with any: locates by PFOR delta,
    // counts by PFOR.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> index_bytes(EncodeErrors& errors) const {
        std::vector<std::uint64_t> locates;
        std::vector<std::uint64_t> counts;
        for (std::size_t locate = 0; locate < created.size(); ++locate) {
            if (created[locate] != 0) {
                locates.push_back(locate);
                counts.push_back(created[locate]);
            }
        }
        return {pfor_bytes(locates, codec::IntTransform::delta, errors) +
                    pfor_bytes(counts, codec::IntTransform::none, errors),
                locates.size()};
    }
};

// One segment size's running state.
struct SizeState {
    std::uint32_t segment_records = 0;
    bool checkpoints = false;
    SegmentSizeCost cost;
    SegmentLists within{true};
    SegmentLists all{false};
    std::vector<std::uint64_t> within_distance;
    std::vector<std::uint64_t> all_distance;
    std::vector<std::uint64_t> day_distance;
    std::vector<std::uint64_t> values;
    std::vector<std::uint8_t> types;

    explicit SizeState(std::uint32_t n, bool checkpoint) : segment_records(n), checkpoints(checkpoint) {
        cost.segment_records = n;
        cost.encodings.resize(kEncodings);
        for (std::size_t e = 0; e < kEncodings; ++e) {
            cost.encodings[e].name = std::string{kEncodingNames[e]};
        }
        cost.encodings[day_distance_checkpoint].measured = checkpoint;
    }

    void add_state(Encoding e, std::uint64_t bytes, std::uint64_t values_count) {
        EncodingCost& c = cost.encodings[e];
        c.state_bytes += bytes;
        c.state_values += values_count;
        c.max_segment_state_values = std::max(c.max_segment_state_values, values_count);
    }

    // At a segment's start: the state the day-wide encodings need there.
    void open_segment(const DayBook& book, EncodeErrors& errors) {
        ++cost.segments;
        if (checkpoints) {
            const auto [bytes, orders] = book.checkpoint_bytes(errors);
            add_state(day_distance_checkpoint, bytes, orders);
        }
        const auto [bytes, entries] = book.index_bytes(errors);
        add_state(day_distance_index, bytes, entries);
    }

    void close_segment(EncodeErrors& errors) {
        auto& e = cost.encodings;
        e[within_stock_imports].column_bytes += grouped_dict_bytes(within_distance, types, errors);
        add_state(within_stock_imports, import_bytes(within.imports(), errors), within.imports().size());
        e[all_creations_imports].column_bytes += grouped_dict_bytes(all_distance, types, errors);
        add_state(all_creations_imports, import_bytes(all.imports(), errors), all.imports().size());
        e[values_pfor_delta].column_bytes += pfor_bytes(values, codec::IntTransform::delta, errors);
        e[values_head_delta].column_bytes += pfor_bytes(values, codec::IntTransform::head_delta, errors);
        e[values_svb_delta].column_bytes += svb_bytes(values, errors);
        const std::uint64_t day = grouped_dict_bytes(day_distance, types, errors);
        e[day_distance_checkpoint].column_bytes += day;
        e[day_distance_index].column_bytes += day;
        cost.imports += within.imports().size();
        cost.in_segment += within_distance.size() - within.imports().size();
        within.clear();
        all.clear();
        within_distance.clear();
        all_distance.clear();
        day_distance.clear();
        values.clear();
        types.clear();
    }
};

class Walker {
public:
    explicit Walker(const ReferenceEvalOptions& options) {
        for (const std::uint32_t n : options.segment_records) {
            sizes_.emplace_back(n, std::ranges::find(options.checkpoint_segment_records, n) !=
                                       options.checkpoint_segment_records.end());
        }
    }

    void record(const itch::Message& message) {
        for (SizeState& size : sizes_) {
            if (records_ % size.segment_records == 0) {
                if (records_ != 0) {
                    size.close_segment(errors_);
                }
                size.open_segment(book_, errors_);
            }
        }
        ++records_;
        std::visit([this](const auto& m) { on(m); }, message);
    }

    std::expected<ReferenceEvalResult, std::string> finish() {
        ReferenceEvalResult result;
        for (SizeState& size : sizes_) {
            // The last segment, full or not, is still open.
            if (records_ != 0) {
                size.close_segment(errors_);
            }
            result.sizes.push_back(std::move(size.cost));
        }
        if (errors_.first.has_value()) {
            return std::unexpected(std::format("an encoding failed: {}", *errors_.first));
        }
        result.records = records_;
        result.references = references_;
        result.unresolved = unresolved_;
        result.peak_live_orders = book_.peak;
        return result;
    }

private:
    void create(std::uint16_t locate, std::uint64_t reference, std::uint64_t shares) {
        for (SizeState& size : sizes_) {
            size.within.create(locate, reference);
            size.all.create(locate, reference);
        }
        book_.create(locate, reference, shares);
    }

    void name(char type, std::uint16_t locate, std::uint64_t reference) {
        ++references_;
        const auto day = book_.distance(locate, reference);
        if (!day.has_value()) {
            ++unresolved_;
        }
        for (SizeState& size : sizes_) {
            size.within_distance.push_back(size.within.name(locate, reference));
            size.all_distance.push_back(size.all.name(locate, reference));
            // An unresolved reference would be stored whole; none occur on the pinned days.
            size.day_distance.push_back(day.value_or(0));
            size.values.push_back(reference);
            size.types.push_back(static_cast<std::uint8_t>(type));
        }
    }

    void on(const itch::AddOrder& m) {
        create(m.header.stock_locate, m.order_reference_number, m.shares);
    }
    void on(const itch::AddOrderWithMpid& m) {
        create(m.header.stock_locate, m.order_reference_number, m.shares);
    }
    void on(const itch::OrderExecuted& m) {
        name('E', m.header.stock_locate, m.order_reference_number);
        book_.reduce(m.order_reference_number, m.executed_shares);
    }
    void on(const itch::OrderExecutedWithPrice& m) {
        name('C', m.header.stock_locate, m.order_reference_number);
        book_.reduce(m.order_reference_number, m.executed_shares);
    }
    void on(const itch::OrderCancel& m) {
        name('X', m.header.stock_locate, m.order_reference_number);
        book_.reduce(m.order_reference_number, m.cancelled_shares);
    }
    void on(const itch::OrderDelete& m) {
        name('D', m.header.stock_locate, m.order_reference_number);
        book_.remove(m.order_reference_number);
    }
    void on(const itch::OrderReplace& m) {
        name('U', m.header.stock_locate, m.original_order_reference_number);
        book_.remove(m.original_order_reference_number);
        create(m.header.stock_locate, m.new_order_reference_number, m.shares);
    }
    template <class T>
    void on(const T& /*message*/) {}

    std::vector<SizeState> sizes_;
    DayBook book_;
    EncodeErrors errors_;
    std::uint64_t records_ = 0;
    std::uint64_t references_ = 0;
    std::uint64_t unresolved_ = 0;
};

constexpr std::string_view kUsage =
    "usage: tick_reference_eval <source> <output.json> --day LABEL --segment-records N[,N...]\n"
    "                           [--checkpoint-segment-records N[,N...]]\n"
    "each checkpoint size must be one of the segment sizes\n";

[[nodiscard]] std::optional<std::vector<std::uint32_t>> parse_list(std::string_view text) {
    std::vector<std::uint32_t> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view part = text.substr(0, comma);
        std::uint32_t value = 0;
        const auto [end, status] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (status != std::errc{} || end != part.data() + part.size() || value == 0 ||
            value > store::kMaxSegmentRecords) {
            return std::nullopt;
        }
        out.push_back(value);
        if (comma == std::string_view::npos) {
            break;
        }
        text = text.substr(comma + 1);
        if (text.empty()) {
            return std::nullopt;
        }
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] std::string host_label() {
    utsname name{};
    if (uname(&name) != 0) {
        return "unknown";
    }
    const std::string_view system{static_cast<const char*>(name.sysname)};
    return system == "Darwin" ? "macOS" : std::string{system};
}

}  // namespace

std::expected<ReferenceEvalResult, std::string> evaluate_references(
    std::span<const std::byte> file, const ReferenceEvalOptions& options) {
    for (const std::uint32_t n : options.checkpoint_segment_records) {
        if (std::ranges::find(options.segment_records, n) == options.segment_records.end()) {
            return std::unexpected(
                std::format("checkpoint segment size {} is not a segment size", n));
        }
    }
    Walker walker{options};
    itch::FrameCursor cursor{file};
    auto frame = cursor.next();
    for (; frame.has_value(); frame = cursor.next()) {
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            return std::unexpected(to_string(message.error()));
        }
        walker.record(*message);
    }
    if (frame.error().kind != itch::FrameStopKind::end) {
        return std::unexpected(
            std::format("the source's frames stop before its end: {}", to_string(frame.error())));
    }
    return walker.finish();
}

int run_reference_eval_cli(std::span<const std::string_view> arguments,
                           const EvalProvenance& provenance, CliOutput& output) {
    if (arguments.size() < 6 || arguments.size() % 2 != 0) {
        output.err += kUsage;
        return kExitUsageOrIo;
    }
    std::string_view day;
    ReferenceEvalOptions options;
    FlagsSeen seen;
    for (std::size_t i = 2; i + 1 < arguments.size(); i += 2) {
        if (!seen.first(arguments[i])) {
            output.err += kUsage;
            return kExitUsageOrIo;
        }
        if (arguments[i] == "--day") {
            day = arguments[i + 1];
        } else if (arguments[i] == "--segment-records") {
            const auto list = parse_list(arguments[i + 1]);
            if (!list.has_value()) {
                output.err += kUsage;
                return kExitUsageOrIo;
            }
            options.segment_records = *list;
        } else if (arguments[i] == "--checkpoint-segment-records") {
            const auto list = parse_list(arguments[i + 1]);
            if (!list.has_value()) {
                output.err += kUsage;
                return kExitUsageOrIo;
            }
            options.checkpoint_segment_records = *list;
        } else {
            output.err += kUsage;
            return kExitUsageOrIo;
        }
    }
    // A checkpoint size is measured only as one of the segment sizes.
    const bool checkpoints_are_sizes =
        std::ranges::all_of(options.checkpoint_segment_records, [&](std::uint32_t n) {
            return std::ranges::find(options.segment_records, n) != options.segment_records.end();
        });
    if (day.empty() || options.segment_records.empty() || !checkpoints_are_sizes) {
        output.err += kUsage;
        return kExitUsageOrIo;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto source = store::MappedFile::open(arguments[0]);
    if (!source.has_value()) {
        output.err += std::format("cannot read the source: {}\n", to_string(source.error()));
        return kExitUsageOrIo;
    }
    const auto result = evaluate_references(source->bytes(), options);
    if (!result.has_value()) {
        output.err += std::format("the source did not decode: {}\n", result.error());
        return kExitUsageOrIo;
    }
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_reference_eval"});
    json.field("schema", 1);
    json.field("day", day);
    json.field("source", std::filesystem::path{arguments[0]}.filename().string());
    json.field("host", host_label());
    json.field("commit",
               provenance.commit.substr(0, std::min<std::size_t>(12, provenance.commit.size())));
    json.field("tree_dirty", provenance.tree_dirty);
    json.field("compiler", provenance.compiler);
    json.field("build_type", provenance.build_type);
    json.field("block_values", std::uint64_t{kBlock});
    json.field("records", result->records);
    json.field("references", result->references);
    json.field("unresolved", result->unresolved);
    json.field("peak_live_orders", result->peak_live_orders);
    json.key("sizes");
    json.begin_array();
    for (const SegmentSizeCost& size : result->sizes) {
        json.begin_object();
        json.field("segment_records", std::uint64_t{size.segment_records});
        json.field("segments", size.segments);
        json.field("in_segment", size.in_segment);
        json.field("imports", size.imports);
        json.key("encodings");
        json.begin_array();
        for (const EncodingCost& cost : size.encodings) {
            json.begin_object();
            json.field("name", std::string_view{cost.name});
            json.field("measured", cost.measured);
            json.field("column_bytes", cost.column_bytes);
            json.field("state_bytes", cost.state_bytes);
            // An encoding not measured has its column alone, which is no cost.
            const std::uint64_t total = cost.column_bytes + cost.state_bytes;
            std::optional<double> bits;
            if (cost.measured) {
                bits = result->references == 0 ? 0.0
                                               : static_cast<double>(total) * 8.0 /
                                                     static_cast<double>(result->references);
            }
            json.field("total_bytes", cost.measured ? std::optional{total} : std::nullopt);
            json.field("state_values", cost.state_values);
            json.field("max_segment_state_values", cost.max_segment_state_values);
            json.field("bits_per_reference", bits);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.field("wall_seconds", std::chrono::duration<double>{Clock::now() - start}.count());
    json.end_object();
    std::ofstream out{std::filesystem::path{arguments[1]}, std::ios::binary};
    out << json.finish();
    // Closed before the check: a JSON this small is written only when the buffer is flushed.
    out.close();
    if (!out) {
        output.err += "cannot write the output\n";
        return kExitUsageOrIo;
    }
    for (const SegmentSizeCost& size : result->sizes) {
        for (const EncodingCost& cost : size.encodings) {
            output.out += std::format("N={} {}: {} column + {} state bytes{}\n",
                                      size.segment_records, cost.name, cost.column_bytes,
                                      cost.state_bytes, cost.measured ? "" : " (not measured)");
        }
    }
    return kExitPass;
}

}  // namespace tick_store::harness
