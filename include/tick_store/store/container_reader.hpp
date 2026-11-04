#pragma once

// Maps a container file read-only and parses it (container.hpp).
//
// open() maps the whole file at its fstat size and proves everything against that
// length before any other read: the trailer's footer offset, the footer's counts and
// every block's and section's extent (M5). So no stored size can send a read past the
// end of the mapping.
//
// Views keep the mapping alive (A3): a BlockView or SectionView from here holds a
// shared reference to it, so its spans stay valid after the reader that made it is
// destroyed, and the mapping goes when its last holder does. The price is one
// reference-count increment and decrement per view; take a view once per block, not
// once per value.
//
// munmap gets the length that was mapped, never one read from the file, and its
// result is checked (M9): see mapped_file.hpp.
//
// SIGBUS (M7): files are published whole by rename and never changed after, so the
// store itself never truncates a file a reader may have mapped. A file cut short in
// place by something else raises SIGBUS on the next read past its new end, which
// nothing here catches: the process dies rather than read a page that is not there.
// docs/format.md has the reasoning.

#include "tick_store/store/container.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace tick_store::store {

class ContainerReader {
public:
    [[nodiscard]] static std::expected<ContainerReader, StoreError> open(
        const std::filesystem::path& path, ReadOptions options = {});

    [[nodiscard]] std::uint32_t version() const noexcept { return parser_.version(); }
    [[nodiscard]] std::uint64_t block_count() const noexcept { return parser_.block_count(); }
    [[nodiscard]] std::uint64_t section_count() const noexcept {
        return parser_.section_count();
    }

    // From the footer alone: no payload page is touched. The metadata span is valid
    // while this reader is.
    [[nodiscard]] std::expected<BlockInfo, StoreError> block_info(std::uint64_t index) const {
        return parser_.block_info(index);
    }

    // The block's bytes, CRC checked unless the options said skip. The view holds the
    // mapping.
    [[nodiscard]] std::expected<BlockView, StoreError> block(std::uint64_t index) const;

    [[nodiscard]] std::expected<SectionInfo, StoreError> section_info(std::uint64_t index) const {
        return parser_.section_info(index);
    }

    // The section of that name, CRC checked unless the options said skip. The view
    // holds the mapping.
    [[nodiscard]] std::expected<SectionView, StoreError> section(std::string_view name) const;

    [[nodiscard]] std::expected<void, StoreError> verify_all() const {
        return parser_.verify_all();
    }

private:
    ContainerReader(std::shared_ptr<const MappedFile> mapping, ContainerParser parser)
        : mapping_(std::move(mapping)), parser_(std::move(parser)) {}

    std::shared_ptr<const MappedFile> mapping_;
    ContainerParser parser_;
};

}  // namespace tick_store::store
