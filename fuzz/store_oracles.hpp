#pragma once

// The check fuzz_stub_store runs, kept apart from libFuzzer so that the unit suite
// replays crashers through the same code. It returns nullptr if the input passed, or a
// description of the broken property.

#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/store_error.hpp"
#include "tick_store/store/stub_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace tick_store::store::oracle {

// One file through the parser. What is refused must be refused consistently. What is
// accepted must rebuild the input exactly from the parsed values alone: the header
// from its fields, each record through the encoder, the tail from its span.
[[nodiscard]] inline const char* check_parse(std::span<const std::byte> file, bool& accepted) {
    accepted = false;
    auto parser = StoreParser::open(file);
    if (!parser.has_value()) {
        const auto again = StoreParser::open(file);
        return again.has_value() || again.error() != parser.error() ? "an open was not repeatable"
                                                                    : nullptr;
    }
    if (parser->tail().size() != parser->header().tail_bytes) {
        return "the tail does not have the size in the header";
    }

    // A record never comes out larger than it went in, so this bounds every write.
    std::vector<std::byte> rebuilt(file.size());
    const auto header = encode_stub_header(parser->header());
    std::ranges::copy(header, rebuilt.begin());
    std::size_t out = header.size();
    std::uint64_t records = 0;

    while (true) {
        const auto record = parser->next();
        if (!record.has_value()) {
            const auto repeat = parser->next();
            return repeat.has_value() || repeat.error() != record.error()
                       ? "a parse error did not repeat"
                       : nullptr;
        }
        if (!record->has_value()) {
            break;
        }
        const auto written = itch::encode_frame(**record, std::span{rebuilt}.subspan(out));
        if (!written.has_value()) {
            return "encode_frame failed on a parsed record";
        }
        out += *written;
        ++records;
    }

    const auto end = parser->next();
    if (!end.has_value() || end->has_value()) {
        return "the end of the records did not repeat";
    }
    // The rebuild takes the count from the header, so it could never disagree with itself.
    if (records != parser->header().record_count) {
        return "the record count differs from the records read";
    }
    if (out + parser->tail().size() != file.size()) {
        return "records and tail do not account for every byte";
    }
    std::ranges::copy(parser->tail(), std::span{rebuilt}.subspan(out).begin());
    if (!std::ranges::equal(rebuilt, file)) {
        return "the rebuilt file differs from the input";
    }
    accepted = true;
    return nullptr;
}

// Runs check_parse on the input and, if it was accepted, on a cut of it, which must be
// refused (D10), and on a copy with one bit flipped, which must be refused or rebuild
// to the flipped bytes. The input chooses the cut and the bit.
[[nodiscard]] inline const char* check_store(std::span<const std::byte> file) {
    bool accepted = false;
    if (const char* failure = check_parse(file, accepted)) {
        return failure;
    }
    if (!accepted) {
        return nullptr;
    }

    const std::size_t choice = std::to_integer<std::size_t>(file[file.size() / 2]) * 257U;
    bool cut_accepted = false;
    if (const char* failure = check_parse(file.first(choice % file.size()), cut_accepted)) {
        return failure;
    }
    if (cut_accepted) {
        return "a truncated file was accepted";
    }

    std::vector<std::byte> flipped(file.begin(), file.end());
    flipped[choice % flipped.size()] ^= static_cast<std::byte>(1U << (choice % 8U));
    bool flip_accepted = false;
    return check_parse(flipped, flip_accepted);
}

}  // namespace tick_store::store::oracle
