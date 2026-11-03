#pragma once

// What tools/tick_codec_eval measures: the column codecs on the columns of a whole ITCH
// day. It walks the file once through the ITCH decoder, cuts each column into blocks of
// a fixed size as it goes, and for every codec registered for that column encodes each
// block, decodes it, and checks every value is exact before the block is dropped. So
// memory holds one block per stream, not the day.
//
// A value goes through two steps, and each is checked on its own:
//
//   representation  the 64-bit word the codec is given: the value itself, a price's
//                   dollar value as a double, or the integer as a double. Its inverse
//                   must give back the exact integer (codec/price_double.hpp).
//   codec           the words, encoded and decoded block by block. The decoded words
//                   must equal the stored words bit for bit.
//
// A failure in either is counted and the first few are kept with their values, so a
// value the representation cannot hold is reported, never rounded away.
//
// To measure another codec, add a CodecEntry to the table in codec_eval.cpp: a block
// encoder, a block decoder of the block.hpp shape, the representation it is given, and
// which columns it applies to. Nothing else changes.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::harness {

enum class ValueKind : std::uint8_t {
    timestamp_ns,
    // Price(4): a uint32 of $0.0001.
    price4,
    // Price(8): a uint64 of $0.00000001, only in the MWCB levels.
    price8,
};

enum class StreamOrder : std::uint8_t {
    // One stream, in file order.
    file,
    // One stream per stock locate, each in file order.
    per_locate,
    // One stream per stock locate and side, each in file order. Add orders only.
    per_locate_side,
};

enum class Representation : std::uint8_t {
    identity,
    // price / 10^4 or / 10^8 as an IEEE-754 double: what a system storing doubles stores.
    dollars,
    // The integer itself as a double.
    integer_as_double,
};

// Counters a codec fills as it encodes, named by its entry, for explaining a ratio.
inline constexpr std::size_t kCodecCounters = 10;
using CodecCounters = std::array<std::uint64_t, kCodecCounters>;

using EncodeBlock = std::expected<void, codec::CodecError> (*)(std::span<const std::uint64_t>,
                                                              std::vector<std::byte>&,
                                                              CodecCounters&);

struct CodecEntry {
    std::string_view name;
    // "gorilla" for Gorilla and its variants, otherwise the integer scheme's family. CR-5
    // compares the best integer scheme with Gorilla on the same column.
    std::string_view family;
    std::string_view what;
    EncodeBlock encode_block;
    codec::BlockDecoder decode_block;
    Representation representation;
    bool for_timestamps;
    bool for_prices;
    // Names of the counters in use. An empty name marks one unused.
    std::array<std::string_view, kCodecCounters> counter_names;
};

// Every codec measured, in report order.
[[nodiscard]] std::span<const CodecEntry> codec_registry() noexcept;

struct ColumnSpec {
    std::string_view name;
    // Which messages and fields feed it.
    std::string_view source;
    ValueKind kind;
    StreamOrder order;
    // The target it is compared with, if any, by its BENCHMARKS.md id.
    std::string_view target;
    // Whether to count every distinct delta, and for timestamps every delta-of-delta,
    // for their exact order-0 entropy. Off where the distinct values would not fit in
    // memory comfortably.
    bool exact_entropy;
};

[[nodiscard]] std::span<const ColumnSpec> column_specs() noexcept;

// The word a codec is given for `value`, and the value a decoded word stands for.
[[nodiscard]] std::uint64_t to_stored_word(Representation representation, ValueKind kind,
                                           std::uint64_t value) noexcept;
[[nodiscard]] std::expected<std::uint64_t, codec::CodecError> from_stored_word(
    Representation representation, ValueKind kind, std::uint64_t word) noexcept;

// One value that did not come back: its position in its stream, the stream's locate (0
// in a file-order column), the value, the word it was stored as, and what came back, if
// anything did. A refused block is reported by its first value.
struct Example {
    std::uint64_t index = 0;
    std::uint16_t locate = 0;
    std::uint64_t value = 0;
    std::uint64_t stored_word = 0;
    std::optional<std::uint64_t> decoded;
};

struct CodecResult {
    const CodecEntry* entry = nullptr;
    std::uint64_t values = 0;
    std::uint64_t blocks = 0;
    // Every block's bytes, headers included.
    std::uint64_t block_bytes = 0;
    std::uint64_t header_bytes = 0;
    // Beyond the blocks, what a reader needs to decode the column: each stream's value
    // count, 8 bytes, and for per-locate streams its locate, 2 bytes.
    std::uint64_t metadata_bytes = 0;
    // Blocks the decoder refused, and words that came back different. Both must be 0.
    std::uint64_t blocks_refused = 0;
    std::uint64_t word_mismatches = 0;
    // Values whose word did not map back to them. Must be 0.
    std::uint64_t representation_failures = 0;
    std::vector<Example> examples;
    CodecCounters counters{};

    [[nodiscard]] std::uint64_t stored_bytes() const noexcept {
        return block_bytes + metadata_bytes;
    }
    [[nodiscard]] bool exact() const noexcept {
        return blocks_refused == 0 && word_mismatches == 0 && representation_failures == 0;
    }
};

struct ColumnResult {
    const ColumnSpec* spec = nullptr;
    std::uint64_t values = 0;
    std::uint64_t streams = 0;
    // The census's "ideal code" of each stream's deltas and delta-of-deltas, summed over
    // streams, in bits per value: a yardstick for comparing transforms, not a codec.
    double ideal_delta_bits = 0;
    double ideal_delta_of_delta_bits = 0;
    // The exact order-0 entropy of the deltas, and of the delta-of-deltas for timestamps,
    // pooled over the column's streams, in bits per delta: the floor for any code that
    // sees one delta at a time with no context beyond the stream order. Absent when the
    // column does not count them. With how many distinct values each had.
    std::optional<double> entropy_delta_bits;
    std::optional<double> entropy_delta_of_delta_bits;
    std::uint64_t distinct_deltas = 0;
    std::uint64_t distinct_delta_of_deltas = 0;
    std::vector<CodecResult> codecs;
};

// What rebuilding file order from per-locate streams would cost if no locate column were
// stored: the exact order-0 entropy of the locate sequence, in bits per message.
struct PermutationEstimate {
    std::uint64_t add_messages = 0;
    double add_locate_entropy_bits = 0;
    double add_locate_side_entropy_bits = 0;
    std::uint64_t messages = 0;
    double locate_entropy_bits = 0;
    std::uint16_t largest_locate = 0;
};

struct EvalOptions {
    std::uint32_t block_values = codec::kDefaultBlockValues;
    std::size_t max_examples = 8;
};

struct EvalResult {
    std::uint64_t messages = 0;
    // Records the decoder returned raw, which have no fields to read.
    std::uint64_t raw_skipped = 0;
    // How the walk of the file ended, from the frame cursor.
    std::string stop;
    std::vector<ColumnResult> columns;
    PermutationEstimate permutation;

    [[nodiscard]] bool exact() const noexcept;
};

// `file` is a whole ITCH file.
[[nodiscard]] std::expected<EvalResult, std::string> evaluate_codecs(
    std::span<const std::byte> file, const EvalOptions& options);

// Every Price(4), all 2^32, through each price representation and back.
struct Price4Proof {
    Representation representation = Representation::dollars;
    std::uint64_t checked = 0;
    std::uint64_t failures = 0;
    std::optional<std::uint32_t> first_failure;
};

[[nodiscard]] Price4Proof prove_price4(Representation representation) noexcept;

// Where a result came from, which the tool fills from its build.
struct EvalProvenance {
    std::string_view commit;
    bool tree_dirty = true;
    std::string_view compiler;
    std::string_view build_type;
    std::string_view sanitizers;
};

//   tick_codec_eval <source> <output.json> --day LABEL [--block-values N]
//                   [--prove-all-price4]
//
// Writes the results JSON, which holds no paths, and prints a one-line summary per
// column. Exit 0 if every value of every column came back exact and, when asked, every
// Price(4) came back through both doubles; 1 if any did not (the JSON says which); 2 on
// a usage or I/O error.
[[nodiscard]] int run_codec_eval_cli(std::span<const std::string_view> arguments,
                                     const EvalProvenance& provenance, CliOutput& output);

}  // namespace tick_store::harness
