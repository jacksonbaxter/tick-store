// libFuzzer target: raw bytes straight into the stub store's parser, as a whole file.
// There is no checksum to get past (Z1). Run commands, limits and the reach check are
// in fuzz/README.md.

#include "store_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> file = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::store::oracle::check_store(file)) {
        std::println(stderr, "check_store: {}", failure);
        std::abort();
    }
    return 0;
}
