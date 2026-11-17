#pragma once

// The checks the codec fuzz targets run, kept apart from libFuzzer so that the unit suite
// replays crashers through the same code. Each returns nullptr if the input passed, or a
// description of the broken property.
//
// One input is read two ways:
//
//   1. As a column of encoded blocks, straight into the block decoder (Z1). Whatever a
//      block decoder accepts must decode the same from an exact copy, and must be refused
//      once cut short. A refusal must repeat and leave the output alone. The accepted
//      values then go through every encoder the codec has:
//        - canonical codecs (Gorilla), where a sequence has one encoding: the block must
//          re-encode to exactly its bytes.
//        - non-canonical codecs (PFOR, Stream VByte, dictionary, run-length), where the encoder makes choices the
//          format leaves open: the re-encoded block must decode to the same values, the
//          encoder must give the same bytes twice, and a codec-specific check may hold
//          the re-encoding to more, such as never being larger.
//      The accepted values are then re-encoded at several block sizes and decoded as
//      whole columns with their exact count (Z2).
//   2. As values: its bytes, eight at a time, are a sequence of 64-bit words for each
//      real encoder, at a block size the input picks. The blocks must decode to exactly
//      those values. This catches an encoder writing what its decoder refuses.
//
// Every buffer handed to a decoder is a heap block of exactly its size, so ASan sees a
// read one byte past it.
//
// Every decode the oracle makes runs on the kernel set this process chose, which is the
// scalar set when TICK_STORE_SIMD=scalar caps it. Each block, and each column part 2
// decodes, is then decoded again with the scalar set forced on this thread: the two must
// give the same answer, refusal or values (the kernel-against-scalar oracle).
//
// How much of an input part 1 examines is an OracleBounds. Its defaults examine all of it,
// as the unit suite does; the fuzz targets of codecs whose blocks can hold many values in
// few bytes pass bounds of their own (kDictFuzzBounds, kRleFuzzBounds, kPforFuzzBounds),
// so that one input's work stays well inside libFuzzer's -timeout (fuzz/README.md,
// "Bounds"). Part 1 reads an input's blocks in order, in three tiers by the values taken
// before a block and the count its header states:
//   - within the cap, the whole of part 1 above, where the values taken before the block,
//     counted twice, and its count stay within the cap (OracleBounds::decoded_values says
//     why twice). Never budgeted there: both kernel sets' decodes and their agreement, the
//     decode from an exact copy, the refusal checks, the cut check, the decoder-side block
//     check (a dictionary's entry count, a run-length block's run count, against its
//     values) and the column's exact-count and wrong-count checks. The re-encodes and the
//     encoder-side block check take the cross-check budget, the block it runs out in checked
//     on the part it reaches, and past it a prefix of each of the next few blocks
//     (OracleBounds::past_budget_blocks); the column re-encodes take the budget's values;
//   - past the cap, up to the compare limit: the block is decoded by the scalar set in
//     place and by the chosen kernel set from an exact copy of its bytes, and the two must
//     agree, refusal or values, and a refused block must leave the output alone. Nothing
//     else;
//   - past that, up to the decode limit: the block is decoded once, in place, by the
//     chosen set, with nothing compared, so ASan and UBSan still see its decode.
// Past the decode limit nothing is examined. A refused block ends part 1 in any tier.

#include "oracle_counts.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/simd.hpp"
#include "tick_store/codec/stream_vbyte.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

namespace tick_store::codec::oracle {

using Bytes = std::vector<std::byte>;
using Words = std::vector<std::uint64_t>;

// One way of encoding: a block, and a column at a given block size.
struct Encoder {
    std::expected<void, CodecError> (*encode_block)(std::span<const std::uint64_t>,
                                                    std::vector<std::byte>&);
    std::expected<void, CodecError> (*encode_column)(std::span<const std::uint64_t>, std::size_t,
                                                     std::vector<std::byte>&);
};

// A check a codec adds on an accepted block and the values it held: nullptr if it passed.
using BlockCheck = const char* (*)(std::span<const std::byte> block,
                                   std::span<const std::uint64_t> values);

// Which of a codec's encoders an accepted block is re-encoded with, given its bytes.
using BlockEncoders = std::span<const Encoder> (*)(std::span<const std::byte> block);

// The functions one codec family gives the oracle.
struct CodecUnderTest {
    BlockDecoder decode_block;
    std::expected<void, CodecError> (*decode_column)(std::span<const std::byte>, std::uint64_t,
                                                     std::vector<std::uint64_t>&);
    // Every encoder: the column checks and part 2 use them all.
    std::span<const Encoder> encoders;
    // Whether the format has one encoding per sequence, so a block must re-encode to
    // exactly its bytes.
    bool canonical = true;
    // What the format fixes about an accepted block given its values, checked on the input
    // block and all its values: never budgeted.
    BlockCheck decoded_block_check = nullptr;
    // What the encoder must do with a block's values, on the values the cross-checks take
    // and, where those are the whole block, the input block, else the first encoder's
    // block of them.
    BlockCheck extra_block_check = nullptr;
    // The encoders each accepted block is re-encoded with, if not all of them.
    BlockEncoders block_encoders = nullptr;
};

[[nodiscard]] inline Bytes exact_bytes(std::span<const std::byte> input) {
    Bytes out(input.begin(), input.end());
    out.shrink_to_fit();
    return out;
}

// The oracle's decodes and encodes, each counted (oracle_counts.hpp): a block decode at the
// count its header states, a column decode at the count asked for, an encode at its values.
[[nodiscard]] inline std::expected<std::size_t, CodecError> counted_decode(
    const CodecUnderTest& codec, std::span<const std::byte> input, Words& out) {
    oracle_counts::counts.decoded += stated_block_count(input).value_or(0);
    return codec.decode_block(input, out);
}
[[nodiscard]] inline std::expected<void, CodecError> counted_decode_column(
    const CodecUnderTest& codec, std::span<const std::byte> column, std::uint64_t count,
    Words& out) {
    oracle_counts::counts.decoded += count;
    return codec.decode_column(column, count, out);
}
[[nodiscard]] inline std::expected<void, CodecError> counted_encode(
    const Encoder& encoder, std::span<const std::uint64_t> values, Bytes& out) {
    oracle_counts::counts.encoded += values.size();
    return encoder.encode_block(values, out);
}
[[nodiscard]] inline std::expected<void, CodecError> counted_encode_column(
    const Encoder& encoder, std::span<const std::uint64_t> values, std::size_t block_values,
    Bytes& out) {
    oracle_counts::counts.encoded += values.size();
    return encoder.encode_column(values, block_values, out);
}

// Block sizes the accepted values are re-encoded at, as whole columns.
// Part 2 covers blocks of one value, at a block size the input picks.
inline constexpr std::array<std::size_t, 3> kOracleBlockSizes{3, 64, kDefaultBlockValues};
// A block size just past one vector of the packed codecs, so part 2 reaches a full vector
// and a tail in one block.
inline constexpr std::size_t kOracleLongBlock = 1'025;

// How much of one input part 1 examines, in values from the input's start (the header
// overview above). The defaults examine everything.
struct OracleBounds {
    // The cap: a block goes through the whole of part 1 while twice the values taken before
    // it and the count its header states stay within this. The column checks run on those
    // blocks. Twice, because the wrong-count check, asking for one value fewer, decodes
    // every block but the last in full before the last is refused, so each value of a block
    // that another follows is decoded once more; counted as a value taken, no input within
    // the cap makes the never-budgeted checks decode more than the cap's values in one
    // block do, and a single block of up to the cap's values is still taken whole.
    std::size_t decoded_values = std::numeric_limits<std::size_t>::max();
    // The compare limit: past the cap, a block is decoded by both kernel sets and compared
    // while the values before it and its count stay within this.
    std::size_t compared_values = std::numeric_limits<std::size_t>::max();
    // The decode limit: past the compare limit, a block is decoded once while the values
    // before it and its count stay within this. Nothing past it is examined.
    std::size_t single_decoded_values = std::numeric_limits<std::size_t>::max();
    // The cross-check budget, within the cap: each block's re-encodes and its codec's
    // encoder-side check run on all its values while this lasts and on the part of the
    // block that it reaches where it runs out. The column re-encodes at kOracleBlockSizes
    // run on the first this many values taken.
    std::size_t cross_checked_values = std::numeric_limits<std::size_t>::max();
    // The prefix past the budget: of the blocks within the cap that start once the budget is
    // spent, the first this many still have their first kPastBudgetValues values (all of
    // them, in a shorter block) through the re-encodes and the encoder-side check. A block
    // within the cap past the budget and past these gets the never-budgeted checks only.
    std::size_t past_budget_blocks = std::numeric_limits<std::size_t>::max();
};

// The values of a block past the budget that the prefix takes: the middle oracle block size.
inline constexpr std::size_t kPastBudgetValues = kOracleBlockSizes[1];

// The fuzz targets' bounds, for the codecs whose blocks hold up to kMaxBlockValues values
// in a few bytes (dictionary, run-length, PFOR at width 0). Each bound is one constant below,
// with the figures it was set from beside it. The bar is 5 s an execution on the Linux host
// with its sibling cpu busy, half of -timeout (fuzz/README.md, "Bounds"). A bound is taken when
// the dearest input it admits, each such input a named seed built from these constants
// (bound_seeds.hpp), is predicted at no more than 4.4 s an execution there: its Mac median
// times the largest factor measured for its target, the Linux worst of five under the worst
// placement over the Mac median (dictionary 6.04, run-length 7.19, PFOR 7.71), a margin over
// the 1.13 swing measured between sessions. So the Mac medians are held under 728, 612 and
// 571 ms; where the least constant the bound's form allows (a whole number of maximal blocks,
// a power of two) lands a target within about 3% over its limit, it is taken and said so.
// Each figure below is ms of one execution through the fuzz target on the Mac (fuzz build,
// -detect_leaks=0, the median of seven rounds over every input timed beside it, each run
// started with load1 under 8), from builds of the target at the bounds named. To repeat one:
// set the constants, build the fuzz preset, which writes the named seeds, and run each seed
// through its target. The store writes codec blocks of kDefaultBlockValues values, far inside
// each cap. What one execution can decode, encode and compare at these bounds is the count
// table in fuzz/README.md, "Bounds", which FuzzOracles.TheCountTableHoldsTheConstants holds
// to these constants.

// A budget of `blocks` blocks: that many at the largest oracle block size, then
// kOracleLongBlock values and `extra` more, so that at each oracle block size (3, 64 and
// 4,096) the column re-encodes cross a block boundary and end on a short block, the last just
// past one 1,024-value vector.
[[nodiscard]] constexpr std::size_t ragged_budget(std::size_t blocks, std::size_t extra) {
    return (blocks * kOracleBlockSizes[2]) + kOracleLongBlock + extra;
}
[[nodiscard]] constexpr bool ends_on_short_blocks(std::size_t budget) {
    return budget % kOracleBlockSizes[0] != 0 && budget % kOracleBlockSizes[1] != 0 &&
           budget % kOracleBlockSizes[2] >= kOracleLongBlock;
}

// The prefix past the budget, the same for the three targets: the first 64 blocks within the
// cap that start once the budget is spent each have their first kPastBudgetValues (64) values
// re-encoded and through the encoder-side check; a block within the cap past those gets the
// never-budgeted checks only. 64 is the largest of 64, 32 and 16 with which every target's
// dearest input fits: the dearest inputs took the same time with no prefix (dictionary 692 and
// 694 ms, run-length 618 and 619, PFOR 566 and 566, no prefix and 64).
inline constexpr std::size_t kFuzzPastBudgetBlocks = 64;

// PFOR's cap: one maximal block, the format's largest.
inline constexpr std::size_t kPforFuzzDecodedValues = kMaxBlockValues;
// PFOR's compare limit, in values from the input's start: the cap and one maximal block, so a
// maximal block just past the cap is decoded by both kernel sets and compared. -max_len holds
// at most three maximal blocks, each at least 5,131 bytes. The costliest input, the dearest
// maximal block, 7 i^2 under delta of delta with the scale, within the cap, then a maximal
// head-delta block with the scale, compared, then a head-delta block of the 646,144 values the
// bytes left hold, decoded once, takes 568 ms. From builds of the target at other limits
// (before the prefix), medians of seven interleaved rounds: with every block past the cap
// compared, that input took 583 ms, past the 571; with the compare limit at the cap and two
// maximal blocks, 582; at the cap and one with nothing past it examined, 557 (the two blocks
// alone 561). Of the other shapes timed beside it at these limits, 213 one-value blocks (as
// many as -max_len holds) before a first block of 2^20 - 426 values took 560, the first block
// shortened by 128 blocks of 64 values 555, and by 16 with a third block after it 567.
inline constexpr std::size_t kPforFuzzComparedValues = kPforFuzzDecodedValues + kMaxBlockValues;
// PFOR's decode limit: one maximal block more, so the block -max_len leaves room for after the
// two is decoded once.
inline constexpr std::size_t kPforFuzzSingleDecodedValues = kPforFuzzComparedValues + kMaxBlockValues;
// PFOR's budget: four blocks, 17,410 values (ragged 1, 2 and 1,026). The first block shortened
// by 128 blocks of 64 values after it, compared, takes 557 ms.
inline constexpr std::size_t kPforFuzzCrossCheckedValues = ragged_budget(4, 1);
inline constexpr OracleBounds kPforFuzzBounds{.decoded_values = kPforFuzzDecodedValues,
                                              .compared_values = kPforFuzzComparedValues,
                                              .single_decoded_values = kPforFuzzSingleDecodedValues,
                                              .cross_checked_values = kPforFuzzCrossCheckedValues,
                                              .past_budget_blocks = kFuzzPastBudgetBlocks};

// Run-length's cap: one maximal block, the format's largest.
inline constexpr std::size_t kRleFuzzDecodedValues = kMaxBlockValues;
// Run-length's compare limit: the cap and one maximal block, the least the bound allows, so a
// maximal block just past the cap is decoded by both kernel sets and compared, and past it a
// block is not examined; its nested blocks are PFOR blocks, which fuzz_pfor compares at every
// size. The costliest input, the dearest maximal block (2^20 runs of one value, 11,302 bytes)
// within the cap, then a maximal block of as many runs as the 5 KB left hold (467,968),
// compared, takes 620 ms, 1.3% over the 612, and the first block shortened by one block of 64
// values 621, 1.5% over: taken as the least compare limit the bound's form allows, within the
// 3%. From builds of the target at other limits, the dearest input at each: two maximal blocks
// past the cap 621 to 623 ms, four 636, eight 656.
inline constexpr std::size_t kRleFuzzComparedValues = kRleFuzzDecodedValues + kMaxBlockValues;
// Run-length's budget: four blocks, 17,410 values (ragged 1, 2 and 1,026). Of the other shapes
// timed beside the costliest at these limits, 109 one-value blocks (as many as -max_len holds)
// before the first block took 557 ms, 40 blocks of 64 runs 594, maximal one-run blocks after the
// first 553. At a budget of two blocks the costliest took 597 ms.
inline constexpr std::size_t kRleFuzzCrossCheckedValues = ragged_budget(4, 1);
inline constexpr OracleBounds kRleFuzzBounds{.decoded_values = kRleFuzzDecodedValues,
                                             .compared_values = kRleFuzzComparedValues,
                                             .single_decoded_values = kRleFuzzComparedValues,
                                             .cross_checked_values = kRleFuzzCrossCheckedValues,
                                             .past_budget_blocks = kFuzzPastBudgetBlocks};

// The dictionary's cap: 2^18 values, the one cap below a maximal block, so that beside it the
// compare tier reaches a maximal block within the bar. The dearest block, values cycling
// through 2,048 under move-to-front with every rank the last, its ranks under head delta with
// the scale, fills it. The unit suite's grid checks blocks from just past the cap to 2^20
// values whole (fuzz_regression_test.cpp, OracleGrid).
inline constexpr std::size_t kDictFuzzDecodedValues = std::size_t{1} << 18U;
// The dictionary's compare limit, in values from the input's start: the cap and one maximal
// block, so every block of up to 2^20 values the input starts with or holds just past the cap
// is decoded by both kernel sets and compared. The costliest input, 300 one-value blocks (as
// many as -max_len holds) before the dearest block of the cap less 600 values, then a maximal
// block of it, compared, takes 695 ms; the cap's block and a maximal block alone
// 680. From builds of the target at other limits (when its cap counted the values
// before a block once, and each block past the budget took 64 values of the re-encodes), a
// quarter of a maximal block further took the cap's block and a maximal block to 733 ms.
inline constexpr std::size_t kDictFuzzComparedValues = kDictFuzzDecodedValues + kMaxBlockValues;
// The dictionary's decode limit: the compare limit, so past it a block is not examined. A
// decode-once tier of a quarter of a maximal block more took the cap's block and a maximal
// block to 704 ms, and one of a maximal block to 782 ms (from the same builds).
inline constexpr std::size_t kDictFuzzSingleDecodedValues = kDictFuzzComparedValues;
// The dictionary's budget: one block, 5,123 values (ragged 2, 3 and 1,027), so one block
// boundary and one partial block are still exercised at each oracle block size. It is the
// least budget, so that the compare tier reaches a maximal block within the bar: at a budget of
// two blocks (9,218 values) the costliest input took 855 ms and at four (17,410) 1,138, past
// the 728.
inline constexpr std::size_t kDictFuzzCrossCheckedValues = ragged_budget(1, 2);
inline constexpr OracleBounds kDictFuzzBounds{.decoded_values = kDictFuzzDecodedValues,
                                              .compared_values = kDictFuzzComparedValues,
                                              .single_decoded_values = kDictFuzzSingleDecodedValues,
                                              .cross_checked_values = kDictFuzzCrossCheckedValues,
                                              .past_budget_blocks = kFuzzPastBudgetBlocks};

static_assert(ends_on_short_blocks(kPforFuzzCrossCheckedValues) &&
                  ends_on_short_blocks(kRleFuzzCrossCheckedValues) &&
                  ends_on_short_blocks(kDictFuzzCrossCheckedValues),
              "a budget must end on a short block at every oracle block size: change its extra");
static_assert(kPforFuzzDecodedValues <= kPforFuzzComparedValues &&
                  kPforFuzzComparedValues <= kPforFuzzSingleDecodedValues &&
                  kRleFuzzDecodedValues <= kRleFuzzComparedValues &&
                  kDictFuzzDecodedValues <= kDictFuzzComparedValues &&
                  kDictFuzzComparedValues <= kDictFuzzSingleDecodedValues,
              "a limit below the one before it");
static_assert(kPforFuzzCrossCheckedValues >= 4 * kOracleBlockSizes[2] &&
                  kRleFuzzCrossCheckedValues >= 4 * kOracleBlockSizes[2],
              "PFOR's and run-length's budgets below four blocks at the largest oracle block size");
static_assert(kDictFuzzComparedValues >= kDictFuzzDecodedValues + kMaxBlockValues &&
                  kPforFuzzComparedValues >= kPforFuzzDecodedValues + kMaxBlockValues &&
                  kRleFuzzComparedValues >= kRleFuzzDecodedValues + kMaxBlockValues,
              "a compare tier must reach a maximal block just past the cap");
static_assert(std::has_single_bit(kFuzzPastBudgetBlocks), "the prefix is a power of two blocks");

// The kernel-against-scalar oracle for one block: decodes `input` again with the scalar
// set and compares with what the chosen set gave, `result` into `decoded`.
[[nodiscard]] inline bool scalar_agrees(const CodecUnderTest& codec, std::span<const std::byte> input,
                                        const std::expected<std::size_t, CodecError>& result,
                                        const Words& decoded) {
    const simd::ScalarKernelsOnThisThread scalar;
    Words by_scalar{0xDEAD};
    oracle_counts::counts.compared += stated_block_count(input).value_or(0);
    const auto again = counted_decode(codec, input, by_scalar);
    if (again.has_value() != result.has_value()) {
        return false;
    }
    if (again.has_value() ? *again != *result : again.error() != result.error()) {
        return false;
    }
    return by_scalar == decoded;
}

// The budgeted part of part 1 for one accepted block, `block` its exact bytes: the values
// `crossed`, a prefix of its values, go through every encoder and back, the same bytes twice
// where the codec leaves the encoder choices, and the codec's encoder-side check; and
// through the input's own bytes where `whole_block` and the codec is canonical.
[[nodiscard]] inline const char* cross_check_block(const CodecUnderTest& codec,
                                                   std::span<const std::byte> block,
                                                   std::span<const std::uint64_t> crossed,
                                                   bool whole_block) {
    const std::span<const Encoder> encoders =
        codec.block_encoders != nullptr ? codec.block_encoders(block) : codec.encoders;
    // The first encoder's block of the values crossed, which stands in for the input block
    // in the extra check when those are only a part of its values.
    Bytes first_reencoded;
    for (const Encoder& encoder : encoders) {
        Bytes reencoded;
        if (!counted_encode(encoder, crossed, reencoded).has_value()) {
            return "the encoder refused values its decoder accepted";
        }
        // Only the whole block's values have the input's bytes as their one encoding.
        if (codec.canonical && whole_block && !std::ranges::equal(reencoded, block)) {
            return "the re-encoded block differs from the input";
        }
        Words twice;
        const Bytes again = exact_bytes(reencoded);
        const auto second = counted_decode(codec, again, twice);
        if (!second.has_value() || *second != again.size() ||
            !std::ranges::equal(twice, crossed)) {
            return "the re-encoded block did not decode to the same values";
        }
        if (!codec.canonical) {
            Bytes repeat;
            if (!counted_encode(encoder, crossed, repeat).has_value() || repeat != reencoded) {
                return "the encoder gave different bytes for the same values";
            }
        }
        if (first_reencoded.empty()) {
            first_reencoded = std::move(reencoded);
        }
    }
    if (codec.extra_block_check != nullptr) {
        const std::span<const std::byte> checked_block =
            whole_block ? block : std::span<const std::byte>{first_reencoded};
        if (const char* failure = codec.extra_block_check(checked_block, crossed)) {
            return failure;
        }
    }
    return nullptr;
}

// Part 1 for one block at the front of `input`. Sets `taken` to its size if accepted,
// or to 0 if refused. The block is decoded in place first, to learn its size, and then
// again from an exact copy of just its bytes, so ASan sees any read past its end. The
// re-encodes and the extra check take the block's values up to `budget`, and `budget`
// loses what they took; with none left, they take its first kPastBudgetValues values while
// `prefix_blocks` lasts, which loses one, and otherwise do not run on this block.
[[nodiscard]] inline const char* check_one_block(const CodecUnderTest& codec,
                                                 std::span<const std::byte> input,
                                                 Words& values, std::size_t& taken,
                                                 std::size_t& budget, std::size_t& prefix_blocks) {
    taken = 0;
    Words decoded{0xDEAD};
    const auto first = counted_decode(codec, input, decoded);
    if (!scalar_agrees(codec, input, first, decoded)) {
        return "the kernel set and the scalar set decoded a block differently";
    }
    if (!first.has_value()) {
        Words again;
        const auto repeat = counted_decode(codec, input, again);
        if (repeat.has_value() || repeat.error() != first.error()) {
            return "a refusal did not repeat";
        }
        if (decoded != Words{0xDEAD}) {
            return "a refused block changed the output";
        }
        return nullptr;
    }
    if (*first < kBlockHeaderSize || *first > input.size()) {
        return "a block claimed bytes outside its input";
    }
    const Bytes copy = exact_bytes(input.first(*first));
    Words exact{0xDEAD};
    const auto from_copy = counted_decode(codec, copy, exact);
    if (!from_copy.has_value() || *from_copy != copy.size() || exact != decoded) {
        return "a block decoded differently from an exact copy of its bytes";
    }
    const std::span<const std::byte> block{copy};
    const std::span<const std::uint64_t> got = std::span{decoded}.subspan(1);
    if (got.empty() || got.size() > kMaxBlockValues) {
        return "an accepted block held no values or too many";
    }
    if (codec.decoded_block_check != nullptr) {
        if (const char* failure = codec.decoded_block_check(block, got)) {
            return failure;
        }
    }

    // The values the re-encodes and the extra check take: all of them inside the budget,
    // else what is left of it; past it, the prefix of the next prefix_blocks blocks.
    std::span<const std::uint64_t> crossed;
    if (budget > 0) {
        crossed = got.first(std::min(got.size(), budget));
        budget -= crossed.size();
    } else if (prefix_blocks > 0) {
        crossed = got.first(std::min(got.size(), kPastBudgetValues));
        --prefix_blocks;
    }
    const bool whole_block = crossed.size() == got.size();
    if (!crossed.empty()) {
        if (const char* failure = cross_check_block(codec, block, crossed, whole_block)) {
            return failure;
        }
    }

    // D10: a block cut anywhere is refused. The input picks where.
    const std::size_t cut = std::to_integer<std::size_t>(block[block.size() / 2]) * 131U %
                            block.size();
    Words ignored;
    if (counted_decode(codec, exact_bytes(block.first(cut)), ignored).has_value()) {
        return "a block cut short was accepted";
    }

    values.insert(values.end(), got.begin(), got.end());
    taken = *first;
    return nullptr;
}

// Past the cap, a block is decoded by the scalar set in place and by the chosen kernel set
// from an exact copy of its bytes, so ASan sees a read past its end by the vector kernels,
// and the two must agree; a refused block must leave the output alone. Sets `taken` to its
// size, or to 0 if it was refused, and `count` to its values.
[[nodiscard]] inline const char* compare_block(const CodecUnderTest& codec,
                                               std::span<const std::byte> input, Words& decoded,
                                               std::size_t& taken, std::size_t& count) {
    taken = 0;
    count = 0;
    Words by_scalar{0xDEAD};
    std::expected<std::size_t, CodecError> first = 0;
    {
        const simd::ScalarKernelsOnThisThread scalar;
        oracle_counts::counts.compared += stated_block_count(input).value_or(0);
        first = counted_decode(codec, input, by_scalar);
    }
    if (!first.has_value()) {
        decoded = Words{0xDEAD};
        const auto again = counted_decode(codec, input, decoded);
        if (again.has_value() || again.error() != first.error()) {
            return "past the cap, the kernel set and the scalar set decoded a block differently";
        }
        return by_scalar != Words{0xDEAD} || decoded != Words{0xDEAD}
                   ? "past the cap, a refused block changed the output"
                   : nullptr;
    }
    if (*first < kBlockHeaderSize || *first > input.size()) {
        return "past the cap, a block claimed bytes outside its input";
    }
    const Bytes copy = exact_bytes(input.first(*first));
    decoded = Words{0xDEAD};
    const auto again = counted_decode(codec, copy, decoded);
    if (!again.has_value() || *again != copy.size() || decoded != by_scalar) {
        return "past the cap, the kernel set and the scalar set decoded a block differently";
    }
    taken = *first;
    count = decoded.size() - 1;
    return nullptr;
}

// What a block stating `stated` values after `before` values taken within the cap counts
// against the cap: its own values and, twice, those before it (OracleBounds::decoded_values).
// Saturates rather than wraps, so a large count is past any cap.
[[nodiscard]] constexpr std::size_t within_cap_measure(std::size_t before, std::size_t stated) {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    if (before > (kMax - stated) / 2) {
        return kMax;
    }
    return (2 * before) + stated;
}

// Part 1 over the input, in the bounds' three tiers, then the column checks on the values
// the blocks within the cap held.
[[nodiscard]] inline const char* check_as_column(const CodecUnderTest& codec,
                                                 std::span<const std::byte> input,
                                                 const OracleBounds& bounds) {
    Words values;
    std::size_t offset = 0;
    std::size_t budget = bounds.cross_checked_values;
    std::size_t prefix_blocks = bounds.past_budget_blocks;
    bool capped = false;
    while (offset < input.size()) {
        // A block whose stated count, with the values taken before it counted twice, would
        // take part 1 past the cap goes to the tiers below (OracleBounds::decoded_values).
        // A count no block may state is the decoder's to refuse, so that block is checked as
        // any other.
        const std::span<const std::byte> rest = input.subspan(offset);
        if (const auto stated = stated_block_count(rest);
            stated.has_value() && within_cap_measure(values.size(), *stated) > bounds.decoded_values) {
            capped = true;
            break;
        }
        std::size_t taken = 0;
        if (const char* failure = check_one_block(codec, rest, values, taken, budget, prefix_blocks)) {
            return failure;
        }
        if (taken == 0) {
            break;
        }
        offset += taken;
    }
    const std::size_t column_end = offset;
    if (capped) {
        std::size_t seen = values.size();
        Words scratch;
        while (offset < input.size()) {
            const std::span<const std::byte> rest = input.subspan(offset);
            const auto stated = stated_block_count(rest);
            const std::size_t after = seen + stated.value_or(0);
            std::size_t taken = 0;
            std::size_t count = 0;
            if (after <= bounds.compared_values) {
                if (const char* failure = compare_block(codec, rest, scratch, taken, count)) {
                    return failure;
                }
            } else if (after <= bounds.single_decoded_values) {
                scratch.clear();
                const auto once = counted_decode(codec, rest, scratch);
                taken = once.has_value() ? *once : 0;
                count = scratch.size();
            }
            if (taken == 0) {
                break;
            }
            seen += count;
            offset += taken;
        }
    }
    // Stopped by the cap, the blocks within it are a valid column of their own.
    const bool whole = column_end == input.size() || capped;
    if (whole && column_end > 0) {
        // The input is a valid column: the column decoder takes it at its exact count,
        // and refuses one value more or less.
        const Bytes copy = exact_bytes(input.first(column_end));
        Words decoded;
        if (!counted_decode_column(codec, copy, values.size(), decoded).has_value() || decoded != values) {
            return "a valid column was refused at its exact count";
        }
        Words ignored;
        if (counted_decode_column(codec, copy, values.size() + 1, ignored).has_value() ||
            counted_decode_column(codec, copy, values.size() - 1, ignored).has_value()) {
            return "a column was accepted at the wrong count";
        }
    }
    // The column re-encodes take the first values up to the budget.
    const std::span<const std::uint64_t> crossed =
        std::span<const std::uint64_t>{values}.first(
            std::min(values.size(), bounds.cross_checked_values));
    for (const Encoder& encoder : codec.encoders) {
        for (const std::size_t block_values : kOracleBlockSizes) {
            Bytes column;
            if (!counted_encode_column(encoder, crossed, block_values, column).has_value()) {
                return "the encoder refused a column of accepted values";
            }
            Words decoded;
            if (!counted_decode_column(codec, exact_bytes(column), crossed.size(), decoded).has_value() ||
                !std::ranges::equal(decoded, crossed)) {
                return "a re-encoded column did not decode to the same values";
            }
        }
    }
    return nullptr;
}

// Part 2: the input's bytes as values, through the encoder and back.
[[nodiscard]] inline const char* check_as_values(const CodecUnderTest& codec,
                                                 std::span<const std::byte> input) {
    Words values;
    for (std::size_t at = 0; at + 8 <= input.size(); at += 8) {
        std::uint64_t word = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            word = (word << 8U) | std::to_integer<std::uint64_t>(input[at + i]);
        }
        values.push_back(word);
    }
    // Up to 97 values a block, or past one vector of 1,024 when the input asks.
    std::size_t block_values = 1;
    if (!input.empty()) {
        const auto pick = std::to_integer<std::size_t>(input.back());
        block_values = pick >= 0xF0U ? kOracleLongBlock + (pick - 0xF0U) : 1 + (pick % 97U);
    }
    for (const Encoder& encoder : codec.encoders) {
        Bytes column;
        if (!counted_encode_column(encoder, values, block_values, column).has_value()) {
            return "the encoder refused a column of values";
        }
        Words decoded;
        const Bytes exact = exact_bytes(column);
        const auto done = counted_decode_column(codec, exact, values.size(), decoded);
        if (!done.has_value()) {
            return "a block from the encoder did not decode";
        }
        if (decoded != values) {
            return "a block from the encoder decoded to other values";
        }
        Words by_scalar;
        {
            const simd::ScalarKernelsOnThisThread scalar;
            oracle_counts::counts.compared += values.size();
            if (!counted_decode_column(codec, exact, values.size(), by_scalar).has_value() ||
                by_scalar != decoded) {
                return "the kernel set and the scalar set decoded a column differently";
            }
        }
    }
    return nullptr;
}

[[nodiscard]] inline const char* check_codec(const CodecUnderTest& codec,
                                             std::span<const std::byte> input,
                                             const OracleBounds& bounds = {}) {
    if (const char* failure = check_as_column(codec, input, bounds)) {
        return failure;
    }
    return check_as_values(codec, input);
}

namespace detail {

inline std::expected<void, CodecError> encode_timestamp_block_plain(
    std::span<const std::uint64_t> values, std::vector<std::byte>& out) {
    return encode_timestamp_block(values, out);
}

inline std::expected<void, CodecError> encode_timestamps_plain(
    std::span<const std::uint64_t> values, std::size_t block_values,
    std::vector<std::byte>& out) {
    return encode_timestamps(values, block_values, out);
}

inline std::expected<void, CodecError> encode_xor_block_plain(std::span<const std::uint64_t> words,
                                                              std::vector<std::byte>& out) {
    return encode_xor_block(words, out);
}

inline std::expected<void, CodecError> encode_xor_plain(std::span<const std::uint64_t> words,
                                                        std::size_t block_values,
                                                        std::vector<std::byte>& out) {
    return encode_xor(words, block_values, out);
}

template <IntTransform Transform, bool Scale, bool Patch>
std::expected<void, CodecError> encode_pfor_block_as(std::span<const std::uint64_t> values,
                                                     std::vector<std::byte>& out) {
    return encode_pfor_block(
        values, PforOptions{.transform = Transform, .adaptive_scale = Scale, .patch = Patch}, out);
}

template <IntTransform Transform, bool Scale, bool Patch>
std::expected<void, CodecError> encode_pfor_as(std::span<const std::uint64_t> values,
                                               std::size_t block_values,
                                               std::vector<std::byte>& out) {
    return encode_pfor(values, block_values,
                       PforOptions{.transform = Transform, .adaptive_scale = Scale, .patch = Patch},
                       out);
}

template <IntTransform Transform, bool Scale, bool Patch>
constexpr Encoder pfor_encoder() {
    return {.encode_block = &encode_pfor_block_as<Transform, Scale, Patch>,
            .encode_column = &encode_pfor_as<Transform, Scale, Patch>};
}

template <bool Delta>
std::expected<void, CodecError> encode_svb_block_as(std::span<const std::uint64_t> values,
                                                    std::vector<std::byte>& out) {
    return encode_svb_block(values, Delta, out);
}

template <bool Delta>
std::expected<void, CodecError> encode_svb_as(std::span<const std::uint64_t> values,
                                              std::size_t block_values,
                                              std::vector<std::byte>& out) {
    return encode_svb(values, block_values, Delta, out);
}

inline constexpr std::array<Encoder, 1> kTimestampEncoders{
    Encoder{.encode_block = &encode_timestamp_block_plain,
            .encode_column = &encode_timestamps_plain}};
inline constexpr std::array<Encoder, 1> kXorEncoders{
    Encoder{.encode_block = &encode_xor_block_plain, .encode_column = &encode_xor_plain}};

// Every PFOR option set: each transform, with and without the adaptive scale and patching.
inline constexpr std::array<Encoder, 16> kPforEncoders{
    pfor_encoder<IntTransform::none, false, false>(),
    pfor_encoder<IntTransform::none, false, true>(),
    pfor_encoder<IntTransform::none, true, false>(),
    pfor_encoder<IntTransform::none, true, true>(),
    pfor_encoder<IntTransform::delta, false, false>(),
    pfor_encoder<IntTransform::delta, false, true>(),
    pfor_encoder<IntTransform::delta, true, false>(),
    pfor_encoder<IntTransform::delta, true, true>(),
    pfor_encoder<IntTransform::delta_of_delta, false, false>(),
    pfor_encoder<IntTransform::delta_of_delta, false, true>(),
    pfor_encoder<IntTransform::delta_of_delta, true, false>(),
    pfor_encoder<IntTransform::delta_of_delta, true, true>(),
    pfor_encoder<IntTransform::head_delta, false, false>(),
    pfor_encoder<IntTransform::head_delta, false, true>(),
    pfor_encoder<IntTransform::head_delta, true, false>(),
    pfor_encoder<IntTransform::head_delta, true, true>(),
};

// A PFOR block names its transform, so it is re-encoded with the four option sets of that
// transform; the column checks and part 2 still use all sixteen. The decoder accepted the
// block, so the transform is 0 to 3. A head-delta encoder may fall back to delta, which
// only changes the transform byte, and the oracle checks values, so that is allowed.
[[nodiscard]] inline std::span<const Encoder> pfor_block_encoders(std::span<const std::byte> block) {
    constexpr std::size_t kPerTransform = 4;
    const auto transform = std::to_integer<std::size_t>(block[kBlockHeaderSize]);
    return std::span<const Encoder>{kPforEncoders}.subspan(transform * kPerTransform,
                                                           kPerTransform);
}

inline constexpr std::array<Encoder, 2> kSvbEncoders{
    Encoder{.encode_block = &encode_svb_block_as<false>, .encode_column = &encode_svb_as<false>},
    Encoder{.encode_block = &encode_svb_block_as<true>, .encode_column = &encode_svb_as<true>},
};

// Stream VByte leaves the encoder one choice, zigzag or not, and every other field has one
// valid value. So re-encoding a block's values with its own delta flag gives exactly its
// bytes when the encoder makes the same choice, and otherwise a block that is smaller, or
// the same size when the input took zigzag, since the encoder takes zigzag only when it is
// strictly smaller.
[[nodiscard]] inline const char* check_svb_block(std::span<const std::byte> block,
                                                 std::span<const std::uint64_t> values) {
    constexpr std::uint8_t kDeltaFlag = 1;
    constexpr std::uint8_t kZigzagFlag = 2;
    const auto flags = std::to_integer<std::uint8_t>(block[kBlockHeaderSize]);
    Bytes reencoded;
    oracle_counts::counts.encoded += values.size();
    if (!encode_svb_block(values, (flags & kDeltaFlag) != 0, reencoded).has_value()) {
        return "the encoder refused values its decoder accepted";
    }
    if (std::to_integer<std::uint8_t>(reencoded[kBlockHeaderSize]) == flags) {
        if (!std::ranges::equal(reencoded, block)) {
            return "a block re-encoded with its own flags differs from the input";
        }
        return nullptr;
    }
    const bool tie_to_plain = (flags & kZigzagFlag) != 0 && reencoded.size() == block.size();
    if (reencoded.size() >= block.size() && !tie_to_plain) {
        return "the encoder chose zigzag the other way from a block no larger";
    }
    return nullptr;
}

inline std::expected<void, CodecError> encode_dict_block_plain(
    std::span<const std::uint64_t> values, std::vector<std::byte>& out) {
    return encode_dict_block(values, out);
}

inline std::expected<void, CodecError> encode_dict_plain(std::span<const std::uint64_t> values,
                                                         std::size_t block_values,
                                                         std::vector<std::byte>& out) {
    return encode_dict(values, block_values, out);
}

// The writer's hot columns allow ranked PFOR alone (store/itch_store.hpp).
inline std::expected<void, CodecError> encode_dict_block_pfor_only(
    std::span<const std::uint64_t> values, std::vector<std::byte>& out) {
    return encode_dict_block(values, out, nullptr, kDictRankedPforOnly);
}

inline std::expected<void, CodecError> encode_dict_pfor_only(std::span<const std::uint64_t> values,
                                                             std::size_t block_values,
                                                             std::vector<std::byte>& out) {
    return encode_dict(values, block_values, out, nullptr, kDictRankedPforOnly);
}

inline std::expected<void, CodecError> encode_rle_block_plain(
    std::span<const std::uint64_t> values, std::vector<std::byte>& out) {
    return encode_rle_block(values, out);
}

inline std::expected<void, CodecError> encode_rle_plain(std::span<const std::uint64_t> values,
                                                        std::size_t block_values,
                                                        std::vector<std::byte>& out) {
    return encode_rle(values, block_values, out);
}

inline constexpr std::array<Encoder, 2> kDictEncoders{
    Encoder{.encode_block = &encode_dict_block_plain, .encode_column = &encode_dict_plain},
    Encoder{.encode_block = &encode_dict_block_pfor_only,
            .encode_column = &encode_dict_pfor_only}};
inline constexpr std::array<Encoder, 1> kRleEncoders{
    Encoder{.encode_block = &encode_rle_block_plain, .encode_column = &encode_rle_plain}};

// The u32 little-endian field at `at` bytes into a block's payload.
[[nodiscard]] inline std::uint64_t payload_u32(std::span<const std::byte> block, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 4; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(block[kBlockHeaderSize + at + i - 1]);
    }
    return value;
}

inline constexpr std::size_t kDictModeAt = 0;
inline constexpr std::size_t kDictEntriesAt = 1;

[[nodiscard]] inline std::uint64_t distinct_count(std::span<const std::uint64_t> values) {
    Words distinct(values.begin(), values.end());
    std::ranges::sort(distinct);
    const auto unique_end = std::ranges::unique(distinct).begin();
    return static_cast<std::uint64_t>(unique_end - distinct.begin());
}

// A dictionary block's entries are distinct and every one is used, so its entry count is
// the number of distinct values it held, whichever mode it took. On the input block and
// all its values.
[[nodiscard]] inline const char* check_dict_entries(std::span<const std::byte> block,
                                                    std::span<const std::uint64_t> values) {
    if (payload_u32(block, kDictEntriesAt) != distinct_count(values)) {
        return "a dictionary block's entry count is not its count of distinct values";
    }
    return nullptr;
}

// The encoder must give the same count as the distinct values. It takes move-to-front only
// within its entry limit. Held to ranked PFOR, it must take that mode, with the same count,
// in a block no smaller than the one it chooses with every mode, since it always tries
// ranked PFOR.
[[nodiscard]] inline const char* check_dict_block(std::span<const std::byte> /*block*/,
                                                  std::span<const std::uint64_t> values) {
    constexpr std::size_t kModeAt = kDictModeAt;
    constexpr std::size_t kEntriesAt = kDictEntriesAt;
    const std::uint64_t count = distinct_count(values);
    Bytes reencoded;
    oracle_counts::counts.encoded += 2 * values.size();
    if (!encode_dict_block(values, reencoded).has_value()) {
        return "the encoder refused values its decoder accepted";
    }
    if (payload_u32(reencoded, kEntriesAt) != count) {
        return "the encoder wrote an entry count other than the distinct values";
    }
    const auto mode = std::to_integer<std::uint8_t>(reencoded[kBlockHeaderSize + kModeAt]);
    if (mode == static_cast<std::uint8_t>(DictMode::sorted_move_to_front) &&
        count > kDictMtfMaxEntries) {
        return "the encoder took move-to-front past its entry limit";
    }
    Bytes pfor_only;
    if (!encode_dict_block(values, pfor_only, nullptr, kDictRankedPforOnly).has_value()) {
        return "the encoder held to ranked PFOR refused values its decoder accepted";
    }
    if (std::to_integer<std::uint8_t>(pfor_only[kBlockHeaderSize + kModeAt]) !=
            static_cast<std::uint8_t>(DictMode::ranked_pfor) ||
        payload_u32(pfor_only, kEntriesAt) != count) {
        return "the encoder held to ranked PFOR took another mode or entry count";
    }
    if (pfor_only.size() < reencoded.size()) {
        return "the encoder with every mode wrote a block larger than ranked PFOR alone";
    }
    return nullptr;
}

inline constexpr std::size_t kRleRunsAt = 0;

[[nodiscard]] inline std::uint64_t maximal_runs(std::span<const std::uint64_t> values) {
    std::uint64_t runs = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        runs += i == 0 || values[i] != values[i - 1] ? 1U : 0U;
    }
    return runs;
}

// Runs are maximal, so a run-length block's run count is fixed by its values. On the input
// block and all its values.
[[nodiscard]] inline const char* check_rle_runs(std::span<const std::byte> block,
                                                std::span<const std::uint64_t> values) {
    if (payload_u32(block, kRleRunsAt) != maximal_runs(values)) {
        return "a run-length block's run count is not its count of maximal runs";
    }
    return nullptr;
}

// The encoder must write the maximal runs' count.
[[nodiscard]] inline const char* check_rle_block(std::span<const std::byte> /*block*/,
                                                 std::span<const std::uint64_t> values) {
    constexpr std::size_t kRunsAt = kRleRunsAt;
    const std::uint64_t runs = maximal_runs(values);
    Bytes reencoded;
    oracle_counts::counts.encoded += values.size();
    if (!encode_rle_block(values, reencoded).has_value()) {
        return "the encoder refused values its decoder accepted";
    }
    if (payload_u32(reencoded, kRunsAt) != runs) {
        return "the encoder wrote a run count other than the maximal runs";
    }
    return nullptr;
}

}  // namespace detail

inline constexpr CodecUnderTest kTimestampCodec{
    .decode_block = &decode_timestamp_block,
    .decode_column = &decode_timestamps,
    .encoders = detail::kTimestampEncoders,
    .canonical = true,
    .decoded_block_check = nullptr,
    .extra_block_check = nullptr,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kXorCodec{
    .decode_block = &decode_xor_block,
    .decode_column = &decode_xor,
    .encoders = detail::kXorEncoders,
    .canonical = true,
    .decoded_block_check = nullptr,
    .extra_block_check = nullptr,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kPforCodec{
    .decode_block = &decode_pfor_block,
    .decode_column = &decode_pfor,
    .encoders = detail::kPforEncoders,
    .canonical = false,
    .decoded_block_check = nullptr,
    .extra_block_check = nullptr,
    .block_encoders = &detail::pfor_block_encoders,
};

inline constexpr CodecUnderTest kSvbCodec{
    .decode_block = &decode_svb_block,
    .decode_column = &decode_svb,
    .encoders = detail::kSvbEncoders,
    .canonical = false,
    .decoded_block_check = nullptr,
    .extra_block_check = &detail::check_svb_block,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kDictCodec{
    .decode_block = &decode_dict_block,
    .decode_column = &decode_dict,
    .encoders = detail::kDictEncoders,
    .canonical = false,
    .decoded_block_check = &detail::check_dict_entries,
    .extra_block_check = &detail::check_dict_block,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kRleCodec{
    .decode_block = &decode_rle_block,
    .decode_column = &decode_rle,
    .encoders = detail::kRleEncoders,
    .canonical = false,
    .decoded_block_check = &detail::check_rle_runs,
    .extra_block_check = &detail::check_rle_block,
    .block_encoders = nullptr,
};

// Each of these three examines the whole input when called without bounds, as the unit
// suite calls it; its fuzz target calls the overload taking bounds with its own bounds.
[[nodiscard]] inline const char* check_dict_column(std::span<const std::byte> input,
                                                   const OracleBounds& bounds) {
    return check_codec(kDictCodec, input, bounds);
}

[[nodiscard]] inline const char* check_dict_column(std::span<const std::byte> input) {
    return check_dict_column(input, OracleBounds{});
}

[[nodiscard]] inline const char* check_rle_column(std::span<const std::byte> input,
                                                  const OracleBounds& bounds) {
    return check_codec(kRleCodec, input, bounds);
}

[[nodiscard]] inline const char* check_rle_column(std::span<const std::byte> input) {
    return check_rle_column(input, OracleBounds{});
}

[[nodiscard]] inline const char* check_pfor_column(std::span<const std::byte> input,
                                                   const OracleBounds& bounds) {
    return check_codec(kPforCodec, input, bounds);
}

[[nodiscard]] inline const char* check_pfor_column(std::span<const std::byte> input) {
    return check_pfor_column(input, OracleBounds{});
}

[[nodiscard]] inline const char* check_svb_column(std::span<const std::byte> input) {
    return check_codec(kSvbCodec, input);
}

[[nodiscard]] inline const char* check_timestamp_column(std::span<const std::byte> input) {
    return check_codec(kTimestampCodec, input);
}

// The price mappings, on every word a column held or the input spelled: an inverse that
// accepts a double must give an integer that maps back to exactly that double, and every
// Price(4), and every integer up to 2^53, must come back through its inverse.
[[nodiscard]] inline const char* check_price_mappings(std::span<const std::uint64_t> words) {
    constexpr std::array<std::uint64_t, 3> kScales{1, kPrice4Scale, kPrice8Scale};
    constexpr std::uint64_t kExactIntegers = std::uint64_t{1} << 53U;
    for (const std::uint64_t word : words) {
        const auto value = std::bit_cast<double>(word);
        for (const std::uint64_t scale : kScales) {
            const auto integer =
                double_to_scaled(value, scale, std::numeric_limits<std::uint64_t>::max());
            if (integer.has_value() &&
                std::bit_cast<std::uint64_t>(scaled_to_double(*integer, scale)) != word) {
                return "a price inverse accepted a double that is not its image";
            }
        }
        const auto price = static_cast<std::uint32_t>(word);
        if (dollars_to_price4(price4_to_dollars(price)) != price) {
            return "a Price(4) did not come back through dollars";
        }
        const std::uint64_t integer = word % (kExactIntegers + 1);
        if (double_to_scaled(scaled_to_double(integer, 1), 1,
                             std::numeric_limits<std::uint64_t>::max()) != integer) {
            return "an integer up to 2^53 did not come back through a double";
        }
    }
    return nullptr;
}

[[nodiscard]] inline const char* check_xor_column(std::span<const std::byte> input) {
    if (const char* failure = check_codec(kXorCodec, input)) {
        return failure;
    }
    // The words the input held as a column, if it was one, and as plain values.
    Words words;
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto taken = decode_xor_block(input.subspan(offset), words);
        if (!taken.has_value()) {
            break;
        }
        offset += *taken;
    }
    for (std::size_t at = 0; at + 8 <= input.size(); at += 8) {
        std::uint64_t word = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            word = (word << 8U) | std::to_integer<std::uint64_t>(input[at + i]);
        }
        words.push_back(word);
    }
    return check_price_mappings(words);
}

}  // namespace tick_store::codec::oracle
