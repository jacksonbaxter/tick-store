#pragma once

// What every kernel set shares: the one driver that runs a set's whole steps and leaves
// the rest to the scalar kernel, and the scalar kernels themselves, which are both the
// scalar set and every other set's tail. Private to the library.
//
// Included by each set's translation unit. Everything but the declarations sits in an
// unnamed namespace, so each file gets a copy of its own, compiled for that file's
// instruction set: the AVX2 file includes this inside its target("avx2") region, so the
// driver and the scalar tails inline into its kernels, and no copy with AVX2
// instructions can be merged with the baseline copies of other files, as an inline
// function with external linkage would be (Arrow's ODR problem).

#include "kernel_sets.hpp"

#include "tick_store/codec/simd.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace tick_store::codec::simd::detail {

// NOLINTNEXTLINE(misc-anonymous-namespace-in-header,cert-dcl59-cpp): one copy per instruction set, above
namespace {

// The driver. Notes the call, then runs `step(i)` for each whole step of `Step` items
// from 0 while `fits(i)` says the step's input is in bounds, and `tail(i)` once on what
// is left, if anything. `step` and `fits` may keep state of their own, as Stream VByte's
// data position. Returns where the whole steps stopped.
template <std::size_t Step, class Fits, class StepFn, class Tail>
inline std::size_t drive(KernelLevel level, std::size_t count, Fits&& fits, StepFn&& step,
                         Tail&& tail) noexcept {
    note_run(level);
    std::size_t i = 0;
    while (i + Step <= count && fits(i)) {
        step(i);
        i += Step;
    }
    if (i < count) {
        tail(i);
    }
    return i;
}

// A step whose input is only the items themselves always fits.
inline constexpr auto kAlwaysFits = [](std::size_t) noexcept { return true; };

[[nodiscard]] constexpr std::uint64_t zigzag_decode(std::uint64_t value) noexcept {
    return (value >> 1U) ^ (std::uint64_t{0} - (value & 1U));
}

[[nodiscard]] constexpr std::uint64_t low_mask(unsigned width) noexcept {
    return width >= 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1U;
}

[[nodiscard]] inline std::uint64_t load_u64_le(const std::byte* at) noexcept {
    std::uint64_t word = 0;
    std::memcpy(&word, at, sizeof word);
    if constexpr (std::endian::native == std::endian::big) {
        word = std::byteswap(word);
    }
    return word;
}

// --- The scalar kernels, on a range of values from `first` ------------------------------

inline void add_base_from(std::span<std::uint64_t> values, std::size_t first, std::uint64_t base,
                          bool zigzag) noexcept {
    if (zigzag) {
        for (std::size_t i = first; i < values.size(); ++i) {
            values[i] = zigzag_decode(values[i] + base);
        }
    } else {
        for (std::size_t i = first; i < values.size(); ++i) {
            values[i] += base;
        }
    }
}

[[nodiscard]] inline std::uint64_t prefix_sum_from(std::span<std::uint64_t> values,
                                                   std::size_t first,
                                                   std::uint64_t carry) noexcept {
    for (std::size_t i = first; i < values.size(); ++i) {
        carry += values[i];
        values[i] = carry;
    }
    return carry;
}

// The same sums with the carry's chain one add long for every four values: each group's
// own sums are formed off the chain, then the carry is added to all four. Arm's engineers
// report this scalar form beating NEON on Neoverse (TASKS.md), so each set measures both
// and keeps the faster (NOTES.md, "SIMD").
[[nodiscard]] inline std::uint64_t prefix_sum_tree_from(std::span<std::uint64_t> values,
                                                        std::size_t first,
                                                        std::uint64_t carry) noexcept {
    std::size_t i = first;
    for (; i + 4U <= values.size(); i += 4U) {
        const std::uint64_t x0 = values[i];
        const std::uint64_t x1 = values[i + 1U];
        const std::uint64_t x2 = values[i + 2U];
        const std::uint64_t x3 = values[i + 3U];
        const std::uint64_t s1 = x0 + x1;
        const std::uint64_t s2 = s1 + x2;
        const std::uint64_t s3 = s1 + (x2 + x3);
        values[i] = carry + x0;
        values[i + 1U] = carry + s1;
        values[i + 2U] = carry + s2;
        values[i + 3U] = carry + s3;
        carry += s3;
    }
    return prefix_sum_from(values, i, carry);
}

[[nodiscard]] inline std::uint64_t delta_pass_from(std::span<std::uint64_t> values,
                                                   std::size_t first, std::uint64_t base,
                                                   bool zigzag, std::uint64_t carry) noexcept {
    if (zigzag) {
        for (std::size_t i = first; i < values.size(); ++i) {
            carry += zigzag_decode(values[i] + base);
            values[i] = carry;
        }
    } else {
        for (std::size_t i = first; i < values.size(); ++i) {
            carry += values[i] + base;
            values[i] = carry;
        }
    }
    return carry;
}

// The linear layout from value `first`: value i at bits [i w, (i + 1) w), least
// significant bit first. `bytes` holds exactly the bytes the values need, so a value's
// bytes past the end are read as 0, never loaded. Width 1 to 64.
inline void unpack_linear_from(std::span<const std::byte> bytes, unsigned width,
                               std::span<std::uint64_t> values, std::size_t first) noexcept {
    const std::uint64_t mask = low_mask(width);
    const auto byte_at = [&bytes](std::size_t at) {
        return at < bytes.size() ? std::to_integer<std::uint64_t>(bytes[at]) : 0U;
    };
    for (std::size_t i = first; i < values.size(); ++i) {
        const std::size_t bit = i * width;
        const std::size_t at = bit / 8U;
        const auto skip = static_cast<unsigned>(bit % 8U);
        std::uint64_t word = 0;
        if (at + 8U <= bytes.size()) {
            word = load_u64_le(bytes.data() + at);
        } else {
            for (std::size_t b = 8; b > 0; --b) {
                word = (word << 8U) | byte_at(at + b - 1U);
            }
        }
        word >>= skip;
        if (skip != 0U && skip + width > 64U) {
            // A width of at most 64 straddles only from skip > 0, so the shift is below 64.
            word |= byte_at(at + 8U) << (64U - skip);
        }
        values[i] = word & mask;
    }
}

// Stream VByte's length code of value i.
[[nodiscard]] inline unsigned svb_code(std::span<const std::byte> control, std::size_t i) noexcept {
    return (std::to_integer<unsigned>(control[i / 4U]) >> (2U * (i % 4U))) & 3U;
}

// Values from `first`, with the data from byte `at`. False if a value is not in its
// shortest length. The caller has checked `data` holds exactly what the codes say.
[[nodiscard]] inline bool svb_decode_from(std::span<const std::byte> control,
                                          std::span<const std::byte> data,
                                          std::span<std::uint64_t> values, std::size_t first,
                                          std::size_t at) noexcept {
    bool canonical = true;
    for (std::size_t i = first; i < values.size(); ++i) {
        const unsigned code = svb_code(control, i);
        std::uint64_t value = 0;
        for (unsigned b = code + 1U; b > 0; --b) {
            value = (value << 8U) | std::to_integer<std::uint64_t>(data[at + b - 1U]);
        }
        // The top byte of a value longer than one byte is not zero.
        canonical = canonical && (code == 0 || (value >> (8U * code)) != 0);
        values[i] = value;
        at += code + 1U;
    }
    return canonical;
}

// The data bytes of the four values one control byte codes, for every control byte.
inline constexpr auto kSvbLengths = [] {
    std::array<std::uint8_t, 256> lengths{};
    for (unsigned c = 0; c < 256; ++c) {
        lengths[c] = static_cast<std::uint8_t>(4U + (c & 3U) + ((c >> 2U) & 3U) +
                                               ((c >> 4U) & 3U) + ((c >> 6U) & 3U));
    }
    return lengths;
}();

}  // namespace

}  // namespace tick_store::codec::simd::detail
