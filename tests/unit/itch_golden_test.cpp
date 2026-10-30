// Golden bytes both ways, one test per type: the hand-built bytes decode to the
// values read off them by eye, and those values encode back to the same bytes.

#include "itch_fixtures.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>
#include <variant>

namespace tick_store::itch {
namespace {

using test::Bytes;
using test::fixture;
using test::fixtures;

template <std::size_t N>
Alpha<N> alpha(std::string_view text) {
    Alpha<N> out{};
    std::ranges::transform(text.substr(0, N), out.begin(),
                           [](char c) { return static_cast<std::byte>(c); });
    return out;
}

const Stock kZvzzt = alpha<8>("ZVZZT   ");

constexpr std::byte ch(char c) { return static_cast<std::byte>(c); }

// Decodes `type`'s fixture as T and checks the header and the way back to bytes.
template <class T>
T golden(char type) {
    const Bytes& input = fixture(type).bytes;
    EXPECT_EQ(input.size(), T::wire_size);

    const auto decoded = decode_strict(input);
    EXPECT_TRUE(decoded.has_value());
    if (!decoded.has_value() || !std::holds_alternative<T>(*decoded)) {
        ADD_FAILURE() << "decoded to the wrong alternative";
        return T{};
    }
    const T message = std::get<T>(*decoded);

    EXPECT_EQ(message.header.stock_locate, 0x1112U);
    EXPECT_EQ(message.header.tracking_number, 0x2122U);
    EXPECT_EQ(message.header.timestamp_ns, 0x313233343536ULL);

    // The lossless entry point agrees on a conforming message.
    const auto lossless = decode(input);
    EXPECT_TRUE(lossless.has_value() && *lossless == *decoded);

    Bytes out(T::wire_size);
    const auto written = encode(message, out);
    EXPECT_TRUE(written.has_value());
    EXPECT_EQ(written.value_or(0), T::wire_size);
    EXPECT_EQ(out, input);
    return message;
}

TEST(ItchGolden, SystemEvent) {
    const auto m = golden<SystemEvent>('S');
    EXPECT_EQ(m.event_code, ch('O'));
}

TEST(ItchGolden, StockDirectory) {
    const auto m = golden<StockDirectory>('R');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.market_category, ch('Q'));
    EXPECT_EQ(m.financial_status_indicator, ch('N'));
    EXPECT_EQ(m.round_lot_size, 0x41424344U);
    EXPECT_EQ(m.round_lots_only, ch('Y'));
    EXPECT_EQ(m.issue_classification, ch('C'));
    EXPECT_EQ(m.issue_sub_type, alpha<2>("AI"));
    EXPECT_EQ(m.authenticity, ch('P'));
    EXPECT_EQ(m.short_sale_threshold_indicator, ch('E'));
    EXPECT_EQ(m.ipo_flag, ch('F'));
    EXPECT_EQ(m.luld_reference_price_tier, ch('1'));
    EXPECT_EQ(m.etp_flag, ch('G'));
    EXPECT_EQ(m.etp_leverage_factor, 0x51525354U);
    EXPECT_EQ(m.inverse_indicator, ch('H'));
}

TEST(ItchGolden, StockTradingAction) {
    const auto m = golden<StockTradingAction>('H');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.trading_state, ch('T'));
    EXPECT_EQ(m.reserved, ch('x'));
    EXPECT_EQ(m.reason, alpha<4>("MWC1"));
}

TEST(ItchGolden, RegShoRestriction) {
    const auto m = golden<RegShoRestriction>('Y');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.reg_sho_action, ch('1'));
}

TEST(ItchGolden, MarketParticipantPosition) {
    const auto m = golden<MarketParticipantPosition>('L');
    EXPECT_EQ(m.mpid, alpha<4>("NSDQ"));
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.primary_market_maker, ch('Y'));
    EXPECT_EQ(m.market_maker_mode, ch('P'));
    EXPECT_EQ(m.market_participant_state, ch('A'));
}

TEST(ItchGolden, MwcbDeclineLevel) {
    const auto m = golden<MwcbDeclineLevel>('V');
    EXPECT_EQ(m.level_1, Price8{0x4142434445464748ULL});
    EXPECT_EQ(m.level_2, Price8{0x5152535455565758ULL});
    EXPECT_EQ(m.level_3, Price8{0x6162636465666768ULL});
}

TEST(ItchGolden, MwcbStatus) {
    const auto m = golden<MwcbStatus>('W');
    EXPECT_EQ(m.breached_level, ch('2'));
}

TEST(ItchGolden, IpoQuotingPeriodUpdate) {
    const auto m = golden<IpoQuotingPeriodUpdate>('K');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.ipo_quotation_release_time, 0x41424344U);
    EXPECT_EQ(m.ipo_quotation_release_qualifier, ch('A'));
    EXPECT_EQ(m.ipo_price, Price4{0x51525354U});
}

TEST(ItchGolden, LuldAuctionCollar) {
    const auto m = golden<LuldAuctionCollar>('J');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.auction_collar_reference_price, Price4{0x41424344U});
    EXPECT_EQ(m.upper_auction_collar_price, Price4{0x51525354U});
    EXPECT_EQ(m.lower_auction_collar_price, Price4{0x61626364U});
    EXPECT_EQ(m.auction_collar_extension, 0x71727374U);
}

TEST(ItchGolden, OperationalHalt) {
    const auto m = golden<OperationalHalt>('h');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.market_code, ch('Q'));
    EXPECT_EQ(m.operational_halt_action, ch('H'));
}

TEST(ItchGolden, AddOrder) {
    const auto m = golden<AddOrder>('A');
    EXPECT_EQ(m.order_reference_number, 0x4142434445464748ULL);
    EXPECT_EQ(m.buy_sell_indicator, ch('B'));
    EXPECT_EQ(m.shares, 0x51525354U);
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.price, Price4{0x61626364U});
}

TEST(ItchGolden, AddOrderWithMpid) {
    const auto m = golden<AddOrderWithMpid>('F');
    EXPECT_EQ(m.order_reference_number, 0x4142434445464748ULL);
    EXPECT_EQ(m.buy_sell_indicator, ch('S'));
    EXPECT_EQ(m.shares, 0x51525354U);
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.price, Price4{0x61626364U});
    EXPECT_EQ(m.attribution, alpha<4>("GSCO"));
}

TEST(ItchGolden, OrderExecuted) {
    const auto m = golden<OrderExecuted>('E');
    EXPECT_EQ(m.order_reference_number, 0x4142434445464748ULL);
    EXPECT_EQ(m.executed_shares, 0x51525354U);
    EXPECT_EQ(m.match_number, 0x6162636465666768ULL);
}

TEST(ItchGolden, OrderExecutedWithPrice) {
    const auto m = golden<OrderExecutedWithPrice>('C');
    EXPECT_EQ(m.order_reference_number, 0x4142434445464748ULL);
    EXPECT_EQ(m.executed_shares, 0x51525354U);
    EXPECT_EQ(m.match_number, 0x6162636465666768ULL);
    EXPECT_EQ(m.printable, ch('Y'));
    EXPECT_EQ(m.execution_price, Price4{0x71727374U});
}

TEST(ItchGolden, OrderCancel) {
    const auto m = golden<OrderCancel>('X');
    EXPECT_EQ(m.order_reference_number, 0x4142434445464748ULL);
    EXPECT_EQ(m.cancelled_shares, 0x51525354U);
}

TEST(ItchGolden, OrderDelete) {
    const auto m = golden<OrderDelete>('D');
    EXPECT_EQ(m.order_reference_number, 0x4142434445464748ULL);
}

TEST(ItchGolden, OrderReplace) {
    const auto m = golden<OrderReplace>('U');
    EXPECT_EQ(m.original_order_reference_number, 0x4142434445464748ULL);
    EXPECT_EQ(m.new_order_reference_number, 0x5152535455565758ULL);
    EXPECT_EQ(m.shares, 0x61626364U);
    EXPECT_EQ(m.price, Price4{0x71727374U});
}

TEST(ItchGolden, Trade) {
    const auto m = golden<Trade>('P');
    EXPECT_EQ(m.order_reference_number, 0x4142434445464748ULL);
    EXPECT_EQ(m.buy_sell_indicator, ch('B'));
    EXPECT_EQ(m.shares, 0x51525354U);
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.price, Price4{0x61626364U});
    EXPECT_EQ(m.match_number, 0x7172737475767778ULL);
}

TEST(ItchGolden, CrossTrade) {
    const auto m = golden<CrossTrade>('Q');
    EXPECT_EQ(m.shares, 0x4142434445464748ULL);
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.cross_price, Price4{0x51525354U});
    EXPECT_EQ(m.match_number, 0x6162636465666768ULL);
    EXPECT_EQ(m.cross_type, ch('O'));
}

TEST(ItchGolden, BrokenTrade) {
    const auto m = golden<BrokenTrade>('B');
    EXPECT_EQ(m.match_number, 0x4142434445464748ULL);
}

TEST(ItchGolden, NetOrderImbalance) {
    const auto m = golden<NetOrderImbalance>('I');
    EXPECT_EQ(m.paired_shares, 0x4142434445464748ULL);
    EXPECT_EQ(m.imbalance_shares, 0x5152535455565758ULL);
    EXPECT_EQ(m.imbalance_direction, ch('B'));
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.far_price, Price4{0x61626364U});
    EXPECT_EQ(m.near_price, Price4{0x71727374U});
    EXPECT_EQ(m.current_reference_price, Price4{0x81828384U});
    EXPECT_EQ(m.cross_type, ch('O'));
    EXPECT_EQ(m.price_variation_indicator, ch('L'));
}

TEST(ItchGolden, RetailInterest) {
    const auto m = golden<RetailInterest>('N');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.interest_flag, ch('B'));
}

TEST(ItchGolden, DirectListingPriceDiscovery) {
    const auto m = golden<DirectListingPriceDiscovery>('O');
    EXPECT_EQ(m.stock, kZvzzt);
    EXPECT_EQ(m.open_eligibility_status, ch('Y'));
    EXPECT_EQ(m.minimum_allowable_price, Price4{0x41424344U});
    EXPECT_EQ(m.maximum_allowable_price, Price4{0x51525354U});
    EXPECT_EQ(m.near_execution_price, Price4{0x61626364U});
    EXPECT_EQ(m.near_execution_time, 0x7172737475767778ULL);
    EXPECT_EQ(m.lower_price_range_collar, Price4{0x81828384U});
    EXPECT_EQ(m.upper_price_range_collar, Price4{0x91929394U});
}

// The fixtures themselves, since every test below trusts them.
TEST(ItchFixtures, CoverAll23TypesWithSoundFieldTables) {
    ASSERT_EQ(fixtures().size(), 23U);
    ASSERT_EQ(fixtures().size(), kMessageTypeCount);
    std::set<char> types;
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        types.insert(f.type);
        EXPECT_EQ(f.bytes.size(), f.length);
        EXPECT_EQ(f.bytes.front(), static_cast<std::byte>(f.type));
        std::size_t next = 0;
        for (const auto& field : f.fields) {
            EXPECT_EQ(field.offset, next) << field.name;
            next += field.width;
        }
        EXPECT_EQ(next, f.length);
    }
    EXPECT_EQ(types.size(), 23U);
}

TEST(ItchWireSize, IsPinnedForEveryByteValue) {
    const std::array<std::pair<char, std::size_t>, 23> expected{{
        {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28},
        {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
        {'U', 35}, {'P', 44}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48},
    }};
    std::size_t known = 0;
    for (unsigned value = 0; value < 256; ++value) {
        const auto type = static_cast<std::byte>(value);
        const auto size = wire_size_of(type);
        const auto match = std::ranges::find_if(expected, [value](const auto& entry) {
            return static_cast<unsigned char>(entry.first) == value;
        });
        if (match == expected.end()) {
            EXPECT_FALSE(size.has_value()) << value;
        } else {
            ++known;
            EXPECT_EQ(size, match->second) << value;
        }
    }
    EXPECT_EQ(known, 23U);
}

}  // namespace
}  // namespace tick_store::itch
