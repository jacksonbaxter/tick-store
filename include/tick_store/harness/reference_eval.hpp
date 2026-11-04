#pragma once

// What tools/tick_reference_eval measures: the bytes each way of storing the references of
// E, C, X, D and U's original order costs on a whole day, with every byte of the state a
// segment needs to decode on its own counted, at several segment sizes. Each figure is the
// real codecs' output, block by block, never an entropy.
//
// The encodings, each cut into segments of N records like the store:
//
//   within_stock_imports   the store's: a distance back in the locate's list of the
//                          segment's creations and imports, dictionary coded and grouped by
//                          type, with each segment's import list (pfor, delta or head delta).
//                          State: the import list, inside the segment.
//   all_creations_imports  the same over one list of all the segment's creations and
//                          imports, whatever their locate.
//   values_pfor_delta,     the references themselves, in file order: PFOR on deltas, PFOR head
//   values_head_delta,     delta, Stream VByte on deltas. No state.
//   values_svb_delta
//   day_distance_checkpoint  the distance back in the locate's creations over the whole day,
//                          as NOTES.md measured it, with a checkpoint at each segment's start:
//                          every order live on the book then, by locate, with its creation
//                          ordinal in the locate and its reference. Measured where asked for.
//   day_distance_index     the same distances with no checkpoint: each segment stores how many
//                          creations each locate had before it, so a reader can find the
//                          segment that created an order, at the price of decoding it.
//
// The book is kept by reference to know which orders are live: a creation adds an order with
// its shares, D and U's original remove it, E and C take shares off and remove it at zero, X
// takes shares off.

#include "tick_store/harness/codec_eval.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::harness {

struct ReferenceEvalOptions {
    std::vector<std::uint32_t> segment_records;
    // The sizes at which the book checkpoint is encoded too; each must be in segment_records.
    std::vector<std::uint32_t> checkpoint_segment_records;
};

struct EncodingCost {
    std::string name;
    // The reference column's bytes, and the state's, which together are what it costs.
    std::uint64_t column_bytes = 0;
    std::uint64_t state_bytes = 0;
    // State values: imports, live orders in checkpoints, or index entries.
    std::uint64_t state_values = 0;
    // Most state values one segment held.
    std::uint64_t max_segment_state_values = 0;
    bool measured = true;
};

struct SegmentSizeCost {
    std::uint32_t segment_records = 0;
    std::uint64_t segments = 0;
    // References the store resolves inside the segment by locate, and those it imports.
    std::uint64_t in_segment = 0;
    std::uint64_t imports = 0;
    std::vector<EncodingCost> encodings;
};

struct ReferenceEvalResult {
    std::uint64_t records = 0;
    std::uint64_t references = 0;
    // References no creation on the book under the same locate explains.
    std::uint64_t unresolved = 0;
    std::uint64_t peak_live_orders = 0;
    std::vector<SegmentSizeCost> sizes;
};

[[nodiscard]] std::expected<ReferenceEvalResult, std::string> evaluate_references(
    std::span<const std::byte> file, const ReferenceEvalOptions& options);

//   tick_reference_eval <source> <output.json> --day LABEL --segment-records N[,N...]
//                       [--checkpoint-segment-records N[,N...]]
//
// Writes the JSON, which holds no paths. Exit 0, or 2 on a usage or I/O error.
[[nodiscard]] int run_reference_eval_cli(std::span<const std::string_view> arguments,
                                         const EvalProvenance& provenance, CliOutput& output);

}  // namespace tick_store::harness
