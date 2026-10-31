#pragma once

// SHA-256 from FIPS 180-4, written here so the correctness oracle adds no crypto
// dependency. It is a checksum for the oracle, not a security boundary.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::harness {

inline constexpr std::size_t kSha256Size = 32;

using Sha256Digest = std::array<std::byte, kSha256Size>;

// Streaming hash. Feed update() any number of times, then call finish() once.
class Sha256 {
public:
    void update(std::span<const std::byte> bytes) noexcept;

    // Pads, and returns the digest of everything fed so far. The object is spent:
    // it must not be used again.
    [[nodiscard]] Sha256Digest finish() noexcept;

private:
    static constexpr std::size_t kBlockSize = 64;

    void compress(std::span<const std::byte, kBlockSize> block) noexcept;

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::byte, kBlockSize> pending_{};
    std::size_t pending_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> bytes) noexcept;

// Lowercase hex, 64 characters.
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);

// Exactly 64 hex characters, in either case, or nullopt.
[[nodiscard]] std::optional<Sha256Digest> parse_sha256_hex(std::string_view text) noexcept;

}  // namespace tick_store::harness
