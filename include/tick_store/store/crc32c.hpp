#pragma once

// CRC32C (Castagnoli, reflected polynomial 0x82F63B78), the checksum of the container
// format: the same function as iSCSI (RFC 3720), ext4 metadata and Prometheus chunks.
//
// Three kernels compute it, and all give the same value for the same bytes:
//
//   software  slicing-by-8 over constexpr tables, always built, any host
//   armv8     the ARMv8 CRC32C instructions, built on arm64 when the compiler's
//             baseline target has them (__ARM_FEATURE_CRC32, true for every Apple
//             arm64 target). There is no runtime check: the instructions are part of
//             the baseline, as NEON is
//   sse42     the SSE4.2 crc32 instruction, built on x86-64 and chosen at run time
//             from CPUID, read once and cached
//
// crc32c() uses the best kernel this build has and this CPU runs. Setting the
// environment variable TICK_STORE_CRC32C=software before the first call forces the
// software kernel for the life of the process, so a run can check that path in
// production code. crc32c_active_kernel() says which kernel is in use, so a test can
// fail when the one it expects did not run.
//
// Over-read contract: every kernel reads exactly the bytes of the span, and needs no
// padding after it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace tick_store::store {

enum class Crc32cKernel : std::uint8_t {
    software,
    armv8,
    sse42,
};

[[nodiscard]] std::string_view to_string(Crc32cKernel kernel) noexcept;

// The CRC32C of `bytes`, continuing from `crc`, which is 0 to start or the value an
// earlier call returned for the bytes before these. So crc32c(b, crc32c(a)) equals
// the CRC of a followed by b.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes,
                                   std::uint32_t crc = 0) noexcept;

// The same, with the kernel named, or nullopt if this build or this CPU lacks it.
[[nodiscard]] std::optional<std::uint32_t> crc32c_with(Crc32cKernel kernel,
                                                       std::span<const std::byte> bytes,
                                                       std::uint32_t crc = 0) noexcept;

[[nodiscard]] bool crc32c_kernel_available(Crc32cKernel kernel) noexcept;

// The kernel crc32c() uses, chosen on the first call to either function.
[[nodiscard]] Crc32cKernel crc32c_active_kernel() noexcept;

}  // namespace tick_store::store
