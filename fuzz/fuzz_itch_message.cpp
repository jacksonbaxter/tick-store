// libFuzzer target: raw bytes straight into the decoders, as one payload. Run
// commands, limits and the reach check are in fuzz/README.md.

#include "itch_oracles.hpp"

#include "tick_store/itch/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // A 2-byte length prefix cannot declare more, so a longer input is not a
    // payload. -1 keeps it out of the corpus.
    if (size > tick_store::itch::kMaxPayloadSize) {
        return -1;
    }
    const std::span<const std::byte> payload = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::itch::oracle::check_message(payload)) {
        std::println(stderr, "check_message: {}", failure);
        std::abort();
    }
    return 0;
}
