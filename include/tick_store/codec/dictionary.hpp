#pragma once

// A dictionary codec for columns with few distinct values, such as shares, message types
// and flags: the block's distinct values once, then each value as an index into them.
//
// A block is the frame in block.hpp around this payload, little-endian throughout:
//
//   u8     mode           0 ranked dictionary, indices by PFOR
//                         1 ranked dictionary, indices by canonical Huffman codes
//                         2 sorted dictionary, move-to-front ranks by PFOR
//   u32    entries K      1 to the block's value count n; at most kDictMtfMaxEntries in
//                         mode 2
//   ...    dictionary     a PFOR block (pfor.hpp), its frame included, of the K entries
//   modes 0 and 2:
//   ...    indices        a PFOR block of n values, each below K
//   mode 1, one bit stream, most significant bit first, zero-padded to a whole byte:
//          longest L      8 bits: 0 if K = 1, otherwise 1 to kDictMaxCodeBits
//          counts         L fields of bit_width(K) bits: how many codes have each
//                         length from 1 to L
//          codes          n codes
//
// The dictionary's order is part of the format, so a block's dictionary has one valid
// order and every entry is used:
//
//   ranked   by how often the block uses each entry, most often first, and by value,
//            smallest first, among entries used equally often. So the commonest values
//            have the smallest indices, PFOR packs them in few bits and patches the rest
//            as exceptions: a dictionary of frequent values plus exceptions.
//   sorted   strictly ascending, which PFOR's delta codes cheaply, for many distinct
//            values. Each value is then coded by its rank in a move-to-front list of the
//            indices, which starts as 0, 1, ..., K - 1 and puts each index used at the
//            front: a value seen recently has a small rank.
//
// Huffman codes are canonical: entry i takes the i-th code in order of length and then
// index, and since a ranked dictionary puts commoner entries first, lengths never fall
// with the index, so the counts per length are the whole table. The code must be
// complete: its lengths fill the code space exactly, and L is the longest length used.
//
// The encoder's choices, which the format leaves open: the mode, the smallest of the
// three; and the PFOR blocks' own choices. So a sequence has more than one valid encoding,
// and the fuzz oracle checks values and the encoder's own consistency, as for PFOR. The
// decoder checks every field, every size before it is used, the dictionary's order and
// that every entry is used. Errors follow codec_error.hpp, with three of their own:
// "dict entry unused" has expected = the entry's index and actual = 0, its uses; "dict
// entry order" has expected and actual = the two entries out of order, i - 1 and i; and
// "dict entry repeated" has expected = 0 and actual = 1.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tick_store::codec {

enum class DictMode : std::uint8_t {
    ranked_pfor = 0,
    ranked_huffman = 1,
    sorted_move_to_front = 2,
};

inline constexpr std::size_t kDictModes = 3;

// The longest Huffman code. A block of at most 2^20 values cannot need a code longer than
// 29 bits, since a code of length l needs counts at least the Fibonacci number F(l + 1).
inline constexpr unsigned kDictMaxCodeBits = 32;

// Move-to-front costs a decoder up to K steps a value, so mode 2 is held to dictionaries
// of at most this many entries.
inline constexpr std::uint64_t kDictMtfMaxEntries = 2048;

// The fewest payload bytes a block takes: mode, entry count, and the dictionary's PFOR
// block at its smallest.
inline constexpr std::uint64_t kDictMinPayloadBytes = 1 + 4 + kBlockHeaderSize + 8;

struct DictStats {
    // Blocks coded in each mode.
    std::array<std::uint64_t, kDictModes> blocks{};
    // Dictionary entries over all blocks.
    std::uint64_t entries = 0;
    // Blocks encode_dict wrote as a copy of the block before, whose values they repeat.
    // They are counted in `blocks` and `entries` too.
    std::uint64_t repeated = 0;
};

// Appends one block holding `values`, which must hold 1 to kMaxBlockValues values.
[[nodiscard]] std::expected<void, CodecError> encode_dict_block(std::span<const std::uint64_t> values,
                                                                std::vector<std::byte>& out,
                                                                DictStats* stats = nullptr);

// Decodes the block at the front of `block`, appending exactly its count of values to
// `out`, and returns the bytes it took. On error, `out` keeps what it held before.
[[nodiscard]] std::expected<std::size_t, CodecError> decode_dict_block(
    std::span<const std::byte> block, std::vector<std::uint64_t>& out);

// Appends `values` as consecutive blocks of `block_values`, the last one shorter. A block
// whose values repeat the block before's is a copy of its bytes, which is what encoding it
// would give: the encoder is deterministic. A block of 2^20 values can come from 23 bytes,
// one entry with codes of no bits, and cut into blocks of 3 it would otherwise cost the
// whole encoder 349,526 times over.
[[nodiscard]] std::expected<void, CodecError> encode_dict(std::span<const std::uint64_t> values,
                                                          std::size_t block_values,
                                                          std::vector<std::byte>& out,
                                                          DictStats* stats = nullptr);

// Decodes a whole column that must hold exactly `count` values, appending them to `out`.
[[nodiscard]] std::expected<void, CodecError> decode_dict(std::span<const std::byte> column,
                                                          std::uint64_t count,
                                                          std::vector<std::uint64_t>& out);

}  // namespace tick_store::codec
