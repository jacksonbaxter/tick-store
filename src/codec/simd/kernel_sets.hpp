#pragma once

// The functions the kernel sets' files define for dispatch.cpp, apart from driver.hpp so
// the AVX2 file can declare them before its target("avx2") region begins: each is
// baseline code with external linkage. Private to the library.

#include "tick_store/codec/simd.hpp"

namespace tick_store::codec::simd::detail {

// Counts one call into `level`'s kernels on this thread (kernel_runs()).
void note_run(KernelLevel level) noexcept;

// Each set's table, defined in its own file. The NEON set exists only on arm64 and the
// AVX2 set only on x86-64, which also says whether this processor runs it.
[[nodiscard]] const Kernels& scalar_kernels() noexcept;
[[nodiscard]] const Kernels& neon_kernels() noexcept;
[[nodiscard]] const Kernels& avx2_kernels() noexcept;
[[nodiscard]] bool cpu_has_avx2() noexcept;
[[nodiscard]] PrefixSumForms scalar_prefix_sum_forms() noexcept;
[[nodiscard]] PrefixSumForms neon_prefix_sum_forms() noexcept;
[[nodiscard]] PrefixSumForms avx2_prefix_sum_forms() noexcept;

}  // namespace tick_store::codec::simd::detail
