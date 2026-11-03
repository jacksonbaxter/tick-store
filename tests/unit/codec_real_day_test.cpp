// The Gorilla codecs on the start of the pinned day: every timestamp and every add-order
// price of the first million records, encoded at the default block size, decoded and
// compared exactly, with the encoded sizes pinned. The sizes were checked against an
// independent model of the formats, not only taken from this code. The data file is not in the
// repository, so a machine without it skips, and says so. Whole days belong to
// tools/tick_codec_eval.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace tick_store::codec {
namespace {

constexpr auto kDayFile = "12302019.NASDAQ_ITCH50";
constexpr std::size_t kMessages = 1'000'000;
// No type is longer than 50 bytes, plus its prefix.
constexpr std::size_t kBytesNeeded = kMessages * (50 + itch::kFramePrefixSize);

struct Columns {
    std::vector<std::uint64_t> timestamps;
    std::vector<std::uint64_t> add_prices;
};

Columns read_columns(const std::filesystem::path& path) {
    std::vector<char> file(kBytesNeeded);
    std::ifstream in{path, std::ios::binary};
    in.read(file.data(), static_cast<std::streamsize>(file.size()));
    file.resize(static_cast<std::size_t>(in.gcount()));
    itch::FrameCursor cursor{std::as_bytes(std::span{file})};
    Columns columns;
    for (std::size_t i = 0; i < kMessages; ++i) {
        const auto frame = cursor.next();
        if (!frame.has_value()) {
            ADD_FAILURE() << "frame " << i << " did not read";
            break;
        }
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            ADD_FAILURE() << "message " << i << " did not decode";
            break;
        }
        std::visit(
            [&](const auto& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (!std::is_same_v<T, itch::RawMessage>) {
                    columns.timestamps.push_back(m.header.timestamp_ns);
                }
                if constexpr (std::is_same_v<T, itch::AddOrder> ||
                              std::is_same_v<T, itch::AddOrderWithMpid>) {
                    columns.add_prices.push_back(static_cast<std::uint32_t>(m.price));
                }
            },
            *message);
    }
    return columns;
}

TEST(ItchRealDay, GorillaCodecsRoundTripTheFirstMillionRecords) {
    const std::filesystem::path path = std::filesystem::path{TICK_STORE_DATA_DIR} / kDayFile;
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "NOT CHECKED: " << kDayFile
                     << " is not in the data directory, so the codecs did not run on it. "
                        "Fetch it with scripts/fetch_data.py.";
    }
    const Columns columns = read_columns(path);
    ASSERT_EQ(columns.timestamps.size(), kMessages);
    ASSERT_EQ(columns.add_prices.size(), 307'026U);

    std::vector<std::byte> timestamps;
    ASSERT_TRUE(encode_timestamps(columns.timestamps, kDefaultBlockValues, timestamps));
    std::vector<std::uint64_t> back;
    ASSERT_TRUE(decode_timestamps(timestamps, kMessages, back));
    EXPECT_EQ(back, columns.timestamps);
    EXPECT_EQ(timestamps.size(), 3'823'166U);

    // XOR on the dollar doubles, each brought back to its exact integer price.
    std::vector<double> dollars;
    std::vector<std::uint64_t> integers_as_doubles;
    for (const std::uint64_t price : columns.add_prices) {
        dollars.push_back(price4_to_dollars(static_cast<std::uint32_t>(price)));
        integers_as_doubles.push_back(
            std::bit_cast<std::uint64_t>(scaled_to_double(price, 1)));
    }
    std::vector<std::byte> dollar_column;
    ASSERT_TRUE(encode_xor_doubles(dollars, kDefaultBlockValues, dollar_column));
    std::vector<double> dollars_back;
    ASSERT_TRUE(decode_xor_doubles(dollar_column, dollars.size(), dollars_back));
    ASSERT_EQ(dollars_back.size(), columns.add_prices.size());
    for (std::size_t i = 0; i < dollars_back.size(); ++i) {
        const auto price = dollars_to_price4(dollars_back[i]);
        ASSERT_TRUE(price.has_value()) << "value " << i << ": " << to_string(price.error());
        ASSERT_EQ(*price, columns.add_prices[i]) << "value " << i;
    }
    EXPECT_EQ(dollar_column.size(), 2'303'797U);

    std::vector<std::byte> integer_column;
    ASSERT_TRUE(encode_xor(integers_as_doubles, kDefaultBlockValues, integer_column));
    std::vector<std::uint64_t> integers_back;
    ASSERT_TRUE(decode_xor(integer_column, integers_as_doubles.size(), integers_back));
    EXPECT_EQ(integers_back, integers_as_doubles);
    EXPECT_EQ(integer_column.size(), 978'526U);

    std::vector<std::byte> raw_column;
    ASSERT_TRUE(encode_xor(columns.add_prices, kDefaultBlockValues, raw_column));
    std::vector<std::uint64_t> raw_back;
    ASSERT_TRUE(decode_xor(raw_column, columns.add_prices.size(), raw_back));
    EXPECT_EQ(raw_back, columns.add_prices);
    EXPECT_EQ(raw_column.size(), 1'227'691U);
}

}  // namespace
}  // namespace tick_store::codec
