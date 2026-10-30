#pragma once

// Wire layout of every ITCH 5.0 message, as one table per type. decode and encode
// are both generated from these tables, so they cannot disagree.
//
// The compile-time checks at the bottom prove each table is contiguous from the
// type byte to the type's stated length, leaving no byte uncovered. The hand-built
// golden fixtures in tests/unit check the tables against the specification.

#include "tick_store/itch/message.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace tick_store::itch::detail {

// How many bytes a field type occupies on the wire.
template <class V>
inline constexpr std::size_t natural_width = sizeof(V);

template <std::size_t N>
inline constexpr std::size_t natural_width<std::array<std::byte, N>> = N;

// A field located by a pointer to its member of the message struct.
template <auto Member>
struct BodyField;

template <class Owner, class V, V Owner::* Member>
struct BodyField<Member> {
    using Value = V;
    static constexpr std::size_t width = natural_width<V>;

    std::string_view name;
    std::size_t offset;

    template <class M>
    [[nodiscard]] static constexpr auto& of(M& message) noexcept {
        return message.*Member;
    }
};

// A field of the common header, which every message holds in `header`. Width is
// explicit because the timestamp is 6 bytes held in a uint64_t.
template <auto Member, std::size_t Width>
struct HeaderField;

template <class V, V MessageHeader::* Member, std::size_t Width>
struct HeaderField<Member, Width> {
    using Value = V;
    static constexpr std::size_t width = Width;

    std::string_view name;
    std::size_t offset;

    template <class M>
    [[nodiscard]] static constexpr auto& of(M& message) noexcept {
        return message.header.*Member;
    }
};

template <auto Member>
[[nodiscard]] consteval BodyField<Member> field(std::string_view name, std::size_t offset) {
    return {.name = name, .offset = offset};
}

// The Message Type byte comes first in every message, and the dispatch in
// decode_strict has already read it, so no table lists it.
inline constexpr std::size_t kTypeByteSize = 1;

// Offset 11 is where the fields after the common header begin in all 23 types.
inline constexpr auto kHeaderFields = std::tuple{
    HeaderField<&MessageHeader::stock_locate, 2>{.name = "stock_locate", .offset = 1},
    HeaderField<&MessageHeader::tracking_number, 2>{.name = "tracking_number", .offset = 3},
    HeaderField<&MessageHeader::timestamp_ns, 6>{.name = "timestamp_ns", .offset = 5},
};

// The fields after the header, in wire order. Specialised once per type.
template <class T>
inline constexpr auto kBodyFields = std::tuple{};

template <>
inline constexpr auto kBodyFields<SystemEvent> = std::tuple{
    field<&SystemEvent::event_code>("event_code", 11),
};

template <>
inline constexpr auto kBodyFields<StockDirectory> = std::tuple{
    field<&StockDirectory::stock>("stock", 11),
    field<&StockDirectory::market_category>("market_category", 19),
    field<&StockDirectory::financial_status_indicator>("financial_status_indicator", 20),
    field<&StockDirectory::round_lot_size>("round_lot_size", 21),
    field<&StockDirectory::round_lots_only>("round_lots_only", 25),
    field<&StockDirectory::issue_classification>("issue_classification", 26),
    field<&StockDirectory::issue_sub_type>("issue_sub_type", 27),
    field<&StockDirectory::authenticity>("authenticity", 29),
    field<&StockDirectory::short_sale_threshold_indicator>("short_sale_threshold_indicator", 30),
    field<&StockDirectory::ipo_flag>("ipo_flag", 31),
    field<&StockDirectory::luld_reference_price_tier>("luld_reference_price_tier", 32),
    field<&StockDirectory::etp_flag>("etp_flag", 33),
    field<&StockDirectory::etp_leverage_factor>("etp_leverage_factor", 34),
    field<&StockDirectory::inverse_indicator>("inverse_indicator", 38),
};

template <>
inline constexpr auto kBodyFields<StockTradingAction> = std::tuple{
    field<&StockTradingAction::stock>("stock", 11),
    field<&StockTradingAction::trading_state>("trading_state", 19),
    field<&StockTradingAction::reserved>("reserved", 20),
    field<&StockTradingAction::reason>("reason", 21),
};

template <>
inline constexpr auto kBodyFields<RegShoRestriction> = std::tuple{
    field<&RegShoRestriction::stock>("stock", 11),
    field<&RegShoRestriction::reg_sho_action>("reg_sho_action", 19),
};

template <>
inline constexpr auto kBodyFields<MarketParticipantPosition> = std::tuple{
    field<&MarketParticipantPosition::mpid>("mpid", 11),
    field<&MarketParticipantPosition::stock>("stock", 15),
    field<&MarketParticipantPosition::primary_market_maker>("primary_market_maker", 23),
    field<&MarketParticipantPosition::market_maker_mode>("market_maker_mode", 24),
    field<&MarketParticipantPosition::market_participant_state>("market_participant_state", 25),
};

template <>
inline constexpr auto kBodyFields<MwcbDeclineLevel> = std::tuple{
    field<&MwcbDeclineLevel::level_1>("level_1", 11),
    field<&MwcbDeclineLevel::level_2>("level_2", 19),
    field<&MwcbDeclineLevel::level_3>("level_3", 27),
};

template <>
inline constexpr auto kBodyFields<MwcbStatus> = std::tuple{
    field<&MwcbStatus::breached_level>("breached_level", 11),
};

template <>
inline constexpr auto kBodyFields<IpoQuotingPeriodUpdate> = std::tuple{
    field<&IpoQuotingPeriodUpdate::stock>("stock", 11),
    field<&IpoQuotingPeriodUpdate::ipo_quotation_release_time>("ipo_quotation_release_time", 19),
    field<&IpoQuotingPeriodUpdate::ipo_quotation_release_qualifier>(
        "ipo_quotation_release_qualifier", 23),
    field<&IpoQuotingPeriodUpdate::ipo_price>("ipo_price", 24),
};

template <>
inline constexpr auto kBodyFields<LuldAuctionCollar> = std::tuple{
    field<&LuldAuctionCollar::stock>("stock", 11),
    field<&LuldAuctionCollar::auction_collar_reference_price>("auction_collar_reference_price", 19),
    field<&LuldAuctionCollar::upper_auction_collar_price>("upper_auction_collar_price", 23),
    field<&LuldAuctionCollar::lower_auction_collar_price>("lower_auction_collar_price", 27),
    field<&LuldAuctionCollar::auction_collar_extension>("auction_collar_extension", 31),
};

template <>
inline constexpr auto kBodyFields<OperationalHalt> = std::tuple{
    field<&OperationalHalt::stock>("stock", 11),
    field<&OperationalHalt::market_code>("market_code", 19),
    field<&OperationalHalt::operational_halt_action>("operational_halt_action", 20),
};

template <>
inline constexpr auto kBodyFields<AddOrder> = std::tuple{
    field<&AddOrder::order_reference_number>("order_reference_number", 11),
    field<&AddOrder::buy_sell_indicator>("buy_sell_indicator", 19),
    field<&AddOrder::shares>("shares", 20),
    field<&AddOrder::stock>("stock", 24),
    field<&AddOrder::price>("price", 32),
};

template <>
inline constexpr auto kBodyFields<AddOrderWithMpid> = std::tuple{
    field<&AddOrderWithMpid::order_reference_number>("order_reference_number", 11),
    field<&AddOrderWithMpid::buy_sell_indicator>("buy_sell_indicator", 19),
    field<&AddOrderWithMpid::shares>("shares", 20),
    field<&AddOrderWithMpid::stock>("stock", 24),
    field<&AddOrderWithMpid::price>("price", 32),
    field<&AddOrderWithMpid::attribution>("attribution", 36),
};

template <>
inline constexpr auto kBodyFields<OrderExecuted> = std::tuple{
    field<&OrderExecuted::order_reference_number>("order_reference_number", 11),
    field<&OrderExecuted::executed_shares>("executed_shares", 19),
    field<&OrderExecuted::match_number>("match_number", 23),
};

template <>
inline constexpr auto kBodyFields<OrderExecutedWithPrice> = std::tuple{
    field<&OrderExecutedWithPrice::order_reference_number>("order_reference_number", 11),
    field<&OrderExecutedWithPrice::executed_shares>("executed_shares", 19),
    field<&OrderExecutedWithPrice::match_number>("match_number", 23),
    field<&OrderExecutedWithPrice::printable>("printable", 31),
    field<&OrderExecutedWithPrice::execution_price>("execution_price", 32),
};

template <>
inline constexpr auto kBodyFields<OrderCancel> = std::tuple{
    field<&OrderCancel::order_reference_number>("order_reference_number", 11),
    field<&OrderCancel::cancelled_shares>("cancelled_shares", 19),
};

template <>
inline constexpr auto kBodyFields<OrderDelete> = std::tuple{
    field<&OrderDelete::order_reference_number>("order_reference_number", 11),
};

template <>
inline constexpr auto kBodyFields<OrderReplace> = std::tuple{
    field<&OrderReplace::original_order_reference_number>("original_order_reference_number", 11),
    field<&OrderReplace::new_order_reference_number>("new_order_reference_number", 19),
    field<&OrderReplace::shares>("shares", 27),
    field<&OrderReplace::price>("price", 31),
};

template <>
inline constexpr auto kBodyFields<Trade> = std::tuple{
    field<&Trade::order_reference_number>("order_reference_number", 11),
    field<&Trade::buy_sell_indicator>("buy_sell_indicator", 19),
    field<&Trade::shares>("shares", 20),
    field<&Trade::stock>("stock", 24),
    field<&Trade::price>("price", 32),
    field<&Trade::match_number>("match_number", 36),
};

template <>
inline constexpr auto kBodyFields<CrossTrade> = std::tuple{
    field<&CrossTrade::shares>("shares", 11),
    field<&CrossTrade::stock>("stock", 19),
    field<&CrossTrade::cross_price>("cross_price", 27),
    field<&CrossTrade::match_number>("match_number", 31),
    field<&CrossTrade::cross_type>("cross_type", 39),
};

template <>
inline constexpr auto kBodyFields<BrokenTrade> = std::tuple{
    field<&BrokenTrade::match_number>("match_number", 11),
};

template <>
inline constexpr auto kBodyFields<NetOrderImbalance> = std::tuple{
    field<&NetOrderImbalance::paired_shares>("paired_shares", 11),
    field<&NetOrderImbalance::imbalance_shares>("imbalance_shares", 19),
    field<&NetOrderImbalance::imbalance_direction>("imbalance_direction", 27),
    field<&NetOrderImbalance::stock>("stock", 28),
    field<&NetOrderImbalance::far_price>("far_price", 36),
    field<&NetOrderImbalance::near_price>("near_price", 40),
    field<&NetOrderImbalance::current_reference_price>("current_reference_price", 44),
    field<&NetOrderImbalance::cross_type>("cross_type", 48),
    field<&NetOrderImbalance::price_variation_indicator>("price_variation_indicator", 49),
};

template <>
inline constexpr auto kBodyFields<RetailInterest> = std::tuple{
    field<&RetailInterest::stock>("stock", 11),
    field<&RetailInterest::interest_flag>("interest_flag", 19),
};

template <>
inline constexpr auto kBodyFields<DirectListingPriceDiscovery> = std::tuple{
    field<&DirectListingPriceDiscovery::stock>("stock", 11),
    field<&DirectListingPriceDiscovery::open_eligibility_status>("open_eligibility_status", 19),
    field<&DirectListingPriceDiscovery::minimum_allowable_price>("minimum_allowable_price", 20),
    field<&DirectListingPriceDiscovery::maximum_allowable_price>("maximum_allowable_price", 24),
    field<&DirectListingPriceDiscovery::near_execution_price>("near_execution_price", 28),
    field<&DirectListingPriceDiscovery::near_execution_time>("near_execution_time", 32),
    field<&DirectListingPriceDiscovery::lower_price_range_collar>("lower_price_range_collar", 40),
    field<&DirectListingPriceDiscovery::upper_price_range_collar>("upper_price_range_collar", 44),
};

// Header then body, in wire order.
template <class T>
inline constexpr auto kAllFields = std::tuple_cat(kHeaderFields, kBodyFields<T>);

// True when the fields run from just after the type byte to the end of the
// message with no gap or overlap. Every byte of a message therefore belongs to
// exactly one field, which is what a byte-exact round trip rests on.
template <class T>
[[nodiscard]] consteval bool covers_every_byte() {
    std::size_t next = kTypeByteSize;
    bool contiguous = true;
    std::apply(
        [&](const auto&... fields) {
            ((contiguous = contiguous && fields.offset == next, next += fields.width), ...);
        },
        kAllFields<T>);
    return contiguous && next == T::wire_size;
}

// Applies covers_every_byte to the typed alternatives of Message.
template <std::size_t... I>
[[nodiscard]] consteval bool all_layouts_cover_every_byte(std::index_sequence<I...> /*unused*/) {
    return (covers_every_byte<std::variant_alternative_t<I, Message>>() && ...);
}

static_assert(all_layouts_cover_every_byte(std::make_index_sequence<kMessageTypeCount>{}),
              "a message layout has a gap, an overlap or the wrong total length");

}  // namespace tick_store::itch::detail
