// libFuzzer target: raw bytes straight into the ITCH store's reader, as a whole file: the
// store and symbols sections, every block's metadata and the segment summaries, the
// directory, the column chunks and the zstd wrapper, and the rules tying a segment's
// columns together, including its import list. The bytes reach those checks below the
// CRCs, by Checksums::skip and by recomputing every CRC (Z1), and an accepted file must
// survive decode, re-encode, compare through the real writer (Z2). The oracle is in
// itch_store_oracles.hpp; run commands, limits and the reach check are in fuzz/README.md.

#include "itch_store_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> file = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::store::oracle::check_itch_store(file)) {
        std::println(stderr, "check_itch_store: {}", failure);
        std::abort();
    }
    return 0;
}
