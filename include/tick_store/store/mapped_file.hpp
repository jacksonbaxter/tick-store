#pragma once

// A whole file mapped read-only.
//
// The mapped length is the fstat size at open (M5), and munmap gets that same
// length (M9). Any span taken from bytes() is valid only while this object is, and
// it stays valid across a move, because a move does not change the mapping.
//
// SIGBUS (M7): a file cut short while it is mapped raises SIGBUS on the next read
// past the new end. Nothing here catches it. These are verification tools working
// on files no other process is writing, so the process dies and the run has failed,
// which is the right outcome for a check.

#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>

namespace tick_store::store {

class MappedFile {
public:
    // An empty regular file gives an empty span and no mapping, since mmap refuses
    // length 0. Anything but a regular file is an error.
    [[nodiscard]] static std::expected<MappedFile, StoreError> open(
        const std::filesystem::path& path);

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Unmaps. munmap can only fail here on a bug, so a failure aborts: a destructor
    // has nowhere to report it, and carrying on would hide a corrupted address space.
    ~MappedFile();

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
    explicit MappedFile(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    void unmap() noexcept;

    std::span<const std::byte> bytes_;
};

}  // namespace tick_store::store
