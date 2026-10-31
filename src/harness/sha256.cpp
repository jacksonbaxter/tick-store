#include "tick_store/harness/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::harness {
namespace {

// FIPS 180-4 section 4.2.2.
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

// Where the 8-byte message length sits in the last block.
constexpr std::size_t kLengthOffset = 56;

[[nodiscard]] constexpr std::uint32_t load_be32(std::span<const std::byte, 4> bytes) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

void Sha256::compress(std::span<const std::byte, kBlockSize> block) noexcept {
    std::array<std::uint32_t, 64> w;  // NOLINT(cppcoreguidelines-pro-type-member-init): filled below
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = load_be32(block.subspan(t * 4).first<4>());
    }
    for (std::size_t t = 16; t < w.size(); ++t) {
        const std::uint32_t s0 =
            std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3U);
        const std::uint32_t s1 =
            std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10U);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t t = 0; t < w.size(); ++t) {
        const std::uint32_t big1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + big1 + ch + kRoundConstants[t] + w[t];
        const std::uint32_t big0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = big0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::byte> bytes) noexcept {
    total_bytes_ += bytes.size();

    if (pending_size_ != 0) {
        const std::size_t take = std::min(kBlockSize - pending_size_, bytes.size());
        std::ranges::copy(bytes.first(take), std::span{pending_}.subspan(pending_size_).begin());
        pending_size_ += take;
        bytes = bytes.subspan(take);
        if (pending_size_ < kBlockSize) {
            return;
        }
        compress(pending_);
        pending_size_ = 0;
    }

    while (bytes.size() >= kBlockSize) {
        compress(bytes.first<kBlockSize>());
        bytes = bytes.subspan(kBlockSize);
    }

    std::ranges::copy(bytes, pending_.begin());
    pending_size_ = bytes.size();
}

Sha256Digest Sha256::finish() noexcept {
    // total_bytes_ is taken first, because the padding goes through update().
    const std::uint64_t total_bits = total_bytes_ * 8U;

    constexpr std::array<std::byte, 1> kOneBit{std::byte{0x80}};
    update(kOneBit);
    constexpr std::array<std::byte, kBlockSize> kZeros{};
    update(std::span{kZeros}.first((kBlockSize + kLengthOffset - pending_size_) % kBlockSize));

    std::array<std::byte, 8> length{};
    for (std::size_t i = 0; i < length.size(); ++i) {
        length[i] = static_cast<std::byte>((total_bits >> (8U * (7U - i))) & 0xFFU);
    }
    update(length);

    Sha256Digest digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[(i * 4) + j] = static_cast<std::byte>((state_[i] >> (8U * (3U - j))) & 0xFFU);
        }
    }
    return digest;
}

Sha256Digest sha256(std::span<const std::byte> bytes) noexcept {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

std::string to_hex(const Sha256Digest& digest) {
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::byte b : digest) {
        text += std::format("{:02x}", std::to_integer<unsigned>(b));
    }
    return text;
}

std::optional<Sha256Digest> parse_sha256_hex(std::string_view text) noexcept {
    if (text.size() != kSha256Size * 2) {
        return std::nullopt;
    }
    Sha256Digest digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = hex_value(text[i * 2]);
        const int low = hex_value(text[(i * 2) + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        digest[i] = static_cast<std::byte>((static_cast<unsigned>(high) << 4U) |
                                           static_cast<unsigned>(low));
    }
    return digest;
}

}  // namespace tick_store::harness
