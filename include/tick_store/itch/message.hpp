#pragma once

// The 23 message types of Nasdaq TotalView-ITCH 5.0, as plain typed values.
//
// Field names and order follow the specification's tables. Offsets and widths
// live in one place, src/itch/layout.hpp.
//
// Nothing here is normalised, because a byte-exact round trip depends on it:
//
// - Alpha fields stay as raw bytes, padding included. The specification's enum
//   lists have grown over time, so a value not listed there is data, not an error.
// - A timestamp is not range-checked against the length of a day.
// - Fields the specification calls constant or reserved are still kept: `H`
//   Reserved, `P` Order Reference Number and Buy/Sell Indicator.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>

namespace tick_store::itch {

// A price with 4 implied decimals, as on the wire. Distinct types keep a price
// from being passed where shares are expected.
enum class Price4 : std::uint32_t {};

// A price with 8 implied decimals, used only by the MWCB levels.
enum class Price8 : std::uint64_t {};

// An Alpha field wider than one byte. A one-byte Alpha field is a std::byte.
template <std::size_t N>
using Alpha = std::array<std::byte, N>;

using Stock = Alpha<8>;
using Mpid = Alpha<4>;

// The fields shared by every type, which follow the Message Type byte. The type
// byte itself is implied by the struct.
struct MessageHeader {
    std::uint16_t stock_locate = 0;
    std::uint16_t tracking_number = 0;
    // Nanoseconds since midnight. 6 bytes on the wire.
    std::uint64_t timestamp_ns = 0;

    friend bool operator==(const MessageHeader&, const MessageHeader&) = default;
};

// Every struct below has `type_code`, the wire byte, and `wire_size`, the length
// of the whole message from the type byte on, excluding the frame prefix.

struct SystemEvent {
    static constexpr std::byte type_code{'S'};
    static constexpr std::size_t wire_size = 12;

    MessageHeader header;
    std::byte event_code{};

    friend bool operator==(const SystemEvent&, const SystemEvent&) = default;
};

struct StockDirectory {
    static constexpr std::byte type_code{'R'};
    static constexpr std::size_t wire_size = 39;

    MessageHeader header;
    Stock stock{};
    std::byte market_category{};
    std::byte financial_status_indicator{};
    std::uint32_t round_lot_size = 0;
    std::byte round_lots_only{};
    std::byte issue_classification{};
    Alpha<2> issue_sub_type{};
    std::byte authenticity{};
    std::byte short_sale_threshold_indicator{};
    std::byte ipo_flag{};
    std::byte luld_reference_price_tier{};
    std::byte etp_flag{};
    std::uint32_t etp_leverage_factor = 0;
    std::byte inverse_indicator{};

    friend bool operator==(const StockDirectory&, const StockDirectory&) = default;
};

struct StockTradingAction {
    static constexpr std::byte type_code{'H'};
    static constexpr std::size_t wire_size = 25;

    MessageHeader header;
    Stock stock{};
    std::byte trading_state{};
    std::byte reserved{};
    Alpha<4> reason{};

    friend bool operator==(const StockTradingAction&, const StockTradingAction&) = default;
};

struct RegShoRestriction {
    static constexpr std::byte type_code{'Y'};
    static constexpr std::size_t wire_size = 20;

    MessageHeader header;
    Stock stock{};
    std::byte reg_sho_action{};

    friend bool operator==(const RegShoRestriction&, const RegShoRestriction&) = default;
};

struct MarketParticipantPosition {
    static constexpr std::byte type_code{'L'};
    static constexpr std::size_t wire_size = 26;

    MessageHeader header;
    Mpid mpid{};
    Stock stock{};
    std::byte primary_market_maker{};
    std::byte market_maker_mode{};
    std::byte market_participant_state{};

    friend bool operator==(const MarketParticipantPosition&,
                           const MarketParticipantPosition&) = default;
};

struct MwcbDeclineLevel {
    static constexpr std::byte type_code{'V'};
    static constexpr std::size_t wire_size = 35;

    MessageHeader header;
    Price8 level_1{};
    Price8 level_2{};
    Price8 level_3{};

    friend bool operator==(const MwcbDeclineLevel&, const MwcbDeclineLevel&) = default;
};

struct MwcbStatus {
    static constexpr std::byte type_code{'W'};
    static constexpr std::size_t wire_size = 12;

    MessageHeader header;
    std::byte breached_level{};

    friend bool operator==(const MwcbStatus&, const MwcbStatus&) = default;
};

struct IpoQuotingPeriodUpdate {
    static constexpr std::byte type_code{'K'};
    static constexpr std::size_t wire_size = 28;

    MessageHeader header;
    Stock stock{};
    std::uint32_t ipo_quotation_release_time = 0;
    std::byte ipo_quotation_release_qualifier{};
    Price4 ipo_price{};

    friend bool operator==(const IpoQuotingPeriodUpdate&,
                           const IpoQuotingPeriodUpdate&) = default;
};

struct LuldAuctionCollar {
    static constexpr std::byte type_code{'J'};
    static constexpr std::size_t wire_size = 35;

    MessageHeader header;
    Stock stock{};
    Price4 auction_collar_reference_price{};
    Price4 upper_auction_collar_price{};
    Price4 lower_auction_collar_price{};
    std::uint32_t auction_collar_extension = 0;

    friend bool operator==(const LuldAuctionCollar&, const LuldAuctionCollar&) = default;
};

struct OperationalHalt {
    static constexpr std::byte type_code{'h'};
    static constexpr std::size_t wire_size = 21;

    MessageHeader header;
    Stock stock{};
    std::byte market_code{};
    std::byte operational_halt_action{};

    friend bool operator==(const OperationalHalt&, const OperationalHalt&) = default;
};

struct AddOrder {
    static constexpr std::byte type_code{'A'};
    static constexpr std::size_t wire_size = 36;

    MessageHeader header;
    std::uint64_t order_reference_number = 0;
    std::byte buy_sell_indicator{};
    std::uint32_t shares = 0;
    Stock stock{};
    Price4 price{};

    friend bool operator==(const AddOrder&, const AddOrder&) = default;
};

struct AddOrderWithMpid {
    static constexpr std::byte type_code{'F'};
    static constexpr std::size_t wire_size = 40;

    MessageHeader header;
    std::uint64_t order_reference_number = 0;
    std::byte buy_sell_indicator{};
    std::uint32_t shares = 0;
    Stock stock{};
    Price4 price{};
    Mpid attribution{};

    friend bool operator==(const AddOrderWithMpid&, const AddOrderWithMpid&) = default;
};

struct OrderExecuted {
    static constexpr std::byte type_code{'E'};
    static constexpr std::size_t wire_size = 31;

    MessageHeader header;
    std::uint64_t order_reference_number = 0;
    std::uint32_t executed_shares = 0;
    std::uint64_t match_number = 0;

    friend bool operator==(const OrderExecuted&, const OrderExecuted&) = default;
};

struct OrderExecutedWithPrice {
    static constexpr std::byte type_code{'C'};
    static constexpr std::size_t wire_size = 36;

    MessageHeader header;
    std::uint64_t order_reference_number = 0;
    std::uint32_t executed_shares = 0;
    std::uint64_t match_number = 0;
    std::byte printable{};
    Price4 execution_price{};

    friend bool operator==(const OrderExecutedWithPrice&,
                           const OrderExecutedWithPrice&) = default;
};

struct OrderCancel {
    static constexpr std::byte type_code{'X'};
    static constexpr std::size_t wire_size = 23;

    MessageHeader header;
    std::uint64_t order_reference_number = 0;
    std::uint32_t cancelled_shares = 0;

    friend bool operator==(const OrderCancel&, const OrderCancel&) = default;
};

struct OrderDelete {
    static constexpr std::byte type_code{'D'};
    static constexpr std::size_t wire_size = 19;

    MessageHeader header;
    std::uint64_t order_reference_number = 0;

    friend bool operator==(const OrderDelete&, const OrderDelete&) = default;
};

struct OrderReplace {
    static constexpr std::byte type_code{'U'};
    static constexpr std::size_t wire_size = 35;

    MessageHeader header;
    std::uint64_t original_order_reference_number = 0;
    std::uint64_t new_order_reference_number = 0;
    std::uint32_t shares = 0;
    Price4 price{};

    friend bool operator==(const OrderReplace&, const OrderReplace&) = default;
};

struct Trade {
    static constexpr std::byte type_code{'P'};
    static constexpr std::size_t wire_size = 44;

    MessageHeader header;
    std::uint64_t order_reference_number = 0;
    std::byte buy_sell_indicator{};
    std::uint32_t shares = 0;
    Stock stock{};
    Price4 price{};
    std::uint64_t match_number = 0;

    friend bool operator==(const Trade&, const Trade&) = default;
};

struct CrossTrade {
    static constexpr std::byte type_code{'Q'};
    static constexpr std::size_t wire_size = 40;

    MessageHeader header;
    // 8 bytes here and in NetOrderImbalance, 4 in every other shares field.
    std::uint64_t shares = 0;
    Stock stock{};
    Price4 cross_price{};
    std::uint64_t match_number = 0;
    std::byte cross_type{};

    friend bool operator==(const CrossTrade&, const CrossTrade&) = default;
};

struct BrokenTrade {
    static constexpr std::byte type_code{'B'};
    static constexpr std::size_t wire_size = 19;

    MessageHeader header;
    std::uint64_t match_number = 0;

    friend bool operator==(const BrokenTrade&, const BrokenTrade&) = default;
};

struct NetOrderImbalance {
    static constexpr std::byte type_code{'I'};
    static constexpr std::size_t wire_size = 50;

    MessageHeader header;
    std::uint64_t paired_shares = 0;
    std::uint64_t imbalance_shares = 0;
    std::byte imbalance_direction{};
    Stock stock{};
    Price4 far_price{};
    Price4 near_price{};
    Price4 current_reference_price{};
    std::byte cross_type{};
    std::byte price_variation_indicator{};

    friend bool operator==(const NetOrderImbalance&, const NetOrderImbalance&) = default;
};

struct RetailInterest {
    static constexpr std::byte type_code{'N'};
    static constexpr std::size_t wire_size = 20;

    MessageHeader header;
    Stock stock{};
    std::byte interest_flag{};

    friend bool operator==(const RetailInterest&, const RetailInterest&) = default;
};

struct DirectListingPriceDiscovery {
    static constexpr std::byte type_code{'O'};
    static constexpr std::size_t wire_size = 48;

    MessageHeader header;
    Stock stock{};
    std::byte open_eligibility_status{};
    Price4 minimum_allowable_price{};
    Price4 maximum_allowable_price{};
    Price4 near_execution_price{};
    std::uint64_t near_execution_time = 0;
    Price4 lower_price_range_collar{};
    Price4 upper_price_range_collar{};

    friend bool operator==(const DirectListingPriceDiscovery&,
                           const DirectListingPriceDiscovery&) = default;
};

// Why a payload took the raw path.
enum class RawReason : std::uint8_t {
    // The first byte is not one of the 23 type codes.
    unknown_type,
    // The first byte is a known type code, but the length is not that type's.
    wrong_length,
};

inline constexpr std::size_t kRawReasonCount = 2;

// A payload that is not a conforming ITCH 5.0 message, kept whole: `bytes` is
// every byte of the payload, type byte first, so encoding reproduces it exactly.
//
// Lifetime: unlike every other alternative of Message, this one borrows. `bytes`
// is a view of the span given to decode() and is valid only while that memory is.
struct RawMessage {
    std::span<const std::byte> bytes;
    RawReason reason = RawReason::unknown_type;

    // Compares content, not addresses.
    friend bool operator==(const RawMessage& a, const RawMessage& b) noexcept {
        if (a.reason != b.reason || a.bytes.size() != b.bytes.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.bytes.size(); ++i) {
            if (a.bytes[i] != b.bytes[i]) {
                return false;
            }
        }
        return true;
    }
};

using Message =
    std::variant<SystemEvent, StockDirectory, StockTradingAction, RegShoRestriction,
                 MarketParticipantPosition, MwcbDeclineLevel, MwcbStatus, IpoQuotingPeriodUpdate,
                 LuldAuctionCollar, OperationalHalt, AddOrder, AddOrderWithMpid, OrderExecuted,
                 OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace, Trade, CrossTrade,
                 BrokenTrade, NetOrderImbalance, RetailInterest, DirectListingPriceDiscovery,
                 RawMessage>;

// Number of typed alternatives of Message, which excludes RawMessage.
inline constexpr std::size_t kMessageTypeCount = std::variant_size_v<Message> - 1;

// A Message is a value that can be copied with memcpy and held in a ring buffer.
static_assert(std::is_trivially_copyable_v<Message>);

}  // namespace tick_store::itch
