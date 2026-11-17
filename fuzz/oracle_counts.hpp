#pragma once

// What one execution of a bounded fuzz oracle decodes, encodes and compares, counted as the
// oracle works, so the unit suite can hold each target's count table (fuzz/README.md,
// "Bounds") to the constants on the named seeds. The fuzz targets never read them.
//
// Counted in values:
//   - decoded: every decode the oracle asks for, at the most it may give. A codec block at
//     the count its header states (what an accepted block gives, and the most a refused one
//     may decode before it is refused); a codec column at the count asked for (block.hpp:
//     a column costs at most the work of its count); a store segment read at the values its
//     directory entry states (a column is decoded only to its stated count); a query at the
//     values the query reports it decoded (QueryStats::values_decoded).
//   - encoded: every value handed to an encoder: a codec block or column, or a store file
//     written again, at the values the written file states.
//   - compared: every value the kernel-against-scalar oracle decodes with the scalar set
//     forced and compares with the chosen set's answer, counted as decoded.

#include <cstdint>

namespace tick_store::oracle_counts {

struct Counts {
    std::uint64_t decoded = 0;
    std::uint64_t encoded = 0;
    std::uint64_t compared = 0;

    friend bool operator==(const Counts&, const Counts&) = default;
};

// The counts on this thread since the last reset.
inline thread_local Counts counts{};

inline void reset() { counts = {}; }

}  // namespace tick_store::oracle_counts
