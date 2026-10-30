#pragma once

// Small helpers for writing wire bytes by hand in tests.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::itch::test {

using Bytes = std::vector<std::byte>;

// A list of byte values, each given as a hex literal or a character.
[[nodiscard]] inline Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes out;
    out.reserve(values.size());
    for (const unsigned value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

// Appends the bytes of an ASCII string, unchanged.
inline void append_ascii(Bytes& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

// `value` as a big-endian length prefix, then `payload`.
[[nodiscard]] inline Bytes framed(std::uint16_t value, const Bytes& payload) {
    Bytes out;
    out.reserve(payload.size() + 2);
    out.push_back(static_cast<std::byte>(value >> 8U));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Copies `input` into a heap block of exactly its size, so ASan reports a read
// one byte past the end. An empty input stays empty.
[[nodiscard]] inline Bytes exact_copy(std::span<const std::byte> input) {
    Bytes out(input.begin(), input.end());
    out.shrink_to_fit();
    return out;
}

}  // namespace tick_store::itch::test
