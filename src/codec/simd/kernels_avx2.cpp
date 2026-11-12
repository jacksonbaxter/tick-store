// The AVX2 kernel set, built on x86-64 only and chosen at run time from CPUID.
//
// This file is not compiled with -mavx2. Every function from the region below is marked
// target("avx2") by the pragma instead, and the shared headers' code sits in unnamed
// namespaces, so nothing with AVX2 instructions has external linkage: no inline function
// from a header, std:: templates included, can be emitted here with AVX2 instructions and
// then chosen by the linker for code that runs on a processor without them (Arrow's ODR
// problem). The only external functions are the CPU check, which is baseline code, and
// the table accessors, which return data. No PEXT, PDEP or gather: microcoded or slow on
// Zen 1. Zen 1 also runs every 256-bit operation as two 128-bit halves.

#include "kernel_sets.hpp"

#include "tick_store/codec/simd.hpp"

#include <cpuid.h>
#include <immintrin.h>

// Every standard header the shared headers use comes before the region, so no standard
// function is declared with the target attribute.
#include <algorithm>
#include <array>
#include <bit>  // IWYU pragma: keep
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>  // IWYU pragma: keep

namespace tick_store::codec::simd::detail {

namespace {

// XCR0 by XGETBV, which needs the xsave target. Called only once CPUID has said the
// processor and the OS have it (OSXSAVE).
[[gnu::target("xsave")]] std::uint64_t read_xcr0() noexcept { return _xgetbv(0); }

}  // namespace

bool cpu_has_avx2() noexcept {
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    // The OS must save the YMM state (OSXSAVE, then XCR0 bits 1 and 2) as well as the
    // processor having AVX.
    if ((ecx & bit_OSXSAVE) == 0 || (ecx & bit_AVX) == 0) {
        return false;
    }
    if ((read_xcr0() & 0x6U) != 0x6U) {
        return false;
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (ebx & bit_AVX2) != 0;
}

}  // namespace tick_store::codec::simd::detail

#pragma clang attribute push(__attribute__((target("avx2"))), apply_to = function)

#include "driver.hpp"
#include "linear_tables.hpp"
#include "svb_tables.hpp"
#include "unpack_impl.hpp"

namespace tick_store::codec::simd::detail {
namespace {

constexpr KernelLevel kLevel = KernelLevel::avx2;

struct Avx2Ops {
    using V = __m256i;
    static constexpr std::size_t kLanes = 4;

    [[gnu::always_inline]] static V load(const std::byte* at) noexcept {
        V v;
        std::memcpy(&v, at, sizeof v);
        return v;
    }
    [[gnu::always_inline]] static void store(std::uint64_t* at, V v) noexcept {
        std::memcpy(at, &v, sizeof v);
    }
    template <unsigned S>
    [[gnu::always_inline]] static V shr(V v) noexcept {
        return _mm256_srli_epi64(v, S);
    }
    template <unsigned S>
    [[gnu::always_inline]] static V shl(V v) noexcept {
        return _mm256_slli_epi64(v, S);
    }
    // A count of 64 or more gives 0.
    [[gnu::always_inline]] static V shr_by(V v, unsigned count) noexcept {
        return _mm256_srl_epi64(v, _mm_cvtsi32_si128(static_cast<int>(count)));
    }
    [[gnu::always_inline]] static V shl_by(V v, unsigned count) noexcept {
        return _mm256_sll_epi64(v, _mm_cvtsi32_si128(static_cast<int>(count)));
    }
    [[gnu::always_inline]] static V and_(V a, V b) noexcept { return _mm256_and_si256(a, b); }
    [[gnu::always_inline]] static V or_(V a, V b) noexcept { return _mm256_or_si256(a, b); }
    [[gnu::always_inline]] static V splat(std::uint64_t value) noexcept {
        return _mm256_set1_epi64x(static_cast<long long>(value));
    }
    [[gnu::always_inline]] static V zero() noexcept { return _mm256_setzero_si256(); }
};

using V = Avx2Ops::V;
using Avx2Row = Row<Avx2Ops>;

[[gnu::always_inline]] inline V load_values(const std::uint64_t* at) noexcept {
    V v;
    std::memcpy(&v, at, sizeof v);
    return v;
}

[[gnu::always_inline]] inline void store_values(std::uint64_t* at, V v) noexcept {
    std::memcpy(at, &v, sizeof v);
}

[[gnu::always_inline]] inline std::uint64_t last_lane(V v) noexcept {
    return static_cast<std::uint64_t>(_mm256_extract_epi64(v, 3));
}

[[gnu::always_inline]] inline V zigzag(V v) noexcept {
    const V sign = _mm256_sub_epi64(_mm256_setzero_si256(), _mm256_and_si256(v, _mm256_set1_epi64x(1)));
    return _mm256_xor_si256(_mm256_srli_epi64(v, 1), sign);
}

[[gnu::always_inline]] inline V from_offset(V v, V base, bool zz) noexcept {
    const V sum = _mm256_add_epi64(v, base);
    return zz ? zigzag(sum) : sum;
}

// Every lane set to lane 3.
[[gnu::always_inline]] inline V broadcast_last(V v) noexcept {
    return _mm256_permute4x64_epi64(v, 0xFF);
}

// Running sums of the four lanes, relative to the register's start: within each 128-bit
// half by a byte shift, then the low half's total added to the high half.
[[gnu::always_inline]] inline V sum_lanes(V x) noexcept {
    x = _mm256_add_epi64(x, _mm256_slli_si256(x, 8));
    const V low_total = _mm256_permute4x64_epi64(x, 0x55);
    return _mm256_add_epi64(x, _mm256_blend_epi32(_mm256_setzero_si256(), low_total, 0xF0));
}

// --- Unpack ------------------------------------------------------------------------------

[[nodiscard]] bool well_formed(std::span<const std::byte> packed, unsigned width) noexcept {
    return width <= kMaxWidth && packed.size() == kWordStride * width;
}

void unpack_vector_avx2(std::span<const std::byte> packed, unsigned width,
                        std::span<std::uint64_t, kVectorValues> values) noexcept {
    drive<kVectorValues>(
        kLevel, kVectorValues, kAlwaysFits,
        [&](std::size_t) {
            if (!well_formed(packed, width)) {
                for (std::uint64_t& value : values) {
                    value = 0;
                }
                return;
            }
            kUnpackTable<Avx2Ops>[width](packed.data(), values.data());
        },
        [](std::size_t) {});
}

// Eight values a step, which take exactly `width` bytes: four at a time, from two
// 16-byte loads joined in one register, a byte shuffle into their lanes, a shift per lane
// and the mask. A step runs while its loads stay inside the bytes; the rest, and widths
// above kLinearSimdWidest, go to the scalar loop.
void unpack_linear_avx2(std::span<const std::byte> bytes, unsigned width,
                        std::span<std::uint64_t> values) noexcept {
    const bool wide = width > kLinearSimdWidest || width == 0;
    const LinearWidth& table = kLinearTable[wide ? 1U : width];
    std::array<__m256i, 2> shuffle{};
    std::array<__m256i, 2> shift{};
    for (std::size_t h = 0; h < 2; ++h) {
        const LinearPair& low = table.pairs[2 * h];
        const LinearPair& high = table.pairs[(2 * h) + 1];
        std::array<std::uint8_t, 32> joined{};
        std::ranges::copy(low.shuffle, joined.begin());
        std::ranges::copy(high.shuffle, joined.begin() + 16);
        std::memcpy(&shuffle[h], joined.data(), sizeof shuffle[h]);
        const std::array<std::uint64_t, 4> counts{low.shift[0], low.shift[1], high.shift[0], high.shift[1]};
        std::memcpy(&shift[h], counts.data(), sizeof shift[h]);
    }
    const V mask = Avx2Ops::splat(low_mask(width));
    std::uint64_t* const out = values.data();
    drive<8>(
        kLevel, values.size(),
        [&](std::size_t i) { return !wide && ((i / 8U) * width) + table.reach <= bytes.size(); },
        [&](std::size_t i) {
            const std::byte* const group = bytes.data() + ((i / 8U) * width);
            for (std::size_t h = 0; h < 2; ++h) {
                __m128i low;
                __m128i high;
                std::memcpy(&low, group + table.pairs[2 * h].load, sizeof low);
                std::memcpy(&high, group + table.pairs[(2 * h) + 1].load, sizeof high);
                const __m256i source = _mm256_inserti128_si256(_mm256_castsi128_si256(low), high, 1);
                const __m256i v = _mm256_srlv_epi64(_mm256_shuffle_epi8(source, shuffle[h]), shift[h]);
                store_values(out + i + (4U * h), _mm256_and_si256(v, mask));
            }
        },
        [&](std::size_t first) { unpack_linear_from(bytes, width, values, first); });
}

// --- Frame of reference and the inverse delta ---------------------------------------------

void add_base_avx2(std::span<std::uint64_t> values, std::uint64_t base, bool zz) noexcept {
    const V b = Avx2Ops::splat(base);
    std::uint64_t* const p = values.data();
    drive<8>(
        kLevel, values.size(), kAlwaysFits,
        [&](std::size_t i) {
            const V x0 = from_offset(load_values(p + i), b, zz);
            const V x1 = from_offset(load_values(p + i + 4), b, zz);
            store_values(p + i, x0);
            store_values(p + i + 4, x1);
        },
        [&](std::size_t first) { add_base_from(values, first, base, zz); });
}

// Running sums of eight values in two registers from the carry `c`, which then holds the
// last sum in every lane. The next carry comes from the group's own total, off the chain.
[[gnu::always_inline]] inline void sum_eight(V& x0, V& x1, V& c) noexcept {
    x0 = sum_lanes(x0);
    x1 = _mm256_add_epi64(sum_lanes(x1), broadcast_last(x0));
    const V total = broadcast_last(x1);
    x0 = _mm256_add_epi64(x0, c);
    x1 = _mm256_add_epi64(x1, c);
    c = _mm256_add_epi64(c, total);
}

std::uint64_t prefix_sum_avx2_vector(std::span<std::uint64_t> values, std::uint64_t carry) noexcept {
    std::uint64_t* const p = values.data();
    V c = Avx2Ops::splat(carry);
    const std::size_t done = drive<8>(
        kLevel, values.size(), kAlwaysFits,
        [&](std::size_t i) {
            V x0 = load_values(p + i);
            V x1 = load_values(p + i + 4);
            sum_eight(x0, x1, c);
            store_values(p + i, x0);
            store_values(p + i + 4, x1);
        },
        [&](std::size_t first) { carry = prefix_sum_from(values, first, last_lane(c)); });
    return done == values.size() ? last_lane(c) : carry;
}

std::uint64_t prefix_sum_avx2_tree(std::span<std::uint64_t> values, std::uint64_t carry) noexcept {
    drive<1>(
        kLevel, values.size(), [](std::size_t) { return false; }, [](std::size_t) {},
        [&](std::size_t first) { carry = prefix_sum_tree_from(values, first, carry); });
    return carry;
}

std::uint64_t delta_pass_avx2(std::span<std::uint64_t> values, std::uint64_t base, bool zz,
                              std::uint64_t carry) noexcept {
    std::uint64_t* const p = values.data();
    const V b = Avx2Ops::splat(base);
    V c = Avx2Ops::splat(carry);
    const std::size_t done = drive<8>(
        kLevel, values.size(), kAlwaysFits,
        [&](std::size_t i) {
            V x0 = from_offset(load_values(p + i), b, zz);
            V x1 = from_offset(load_values(p + i + 4), b, zz);
            sum_eight(x0, x1, c);
            store_values(p + i, x0);
            store_values(p + i + 4, x1);
        },
        [&](std::size_t first) {
            carry = delta_pass_from(values, first, base, zz, last_lane(c));
        });
    return done == values.size() ? last_lane(c) : carry;
}

// --- The fused kernels ------------------------------------------------------------------

// Running sums of a row of 16 values in four registers, from `c`.
[[gnu::always_inline]] inline void sum_row(Avx2Row& x, V& c) noexcept {
    for (V& v : x) {
        v = sum_lanes(v);
    }
    const V t0 = broadcast_last(x[0]);
    const V t01 = _mm256_add_epi64(t0, broadcast_last(x[1]));
    const V t012 = _mm256_add_epi64(t01, broadcast_last(x[2]));
    x[1] = _mm256_add_epi64(x[1], t0);
    x[2] = _mm256_add_epi64(x[2], t01);
    x[3] = _mm256_add_epi64(x[3], t012);
    const V total = broadcast_last(x[3]);
    for (V& v : x) {
        v = _mm256_add_epi64(v, c);
    }
    c = _mm256_add_epi64(c, total);
}

template <bool Zigzag>
std::uint64_t unpack_delta_rows(std::span<const std::byte> packed, unsigned width,
                                std::uint64_t base, std::uint64_t carry,
                                std::span<std::uint64_t, kVectorValues> values) noexcept {
    const V mask = Avx2Ops::splat(low_mask(width));
    const V b = Avx2Ops::splat(base);
    V c = Avx2Ops::splat(carry);
    std::uint64_t* const out = values.data();
    for (std::size_t r = 0; r < kRows; ++r) {
        Avx2Row row;  // NOLINT(cppcoreguidelines-pro-type-member-init): every register written below
        unpack_row_rt<Avx2Ops>(packed.data(), width, r, mask, row);
        for (V& v : row) {
            v = from_offset(v, b, Zigzag);
        }
        sum_row(row, c);
        for (std::size_t g = 0; g < row.size(); ++g) {
            store_values(out + (r * kLaneCount) + (g * 4U), row[g]);
        }
    }
    return last_lane(c);
}

std::uint64_t unpack_delta_avx2(std::span<const std::byte> packed, unsigned width,
                                std::uint64_t base, bool zz, std::uint64_t carry,
                                std::span<std::uint64_t, kVectorValues> values) noexcept {
    if (width == 0 || !well_formed(packed, width)) {
        // Nothing to load: every offset is 0, as the scalar unpack leaves it.
        for (std::uint64_t& value : values) {
            value = 0;
        }
        return delta_pass_avx2(values, base, zz, carry);
    }
    drive<kVectorValues>(
        kLevel, kVectorValues, kAlwaysFits,
        [&](std::size_t) {
            carry = zz ? unpack_delta_rows<true>(packed, width, base, carry, values)
                       : unpack_delta_rows<false>(packed, width, base, carry, values);
        },
        [](std::size_t) {});
    return carry;
}

// Running sums of eight 32-bit lanes, relative to the register's start.
[[gnu::always_inline]] inline __m256i sum_lanes32(__m256i x) noexcept {
    x = _mm256_add_epi32(x, _mm256_slli_si256(x, 4));
    x = _mm256_add_epi32(x, _mm256_slli_si256(x, 8));
    // The low half's total, lane 3, into every lane of the high half only.
    const __m256i total = _mm256_shuffle_epi32(x, 0xFF);
    return _mm256_add_epi32(x, _mm256_permute2x128_si256(total, total, 0x08));
}

// Every 32-bit lane set to lane 7.
[[gnu::always_inline]] inline __m256i broadcast_last32(__m256i x) noexcept {
    const __m256i high = _mm256_shuffle_epi32(x, 0xFF);
    return _mm256_permute2x128_si256(high, high, 0x11);
}

template <bool Zigzag>
[[gnu::always_inline]] inline void store_widened(std::uint64_t* at, __m256i sums, V carry) noexcept {
    const __m128i low = _mm256_castsi256_si128(sums);
    const __m128i high = _mm256_extracti128_si256(sums, 1);
    if constexpr (Zigzag) {
        store_values(at, _mm256_add_epi64(carry, _mm256_cvtepi32_epi64(low)));
        store_values(at + 4, _mm256_add_epi64(carry, _mm256_cvtepi32_epi64(high)));
    } else {
        store_values(at, _mm256_add_epi64(carry, _mm256_cvtepu32_epi64(low)));
        store_values(at + 4, _mm256_add_epi64(carry, _mm256_cvtepu32_epi64(high)));
    }
}

// The narrow form: each row's offsets plus the base are below 2^22, so they go to 32-bit
// lanes, eight to a register, summed there relative to the vector's start, then widened
// and added to the carry.
template <bool Zigzag>
std::uint64_t unpack_delta_narrow_rows(std::span<const std::byte> packed, unsigned width,
                                       std::uint64_t base, std::uint64_t carry,
                                       std::span<std::uint64_t, kVectorValues> values) noexcept {
    const V mask = Avx2Ops::splat(low_mask(width));
    const __m256i b = _mm256_set1_epi32(static_cast<int>(base));
    const __m256i order = _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7);
    const V c64 = Avx2Ops::splat(carry);
    __m256i c32 = _mm256_setzero_si256();
    std::uint64_t* const out = values.data();
    for (std::size_t r = 0; r < kRows; ++r) {
        Avx2Row row;  // NOLINT(cppcoreguidelines-pro-type-member-init): every register written below
        unpack_row_rt<Avx2Ops>(packed.data(), width, r, mask, row);
        std::array<__m256i, 2> x{};
        for (std::size_t h = 0; h < x.size(); ++h) {
            // Lanes 8h to 8h + 3 in the even 32-bit places, 8h + 4 to 8h + 7 in the odd, then
            // put in order.
            const __m256i mixed = _mm256_blend_epi32(row[2 * h], _mm256_slli_epi64(row[(2 * h) + 1], 32), 0xAA);
            const __m256i offsets = _mm256_add_epi32(_mm256_permutevar8x32_epi32(mixed, order), b);
            const __m256i n = [&] {
                if constexpr (Zigzag) {
                    const __m256i sign = _mm256_sub_epi32(
                        _mm256_setzero_si256(), _mm256_and_si256(offsets, _mm256_set1_epi32(1)));
                    return _mm256_xor_si256(_mm256_srli_epi32(offsets, 1), sign);
                } else {
                    return offsets;
                }
            }();
            x[h] = sum_lanes32(n);
        }
        x[1] = _mm256_add_epi32(x[1], broadcast_last32(x[0]));
        x[0] = _mm256_add_epi32(x[0], c32);
        x[1] = _mm256_add_epi32(x[1], c32);
        c32 = broadcast_last32(x[1]);
        std::uint64_t* const at = out + (r * kLaneCount);
        store_widened<Zigzag>(at, x[0], c64);
        store_widened<Zigzag>(at + 8, x[1], c64);
    }
    return values[kVectorValues - 1];
}

std::uint64_t unpack_delta_narrow_avx2(std::span<const std::byte> packed, unsigned width,
                                       std::uint64_t base, bool zz, std::uint64_t carry,
                                       std::span<std::uint64_t, kVectorValues> values) noexcept {
    // Written so nothing wraps: base + low_mask(width) < kNarrowDeltaLimit.
    if (width == 0 || !well_formed(packed, width) || base >= kNarrowDeltaLimit ||
        low_mask(width) >= kNarrowDeltaLimit - base) {
        return unpack_delta_avx2(packed, width, base, zz, carry, values);
    }
    drive<kVectorValues>(
        kLevel, kVectorValues, kAlwaysFits,
        [&](std::size_t) {
            carry = zz ? unpack_delta_narrow_rows<true>(packed, width, base, carry, values)
                       : unpack_delta_narrow_rows<false>(packed, width, base, carry, values);
        },
        [](std::size_t) {});
    return carry;
}

// --- Stream VByte -------------------------------------------------------------------------

bool svb_decode_avx2(std::span<const std::byte> control, std::span<const std::byte> data,
                     std::span<std::uint64_t> values) noexcept {
    std::size_t at = 0;
    __m128i ok = _mm_set1_epi32(-1);
    bool tail_ok = true;
    std::uint64_t* const out = values.data();
    drive<4>(
        kLevel, values.size(), [&](std::size_t) { return at + 16U <= data.size(); },
        [&](std::size_t i) {
            const auto c = std::to_integer<unsigned>(control[i / 4U]);
            __m128i bytes;
            std::memcpy(&bytes, data.data() + at, sizeof bytes);
            __m128i shuffle;
            std::memcpy(&shuffle, kSvbShuffle[c].data(), sizeof shuffle);
            __m128i least;
            std::memcpy(&least, kSvbLeast[c].data(), sizeof least);
            const __m128i v = _mm_shuffle_epi8(bytes, shuffle);
            // v >= least, unsigned, as max(v, least) == v.
            ok = _mm_and_si128(ok, _mm_cmpeq_epi32(_mm_max_epu32(v, least), v));
            store_values(out + i, _mm256_cvtepu32_epi64(v));
            at += kSvbLengths[c];
        },
        [&](std::size_t first) { tail_ok = svb_decode_from(control, data, values, first, at); });
    return tail_ok && _mm_movemask_epi8(ok) == 0xFFFF;
}

constexpr Kernels kAvx2{
    .level = kLevel,
    // Fused measured faster than two passes on Zen 1, 1.29 against 1.19 G values/s, the
    // median over every width; the in-register prefix sum lost to the scalar tree, 1.57
    // against 2.04, since a cross-lane permute costs more than the adds it saves when
    // each 256-bit operation is split in two (docs/NOTES.md, "SIMD").
    .fuse_delta = true,
    .unpack_vector = &unpack_vector_avx2,
    .unpack_linear = &unpack_linear_avx2,
    .add_base = &add_base_avx2,
    .prefix_sum = &prefix_sum_avx2_tree,
    .delta_pass = &delta_pass_avx2,
    .unpack_delta = &unpack_delta_avx2,
    .unpack_delta_narrow = &unpack_delta_narrow_avx2,
    .svb_decode = &svb_decode_avx2,
};

}  // namespace
}  // namespace tick_store::codec::simd::detail

#pragma clang attribute pop

namespace tick_store::codec::simd::detail {

// Baseline code: they return data, and dispatch.cpp calls them only once cpu_has_avx2()
// has said yes.
const Kernels& avx2_kernels() noexcept { return kAvx2; }

PrefixSumForms avx2_prefix_sum_forms() noexcept {
    return {.vector = &prefix_sum_avx2_vector, .tree = &prefix_sum_avx2_tree};
}

}  // namespace tick_store::codec::simd::detail
