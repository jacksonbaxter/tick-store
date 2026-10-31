// SHA-256 against the FIPS 180 examples and the padding edges. The digests of the
// boundary lengths come from an independent implementation, not from this one.

#include "tick_store/harness/sha256.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::harness {
namespace {

std::span<const std::byte> as_bytes(std::string_view text) {
    return std::as_bytes(std::span{text});
}

std::string hex_of(std::string_view text) { return to_hex(sha256(as_bytes(text))); }

TEST(Sha256, Fips180Vectors) {
    EXPECT_EQ(hex_of(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hex_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                     "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, Fips180MillionA) {
    const std::string million(1'000'000, 'a');
    EXPECT_EQ(hex_of(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// A message of 55 bytes just fits its padding in one block, and 56 needs a second.
// The other lengths sit around the block size and the same edge one block later.
TEST(Sha256, PaddingBoundaries) {
    const auto hex_of_length = [](std::size_t length) { return hex_of(std::string(length, 'a')); };
    EXPECT_EQ(hex_of_length(55), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_EQ(hex_of_length(56), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    EXPECT_EQ(hex_of_length(57), "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6");
    EXPECT_EQ(hex_of_length(63), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    EXPECT_EQ(hex_of_length(64), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    EXPECT_EQ(hex_of_length(65), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    EXPECT_EQ(hex_of_length(119), "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
    EXPECT_EQ(hex_of_length(120), "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

TEST(Sha256, EverySplitOfAMessageGivesTheSameDigest) {
    std::vector<std::byte> message(200);
    for (std::size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<std::byte>(i * 37U);
    }
    const Sha256Digest whole = sha256(message);
    for (std::size_t split = 0; split <= message.size(); ++split) {
        Sha256 hash;
        hash.update(std::span{message}.first(split));
        hash.update(std::span{message}.subspan(split));
        EXPECT_EQ(hash.finish(), whole) << "split at " << split;
    }
}

TEST(Sha256, OneByteAtATimeGivesTheSameDigest) {
    const std::string text(1000, 'x');
    Sha256 hash;
    for (const std::byte b : as_bytes(text)) {
        hash.update(std::span{&b, 1});
    }
    EXPECT_EQ(to_hex(hash.finish()), hex_of(text));
}

TEST(Sha256Hex, RoundTrips) {
    const Sha256Digest digest = sha256(as_bytes("abc"));
    const std::string text = to_hex(digest);
    EXPECT_EQ(parse_sha256_hex(text), digest);

    std::string upper = text;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    EXPECT_EQ(parse_sha256_hex(upper), digest);
}

TEST(Sha256Hex, RefusesAnythingElse) {
    const std::string good(64, '0');
    EXPECT_TRUE(parse_sha256_hex(good).has_value());
    EXPECT_FALSE(parse_sha256_hex("").has_value());
    EXPECT_FALSE(parse_sha256_hex(good.substr(1)).has_value());
    EXPECT_FALSE(parse_sha256_hex(good + "0").has_value());
    for (const char bad : {'g', 'G', ' ', '/', ':', '@', '`'}) {
        std::string text = good;
        text[63] = bad;
        EXPECT_FALSE(parse_sha256_hex(text).has_value()) << "character " << bad;
        text = good;
        text[0] = bad;
        EXPECT_FALSE(parse_sha256_hex(text).has_value()) << "character " << bad;
    }
}

}  // namespace
}  // namespace tick_store::harness
