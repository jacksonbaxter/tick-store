#pragma once

// The interleaved lane unpack, written once over an instruction set's operations, for
// the NEON and AVX2 files to instantiate. Private to the library; in an unnamed
// namespace for the reason driver.hpp gives, and included by the AVX2 file inside its
// target("avx2") region.
//
// The layout is lane_pack.hpp's: word k of lane l at byte (16 k + l) * 8, row r of lane l
// at bits [r w, (r + 1) w) of that lane. A register holds Ops::kLanes adjacent lanes, so
// the words a register needs are kLanes consecutive words, and one row of them is
// kLanes consecutive values.
//
// Two forms:
//
//   unpack_width<Ops, W>   the width a template argument, so every shift is an
//                          immediate and every word is loaded once. One group of lanes
//                          at a time through all 64 rows, which keeps one function per
//                          width near a kilobyte. The 65 widths go in a table.
//   unpack_row_rt<Ops>     the width at run time, every shift a register count, one
//                          row of all 16 lanes at a time, for the fused delta kernels,
//                          which need a whole row to sum it. A shift count of 64 gives 0
//                          on both instruction sets, so a row that does not straddle two
//                          words takes no branch: its high word's share shifts to 0.
//
// Neither reads a byte past 128 * W: the last row of a lane ends exactly at the end of
// its last word, so no row straddles past it.
//
// Ops supplies: V, kLanes, load(const std::byte*) unaligned, store(std::uint64_t*, V),
// shr<S>(V) and shl<S>(V) for S in 1 to 63, shr_by(V, count) and shl_by(V, count) for
// counts in 0 to 64, and_(V, V), or_(V, V), splat(std::uint64_t) and zero().

#include "driver.hpp"

#include "tick_store/codec/simd.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace tick_store::codec::simd::detail {
// NOLINTNEXTLINE(misc-anonymous-namespace-in-header,cert-dcl59-cpp): one copy per instruction set
namespace {

inline constexpr std::size_t kLaneCount = 16;
inline constexpr std::size_t kRows = 64;
// Bytes from word k of a lane to word k + 1 of the same lane.
inline constexpr std::size_t kWordStride = kLaneCount * 8U;
inline constexpr std::size_t kMaxWidth = 64;

template <class Ops, unsigned W, std::size_t R>
[[gnu::always_inline]] inline void unpack_one_row(const std::byte* in, std::uint64_t* out,
                                                  typename Ops::V& cur,
                                                  typename Ops::V mask) noexcept {
    constexpr std::size_t kBit = R * W;
    constexpr std::size_t kWord = kBit / 64U;
    constexpr unsigned kShift = kBit % 64U;
    typename Ops::V v = cur;
    if constexpr (kShift != 0) {
        v = Ops::template shr<kShift>(v);
    }
    if constexpr (kShift + W > 64U) {
        // Straddles into the next word, which the next row starts in.
        const typename Ops::V next = Ops::load(in + ((kWord + 1U) * kWordStride));
        v = Ops::or_(v, Ops::template shl<64U - kShift>(next));
        cur = next;
    } else if constexpr (kShift + W == 64U && R + 1U < kRows) {
        // Ends the word, so the next row starts the next one.
        cur = Ops::load(in + ((kWord + 1U) * kWordStride));
    }
    if constexpr (W < 64U) {
        v = Ops::and_(v, mask);
    }
    Ops::store(out + (R * kLaneCount), v);
}

template <class Ops, unsigned W, std::size_t... R>
[[gnu::always_inline]] inline void unpack_group(const std::byte* in, std::uint64_t* out,
                                                std::index_sequence<R...> /*rows*/) noexcept {
    typename Ops::V cur = Ops::load(in);
    const typename Ops::V mask = Ops::splat(low_mask(W));
    (unpack_one_row<Ops, W, R>(in, out, cur, mask), ...);
}

template <class Ops, unsigned W>
void unpack_width(const std::byte* packed, std::uint64_t* out) noexcept {
    if constexpr (W == 0) {
        for (std::size_t i = 0; i < kVectorValues; i += Ops::kLanes) {
            Ops::store(out + i, Ops::zero());
        }
    } else {
        for (std::size_t lane = 0; lane < kLaneCount; lane += Ops::kLanes) {
            unpack_group<Ops, W>(packed + (lane * 8U), out + lane, std::make_index_sequence<kRows>{});
        }
    }
}

using UnpackWidthFn = void (*)(const std::byte*, std::uint64_t*) noexcept;

template <class Ops, std::size_t... W>
constexpr std::array<UnpackWidthFn, kMaxWidth + 1U> make_unpack_table(
    std::index_sequence<W...> /*widths*/) noexcept {
    return {&unpack_width<Ops, static_cast<unsigned>(W)>...};
}

template <class Ops>
inline constexpr std::array<UnpackWidthFn, kMaxWidth + 1U> kUnpackTable =
    make_unpack_table<Ops>(std::make_index_sequence<kMaxWidth + 1U>{});

template <class Ops>
inline constexpr std::size_t kGroups = kLaneCount / Ops::kLanes;

template <class Ops>
using Row = std::array<typename Ops::V, kGroups<Ops>>;

// Row r of all 16 lanes at a run-time width of 1 to 64, into `row`.
template <class Ops>
[[gnu::always_inline]] inline void unpack_row_rt(const std::byte* packed, unsigned width,
                                                 std::size_t r, typename Ops::V mask,
                                                 Row<Ops>& row) noexcept {
    const std::size_t bit = r * width;
    const std::size_t word = bit / 64U;
    const auto shift = static_cast<unsigned>(bit % 64U);
    const bool straddles = shift + width > 64U;
    // Without a straddle the high word is the low one again, shifted out to 0.
    const std::size_t high_word = straddles ? word + 1U : word;
    const unsigned high_shift = straddles ? 64U - shift : 64U;
    const std::byte* const low_at = packed + (word * kWordStride);
    const std::byte* const high_at = packed + (high_word * kWordStride);
    for (std::size_t g = 0; g < kGroups<Ops>; ++g) {
        const std::size_t offset = g * Ops::kLanes * 8U;
        const typename Ops::V low = Ops::shr_by(Ops::load(low_at + offset), shift);
        const typename Ops::V high = Ops::shl_by(Ops::load(high_at + offset), high_shift);
        row[g] = Ops::and_(Ops::or_(low, high), mask);
    }
}

}  // namespace
}  // namespace tick_store::codec::simd::detail
