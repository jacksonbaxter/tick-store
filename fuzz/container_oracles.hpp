#pragma once

// The check fuzz_container runs, kept apart from libFuzzer so the unit suite replays
// crashers through the same code. It returns nullptr if the input passed, or a
// description of the broken property.
//
// Raw bytes reach the parser below its CRCs two ways (Z1): with Checksums::skip, and
// with every CRC the input's own structure points at recomputed first
// (fixed_checksums). The structural checks must give the same verdict both ways, so a
// check that depends on a CRC by mistake shows up.
//
// The format has exactly one encoding of any set of blocks and sections, so the oracle
// is decode, re-encode, compare (Z2): an accepted file is rebuilt from its parsed
// values alone through build_container(), the writer's own encoding, and must come
// back byte for byte.

#include "tick_store/codec/registry.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/crc32c.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace tick_store::store::oracle {

namespace detail {

using Bytes = std::vector<std::byte>;

[[nodiscard]] inline std::uint64_t le(std::span<const std::byte> bytes, std::size_t offset,
                                      std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = width; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + i - 1]);
    }
    return value;
}

inline void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    std::ignore = write_le<4>(bytes, offset, value);
}

// True if [offset, offset + size) lies inside [0, limit), compared without wrapping.
[[nodiscard]] inline bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

}  // namespace detail

// A copy of `file` with each CRC its structure points at recomputed: every block and
// section CRC whose entry and bytes lie inside the file, then the footer CRC. Where the
// structure points nowhere, the bytes are left as they are.
[[nodiscard]] inline detail::Bytes fixed_checksums(std::span<const std::byte> file) {
    using detail::fits;
    using detail::le;
    detail::Bytes out(file.begin(), file.end());
    if (out.size() < kMinContainerSize) {
        return out;
    }
    const std::size_t trailer = out.size() - kTrailerSize;
    const std::uint64_t footer = le(out, trailer + kTrailerFooterOffset, 8);
    if (footer < kContainerHeaderSize || footer > trailer - kFooterFixedSize) {
        return out;
    }
    const auto footer_at = static_cast<std::size_t>(footer);
    const std::size_t footer_size = trailer - footer_at;
    const std::uint64_t blocks = le(out, footer_at + kFooterBlockCount, 8);
    const std::uint64_t sections = le(out, footer_at + kFooterSectionCount, 8);
    const std::size_t room = footer_size - kFooterFixedSize;

    // Entries are read only where they lie inside the footer, so a huge count costs
    // nothing beyond the footer's own size.
    const std::uint64_t block_entries = std::min<std::uint64_t>(blocks, room / kBlockEntrySize);
    for (std::uint64_t i = 0; i < block_entries; ++i) {
        const std::size_t entry =
            footer_at + kFooterFixedSize + static_cast<std::size_t>(i * kBlockEntrySize);
        const std::uint64_t offset = le(out, entry + kBlockPayloadOffset, 8);
        const std::uint64_t size = le(out, entry + kBlockPayloadSize, 4);
        if (fits(offset, size, footer)) {
            const auto codec = static_cast<codec::CodecId>(le(out, entry + kBlockCodec, 2));
            detail::put32(out, entry + kBlockCrc,
                          block_crc(codec, std::span<const std::byte>{out}.subspan(
                                               static_cast<std::size_t>(offset),
                                               static_cast<std::size_t>(size))));
        }
    }
    if (blocks <= room / kBlockEntrySize) {
        const std::size_t sections_at =
            footer_at + kFooterFixedSize + static_cast<std::size_t>(blocks * kBlockEntrySize);
        const std::size_t section_room = room - static_cast<std::size_t>(blocks * kBlockEntrySize);
        const std::uint64_t section_entries =
            std::min<std::uint64_t>(sections, section_room / kSectionEntrySize);
        for (std::uint64_t i = 0; i < section_entries; ++i) {
            const std::size_t entry = sections_at + static_cast<std::size_t>(i * kSectionEntrySize);
            const std::uint64_t offset = le(out, entry + kSectionOffset, 8);
            const std::uint64_t size = le(out, entry + kSectionSize, 8);
            if (fits(offset, size, footer)) {
                detail::put32(out, entry + kSectionCrc,
                              crc32c(std::span<const std::byte>{out}.subspan(
                                  static_cast<std::size_t>(offset), static_cast<std::size_t>(size))));
            }
        }
    }
    detail::put32(out, trailer + kTrailerFooterCrc,
                  crc32c(std::span<const std::byte>{out}.subspan(footer_at, footer_size)));
    return out;
}

namespace detail {

// The parts of an accepted file in file order, from the parser's values alone, taken
// in the order the parser's own walk takes them: an empty section at an offset first,
// then a block there, then a section there.
[[nodiscard]] inline const char* parts_in_file_order(const ContainerParser& parser,
                                                     std::span<const std::byte> file,
                                                     std::vector<ContainerPart>& parts) {
    // Where a view's bytes start in the file, from its pointer.
    const auto offset_of = [file](std::span<const std::byte> bytes) {
        return static_cast<std::uint64_t>(bytes.data() - file.data());
    };
    std::uint64_t block = 0;
    std::uint64_t section = 0;
    std::uint64_t cursor = kContainerHeaderSize;
    while (block < parser.block_count() || section < parser.section_count()) {
        const auto block_info = parser.block_info(block);
        const auto section_info = parser.section_info(section);
        const bool block_here = block_info.has_value() && block_info->payload_offset == cursor;
        const bool section_here = section_info.has_value() && section_info->offset == cursor;
        const bool empty_section_here = section_here && section_info->size == 0;
        if (block_here && !empty_section_here) {
            const auto view = parser.block(block);
            if (!view.has_value()) {
                return "an accepted block could not be read";
            }
            if (view->codec != block_info->codec ||
                view->payload.size() != block_info->payload_size ||
                (!view->payload.empty() && offset_of(view->payload) != block_info->payload_offset) ||
                view->metadata.data() != block_info->metadata.data()) {
                return "a block's view and its directory entry disagree";
            }
            parts.push_back({.section_name = "",
                             .codec = view->codec,
                             .metadata = Bytes(view->metadata.begin(), view->metadata.end()),
                             .bytes = Bytes(view->payload.begin(), view->payload.end())});
            cursor += view->payload.size();
            ++block;
        } else if (section_here) {
            const auto view = parser.section(section_info->name);
            if (!view.has_value()) {
                return "an accepted section could not be found by its name";
            }
            if (view->name != section_info->name || view->bytes.size() != section_info->size ||
                (!view->bytes.empty() && offset_of(view->bytes) != section_info->offset)) {
                return "a section found by name is not the entry that names it";
            }
            parts.push_back({.section_name = std::string{view->name},
                             .codec = codec::CodecId::raw,
                             .metadata = {},
                             .bytes = Bytes(view->bytes.begin(), view->bytes.end())});
            cursor += view->bytes.size();
            ++section;
        } else {
            return "an accepted file has extents the walk cannot follow";
        }
    }
    if (cursor != parser.footer_offset()) {
        return "an accepted file's extents do not reach its footer";
    }
    return nullptr;
}

}  // namespace detail

// One parse of `file`. What is refused must be refused the same way again. What is
// accepted, with every block and section readable under the same options, must rebuild
// to `expected` exactly: the input itself when CRCs are verified, or the input with its
// CRCs recomputed when they are skipped.
[[nodiscard]] inline const char* check_parse(std::span<const std::byte> file,
                                             std::span<const std::byte> expected,
                                             ReadOptions options, bool& accepted) {
    accepted = false;
    const auto parser = ContainerParser::open(file, options);
    if (!parser.has_value()) {
        const auto again = ContainerParser::open(file, options);
        return again.has_value() || again.error() != parser.error() ? "an open was not repeatable"
                                                                    : nullptr;
    }
    if (options.checksums == Checksums::verify) {
        const auto verified = parser->verify_all();
        if (!verified.has_value()) {
            const auto again = parser->verify_all();
            return again.has_value() || again.error() != verified.error()
                       ? "a CRC refusal was not repeatable"
                       : nullptr;
        }
    }
    if (parser->block_info(parser->block_count()).has_value() ||
        parser->section_info(parser->section_count()).has_value()) {
        return "an index one past the end was found";
    }

    std::vector<ContainerPart> parts;
    if (const char* failure = detail::parts_in_file_order(*parser, file, parts)) {
        return failure;
    }
    const auto rebuilt = build_container(parts);
    if (!rebuilt.has_value()) {
        return "the writer refused the parts of an accepted file";
    }
    if (!std::ranges::equal(*rebuilt, expected)) {
        return "the rebuilt file differs from the input";
    }
    accepted = true;
    return nullptr;
}

// Runs check_parse on the input as it is, with CRCs verified and skipped, and with its
// CRCs recomputed. Skipping CRCs and recomputing them must give the same verdict. If
// the structure is valid, a cut of the input and a copy with one bit flipped are each
// refused or valid in full, never accepted in part; the input chooses the cut and the
// bit. A cut can be valid: a file whose first section holds a whole container's bytes
// in the right place leaves a valid file when cut there.
[[nodiscard]] inline const char* check_container(std::span<const std::byte> file) {
    const detail::Bytes fixed = fixed_checksums(file);
    bool as_is = false;
    if (const char* failure = check_parse(file, file, {.checksums = Checksums::verify}, as_is)) {
        return failure;
    }
    bool skipped = false;
    if (const char* failure = check_parse(file, fixed, {.checksums = Checksums::skip}, skipped)) {
        return failure;
    }
    bool repaired = false;
    if (const char* failure =
            check_parse(fixed, fixed, {.checksums = Checksums::verify}, repaired)) {
        return failure;
    }
    if (skipped != repaired) {
        return "skipping CRCs and recomputing them gave different verdicts";
    }
    if (as_is && !skipped) {
        return "a file accepted with CRCs verified was refused with them skipped";
    }
    if (!repaired) {
        return nullptr;
    }

    const std::size_t choice = std::to_integer<std::size_t>(fixed[fixed.size() / 2]) * 257U;
    const auto cut = std::span<const std::byte>{fixed}.first(choice % fixed.size());
    bool cut_accepted = false;
    if (const char* failure = check_parse(cut, cut, {.checksums = Checksums::verify}, cut_accepted)) {
        return failure;
    }

    detail::Bytes flipped = fixed;
    flipped[choice % flipped.size()] ^= static_cast<std::byte>(1U << (choice % 8U));
    bool flip_accepted = false;
    if (const char* failure =
            check_parse(flipped, flipped, {.checksums = Checksums::verify}, flip_accepted)) {
        return failure;
    }
    const detail::Bytes flipped_fixed = fixed_checksums(flipped);
    return check_parse(flipped, flipped_fixed, {.checksums = Checksums::skip}, flip_accepted);
}

}  // namespace tick_store::store::oracle
