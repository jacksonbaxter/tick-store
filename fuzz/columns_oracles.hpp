#pragma once

// The check fuzz_columns runs, kept apart from libFuzzer so that the unit suite replays
// crashers through the same code. It returns nullptr if the input passed, or a
// description of the broken property.
//
// An input is a whole column directory as sections, each a 4-byte little-endian size and
// that many bytes: the manifest, then every file in the order of kColumnFileNames. The
// sizes only cut the input up, so all the parser's checks still see raw bytes (Z1).

#include "tick_store/store/columns.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::store::oracle {

inline constexpr std::size_t kSectionCount = kColumnFileCount + 1;
using ColumnSections = std::array<std::span<const std::byte>, kSectionCount>;

[[nodiscard]] inline std::vector<std::byte> pack_columns(std::string_view manifest,
                                                         const ColumnBuffers& files) {
    std::vector<std::byte> out;
    const auto add = [&](std::span<const std::byte> section) {
        const std::size_t at = out.size();
        out.resize(at + 4);
        static_cast<void>(write_le<4>(out, at, section.size()));
        out.insert(out.end(), section.begin(), section.end());
    };
    add(std::as_bytes(std::span{manifest}));
    for (const std::vector<std::byte>& file : files) {
        add(file);
    }
    return out;
}

// Refuses an input that is not exactly the sections, since those never reach the parser.
[[nodiscard]] inline std::optional<ColumnSections> split_columns(std::span<const std::byte> input) {
    ColumnSections sections{};
    std::size_t at = 0;
    for (std::span<const std::byte>& section : sections) {
        const auto size = read_le<4>(input, at);
        if (!size.has_value() || *size > input.size() - at - 4) {
            return std::nullopt;
        }
        section = input.subspan(at + 4, static_cast<std::size_t>(*size));
        at += 4 + static_cast<std::size_t>(*size);
    }
    if (at != input.size()) {
        return std::nullopt;
    }
    return sections;
}

[[nodiscard]] inline ColumnBytes files_of(const ColumnSections& sections) {
    ColumnBytes files{};
    std::ranges::copy(std::span{sections}.subspan(1), files.begin());
    return files;
}

struct Rebuilt {
    std::vector<std::vector<std::byte>> frames;
    std::span<const std::byte> tail;
};

// Reads a directory to its end. nullopt means it was refused somewhere, with the refusal
// proven repeatable. A broken property sets `failure`.
[[nodiscard]] inline std::optional<Rebuilt> read_columns(const ColumnSections& sections,
                                                         const char*& failure) {
    const auto manifest = parse_columns_manifest(sections[0]);
    if (!manifest.has_value()) {
        const auto again = parse_columns_manifest(sections[0]);
        if (again.has_value() || again.error() != manifest.error()) {
            failure = "a manifest refusal was not repeatable";
        }
        return std::nullopt;
    }
    if (format_columns_manifest(*manifest) != std::string_view{
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): viewing bytes as chars.
            reinterpret_cast<const char*>(sections[0].data()), sections[0].size()}) {
        failure = "an accepted manifest does not format back to itself";
        return std::nullopt;
    }
    const ColumnBytes files = files_of(sections);
    auto parser = ColumnsParser::open(*manifest, files);
    if (!parser.has_value()) {
        const auto again = ColumnsParser::open(*manifest, files);
        if (again.has_value() || again.error() != parser.error()) {
            failure = "an open refusal was not repeatable";
        }
        return std::nullopt;
    }

    Rebuilt rebuilt;
    rebuilt.tail = parser->tail();
    while (true) {
        const auto frame = parser->next();
        if (!frame.has_value()) {
            const auto repeat = parser->next();
            if (repeat.has_value() || repeat.error() != frame.error()) {
                failure = "a row refusal did not repeat";
            }
            return std::nullopt;
        }
        if (!frame->has_value()) {
            break;
        }
        const std::span<const std::byte> bytes = **frame;
        const std::uint64_t declared =
            (std::to_integer<std::uint64_t>(bytes[0]) << 8U) | std::to_integer<std::uint64_t>(bytes[1]);
        if (bytes.size() < 3 || declared != bytes.size() - 2) {
            failure = "a frame's prefix does not state its size";
            return std::nullopt;
        }
        rebuilt.frames.emplace_back(bytes.begin(), bytes.end());
    }
    const auto end = parser->next();
    if (!end.has_value() || end->has_value()) {
        failure = "the end of the rows did not repeat";
        return std::nullopt;
    }
    if (rebuilt.frames.size() != manifest->records) {
        failure = "the row count differs from the manifest";
        return std::nullopt;
    }
    return rebuilt;
}

// Exports what was read, and reads that back: the same frames must come out. The first
// directory need not be canonical, since a row in raw form may hold a conforming message,
// so this compares frames and not column bytes.
[[nodiscard]] inline const char* check_reexport(const Rebuilt& rebuilt) {
    ColumnsEncoder encoder;
    for (const std::vector<std::byte>& frame : rebuilt.frames) {
        if (!encoder.append(std::span{frame}.subspan(2)).has_value()) {
            return "the encoder refused a frame the parser gave";
        }
    }
    auto buffers = std::move(encoder.buffers());
    buffers[kColumnFileCount - 1].assign(rebuilt.tail.begin(), rebuilt.tail.end());
    const auto packed = pack_columns(format_columns_manifest(encoder.manifest(rebuilt.tail.size())),
                                     buffers);
    const auto sections = split_columns(packed);
    if (!sections.has_value()) {
        return "the encoder's sections did not split";
    }
    const char* failure = nullptr;
    const auto again = read_columns(*sections, failure);
    if (failure != nullptr) {
        return failure;
    }
    if (!again.has_value()) {
        return "the parser refused the encoder's own output";
    }
    if (again->frames != rebuilt.frames) {
        return "re-exported rows gave different frames";
    }
    if (!std::ranges::equal(again->tail, rebuilt.tail)) {
        return "re-exported rows gave a different tail";
    }
    return nullptr;
}

// Runs the checks on the input and, if it was accepted, on a cut of one file, which must
// be refused (D10), and on a copy with one bit flipped, which must be refused or give
// frames that re-export cleanly. The input chooses the file, the cut and the bit.
[[nodiscard]] inline const char* check_columns(std::span<const std::byte> input) {
    const auto sections = split_columns(input);
    if (!sections.has_value()) {
        return nullptr;
    }
    const char* failure = nullptr;
    const auto rebuilt = read_columns(*sections, failure);
    if (failure != nullptr) {
        return failure;
    }
    if (!rebuilt.has_value()) {
        return nullptr;
    }
    if (const char* broken = check_reexport(*rebuilt)) {
        return broken;
    }

    const std::size_t choice = std::to_integer<std::size_t>(input[input.size() / 2]) * 257U;
    ColumnSections cut = *sections;
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        std::span<const std::byte>& section = cut[(choice + i) % kSectionCount];
        if (!section.empty()) {
            section = section.first(choice % section.size());
            const auto refused = read_columns(cut, failure);
            if (failure != nullptr) {
                return failure;
            }
            if (refused.has_value()) {
                return "a directory with a cut file was accepted";
            }
            break;
        }
    }

    std::vector<std::byte> flipped(input.begin(), input.end());
    flipped[choice % flipped.size()] ^= static_cast<std::byte>(1U << (choice % 8U));
    const auto flipped_sections = split_columns(flipped);
    if (!flipped_sections.has_value()) {
        return nullptr;
    }
    const auto after = read_columns(*flipped_sections, failure);
    if (failure != nullptr) {
        return failure;
    }
    return after.has_value() ? check_reexport(*after) : nullptr;
}

}  // namespace tick_store::store::oracle
