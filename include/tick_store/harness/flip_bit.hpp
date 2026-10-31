#pragma once

// G-2's fault injection. It lives in the harness and works on a file from the outside:
// no store or production code has a hook for it. Flipping the same bit twice restores
// the file.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace tick_store::harness {

struct FlippedByte {
    std::uint8_t before = 0;
    std::uint8_t after = 0;
};

// Flips bit `bit` of the byte at `byte_offset`, in place. Bit 0 is the least
// significant bit of the byte, the same numbering the oracle reports. Fails, changing
// nothing, if `bit` is above 7 or the byte is not in the file.
[[nodiscard]] std::expected<FlippedByte, std::string> flip_bit(const std::filesystem::path& path,
                                                               std::uint64_t byte_offset,
                                                               unsigned bit);

}  // namespace tick_store::harness
