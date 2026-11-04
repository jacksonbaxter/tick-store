// libFuzzer target: raw bytes straight into the container parser, as a whole file:
// header, trailer, footer, block and section entries. The bytes reach the structural
// checks below the CRCs, by Checksums::skip and by recomputing every CRC the input
// points at (Z1), and an accepted file must rebuild byte for byte through the writer's
// encoding (Z2). The oracle is in container_oracles.hpp; run commands, limits and the
// reach check are in fuzz/README.md.

#include "container_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> file = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::store::oracle::check_container(file)) {
        std::println(stderr, "check_container: {}", failure);
        std::abort();
    }
    return 0;
}
