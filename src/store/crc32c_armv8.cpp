// The ARMv8 CRC32C kernel. Built only on arm64, and only when the compiler's baseline
// target has the CRC instructions, so nothing here needs a runtime check.

#include "crc32c_kernels.hpp"

#include <arm_acle.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#ifndef __ARM_FEATURE_CRC32
#error "crc32c_armv8.cpp needs a target with the ARMv8 CRC32 instructions"
#endif

// The instructions take their operand as a little-endian value.
static_assert(std::endian::native == std::endian::little);

namespace tick_store::store::detail {
namespace {

template <typename Word>
[[nodiscard]] Word load(std::span<const std::byte> bytes) noexcept {
    Word word = 0;
    std::memcpy(&word, bytes.data(), sizeof word);
    return word;
}

}  // namespace

std::uint32_t crc32c_armv8(std::uint32_t state, std::span<const std::byte> bytes) noexcept {
    while (bytes.size() >= 8) {
        state = __crc32cd(state, load<std::uint64_t>(bytes));
        bytes = bytes.subspan(8);
    }
    // The tail by exact reads: at most one of each width.
    if (bytes.size() >= 4) {
        state = __crc32cw(state, load<std::uint32_t>(bytes));
        bytes = bytes.subspan(4);
    }
    if (bytes.size() >= 2) {
        state = __crc32ch(state, load<std::uint16_t>(bytes));
        bytes = bytes.subspan(2);
    }
    if (!bytes.empty()) {
        state = __crc32cb(state, std::to_integer<std::uint8_t>(bytes[0]));
    }
    return state;
}

}  // namespace tick_store::store::detail
