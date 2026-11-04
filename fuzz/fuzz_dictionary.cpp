// libFuzzer target: raw bytes straight into the dictionary block decoder, as a column of
// blocks, and through its encoder as values. A block has no checksum to get past (Z1). The
// format leaves the encoder choices (the mode and the nested PFOR blocks'), so the oracle
// in codec_oracles.hpp checks values, the encoder's own consistency and the entry count,
// not bytes; run commands, limits and the reach check are in fuzz/README.md.

#include "codec_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> input = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::codec::oracle::check_dict_column(input)) {
        std::println(stderr, "check_dict_column: {}", failure);
        std::abort();
    }
    return 0;
}
