#pragma once

// Patched frame of reference on interleaved lanes: the integer codec measured against
// Gorilla. One decoder serves every variant, because a block names its own transform.
//
// A block is the frame in block.hpp around this payload, every field on whole bytes and
// little-endian:
//
//   u8      transform     0 none, 1 delta, 2 delta-of-delta, 3 head delta
//                         (int_transform.hpp)
//   u8      scale bytes   1 to 8
//   ...     scale s       that many bytes, s >= 1, its top byte not zero
//   if s > 1, the remainders:
//     u32   count         at most the block's value count n
//     ...   patch list    (patch_list.hpp) over n values, of v mod s, each 1 to s - 1,
//                         in bit_width(s - 1) bits
//   then one vector for each 1,024 values, the last holding what is left, m values:
//     u8    mode          0 plain, 1 zigzag
//     u8    width b       0 to 64
//     u8    base bytes    0 to 8, the top one not zero
//     ...   base          that many bytes
//     ...   packed        m == 1,024: 128 * b bytes, interleaved (lane_pack.hpp)
//                         otherwise: linear_bytes(m, b) bytes, linear
//     u16   exceptions e  at most m
//     if e > 0:
//       u8  high width h  1 to 64 - b, and the highest exception has bit h - 1 set
//       ... patch list    over m values, of the bits above b, each at least 1, in h bits
//
// Decoding a block: each vector's values are its packed b bits, with an exception's high
// bits put back above them, plus the base, wrapping; then zigzag decoded if the mode says
// so. That gives the transformed sequence y of the whole block, which the inverse
// transform turns into q. Each value is q * s plus its remainder, if it has one. Nothing
// may overflow 64 bits there, and nothing else wraps that an encoder did not wrap.
//
// The encoder's choices, which the format leaves open:
//
//   - Head delta falls back to delta for a block it cannot code, one with a value of 2^61
//     or more far from every head, and the block's transform byte says delta.
//   - The scale. With adaptive_scale off it is 1. With it on, the candidates are 1, the
//     greatest common divisor of the whole block, and the commonest divisors of runs of
//     8 values, with the divisors those have in common, all taken from the block itself:
//     no factor such as 100 is built in. The block is encoded at each and the smallest
//     kept, so a block where a factor does not hold everywhere pays a remainder for each
//     value off it, or falls back to 1 if that is cheaper.
//   - Per vector, plain against zigzag, whichever is smaller. Plain takes the base as the
//     smallest value read as signed, so a column that only rises, like timestamps, pays
//     no sign bit; zigzag puts a large step either way among the exceptions.
//   - Per vector, the width b that makes the vector smallest, exceptions counted, among
//     the widths that leave at most half its values as exceptions, and the base as the
//     smallest value in the mode chosen. With patch off, b is the widest value's width
//     and there are no exceptions.
//
// So unlike the Gorilla codecs a sequence has more than one valid encoding, and the
// decoder checks structure, not the encoder's choices: every field in range, every size
// before it is used, every list in order, every byte of the payload used. The fuzz oracle
// checks that what decodes re-encodes to the same values, and that the encoder's own
// output is a fixed point of decode then encode.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

enum class IntTransform : std::uint8_t {
    none = 0,
    delta = 1,
    delta_of_delta = 2,
    head_delta = 3,
};

struct PforOptions {
    IntTransform transform = IntTransform::delta;
    bool adaptive_scale = false;
    bool patch = true;
};

// The fewest payload bytes a block of one vector takes: transform, scale bytes, a one-byte
// scale, the vector's three header bytes and its exception count. Each further vector
// takes at least 5 more.
inline constexpr std::size_t kPforMinPayloadBytes = 8;
inline constexpr std::size_t kPforMinVectorBytes = 5;

// How a column was coded, for explaining a ratio and the layout's costs.
struct PforStats {
    // Blocks coded with a scale above 1, and the remainders they carried.
    std::uint64_t scaled_blocks = 0;
    std::uint64_t remainders = 0;
    // Full vectors of 1,024, and values in the shorter vectors at the end of a block.
    std::uint64_t full_vectors = 0;
    std::uint64_t tail_values = 0;
    std::uint64_t zigzag_vectors = 0;
    std::uint64_t exceptions = 0;
    // Over full vectors only: the bits their packed values and exception lists took,
    // and what the same would take had each 128 values chosen their own width, at one
    // byte each for that width and two for the exception count. The difference is the
    // adaptivity a 1,024-value width gives up.
    std::uint64_t full_vector_bits = 0;
    std::uint64_t full_vector_bits_per_128 = 0;
    // What padding each shorter vector to 1,024 values, as an interleaved layout over
    // every vector would, would have added.
    std::uint64_t tail_padding_bits = 0;
    // Blocks asked for head delta that fell back to delta.
    std::uint64_t head_delta_fallbacks = 0;
};

// Appends one block holding `values`, which must hold 1 to kMaxBlockValues values.
[[nodiscard]] std::expected<void, CodecError> encode_pfor_block(std::span<const std::uint64_t> values,
                                                                const PforOptions& options,
                                                                std::vector<std::byte>& out,
                                                                PforStats* stats = nullptr);

// Decodes the block at the front of `block`, appending exactly its count of values to
// `out`, and returns the bytes it took. On error, `out` keeps what it held before.
[[nodiscard]] std::expected<std::size_t, CodecError> decode_pfor_block(
    std::span<const std::byte> block, std::vector<std::uint64_t>& out);

// Appends `values` as consecutive blocks of `block_values`, the last one shorter.
[[nodiscard]] std::expected<void, CodecError> encode_pfor(std::span<const std::uint64_t> values,
                                                          std::size_t block_values,
                                                          const PforOptions& options,
                                                          std::vector<std::byte>& out,
                                                          PforStats* stats = nullptr);

// Decodes a whole column that must hold exactly `count` values, appending them to `out`.
[[nodiscard]] std::expected<void, CodecError> decode_pfor(std::span<const std::byte> column,
                                                          std::uint64_t count,
                                                          std::vector<std::uint64_t>& out);

}  // namespace tick_store::codec
