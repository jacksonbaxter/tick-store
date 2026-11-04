#pragma once

// The CRC32C kernels behind crc32c.hpp. Each takes and returns the running state, the
// CRC with its bits inverted, so a caller inverts once at each end. Each reads exactly
// the bytes of its span.
//
// A hardware kernel lives in its own translation unit, compiled only for its
// architecture, so no instruction it uses can reach code that runs on a CPU without
// it. The build lists the file for the target processor, and defines
// TICK_STORE_CRC32C_ARMV8 or TICK_STORE_CRC32C_SSE42 when it does.

#include <cstddef>
#include <cstdint>
#include <span>

namespace tick_store::store::detail {

[[nodiscard]] std::uint32_t crc32c_software(std::uint32_t state,
                                            std::span<const std::byte> bytes) noexcept;

#ifdef TICK_STORE_CRC32C_ARMV8
[[nodiscard]] std::uint32_t crc32c_armv8(std::uint32_t state,
                                         std::span<const std::byte> bytes) noexcept;
#endif

#ifdef TICK_STORE_CRC32C_SSE42
// True if this CPU has SSE4.2, from CPUID leaf 1, ECX bit 20.
[[nodiscard]] bool cpu_has_sse42() noexcept;

// Callable only when cpu_has_sse42() is true.
[[nodiscard]] std::uint32_t crc32c_sse42(std::uint32_t state,
                                         std::span<const std::byte> bytes) noexcept;
#endif

}  // namespace tick_store::store::detail
