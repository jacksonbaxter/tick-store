#pragma once

// The census of one ITCH file: the small aggregate statistics no source publishes,
// which the codec choices of Phase 3 rest on. It walks a mapped file once, and all it
// keeps is counters, histograms and one fixed entry per stock locate, so nothing grows
// with the number of records.
//
// A record that took the raw path has no fields to read. It is counted by type and
// length, and nothing else here sees it.

#include "tick_store/itch/message.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::harness {

// Differences counted by size in powers of two, with the signs apart. Positive bucket
// k holds [2^k, 2^(k+1)), negative bucket k holds the same range below zero.
class Log2Histogram {
public:
    static constexpr std::size_t kBuckets = 64;

    constexpr void add(std::int64_t value) noexcept {
        if (value > 0) {
            ++positive_[bucket(static_cast<std::uint64_t>(value))];
        } else if (value < 0) {
            ++negative_[bucket(std::uint64_t{0} - static_cast<std::uint64_t>(value))];
        } else {
            ++zero_;
        }
    }

    // current - previous, for values too wide for a signed difference.
    constexpr void add_difference(std::uint64_t current, std::uint64_t previous) noexcept {
        if (current > previous) {
            ++positive_[bucket(current - previous)];
        } else if (current < previous) {
            ++negative_[bucket(previous - current)];
        } else {
            ++zero_;
        }
    }

    [[nodiscard]] constexpr std::uint64_t zero() const noexcept { return zero_; }

    [[nodiscard]] constexpr const std::array<std::uint64_t, kBuckets>& positive() const noexcept {
        return positive_;
    }

    [[nodiscard]] constexpr const std::array<std::uint64_t, kBuckets>& negative() const noexcept {
        return negative_;
    }

    [[nodiscard]] std::uint64_t total() const noexcept;
    [[nodiscard]] std::uint64_t positive_total() const noexcept;
    [[nodiscard]] std::uint64_t negative_total() const noexcept;

    // Mean significant bits of the magnitude. Zero takes none.
    [[nodiscard]] double mean_bit_width() const noexcept;

    // Bits per value of an ideal code for this histogram: the entropy of (sign, bucket)
    // plus the k low bits a value of bucket k still needs. A fair way to compare two
    // transforms of one column, and not a codec's real size.
    [[nodiscard]] double ideal_code_bits() const noexcept;

private:
    [[nodiscard]] static constexpr std::size_t bucket(std::uint64_t magnitude) noexcept {
        return static_cast<std::size_t>(std::bit_width(magnitude)) - 1;
    }

    std::uint64_t zero_ = 0;
    std::array<std::uint64_t, kBuckets> positive_{};
    std::array<std::uint64_t, kBuckets> negative_{};
};

struct TypeCensus {
    std::uint64_t frames = 0;
    // Prefix included.
    std::uint64_t frame_bytes = 0;
    // A known type at any length but the specification's.
    std::uint64_t off_spec_length = 0;
    std::uint64_t tracking_nonzero = 0;
    // Records of this type that carry a stock field, and those that disagree with the
    // directory's stock for their locate.
    std::uint64_t stock_checked = 0;
    std::uint64_t stock_mismatches = 0;
    // Records whose locate no `R` has named yet.
    std::uint64_t locate_not_in_directory = 0;
};

// One stream of timestamps, in the order the file holds them. `decreases` is the number
// of steps backwards, so 0 means non-decreasing.
struct TimestampCensus {
    std::uint64_t values = 0;
    // Steps of zero: a value repeating the nanosecond before it.
    std::uint64_t repeats = 0;
    std::uint64_t decreases = 0;
    std::uint64_t largest_decrease_ns = 0;
    Log2Histogram delta;
    Log2Histogram delta_of_delta;
};

// Every field of 4 implied decimals. MWCB levels have 8, so a multiple of 100 means
// something else there, and they are not counted.
enum class PriceField : std::uint8_t {
    add_order,
    add_order_mpid,
    order_replace,
    executed_with_price,
    trade,
    cross_trade,
    ipo,
    collar_reference,
    collar_upper,
    collar_lower,
    imbalance_far,
    imbalance_near,
    imbalance_reference,
    listing_minimum,
    listing_maximum,
    listing_near_execution,
    listing_lower_collar,
    listing_upper_collar,
};

inline constexpr std::size_t kPriceFieldCount = 18;

[[nodiscard]] std::string_view name_of(PriceField field) noexcept;

// Zero means "no price" in several of these fields, so it is counted on its own and the
// divisibility counters skip it.
struct PriceCensus {
    std::uint64_t values = 0;
    std::uint64_t zero = 0;
    std::uint64_t multiples_of_100 = 0;
    std::uint64_t multiples_of_50 = 0;
    std::uint64_t multiples_of_10 = 0;
    // Below $1.00, where the tick is $0.0001 and a whole-cent price is not required.
    std::uint64_t below_one_dollar = 0;
    std::uint64_t below_one_dollar_multiples_of_100 = 0;
    // Non-zero range.
    std::uint32_t minimum = 0;
    std::uint32_t maximum = 0;
};

enum class ReferenceField : std::uint8_t {
    add_order,
    add_order_mpid,
    executed,
    executed_with_price,
    cancel,
    deleted,
    replace_original,
    replace_new,
    trade,
};

inline constexpr std::size_t kReferenceFieldCount = 9;

[[nodiscard]] std::string_view name_of(ReferenceField field) noexcept;

struct ReferenceCensus {
    std::uint64_t values = 0;
    std::uint64_t zero = 0;
    // Against the same field's previous value.
    Log2Histogram delta_previous;
    // Against the largest reference seen so far in any field.
    Log2Histogram delta_running_maximum;
};

struct LocateCensus {
    // Locates with at least one record.
    std::uint64_t seen = 0;
    // Locates with at least one `R`.
    std::uint64_t in_directory = 0;
    // An `R` for a locate that already had one with a different stock.
    std::uint64_t directory_changes = 0;
    // Stocks that the directory gives more than one locate.
    std::uint64_t stocks_with_several_locates = 0;
    // Locates whose own timestamps go backwards at least once.
    std::uint64_t locates_with_a_decrease = 0;
};

struct Census {
    std::uint64_t file_bytes = 0;
    std::uint64_t frames = 0;
    std::uint64_t frame_bytes = 0;
    std::uint64_t tail_bytes = 0;
    std::string stop;
    std::uint64_t tracking_checked = 0;
    std::uint64_t tracking_nonzero = 0;
    std::uint64_t first_timestamp_ns = 0;
    std::uint64_t last_timestamp_ns = 0;
    std::array<TypeCensus, 256> types{};
    TimestampCensus timestamps_global;
    // The sum over every locate's own stream.
    TimestampCensus timestamps_per_locate;
    std::array<PriceCensus, kPriceFieldCount> prices{};
    std::array<ReferenceCensus, kReferenceFieldCount> references{};
    LocateCensus locates;

    [[nodiscard]] const PriceCensus& price(PriceField field) const noexcept {
        return prices[static_cast<std::size_t>(field)];
    }

    [[nodiscard]] const ReferenceCensus& reference(ReferenceField field) const noexcept {
        return references[static_cast<std::size_t>(field)];
    }

    [[nodiscard]] const TypeCensus& type(char code) const noexcept {
        return types[static_cast<unsigned char>(code)];
    }
};

// `file` is a whole ITCH file.
[[nodiscard]] Census take_census(std::span<const std::byte> file);

// The census as JSON. It holds no paths, because none reach it.
[[nodiscard]] std::string to_json(const Census& census, double wall_seconds);

}  // namespace tick_store::harness
