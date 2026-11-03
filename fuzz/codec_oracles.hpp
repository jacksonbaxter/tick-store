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
//        - non-canonical codecs (PFOR, Stream VByte), where the encoder makes choices the
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

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/price_double.hpp"
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

// Part 1 for one block at the front of `input`. Sets `taken` to its size if accepted,
// or to 0 if refused. The block is decoded in place first, to learn its size, and then
// again from an exact copy of just its bytes, so ASan sees any read past its end.
[[nodiscard]] inline const char* check_one_block(const CodecUnderTest& codec,
                                                 std::span<const std::byte> input,
                                                 Words& values, std::size_t& taken) {
    taken = 0;
    Words decoded{0xDEAD};
    const auto first = codec.decode_block(input, decoded);
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

    const std::span<const Encoder> encoders =
        codec.block_encoders != nullptr ? codec.block_encoders(block) : codec.encoders;
    for (const Encoder& encoder : encoders) {
        Bytes reencoded;
        if (!encoder.encode_block(got, reencoded).has_value()) {
            return "the encoder refused values its decoder accepted";
        }
        if (codec.canonical && !std::ranges::equal(reencoded, block)) {
            return "the re-encoded block differs from the input";
        }
        Words twice;
        const Bytes again = exact_bytes(reencoded);
        const auto second = codec.decode_block(again, twice);
        if (!second.has_value() || *second != again.size() || !std::ranges::equal(twice, got)) {
            return "the re-encoded block did not decode to the same values";
        }
        if (!codec.canonical) {
            Bytes repeat;
            if (!encoder.encode_block(got, repeat).has_value() || repeat != reencoded) {
                return "the encoder gave different bytes for the same values";
            }
        }
    }
    if (codec.extra_block_check != nullptr) {
        if (const char* failure = codec.extra_block_check(block, got)) {
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

// Part 1 over the whole input, then the column checks on the values it held.
[[nodiscard]] inline const char* check_as_column(const CodecUnderTest& codec,
                                                 std::span<const std::byte> input) {
    Words values;
    std::size_t offset = 0;
    while (offset < input.size()) {
        std::size_t taken = 0;
        if (const char* failure = check_one_block(codec, input.subspan(offset), values, taken)) {
            return failure;
        }
        if (taken == 0) {
            break;
        }
        offset += taken;
    }
    const bool whole = offset == input.size();
    if (whole && !input.empty()) {
        // The input is a valid column: the column decoder takes it at its exact count,
        // and refuses one value more or less.
        const Bytes copy = exact_bytes(input);
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
    for (const Encoder& encoder : codec.encoders) {
        for (const std::size_t block_values : kOracleBlockSizes) {
            Bytes column;
            if (!encoder.encode_column(values, block_values, column).has_value()) {
                return "the encoder refused a column of accepted values";
            }
            Words decoded;
            if (!codec.decode_column(exact_bytes(column), values.size(), decoded).has_value() ||
                decoded != values) {
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
        const auto done = codec.decode_column(exact_bytes(column), values.size(), decoded);
        if (!done.has_value()) {
            return "a block from the encoder did not decode";
        }
        if (decoded != values) {
            return "a block from the encoder decoded to other values";
        }
    }
    return nullptr;
}

[[nodiscard]] inline const char* check_codec(const CodecUnderTest& codec,
                                             std::span<const std::byte> input) {
    if (const char* failure = check_as_column(codec, input)) {
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
inline constexpr std::array<Encoder, 12> kPforEncoders{
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
};

// A PFOR block names its transform, so it is re-encoded with the four option sets of that
// transform; the column checks and part 2 still use all twelve. The decoder accepted the
// block, so the transform is 0 to 2.
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

[[nodiscard]] inline const char* check_pfor_column(std::span<const std::byte> input) {
    return check_codec(kPforCodec, input);
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
