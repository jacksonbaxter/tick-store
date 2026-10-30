// libFuzzer target: raw bytes straight into the frame cursor, as a whole stream.
// Run commands, limits and the reach check are in fuzz/README.md.

#include "itch_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> stream = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::itch::oracle::check_stream(stream)) {
        std::println(stderr, "check_stream: {}", failure);
        std::abort();
    }
    return 0;
}
