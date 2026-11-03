#include "tick_store/codec/lane_pack.hpp"

#include "tick_store/codec/bit_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tick_store::codec {

// The loops over lanes are the ones a compiler vectorises: 16 independent words with
// the same shift, and the shift and the straddle test hoisted out to the row. Each row
// goes through local arrays, loaded whole, computed, then stored whole: with the input
// and output spans read and written directly, the compiler has to assume they overlap,
// and clang kept every lane scalar.
using Lanes = std::array<std::uint64_t, kLanes>;

void pack_vector(std::span<const std::uint64_t, kVectorValues> values, unsigned width,
                 std::span<std::uint64_t> words) noexcept {
    std::ranges::fill(words, std::uint64_t{0});
    if (width == 0 || width > kWordBits || words.size() != vector_words(width)) {
        return;
    }
    const std::uint64_t mask = low_bits(width);
    for (std::size_t row = 0; row < kLaneRows; ++row) {
        const std::size_t bit = row * width;
        const std::size_t word = bit / kWordBits;
        const auto shift = static_cast<unsigned>(bit % kWordBits);
        Lanes in{};
        std::ranges::copy(values.subspan(row * kLanes, kLanes), in.begin());
        const std::span<std::uint64_t> low = words.subspan(word * kLanes, kLanes);
        Lanes packed{};
        std::ranges::copy(low, packed.begin());
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            packed[lane] |= (in[lane] & mask) << shift;
        }
        std::ranges::copy(packed, low.begin());
        // The value straddles into the next word. Then shift > 0, so 64 - shift < 64,
        // and the next word exists: (word + 1) * 64 < bit + width <= 64 * width. The
        // next word is still all zero, since rows fill words in order.
        if (shift + width > kWordBits) {
            Lanes high{};
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                high[lane] = (in[lane] & mask) >> (kWordBits - shift);
            }
            std::ranges::copy(high, words.subspan((word + 1) * kLanes, kLanes).begin());
        }
    }
}

void unpack_vector(std::span<const std::uint64_t> words, unsigned width,
                   std::span<std::uint64_t, kVectorValues> values) noexcept {
    if (width == 0 || width > kWordBits || words.size() != vector_words(width)) {
        std::ranges::fill(values, std::uint64_t{0});
        return;
    }
    const std::uint64_t mask = low_bits(width);
    for (std::size_t row = 0; row < kLaneRows; ++row) {
        const std::size_t bit = row * width;
        const std::size_t word = bit / kWordBits;
        const auto shift = static_cast<unsigned>(bit % kWordBits);
        Lanes low{};
        std::ranges::copy(words.subspan(word * kLanes, kLanes), low.begin());
        Lanes out{};
        if (shift + width > kWordBits) {
            Lanes high{};
            std::ranges::copy(words.subspan((word + 1) * kLanes, kLanes), high.begin());
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out[lane] = ((low[lane] >> shift) | (high[lane] << (kWordBits - shift))) & mask;
            }
        } else {
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                out[lane] = (low[lane] >> shift) & mask;
            }
        }
        std::ranges::copy(out, values.subspan(row * kLanes, kLanes).begin());
    }
}

void append_words_le(std::span<const std::uint64_t> words, std::vector<std::byte>& out) {
    out.reserve(out.size() + (words.size() * 8U));
    for (const std::uint64_t word : words) {
        for (unsigned i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::byte>((word >> (8U * i)) & 0xFFU));
        }
    }
}

void load_words_le(std::span<const std::byte> bytes, std::span<std::uint64_t> words) noexcept {
    const std::size_t count = std::min(words.size(), bytes.size() / 8U);
    for (std::size_t w = 0; w < count; ++w) {
        std::uint64_t word = 0;
        for (std::size_t i = 8; i > 0; --i) {
            word = (word << 8U) | std::to_integer<std::uint64_t>(bytes[(w * 8U) + i - 1]);
        }
        words[w] = word;
    }
}

void pack_linear(std::span<const std::uint64_t> values, unsigned width,
                 std::vector<std::byte>& out) {
    if (width == 0 || width > kWordBits) {
        return;
    }
    const std::uint64_t mask = low_bits(width);
    std::uint64_t pending = 0;
    unsigned filled = 0;  // always below 64
    const auto flush = [&out](std::uint64_t word, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) {
            out.push_back(static_cast<std::byte>((word >> (8U * i)) & 0xFFU));
        }
    };
    for (const std::uint64_t raw : values) {
        const std::uint64_t value = raw & mask;
        pending |= value << filled;
        if (filled + width < kWordBits) {
            filled += width;
            continue;
        }
        // The word is full. What did not fit starts the next; when filled is 0 the
        // value was a whole word and nothing is left over.
        flush(pending, 8);
        pending = filled == 0 ? 0 : value >> (kWordBits - filled);
        filled = filled + width - kWordBits;
    }
    flush(pending, (filled + 7U) / 8U);
}

bool unpack_linear(std::span<const std::byte> bytes, unsigned width,
                   std::span<std::uint64_t> values) noexcept {
    if (width > kWordBits) {
        return false;
    }
    if (width == 0) {
        std::ranges::fill(values, std::uint64_t{0});
        return bytes.empty();
    }
    if (bytes.size() != linear_bytes(values.size(), width)) {
        return false;
    }
    const std::uint64_t mask = low_bits(width);
    // The up to nine bytes from `at` that hold a value, read as little-endian, with
    // bytes past the end taken as zero.
    const auto byte_at = [&bytes](std::size_t at) {
        return at < bytes.size() ? std::to_integer<std::uint64_t>(bytes[at]) : 0U;
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t bit = i * width;
        const std::size_t first = bit / 8U;
        const auto skip = static_cast<unsigned>(bit % 8U);
        std::uint64_t word = 0;
        for (std::size_t b = 8; b > 0; --b) {
            word = (word << 8U) | byte_at(first + b - 1);
        }
        word >>= skip;
        if (skip + width > kWordBits) {
            // skip > 0 here, so the shift is below 64.
            word |= byte_at(first + 8) << (kWordBits - skip);
        }
        values[i] = word & mask;
    }
    const std::size_t used = values.size() * width;
    if (used % 8U != 0) {
        const auto last = std::to_integer<unsigned>(bytes.back());
        return (last >> (used % 8U)) == 0;
    }
    return true;
}

}  // namespace tick_store::codec
