#pragma once

// A whole file mapped read-only.
//
// The mapped length is the fstat size at open (M5), and munmap gets that same
// length (M9). Any span taken from bytes() is valid only while this object is, and
// it stays valid across a move, because a move does not change the mapping.
//
// SIGBUS (M7): a file cut short while it is mapped raises SIGBUS on the next read
// past the new end, and so does a read error from the device under a mapped page.
// Nothing here catches the signal, and nothing ever will: a handler cannot tell which
// read faulted or resume it safely, which is ClickHouse's and QuestDB's conclusion
// too. The process dies. The store prevents the cause instead: a file is published
// whole by rename and never written, truncated or extended after (container_writer.hpp),
// so a writer replacing a file makes a new inode and the old mapping keeps the old
// one. Only something outside the store cutting a file in place can raise the
// signal. docs/format.md states the same stance for the container.

#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

namespace tick_store::store {

// The kernel's read-ahead hint for the mapping. Only a hint: reads work the same
// under either.
enum class Access : std::uint8_t {
    // One pass from start to end, the verification tools' pattern.
    sequential,
    // Reads anywhere, such as blocks picked out of a container by an index. The
    // kernel's default read-ahead, not MADV_RANDOM, which would fault a block of a
    // few pages in one page at a time.
    normal,
};

class MappedFile {
public:
    // An empty regular file gives an empty span and no mapping, since mmap refuses
    // length 0. Anything but a regular file is an error.
    [[nodiscard]] static std::expected<MappedFile, StoreError> open(
        const std::filesystem::path& path, Access access = Access::sequential);

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
