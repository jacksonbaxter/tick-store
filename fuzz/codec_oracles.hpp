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
// few bytes pass kFuzzBounds, which caps the values part 1 takes from one input and
// budgets the cross-checks, so that one input's work stays well inside libFuzzer's
// -timeout (fuzz/README.md, "Bounds"). Never budgeted, for every block part 1 takes: both
// kernel sets' decodes and their agreement, the decode from an exact copy, the refusal
// checks, the cut check, and the column's exact-count and wrong-count checks.

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

// A check a non-canonical codec adds on an accepted block and the values it held: nullptr
// if it passed.
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
    BlockCheck extra_block_check = nullptr;
    // The encoders each accepted block is re-encoded with, if not all of them.
    BlockEncoders block_encoders = nullptr;
};

[[nodiscard]] inline Bytes exact_bytes(std::span<const std::byte> input) {
    Bytes out(input.begin(), input.end());
    out.shrink_to_fit();
    return out;
}

// Block sizes the accepted values are re-encoded at, as whole columns.
// Part 2 covers blocks of one value, at a block size the input picks.
inline constexpr std::array<std::size_t, 3> kOracleBlockSizes{3, 64, kDefaultBlockValues};
// A block size just past one vector of the packed codecs, so part 2 reaches a full vector
// and a tail in one block.
inline constexpr std::size_t kOracleLongBlock = 1'025;

// How much of one input part 1 examines. The defaults examine everything.
struct OracleBounds {
    // The values part 1 takes from one input. A block is taken only while the values
    // taken before it and the count its header states stay within this. That block and
    // everything after it are not examined at all, as if the input ended before it; the
    // column checks then run on the blocks taken.
    std::size_t decoded_values = std::numeric_limits<std::size_t>::max();
    // The values the cross-checks take: each block's re-encodes and its codec's extra
    // check run on all its values while this lasts, on the part of the block that is left
    // where it runs out, and on the first kPastBudgetValues values of every block after.
    // The column re-encodes at kOracleBlockSizes run on the first this many values taken.
    std::size_t cross_checked_values = std::numeric_limits<std::size_t>::max();
};

// The values of each block past the budget that still go through every re-encode and the
// extra check: the middle oracle block size, so every block the oracle takes still meets
// the encoders.
inline constexpr std::size_t kPastBudgetValues = kOracleBlockSizes[1];

// The fuzz targets' bounds, for the codecs whose blocks hold up to kMaxBlockValues values
// in a few bytes (dictionary, run-length, PFOR at width 0). Their reasons and the costliest
// inputs they allow, timed, are in fuzz/README.md, "Bounds".
//
// The cap, one largest block's values: every block the format allows is taken whole as an
// input's first, and what part 1 never budgets, five or six decodes of every value taken,
// costs at most what one maximal block's does. The dearest is a move-to-front dictionary
// block of 2,048 entries whose every rank is the last, 2,047 steps a value: its decodes
// alone take about 0.65 s on the Mac, so a second such block would pass the 5 s the
// costliest input may take on the Linux host.
inline constexpr std::size_t kFuzzDecodedValues = kMaxBlockValues;
// The budget, two blocks at the largest oracle block size, the fewest that give each column
// re-encode a block boundary and a block repeating the one before at every size; a block's
// re-encodes then reach eight full vectors of the packed codecs. Each 4,096 values more
// costs that move-to-front input about 0.14 s on the Mac, which a third block would take
// past what its margin allows.
inline constexpr std::size_t kFuzzCrossCheckedValues = std::size_t{2} * kOracleBlockSizes[2];
inline constexpr OracleBounds kFuzzBounds{.decoded_values = kFuzzDecodedValues,
                                          .cross_checked_values = kFuzzCrossCheckedValues};

// The kernel-against-scalar oracle for one block: decodes `input` again with the scalar
// set and compares with what the chosen set gave, `result` into `decoded`.
[[nodiscard]] inline bool scalar_agrees(const CodecUnderTest& codec, std::span<const std::byte> input,
                                        const std::expected<std::size_t, CodecError>& result,
                                        const Words& decoded) {
    const simd::ScalarKernelsOnThisThread scalar;
    Words by_scalar{0xDEAD};
    const auto again = codec.decode_block(input, by_scalar);
    if (again.has_value() != result.has_value()) {
        return false;
    }
    if (again.has_value() ? *again != *result : again.error() != result.error()) {
        return false;
    }
    return by_scalar == decoded;
}

// Part 1 for one block at the front of `input`. Sets `taken` to its size if accepted,
// or to 0 if refused. The block is decoded in place first, to learn its size, and then
// again from an exact copy of just its bytes, so ASan sees any read past its end. The
// re-encodes and the extra check take the block's values up to `budget`, at least
// kPastBudgetValues of them, and `budget` loses what they took.
[[nodiscard]] inline const char* check_one_block(const CodecUnderTest& codec,
                                                 std::span<const std::byte> input,
                                                 Words& values, std::size_t& taken,
                                                 std::size_t& budget) {
    taken = 0;
    Words decoded{0xDEAD};
    const auto first = codec.decode_block(input, decoded);
    if (!scalar_agrees(codec, input, first, decoded)) {
        return "the kernel set and the scalar set decoded a block differently";
    }
    if (!first.has_value()) {
        Words again;
        const auto repeat = codec.decode_block(input, again);
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
    const auto from_copy = codec.decode_block(copy, exact);
    if (!from_copy.has_value() || *from_copy != copy.size() || exact != decoded) {
        return "a block decoded differently from an exact copy of its bytes";
    }
    const std::span<const std::byte> block{copy};
    const std::span<const std::uint64_t> got = std::span{decoded}.subspan(1);
    if (got.empty() || got.size() > kMaxBlockValues) {
        return "an accepted block held no values or too many";
    }

    // The values the re-encodes and the extra check take: all of them inside the budget,
    // else what is left of it, and never fewer than kPastBudgetValues.
    const std::size_t crossed_count =
        std::min(got.size(), std::max(budget, kPastBudgetValues));
    budget -= std::min(budget, crossed_count);
    const std::span<const std::uint64_t> crossed = got.first(crossed_count);
    const bool whole_block = crossed.size() == got.size();

    const std::span<const Encoder> encoders =
        codec.block_encoders != nullptr ? codec.block_encoders(block) : codec.encoders;
    // The first encoder's block of the values crossed, which stands in for the input block
    // in the extra check when those are only a part of its values.
    Bytes first_reencoded;
    for (const Encoder& encoder : encoders) {
        Bytes reencoded;
        if (!encoder.encode_block(crossed, reencoded).has_value()) {
            return "the encoder refused values its decoder accepted";
        }
        // Only the whole block's values have the input's bytes as their one encoding.
        if (codec.canonical && whole_block && !std::ranges::equal(reencoded, block)) {
            return "the re-encoded block differs from the input";
        }
        Words twice;
        const Bytes again = exact_bytes(reencoded);
        const auto second = codec.decode_block(again, twice);
        if (!second.has_value() || *second != again.size() ||
            !std::ranges::equal(twice, crossed)) {
            return "the re-encoded block did not decode to the same values";
        }
        if (!codec.canonical) {
            Bytes repeat;
            if (!encoder.encode_block(crossed, repeat).has_value() || repeat != reencoded) {
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

    // D10: a block cut anywhere is refused. The input picks where.
    const std::size_t cut = std::to_integer<std::size_t>(block[block.size() / 2]) * 131U %
                            block.size();
    Words ignored;
    if (codec.decode_block(exact_bytes(block.first(cut)), ignored).has_value()) {
        return "a block cut short was accepted";
    }

    values.insert(values.end(), got.begin(), got.end());
    taken = *first;
    return nullptr;
}

// Part 1 over the input, up to the bounds' cap, then the column checks on the values it
// held.
[[nodiscard]] inline const char* check_as_column(const CodecUnderTest& codec,
                                                 std::span<const std::byte> input,
                                                 const OracleBounds& bounds) {
    Words values;
    std::size_t offset = 0;
    std::size_t budget = bounds.cross_checked_values;
    bool capped = false;
    while (offset < input.size()) {
        // A block whose stated count would take part 1 past the cap is not decoded: it and
        // what follows are not examined. A count no block may state is the decoder's to
        // refuse, so that block is checked as any other.
        const std::span<const std::byte> rest = input.subspan(offset);
        if (const auto stated = stated_block_count(rest);
            stated.has_value() && values.size() + *stated > bounds.decoded_values) {
            capped = true;
            break;
        }
        std::size_t taken = 0;
        if (const char* failure = check_one_block(codec, rest, values, taken, budget)) {
            return failure;
        }
        if (taken == 0) {
            break;
        }
        offset += taken;
    }
    // Stopped by the cap, the blocks taken are a valid column of their own.
    const bool whole = offset == input.size() || capped;
    if (whole && offset > 0) {
        // The input is a valid column: the column decoder takes it at its exact count,
        // and refuses one value more or less.
        const Bytes copy = exact_bytes(input.first(offset));
        Words decoded;
        if (!codec.decode_column(copy, values.size(), decoded).has_value() || decoded != values) {
            return "a valid column was refused at its exact count";
        }
        Words ignored;
        if (codec.decode_column(copy, values.size() + 1, ignored).has_value() ||
            codec.decode_column(copy, values.size() - 1, ignored).has_value()) {
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
            if (!encoder.encode_column(crossed, block_values, column).has_value()) {
                return "the encoder refused a column of accepted values";
            }
            Words decoded;
            if (!codec.decode_column(exact_bytes(column), crossed.size(), decoded).has_value() ||
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
        if (!encoder.encode_column(values, block_values, column).has_value()) {
            return "the encoder refused a column of values";
        }
        Words decoded;
        const Bytes exact = exact_bytes(column);
        const auto done = codec.decode_column(exact, values.size(), decoded);
        if (!done.has_value()) {
            return "a block from the encoder did not decode";
        }
        if (decoded != values) {
            return "a block from the encoder decoded to other values";
        }
        Words by_scalar;
        {
            const simd::ScalarKernelsOnThisThread scalar;
            if (!codec.decode_column(exact, values.size(), by_scalar).has_value() ||
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

// A dictionary block's entries are distinct and every one is used, so its entry count is
// the number of distinct values it held, whichever mode it took, and the encoder must
// give the same count. The encoder takes move-to-front only within its entry limit. Held to
// ranked PFOR, it must take that mode, with the same count, in a block no smaller than the
// one it chooses with every mode, since it always tries ranked PFOR.
[[nodiscard]] inline const char* check_dict_block(std::span<const std::byte> block,
                                                  std::span<const std::uint64_t> values) {
    constexpr std::size_t kModeAt = 0;
    constexpr std::size_t kEntriesAt = 1;
    Words distinct(values.begin(), values.end());
    std::ranges::sort(distinct);
    const auto unique_end = std::ranges::unique(distinct).begin();
    const auto count = static_cast<std::uint64_t>(unique_end - distinct.begin());
    if (payload_u32(block, kEntriesAt) != count) {
        return "a dictionary block's entry count is not its count of distinct values";
    }
    Bytes reencoded;
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

// Runs are maximal, so a run-length block's run count is fixed by its values, and the
// encoder must write the same count.
[[nodiscard]] inline const char* check_rle_block(std::span<const std::byte> block,
                                                 std::span<const std::uint64_t> values) {
    constexpr std::size_t kRunsAt = 0;
    std::uint64_t runs = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        runs += i == 0 || values[i] != values[i - 1] ? 1U : 0U;
    }
    if (payload_u32(block, kRunsAt) != runs) {
        return "a run-length block's run count is not its count of maximal runs";
    }
    Bytes reencoded;
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
    .extra_block_check = nullptr,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kXorCodec{
    .decode_block = &decode_xor_block,
    .decode_column = &decode_xor,
    .encoders = detail::kXorEncoders,
    .canonical = true,
    .extra_block_check = nullptr,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kPforCodec{
    .decode_block = &decode_pfor_block,
    .decode_column = &decode_pfor,
    .encoders = detail::kPforEncoders,
    .canonical = false,
    .extra_block_check = nullptr,
    .block_encoders = &detail::pfor_block_encoders,
};

inline constexpr CodecUnderTest kSvbCodec{
    .decode_block = &decode_svb_block,
    .decode_column = &decode_svb,
    .encoders = detail::kSvbEncoders,
    .canonical = false,
    .extra_block_check = &detail::check_svb_block,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kDictCodec{
    .decode_block = &decode_dict_block,
    .decode_column = &decode_dict,
    .encoders = detail::kDictEncoders,
    .canonical = false,
    .extra_block_check = &detail::check_dict_block,
    .block_encoders = nullptr,
};

inline constexpr CodecUnderTest kRleCodec{
    .decode_block = &decode_rle_block,
    .decode_column = &decode_rle,
    .encoders = detail::kRleEncoders,
    .canonical = false,
    .extra_block_check = &detail::check_rle_block,
    .block_encoders = nullptr,
};

// Each of these three examines the whole input when called without bounds, as the unit
// suite calls it; its fuzz target calls the overload taking bounds with kFuzzBounds.
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
