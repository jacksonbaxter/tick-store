#pragma once

// What one execution of a bounded fuzz oracle decodes, encodes and compares, counted as the
// oracle works, so the unit suite can hold each target's count table (fuzz/README.md,
// "Bounds") to the constants on the named seeds. The fuzz targets never read them.
//
// Counted in values:
//   - decoded: every decode the oracle asks for, at what it can decode. A codec block at the
//     count its header states where its header and the payload that states are all there
//     (what an accepted block gives, and the most a refused one may decode before it is
//     refused), else none, since every block decoder refuses a cut header or payload before
//     decoding anything (block.cpp, read_block_header); a codec column at the values of the
//     blocks the column walk decodes before it stops (block.cpp, decode_column, which refuses
//     a block that would pass the count asked before decoding it); a store segment read at the
//     values its directory entry states (a column is decoded only to its stated count); a
//     query at the values the query reports it decoded (QueryStats::values_decoded).
//   - encoded: every value handed to an encoder: a codec block or column, or a store file
//     written again, at the values the written file states.
//   - compared: every value the kernel-against-scalar oracle decodes with the scalar set
//     forced and compares with the chosen set's answer, counted as decoded.
// And in parts, for the work a part costs whatever its values (fuzz/README.md, "Bounds"):
//   - blocks: every codec block part 1 examines, in any tier, accepted or refused;
//   - segments: every store segment the store oracle reads.

#include <cstdint>

namespace tick_store::oracle_counts {

struct Counts {
    std::uint64_t decoded = 0;
    std::uint64_t encoded = 0;
    std::uint64_t compared = 0;
    std::uint64_t blocks = 0;
    std::uint64_t segments = 0;

    friend bool operator==(const Counts&, const Counts&) = default;
};

// The counts on this thread since the last reset.
inline thread_local Counts counts{};

inline void reset() { counts = {}; }

}  // namespace tick_store::oracle_counts
