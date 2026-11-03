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
//
// Head delta is for columns that interleave several rising sequences, as ITCH order
// references do (NOTES.md, "Order references"): each value is coded against one of the
// last kHeadDeltaHeads values the column has taken from different sequences, its heads,
// kept most recently used first. The output word is a payload and a 3-bit token:
//
//   y = (payload << 3) | token
//   token 0 to 6   x = heads[token] + payload, modulo 2^64; that head moves to the front
//                  and takes the value x
//   token 7        x = payload, a raw value below 2^61; x goes in at the front and the
//                  last head drops out
//
// Every head starts at 0. The encoder takes the head with the smallest gap x - head,
// modulo 2^64, the lowest token among equal gaps, if that gap is below 2^61; otherwise
// it escapes with token 7 if x is below 2^61; otherwise the sequence has no head-delta
// form and head_delta_encode says so. The inverse takes every word to a value, so a
// decoder needs no check beyond its own fields.

#include <array>
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

inline constexpr unsigned kHeadDeltaTokenBits = 3;
inline constexpr std::size_t kHeadDeltaHeads = (std::size_t{1} << kHeadDeltaTokenBits) - 1U;
inline constexpr std::uint64_t kHeadDeltaEscape = kHeadDeltaHeads;
inline constexpr std::uint64_t kHeadDeltaTokenMask = kHeadDeltaEscape;
// Payloads are below this: 2^61.
inline constexpr std::uint64_t kHeadDeltaPayloadLimit = std::uint64_t{1}
                                                        << (64U - kHeadDeltaTokenBits);

namespace detail {

// Moves heads[0..from) down one place and puts `value` at the front.
inline void head_to_front(std::array<std::uint64_t, kHeadDeltaHeads>& heads, std::size_t from,
                          std::uint64_t value) noexcept {
    for (std::size_t t = from; t > 0; --t) {
        heads[t] = heads[t - 1];
    }
    heads[0] = value;
}

}  // namespace detail

// Head delta as above. `out` holds at least in.size() values and does not overlap `in`.
// Returns false, with `out` unspecified, if a value is reachable neither from a head nor
// raw, which needs a value of 2^61 or more far from every head.
[[nodiscard]] inline bool head_delta_encode(std::span<const std::uint64_t> in,
                                            std::span<std::uint64_t> out) noexcept {
    std::array<std::uint64_t, kHeadDeltaHeads> heads{};
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::uint64_t x = in[i];
        std::size_t token = 0;
        std::uint64_t gap = x - heads[0];
        for (std::size_t t = 1; t < kHeadDeltaHeads; ++t) {
            if (x - heads[t] < gap) {
                gap = x - heads[t];
                token = t;
            }
        }
        if (gap < kHeadDeltaPayloadLimit) {
            out[i] = (gap << kHeadDeltaTokenBits) | token;
            detail::head_to_front(heads, token, x);
        } else if (x < kHeadDeltaPayloadLimit) {
            out[i] = (x << kHeadDeltaTokenBits) | kHeadDeltaEscape;
            detail::head_to_front(heads, kHeadDeltaHeads - 1U, x);
        } else {
            return false;
        }
    }
    return true;
}

// The inverse of head_delta_encode, in place. Defined for every word.
inline void head_delta_decode(std::span<std::uint64_t> values) noexcept {
    std::array<std::uint64_t, kHeadDeltaHeads> heads{};
    for (std::uint64_t& value : values) {
        const std::uint64_t token = value & kHeadDeltaTokenMask;
        const std::uint64_t payload = value >> kHeadDeltaTokenBits;
        if (token == kHeadDeltaEscape) {
            detail::head_to_front(heads, kHeadDeltaHeads - 1U, payload);
            value = payload;
        } else {
            const std::uint64_t x = heads[token] + payload;
            detail::head_to_front(heads, static_cast<std::size_t>(token), x);
            value = x;
        }
    }
}

}  // namespace tick_store::codec
