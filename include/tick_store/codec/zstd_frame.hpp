#pragma once

// zstd over another codec's bytes: the composite codec the registry names `zstd` (id 8).
// The store wraps a column's encoded bytes in it where zstd makes them smaller, on the
// few columns where docs/NOTES.md found that it pays.
//
// A payload, little-endian:
//
//   offset 0  u16  inner codec id: readable in the registry, and neither none nor zstd
//   offset 2  u16  reserved, 0
//   offset 4  u32  inner bytes n: 1 to the caller's limit
//   offset 8       exactly one zstd frame, filling the rest of the payload, whose content
//                  size is stated in its header and is n
//
// Decoding checks every field before anything is sized by it, and allocates n bytes only
// once n is within the limit the caller gives, which the store derives from the column's
// value count (D8). zstd's own decoder checks the frame. The encoder runs at level 1 and is
// deterministic, but another encoder could write another valid frame for the same bytes,
// so, as for PFOR, a payload has more than one valid encoding: the fuzz oracle compares
// what it decodes to, not its bytes.

#include "tick_store/codec/codec_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

inline constexpr std::size_t kZstdHeaderSize = 8;
inline constexpr int kZstdLevel = 1;

// The inner codec and its bytes, decompressed.
struct ZstdPayload {
    std::uint16_t inner_codec = 0;
    std::vector<std::byte> bytes;
};

// Appends a payload holding `inner` compressed at kZstdLevel. `inner` must not be empty.
[[nodiscard]] std::expected<void, CodecError> encode_zstd_payload(
    std::uint16_t inner_codec, std::span<const std::byte> inner, std::vector<std::byte>& out);

// Decodes a whole payload. `max_inner_bytes` bounds the inner size before any allocation.
// Errors: "zstd header" truncated; "zstd inner codec" bad_field (expected 0, actual the id);
// "zstd reserved" bad_field; "zstd inner size" bad_count (expected the limit, actual n);
// "zstd frame" bad_field where zstd refuses the frame, does not state n as its content size,
// or does not end exactly at the payload's end (expected and actual are sizes in bytes, or 0
// and zstd's error code). Offsets are in bits from the payload's first byte.
[[nodiscard]] std::expected<ZstdPayload, CodecError> decode_zstd_payload(
    std::span<const std::byte> payload, std::uint64_t max_inner_bytes);

}  // namespace tick_store::codec
