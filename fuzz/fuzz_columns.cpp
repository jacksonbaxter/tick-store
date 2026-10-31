// libFuzzer target: raw bytes into the column directory's manifest parser and row parser.
// There is no checksum to get past (Z1). Run commands, limits and the reach check are in
// fuzz/README.md.

#include "columns_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> input = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::store::oracle::check_columns(input)) {
        std::println(stderr, "check_columns: {}", failure);
        std::abort();
    }
    return 0;
}
