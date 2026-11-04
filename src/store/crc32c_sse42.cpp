// The SSE4.2 CRC32C kernel, built only on x86-64. The crc32 instruction is enabled for
// the kernel function alone, by a target attribute, so the rest of this file and the
// program stay on the baseline instruction set, and the dispatcher calls the kernel
// only after cpu_has_sse42() has said yes.

#include "crc32c_kernels.hpp"

#include <cpuid.h>
#include <nmmintrin.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// The instruction takes its operand as a little-endian value.
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

bool cpu_has_sse42() noexcept {
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (ecx & bit_SSE4_2) != 0;
}

__attribute__((target("sse4.2"))) std::uint32_t crc32c_sse42(
    std::uint32_t state, std::span<const std::byte> bytes) noexcept {
    std::uint64_t wide = state;
    while (bytes.size() >= 8) {
        wide = _mm_crc32_u64(wide, load<std::uint64_t>(bytes));
        bytes = bytes.subspan(8);
    }
    // The instruction's 64-bit form leaves the high half zero.
    state = static_cast<std::uint32_t>(wide);
    // The tail by exact reads: at most one of each width.
    if (bytes.size() >= 4) {
        state = _mm_crc32_u32(state, load<std::uint32_t>(bytes));
        bytes = bytes.subspan(4);
    }
    if (bytes.size() >= 2) {
        state = _mm_crc32_u16(state, load<std::uint16_t>(bytes));
        bytes = bytes.subspan(2);
    }
    if (!bytes.empty()) {
        state = _mm_crc32_u8(state, std::to_integer<std::uint8_t>(bytes[0]));
    }
    return state;
}

}  // namespace tick_store::store::detail
