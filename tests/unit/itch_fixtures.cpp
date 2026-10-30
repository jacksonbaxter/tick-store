#include "itch_fixtures.hpp"

#include "test_bytes.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace tick_store::itch::test {
namespace {

// Type byte, then the common header.
Bytes header(char type) {
    return bytes({static_cast<unsigned char>(type),
                  0x11, 0x12,                           // stock_locate
                  0x21, 0x22,                           // tracking_number
                  0x31, 0x32, 0x33, 0x34, 0x35, 0x36}); // timestamp_ns
}

Bytes body(char type, std::initializer_list<unsigned> rest) {
    Bytes out = header(type);
    const Bytes tail = bytes(rest);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

std::vector<FieldSpec> layout(std::initializer_list<FieldSpec> rest) {
    std::vector<FieldSpec> out{
        {"message_type", 0, 1},
        {"stock_locate", 1, 2},
        {"tracking_number", 3, 2},
        {"timestamp_ns", 5, 6},
    };
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

std::vector<Fixture> build() {
    std::vector<Fixture> all;

    all.push_back({"SystemEvent", 'S', 12,
                   body('S', {'O'}),
                   layout({{"event_code", 11, 1}})});

    all.push_back({"StockDirectory", 'R', 39,
                   body('R', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              'Q',                                     // market_category
                              'N',                                     // financial_status_indicator
                              0x41, 0x42, 0x43, 0x44,                  // round_lot_size
                              'Y',                                     // round_lots_only
                              'C',                                     // issue_classification
                              'A', 'I',                                // issue_sub_type
                              'P',                                     // authenticity
                              'E',                                     // short_sale_threshold_indicator
                              'F',                                     // ipo_flag
                              '1',                                     // luld_reference_price_tier
                              'G',                                     // etp_flag
                              0x51, 0x52, 0x53, 0x54,                  // etp_leverage_factor
                              'H'}),                                   // inverse_indicator
                   layout({{"stock", 11, 8},
                           {"market_category", 19, 1},
                           {"financial_status_indicator", 20, 1},
                           {"round_lot_size", 21, 4},
                           {"round_lots_only", 25, 1},
                           {"issue_classification", 26, 1},
                           {"issue_sub_type", 27, 2},
                           {"authenticity", 29, 1},
                           {"short_sale_threshold_indicator", 30, 1},
                           {"ipo_flag", 31, 1},
                           {"luld_reference_price_tier", 32, 1},
                           {"etp_flag", 33, 1},
                           {"etp_leverage_factor", 34, 4},
                           {"inverse_indicator", 38, 1}})});

    all.push_back({"StockTradingAction", 'H', 25,
                   body('H', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              'T',                                     // trading_state
                              'x',                                     // reserved
                              'M', 'W', 'C', '1'}),                    // reason
                   layout({{"stock", 11, 8},
                           {"trading_state", 19, 1},
                           {"reserved", 20, 1},
                           {"reason", 21, 4}})});

    all.push_back({"RegShoRestriction", 'Y', 20,
                   body('Y', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              '1'}),                                   // reg_sho_action
                   layout({{"stock", 11, 8}, {"reg_sho_action", 19, 1}})});

    all.push_back({"MarketParticipantPosition", 'L', 26,
                   body('L', {'N', 'S', 'D', 'Q',                      // mpid
                              'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              'Y',                                     // primary_market_maker
                              'P',                                     // market_maker_mode
                              'A'}),                                   // market_participant_state
                   layout({{"mpid", 11, 4},
                           {"stock", 15, 8},
                           {"primary_market_maker", 23, 1},
                           {"market_maker_mode", 24, 1},
                           {"market_participant_state", 25, 1}})});

    all.push_back({"MwcbDeclineLevel", 'V', 35,
                   body('V', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,   // level_1
                              0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,   // level_2
                              0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68}), // level_3
                   layout({{"level_1", 11, 8}, {"level_2", 19, 8}, {"level_3", 27, 8}})});

    all.push_back({"MwcbStatus", 'W', 12,
                   body('W', {'2'}),
                   layout({{"breached_level", 11, 1}})});

    all.push_back({"IpoQuotingPeriodUpdate", 'K', 28,
                   body('K', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              0x41, 0x42, 0x43, 0x44,                  // ipo_quotation_release_time
                              'A',                                     // ipo_quotation_release_qualifier
                              0x51, 0x52, 0x53, 0x54}),                // ipo_price
                   layout({{"stock", 11, 8},
                           {"ipo_quotation_release_time", 19, 4},
                           {"ipo_quotation_release_qualifier", 23, 1},
                           {"ipo_price", 24, 4}})});

    all.push_back({"LuldAuctionCollar", 'J', 35,
                   body('J', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              0x41, 0x42, 0x43, 0x44,                  // auction_collar_reference_price
                              0x51, 0x52, 0x53, 0x54,                  // upper_auction_collar_price
                              0x61, 0x62, 0x63, 0x64,                  // lower_auction_collar_price
                              0x71, 0x72, 0x73, 0x74}),                // auction_collar_extension
                   layout({{"stock", 11, 8},
                           {"auction_collar_reference_price", 19, 4},
                           {"upper_auction_collar_price", 23, 4},
                           {"lower_auction_collar_price", 27, 4},
                           {"auction_collar_extension", 31, 4}})});

    all.push_back({"OperationalHalt", 'h', 21,
                   body('h', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              'Q',                                     // market_code
                              'H'}),                                   // operational_halt_action
                   layout({{"stock", 11, 8},
                           {"market_code", 19, 1},
                           {"operational_halt_action", 20, 1}})});

    all.push_back({"AddOrder", 'A', 36,
                   body('A', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,  // order_reference_number
                              'B',                                             // buy_sell_indicator
                              0x51, 0x52, 0x53, 0x54,                          // shares
                              'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',          // stock
                              0x61, 0x62, 0x63, 0x64}),                        // price
                   layout({{"order_reference_number", 11, 8},
                           {"buy_sell_indicator", 19, 1},
                           {"shares", 20, 4},
                           {"stock", 24, 8},
                           {"price", 32, 4}})});

    all.push_back({"AddOrderWithMpid", 'F', 40,
                   body('F', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,  // order_reference_number
                              'S',                                             // buy_sell_indicator
                              0x51, 0x52, 0x53, 0x54,                          // shares
                              'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',          // stock
                              0x61, 0x62, 0x63, 0x64,                          // price
                              'G', 'S', 'C', 'O'}),                            // attribution
                   layout({{"order_reference_number", 11, 8},
                           {"buy_sell_indicator", 19, 1},
                           {"shares", 20, 4},
                           {"stock", 24, 8},
                           {"price", 32, 4},
                           {"attribution", 36, 4}})});

    all.push_back({"OrderExecuted", 'E', 31,
                   body('E', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,   // order_reference_number
                              0x51, 0x52, 0x53, 0x54,                           // executed_shares
                              0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68}), // match_number
                   layout({{"order_reference_number", 11, 8},
                           {"executed_shares", 19, 4},
                           {"match_number", 23, 8}})});

    all.push_back({"OrderExecutedWithPrice", 'C', 36,
                   body('C', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,  // order_reference_number
                              0x51, 0x52, 0x53, 0x54,                          // executed_shares
                              0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,  // match_number
                              'Y',                                             // printable
                              0x71, 0x72, 0x73, 0x74}),                        // execution_price
                   layout({{"order_reference_number", 11, 8},
                           {"executed_shares", 19, 4},
                           {"match_number", 23, 8},
                           {"printable", 31, 1},
                           {"execution_price", 32, 4}})});

    all.push_back({"OrderCancel", 'X', 23,
                   body('X', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,  // order_reference_number
                              0x51, 0x52, 0x53, 0x54}),                        // cancelled_shares
                   layout({{"order_reference_number", 11, 8}, {"cancelled_shares", 19, 4}})});

    all.push_back({"OrderDelete", 'D', 19,
                   body('D', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48}),  // order_reference_number
                   layout({{"order_reference_number", 11, 8}})});

    all.push_back({"OrderReplace", 'U', 35,
                   body('U', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,  // original_order_reference_number
                              0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,  // new_order_reference_number
                              0x61, 0x62, 0x63, 0x64,                          // shares
                              0x71, 0x72, 0x73, 0x74}),                        // price
                   layout({{"original_order_reference_number", 11, 8},
                           {"new_order_reference_number", 19, 8},
                           {"shares", 27, 4},
                           {"price", 31, 4}})});

    all.push_back({"Trade", 'P', 44,
                   body('P', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,   // order_reference_number
                              'B',                                              // buy_sell_indicator
                              0x51, 0x52, 0x53, 0x54,                           // shares
                              'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',           // stock
                              0x61, 0x62, 0x63, 0x64,                           // price
                              0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78}), // match_number
                   layout({{"order_reference_number", 11, 8},
                           {"buy_sell_indicator", 19, 1},
                           {"shares", 20, 4},
                           {"stock", 24, 8},
                           {"price", 32, 4},
                           {"match_number", 36, 8}})});

    all.push_back({"CrossTrade", 'Q', 40,
                   body('Q', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,  // shares
                              'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',          // stock
                              0x51, 0x52, 0x53, 0x54,                          // cross_price
                              0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,  // match_number
                              'O'}),                                           // cross_type
                   layout({{"shares", 11, 8},
                           {"stock", 19, 8},
                           {"cross_price", 27, 4},
                           {"match_number", 31, 8},
                           {"cross_type", 39, 1}})});

    all.push_back({"BrokenTrade", 'B', 19,
                   body('B', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48}),  // match_number
                   layout({{"match_number", 11, 8}})});

    all.push_back({"NetOrderImbalance", 'I', 50,
                   body('I', {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,  // paired_shares
                              0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,  // imbalance_shares
                              'B',                                             // imbalance_direction
                              'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',          // stock
                              0x61, 0x62, 0x63, 0x64,                          // far_price
                              0x71, 0x72, 0x73, 0x74,                          // near_price
                              0x81, 0x82, 0x83, 0x84,                          // current_reference_price
                              'O',                                             // cross_type
                              'L'}),                                           // price_variation_indicator
                   layout({{"paired_shares", 11, 8},
                           {"imbalance_shares", 19, 8},
                           {"imbalance_direction", 27, 1},
                           {"stock", 28, 8},
                           {"far_price", 36, 4},
                           {"near_price", 40, 4},
                           {"current_reference_price", 44, 4},
                           {"cross_type", 48, 1},
                           {"price_variation_indicator", 49, 1}})});

    all.push_back({"RetailInterest", 'N', 20,
                   body('N', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',  // stock
                              'B'}),                                   // interest_flag
                   layout({{"stock", 11, 8}, {"interest_flag", 19, 1}})});

    all.push_back({"DirectListingPriceDiscovery", 'O', 48,
                   body('O', {'Z', 'V', 'Z', 'Z', 'T', ' ', ' ', ' ',          // stock
                              'Y',                                             // open_eligibility_status
                              0x41, 0x42, 0x43, 0x44,                          // minimum_allowable_price
                              0x51, 0x52, 0x53, 0x54,                          // maximum_allowable_price
                              0x61, 0x62, 0x63, 0x64,                          // near_execution_price
                              0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,  // near_execution_time
                              0x81, 0x82, 0x83, 0x84,                          // lower_price_range_collar
                              0x91, 0x92, 0x93, 0x94}),                        // upper_price_range_collar
                   layout({{"stock", 11, 8},
                           {"open_eligibility_status", 19, 1},
                           {"minimum_allowable_price", 20, 4},
                           {"maximum_allowable_price", 24, 4},
                           {"near_execution_price", 28, 4},
                           {"near_execution_time", 32, 8},
                           {"lower_price_range_collar", 40, 4},
                           {"upper_price_range_collar", 44, 4}})});

    return all;
}

}  // namespace

const std::vector<Fixture>& fixtures() {
    static const std::vector<Fixture> all = build();
    return all;
}

const Fixture& fixture(char type) {
    const auto& all = fixtures();
    const auto found =
        std::ranges::find_if(all, [type](const Fixture& f) { return f.type == type; });
    if (found == all.end()) {
        throw std::invalid_argument("no fixture for this type");
    }
    return *found;
}

}  // namespace tick_store::itch::test
