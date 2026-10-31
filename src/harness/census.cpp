#include "tick_store/harness/census.hpp"

#include "tick_store/harness/json_writer.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tick_store::harness {
namespace {

constexpr std::size_t kLocateCount = std::size_t{1} << 16U;
constexpr std::uint32_t kOneDollar = 10'000;

constexpr std::array<std::string_view, kPriceFieldCount> kPriceNames{
    "A.price",
    "F.price",
    "U.price",
    "C.execution_price",
    "P.price",
    "Q.cross_price",
    "K.ipo_price",
    "J.auction_collar_reference_price",
    "J.upper_auction_collar_price",
    "J.lower_auction_collar_price",
    "I.far_price",
    "I.near_price",
    "I.current_reference_price",
    "O.minimum_allowable_price",
    "O.maximum_allowable_price",
    "O.near_execution_price",
    "O.lower_price_range_collar",
    "O.upper_price_range_collar",
};

constexpr std::array<std::string_view, kReferenceFieldCount> kReferenceNames{
    "A.order_reference_number",
    "F.order_reference_number",
    "E.order_reference_number",
    "C.order_reference_number",
    "X.order_reference_number",
    "D.order_reference_number",
    "U.original_order_reference_number",
    "U.new_order_reference_number",
    "P.order_reference_number",
};

// Where one stream of timestamps has got to.
struct TimestampStream {
    std::uint64_t previous = 0;
    std::int64_t previous_delta = 0;
    // How many values have gone by, capped at the 2 a delta of deltas needs.
    std::uint8_t seen = 0;
    bool decreased = false;

    void observe(std::uint64_t timestamp, TimestampCensus& census) noexcept {
        ++census.values;
        if (seen > 0) {
            const auto delta =
                static_cast<std::int64_t>(timestamp) - static_cast<std::int64_t>(previous);
            census.delta.add(delta);
            if (delta == 0) {
                ++census.repeats;
            } else if (delta < 0) {
                ++census.decreases;
                decreased = true;
                census.largest_decrease_ns =
                    std::max(census.largest_decrease_ns, previous - timestamp);
            }
            if (seen > 1) {
                census.delta_of_delta.add(delta - previous_delta);
            }
            previous_delta = delta;
        }
        seen = seen > 1 ? seen : static_cast<std::uint8_t>(seen + 1);
        previous = timestamp;
    }
};

// One fixed entry per locate. Too big for the stack, and the same size for any file.
struct LocateTables {
    std::array<TimestampStream, kLocateCount> timestamps{};
    std::array<itch::Stock, kLocateCount> stock{};
    std::bitset<kLocateCount> seen;
    std::bitset<kLocateCount> named;
    std::bitset<kLocateCount> in_directory;
};

class Walk {
public:
    explicit Walk(Census& census) : census_(census), tables_(std::make_unique<LocateTables>()) {}

    void observe(const itch::Frame& frame) {
        const std::size_t bytes = itch::kFramePrefixSize + frame.payload.size();
        TypeCensus& type = census_.types[std::to_integer<unsigned char>(frame.payload[0])];
        ++census_.frames;
        census_.frame_bytes += bytes;
        ++type.frames;
        type.frame_bytes += bytes;

        // The cursor returns no empty payload, and only that fails to decode.
        const auto message = itch::decode(frame.payload);
        if (!message.has_value()) [[unlikely]] {
            return;
        }
        std::visit(
            [&]<class M>(const M& typed) {
                if constexpr (std::same_as<M, itch::RawMessage>) {
                    if (typed.reason == itch::RawReason::wrong_length) {
                        ++type.off_spec_length;
                    }
                } else {
                    observe_header(typed.header, type);
                    if constexpr (requires { typed.stock; }) {
                        observe_stock(typed.header.stock_locate, typed.stock, type);
                    }
                    observe_fields(typed);
                }
            },
            *message);
    }

    void finish() {
        LocateCensus& locates = census_.locates;
        locates.seen = tables_->seen.count();
        locates.in_directory = tables_->in_directory.count();
        locates.locates_with_a_decrease = static_cast<std::uint64_t>(
            std::ranges::count_if(tables_->timestamps, &TimestampStream::decreased));

        std::vector<itch::Stock> stocks;
        stocks.reserve(locates.in_directory);
        for (std::size_t locate = 0; locate < kLocateCount; ++locate) {
            if (tables_->in_directory[locate]) {
                stocks.push_back(tables_->stock[locate]);
            }
        }
        std::ranges::sort(stocks);
        for (auto run = stocks.begin(); run != stocks.end();) {
            const auto end = std::ranges::upper_bound(run, stocks.end(), *run);
            locates.stocks_with_several_locates += std::distance(run, end) > 1 ? 1U : 0U;
            run = end;
        }
    }

private:
    void observe_header(const itch::MessageHeader& header, TypeCensus& type) noexcept {
        ++census_.tracking_checked;
        if (header.tracking_number != 0) {
            ++census_.tracking_nonzero;
            ++type.tracking_nonzero;
        }
        if (census_.timestamps_global.values == 0) {
            census_.first_timestamp_ns = header.timestamp_ns;
        }
        census_.last_timestamp_ns = header.timestamp_ns;
        global_timestamps_.observe(header.timestamp_ns, census_.timestamps_global);
        tables_->timestamps[header.stock_locate].observe(header.timestamp_ns,
                                                         census_.timestamps_per_locate);
        tables_->seen[header.stock_locate] = true;
        if (!tables_->in_directory[header.stock_locate]) {
            ++type.locate_not_in_directory;
        }
    }

    // The first stock named for a locate has nothing to be compared with, and every
    // later one is held to it. That covers a record arriving before its locate's `R`,
    // because the `R` is then the record that gets compared.
    void observe_stock(std::uint16_t locate, const itch::Stock& stock, TypeCensus& type) noexcept {
        if (!tables_->named[locate]) {
            tables_->named[locate] = true;
            tables_->stock[locate] = stock;
            return;
        }
        ++type.stock_checked;
        if (tables_->stock[locate] != stock) {
            ++type.stock_mismatches;
        }
    }

    void observe_price(PriceField field, itch::Price4 price) noexcept {
        PriceCensus& census = census_.prices[static_cast<std::size_t>(field)];
        const auto value = static_cast<std::uint32_t>(price);
        ++census.values;
        if (value == 0) {
            ++census.zero;
            return;
        }
        census.minimum = census.minimum == 0 ? value : std::min(census.minimum, value);
        census.maximum = std::max(census.maximum, value);
        const bool whole_cent = value % 100U == 0;
        census.multiples_of_100 += whole_cent ? 1U : 0U;
        census.multiples_of_50 += value % 50U == 0 ? 1U : 0U;
        census.multiples_of_10 += value % 10U == 0 ? 1U : 0U;
        if (value < kOneDollar) {
            ++census.below_one_dollar;
            census.below_one_dollar_multiples_of_100 += whole_cent ? 1U : 0U;
        }
    }

    void observe_reference(ReferenceField field, std::uint64_t reference) noexcept {
        const auto index = static_cast<std::size_t>(field);
        ReferenceCensus& census = census_.references[index];
        if (census.values > 0) {
            census.delta_previous.add_difference(reference, previous_reference_[index]);
        }
        ++census.values;
        census.zero += reference == 0 ? 1U : 0U;
        previous_reference_[index] = reference;
        if (any_reference_) {
            census.delta_running_maximum.add_difference(reference, largest_reference_);
        }
        any_reference_ = true;
        largest_reference_ = std::max(largest_reference_, reference);
    }

    // Records with nothing beyond the header and the stock.
    template <class M>
    void observe_fields(const M& /*message*/) noexcept {}

    void observe_fields(const itch::StockDirectory& message) noexcept {
        const std::size_t locate = message.header.stock_locate;
        if (tables_->in_directory[locate] && tables_->stock[locate] != message.stock) {
            ++census_.locates.directory_changes;
        }
        tables_->stock[locate] = message.stock;
        tables_->in_directory[locate] = true;
    }

    void observe_fields(const itch::AddOrder& message) noexcept {
        observe_price(PriceField::add_order, message.price);
        observe_reference(ReferenceField::add_order, message.order_reference_number);
    }

    void observe_fields(const itch::AddOrderWithMpid& message) noexcept {
        observe_price(PriceField::add_order_mpid, message.price);
        observe_reference(ReferenceField::add_order_mpid, message.order_reference_number);
    }

    void observe_fields(const itch::OrderExecuted& message) noexcept {
        observe_reference(ReferenceField::executed, message.order_reference_number);
    }

    void observe_fields(const itch::OrderExecutedWithPrice& message) noexcept {
        observe_price(PriceField::executed_with_price, message.execution_price);
        observe_reference(ReferenceField::executed_with_price, message.order_reference_number);
    }

    void observe_fields(const itch::OrderCancel& message) noexcept {
        observe_reference(ReferenceField::cancel, message.order_reference_number);
    }

    void observe_fields(const itch::OrderDelete& message) noexcept {
        observe_reference(ReferenceField::deleted, message.order_reference_number);
    }

    void observe_fields(const itch::OrderReplace& message) noexcept {
        observe_price(PriceField::order_replace, message.price);
        observe_reference(ReferenceField::replace_original,
                          message.original_order_reference_number);
        observe_reference(ReferenceField::replace_new, message.new_order_reference_number);
    }

    void observe_fields(const itch::Trade& message) noexcept {
        observe_price(PriceField::trade, message.price);
        observe_reference(ReferenceField::trade, message.order_reference_number);
    }

    void observe_fields(const itch::CrossTrade& message) noexcept {
        observe_price(PriceField::cross_trade, message.cross_price);
    }

    void observe_fields(const itch::IpoQuotingPeriodUpdate& message) noexcept {
        observe_price(PriceField::ipo, message.ipo_price);
    }

    void observe_fields(const itch::LuldAuctionCollar& message) noexcept {
        observe_price(PriceField::collar_reference, message.auction_collar_reference_price);
        observe_price(PriceField::collar_upper, message.upper_auction_collar_price);
        observe_price(PriceField::collar_lower, message.lower_auction_collar_price);
    }

    void observe_fields(const itch::NetOrderImbalance& message) noexcept {
        observe_price(PriceField::imbalance_far, message.far_price);
        observe_price(PriceField::imbalance_near, message.near_price);
        observe_price(PriceField::imbalance_reference, message.current_reference_price);
    }

    void observe_fields(const itch::DirectListingPriceDiscovery& message) noexcept {
        observe_price(PriceField::listing_minimum, message.minimum_allowable_price);
        observe_price(PriceField::listing_maximum, message.maximum_allowable_price);
        observe_price(PriceField::listing_near_execution, message.near_execution_price);
        observe_price(PriceField::listing_lower_collar, message.lower_price_range_collar);
        observe_price(PriceField::listing_upper_collar, message.upper_price_range_collar);
    }

    Census& census_;
    std::unique_ptr<LocateTables> tables_;
    TimestampStream global_timestamps_;
    std::array<std::uint64_t, kReferenceFieldCount> previous_reference_{};
    std::uint64_t largest_reference_ = 0;
    bool any_reference_ = false;
};

[[nodiscard]] double share(std::uint64_t part, std::uint64_t whole) noexcept {
    return whole == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(whole);
}

template <class Visit>
void for_each_bucket(const Log2Histogram& histogram, const Visit& visit) {
    if (histogram.zero() != 0) {
        visit(histogram.zero(), 0U);
    }
    for (const auto* buckets : {&histogram.positive(), &histogram.negative()}) {
        for (std::size_t k = 0; k < Log2Histogram::kBuckets; ++k) {
            if ((*buckets)[k] != 0) {
                visit((*buckets)[k], static_cast<unsigned>(k));
            }
        }
    }
}

void write_buckets(JsonWriter& json, std::string_view name,
                   const std::array<std::uint64_t, Log2Histogram::kBuckets>& buckets) {
    json.key(name);
    json.begin_object();
    for (std::size_t k = 0; k < buckets.size(); ++k) {
        if (buckets[k] != 0) {
            json.field(std::format("2^{}", k), buckets[k]);
        }
    }
    json.end_object();
}

void write_histogram(JsonWriter& json, std::string_view name, const Log2Histogram& histogram) {
    json.key(name);
    json.begin_object();
    json.field("values", histogram.total());
    json.field("mean_bit_width", histogram.mean_bit_width());
    json.field("ideal_code_bits", histogram.ideal_code_bits());
    json.field("zero", histogram.zero());
    write_buckets(json, "positive", histogram.positive());
    write_buckets(json, "negative", histogram.negative());
    json.end_object();
}

void write_file_section(JsonWriter& json, const Census& census) {
    json.key("file");
    json.begin_object();
    json.field("bytes", census.file_bytes);
    json.field("frames", census.frames);
    json.field("frame_bytes", census.frame_bytes);
    json.field("tail_bytes", census.tail_bytes);
    json.field("frames_and_tail_equal_file_size",
               census.frame_bytes + census.tail_bytes == census.file_bytes);
    json.field("stop", std::string_view{census.stop});
    json.field("zero_length_terminator",
               census.stop == itch::to_string(itch::FrameStopKind::zero_length));
    json.end_object();
}

void write_types(JsonWriter& json, const Census& census) {
    json.key("types");
    json.begin_array();
    std::uint64_t unknown_frames = 0;
    std::uint64_t off_spec_frames = 0;
    for (std::size_t code = 0; code < census.types.size(); ++code) {
        const TypeCensus& type = census.types[code];
        if (type.frames == 0) {
            continue;
        }
        const auto length = itch::wire_size_of(static_cast<std::byte>(code));
        unknown_frames += length.has_value() ? 0U : type.frames;
        off_spec_frames += type.off_spec_length;
        json.begin_object();
        json.field("type", std::string(1, static_cast<char>(code)));
        json.field("type_byte", code);
        json.field("frames", type.frames);
        json.field("frame_bytes", type.frame_bytes);
        json.field("share_of_frames", share(type.frames, census.frames));
        json.field("share_of_bytes", share(type.frame_bytes, census.file_bytes));
        json.field("spec_payload_length", length);
        json.field("off_spec_length", type.off_spec_length);
        json.field("tracking_nonzero", type.tracking_nonzero);
        json.field("stock_checked", type.stock_checked);
        json.field("stock_mismatches", type.stock_mismatches);
        json.field("locate_not_in_directory", type.locate_not_in_directory);
        json.end_object();
    }
    json.end_array();
    json.field("unknown_type_frames", unknown_frames);
    json.field("off_spec_length_frames", off_spec_frames);
}

void write_timestamps(JsonWriter& json, std::string_view name, const TimestampCensus& census) {
    json.key(name);
    json.begin_object();
    json.field("values", census.values);
    json.field("non_decreasing", census.decreases == 0);
    json.field("decreases", census.decreases);
    json.field("largest_decrease_ns", census.largest_decrease_ns);
    json.field("repeats_previous_ns", census.repeats);
    json.field("share_repeating_previous_ns", share(census.repeats, census.delta.total()));
    write_histogram(json, "delta", census.delta);
    write_histogram(json, "delta_of_delta", census.delta_of_delta);
    json.end_object();
}

void write_prices(JsonWriter& json, const Census& census) {
    json.key("prices");
    json.begin_object();
    for (std::size_t i = 0; i < census.prices.size(); ++i) {
        const PriceCensus& price = census.prices[i];
        if (price.values == 0) {
            continue;
        }
        const std::uint64_t nonzero = price.values - price.zero;
        json.key(kPriceNames[i]);
        json.begin_object();
        json.field("values", price.values);
        json.field("zero", price.zero);
        json.field("multiples_of_100", price.multiples_of_100);
        json.field("multiples_of_50", price.multiples_of_50);
        json.field("multiples_of_10", price.multiples_of_10);
        json.field("share_of_nonzero_multiple_of_100", share(price.multiples_of_100, nonzero));
        json.field("below_one_dollar", price.below_one_dollar);
        json.field("below_one_dollar_multiples_of_100", price.below_one_dollar_multiples_of_100);
        json.field("minimum_nonzero", price.minimum);
        json.field("maximum", price.maximum);
        json.end_object();
    }
    json.end_object();
}

void write_references(JsonWriter& json, const Census& census) {
    json.key("order_references");
    json.begin_object();
    for (std::size_t i = 0; i < census.references.size(); ++i) {
        const ReferenceCensus& reference = census.references[i];
        if (reference.values == 0) {
            continue;
        }
        json.key(kReferenceNames[i]);
        json.begin_object();
        json.field("values", reference.values);
        json.field("zero", reference.zero);
        write_histogram(json, "delta_previous_same_field", reference.delta_previous);
        write_histogram(json, "delta_running_maximum", reference.delta_running_maximum);
        json.end_object();
    }
    json.end_object();
}

}  // namespace

std::uint64_t Log2Histogram::positive_total() const noexcept {
    return std::accumulate(positive_.begin(), positive_.end(), std::uint64_t{0});
}

std::uint64_t Log2Histogram::negative_total() const noexcept {
    return std::accumulate(negative_.begin(), negative_.end(), std::uint64_t{0});
}

std::uint64_t Log2Histogram::total() const noexcept {
    return zero_ + positive_total() + negative_total();
}

double Log2Histogram::mean_bit_width() const noexcept {
    const std::uint64_t values = total();
    if (values == 0) {
        return 0.0;
    }
    double bits = 0.0;
    for (std::size_t k = 0; k < kBuckets; ++k) {
        const auto count = static_cast<double>(positive_[k] + negative_[k]);
        bits += count * static_cast<double>(k + 1);
    }
    return bits / static_cast<double>(values);
}

double Log2Histogram::ideal_code_bits() const noexcept {
    const std::uint64_t values = total();
    if (values == 0) {
        return 0.0;
    }
    double bits = 0.0;
    for_each_bucket(*this, [&](std::uint64_t count, unsigned k) {
        const double p = static_cast<double>(count) / static_cast<double>(values);
        bits += p * (static_cast<double>(k) - std::log2(p));
    });
    return bits;
}

std::string_view name_of(PriceField field) noexcept {
    return kPriceNames[static_cast<std::size_t>(field)];
}

std::string_view name_of(ReferenceField field) noexcept {
    return kReferenceNames[static_cast<std::size_t>(field)];
}

Census take_census(std::span<const std::byte> file) {
    Census census;
    census.file_bytes = file.size();
    Walk walk{census};
    itch::FrameCursor cursor{file};
    auto frame = cursor.next();
    for (; frame.has_value(); frame = cursor.next()) {
        walk.observe(*frame);
    }
    census.stop = itch::to_string(frame.error().kind);
    census.tail_bytes = cursor.remaining().size();
    walk.finish();
    return census;
}

std::string to_json(const Census& census, double wall_seconds) {
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_census"});
    json.field("schema", 1);
    write_file_section(json, census);
    write_types(json, census);

    json.key("tracking_number");
    json.begin_object();
    json.field("checked", census.tracking_checked);
    json.field("nonzero", census.tracking_nonzero);
    json.field("share_nonzero", share(census.tracking_nonzero, census.tracking_checked));
    json.end_object();

    json.key("timestamps");
    json.begin_object();
    json.field("first_ns", census.first_timestamp_ns);
    json.field("last_ns", census.last_timestamp_ns);
    write_timestamps(json, "global", census.timestamps_global);
    write_timestamps(json, "per_locate", census.timestamps_per_locate);
    json.end_object();

    write_prices(json, census);
    write_references(json, census);

    json.key("stock_locate");
    json.begin_object();
    json.field("locates_seen", census.locates.seen);
    json.field("locates_in_directory", census.locates.in_directory);
    json.field("directory_changes", census.locates.directory_changes);
    json.field("stocks_with_several_locates", census.locates.stocks_with_several_locates);
    json.field("locates_with_a_timestamp_decrease", census.locates.locates_with_a_decrease);
    std::uint64_t checked = 0;
    std::uint64_t mismatches = 0;
    for (const TypeCensus& type : census.types) {
        checked += type.stock_checked;
        mismatches += type.stock_mismatches;
    }
    json.field("stock_checked", checked);
    json.field("stock_mismatches", mismatches);
    json.end_object();

    json.field("wall_seconds", wall_seconds);
    json.end_object();
    return json.finish();
}

}  // namespace tick_store::harness
