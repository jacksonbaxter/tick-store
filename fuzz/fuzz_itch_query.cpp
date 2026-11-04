// libFuzzer target: raw bytes as a whole ITCH store file into the query engine
// (store/itch_query.hpp), below the CRCs: the seek index, the locate runs and their
// directory, the plain flag and every codec block the indexed path reads on its own. Every
// query must answer or refuse, the same way twice, and on a file the whole decode accepts
// the indexed path must give exactly what the exact path gives. The oracle is in
// itch_query_oracles.hpp; run commands and limits are in fuzz/README.md.

#include "itch_query_oracles.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> file = std::as_bytes(std::span{data, size});
    if (const char* failure = tick_store::store::oracle::check_itch_query(file)) {
        std::println(stderr, "check_itch_query: {}", failure);
        std::abort();
    }
    return 0;
}
