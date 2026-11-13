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
// Each codec's encoded blocks also go through zstd at level 1, and each column's raw
// values at their wire width, so a generic compressor is measured on both. Every zstd
// frame is decompressed and compared byte for byte, as the codecs' blocks are.
//
// Beside the columns, an analysis section measures what the columns cannot show on their
// own: what the add references' interleaved sequences follow, how far back the messages
// that name an order reach for it, which fields can be derived, and the order-1 and
// recency entropies of the message type and locate. See Analysis below.
//
// To measure another codec, add a CodecEntry to the table in codec_eval.cpp: a block
// encoder, a block decoder of the block.hpp shape, the representation it is given, and
// which kinds of column it applies to. Nothing else changes.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
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
    // An identifier that mostly rises: order references and match numbers.
    reference,
    // A count or a distance: shares, and references recoded as differences.
    quantity,
    // A small set of codes: message types, locates, sides, flags and other bytes.
    symbol,
};

inline constexpr std::size_t kValueKinds = 6;

enum class StreamOrder : std::uint8_t {
    // One stream, in file order.
    file,
    // One stream per stock locate, each in file order.
    per_locate,
    // One stream per stock locate and side, each in file order. Add orders only.
    per_locate_side,
    // One stream per message type, each in file order.
    per_type,
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
    // The kinds of column it applies to, one bit per ValueKind.
    std::uint8_t kinds;
    // Names of the counters in use. An empty name marks one unused.
    std::array<std::string_view, kCodecCounters> counter_names;
};

[[nodiscard]] constexpr std::uint8_t kind_bit(ValueKind kind) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(kind));
}

[[nodiscard]] constexpr bool applies_to(const CodecEntry& entry, ValueKind kind) noexcept {
    return (entry.kinds & kind_bit(kind)) != 0;
}

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
    // Whether to count every distinct value, delta and delta-of-delta for their exact
    // order-0 entropies. Off where the distinct values would not fit in memory
    // comfortably or say nothing.
    bool count_values;
    bool count_deltas;
    bool count_dods;
    // Bytes a value takes on the wire, the width raw zstd is given it at.
    std::uint8_t raw_bytes;
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
    // Each encoded block through zstd level 1: the frames' bytes, and per block the smaller
    // of the frame and the block plus a byte to say which. zstd frames that did not
    // decompress to their block's exact bytes: must be 0.
    std::uint64_t zstd_bytes = 0;
    std::uint64_t zstd_or_plain_bytes = 0;
    std::uint64_t zstd_mismatches = 0;
    // Time spent in the block decoder and in zstd's decompressor on its frames, for their
    // ratio only: not a benchmark.
    std::uint64_t decode_ns = 0;
    std::uint64_t zstd_decompress_ns = 0;

    [[nodiscard]] std::uint64_t stored_bytes() const noexcept {
        return block_bytes + metadata_bytes;
    }
    [[nodiscard]] bool exact() const noexcept {
        return blocks_refused == 0 && word_mismatches == 0 && representation_failures == 0 &&
               zstd_mismatches == 0;
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
    // The exact order-0 entropy of the values themselves, in bits per value: the floor for
    // a code with no context at all, the same in every order. Absent where the column does
    // not count them, as for timestamps, which are nearly all distinct.
    std::optional<double> entropy_value_bits;
    std::uint64_t distinct_values = 0;
    std::vector<CodecResult> codecs;
    // The raw values at raw_bytes each, little-endian, block by block through zstd level
    // 1: their bytes, the frames' bytes, frames that did not come back exact (must be 0),
    // and the time zstd took to decompress them.
    std::uint64_t raw_bytes = 0;
    std::uint64_t raw_zstd_bytes = 0;
    std::uint64_t raw_zstd_mismatches = 0;
    std::uint64_t raw_zstd_decompress_ns = 0;
    std::uint64_t metadata_bytes = 0;

    [[nodiscard]] bool exact() const noexcept;
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

// The add references' interleaved sequences, through head delta's seven heads: how often
// each head is taken, the entropies of the token and of the gap, and whether the head a
// value extends was last extended by a message of the same locate, side or MPID. The
// baselines are the same shares between consecutive adds, what a sequence that followed
// nothing would show.
struct HeadModes {
    std::uint64_t values = 0;
    std::array<std::uint64_t, 8> tokens{};
    double token_entropy_bits = 0;
    double gap_entropy_bits = 0;
    double token_and_gap_entropy_bits = 0;
    std::uint64_t distinct_gaps = 0;
    // Steps that extend a head, tokens 0 to 6.
    std::uint64_t head_steps = 0;
    // Among all head steps. A step to a head no value has reached yet stays in head_steps, as
    // it does not in side_steps, and is compared with the default owner's locate, 0, so it
    // counts as the same locate only for a value of locate 0. That is at most
    // codec::kHeadDeltaHeads steps in a stream (review RR-2).
    std::uint64_t same_locate = 0;
    // Among head steps where both sides are known. A U carries no side: its new order keeps
    // the side of the order it replaces, unknown if that order is not on the book. A step to
    // a head no value has reached yet has no side to compare with, so it is not counted.
    std::uint64_t side_steps = 0;
    std::uint64_t same_side = 0;
    // Among steps where both messages are F, so both carry an MPID.
    std::uint64_t mpid_steps = 0;
    std::uint64_t same_mpid = 0;
    std::uint64_t consecutive = 0;
    std::uint64_t consecutive_same_locate = 0;
    // Among consecutive pairs where both sides are known.
    std::uint64_t consecutive_side_pairs = 0;
    std::uint64_t consecutive_same_side = 0;
    std::uint64_t consecutive_mpid = 0;
    std::uint64_t consecutive_same_mpid = 0;
    // The plain delta from the previous add reference: how many fall at each of -16 to
    // 16, and below and above.
    std::array<std::uint64_t, 33> small_deltas{};
    std::uint64_t deltas_below = 0;
    std::uint64_t deltas_above = 0;
};

// The messages that name an order (E, C, X, D and U's original), each against the
// message that created that order (A, F or U's new reference): how many, how many named
// no order on the book, how many carry a locate other than the order's, and how far back
// the order was created. Distances count creations, globally and within the locate.
inline constexpr std::array<std::uint64_t, 3> kLocalityBlocks{1'024, 4'096, 65'536};
inline constexpr std::array<std::uint64_t, 3> kLocalityWindowsNs{1'000'000, 1'000'000'000,
                                                                 60'000'000'000};

struct ReferenceLocality {
    char type = '?';
    std::uint64_t messages = 0;
    std::uint64_t unresolved = 0;
    std::uint64_t locate_mismatches = 0;
    // The order was created in the block of N creations the store is filling, and at most
    // N creations back.
    std::array<std::uint64_t, kLocalityBlocks.size()> same_block{};
    std::array<std::uint64_t, kLocalityBlocks.size()> within_distance{};
    // The order was created in the same aligned window of W nanoseconds, and at most W
    // nanoseconds before.
    std::array<std::uint64_t, kLocalityWindowsNs.size()> same_window{};
    std::array<std::uint64_t, kLocalityWindowsNs.size()> within_time{};
    // Reductions (E, C, X) whose shares are exactly the order's remaining shares.
    std::uint64_t shares_equal_remaining = 0;
};

struct Analysis {
    HeadModes add_heads;
    HeadModes creation_heads;
    std::vector<ReferenceLocality> references;
    std::uint64_t peak_live_orders = 0;
    std::uint64_t live_orders_at_end = 0;
    std::uint64_t duplicate_creations = 0;
    // The message type: order-0 entropy, and order-1 given the previous message's type.
    double type_entropy_bits = 0;
    double type_order1_entropy_bits = 0;
    // The locate as its recency rank, the number of distinct locates seen since its last
    // use, with a first use as one more symbol: every message, and only messages that
    // name no order, whose locate the order cannot give.
    double locate_recency_entropy_bits = 0;
    double unreferenced_locate_recency_entropy_bits = 0;
    std::uint64_t locate_first_uses = 0;
    // Messages with a stock field, and those whose stock differs from the directory's for
    // their locate, by type; a locate with no directory entry counts as differing.
    std::array<std::uint64_t, 24> stock_fields{};
    std::array<std::uint64_t, 24> stock_mismatches{};
    std::array<std::uint64_t, 24> stock_on_locate_zero{};
    // Records the decoder returned raw, by reason: unknown type, wrong length.
    std::uint64_t raw_unknown_type = 0;
    std::uint64_t raw_wrong_length = 0;
};

struct EvalOptions {
    std::uint32_t block_values = codec::kDefaultBlockValues;
    std::size_t max_examples = 8;
    // Only these columns, by name; all when empty. The analysis runs either way.
    std::vector<std::string> columns;
    // Called with each block a column cuts, before any codec sees it: the column's name,
    // the stream's index and the values. For tests; empty in the tool.
    std::function<void(std::string_view, std::size_t, std::span<const std::uint64_t>)>
        observe_block;
};

struct EvalResult {
    std::uint64_t messages = 0;
    // Records the decoder returned raw, which have no fields to read.
    std::uint64_t raw_skipped = 0;
    // How the walk of the file ended, from the frame cursor.
    std::string stop;
    std::vector<ColumnResult> columns;
    PermutationEstimate permutation;
    Analysis analysis;

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
    // As BuildInfo's: compared with `commit` only when commit_source is "git" (A-8).
    bool tree_dirty = true;
    // "git", "export" or "unknown", as BuildInfo's.
    std::string_view commit_source = "unknown";
    std::string_view compiler;
    std::string_view build_type;
    std::string_view sanitizers;
};

//   tick_codec_eval <source> <output.json> --day LABEL [--block-values N]
//                   [--prove-all-price4] [--columns NAME,NAME,...]
//
// --columns runs only the named columns, for a quick look; the JSON says so, and
// results meant to be kept come from a run of every column.
//
// Writes the results JSON, which holds no paths, and prints a one-line summary per
// column. Exit 0 if every value of every column came back exact and, when asked, every
// Price(4) came back through both doubles; 1 if any did not (the JSON says which); 2 on
// a usage or I/O error.
[[nodiscard]] int run_codec_eval_cli(std::span<const std::string_view> arguments,
                                     const EvalProvenance& provenance, CliOutput& output);

}  // namespace tick_store::harness
