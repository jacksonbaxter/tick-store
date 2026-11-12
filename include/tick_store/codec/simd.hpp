#pragma once

// The decode kernels the codecs share, behind one dispatch table.
//
// Three kernel sets exist, and each gives the same values for the same input:
//
//   scalar  plain C++ written so a compiler may vectorise it, always built, any host.
//           On arm64, clang turns the lane unpack loop into NEON by itself (docs/NOTES.md),
//           so "scalar" there means "no hand-written intrinsics", not "no vectors"
//   neon    hand-written NEON, built on arm64. NEON is part of the arm64 baseline, so
//           there is no runtime check, as for the CRC32C instructions. No SVE or SME:
//           neither host has them
//   avx2    hand-written AVX2, built on x86-64 in its own translation unit, its
//           functions marked with a target attribute rather than the file compiled
//           with -mavx2, so no inline function from a header is ever emitted with AVX2
//           instructions for the linker to pick (Arrow's ODR problem). Chosen at run
//           time from CPUID and XGETBV, read once and cached. No PEXT, PDEP or gather,
//           which are microcoded or slow on Zen 1, and no AVX-512, which neither host
//           runs
//
// active_kernels() is the best set this build has and this processor runs, capped by the
// environment variable TICK_STORE_SIMD (scalar, neon or avx2) read once, before the first
// call returns. A cap names the highest level allowed; a cap this build or processor
// cannot reach is refused at that first call with a message on stderr and abort(), so a
// run that asked for a kernel never quietly gets another (S1, S6). kernel_runs() counts
// each set's calls on this thread, so a test can prove which set really ran.
//
// Every kernel takes spans and reads and writes exactly what they cover: no padding is
// needed after any input, and loads of packed bytes go through byte pointers and
// unaligned vector loads, never through a typed pointer to a misaligned word (S3, S7).
// Every set runs its whole steps through one driver, with the scalar kernel finishing
// what is left (src/codec/simd/driver.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tick_store::codec::simd {

enum class KernelLevel : std::uint8_t {
    scalar = 0,
    neon = 1,
    avx2 = 2,
};

inline constexpr std::size_t kKernelLevels = 3;
inline constexpr std::array<KernelLevel, kKernelLevels> kAllKernelLevels{
    KernelLevel::scalar, KernelLevel::neon, KernelLevel::avx2};

// Values of one interleaved vector (lane_pack.hpp), restated here to keep this header
// free of the codecs.
inline constexpr std::size_t kVectorValues = 1024;

// The largest value of the 32-bit fused delta kernel's input, before zigzag: base plus
// the largest packed value must stay below this, so 1,024 of them sum below 2^32, or,
// zigzag decoded, below 2^31 in magnitude.
inline constexpr std::uint64_t kNarrowDeltaLimit = std::uint64_t{1} << 22U;

[[nodiscard]] std::string_view to_string(KernelLevel level) noexcept;

struct Kernels {
    KernelLevel level = KernelLevel::scalar;

    // Whether PFOR decodes a full delta vector with no exceptions by unpack_delta, in one
    // pass, rather than by unpack_vector then delta_pass. Set per set from what measured
    // faster on its host (docs/NOTES.md, "SIMD").
    bool fuse_delta = false;

    // Unpacks a full interleaved vector at `width`, 0 to 64, from exactly 128 * width
    // little-endian bytes.
    void (*unpack_vector)(std::span<const std::byte> packed, unsigned width,
                          std::span<std::uint64_t, kVectorValues> values) noexcept = nullptr;

    // Unpacks values.size() values of the linear layout (lane_pack.hpp) at `width`, 1 to
    // 64, from `bytes`, which holds exactly linear_bytes(values.size(), width) bytes. The
    // caller checks the width, the size and the padding.
    void (*unpack_linear)(std::span<const std::byte> bytes, unsigned width,
                          std::span<std::uint64_t> values) noexcept = nullptr;

    // Frame of reference: each value plus `base`, modulo 2^64, then zigzag decoded if
    // `zigzag`.
    void (*add_base)(std::span<std::uint64_t> values, std::uint64_t base,
                     bool zigzag) noexcept = nullptr;

    // The inverse delta: a running sum from `carry`, in place. Returns the last sum, or
    // `carry` for no values.
    std::uint64_t (*prefix_sum)(std::span<std::uint64_t> values,
                                std::uint64_t carry) noexcept = nullptr;

    // add_base then prefix_sum, in one pass over the values: the second pass of a
    // two-pass delta decode.
    std::uint64_t (*delta_pass)(std::span<std::uint64_t> values, std::uint64_t base,
                                bool zigzag, std::uint64_t carry) noexcept = nullptr;

    // unpack_vector, add_base and prefix_sum fused, each row of 16 values going from
    // packed words to sums in registers. Only for a vector with no exceptions. PFOR calls
    // it only where fuse_delta is true, the AVX2 set alone; the scalar and NEON ones run
    // only in the kernel tests and simd_kernel_bench, so on arm64 not even the fuzz
    // oracle's kernel-against-scalar check reaches NEON's.
    std::uint64_t (*unpack_delta)(std::span<const std::byte> packed, unsigned width,
                                  std::uint64_t base, bool zigzag, std::uint64_t carry,
                                  std::span<std::uint64_t, kVectorValues> values) noexcept =
        nullptr;

    // The same, with the sums taken in 32-bit lanes relative to `carry` and widened on
    // the way out, when base + low_bits(width) < kNarrowDeltaLimit: the NEON and AVX2
    // kernels check that themselves and run their unpack_delta otherwise, and the scalar
    // set's is its unpack_delta. No decoder calls it: it runs only in the kernel tests and
    // simd_kernel_bench.
    std::uint64_t (*unpack_delta_narrow)(std::span<const std::byte> packed, unsigned width,
                                         std::uint64_t base, bool zigzag, std::uint64_t carry,
                                         std::span<std::uint64_t, kVectorValues> values) noexcept =
        nullptr;

    // Stream VByte's low 32 bits: values.size() values, value i's length code at bits
    // 2 * (i % 4) of control[i / 4], from `data`, which holds exactly the bytes the codes
    // say. Returns false if any value is not in its shortest length; `values` is then
    // unspecified and the caller's own checked loop finds which.
    bool (*svb_decode)(std::span<const std::byte> control, std::span<const std::byte> data,
                       std::span<std::uint64_t> values) noexcept = nullptr;
};

// The two forms of the inverse delta a set was measured with (docs/NOTES.md, "SIMD"): its
// own vector form, and the scalar tree, whose carry chain is one add for every four
// values. Kernels::prefix_sum is whichever measured faster on that set's host. Both
// members are null if the level is not available; for the scalar set, `vector` is the
// plain loop.
struct PrefixSumForms {
    std::uint64_t (*vector)(std::span<std::uint64_t> values, std::uint64_t carry) noexcept = nullptr;
    std::uint64_t (*tree)(std::span<std::uint64_t> values, std::uint64_t carry) noexcept = nullptr;
};

[[nodiscard]] PrefixSumForms prefix_sum_forms(KernelLevel level) noexcept;

// The data bytes Stream VByte's codes say `count` values take: one table lookup for each
// control byte, less the codes past the last value, which the caller has checked are 0.
// The prolog every set's svb_decode is called after.
[[nodiscard]] std::size_t svb_data_bytes(std::span<const std::byte> control,
                                         std::size_t count) noexcept;

// Whether this build has the set and this processor runs it.
[[nodiscard]] bool kernel_level_available(KernelLevel level) noexcept;

// The set for `level`, or nullptr if it is not available.
[[nodiscard]] const Kernels* kernels_for(KernelLevel level) noexcept;

// The set the codecs use, chosen at the first call, unless a ScalarKernelsOnThisThread
// lives on this thread.
[[nodiscard]] const Kernels& active_kernels() noexcept;

// The best level this build has and this processor runs, ignoring the cap.
[[nodiscard]] KernelLevel best_kernel_level() noexcept;

// While one lives, the codecs on this thread decode with the scalar set, whatever set
// was chosen, so a check can decode the same bytes both ways and compare: the fuzz
// oracles' kernel-against-scalar test. Nests, and restores what was there before.
class ScalarKernelsOnThisThread {
public:
    ScalarKernelsOnThisThread() noexcept;
    ~ScalarKernelsOnThisThread();
    ScalarKernelsOnThisThread(const ScalarKernelsOnThisThread&) = delete;
    ScalarKernelsOnThisThread& operator=(const ScalarKernelsOnThisThread&) = delete;
    ScalarKernelsOnThisThread(ScalarKernelsOnThisThread&&) = delete;
    ScalarKernelsOnThisThread& operator=(ScalarKernelsOnThisThread&&) = delete;

private:
    const Kernels* previous_;
};

// Calls into each set's kernels on this thread since the last reset, by level.
[[nodiscard]] std::array<std::uint64_t, kKernelLevels> kernel_runs() noexcept;
void reset_kernel_runs() noexcept;

// Parses a level's name, as TICK_STORE_SIMD takes it. False for anything else.
[[nodiscard]] bool parse_kernel_level(std::string_view name, KernelLevel& level) noexcept;

}  // namespace tick_store::codec::simd
