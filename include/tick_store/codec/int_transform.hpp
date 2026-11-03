#pragma once

// The integer transforms the packed codecs share: zigzag, delta and delta-of-delta, all
// on uint64_t with arithmetic that wraps modulo 2^64 (D5). So every sequence of 64-bit
// values has a transform and comes back from its inverse exactly, a step backwards or a
// jump across the whole range included, and no signed overflow can happen anywhere.
//
// The loops are written so a compiler can vectorise them: no early exits, no calls, and
// the delta loops read only the input and the previous input. The inverses of delta and
// delta-of-delta are prefix sums, a dependency chain a later SIMD kernel may break up,
// as TASKS.md plans.
//
// Delta takes the value before the first as 0, so the first output is the first value
// itself. Delta-of-delta takes the delta before the first delta as 0, as Gorilla does:
//
//   y[0] = q[0]
//   y[1] = q[1] - q[0]
//   y[i] = (q[i] - q[i-1]) - (q[i-1] - q[i-2])   for i >= 2

#include <cstddef>
#include <cstdint>
#include <span>

namespace tick_store::codec {

// Maps a two's-complement value to an unsigned one with small magnitudes small: 0, -1,
// 1, -2 become 0, 1, 2, 3. The shift of the sign is arithmetic on the bit pattern, done
// here by negating the top bit, so it is defined for every input.
[[nodiscard]] constexpr std::uint64_t zigzag_encode(std::uint64_t value) noexcept {
    return (value << 1U) ^ (std::uint64_t{0} - (value >> 63U));
}

[[nodiscard]] constexpr std::uint64_t zigzag_decode(std::uint64_t value) noexcept {
    return (value >> 1U) ^ (std::uint64_t{0} - (value & 1U));
}

// `out[i] = in[i] - in[i-1]`, with in[-1] = 0. `out` holds at least in.size() values and
// does not overlap `in`.
inline void delta_encode(std::span<const std::uint64_t> in, std::span<std::uint64_t> out) noexcept {
    if (in.empty()) {
        return;
    }
    out[0] = in[0];
    for (std::size_t i = 1; i < in.size(); ++i) {
        out[i] = in[i] - in[i - 1];
    }
}

// The inverse of delta_encode, in place: a running sum.
inline void delta_decode(std::span<std::uint64_t> values) noexcept {
    std::uint64_t sum = 0;
    for (std::uint64_t& value : values) {
        sum += value;
        value = sum;
    }
}

// Delta-of-delta as above, with the same contract on `out`.
inline void delta_of_delta_encode(std::span<const std::uint64_t> in,
                                  std::span<std::uint64_t> out) noexcept {
    if (in.empty()) {
        return;
    }
    out[0] = in[0];
    if (in.size() == 1) {
        return;
    }
    out[1] = in[1] - in[0];
    for (std::size_t i = 2; i < in.size(); ++i) {
        out[i] = (in[i] - in[i - 1]) - (in[i - 1] - in[i - 2]);
    }
}

// The inverse of delta_of_delta_encode, in place: the deltas are a running sum of y[1..],
// and the values a running sum of the deltas from y[0].
inline void delta_of_delta_decode(std::span<std::uint64_t> values) noexcept {
    if (values.empty()) {
        return;
    }
    std::uint64_t value = values[0];
    std::uint64_t delta = 0;
    for (std::size_t i = 1; i < values.size(); ++i) {
        delta += values[i];
        value += delta;
        values[i] = value;
    }
}

}  // namespace tick_store::codec
