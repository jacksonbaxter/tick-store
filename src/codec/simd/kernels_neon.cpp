// The NEON kernel set, built on arm64 only, where NEON is the baseline: no runtime check.
// Every kernel runs its whole steps through drive() and leaves the rest to the scalar
// code in driver.hpp.

#include "driver.hpp"
#include "linear_tables.hpp"
#include "svb_tables.hpp"
#include "unpack_impl.hpp"

#include "tick_store/codec/simd.hpp"

#include <arm_neon.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace tick_store::codec::simd::detail {
namespace {

constexpr KernelLevel kLevel = KernelLevel::neon;

struct NeonOps {
    using V = uint64x2_t;
    static constexpr std::size_t kLanes = 2;

    [[gnu::always_inline]] static V load(const std::byte* at) noexcept {
        V v;
        std::memcpy(&v, at, sizeof v);
        return v;
    }
    [[gnu::always_inline]] static void store(std::uint64_t* at, V v) noexcept { vst1q_u64(at, v); }
    template <unsigned S>
    [[gnu::always_inline]] static V shr(V v) noexcept {
        return vshrq_n_u64(v, S);
    }
    template <unsigned S>
    [[gnu::always_inline]] static V shl(V v) noexcept {
        return vshlq_n_u64(v, S);
    }
    // USHL takes a signed count per lane: negative shifts right, and 64 or more gives 0.
    [[gnu::always_inline]] static V shr_by(V v, unsigned count) noexcept {
        return vshlq_u64(v, vdupq_n_s64(-static_cast<std::int64_t>(count)));
    }
    [[gnu::always_inline]] static V shl_by(V v, unsigned count) noexcept {
        return vshlq_u64(v, vdupq_n_s64(static_cast<std::int64_t>(count)));
    }
    [[gnu::always_inline]] static V and_(V a, V b) noexcept { return vandq_u64(a, b); }
    [[gnu::always_inline]] static V or_(V a, V b) noexcept { return vorrq_u64(a, b); }
    [[gnu::always_inline]] static V splat(std::uint64_t value) noexcept { return vdupq_n_u64(value); }
    [[gnu::always_inline]] static V zero() noexcept { return vdupq_n_u64(0); }
};

using V = NeonOps::V;
using NeonRow = Row<NeonOps>;

[[gnu::always_inline]] inline V zigzag(V v) noexcept {
    const V sign = vreinterpretq_u64_s64(vshrq_n_s64(vreinterpretq_s64_u64(vshlq_n_u64(v, 63)), 63));
    return veorq_u64(vshrq_n_u64(v, 1), sign);
}

[[gnu::always_inline]] inline V from_offset(V v, V base, bool zz) noexcept {
    const V sum = vaddq_u64(v, base);
    return zz ? zigzag(sum) : sum;
}

// --- Unpack ------------------------------------------------------------------------------

[[nodiscard]] bool well_formed(std::span<const std::byte> packed, unsigned width) noexcept {
    return width <= kMaxWidth && packed.size() == kWordStride * width;
}

void unpack_vector_neon(std::span<const std::byte> packed, unsigned width,
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
            kUnpackTable<NeonOps>[width](packed.data(), values.data());
        },
        [](std::size_t) {});
}

// Eight values a step, which take exactly `width` bytes: each pair of values from one
// 16-byte load, a byte shuffle into their lanes, a shift per lane and the mask. A step
// runs while its loads stay inside the bytes; the rest, and widths above
// kLinearSimdWidest, go to the scalar loop.
void unpack_linear_neon(std::span<const std::byte> bytes, unsigned width,
                        std::span<std::uint64_t> values) noexcept {
    const bool wide = width > kLinearSimdWidest || width == 0;
    const LinearWidth& table = kLinearTable[wide ? 1U : width];
    std::array<uint8x16_t, 4> shuffle{};
    std::array<int64x2_t, 4> shift{};
    for (std::size_t p = 0; p < 4; ++p) {
        shuffle[p] = vld1q_u8(table.pairs[p].shuffle.data());
        shift[p] = vnegq_s64(vreinterpretq_s64_u64(vld1q_u64(table.pairs[p].shift.data())));
    }
    const V mask = vdupq_n_u64(low_mask(width));
    std::uint64_t* const out = values.data();
    drive<8>(
        kLevel, values.size(),
        [&](std::size_t i) { return !wide && ((i / 8U) * width) + table.reach <= bytes.size(); },
        [&](std::size_t i) {
            const std::byte* const group = bytes.data() + ((i / 8U) * width);
            for (std::size_t p = 0; p < 4; ++p) {
                uint8x16_t source;
                std::memcpy(&source, group + table.pairs[p].load, sizeof source);
                const V v = vshlq_u64(vreinterpretq_u64_u8(vqtbl1q_u8(source, shuffle[p])), shift[p]);
                vst1q_u64(out + i + (2U * p), vandq_u64(v, mask));
            }
        },
        [&](std::size_t first) { unpack_linear_from(bytes, width, values, first); });
}

// --- Frame of reference and the inverse delta ---------------------------------------------

void add_base_neon(std::span<std::uint64_t> values, std::uint64_t base, bool zz) noexcept {
    const V b = vdupq_n_u64(base);
    std::uint64_t* const p = values.data();
    drive<8>(
        kLevel, values.size(), kAlwaysFits,
        [&](std::size_t i) {
            const V x0 = from_offset(vld1q_u64(p + i), b, zz);
            const V x1 = from_offset(vld1q_u64(p + i + 2), b, zz);
            const V x2 = from_offset(vld1q_u64(p + i + 4), b, zz);
            const V x3 = from_offset(vld1q_u64(p + i + 6), b, zz);
            vst1q_u64(p + i, x0);
            vst1q_u64(p + i + 2, x1);
            vst1q_u64(p + i + 4, x2);
            vst1q_u64(p + i + 6, x3);
        },
        [&](std::size_t first) { add_base_from(values, first, base, zz); });
}

// Running sums of eight values in four registers, from the carry in `c`, which then
// holds the last sum in both lanes. Kogge-Stone: shifts of one, two and four values.
[[gnu::always_inline]] inline void sum_eight(V& x0, V& x1, V& x2, V& x3, V& c) noexcept {
    const V z = vdupq_n_u64(0);
    x3 = vaddq_u64(x3, vextq_u64(x2, x3, 1));
    x2 = vaddq_u64(x2, vextq_u64(x1, x2, 1));
    x1 = vaddq_u64(x1, vextq_u64(x0, x1, 1));
    x0 = vaddq_u64(x0, vextq_u64(z, x0, 1));
    x3 = vaddq_u64(x3, x2);
    x2 = vaddq_u64(x2, x1);
    x1 = vaddq_u64(x1, x0);
    x3 = vaddq_u64(x3, x1);
    x2 = vaddq_u64(x2, x0);
    // The next carry from this group's total, off the chain of carries.
    const V total = vdupq_laneq_u64(x3, 1);
    x0 = vaddq_u64(x0, c);
    x1 = vaddq_u64(x1, c);
    x2 = vaddq_u64(x2, c);
    x3 = vaddq_u64(x3, c);
    c = vaddq_u64(c, total);
}

// The vector form, measured against the scalar tree in prefix_sum's place.
std::uint64_t prefix_sum_neon_vector(std::span<std::uint64_t> values, std::uint64_t carry) noexcept {
    std::uint64_t* const p = values.data();
    V c = vdupq_n_u64(carry);
    const std::size_t done = drive<8>(
        kLevel, values.size(), kAlwaysFits,
        [&](std::size_t i) {
            V x0 = vld1q_u64(p + i);
            V x1 = vld1q_u64(p + i + 2);
            V x2 = vld1q_u64(p + i + 4);
            V x3 = vld1q_u64(p + i + 6);
            sum_eight(x0, x1, x2, x3, c);
            vst1q_u64(p + i, x0);
            vst1q_u64(p + i + 2, x1);
            vst1q_u64(p + i + 4, x2);
            vst1q_u64(p + i + 6, x3);
        },
        [&](std::size_t first) { carry = prefix_sum_from(values, first, vgetq_lane_u64(c, 0)); });
    return done == values.size() ? vgetq_lane_u64(c, 0) : carry;
}

std::uint64_t prefix_sum_neon_tree(std::span<std::uint64_t> values, std::uint64_t carry) noexcept {
    drive<1>(
        kLevel, values.size(), [](std::size_t) { return false; }, [](std::size_t) {},
        [&](std::size_t first) { carry = prefix_sum_tree_from(values, first, carry); });
    return carry;
}

std::uint64_t delta_pass_neon(std::span<std::uint64_t> values, std::uint64_t base, bool zz,
                              std::uint64_t carry) noexcept {
    std::uint64_t* const p = values.data();
    const V b = vdupq_n_u64(base);
    V c = vdupq_n_u64(carry);
    const std::size_t done = drive<8>(
        kLevel, values.size(), kAlwaysFits,
        [&](std::size_t i) {
            V x0 = from_offset(vld1q_u64(p + i), b, zz);
            V x1 = from_offset(vld1q_u64(p + i + 2), b, zz);
            V x2 = from_offset(vld1q_u64(p + i + 4), b, zz);
            V x3 = from_offset(vld1q_u64(p + i + 6), b, zz);
            sum_eight(x0, x1, x2, x3, c);
            vst1q_u64(p + i, x0);
            vst1q_u64(p + i + 2, x1);
            vst1q_u64(p + i + 4, x2);
            vst1q_u64(p + i + 6, x3);
        },
        [&](std::size_t first) {
            carry = delta_pass_from(values, first, base, zz, vgetq_lane_u64(c, 0));
        });
    return done == values.size() ? vgetq_lane_u64(c, 0) : carry;
}

// --- The fused kernels ------------------------------------------------------------------

// Running sums of a row of 16 values in eight registers, from `c`, which then holds the
// last. Kogge-Stone over shifts of one, two, four and eight values.
[[gnu::always_inline]] inline void sum_row(NeonRow& x, V& c) noexcept {
    const V z = vdupq_n_u64(0);
    for (std::size_t g = x.size(); g-- > 1;) {
        x[g] = vaddq_u64(x[g], vextq_u64(x[g - 1], x[g], 1));
    }
    x[0] = vaddq_u64(x[0], vextq_u64(z, x[0], 1));
    for (std::size_t step = 1; step < x.size(); step *= 2) {
        for (std::size_t g = x.size(); g-- > step;) {
            x[g] = vaddq_u64(x[g], x[g - step]);
        }
    }
    const V total = vdupq_laneq_u64(x[x.size() - 1], 1);
    for (V& v : x) {
        v = vaddq_u64(v, c);
    }
    c = vaddq_u64(c, total);
}

template <bool Zigzag>
std::uint64_t unpack_delta_rows(std::span<const std::byte> packed, unsigned width,
                                std::uint64_t base, std::uint64_t carry,
                                std::span<std::uint64_t, kVectorValues> values) noexcept {
    const V mask = vdupq_n_u64(low_mask(width));
    const V b = vdupq_n_u64(base);
    V c = vdupq_n_u64(carry);
    std::uint64_t* const out = values.data();
    for (std::size_t r = 0; r < kRows; ++r) {
        NeonRow row;  // NOLINT(cppcoreguidelines-pro-type-member-init): every register written below
        unpack_row_rt<NeonOps>(packed.data(), width, r, mask, row);
        for (V& v : row) {
            v = from_offset(v, b, Zigzag);
        }
        sum_row(row, c);
        for (std::size_t g = 0; g < row.size(); ++g) {
            vst1q_u64(out + (r * kLaneCount) + (g * 2U), row[g]);
        }
    }
    return vgetq_lane_u64(c, 0);
}

std::uint64_t unpack_delta_neon(std::span<const std::byte> packed, unsigned width,
                                std::uint64_t base, bool zz, std::uint64_t carry,
                                std::span<std::uint64_t, kVectorValues> values) noexcept {
    if (width == 0 || !well_formed(packed, width)) {
        // Nothing to load: every offset is 0, as the scalar unpack leaves it.
        for (std::uint64_t& value : values) {
            value = 0;
        }
        return delta_pass_neon(values, base, zz, carry);
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

// The narrow form: each row's offsets plus the base are below 2^22, so they go to 32-bit
// lanes, four to a register, and the sums are taken there relative to the vector's
// start, then widened (zero- or sign-extended) and added to the carry.
template <bool Zigzag>
std::uint64_t unpack_delta_narrow_rows(std::span<const std::byte> packed, unsigned width,
                                       std::uint64_t base, std::uint64_t carry,
                                       std::span<std::uint64_t, kVectorValues> values) noexcept {
    const V mask = vdupq_n_u64(low_mask(width));
    const uint32x4_t b = vdupq_n_u32(static_cast<std::uint32_t>(base));
    const uint32x4_t z = vdupq_n_u32(0);
    const V c64 = vdupq_n_u64(carry);
    uint32x4_t c32 = z;
    std::uint64_t* const out = values.data();
    std::array<uint32x4_t, 4> x{};
    for (std::size_t r = 0; r < kRows; ++r) {
        NeonRow row;  // NOLINT(cppcoreguidelines-pro-type-member-init): every register written below
        unpack_row_rt<NeonOps>(packed.data(), width, r, mask, row);
        for (std::size_t h = 0; h < x.size(); ++h) {
            const uint32x4_t offsets = vaddq_u32(
                vuzp1q_u32(vreinterpretq_u32_u64(row[2 * h]), vreinterpretq_u32_u64(row[(2 * h) + 1])),
                b);
            const uint32x4_t n = [&] {
                if constexpr (Zigzag) {
                    const uint32x4_t sign = vreinterpretq_u32_s32(
                        vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(offsets, 31)), 31));
                    return veorq_u32(vshrq_n_u32(offsets, 1), sign);
                } else {
                    return offsets;
                }
            }();
            x[h] = n;
        }
        for (std::size_t h = x.size(); h-- > 1;) {
            x[h] = vaddq_u32(x[h], vextq_u32(x[h - 1], x[h], 3));
        }
        x[0] = vaddq_u32(x[0], vextq_u32(z, x[0], 3));
        for (std::size_t h = x.size(); h-- > 1;) {
            x[h] = vaddq_u32(x[h], vextq_u32(x[h - 1], x[h], 2));
        }
        x[0] = vaddq_u32(x[0], vextq_u32(z, x[0], 2));
        x[3] = vaddq_u32(x[3], x[2]);
        x[2] = vaddq_u32(x[2], x[1]);
        x[1] = vaddq_u32(x[1], x[0]);
        x[3] = vaddq_u32(x[3], x[1]);
        x[2] = vaddq_u32(x[2], x[0]);
        for (uint32x4_t& v : x) {
            v = vaddq_u32(v, c32);
        }
        c32 = vdupq_laneq_u32(x[3], 3);
        std::uint64_t* const at = out + (r * kLaneCount);
        for (std::size_t h = 0; h < x.size(); ++h) {
            if constexpr (Zigzag) {
                const int32x4_t s = vreinterpretq_s32_u32(x[h]);
                vst1q_u64(at + (4 * h), vaddq_u64(c64, vreinterpretq_u64_s64(vmovl_s32(vget_low_s32(s)))));
                vst1q_u64(at + (4 * h) + 2, vaddq_u64(c64, vreinterpretq_u64_s64(vmovl_high_s32(s))));
            } else {
                vst1q_u64(at + (4 * h), vaddq_u64(c64, vmovl_u32(vget_low_u32(x[h]))));
                vst1q_u64(at + (4 * h) + 2, vaddq_u64(c64, vmovl_high_u32(x[h])));
            }
        }
    }
    return values[kVectorValues - 1];
}

std::uint64_t unpack_delta_narrow_neon(std::span<const std::byte> packed, unsigned width,
                                       std::uint64_t base, bool zz, std::uint64_t carry,
                                       std::span<std::uint64_t, kVectorValues> values) noexcept {
    // Written so nothing wraps: base + low_mask(width) < kNarrowDeltaLimit.
    if (width == 0 || !well_formed(packed, width) || base >= kNarrowDeltaLimit ||
        low_mask(width) >= kNarrowDeltaLimit - base) {
        return unpack_delta_neon(packed, width, base, zz, carry, values);
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

bool svb_decode_neon(std::span<const std::byte> control, std::span<const std::byte> data,
                     std::span<std::uint64_t> values) noexcept {
    std::size_t at = 0;
    uint32x4_t ok = vdupq_n_u32(~0U);
    bool tail_ok = true;
    std::uint64_t* const out = values.data();
    drive<4>(
        kLevel, values.size(), [&](std::size_t) { return at + 16U <= data.size(); },
        [&](std::size_t i) {
            const auto c = std::to_integer<unsigned>(control[i / 4U]);
            uint8x16_t bytes;
            std::memcpy(&bytes, data.data() + at, sizeof bytes);
            const uint32x4_t v =
                vreinterpretq_u32_u8(vqtbl1q_u8(bytes, vld1q_u8(kSvbShuffle[c].data())));
            ok = vandq_u32(ok, vcgeq_u32(v, vld1q_u32(kSvbLeast[c].data())));
            vst1q_u64(out + i, vmovl_u32(vget_low_u32(v)));
            vst1q_u64(out + i + 2, vmovl_high_u32(v));
            at += kSvbLengths[c];
        },
        [&](std::size_t first) { tail_ok = svb_decode_from(control, data, values, first, at); });
    return tail_ok && vminvq_u32(ok) != 0;
}

constexpr Kernels kNeon{
    .level = kLevel,
    // two passes measured faster on the M4 (NOTES.md, "SIMD").
    .fuse_delta = false,
    .unpack_vector = &unpack_vector_neon,
    .unpack_linear = &unpack_linear_neon,
    .add_base = &add_base_neon,
    .prefix_sum = &prefix_sum_neon_vector,
    .delta_pass = &delta_pass_neon,
    .unpack_delta = &unpack_delta_neon,
    .unpack_delta_narrow = &unpack_delta_narrow_neon,
    .svb_decode = &svb_decode_neon,
};

}  // namespace

const Kernels& neon_kernels() noexcept { return kNeon; }

PrefixSumForms neon_prefix_sum_forms() noexcept {
    return {.vector = &prefix_sum_neon_vector, .tree = &prefix_sum_neon_tree};
}

}  // namespace tick_store::codec::simd::detail
