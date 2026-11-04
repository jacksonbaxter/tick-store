#include "tick_store/store/crc32c.hpp"

#include "crc32c_kernels.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace tick_store::store {
namespace detail {
namespace {

// The reflected Castagnoli polynomial.
constexpr std::uint32_t kPolynomial = 0x82F63B78U;

using Table = std::array<std::uint32_t, 256>;

// kTables[0] is the classic byte-at-a-time table. kTables[k] advances a byte's
// contribution by k more zero bytes, so eight lookups fold eight bytes at once.
constexpr std::array<Table, 8> kTables = [] {
    std::array<Table, 8> tables{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? kPolynomial : 0U);
        }
        tables[0][i] = crc;
    }
    for (std::size_t k = 1; k < tables.size(); ++k) {
        for (std::size_t i = 0; i < 256; ++i) {
            const std::uint32_t previous = tables[k - 1][i];
            tables[k][i] = (previous >> 8U) ^ tables[0][previous & 0xFFU];
        }
    }
    return tables;
}();

[[nodiscard]] constexpr std::uint32_t byte_step(std::uint32_t state, std::byte value) noexcept {
    return (state >> 8U) ^ kTables[0][(state ^ std::to_integer<std::uint32_t>(value)) & 0xFFU];
}

[[nodiscard]] std::uint64_t load_le64(std::span<const std::byte, 8> bytes) noexcept {
    std::uint64_t word = 0;
    std::memcpy(&word, bytes.data(), sizeof word);
    if constexpr (std::endian::native == std::endian::big) {
        word = std::byteswap(word);
    }
    return word;
}

[[nodiscard]] constexpr std::uint32_t lookup(std::size_t table, std::uint32_t value,
                                             unsigned shift) noexcept {
    return kTables[table][(value >> shift) & 0xFFU];
}

}  // namespace

std::uint32_t crc32c_software(std::uint32_t state, std::span<const std::byte> bytes) noexcept {
    while (bytes.size() >= 8) {
        const std::uint64_t word = load_le64(bytes.first<8>());
        const auto low = static_cast<std::uint32_t>(word) ^ state;
        const auto high = static_cast<std::uint32_t>(word >> 32U);
        state = lookup(7, low, 0) ^ lookup(6, low, 8) ^ lookup(5, low, 16) ^ lookup(4, low, 24) ^
                lookup(3, high, 0) ^ lookup(2, high, 8) ^ lookup(1, high, 16) ^
                lookup(0, high, 24);
        bytes = bytes.subspan(8);
    }
    for (const std::byte value : bytes) {
        state = byte_step(state, value);
    }
    return state;
}

}  // namespace detail

namespace {

#ifdef TICK_STORE_CRC32C_ARMV8
constexpr bool kArmv8Built = true;
#else
constexpr bool kArmv8Built = false;
#endif

[[nodiscard]] bool hardware_available(Crc32cKernel kernel) noexcept {
    switch (kernel) {
        case Crc32cKernel::software:
            return true;
        case Crc32cKernel::armv8:
            // Built only when the baseline target has the instructions.
            return kArmv8Built;
        case Crc32cKernel::sse42:
#ifdef TICK_STORE_CRC32C_SSE42
            return detail::cpu_has_sse42();
#else
            return false;
#endif
    }
    return false;
}

[[nodiscard]] Crc32cKernel choose_kernel() noexcept {
    // Read once, before any thread could change the environment: the first call
    // initialises the cached choice, and C++ makes that initialisation thread-safe.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, under the static's guard.
    const char* forced = std::getenv("TICK_STORE_CRC32C");
    if (forced != nullptr && std::string_view{forced} == "software") {
        return Crc32cKernel::software;
    }
    for (const Crc32cKernel kernel : {Crc32cKernel::armv8, Crc32cKernel::sse42}) {
        if (hardware_available(kernel)) {
            return kernel;
        }
    }
    return Crc32cKernel::software;
}

// The state with the kernel's bits inverted at each end, as CRC32C defines.
[[nodiscard]] std::uint32_t run(Crc32cKernel kernel, std::span<const std::byte> bytes,
                                std::uint32_t crc) noexcept {
    const std::uint32_t state = ~crc;
    switch (kernel) {
        case Crc32cKernel::software:
            return ~detail::crc32c_software(state, bytes);
        case Crc32cKernel::armv8:
#ifdef TICK_STORE_CRC32C_ARMV8
            return ~detail::crc32c_armv8(state, bytes);
#else
            break;
#endif
        case Crc32cKernel::sse42:
#ifdef TICK_STORE_CRC32C_SSE42
            return ~detail::crc32c_sse42(state, bytes);
#else
            break;
#endif
    }
    // Only reachable for a kernel the caller has proved available, so never.
    std::abort();
}

}  // namespace

std::string_view to_string(Crc32cKernel kernel) noexcept {
    switch (kernel) {
        case Crc32cKernel::software:
            return "software";
        case Crc32cKernel::armv8:
            return "armv8";
        case Crc32cKernel::sse42:
            return "sse42";
    }
    return "invalid";
}

bool crc32c_kernel_available(Crc32cKernel kernel) noexcept { return hardware_available(kernel); }

Crc32cKernel crc32c_active_kernel() noexcept {
    static const Crc32cKernel kActive = choose_kernel();
    return kActive;
}

std::uint32_t crc32c(std::span<const std::byte> bytes, std::uint32_t crc) noexcept {
    return run(crc32c_active_kernel(), bytes, crc);
}

std::optional<std::uint32_t> crc32c_with(Crc32cKernel kernel, std::span<const std::byte> bytes,
                                         std::uint32_t crc) noexcept {
    if (!hardware_available(kernel)) {
        return std::nullopt;
    }
    return run(kernel, bytes, crc);
}

}  // namespace tick_store::store
