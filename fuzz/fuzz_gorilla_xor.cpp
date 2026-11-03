// libFuzzer target: raw bytes straight into the Gorilla XOR block decoder, as a column of
// blocks, and through the encoder as values, then the price-to-double mappings on every
// word. A block has no checksum to get past (Z1). The oracle is in codec_oracles.hpp; run
// commands, limits and the reach check are in fuzz/README.md.

#include "codec_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> input = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::codec::oracle::check_xor_column(input)) {
        std::println(stderr, "check_xor_column: {}", failure);
        std::abort();
    }
    return 0;
}
