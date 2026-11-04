// Gorilla XOR on 64-bit words and doubles, and the price-to-double mappings: golden
// bytes both ways, every window edge, bit-exact doubles, one corrupt fixture per check,
// truncation at every byte length, and the encoded size pinned.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/price_double.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::codec {
namespace {

using itch::test::bytes;
using itch::test::exact_copy;
using Bytes = std::vector<std::byte>;
using Words = std::vector<std::uint64_t>;

constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

CodecError error_of(CodecErrorKind kind, std::string_view field, std::uint64_t offset,
                    std::uint64_t expected, std::uint64_t actual) {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

// Every control in turn, worked out bit by bit from the format in gorilla_xor.hpp:
//   0x4059000000000000  100.0, the first word
//   0x4059000000000000  X = 0                           '0'
//   0x4059400000000000  X = 1 << 46                     '11' L=17 B=1, then 1
//   0x4059000000000000  X = 1 << 46, fits               '10' then 1
//   0x4059000000000001  X = 1, leading zeros capped     '11' L=31 B=33, then 33 bits of 1
//   0xBFA6FFFFFFFFFFFE  X = all ones                    '11' L=0 B=64, then 64 ones
//   0x3FA6FFFFFFFFFFFF  X = 0x8000000000000001, fits    '10' then 64 bits
// 271 bits, so 34 bytes with one bit of padding.
constexpr std::array<std::uint64_t, 7> kGoldenWords{
    0x4059000000000000ULL, 0x4059000000000000ULL, 0x4059400000000000ULL, 0x4059000000000000ULL,
    0x4059000000000001ULL, 0xBFA6FFFFFFFFFFFEULL, 0x3FA6FFFFFFFFFFFFULL};

const Words& golden_words() {
    static const Words words(kGoldenWords.begin(), kGoldenWords.end());
    return words;
}

const Bytes& golden_block() {
    static const Bytes block = bytes({
        0x07, 0x00, 0x00, 0x00,                          // count 7
        0x22, 0x00, 0x00, 0x00,                          // payload 34 bytes
        0x40, 0x59, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // first word
        0x71, 0x03, 0x7F, 0xC0, 0x00, 0x00, 0x00, 0x01,  // 0, 11 10001 000000 1, 10 1, 11 ...
        0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // ... 11 00000 111111 ones ...
        0xFF, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ... 10 0x8000000000000001 ...
        0x00, 0x02,                                      // ... and one bit of padding
    });
    return block;
}

Bytes encoded(const Words& words, std::size_t block_values = kDefaultBlockValues,
              XorStats* stats = nullptr) {
    Bytes out;
    const auto done = encode_xor(words, block_values, out, stats);
    EXPECT_TRUE(done.has_value()) << to_string(done.error());
    return out;
}

std::expected<Words, CodecError> decoded(std::span<const std::byte> column, std::uint64_t count) {
    Words out;
    const Bytes input = exact_copy(column);
    if (const auto done = decode_xor(input, count, out); !done.has_value()) {
        return std::unexpected(done.error());
    }
    return out;
}

std::expected<Words, CodecError> decoded_block(const Bytes& block) {
    Words out;
    const Bytes input = exact_copy(block);
    const auto taken = decode_xor_block(input, out);
    if (!taken.has_value()) {
        return std::unexpected(taken.error());
    }
    EXPECT_EQ(*taken, input.size());
    return out;
}

std::string bits_of(std::uint64_t value, unsigned width) {
    std::string out;
    for (unsigned i = width; i > 0; --i) {
        out += ((value >> (i - 1U)) & 1U) != 0 ? '1' : '0';
    }
    return out;
}

// A block with a hand-built payload: a 64-bit first word of zero, then `bits`, given as
// a string of 0 and 1, padded with zeros to a whole byte.
Bytes block_with_bits(std::uint32_t count, std::string_view bits) {
    Bytes payload(8, std::byte{0});
    unsigned filled = 0;
    for (const char bit : bits) {
        if (filled % 8 == 0) {
            payload.push_back(std::byte{0});
        }
        if (bit == '1') {
            payload.back() |= static_cast<std::byte>(0x80U >> (filled % 8U));
        }
        ++filled;
    }
    Bytes block;
    const std::size_t at = begin_block(block, count);
    patch_block_header(block, at, static_cast<std::uint32_t>(payload.size()));
    block.insert(block.end(), payload.begin(), payload.end());
    return block;
}

// '11', L, B - 1, then the data bits: a new window.
std::string new_window(unsigned leading, unsigned bits, std::uint64_t data) {
    EXPECT_LE(leading, kXorMaxLeading) << "a fixture's leading count does not fit 5 bits";
    return "11" + bits_of(leading, 5) + bits_of(bits - 1U, 6) + bits_of(data, bits);
}

TEST(GorillaXor, GoldenBytesBothWays) {
    XorStats stats;
    EXPECT_EQ(encoded(golden_words(), kDefaultBlockValues, &stats), golden_block());
    EXPECT_EQ(decoded_block(golden_block()), golden_words());
    EXPECT_EQ(stats.zero, 1U);
    EXPECT_EQ(stats.reuse, 2U);
    EXPECT_EQ(stats.new_window, 3U);
    EXPECT_EQ(stats.data_bits, 1U + 1U + 33U + 64U + 64U);
}

TEST(GorillaXor, EmptyColumnHasNoBlocks) {
    EXPECT_TRUE(encoded({}).empty());
    EXPECT_EQ(decoded({}, 0), Words{});
}

struct WindowCase {
    const char* label;
    std::uint64_t first;
    std::uint64_t second;
    std::uint64_t third;
    // Bits the second and third words take after the first.
    unsigned bits;
};

// The edges of the window: leading zeros 0, 30, 31 and past the cap; trailing zeros 0
// and 63; one data bit and 64; a third word that fits the window exactly, and one that
// misses it by one bit on either side.
std::vector<WindowCase> window_edges() {
    constexpr unsigned kNew = 2 + 5 + 6;
    return {
        {"B=64, then exact reuse", 0, kAllOnes, 0, kNew + 64 + 2 + 64},
        {"L=0, T=63, one bit", 0, std::uint64_t{1} << 63U, 0, kNew + 1 + 2 + 1},
        {"L=63 capped to 31, T=0", 0, 1, 0, kNew + 33 + 2 + 33},
        {"L=31 exactly", 0, std::uint64_t{1} << 32U, 0, kNew + 1 + 2 + 1},
        {"L=30", 0, std::uint64_t{1} << 33U, 0, kNew + 1 + 2 + 1},
        {"L=32 capped, B=1 would need L=32", 0, std::uint64_t{1} << 31U, 0, kNew + 1 + 2 + 1},
        // The second X is 0x0090..., 0x0110... and 0x0098... in turn, against a window of
        // L=8, B=4 from the first X, 0x00F0....
        {"reuse fits exactly", 0, 0x00F0000000000000ULL, 0x0060000000000000ULL,
         kNew + 4 + 2 + 4},
        {"misses by one bit on the left", 0, 0x00F0000000000000ULL, 0x01E0000000000000ULL,
         kNew + 4 + kNew + 5},
        {"misses by one bit on the right", 0, 0x00F0000000000000ULL, 0x0068000000000000ULL,
         kNew + 4 + kNew + 5},
    };
}

// Each edge alone: its exact cost in bits and its round trip.
TEST(GorillaXor, EveryWindowEdgeCostsItsBitsAndRoundTrips) {
    for (const WindowCase& edge : window_edges()) {
        SCOPED_TRACE(edge.label);
        const Words words{edge.first, edge.second, edge.third};
        const Bytes block = encoded(words);
        EXPECT_EQ(block.size(), kBlockHeaderSize + ((64U + edge.bits + 7U) / 8U));
        EXPECT_EQ(decoded_block(block), words);
    }
}

TEST(GorillaXor, ExtremesRoundTripAtEveryBlockSize) {
    const Words words{0, kAllOnes, 0, 1, std::uint64_t{1} << 63U, kAllOnes, kAllOnes, 0,
                      0x8000000000000001ULL, 0x7FFFFFFFFFFFFFFEULL, 0, 0};
    for (const std::size_t block_values : {std::size_t{1}, std::size_t{2}, std::size_t{5},
                                           std::size_t{kDefaultBlockValues}}) {
        SCOPED_TRACE(block_values);
        EXPECT_EQ(decoded(encoded(words, block_values), words.size()), words);
    }
}

TEST(GorillaXor, SeededWordsRoundTrip) {
    test::SplitMix64 random{11};
    Words words;
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t pick = random() % 4;
        const std::uint64_t value = random();
        // Repeats, small changes in a narrow window, and anything at all.
        if (pick == 0 && !words.empty()) {
            words.push_back(words.back());
        } else if (pick == 1 && !words.empty()) {
            words.push_back(words.back() ^ ((value & 0xFFU) << 20U));
        } else {
            words.push_back(value >> (value % 64));
        }
    }
    for (const std::size_t block_values :
         {std::size_t{1}, std::size_t{3}, std::size_t{1000}, std::size_t{kMaxBlockValues}}) {
        SCOPED_TRACE(block_values);
        EXPECT_EQ(decoded(encoded(words, block_values), words.size()), words);
    }
}

// Bit patterns, never ==, so NaN payloads and the sign of zero are checked (T2).
TEST(GorillaXor, DoublesRoundTripBitForBit) {
    const std::array<std::uint64_t, 12> patterns{
        std::bit_cast<std::uint64_t>(0.0),
        std::bit_cast<std::uint64_t>(-0.0),
        std::bit_cast<std::uint64_t>(123.45),
        std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()),
        std::bit_cast<std::uint64_t>(-std::numeric_limits<double>::infinity()),
        0x7FF8000000000000ULL,  // the default quiet NaN
        0x7FF8000000000001ULL,  // a quiet NaN with a payload
        0xFFF0000000000001ULL,  // a negative signalling NaN
        std::bit_cast<std::uint64_t>(std::numeric_limits<double>::denorm_min()),
        std::bit_cast<std::uint64_t>(std::numeric_limits<double>::max()),
        std::bit_cast<std::uint64_t>(std::numeric_limits<double>::lowest()),
        std::bit_cast<std::uint64_t>(0.1),
    };
    std::vector<double> values;
    values.reserve(patterns.size());
    for (const std::uint64_t pattern : patterns) {
        values.push_back(std::bit_cast<double>(pattern));
    }
    Bytes column;
    ASSERT_TRUE(encode_xor_doubles(values, 5, column).has_value());
    std::vector<double> back;
    ASSERT_TRUE(decode_xor_doubles(exact_copy(column), values.size(), back).has_value());
    ASSERT_EQ(back.size(), patterns.size());
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(back[i]), patterns[i]) << "value " << i;
    }
}

// A silent change in the format changes these sizes (T4).
TEST(GorillaXor, EncodedSizeIsPinned) {
    EXPECT_EQ(golden_block().size(), 42U);

    // Dollar prices on a random walk of whole cents near $50, as XOR on doubles sees them.
    test::SplitMix64 random{3};
    std::vector<double> dollars;
    std::uint32_t price = 500'000;
    for (int i = 0; i < 10'000; ++i) {
        price = price + (static_cast<std::uint32_t>(random() % 11) * 100U) - 500U;
        dollars.push_back(price4_to_dollars(price));
    }
    XorStats stats;
    Bytes column;
    ASSERT_TRUE(encode_xor_doubles(dollars, kDefaultBlockValues, column, &stats).has_value());
    EXPECT_EQ(column.size(), 58'649U);
    EXPECT_EQ(stats.zero, 856U);
    EXPECT_EQ(stats.reuse, 9'086U);
    EXPECT_EQ(stats.new_window, 55U);
}

TEST(GorillaXor, EncoderRefusesBadBlockSizes) {
    Bytes out;
    EXPECT_EQ(encode_xor(golden_words(), 0, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_EQ(encode_xor_block({}, out),
              std::unexpected(error_of(CodecErrorKind::bad_parameter, "block values", 0,
                                       kMaxBlockValues, 0)));
    EXPECT_TRUE(out.empty());
}

// One fixture per check in the decoder, each asserting the exact error (T1). Offsets
// are bits from the start of the block; the first word ends at bit 128.
TEST(GorillaXor, EachCorruptionHasItsError) {
    // Header checks.
    EXPECT_EQ(decoded_block(Bytes{}),
              std::unexpected(error_of(CodecErrorKind::truncated, "block header", 0, 64, 0)));
    EXPECT_EQ(decoded_block(block_with_bits(0, "")),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 1, 0)));
    EXPECT_EQ(decoded_block(block_with_bits(kMaxBlockValues + 1, "")),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0,
                                       kMaxBlockValues, kMaxBlockValues + 1)));
    EXPECT_EQ(decoded_block(block_with_bits(10, "0")),
              std::unexpected(error_of(CodecErrorKind::bad_count, "block count", 0, 9, 10)));
    Bytes cut_payload = block_with_bits(2, "0");
    cut_payload.pop_back();
    EXPECT_EQ(decoded_block(cut_payload),
              std::unexpected(error_of(CodecErrorKind::truncated, "block payload", 64, 72, 64)));

    // D4: a reuse before any window, and a window wider than the word.
    EXPECT_EQ(decoded_block(block_with_bits(2, "10" + bits_of(1, 6))),
              std::unexpected(error_of(CodecErrorKind::bad_window, "xor window reuse", 128, 1, 0)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "11" + bits_of(31, 5) + bits_of(63, 6))),
              std::unexpected(error_of(CodecErrorKind::bad_window, "xor window", 128, 64, 95)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "11" + bits_of(1, 5) + bits_of(63, 6))),
              std::unexpected(error_of(CodecErrorKind::bad_window, "xor window", 128, 64, 65)));

    // Not what the encoder writes: data bits ending in zero, a leading count below the
    // cap whose first data bit is zero, a new window where the old one fitted, and a
    // reuse that changes nothing.
    EXPECT_EQ(decoded_block(block_with_bits(2, new_window(20, 4, 0b1010))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "xor trailing zeros", 128, 41, 40)));
    EXPECT_EQ(decoded_block(block_with_bits(2, new_window(20, 4, 0))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "xor trailing zeros", 128, 64, 40)));
    EXPECT_EQ(decoded_block(block_with_bits(2, new_window(20, 4, 0b0101))),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "xor leading zeros", 128, 21, 20)));
    const std::string fits_old_window = new_window(24, 8, 0xFF) + new_window(25, 7, 0x41);
    EXPECT_EQ(decoded_block(block_with_bits(3, fits_old_window)),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "xor new window", 149, 2, 3)));
    EXPECT_EQ(decoded_block(block_with_bits(3, new_window(28, 4, 0b1001) + "10" + "0000")),
              std::unexpected(
                  error_of(CodecErrorKind::non_canonical, "xor reused data bits", 145, 0, 2)));

    // Each field cut short, in stream order.
    EXPECT_EQ(decoded_block(block_with_bits(9, "00000001")),
              std::unexpected(error_of(CodecErrorKind::truncated, "xor control", 136, 1, 0)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "11000")),
              std::unexpected(error_of(CodecErrorKind::truncated, "xor data length", 135, 6, 1)));
    EXPECT_EQ(decoded_block(block_with_bits(6, "000011")),
              std::unexpected(
                  error_of(CodecErrorKind::truncated, "xor leading zeros", 134, 5, 2)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "11" + bits_of(0, 5) + bits_of(9, 6))),
              std::unexpected(error_of(CodecErrorKind::truncated, "xor data bits", 141, 10, 3)));
    EXPECT_EQ(decoded_block(block_with_bits(3, new_window(0, 8, 0x81) + "10")),
              std::unexpected(error_of(CodecErrorKind::truncated, "xor data bits", 151, 8, 1)));

    // Padding that is not zero, and a spare byte after the last value.
    EXPECT_EQ(decoded_block(block_with_bits(2, "00000001")),
              std::unexpected(error_of(CodecErrorKind::trailing_bits, "block padding", 129, 0, 1)));
    EXPECT_EQ(decoded_block(block_with_bits(2, "000000000")),
              std::unexpected(
                  error_of(CodecErrorKind::trailing_bits, "block padding", 129, 0, 15)));
}

TEST(GorillaXor, ColumnCountMustMatchExactly) {
    const Words words{1, 2, 3, 4, 5, 6, 7};
    const Bytes column = encoded(words, 3);
    EXPECT_EQ(decoded(column, 7), words);
    EXPECT_FALSE(decoded(column, 6).has_value());
    EXPECT_FALSE(decoded(column, 8).has_value());
    EXPECT_FALSE(decoded(column, 5).has_value());
}

// A whole column cut at every byte length is refused, never read as zeros (D10).
TEST(GorillaXor, TruncationAtEveryByteLengthIsAnError) {
    const Bytes column = encoded(golden_words(), 3);
    for (std::size_t length = 0; length < column.size(); ++length) {
        SCOPED_TRACE(length);
        EXPECT_FALSE(decoded(std::span{column}.first(length), golden_words().size()).has_value());
    }
    for (std::size_t length = 0; length < golden_block().size(); ++length) {
        SCOPED_TRACE(length);
        Words out{42};
        const Bytes cut = exact_copy(std::span{golden_block()}.first(length));
        EXPECT_FALSE(decode_xor_block(cut, out).has_value());
        EXPECT_EQ(out, Words{42});
    }
}

std::uint64_t bits_of_double(double value) { return std::bit_cast<std::uint64_t>(value); }

CodecError not_exact(double value) {
    return error_of(CodecErrorKind::not_exact_price, "price as double", 0, 0,
                    bits_of_double(value));
}

TEST(PriceDouble, DollarsAreThePriceOverTenThousand) {
    EXPECT_EQ(bits_of_double(price4_to_dollars(1'234'500)), bits_of_double(123.45));
    EXPECT_EQ(bits_of_double(price4_to_dollars(1)), bits_of_double(0.0001));
    EXPECT_EQ(bits_of_double(price4_to_dollars(0)), bits_of_double(0.0));
    EXPECT_EQ(bits_of_double(price8_to_dollars(320'000'000'000ULL)), bits_of_double(3200.0));
}

// The edges of Price(4) and a stride across all of it. tools/tick_codec_eval checks all
// 2^32 values; this keeps a fast check in every test build.
TEST(PriceDouble, Price4RoundTripsExactly) {
    const std::array<std::uint32_t, 10> edges{0, 1, 99, 100, 9'999, 10'000, 1'234'500,
                                              1'999'999'900, 4'294'967'294, 4'294'967'295};
    for (const std::uint32_t price : edges) {
        EXPECT_EQ(dollars_to_price4(price4_to_dollars(price)), price) << price;
    }
    for (std::uint64_t price = 0; price <= 0xFFFF'FFFFULL; price += 65'521) {
        const auto back = dollars_to_price4(price4_to_dollars(static_cast<std::uint32_t>(price)));
        ASSERT_EQ(back, price) << price;
    }
}

TEST(PriceDouble, Price8AndIntegersRoundTripUpTo2To53) {
    for (const std::uint64_t price : {std::uint64_t{0}, std::uint64_t{1},
                                      std::uint64_t{320'000'000'000ULL}, std::uint64_t{1} << 40U}) {
        EXPECT_EQ(dollars_to_price8(price8_to_dollars(price)), price) << price;
    }
    constexpr std::uint64_t kExact = std::uint64_t{1} << 53U;
    EXPECT_EQ(double_to_scaled(scaled_to_double(kExact, 1), 1, kAllOnes), kExact);
    // 2^53 + 1 is not a double: the mapping cannot bring it back, and says so by
    // returning another integer, which the caller's comparison then reports.
    EXPECT_NE(double_to_scaled(scaled_to_double(kExact + 1, 1), 1, kAllOnes), kExact + 1);
}

TEST(PriceDouble, RefusesEveryDoubleThatIsNotAPrice) {
    const double between = std::nextafter(123.45, 200.0);
    const double above = (4'294'967'295.0 / 10'000.0) + 1.0;
    const auto nan = std::bit_cast<double>(0x7FF8000000000001ULL);
    const double inf = std::numeric_limits<double>::infinity();
    for (const double value : {between, above, nan, inf, -inf, -0.0, -0.0001}) {
        SCOPED_TRACE(bits_of_double(value));
        EXPECT_EQ(dollars_to_price4(value), std::unexpected(not_exact(value)));
    }
    EXPECT_EQ(dollars_to_price8(std::numeric_limits<double>::max()),
              std::unexpected(not_exact(std::numeric_limits<double>::max())));
    EXPECT_EQ(double_to_scaled(0.5, 1, kAllOnes), std::unexpected(not_exact(0.5)));
    EXPECT_EQ(double_to_scaled(7.0, 1, 6), std::unexpected(not_exact(7.0)));
}

}  // namespace
}  // namespace tick_store::codec
