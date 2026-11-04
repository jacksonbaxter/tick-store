// The dispatch table: which kernel set the codecs use, chosen once.

#include "driver.hpp"
#include "kernel_sets.hpp"

#include "tick_store/codec/simd.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace tick_store::codec::simd {
namespace detail {
namespace {

thread_local std::array<std::uint64_t, kKernelLevels> tl_runs{};

// The set a ScalarKernelsOnThisThread put in force on this thread, or nullptr.
thread_local const Kernels* tl_override = nullptr;

}  // namespace

void note_run(KernelLevel level) noexcept { ++tl_runs[static_cast<std::size_t>(level)]; }

}  // namespace detail

namespace {

#ifdef TICK_STORE_SIMD_NEON
constexpr bool kNeonBuilt = true;
#else
constexpr bool kNeonBuilt = false;
#endif

#ifdef TICK_STORE_SIMD_AVX2
constexpr bool kAvx2Built = true;
#else
constexpr bool kAvx2Built = false;
#endif

[[noreturn]] void refuse_cap(std::string_view message, std::string_view value) noexcept {
    // Before the first decode, so nothing has run on the wrong kernel yet.
    (void)std::fprintf(stderr, "tick_store: TICK_STORE_SIMD=%.*s: %.*s\n",
                       static_cast<int>(value.size()), value.data(),
                       static_cast<int>(message.size()), message.data());
    std::abort();
}

[[nodiscard]] const Kernels& choose() noexcept {
    // Read once, under the guard of the static that caches the choice.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, under the static's guard.
    const char* cap = std::getenv("TICK_STORE_SIMD");
    KernelLevel level = best_kernel_level();
    if (cap != nullptr) {
        KernelLevel asked = KernelLevel::scalar;
        if (!parse_kernel_level(cap, asked)) {
            refuse_cap("not a kernel level: use scalar, neon or avx2", cap);
        }
        if (!kernel_level_available(asked)) {
            refuse_cap("this build or processor cannot run that level", cap);
        }
        level = asked;
    }
    return *kernels_for(level);
}

}  // namespace

std::string_view to_string(KernelLevel level) noexcept {
    switch (level) {
        case KernelLevel::scalar:
            return "scalar";
        case KernelLevel::neon:
            return "neon";
        case KernelLevel::avx2:
            return "avx2";
    }
    return "invalid";
}

bool parse_kernel_level(std::string_view name, KernelLevel& level) noexcept {
    for (const KernelLevel candidate : kAllKernelLevels) {
        if (name == to_string(candidate)) {
            level = candidate;
            return true;
        }
    }
    return false;
}

bool kernel_level_available(KernelLevel level) noexcept {
    switch (level) {
        case KernelLevel::scalar:
            return true;
        case KernelLevel::neon:
            return kNeonBuilt;
        case KernelLevel::avx2:
            if constexpr (kAvx2Built) {
                return detail::cpu_has_avx2();
            } else {
                return false;
            }
    }
    return false;
}

const Kernels* kernels_for(KernelLevel level) noexcept {
    if (!kernel_level_available(level)) {
        return nullptr;
    }
    switch (level) {
        case KernelLevel::scalar:
            return &detail::scalar_kernels();
        case KernelLevel::neon:
            if constexpr (kNeonBuilt) {
                return &detail::neon_kernels();
            } else {
                return nullptr;
            }
        case KernelLevel::avx2:
            if constexpr (kAvx2Built) {
                return &detail::avx2_kernels();
            } else {
                return nullptr;
            }
    }
    return nullptr;
}

PrefixSumForms prefix_sum_forms(KernelLevel level) noexcept {
    if (!kernel_level_available(level)) {
        return {};
    }
    switch (level) {
        case KernelLevel::scalar:
            return detail::scalar_prefix_sum_forms();
        case KernelLevel::neon:
            if constexpr (kNeonBuilt) {
                return detail::neon_prefix_sum_forms();
            } else {
                return {};
            }
        case KernelLevel::avx2:
            if constexpr (kAvx2Built) {
                return detail::avx2_prefix_sum_forms();
            } else {
                return {};
            }
    }
    return {};
}

std::size_t svb_data_bytes(std::span<const std::byte> control, std::size_t count) noexcept {
    std::size_t bytes = 0;
    for (const std::byte code : control) {
        bytes += detail::kSvbLengths[std::to_integer<std::size_t>(code)];
    }
    // Each code past the last value is 0, a length of 1 that is not there.
    const std::size_t spare = (control.size() * 4U) - count;
    return bytes - spare;
}

KernelLevel best_kernel_level() noexcept {
    for (const KernelLevel level : {KernelLevel::avx2, KernelLevel::neon}) {
        if (kernel_level_available(level)) {
            return level;
        }
    }
    return KernelLevel::scalar;
}

const Kernels& active_kernels() noexcept {
    static const Kernels& kActive = choose();
    if (const Kernels* forced = detail::tl_override; forced != nullptr) {
        return *forced;
    }
    return kActive;
}

ScalarKernelsOnThisThread::ScalarKernelsOnThisThread() noexcept
    : previous_{detail::tl_override} {
    detail::tl_override = &detail::scalar_kernels();
}

ScalarKernelsOnThisThread::~ScalarKernelsOnThisThread() { detail::tl_override = previous_; }

std::array<std::uint64_t, kKernelLevels> kernel_runs() noexcept { return detail::tl_runs; }

void reset_kernel_runs() noexcept { detail::tl_runs = {}; }

}  // namespace tick_store::codec::simd
