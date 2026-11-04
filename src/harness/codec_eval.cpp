#include "tick_store/harness/codec_eval.hpp"

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/int_transform.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/stream_vbyte.hpp"
#include "tick_store/harness/census.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <sys/utsname.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::harness {

namespace {

constexpr std::string_view kToolVersion = "3";
constexpr std::size_t kLocates = std::size_t{1} << 16U;
// Bytes a reader needs per stream beyond its blocks: the value count, and for a
// per-locate stream the locate it belongs to.
constexpr std::uint64_t kStreamCountBytes = 8;
constexpr std::uint64_t kStreamLocateBytes = 2;
constexpr std::uint64_t kStreamSideBytes = 1;
constexpr std::uint64_t kStreamTypeBytes = 1;
// One stream slot per alternative of itch::Message, raw records included.
constexpr std::size_t kTypeSlots = std::variant_size_v<itch::Message>;
constexpr int kZstdLevel = 1;
constexpr std::string_view kGorilla = "gorilla";

void add_dod(CodecCounters& counters, const codec::DodStats& stats) {
    for (std::size_t i = 0; i < stats.buckets.size(); ++i) {
        counters[i] += stats.buckets[i];
    }
}

std::expected<void, codec::CodecError> gorilla_dod_block(std::span<const std::uint64_t> values,
                                                         std::vector<std::byte>& out,
                                                         CodecCounters& counters) {
    codec::DodStats stats;
    auto done = codec::encode_timestamp_block(values, out, &stats);
    add_dod(counters, stats);
    return done;
}

std::expected<void, codec::CodecError> gorilla_xor_block(std::span<const std::uint64_t> words,
                                                         std::vector<std::byte>& out,
                                                         CodecCounters& counters) {
    codec::XorStats stats;
    auto done = codec::encode_xor_block(words, out, &stats);
    counters[0] += stats.zero;
    counters[1] += stats.reuse;
    counters[2] += stats.new_window;
    counters[3] += stats.data_bits;
    return done;
}

// One PFOR variant: its options fixed at compile time, so each is a plain function.
template <codec::IntTransform Transform, bool Scale, bool Patch>
std::expected<void, codec::CodecError> pfor_block(std::span<const std::uint64_t> values,
                                                  std::vector<std::byte>& out,
                                                  CodecCounters& counters) {
    codec::PforStats stats;
    auto done = codec::encode_pfor_block(
        values, codec::PforOptions{.transform = Transform, .adaptive_scale = Scale, .patch = Patch},
        out, &stats);
    counters[0] += stats.scaled_blocks;
    counters[1] += stats.remainders;
    counters[2] += stats.full_vectors;
    counters[3] += stats.tail_values;
    counters[4] += stats.zigzag_vectors;
    counters[5] += stats.exceptions;
    counters[6] += stats.full_vector_bits;
    counters[7] += stats.full_vector_bits_per_128;
    counters[8] += stats.tail_padding_bits;
    counters[9] += stats.head_delta_fallbacks;
    return done;
}

template <bool Delta>
std::expected<void, codec::CodecError> svb_block(std::span<const std::uint64_t> values,
                                                 std::vector<std::byte>& out,
                                                 CodecCounters& counters) {
    codec::SvbStats stats;
    auto done = codec::encode_svb_block(values, Delta, out, &stats);
    counters[0] += stats.zigzag_blocks;
    for (std::size_t i = 0; i < stats.lengths.size(); ++i) {
        counters[1 + i] += stats.lengths[i];
    }
    counters[5] += stats.highs;
    return done;
}

std::expected<void, codec::CodecError> dict_block(std::span<const std::uint64_t> values,
                                                  std::vector<std::byte>& out,
                                                  CodecCounters& counters) {
    codec::DictStats stats;
    auto done = codec::encode_dict_block(values, out, &stats);
    for (std::size_t i = 0; i < stats.blocks.size(); ++i) {
        counters[i] += stats.blocks[i];
    }
    counters[3] += stats.entries;
    return done;
}

std::expected<void, codec::CodecError> rle_block(std::span<const std::uint64_t> values,
                                                 std::vector<std::byte>& out,
                                                 CodecCounters& counters) {
    codec::RleStats stats;
    auto done = codec::encode_rle_block(values, out, &stats);
    counters[0] += stats.runs;
    counters[1] += stats.delta_blocks;
    return done;
}

constexpr std::array<std::string_view, kCodecCounters> kDodCounters{
    "zero", "bucket_16_bits", "bucket_19_bits", "bucket_23_bits", "escape_64_bits", "", "", "", "",
    ""};
constexpr std::array<std::string_view, kCodecCounters> kXorCounters{
    "zero", "reuse", "new_window", "data_bits", "", "", "", "", "", ""};
constexpr std::array<std::string_view, kCodecCounters> kPforCounters{
    "scaled_blocks",    "remainders", "full_vectors",     "tail_values",
    "zigzag_vectors",   "exceptions", "full_vector_bits", "full_vector_bits_if_width_per_128",
    "tail_padding_bits_if_padded_to_1024", ""};
constexpr std::array<std::string_view, kCodecCounters> kPforHeadCounters{
    "scaled_blocks",    "remainders", "full_vectors",     "tail_values",
    "zigzag_vectors",   "exceptions", "full_vector_bits", "full_vector_bits_if_width_per_128",
    "tail_padding_bits_if_padded_to_1024", "head_delta_fallbacks_to_delta"};
constexpr std::array<std::string_view, kCodecCounters> kSvbCounters{
    "zigzag_blocks", "low_1_byte", "low_2_bytes", "low_3_bytes", "low_4_bytes", "high_patches",
    "", "", "", ""};
constexpr std::array<std::string_view, kCodecCounters> kDictCounters{
    "ranked_pfor_blocks", "ranked_huffman_blocks", "sorted_move_to_front_blocks", "entries",
    "", "", "", "", "", ""};
constexpr std::array<std::string_view, kCodecCounters> kRleCounters{
    "runs", "delta_value_blocks", "", "", "", "", "", "", "", ""};

constexpr unsigned kTimestamps = kind_bit(ValueKind::timestamp_ns);
constexpr unsigned kPrices = unsigned{kind_bit(ValueKind::price4)} | kind_bit(ValueKind::price8);
constexpr unsigned kReferences = kind_bit(ValueKind::reference);
constexpr unsigned kQuantities = kind_bit(ValueKind::quantity);
constexpr unsigned kSymbols = kind_bit(ValueKind::symbol);

[[nodiscard]] constexpr std::uint8_t kinds(unsigned bits) noexcept {
    return static_cast<std::uint8_t>(bits);
}

using codec::IntTransform;

// The codecs measured. A new codec is one more row here.
constexpr std::array kRegistry{
    CodecEntry{.name = "gorilla-dod",
               .family = kGorilla,
               .what = "Gorilla delta-of-delta, nanosecond buckets of 16, 19 and 23 bits",
               .encode_block = &gorilla_dod_block,
               .decode_block = &codec::decode_timestamp_block,
               .representation = Representation::identity,
               .kinds = kinds(kTimestamps),
               .counter_names = kDodCounters},
    CodecEntry{.name = "gorilla-xor-f64-dollars",
               .family = kGorilla,
               .what = "Gorilla XOR on the dollar value as a double, price / 10^4 or / 10^8",
               .encode_block = &gorilla_xor_block,
               .decode_block = &codec::decode_xor_block,
               .representation = Representation::dollars,
               .kinds = kinds(kPrices),
               .counter_names = kXorCounters},
    CodecEntry{.name = "gorilla-xor-f64-integer",
               .family = kGorilla,
               .what = "Gorilla XOR on the integer price as a double",
               .encode_block = &gorilla_xor_block,
               .decode_block = &codec::decode_xor_block,
               .representation = Representation::integer_as_double,
               .kinds = kinds(kPrices),
               .counter_names = kXorCounters},
    CodecEntry{.name = "gorilla-xor-u64-integer",
               .family = kGorilla,
               .what = "Gorilla XOR on the raw integer price",
               .encode_block = &gorilla_xor_block,
               .decode_block = &codec::decode_xor_block,
               .representation = Representation::identity,
               .kinds = kinds(kPrices),
               .counter_names = kXorCounters},
    CodecEntry{.name = "pfor-delta",
               .family = "pfor",
               .what = "Delta, frame of reference per 1,024 values, interleaved bit-packing, "
                       "patched exceptions",
               .encode_block = &pfor_block<IntTransform::delta, false, true>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kTimestamps | kPrices | kReferences),
               .counter_names = kPforCounters},
    CodecEntry{.name = "pfor-delta-unpatched",
               .family = "pfor",
               .what = "Delta, frame of reference per 1,024 values, interleaved bit-packing at "
                       "the widest value's width",
               .encode_block = &pfor_block<IntTransform::delta, false, false>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kTimestamps | kPrices | kReferences),
               .counter_names = kPforCounters},
    CodecEntry{.name = "pfor-dod",
               .family = "pfor",
               .what = "Delta-of-delta, frame of reference per 1,024 values, interleaved "
                       "bit-packing, patched exceptions",
               .encode_block = &pfor_block<IntTransform::delta_of_delta, false, true>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kTimestamps),
               .counter_names = kPforCounters},
    CodecEntry{.name = "pfor-dod-unpatched",
               .family = "pfor",
               .what = "Delta-of-delta, frame of reference per 1,024 values, interleaved "
                       "bit-packing at the widest value's width",
               .encode_block = &pfor_block<IntTransform::delta_of_delta, false, false>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kTimestamps),
               .counter_names = kPforCounters},
    CodecEntry{.name = "svb-delta",
               .family = "stream-vbyte",
               .what = "Delta, Stream VByte on the low 32 bits, high halves patched, zigzag "
                       "per block if smaller",
               .encode_block = &svb_block<true>,
               .decode_block = &codec::decode_svb_block,
               .representation = Representation::identity,
               .kinds = kinds(kTimestamps | kPrices | kReferences),
               .counter_names = kSvbCounters},
    CodecEntry{.name = "pfor-for",
               .family = "pfor",
               .what = "Frame of reference per 1,024 values, no delta, interleaved bit-packing, "
                       "patched exceptions",
               .encode_block = &pfor_block<IntTransform::none, false, true>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kPrices | kReferences | kQuantities | kSymbols),
               .counter_names = kPforCounters},
    CodecEntry{.name = "pfor-for-scaled",
               .family = "pfor",
               .what = "Adaptive scale per block, then frame of reference per 1,024 values, "
                       "interleaved bit-packing, patched exceptions",
               .encode_block = &pfor_block<IntTransform::none, true, true>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kPrices | kQuantities),
               .counter_names = kPforCounters},
    CodecEntry{.name = "pfor-delta-scaled",
               .family = "pfor",
               .what = "Adaptive scale per block, then delta, frame of reference per 1,024 "
                       "values, interleaved bit-packing, patched exceptions",
               .encode_block = &pfor_block<IntTransform::delta, true, true>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kPrices),
               .counter_names = kPforCounters},
    CodecEntry{.name = "pfor-delta-scaled-unpatched",
               .family = "pfor",
               .what = "Adaptive scale per block, then delta, frame of reference per 1,024 "
                       "values, interleaved bit-packing at the widest value's width",
               .encode_block = &pfor_block<IntTransform::delta, true, false>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kPrices),
               .counter_names = kPforCounters},
    CodecEntry{.name = "pfor-head-delta",
               .family = "pfor",
               .what = "Head delta, each value against the nearest of its seven most recent "
                       "heads, then frame of reference per 1,024 values, interleaved "
                       "bit-packing, patched exceptions",
               .encode_block = &pfor_block<IntTransform::head_delta, false, true>,
               .decode_block = &codec::decode_pfor_block,
               .representation = Representation::identity,
               .kinds = kinds(kReferences),
               .counter_names = kPforHeadCounters},
    CodecEntry{.name = "svb-plain",
               .family = "stream-vbyte",
               .what = "No delta, Stream VByte on the low 32 bits, high halves patched",
               .encode_block = &svb_block<false>,
               .decode_block = &codec::decode_svb_block,
               .representation = Representation::identity,
               .kinds = kinds(kQuantities | kSymbols),
               .counter_names = kSvbCounters},
    CodecEntry{.name = "dict",
               .family = "dictionary",
               .what = "Dictionary per block: ranked entries with PFOR or canonical Huffman "
                       "indices, or sorted entries with move-to-front ranks, the smallest",
               .encode_block = &dict_block,
               .decode_block = &codec::decode_dict_block,
               .representation = Representation::identity,
               .kinds = kinds(kQuantities | kSymbols),
               .counter_names = kDictCounters},
    CodecEntry{.name = "rle",
               .family = "run-length",
               .what = "Runs per block: run values and lengths as nested PFOR blocks",
               .encode_block = &rle_block,
               .decode_block = &codec::decode_rle_block,
               .representation = Representation::identity,
               .kinds = kinds(kQuantities | kSymbols),
               .counter_names = kRleCounters},
};

// The columns, in report order. kColumn* below index this table.
constexpr std::array kColumns{
    ColumnSpec{.name = "timestamp",
               .source = "every message's timestamp",
               .kind = ValueKind::timestamp_ns,
               .order = StreamOrder::file,
               .target = "CR-3",
               .count_values = false,
               .count_deltas = true,
               .count_dods = true,
               .raw_bytes = 6},
    ColumnSpec{.name = "timestamp_per_locate",
               .source = "every message's timestamp, one stream per stock locate",
               .kind = ValueKind::timestamp_ns,
               .order = StreamOrder::per_locate,
               .target = "CR-3",
               .count_values = false,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 6},
    ColumnSpec{.name = "add_price",
               .source = "A and F price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = "CR-4",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "add_price_per_locate",
               .source = "A and F price, one stream per stock locate",
               .kind = ValueKind::price4,
               .order = StreamOrder::per_locate,
               .target = "CR-4",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "add_price_per_locate_side",
               .source = "A and F price, one stream per stock locate and side",
               .kind = ValueKind::price4,
               .order = StreamOrder::per_locate_side,
               .target = "CR-4",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "replace_price",
               .source = "U price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "executed_price",
               .source = "C execution price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "trade_price",
               .source = "P price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "mwcb_level",
               .source = "V levels 1, 2 and 3, in that order: the only Price(8) fields",
               .kind = ValueKind::price8,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "add_ref",
               .source = "A and F order reference number",
               .kind = ValueKind::reference,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = false,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "add_ref_per_locate",
               .source = "A and F order reference number, one stream per stock locate",
               .kind = ValueKind::reference,
               .order = StreamOrder::per_locate,
               .target = "CR-6",
               .count_values = false,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "add_ref_vs_running_max",
               .source = "A and F order reference number less the largest A or F reference "
                       "before it, zigzag coded: invertible in file order",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "creation_ref",
               .source = "every reference that creates an order: A and F, and U's new "
                       "reference, in file order",
               .kind = ValueKind::reference,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = false,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "replace_new_ref",
               .source = "U new order reference number",
               .kind = ValueKind::reference,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = false,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "replace_new_minus_original",
               .source = "U new reference less its original reference, zigzag coded",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "referenced_ref",
               .source = "the reference of every message that names an order: E, C, X, D and "
                       "U's original, in file order",
               .kind = ValueKind::reference,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = false,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "referenced_ref_per_type",
               .source = "the reference of every message that names an order, one stream per "
                       "type: E, C, X, D and U's original",
               .kind = ValueKind::reference,
               .order = StreamOrder::per_type,
               .target = "CR-6",
               .count_values = false,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "ref_distance_E",
               .source = "E reference as the creations (A, F, U new) since the one that "
                       "created its order; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_distance_C",
               .source = "C reference as the creations (A, F, U new) since the one that "
                       "created its order; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_distance_X",
               .source = "X reference as the creations (A, F, U new) since the one that "
                       "created its order; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_distance_D",
               .source = "D reference as the creations (A, F, U new) since the one that "
                       "created its order; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_distance_U",
               .source = "U's original reference as the creations (A, F, U new) since the one "
                       "that created its order; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_locate_distance_E",
               .source = "E reference as the creations in the order's locate since the one "
                       "that created it; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_locate_distance_C",
               .source = "C reference as the creations in the order's locate since the one "
                       "that created it; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_locate_distance_X",
               .source = "X reference as the creations in the order's locate since the one "
                       "that created it; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_locate_distance_D",
               .source = "D reference as the creations in the order's locate since the one "
                       "that created it; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "ref_locate_distance_U",
               .source = "U's original reference as the creations in the order's locate since "
                       "the one that created it; 0 for an order not on the book",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-6",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "add_shares",
               .source = "A and F shares",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-7",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "add_shares_per_locate",
               .source = "A and F shares, one stream per stock locate",
               .kind = ValueKind::quantity,
               .order = StreamOrder::per_locate,
               .target = "CR-7",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "executed_shares",
               .source = "E executed shares",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-7",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "executed_with_price_shares",
               .source = "C executed shares",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-7",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "cancelled_shares",
               .source = "X cancelled shares",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-7",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "replace_shares",
               .source = "U shares",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "CR-7",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "reduction_shares_vs_remaining",
               .source = "E, C and X shares as 0 where they are the order's remaining shares, "
                       "else shares + 1, one stream per type",
               .kind = ValueKind::quantity,
               .order = StreamOrder::per_type,
               .target = "CR-7",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "trade_shares",
               .source = "P shares",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "cross_shares",
               .source = "Q shares",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "imbalance_shares",
               .source = "I paired shares and imbalance shares, in that order",
               .kind = ValueKind::quantity,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "message_type",
               .source = "every message's type byte",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
    ColumnSpec{.name = "stock_locate",
               .source = "every message's stock locate",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 2},
    ColumnSpec{.name = "stock_locate_unreferenced",
               .source = "the stock locate of every message that names no order: all but E, C, "
                       "X, D and U",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 2},
    ColumnSpec{.name = "tracking_number",
               .source = "every message's tracking number",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 2},
    ColumnSpec{.name = "tracking_number_per_type",
               .source = "every message's tracking number, one stream per type",
               .kind = ValueKind::symbol,
               .order = StreamOrder::per_type,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 2},
    ColumnSpec{.name = "add_side",
               .source = "A and F buy/sell indicator, as its byte",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
    ColumnSpec{.name = "add_side_per_locate",
               .source = "A and F buy/sell indicator, one stream per stock locate",
               .kind = ValueKind::symbol,
               .order = StreamOrder::per_locate,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
    ColumnSpec{.name = "mpid",
               .source = "F attribution, its four bytes as a big-endian integer",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "printable",
               .source = "C printable flag, as its byte",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
    ColumnSpec{.name = "match_number",
               .source = "E, C, P, Q and B match number, in file order",
               .kind = ValueKind::reference,
               .order = StreamOrder::file,
               .target = "",
               .count_values = false,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "trade_ref",
               .source = "P order reference number",
               .kind = ValueKind::reference,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 8},
    ColumnSpec{.name = "trade_side",
               .source = "P buy/sell indicator, as its byte",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
    ColumnSpec{.name = "cross_price",
               .source = "Q cross price",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "cross_type",
               .source = "Q cross type, as its byte",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
    ColumnSpec{.name = "imbalance_price",
               .source = "I far, near and current reference price, in that order",
               .kind = ValueKind::price4,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = true,
               .count_dods = false,
               .raw_bytes = 4},
    ColumnSpec{.name = "imbalance_flags",
               .source = "I imbalance direction, cross type and price variation indicator, in "
                       "that order, as their bytes",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
    ColumnSpec{.name = "rare_rest_bytes",
               .source = "every payload byte after the common header (type, locate, tracking "
                       "number, timestamp) of S, R, H, Y, L, W, K, J, h, N and O: V and B "
                       "have columns of their own",
               .kind = ValueKind::symbol,
               .order = StreamOrder::file,
               .target = "",
               .count_values = true,
               .count_deltas = false,
               .count_dods = false,
               .raw_bytes = 1},
};

constexpr std::size_t kColumnTimestamp = 0;
constexpr std::size_t kColumnTimestampPerLocate = 1;
constexpr std::size_t kColumnAddPrice = 2;
constexpr std::size_t kColumnAddPricePerLocate = 3;
constexpr std::size_t kColumnAddPricePerLocateSide = 4;
constexpr std::size_t kColumnReplacePrice = 5;
constexpr std::size_t kColumnExecutedPrice = 6;
constexpr std::size_t kColumnTradePrice = 7;
constexpr std::size_t kColumnMwcbLevel = 8;
constexpr std::size_t kColumnAddRef = 9;
constexpr std::size_t kColumnAddRefPerLocate = 10;
constexpr std::size_t kColumnAddRefVsRunningMax = 11;
constexpr std::size_t kColumnCreationRef = 12;
constexpr std::size_t kColumnReplaceNewRef = 13;
constexpr std::size_t kColumnReplaceNewMinusOriginal = 14;
constexpr std::size_t kColumnReferencedRef = 15;
constexpr std::size_t kColumnReferencedRefPerType = 16;
constexpr std::size_t kColumnRefDistanceE = 17;
constexpr std::size_t kColumnRefDistanceC = 18;
constexpr std::size_t kColumnRefDistanceX = 19;
constexpr std::size_t kColumnRefDistanceD = 20;
constexpr std::size_t kColumnRefDistanceU = 21;
constexpr std::size_t kColumnRefLocateDistanceE = 22;
constexpr std::size_t kColumnRefLocateDistanceC = 23;
constexpr std::size_t kColumnRefLocateDistanceX = 24;
constexpr std::size_t kColumnRefLocateDistanceD = 25;
constexpr std::size_t kColumnRefLocateDistanceU = 26;
constexpr std::size_t kColumnAddShares = 27;
constexpr std::size_t kColumnAddSharesPerLocate = 28;
constexpr std::size_t kColumnExecutedShares = 29;
constexpr std::size_t kColumnExecutedWithPriceShares = 30;
constexpr std::size_t kColumnCancelledShares = 31;
constexpr std::size_t kColumnReplaceShares = 32;
constexpr std::size_t kColumnReductionSharesVsRemaining = 33;
constexpr std::size_t kColumnTradeShares = 34;
constexpr std::size_t kColumnCrossShares = 35;
constexpr std::size_t kColumnImbalanceShares = 36;
constexpr std::size_t kColumnMessageType = 37;
constexpr std::size_t kColumnStockLocate = 38;
constexpr std::size_t kColumnStockLocateUnreferenced = 39;
constexpr std::size_t kColumnTrackingNumber = 40;
constexpr std::size_t kColumnTrackingNumberPerType = 41;
constexpr std::size_t kColumnAddSide = 42;
constexpr std::size_t kColumnAddSidePerLocate = 43;
constexpr std::size_t kColumnMpid = 44;
constexpr std::size_t kColumnPrintable = 45;
constexpr std::size_t kColumnMatchNumber = 46;
constexpr std::size_t kColumnTradeRef = 47;
constexpr std::size_t kColumnTradeSide = 48;
constexpr std::size_t kColumnCrossPrice = 49;
constexpr std::size_t kColumnCrossType = 50;
constexpr std::size_t kColumnImbalancePrice = 51;
constexpr std::size_t kColumnImbalanceFlags = 52;
constexpr std::size_t kColumnRareRestBytes = 53;
static_assert(kColumnRareRestBytes + 1 == kColumns.size());

[[nodiscard]] bool applies(const CodecEntry& entry, const ColumnSpec& spec) noexcept {
    return applies_to(entry, spec.kind);
}

[[nodiscard]] std::uint64_t scale_of(ValueKind kind) noexcept {
    return kind == ValueKind::price8 ? codec::kPrice8Scale : codec::kPrice4Scale;
}

[[nodiscard]] std::uint64_t max_of(ValueKind kind) noexcept {
    return kind == ValueKind::price4 ? std::numeric_limits<std::uint32_t>::max()
                                     : std::numeric_limits<std::uint64_t>::max();
}

// Counts of every distinct 64-bit value, in an open-addressing table, for an exact
// order-0 entropy. Sixteen bytes a slot, at most half full.
class ValueCounter {
public:
    void add(std::uint64_t value) {
        if ((used_ + 1) * 2 > slots_.size()) {
            grow();
        }
        Slot& slot = find(value);
        if (slot.count == 0) {
            slot.value = value;
            ++used_;
        }
        ++slot.count;
        ++total_;
    }

    [[nodiscard]] std::uint64_t distinct() const noexcept { return used_; }

    // Bits per value: log2(N) - sum(c log2 c) / N.
    [[nodiscard]] double entropy_bits() const noexcept {
        if (total_ == 0) {
            return 0;
        }
        double sum = 0;
        for (const Slot& slot : slots_) {
            if (slot.count > 0) {
                const auto count = static_cast<double>(slot.count);
                sum += count * std::log2(count);
            }
        }
        const auto total = static_cast<double>(total_);
        return std::log2(total) - (sum / total);
    }

private:
    struct Slot {
        std::uint64_t value = 0;
        std::uint64_t count = 0;
    };

    // SplitMix64's finaliser, so runs of nearby values spread over the table.
    [[nodiscard]] static std::uint64_t mix(std::uint64_t z) noexcept {
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] Slot& find(std::uint64_t value) noexcept {
        const std::size_t mask = slots_.size() - 1;
        std::size_t at = static_cast<std::size_t>(mix(value)) & mask;
        while (slots_[at].count != 0 && slots_[at].value != value) {
            at = (at + 1) & mask;
        }
        return slots_[at];
    }

    void grow() {
        const std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.empty() ? kFirstSlots : old.size() * 2, Slot{});
        for (const Slot& slot : old) {
            if (slot.count > 0) {
                find(slot.value) = slot;
            }
        }
    }

    static constexpr std::size_t kFirstSlots = 1024;
    std::vector<Slot> slots_;
    std::uint64_t used_ = 0;
    std::uint64_t total_ = 0;
};

[[nodiscard]] double entropy_of_counts(std::span<const std::uint64_t> counts) noexcept {
    std::uint64_t total = 0;
    double sum = 0;
    for (const std::uint64_t count : counts) {
        if (count > 0) {
            total += count;
            sum += static_cast<double>(count) * std::log2(static_cast<double>(count));
        }
    }
    if (total == 0) {
        return 0;
    }
    return std::log2(static_cast<double>(total)) - (sum / static_cast<double>(total));
}

// The census's view of one stream: deltas from the second value, delta-of-deltas from
// the third, both as signed differences. Differences wrap like the codecs' do. A column
// that counts exact entropies passes its counters; the delta-of-delta counter is null
// for prices.
struct StreamShape {
    std::uint64_t previous = 0;
    std::uint64_t previous_delta = 0;
    std::uint8_t seen = 0;

    void observe(std::uint64_t value, Log2Histogram& delta, Log2Histogram& dod,
                 ValueCounter* deltas, ValueCounter* dods) {
        if (seen > 0) {
            const std::uint64_t step = value - previous;
            delta.add(static_cast<std::int64_t>(step));
            if (deltas != nullptr) {
                deltas->add(step);
            }
            if (seen > 1) {
                dod.add(static_cast<std::int64_t>(step - previous_delta));
                if (dods != nullptr) {
                    dods->add(step - previous_delta);
                }
            }
            previous_delta = step;
        }
        seen = seen > 1 ? seen : static_cast<std::uint8_t>(seen + 1);
        previous = value;
    }
};

[[nodiscard]] std::size_t streams_of(StreamOrder order) noexcept {
    switch (order) {
        case StreamOrder::file:
            return 1;
        case StreamOrder::per_locate:
            return kLocates;
        case StreamOrder::per_locate_side:
            return kLocates * 2;
        case StreamOrder::per_type:
            return kTypeSlots;
    }
    return 1;
}

// zstd's contexts and buffers, shared by every column: one block is in flight at a time.
class Zstd {
public:
    Zstd() : compress_(ZSTD_createCCtx()), decompress_(ZSTD_createDCtx()) {
        if (compress_ == nullptr || decompress_ == nullptr) {
            throw std::bad_alloc();
        }
    }

    // Compresses `input` at kZstdLevel and decompresses the frame again. Returns the
    // frame's size, or nullopt if either step failed or the bytes did not come back
    // exact. Adds the time decompression took to `decompress_ns`.
    [[nodiscard]] std::optional<std::size_t> round_trip(std::span<const std::byte> input,
                                                        std::uint64_t& decompress_ns) {
        frame_.resize(ZSTD_compressBound(input.size()));
        const std::size_t size = ZSTD_compressCCtx(compress_.get(), frame_.data(), frame_.size(),
                                                   input.data(), input.size(), kZstdLevel);
        if (ZSTD_isError(size) != 0U) {
            return std::nullopt;
        }
        back_.resize(input.size());
        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        const std::size_t got = ZSTD_decompressDCtx(decompress_.get(), back_.data(), back_.size(),
                                                    frame_.data(), size);
        decompress_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        if (ZSTD_isError(got) != 0U || got != input.size() || !std::ranges::equal(back_, input)) {
            return std::nullopt;
        }
        return size;
    }

private:
    struct FreeCompress {
        void operator()(ZSTD_CCtx* context) const noexcept { ZSTD_freeCCtx(context); }
    };
    struct FreeDecompress {
        void operator()(ZSTD_DCtx* context) const noexcept { ZSTD_freeDCtx(context); }
    };
    std::unique_ptr<ZSTD_CCtx, FreeCompress> compress_;
    std::unique_ptr<ZSTD_DCtx, FreeDecompress> decompress_;
    std::vector<std::byte> frame_;
    std::vector<std::byte> back_;
};

// One column through every codec registered for it, a block at a time.
class ColumnRun {
public:
    ColumnRun(const ColumnSpec& spec, const EvalOptions& options, Zstd& zstd)
        : spec_(&spec),
          options_(&options),
          zstd_(&zstd),
          streams_(streams_of(spec.order)),
          shapes_(streams_.size()) {
        for (const CodecEntry& entry : kRegistry) {
            if (applies(entry, spec)) {
                CodecResult result;
                result.entry = &entry;
                results_.push_back(std::move(result));
            }
        }
    }

    // `key` picks the stream: the side (1 for sell) in a per-locate-side column, the
    // message's alternative in a per-type column. Ignored otherwise.
    void add(std::uint16_t locate, std::uint64_t value, std::size_t key = 0) {
        std::size_t stream = 0;
        if (spec_->order == StreamOrder::per_locate) {
            stream = locate;
        } else if (spec_->order == StreamOrder::per_locate_side) {
            stream = (std::size_t{locate} * 2) + (key != 0 ? 1U : 0U);
        } else if (spec_->order == StreamOrder::per_type) {
            stream = key;
        }
        Stream& into = streams_[stream];
        into.pending.push_back(value);
        shapes_[stream].observe(value, delta_, dod_, spec_->count_deltas ? &deltas_ : nullptr,
                                spec_->count_dods ? &dods_ : nullptr);
        if (spec_->count_values) {
            values_seen_.add(value);
        }
        ++values_;
        if (into.pending.size() == options_->block_values) {
            flush(into, stream, locate);
        }
    }

    [[nodiscard]] ColumnResult finish() {
        std::uint64_t streams = 0;
        for (std::size_t stream = 0; stream < streams_.size(); ++stream) {
            Stream& from = streams_[stream];
            if (!from.pending.empty()) {
                flush(from, stream, locate_of(stream));
            }
            if (from.values > 0) {
                ++streams;
            }
        }
        std::uint64_t per_stream = kStreamCountBytes;
        if (spec_->order != StreamOrder::file) {
            per_stream += kStreamLocateBytes;
        }
        if (spec_->order == StreamOrder::per_locate_side) {
            per_stream += kStreamSideBytes;
        }
        if (spec_->order == StreamOrder::per_type) {
            per_stream = kStreamCountBytes + kStreamTypeBytes;
        }
        for (CodecResult& result : results_) {
            result.metadata_bytes = streams * per_stream;
        }
        ColumnResult column{.spec = spec_,
                            .values = values_,
                            .streams = streams,
                            .ideal_delta_bits = delta_.ideal_code_bits(),
                            .ideal_delta_of_delta_bits = dod_.ideal_code_bits(),
                            .entropy_delta_bits = std::nullopt,
                            .entropy_delta_of_delta_bits = std::nullopt,
                            .distinct_deltas = 0,
                            .distinct_delta_of_deltas = 0,
                            .entropy_value_bits = std::nullopt,
                            .distinct_values = 0,
                            .codecs = std::move(results_),
                            .raw_bytes = raw_bytes_,
                            .raw_zstd_bytes = raw_zstd_bytes_,
                            .raw_zstd_mismatches = raw_zstd_mismatches_,
                            .raw_zstd_decompress_ns = raw_zstd_decompress_ns_,
                            .metadata_bytes = streams * per_stream};
        if (spec_->count_deltas) {
            column.entropy_delta_bits = deltas_.entropy_bits();
            column.distinct_deltas = deltas_.distinct();
        }
        if (spec_->count_dods) {
            column.entropy_delta_of_delta_bits = dods_.entropy_bits();
            column.distinct_delta_of_deltas = dods_.distinct();
        }
        if (spec_->count_values) {
            column.entropy_value_bits = values_seen_.entropy_bits();
            column.distinct_values = values_seen_.distinct();
        }
        return column;
    }

private:
    struct Stream {
        std::vector<std::uint64_t> pending;
        // Values already flushed, so an example can name its position in the stream.
        std::uint64_t values = 0;
    };

    // The locate a stream belongs to, for examples: 0 in a file-order or per-type column.
    [[nodiscard]] std::uint16_t locate_of(std::size_t stream) const noexcept {
        switch (spec_->order) {
            case StreamOrder::per_locate:
                return static_cast<std::uint16_t>(stream);
            case StreamOrder::per_locate_side:
                return static_cast<std::uint16_t>(stream / 2);
            case StreamOrder::file:
            case StreamOrder::per_type:
                break;
        }
        return 0;
    }

    void note(CodecResult& result, const Example& example) const {
        if (result.examples.size() < options_->max_examples) {
            result.examples.push_back(example);
        }
    }

    void flush(Stream& stream, std::size_t index, std::uint16_t locate) {
        const std::span<const std::uint64_t> block{stream.pending};
        if (options_->observe_block) {
            options_->observe_block(spec_->name, index, block);
        }
        for (CodecResult& result : results_) {
            check_block(result, block, stream.values, locate);
        }
        raw_zstd(block);
        stream.values += block.size();
        stream.pending.clear();
    }

    // The block's raw values at the column's wire width, little-endian, through zstd.
    void raw_zstd(std::span<const std::uint64_t> block) {
        raw_.clear();
        for (const std::uint64_t value : block) {
            for (unsigned i = 0; i < spec_->raw_bytes; ++i) {
                raw_.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
            }
        }
        raw_bytes_ += raw_.size();
        const auto frame = zstd_->round_trip(raw_, raw_zstd_decompress_ns_);
        if (!frame.has_value()) {
            ++raw_zstd_mismatches_;
            return;
        }
        raw_zstd_bytes_ += *frame;
    }

    void check_block(CodecResult& result, std::span<const std::uint64_t> block,
                     std::uint64_t first_index, std::uint16_t locate) {
        const CodecEntry& entry = *result.entry;
        words_.clear();
        for (const std::uint64_t value : block) {
            words_.push_back(to_stored_word(entry.representation, spec_->kind, value));
        }
        result.values += block.size();
        const auto refuse = [&] {
            ++result.blocks_refused;
            result.word_mismatches += block.size();
            note(result, Example{.index = first_index,
                                 .locate = locate,
                                 .value = block.front(),
                                 .stored_word = words_.front(),
                                 .decoded = std::nullopt});
        };
        encoded_.clear();
        if (!entry.encode_block(words_, encoded_, result.counters).has_value()) {
            refuse();
            return;
        }
        ++result.blocks;
        result.block_bytes += encoded_.size();
        result.header_bytes += codec::kBlockHeaderSize;

        decoded_.clear();
        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        const auto taken = entry.decode_block(encoded_, decoded_);
        result.decode_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        if (!taken.has_value() || *taken != encoded_.size() || decoded_.size() != block.size()) {
            refuse();
            return;
        }
        if (const auto frame = zstd_->round_trip(encoded_, result.zstd_decompress_ns);
            frame.has_value()) {
            result.zstd_bytes += *frame;
            result.zstd_or_plain_bytes += std::min(*frame, encoded_.size()) + 1U;
        } else {
            ++result.zstd_mismatches;
        }
        for (std::size_t i = 0; i < block.size(); ++i) {
            const Example example{.index = first_index + i,
                                  .locate = locate,
                                  .value = block[i],
                                  .stored_word = words_[i],
                                  .decoded = std::nullopt};
            if (decoded_[i] != words_[i]) {
                ++result.word_mismatches;
                note(result, example);
                continue;
            }
            const auto back = from_stored_word(entry.representation, spec_->kind, decoded_[i]);
            if (!back.has_value() || *back != block[i]) {
                ++result.representation_failures;
                Example failed = example;
                if (back.has_value()) {
                    failed.decoded = *back;
                }
                note(result, failed);
            }
        }
    }

    const ColumnSpec* spec_;
    const EvalOptions* options_;
    Zstd* zstd_;
    std::vector<Stream> streams_;
    std::vector<StreamShape> shapes_;
    Log2Histogram delta_;
    Log2Histogram dod_;
    ValueCounter deltas_;
    ValueCounter dods_;
    ValueCounter values_seen_;
    std::uint64_t values_ = 0;
    std::vector<CodecResult> results_;
    std::vector<std::uint64_t> words_;
    std::vector<std::byte> encoded_;
    std::vector<std::uint64_t> decoded_;
    std::vector<std::byte> raw_;
    std::uint64_t raw_bytes_ = 0;
    std::uint64_t raw_zstd_bytes_ = 0;
    std::uint64_t raw_zstd_mismatches_ = 0;
    std::uint64_t raw_zstd_decompress_ns_ = 0;
};

// The type byte of each alternative of itch::Message, by index; '?' for a raw record.
constexpr std::string_view kTypeCodes = "SRHYLVWKJhAFECXDUPQBINO?";
static_assert(kTypeCodes.size() == kTypeSlots);

[[nodiscard]] std::uint64_t mix64(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] std::uint64_t zigzag(std::uint64_t difference) noexcept {
    return (difference << 1U) ^ (std::uint64_t{0} - (difference >> 63U));
}

// An order on the book: what created it, where and when, and the shares it has left.
struct LiveOrder {
    std::uint64_t ref = 0;
    // Its creation's position among all creations (A, F and U's new reference), from 0,
    // and among its locate's creations.
    std::uint64_t ordinal = 0;
    std::uint64_t timestamp = 0;
    std::uint32_t locate_ordinal = 0;
    std::uint32_t shares = 0;
    std::uint16_t locate = 0;
    bool used = false;
};

// Live orders by reference, in an open-addressing table with backward-shift deletion,
// at most half full.
class OrderBook {
public:
    OrderBook() : slots_(kFirstSlots) {}

    [[nodiscard]] LiveOrder* find(std::uint64_t ref) noexcept {
        std::size_t at = home(ref);
        while (slots_[at].used) {
            if (slots_[at].ref == ref) {
                return &slots_[at];
            }
            at = (at + 1) & mask();
        }
        return nullptr;
    }

    // Returns false if the reference was already live; the new order replaces it.
    bool insert(LiveOrder order) {
        if ((live_ + 1) * 2 > slots_.size()) {
            grow();
        }
        order.used = true;
        std::size_t at = home(order.ref);
        while (slots_[at].used) {
            if (slots_[at].ref == order.ref) {
                slots_[at] = order;
                return false;
            }
            at = (at + 1) & mask();
        }
        slots_[at] = order;
        ++live_;
        peak_ = std::max(peak_, live_);
        return true;
    }

    void erase(LiveOrder* order) noexcept {
        auto hole = static_cast<std::size_t>(order - slots_.data());
        std::size_t at = hole;
        while (true) {
            at = (at + 1) & mask();
            if (!slots_[at].used) {
                break;
            }
            // An entry moves back into the hole if the hole lies on its probe path.
            const std::size_t want = home(slots_[at].ref);
            if (((at - want) & mask()) >= ((at - hole) & mask())) {
                slots_[hole] = slots_[at];
                hole = at;
            }
        }
        slots_[hole].used = false;
        --live_;
    }

    [[nodiscard]] std::uint64_t live() const noexcept { return live_; }
    [[nodiscard]] std::uint64_t peak() const noexcept { return peak_; }

private:
    [[nodiscard]] std::size_t mask() const noexcept { return slots_.size() - 1; }
    [[nodiscard]] std::size_t home(std::uint64_t ref) const noexcept {
        return static_cast<std::size_t>(mix64(ref)) & mask();
    }

    void grow() {
        const std::vector<LiveOrder> old = std::move(slots_);
        slots_.assign(old.size() * 2, LiveOrder{});
        for (const LiveOrder& order : old) {
            if (order.used) {
                std::size_t at = home(order.ref);
                while (slots_[at].used) {
                    at = (at + 1) & mask();
                }
                slots_[at] = order;
            }
        }
    }

    static constexpr std::size_t kFirstSlots = std::size_t{1} << 16U;
    std::vector<LiveOrder> slots_;
    std::uint64_t live_ = 0;
    std::uint64_t peak_ = 0;
};

// Each symbol's recency rank: how many distinct symbols were used since its last use, the
// rank move-to-front over an unbounded list gives it. A Fenwick tree over use times marks
// each symbol's last use; times are renumbered when they run out, so the tree stays small.
class RecencyRank {
public:
    explicit RecencyRank(std::size_t symbols) : last_(symbols, 0), tree_(kTimes + 1, 0) {}

    // nullopt on a symbol's first use.
    std::optional<std::uint64_t> use(std::size_t symbol) {
        if (now_ == kTimes) {
            renumber();
        }
        const std::uint32_t last = last_[symbol];
        ++now_;
        std::optional<std::uint64_t> rank;
        if (last != 0) {
            rank = static_cast<std::uint64_t>(prefix(now_ - 1) - prefix(last));
            update(last, -1);
        }
        update(now_, 1);
        last_[symbol] = now_;
        return rank;
    }

private:
    // Enough for every locate twice over, and small enough to stay in cache.
    static constexpr std::uint32_t kTimes = std::uint32_t{1} << 17U;

    void update(std::uint32_t at, std::int32_t delta) noexcept {
        for (; at <= kTimes; at += at & (~at + 1U)) {
            tree_[at] += delta;
        }
    }

    [[nodiscard]] std::int64_t prefix(std::uint32_t at) const noexcept {
        std::int64_t sum = 0;
        for (; at > 0; at -= at & (~at + 1U)) {
            sum += tree_[at];
        }
        return sum;
    }

    void renumber() {
        std::vector<std::pair<std::uint32_t, std::size_t>> used;
        for (std::size_t symbol = 0; symbol < last_.size(); ++symbol) {
            if (last_[symbol] != 0) {
                used.emplace_back(last_[symbol], symbol);
            }
        }
        std::ranges::sort(used);
        std::ranges::fill(tree_, 0);
        now_ = 0;
        for (const auto& [time, symbol] : used) {
            ++now_;
            last_[symbol] = now_;
            update(now_, 1);
        }
    }

    std::vector<std::uint32_t> last_;
    std::vector<std::int32_t> tree_;
    std::uint32_t now_ = 0;
};

// Who last touched a head: the add's locate, side and MPID, if it had one.
struct HeadOwner {
    std::uint16_t locate = 0;
    bool sell = false;
    bool has_mpid = false;
    std::uint32_t mpid = 0;
};

// Follows one reference stream through head delta's heads, as the PFOR transform would
// over a single block holding the whole day, and records HeadModes.
class HeadTracker {
public:
    void observe(std::uint64_t ref, const HeadOwner& owner) {
        HeadModes& m = modes_;
        ++m.values;
        std::size_t token = 0;
        std::uint64_t gap = ref - heads_[0];
        for (std::size_t t = 1; t < codec::kHeadDeltaHeads; ++t) {
            if (ref - heads_[t] < gap) {
                gap = ref - heads_[t];
                token = t;
            }
        }
        if (gap < codec::kHeadDeltaPayloadLimit) {
            ++m.tokens[token];
            gaps_.add(gap);
            joint_.add((gap << codec::kHeadDeltaTokenBits) | token);
            ++m.head_steps;
            const HeadOwner& before = owners_[token];
            m.same_locate += before.locate == owner.locate ? 1U : 0U;
            m.same_side += before.sell == owner.sell ? 1U : 0U;
            if (before.has_mpid && owner.has_mpid) {
                ++m.mpid_steps;
                m.same_mpid += before.mpid == owner.mpid ? 1U : 0U;
            }
            to_front(token, ref, owner);
        } else {
            // An escape: the value raw, or with no head-delta form at all.
            ++m.tokens[codec::kHeadDeltaEscape];
            const std::uint64_t payload = ref < codec::kHeadDeltaPayloadLimit ? ref : 0;
            gaps_.add(payload);
            joint_.add((payload << codec::kHeadDeltaTokenBits) | codec::kHeadDeltaEscape);
            to_front(codec::kHeadDeltaHeads - 1U, ref, owner);
        }
        if (seen_) {
            ++m.consecutive;
            m.consecutive_same_locate += previous_.locate == owner.locate ? 1U : 0U;
            m.consecutive_same_side += previous_.sell == owner.sell ? 1U : 0U;
            if (previous_.has_mpid && owner.has_mpid) {
                ++m.consecutive_mpid;
                m.consecutive_same_mpid += previous_.mpid == owner.mpid ? 1U : 0U;
            }
            const auto delta = static_cast<std::int64_t>(ref - previous_ref_);
            constexpr std::int64_t kSmall = 16;
            if (delta < -kSmall) {
                ++m.deltas_below;
            } else if (delta > kSmall) {
                ++m.deltas_above;
            } else {
                ++m.small_deltas[static_cast<std::size_t>(delta + kSmall)];
            }
        }
        seen_ = true;
        previous_ = owner;
        previous_ref_ = ref;
    }

    [[nodiscard]] HeadModes finish() const {
        HeadModes m = modes_;
        m.token_entropy_bits = entropy_of_counts(m.tokens);
        m.gap_entropy_bits = gaps_.entropy_bits();
        m.token_and_gap_entropy_bits = joint_.entropy_bits();
        m.distinct_gaps = gaps_.distinct();
        return m;
    }

private:
    void to_front(std::size_t from, std::uint64_t ref, const HeadOwner& owner) {
        codec::detail::head_to_front(heads_, from, ref);
        for (std::size_t t = from; t > 0; --t) {
            owners_[t] = owners_[t - 1];
        }
        owners_[0] = owner;
    }

    std::array<std::uint64_t, codec::kHeadDeltaHeads> heads_{};
    std::array<HeadOwner, codec::kHeadDeltaHeads> owners_{};
    ValueCounter gaps_;
    ValueCounter joint_;
    HeadModes modes_;
    bool seen_ = false;
    HeadOwner previous_;
    std::uint64_t previous_ref_ = 0;
};

// The messages that name an order, in ReferenceLocality's order.
enum class Naming : std::uint8_t { executed, executed_with_price, cancel, del, replace };
constexpr std::size_t kNamings = 5;
constexpr std::array<char, kNamings> kNamingTypes{'E', 'C', 'X', 'D', 'U'};
constexpr std::array<std::size_t, kNamings> kDistanceColumns{
    kColumnRefDistanceE, kColumnRefDistanceC, kColumnRefDistanceX, kColumnRefDistanceD,
    kColumnRefDistanceU};
constexpr std::array<std::size_t, kNamings> kLocateDistanceColumns{
    kColumnRefLocateDistanceE, kColumnRefLocateDistanceC, kColumnRefLocateDistanceX,
    kColumnRefLocateDistanceD, kColumnRefLocateDistanceU};

// The common header every typed message starts with: type, locate, tracking, timestamp.
constexpr std::size_t kCommonHeaderBytes = 11;

template <class M>
concept HasStock = requires(const M& m) { m.stock; };

template <class M>
concept NamesAnOrder = std::same_as<M, itch::OrderExecuted> ||
                       std::same_as<M, itch::OrderExecutedWithPrice> ||
                       std::same_as<M, itch::OrderCancel> || std::same_as<M, itch::OrderDelete> ||
                       std::same_as<M, itch::OrderReplace>;

// The rare types whose remaining bytes go to rare_rest_bytes as they are. V's levels and
// B's match number have columns of their own.
template <class M>
concept RareRest =
    std::same_as<M, itch::SystemEvent> || std::same_as<M, itch::StockDirectory> ||
    std::same_as<M, itch::StockTradingAction> || std::same_as<M, itch::RegShoRestriction> ||
    std::same_as<M, itch::MarketParticipantPosition> || std::same_as<M, itch::MwcbStatus> ||
    std::same_as<M, itch::IpoQuotingPeriodUpdate> || std::same_as<M, itch::LuldAuctionCollar> ||
    std::same_as<M, itch::OperationalHalt> || std::same_as<M, itch::RetailInterest> ||
    std::same_as<M, itch::DirectListingPriceDiscovery>;

[[nodiscard]] std::uint64_t byte_value(std::byte value) noexcept {
    return std::to_integer<std::uint64_t>(value);
}

// Feeds each message's fields to the columns that take them, and keeps the book and the
// counts the analysis needs.
class Walk {
public:
    explicit Walk(const EvalOptions& options)
        : add_locates_(kLocates),
          add_locate_sides_(kLocates * 2),
          locates_(kLocates),
          locate_creations_(kLocates, 0),
          directory_(kLocates),
          directory_set_(kLocates, false),
          recency_(kLocates),
          unreferenced_recency_(kLocates),
          recency_counts_(kLocates + 1, 0),
          unreferenced_recency_counts_(kLocates + 1, 0) {
        runs_.reserve(kColumns.size());
        for (std::size_t i = 0; i < kColumns.size(); ++i) {
            const ColumnSpec& spec = kColumns[i];
            if (options.columns.empty() ||
                std::ranges::find(options.columns, spec.name) != options.columns.end()) {
                runs_.emplace_back(spec, options, zstd_);
                by_index_[i] = &runs_.back();
            }
        }
        for (std::size_t i = 0; i < kNamings; ++i) {
            locality_[i].type = kNamingTypes[i];
        }
    }

    void observe(const itch::Message& message, std::span<const std::byte> payload) {
        ++messages_;
        const std::size_t slot = message.index();
        std::visit([&]<class M>(const M& typed) { observe_typed(typed, slot, payload); },
                   message);
    }

    [[nodiscard]] EvalResult finish(std::string stop) {
        EvalResult result;
        result.messages = messages_;
        result.raw_skipped = raw_;
        result.stop = std::move(stop);
        for (ColumnRun& run : runs_) {
            result.columns.push_back(run.finish());
        }
        result.permutation = PermutationEstimate{
            .add_messages = adds_,
            .add_locate_entropy_bits = entropy_of_counts(add_locates_),
            .add_locate_side_entropy_bits = entropy_of_counts(add_locate_sides_),
            .messages = messages_ - raw_,
            .locate_entropy_bits = entropy_of_counts(locates_),
            .largest_locate = largest_locate_};
        Analysis& analysis = result.analysis;
        analysis.add_heads = add_heads_.finish();
        analysis.creation_heads = creation_heads_.finish();
        analysis.references.assign(locality_.begin(), locality_.end());
        analysis.peak_live_orders = book_.peak();
        analysis.live_orders_at_end = book_.live();
        analysis.duplicate_creations = duplicate_creations_;
        analysis.type_entropy_bits = entropy_of_counts(type_counts_);
        double conditional = 0;
        std::uint64_t pairs = 0;
        for (const auto& row : type_pairs_) {
            std::uint64_t total = 0;
            for (const std::uint64_t count : row) {
                total += count;
            }
            conditional += static_cast<double>(total) * entropy_of_counts(row);
            pairs += total;
        }
        analysis.type_order1_entropy_bits =
            pairs == 0 ? 0.0 : conditional / static_cast<double>(pairs);
        analysis.locate_recency_entropy_bits = entropy_of_counts(recency_counts_);
        analysis.unreferenced_locate_recency_entropy_bits =
            entropy_of_counts(unreferenced_recency_counts_);
        analysis.locate_first_uses = recency_counts_[kLocates];
        analysis.stock_fields = stock_fields_;
        analysis.stock_mismatches = stock_mismatches_;
        analysis.stock_on_locate_zero = stock_on_locate_zero_;
        analysis.raw_unknown_type = raw_unknown_type_;
        analysis.raw_wrong_length = raw_wrong_length_;
        return result;
    }

private:
    void put(std::size_t column, std::uint16_t locate, std::uint64_t value, std::size_t key = 0) {
        if (ColumnRun* run = by_index_[column]; run != nullptr) {
            run->add(locate, value, key);
        }
    }

    template <class M>
    void observe_typed(const M& typed, std::size_t slot, std::span<const std::byte> payload) {
        if constexpr (std::same_as<M, itch::RawMessage>) {
            ++raw_;
            if (typed.reason == itch::RawReason::wrong_length) {
                ++raw_wrong_length_;
            } else {
                ++raw_unknown_type_;
            }
            if (!typed.bytes.empty()) {
                put(kColumnMessageType, 0, byte_value(typed.bytes.front()));
            }
        } else {
            const std::uint16_t locate = typed.header.stock_locate;
            const std::uint64_t timestamp = typed.header.timestamp_ns;
            ++locates_[locate];
            largest_locate_ = std::max(largest_locate_, locate);
            put(kColumnTimestamp, locate, timestamp);
            put(kColumnTimestampPerLocate, locate, timestamp);
            put(kColumnMessageType, locate, byte_value(M::type_code));
            put(kColumnStockLocate, locate, locate);
            put(kColumnTrackingNumber, locate, typed.header.tracking_number);
            put(kColumnTrackingNumberPerType, locate, typed.header.tracking_number, slot);
            observe_type_and_locate(slot, locate, !NamesAnOrder<M>);
            if constexpr (!NamesAnOrder<M>) {
                put(kColumnStockLocateUnreferenced, locate, locate);
            }
            if constexpr (HasStock<M>) {
                observe_stock(typed, slot, locate);
            }
            if constexpr (RareRest<M>) {
                for (std::size_t i = kCommonHeaderBytes; i < payload.size(); ++i) {
                    put(kColumnRareRestBytes, locate, byte_value(payload[i]));
                }
            }
            observe_fields(typed, slot, locate, timestamp);
        }
    }

    void observe_type_and_locate(std::size_t slot, std::uint16_t locate, bool unreferenced) {
        ++type_counts_[slot];
        if (previous_slot_.has_value()) {
            ++type_pairs_[*previous_slot_][slot];
        }
        previous_slot_ = slot;
        const auto rank = recency_.use(locate);
        ++recency_counts_[rank.value_or(kLocates)];
        if (unreferenced) {
            const auto alone = unreferenced_recency_.use(locate);
            ++unreferenced_recency_counts_[alone.value_or(kLocates)];
        }
    }

    template <class M>
    void observe_stock(const M& typed, std::size_t slot, std::uint16_t locate) {
        ++stock_fields_[slot];
        if (locate == 0) {
            ++stock_on_locate_zero_[slot];
        }
        if constexpr (std::same_as<M, itch::StockDirectory>) {
            directory_[locate] = typed.stock;
            directory_set_[locate] = true;
        } else if (!directory_set_[locate] || directory_[locate] != typed.stock) {
            ++stock_mismatches_[slot];
        }
    }

    // Creates an order, from an add or a replace's new reference.
    void create(std::uint64_t ref, std::uint16_t locate, std::uint64_t timestamp,
                std::uint32_t shares, const HeadOwner& owner) {
        put(kColumnCreationRef, locate, ref);
        creation_heads_.observe(ref, owner);
        const LiveOrder order{.ref = ref,
                              .ordinal = creations_,
                              .timestamp = timestamp,
                              .locate_ordinal = static_cast<std::uint32_t>(locate_creations_[locate]),
                              .shares = shares,
                              .locate = locate,
                              .used = true};
        if (!book_.insert(order)) {
            ++duplicate_creations_;
        }
        ++creations_;
        ++locate_creations_[locate];
    }

    // A message that names an order: its reference columns and locality. Returns the
    // order, or nullptr if it is not on the book.
    LiveOrder* name(Naming naming, std::uint64_t ref, std::size_t slot, std::uint16_t locate,
                    std::uint64_t timestamp) {
        const auto index = static_cast<std::size_t>(naming);
        ReferenceLocality& locality = locality_[index];
        ++locality.messages;
        put(kColumnReferencedRef, locate, ref);
        put(kColumnReferencedRefPerType, locate, ref, slot);
        LiveOrder* order = book_.find(ref);
        if (order == nullptr) {
            ++locality.unresolved;
            put(kDistanceColumns[index], locate, 0);
            put(kLocateDistanceColumns[index], locate, 0);
            return nullptr;
        }
        const std::uint64_t distance = creations_ - order->ordinal;
        const std::uint64_t locate_distance =
            locate_creations_[order->locate] - order->locate_ordinal;
        put(kDistanceColumns[index], locate, distance);
        put(kLocateDistanceColumns[index], locate, locate_distance);
        locality.locate_mismatches += order->locate != locate ? 1U : 0U;
        for (std::size_t i = 0; i < kLocalityBlocks.size(); ++i) {
            const std::uint64_t n = kLocalityBlocks[i];
            locality.same_block[i] += order->ordinal / n == (creations_ - 1) / n ? 1U : 0U;
            locality.within_distance[i] += distance <= n ? 1U : 0U;
        }
        for (std::size_t i = 0; i < kLocalityWindowsNs.size(); ++i) {
            const std::uint64_t w = kLocalityWindowsNs[i];
            locality.same_window[i] += order->timestamp / w == timestamp / w ? 1U : 0U;
            locality.within_time[i] +=
                timestamp >= order->timestamp && timestamp - order->timestamp <= w ? 1U : 0U;
        }
        return order;
    }

    // E, C and X: the shares against what the order has left, which they then reduce.
    void reduce(Naming naming, LiveOrder* order, std::uint32_t shares, std::size_t slot,
                std::uint16_t locate) {
        ReferenceLocality& locality = locality_[static_cast<std::size_t>(naming)];
        const bool all = order != nullptr && order->shares == shares;
        locality.shares_equal_remaining += all ? 1U : 0U;
        put(kColumnReductionSharesVsRemaining, locate, all ? 0 : std::uint64_t{shares} + 1U, slot);
        if (order != nullptr) {
            order->shares -= std::min(order->shares, shares);
            if (order->shares == 0) {
                book_.erase(order);
            }
        }
    }

    template <class M>
    void observe_fields(const M& typed, std::size_t slot, std::uint16_t locate,
                        std::uint64_t timestamp) {
        if constexpr (std::same_as<M, itch::AddOrder> || std::same_as<M, itch::AddOrderWithMpid>) {
            const bool sell = typed.buy_sell_indicator == std::byte{'S'};
            ++adds_;
            ++add_locates_[locate];
            ++add_locate_sides_[(std::size_t{locate} * 2) + (sell ? 1U : 0U)];
            put(kColumnAddPrice, locate, price(typed.price));
            put(kColumnAddPricePerLocate, locate, price(typed.price));
            put(kColumnAddPricePerLocateSide, locate, price(typed.price), sell ? 1U : 0U);
            const std::uint64_t ref = typed.order_reference_number;
            put(kColumnAddRef, locate, ref);
            put(kColumnAddRefPerLocate, locate, ref);
            put(kColumnAddRefVsRunningMax, locate, zigzag(ref - add_ref_max_));
            add_ref_max_ = std::max(add_ref_max_, ref);
            put(kColumnAddShares, locate, typed.shares);
            put(kColumnAddSharesPerLocate, locate, typed.shares);
            put(kColumnAddSide, locate, byte_value(typed.buy_sell_indicator));
            put(kColumnAddSidePerLocate, locate, byte_value(typed.buy_sell_indicator));
            constexpr bool kHasMpid = std::same_as<M, itch::AddOrderWithMpid>;
            std::uint32_t mpid = 0;
            if constexpr (kHasMpid) {
                for (const std::byte b : typed.attribution) {
                    mpid = (mpid << 8U) | std::to_integer<std::uint32_t>(b);
                }
                put(kColumnMpid, locate, mpid);
            }
            const HeadOwner owner{.locate = locate, .sell = sell, .has_mpid = kHasMpid, .mpid = mpid};
            add_heads_.observe(ref, owner);
            create(ref, locate, timestamp, typed.shares, owner);
        } else if constexpr (std::same_as<M, itch::OrderExecuted>) {
            LiveOrder* order =
                name(Naming::executed, typed.order_reference_number, slot, locate, timestamp);
            put(kColumnExecutedShares, locate, typed.executed_shares);
            put(kColumnMatchNumber, locate, typed.match_number);
            reduce(Naming::executed, order, typed.executed_shares, slot, locate);
        } else if constexpr (std::same_as<M, itch::OrderExecutedWithPrice>) {
            LiveOrder* order = name(Naming::executed_with_price, typed.order_reference_number,
                                    slot, locate, timestamp);
            put(kColumnExecutedPrice, locate, price(typed.execution_price));
            put(kColumnExecutedWithPriceShares, locate, typed.executed_shares);
            put(kColumnMatchNumber, locate, typed.match_number);
            put(kColumnPrintable, locate, byte_value(typed.printable));
            reduce(Naming::executed_with_price, order, typed.executed_shares, slot, locate);
        } else if constexpr (std::same_as<M, itch::OrderCancel>) {
            LiveOrder* order =
                name(Naming::cancel, typed.order_reference_number, slot, locate, timestamp);
            put(kColumnCancelledShares, locate, typed.cancelled_shares);
            reduce(Naming::cancel, order, typed.cancelled_shares, slot, locate);
        } else if constexpr (std::same_as<M, itch::OrderDelete>) {
            if (LiveOrder* order =
                    name(Naming::del, typed.order_reference_number, slot, locate, timestamp)) {
                book_.erase(order);
            }
        } else if constexpr (std::same_as<M, itch::OrderReplace>) {
            const std::uint64_t original = typed.original_order_reference_number;
            const std::uint64_t replacement = typed.new_order_reference_number;
            const HeadOwner owner{.locate = locate, .sell = false, .has_mpid = false, .mpid = 0};
            if (LiveOrder* order = name(Naming::replace, original, slot, locate, timestamp)) {
                book_.erase(order);
            }
            put(kColumnReplacePrice, locate, price(typed.price));
            put(kColumnReplaceNewRef, locate, replacement);
            put(kColumnReplaceNewMinusOriginal, locate, zigzag(replacement - original));
            put(kColumnReplaceShares, locate, typed.shares);
            create(replacement, locate, timestamp, typed.shares, owner);
        } else if constexpr (std::same_as<M, itch::Trade>) {
            put(kColumnTradePrice, locate, price(typed.price));
            put(kColumnTradeShares, locate, typed.shares);
            put(kColumnMatchNumber, locate, typed.match_number);
            put(kColumnTradeRef, locate, typed.order_reference_number);
            put(kColumnTradeSide, locate, byte_value(typed.buy_sell_indicator));
        } else if constexpr (std::same_as<M, itch::CrossTrade>) {
            put(kColumnCrossShares, locate, typed.shares);
            put(kColumnCrossPrice, locate, price(typed.cross_price));
            put(kColumnMatchNumber, locate, typed.match_number);
            put(kColumnCrossType, locate, byte_value(typed.cross_type));
        } else if constexpr (std::same_as<M, itch::BrokenTrade>) {
            put(kColumnMatchNumber, locate, typed.match_number);
        } else if constexpr (std::same_as<M, itch::NetOrderImbalance>) {
            put(kColumnImbalanceShares, locate, typed.paired_shares);
            put(kColumnImbalanceShares, locate, typed.imbalance_shares);
            put(kColumnImbalancePrice, locate, price(typed.far_price));
            put(kColumnImbalancePrice, locate, price(typed.near_price));
            put(kColumnImbalancePrice, locate, price(typed.current_reference_price));
            put(kColumnImbalanceFlags, locate, byte_value(typed.imbalance_direction));
            put(kColumnImbalanceFlags, locate, byte_value(typed.cross_type));
            put(kColumnImbalanceFlags, locate, byte_value(typed.price_variation_indicator));
        } else if constexpr (std::same_as<M, itch::MwcbDeclineLevel>) {
            put(kColumnMwcbLevel, locate, static_cast<std::uint64_t>(typed.level_1));
            put(kColumnMwcbLevel, locate, static_cast<std::uint64_t>(typed.level_2));
            put(kColumnMwcbLevel, locate, static_cast<std::uint64_t>(typed.level_3));
        }
    }

    [[nodiscard]] static std::uint64_t price(itch::Price4 value) noexcept {
        return static_cast<std::uint32_t>(value);
    }

    Zstd zstd_;
    std::vector<ColumnRun> runs_;
    std::array<ColumnRun*, kColumns.size()> by_index_{};
    std::uint64_t messages_ = 0;
    std::uint64_t raw_ = 0;
    std::uint64_t adds_ = 0;
    std::vector<std::uint64_t> add_locates_;
    std::vector<std::uint64_t> add_locate_sides_;
    std::vector<std::uint64_t> locates_;
    std::uint16_t largest_locate_ = 0;

    OrderBook book_;
    std::uint64_t creations_ = 0;
    std::vector<std::uint64_t> locate_creations_;
    std::uint64_t duplicate_creations_ = 0;
    std::uint64_t add_ref_max_ = 0;
    std::array<ReferenceLocality, kNamings> locality_{};
    HeadTracker add_heads_;
    HeadTracker creation_heads_;

    std::array<std::uint64_t, kTypeSlots> type_counts_{};
    std::array<std::array<std::uint64_t, kTypeSlots>, kTypeSlots> type_pairs_{};
    std::optional<std::size_t> previous_slot_;
    std::vector<itch::Stock> directory_;
    std::vector<bool> directory_set_;
    RecencyRank recency_;
    RecencyRank unreferenced_recency_;
    std::vector<std::uint64_t> recency_counts_;
    std::vector<std::uint64_t> unreferenced_recency_counts_;
    std::array<std::uint64_t, kTypeSlots> stock_fields_{};
    std::array<std::uint64_t, kTypeSlots> stock_mismatches_{};
    std::array<std::uint64_t, kTypeSlots> stock_on_locate_zero_{};
    std::uint64_t raw_unknown_type_ = 0;
    std::uint64_t raw_wrong_length_ = 0;
};

[[nodiscard]] std::string_view name_of(ValueKind kind) noexcept {
    switch (kind) {
        case ValueKind::timestamp_ns:
            return "timestamp_ns";
        case ValueKind::price4:
            return "price4";
        case ValueKind::price8:
            return "price8";
        case ValueKind::reference:
            return "reference";
        case ValueKind::quantity:
            return "quantity";
        case ValueKind::symbol:
            return "symbol";
    }
    return "unknown";
}

[[nodiscard]] std::string_view name_of(StreamOrder order) noexcept {
    switch (order) {
        case StreamOrder::file:
            return "file";
        case StreamOrder::per_locate:
            return "per_locate";
        case StreamOrder::per_locate_side:
            return "per_locate_side";
        case StreamOrder::per_type:
            return "per_type";
    }
    return "unknown";
}

[[nodiscard]] std::string_view name_of(Representation representation) noexcept {
    switch (representation) {
        case Representation::identity:
            return "identity";
        case Representation::dollars:
            return "dollars";
        case Representation::integer_as_double:
            return "integer_as_double";
    }
    return "unknown";
}

[[nodiscard]] double bits_per_value(const CodecResult& result) noexcept {
    return result.values == 0
               ? 0.0
               : static_cast<double>(result.stored_bytes()) * 8.0 /
                     static_cast<double>(result.values);
}

// BENCHMARKS.md's wording, unchanged.
struct Target {
    std::string_view id;
    std::string_view what;
    double limit;
};

constexpr std::array kTargets{
    Target{.id = "CR-3", .what = "Timestamp column at most 20.0 bits per value", .limit = 20.0},
    Target{.id = "CR-4",
           .what = "Add-order price column at most 7.9 bits per value",
           .limit = 7.9},
    Target{.id = "CR-6",
           .what = "Order reference columns at most 11.2 bits per value",
           .limit = 11.2},
    Target{.id = "CR-7", .what = "Shares columns at most 3.8 bits per value", .limit = 3.8},
};

[[nodiscard]] double bits_of(std::uint64_t bytes, std::uint64_t values) noexcept {
    return values == 0 ? 0.0 : static_cast<double>(bytes) * 8.0 / static_cast<double>(values);
}

[[nodiscard]] double ratio_or_zero(std::uint64_t over, std::uint64_t under) noexcept {
    return under == 0 ? 0.0 : static_cast<double>(over) / static_cast<double>(under);
}

constexpr double kCr5Ratio = 1.53;

[[nodiscard]] const Target* target_of(std::string_view id) noexcept {
    const auto* found = std::ranges::find(kTargets, id, &Target::id);
    return found == kTargets.end() ? nullptr : found;
}

[[nodiscard]] const CodecResult* result_of(const ColumnResult& column, std::string_view codec) {
    const auto found = std::ranges::find_if(
        column.codecs, [&](const CodecResult& result) { return result.entry->name == codec; });
    return found == column.codecs.end() ? nullptr : &*found;
}

void write_example(JsonWriter& json, const Example& example) {
    json.begin_object();
    json.field("index", example.index);
    json.field("locate", example.locate);
    json.field("value", example.value);
    json.field("stored_word", std::format("{:#018x}", example.stored_word));
    json.field("decoded", example.decoded);
    json.end_object();
}

void write_codec(JsonWriter& json, const CodecResult& result) {
    const CodecEntry& entry = *result.entry;
    json.begin_object();
    json.field("codec", entry.name);
    json.field("family", entry.family);
    json.field("what", entry.what);
    json.field("representation", name_of(entry.representation));
    json.field("values", result.values);
    json.field("blocks", result.blocks);
    json.field("block_bytes", result.block_bytes);
    json.field("header_bytes", result.header_bytes);
    json.field("metadata_bytes", result.metadata_bytes);
    json.field("stored_bytes", result.stored_bytes());
    json.field("bits_per_value", bits_per_value(result));
    json.field("exact", result.exact());
    json.field("blocks_refused", result.blocks_refused);
    json.field("word_mismatches", result.word_mismatches);
    json.field("representation_failures", result.representation_failures);
    json.field("zstd1_bytes", result.zstd_bytes);
    json.field("zstd1_bits_per_value", bits_of(result.zstd_bytes + result.metadata_bytes,
                                               result.values));
    json.field("zstd1_or_plain_bytes", result.zstd_or_plain_bytes);
    json.field("zstd1_or_plain_bits_per_value",
               bits_of(result.zstd_or_plain_bytes + result.metadata_bytes, result.values));
    json.field("zstd1_mismatches", result.zstd_mismatches);
    json.field("zstd1_decompress_over_decode_time",
               ratio_or_zero(result.zstd_decompress_ns, result.decode_ns));
    json.key("examples");
    json.begin_array();
    for (const Example& example : result.examples) {
        write_example(json, example);
    }
    json.end_array();
    json.key("counters");
    json.begin_object();
    for (std::size_t i = 0; i < kCodecCounters; ++i) {
        if (!entry.counter_names[i].empty()) {
            json.field(entry.counter_names[i], result.counters[i]);
        }
    }
    json.end_object();
    json.end_object();
}

// The smallest exact codec on a column whose family `wanted` accepts.
template <class Wanted>
[[nodiscard]] const CodecResult* best_of(const ColumnResult& column, Wanted wanted) {
    const CodecResult* best = nullptr;
    for (const CodecResult& result : column.codecs) {
        if (result.values > 0 && result.exact() && wanted(*result.entry) &&
            (best == nullptr || result.stored_bytes() < best->stored_bytes())) {
            best = &result;
        }
    }
    return best;
}

[[nodiscard]] double ratio_of(const CodecResult& over, const CodecResult& under) noexcept {
    return under.stored_bytes() == 0 ? 0.0
                                     : static_cast<double>(over.stored_bytes()) /
                                           static_cast<double>(under.stored_bytes());
}

// CR-5 on one column: the best integer scheme against a Gorilla codec on the same
// column, so in the same order.
void write_cr5_against(JsonWriter& json, std::string_view key, const CodecResult& integer,
                       const CodecResult& gorilla) {
    json.key(key);
    json.begin_object();
    json.field("gorilla_codec", gorilla.entry->name);
    json.field("gorilla_bits_per_value", bits_per_value(gorilla));
    json.field("ratio", ratio_of(gorilla, integer));
    json.field("ratio_at_least", kCr5Ratio);
    json.field("met", ratio_of(gorilla, integer) >= kCr5Ratio);
    json.end_object();
}

// The best codec on a column against its target; for prices how XOR on the dollar
// doubles compares with XOR on the integers, and CR-5.
void write_comparison(JsonWriter& json, const ColumnResult& column) {
    json.key("comparison");
    json.begin_object();
    const CodecResult* best = best_of(column, [](const CodecEntry&) { return true; });
    if (best != nullptr) {
        json.field("best_exact_codec", best->entry->name);
        json.field("best_bits_per_value", bits_per_value(*best));
    }
    if (const Target* target = target_of(column.spec->target);
        target != nullptr && best != nullptr) {
        json.field("target", target->id);
        json.field("target_what", target->what);
        json.field("target_bits_per_value_at_most", target->limit);
        json.field("met", bits_per_value(*best) <= target->limit);
        json.field("gap_bits_per_value", bits_per_value(*best) - target->limit);
    }
    if (column.spec->kind == ValueKind::timestamp_ns) {
        json.field("census_ideal_delta_bits", column.ideal_delta_bits);
        json.field("census_ideal_delta_of_delta_bits", column.ideal_delta_of_delta_bits);
    }
    const CodecResult* dollars = result_of(column, "gorilla-xor-f64-dollars");
    const CodecResult* integer = result_of(column, "gorilla-xor-u64-integer");
    const CodecResult* as_double = result_of(column, "gorilla-xor-f64-integer");
    if (dollars != nullptr && integer != nullptr && as_double != nullptr &&
        integer->stored_bytes() > 0 && as_double->stored_bytes() > 0) {
        json.field("xor_dollars_over_xor_u64_integer", ratio_of(*dollars, *integer));
        json.field("xor_dollars_over_xor_f64_integer", ratio_of(*dollars, *as_double));
        json.field("cr5_bits_per_value_an_integer_scheme_must_reach",
                   bits_per_value(*dollars) / kCr5Ratio);
    }
    const CodecResult* best_integer =
        best_of(column, [](const CodecEntry& entry) { return entry.family != kGorilla; });
    const CodecResult* best_gorilla =
        best_of(column, [](const CodecEntry& entry) { return entry.family == kGorilla; });
    if (column.spec->kind != ValueKind::timestamp_ns && best_integer != nullptr &&
        best_gorilla != nullptr && dollars != nullptr && dollars->exact()) {
        json.key("CR-5");
        json.begin_object();
        json.field("best_integer_codec", best_integer->entry->name);
        json.field("best_integer_bits_per_value", bits_per_value(*best_integer));
        write_cr5_against(json, "against_gorilla_xor_f64_dollars", *best_integer, *dollars);
        write_cr5_against(json, "against_best_gorilla", *best_integer, *best_gorilla);
        json.end_object();
    }
    json.end_object();
}

void write_column(JsonWriter& json, const ColumnResult& column) {
    json.begin_object();
    json.field("name", column.spec->name);
    json.field("source", column.spec->source);
    json.field("kind", name_of(column.spec->kind));
    json.field("order", name_of(column.spec->order));
    json.field("values", column.values);
    json.field("streams", column.streams);
    json.field("ideal_delta_bits", column.ideal_delta_bits);
    json.field("ideal_delta_of_delta_bits", column.ideal_delta_of_delta_bits);
    json.field("entropy_delta_bits", column.entropy_delta_bits);
    json.field("distinct_deltas", column.distinct_deltas);
    json.field("entropy_delta_of_delta_bits", column.entropy_delta_of_delta_bits);
    json.field("distinct_delta_of_deltas", column.distinct_delta_of_deltas);
    json.field("entropy_value_bits", column.entropy_value_bits);
    json.field("distinct_values", column.distinct_values);
    json.field("raw_bytes_per_value", column.spec->raw_bytes);
    json.field("raw_bytes", column.raw_bytes);
    json.field("raw_zstd1_bytes", column.raw_zstd_bytes);
    json.field("raw_zstd1_bits_per_value",
               bits_of(column.raw_zstd_bytes + column.metadata_bytes, column.values));
    json.field("raw_zstd1_mismatches", column.raw_zstd_mismatches);
    if (const CodecResult* best = best_of(column, [](const CodecEntry&) { return true; });
        best != nullptr) {
        json.field("raw_zstd1_decompress_over_best_codec_decode_time",
                   ratio_or_zero(column.raw_zstd_decompress_ns, best->decode_ns));
    }
    json.key("codecs");
    json.begin_array();
    for (const CodecResult& result : column.codecs) {
        write_codec(json, result);
    }
    json.end_array();
    write_comparison(json, column);
    json.end_object();
}

template <class Range>
void write_numbers(JsonWriter& json, std::string_view key, const Range& numbers) {
    json.key(key);
    json.begin_array();
    for (const auto number : numbers) {
        json.value(number);
    }
    json.end_array();
}

[[nodiscard]] double share_of(std::uint64_t part, std::uint64_t whole) noexcept {
    return whole == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(whole);
}

void write_heads(JsonWriter& json, std::string_view key, const HeadModes& m) {
    json.key(key);
    json.begin_object();
    json.field("values", m.values);
    write_numbers(json, "tokens_0_to_6_then_escape", m.tokens);
    json.field("token_entropy_bits", m.token_entropy_bits);
    json.field("gap_entropy_bits", m.gap_entropy_bits);
    json.field("token_and_gap_entropy_bits", m.token_and_gap_entropy_bits);
    json.field("distinct_gaps", m.distinct_gaps);
    json.field("head_steps", m.head_steps);
    json.field("head_same_locate_share", share_of(m.same_locate, m.head_steps));
    json.field("head_same_side_share", share_of(m.same_side, m.head_steps));
    json.field("head_mpid_steps", m.mpid_steps);
    json.field("head_same_mpid_share", share_of(m.same_mpid, m.mpid_steps));
    json.field("consecutive_pairs", m.consecutive);
    json.field("consecutive_same_locate_share", share_of(m.consecutive_same_locate, m.consecutive));
    json.field("consecutive_same_side_share", share_of(m.consecutive_same_side, m.consecutive));
    json.field("consecutive_mpid_pairs", m.consecutive_mpid);
    json.field("consecutive_same_mpid_share",
               share_of(m.consecutive_same_mpid, m.consecutive_mpid));
    write_numbers(json, "plain_deltas_minus_16_to_16", m.small_deltas);
    json.field("plain_deltas_below_minus_16", m.deltas_below);
    json.field("plain_deltas_above_16", m.deltas_above);
    json.end_object();
}

void write_locality(JsonWriter& json, const ReferenceLocality& r) {
    json.begin_object();
    json.field("type", std::string_view{&r.type, 1});
    json.field("messages", r.messages);
    json.field("unresolved", r.unresolved);
    json.field("locate_mismatches", r.locate_mismatches);
    const std::uint64_t resolved = r.messages - r.unresolved;
    for (std::size_t i = 0; i < kLocalityBlocks.size(); ++i) {
        json.field(std::format("same_block_of_{}_creations_share", kLocalityBlocks[i]),
                   share_of(r.same_block[i], resolved));
        json.field(std::format("within_{}_creations_share", kLocalityBlocks[i]),
                   share_of(r.within_distance[i], resolved));
    }
    for (std::size_t i = 0; i < kLocalityWindowsNs.size(); ++i) {
        json.field(std::format("same_aligned_{}_ns_window_share", kLocalityWindowsNs[i]),
                   share_of(r.same_window[i], resolved));
        json.field(std::format("within_{}_ns_share", kLocalityWindowsNs[i]),
                   share_of(r.within_time[i], resolved));
    }
    if (r.type == 'E' || r.type == 'C' || r.type == 'X') {
        json.field("shares_equal_remaining", r.shares_equal_remaining);
        json.field("shares_equal_remaining_share", share_of(r.shares_equal_remaining, r.messages));
    }
    json.end_object();
}

void write_analysis(JsonWriter& json, const Analysis& a) {
    json.key("analysis");
    json.begin_object();
    json.field("what", std::string_view{
                           "What the columns cannot show alone. Head modes follow a reference "
                           "stream through head delta's seven heads over the whole day, as one "
                           "block would. Locality measures each message that names an order "
                           "against the creation of that order on a book kept by reference; "
                           "an order leaves the book on D, on U's original, or when its shares "
                           "run out. Recency ranks count distinct locates since the last use."});
    write_heads(json, "add_ref_heads", a.add_heads);
    write_heads(json, "creation_ref_heads", a.creation_heads);
    json.key("reference_locality");
    json.begin_array();
    for (const ReferenceLocality& r : a.references) {
        write_locality(json, r);
    }
    json.end_array();
    json.field("peak_live_orders", a.peak_live_orders);
    json.field("live_orders_at_end", a.live_orders_at_end);
    json.field("duplicate_creations", a.duplicate_creations);
    json.field("type_entropy_bits", a.type_entropy_bits);
    json.field("type_order1_entropy_bits", a.type_order1_entropy_bits);
    json.field("locate_recency_entropy_bits", a.locate_recency_entropy_bits);
    json.field("unreferenced_locate_recency_entropy_bits",
               a.unreferenced_locate_recency_entropy_bits);
    json.field("locate_first_uses", a.locate_first_uses);
    json.key("stock_fields");
    json.begin_object();
    for (std::size_t slot = 0; slot < a.stock_fields.size(); ++slot) {
        if (a.stock_fields[slot] == 0) {
            continue;
        }
        json.key(std::string_view{&kTypeCodes[slot], 1});
        json.begin_object();
        json.field("messages", a.stock_fields[slot]);
        json.field("differ_from_directory", a.stock_mismatches[slot]);
        json.field("on_locate_zero", a.stock_on_locate_zero[slot]);
        json.end_object();
    }
    json.end_object();
    json.field("raw_unknown_type", a.raw_unknown_type);
    json.field("raw_wrong_length", a.raw_wrong_length);
    json.end_object();
}

[[nodiscard]] std::string host_label() {
    utsname name{};
    if (uname(&name) != 0) {
        return "unknown";
    }
    const std::string_view system{static_cast<const char*>(name.sysname)};
    return system == "Darwin" ? "macOS" : std::string{system};
}

struct CliOptions {
    std::string_view source;
    std::string_view output;
    std::string_view day;
    std::uint32_t block_values = codec::kDefaultBlockValues;
    bool prove_all_price4 = false;
    std::vector<std::string> columns;
};

constexpr std::string_view kUsage =
    "usage: tick_codec_eval <source> <output.json> --day LABEL [--block-values N] "
    "[--prove-all-price4] [--columns NAME,NAME,...]\n";

[[nodiscard]] std::optional<CliOptions> parse_options(std::span<const std::string_view> args) {
    if (args.size() < 2) {
        return std::nullopt;
    }
    CliOptions options;
    options.source = args[0];
    options.output = args[1];
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (flag == "--prove-all-price4") {
            options.prove_all_price4 = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            return std::nullopt;
        }
        const std::string_view value = args[++i];
        if (flag == "--day") {
            options.day = value;
        } else if (flag == "--columns") {
            for (std::size_t at = 0; at <= value.size();) {
                const std::size_t comma = std::min(value.find(',', at), value.size());
                if (comma == at) {
                    return std::nullopt;
                }
                options.columns.emplace_back(value.substr(at, comma - at));
                at = comma + 1;
            }
        } else if (flag == "--block-values") {
            std::uint32_t parsed = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() ||
                !codec::check_block_values(parsed).has_value()) {
                return std::nullopt;
            }
            options.block_values = parsed;
        } else {
            return std::nullopt;
        }
    }
    if (options.day.empty()) {
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return !file.fail();
}

}  // namespace

std::span<const CodecEntry> codec_registry() noexcept { return kRegistry; }

std::span<const ColumnSpec> column_specs() noexcept { return kColumns; }

std::uint64_t to_stored_word(Representation representation, ValueKind kind,
                             std::uint64_t value) noexcept {
    switch (representation) {
        case Representation::identity:
            return value;
        case Representation::dollars:
            return std::bit_cast<std::uint64_t>(codec::scaled_to_double(value, scale_of(kind)));
        case Representation::integer_as_double:
            return std::bit_cast<std::uint64_t>(codec::scaled_to_double(value, 1));
    }
    return value;
}

std::expected<std::uint64_t, codec::CodecError> from_stored_word(Representation representation,
                                                                 ValueKind kind,
                                                                 std::uint64_t word) noexcept {
    switch (representation) {
        case Representation::identity:
            return word;
        case Representation::dollars:
            return codec::double_to_scaled(std::bit_cast<double>(word), scale_of(kind),
                                           max_of(kind));
        case Representation::integer_as_double:
            return codec::double_to_scaled(std::bit_cast<double>(word), 1, max_of(kind));
    }
    return word;
}

bool ColumnResult::exact() const noexcept {
    return raw_zstd_mismatches == 0 && std::ranges::all_of(codecs, &CodecResult::exact);
}

bool EvalResult::exact() const noexcept {
    return std::ranges::all_of(columns, &ColumnResult::exact);
}

std::expected<EvalResult, std::string> evaluate_codecs(std::span<const std::byte> file,
                                                       const EvalOptions& options) {
    if (!codec::check_block_values(options.block_values).has_value()) {
        return std::unexpected(std::format("block size {} is outside 1 to {}",
                                           options.block_values, codec::kMaxBlockValues));
    }
    for (const std::string& name : options.columns) {
        if (std::ranges::find(kColumns, name, &ColumnSpec::name) == kColumns.end()) {
            return std::unexpected(std::format("no column is named {}", name));
        }
    }
    Walk walk{options};
    itch::FrameCursor cursor{file};
    while (true) {
        const auto frame = cursor.next();
        if (!frame.has_value()) {
            return walk.finish(itch::to_string(frame.error()));
        }
        // The cursor returns no empty payload, and only that fails to decode.
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            return std::unexpected(itch::to_string(message.error()));
        }
        walk.observe(*message, frame->payload);
    }
}

Price4Proof prove_price4(Representation representation) noexcept {
    Price4Proof proof;
    proof.representation = representation;
    for (std::uint64_t price = 0; price <= std::numeric_limits<std::uint32_t>::max(); ++price) {
        const std::uint64_t word = to_stored_word(representation, ValueKind::price4, price);
        const auto back = from_stored_word(representation, ValueKind::price4, word);
        ++proof.checked;
        if (!back.has_value() || *back != price) {
            if (!proof.first_failure.has_value()) {
                proof.first_failure = static_cast<std::uint32_t>(price);
            }
            ++proof.failures;
        }
    }
    return proof;
}

int run_codec_eval_cli(std::span<const std::string_view> arguments,
                       const EvalProvenance& provenance, CliOutput& output) {
    const auto options = parse_options(arguments);
    if (!options.has_value()) {
        output.err += kUsage;
        return kExitUsageOrIo;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto source = store::MappedFile::open(options->source);
    if (!source.has_value()) {
        output.err += std::format("cannot read the source: {}\n", to_string(source.error()));
        return kExitUsageOrIo;
    }
    const auto result = evaluate_codecs(source->bytes(),
                                        EvalOptions{.block_values = options->block_values,
                                                    .max_examples = EvalOptions{}.max_examples,
                                                    .columns = options->columns,
                                                    .observe_block = {}});
    if (!result.has_value()) {
        output.err += std::format("the source did not decode: {}\n", result.error());
        return kExitUsageOrIo;
    }
    std::vector<Price4Proof> proofs;
    if (options->prove_all_price4) {
        proofs.push_back(prove_price4(Representation::dollars));
        proofs.push_back(prove_price4(Representation::integer_as_double));
    }
    const double seconds = std::chrono::duration<double>{Clock::now() - start}.count();

    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_codec_eval"});
    json.field("schema", 1);
    json.field("tool_version", kToolVersion);
    json.field("day", options->day);
    json.field("source", std::filesystem::path{options->source}.filename().string());
    json.field("host", host_label());
    json.field("commit",
               provenance.commit.substr(0, std::min<std::size_t>(12, provenance.commit.size())));
    json.field("tree_dirty", provenance.tree_dirty);
    json.field("compiler", provenance.compiler);
    json.field("build_type", provenance.build_type);
    json.field("sanitizers", provenance.sanitizers);
    json.field("block_values", options->block_values);
    json.key("columns_subset");
    if (options->columns.empty()) {
        json.null();
    } else {
        json.begin_array();
        for (const std::string& name : options->columns) {
            json.value(std::string_view{name});
        }
        json.end_array();
    }
    json.field("zstd_version", std::string_view{ZSTD_versionString()});
    json.field("zstd_level", kZstdLevel);
    json.field("messages", result->messages);
    json.field("raw_messages_skipped", result->raw_skipped);
    json.field("stop", result->stop);
    json.field("every_value_exact", result->exact());
    json.field("not_counted",
               std::string_view{"A per-locate column also needs a stored permutation or "
                                "ordinal to rebuild file order. That cost is not in its "
                                "stored bytes: see permutation_estimate."});
    const PermutationEstimate& permutation = result->permutation;
    json.key("permutation_estimate");
    json.begin_object();
    json.field("what", std::string_view{
                           "Per-locate streams rebuild file order from the locate of each message "
                           "in file order. A store that keeps a file-order locate column, which "
                           "re-serialising every message needs anyway, pays nothing more. One "
                           "that keeps locates only as its partitions must store that sequence: "
                           "at least its exact order-0 entropy below, in bits per message, with no "
                           "context, or the fixed width of the largest locate."});
    json.field("add_messages", permutation.add_messages);
    json.field("add_locate_entropy_bits", permutation.add_locate_entropy_bits);
    json.field("add_locate_side_entropy_bits", permutation.add_locate_side_entropy_bits);
    json.field("messages", permutation.messages);
    json.field("locate_entropy_bits", permutation.locate_entropy_bits);
    json.field("largest_locate", permutation.largest_locate);
    json.field("locate_fixed_width_bits",
               static_cast<unsigned>(std::bit_width(permutation.largest_locate)));
    json.end_object();
    json.key("price4_exhaustive_proofs");
    json.begin_array();
    for (const Price4Proof& proof : proofs) {
        json.begin_object();
        json.field("representation", name_of(proof.representation));
        json.field("checked", proof.checked);
        json.field("failures", proof.failures);
        json.field("first_failure", proof.first_failure);
        json.end_object();
    }
    json.end_array();
    json.key("targets");
    json.begin_object();
    for (const Target& target : kTargets) {
        json.key(target.id);
        json.begin_object();
        json.field("what", target.what);
        json.field("bits_per_value_at_most", target.limit);
        json.end_object();
    }
    json.key("CR-5");
    json.begin_object();
    json.field("what", std::string_view{"Best integer scheme at least 1.53x smaller than "
                                        "Gorilla XOR on the same price column"});
    json.field("ratio_at_least", kCr5Ratio);
    json.field("status", std::string_view{"judged per price column under comparison.CR-5, "
                                          "against Gorilla XOR on the dollar doubles and "
                                          "against the best Gorilla variant, same order on "
                                          "both sides"});
    json.end_object();
    json.end_object();
    json.key("columns");
    json.begin_array();
    for (const ColumnResult& column : result->columns) {
        write_column(json, column);
    }
    json.end_array();
    write_analysis(json, result->analysis);
    json.field("wall_seconds", seconds);
    json.end_object();
    if (!write_text(options->output, json.finish())) {
        output.err += "cannot write the results\n";
        return kExitUsageOrIo;
    }

    for (const ColumnResult& column : result->columns) {
        for (const CodecResult& codec_result : column.codecs) {
            output.out += std::format("{:<31} {:<27} {:>12} values {:>8.3f} bits/value {}\n",
                                      column.spec->name, codec_result.entry->name,
                                      codec_result.values, bits_per_value(codec_result),
                                      codec_result.exact() ? "exact" : "NOT EXACT");
        }
    }
    for (const Price4Proof& proof : proofs) {
        output.out += std::format("all 2^32 Price(4) through {}: {} failures\n",
                                  name_of(proof.representation), proof.failures);
    }
    const bool proofs_hold = std::ranges::all_of(
        proofs, [](const Price4Proof& proof) { return proof.failures == 0; });
    return result->exact() && proofs_hold ? kExitPass : kExitMismatch;
}

}  // namespace tick_store::harness
