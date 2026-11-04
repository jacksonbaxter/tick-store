#pragma once

// Durability: the named modes a writer commits in, the system calls behind one seam,
// and the flush each platform needs.
//
// Classes come from BENCHMARKS.md: class A returns once the data is in the OS page
// cache, class B after a flush to the device. Every write number states its class and
// its flush granularity, so both are part of the API here, not a detail of it.

#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>

namespace tick_store::store {

// What a writer's commit has made safe by the time it returns (M8). Flush granularity
// is per file: a commit is the only flush, so nothing written before it is durable,
// and nothing is visible at the final path until it.
enum class Durability : std::uint8_t {
    // Class A. Every byte has been handed to the kernel and the file renamed into
    // place; nothing is flushed. A crash of the process loses nothing, since the page
    // cache outlives it. A crash of the OS or a power loss can lose the new file, can
    // lose the old one it replaced, and can leave a file at the path that the reader
    // refuses at open, because the kernel may write the rename before the data. The
    // reader never accepts such a file as valid (F7).
    page_cache,
    // Class B. The file's data is flushed to the device before the rename that
    // publishes it, and the rename is flushed after (M1, M2). A crash at any point
    // leaves either the old file or the whole new one at the path, never a mix.
    // macOS: fcntl(F_FULLFSYNC) on the file and on its directory, falling back to
    // fsync only where the file system refuses F_FULLFSYNC with ENOTSUP, EINVAL or
    // ENOTTY (M4), because a plain fsync on macOS stops at the drive's cache.
    // Linux: fdatasync on the file, fsync on the directory.
    device_flush,
};

[[nodiscard]] std::string_view to_string(Durability durability) noexcept;

// The system calls a writer makes, behind one interface, so a test can record the
// calls and make any of them fail. Each returns what its POSIX call returns and sets
// errno the same way. posix_file_ops() is the real one.
class FileOps {
public:
    FileOps() = default;
    FileOps(const FileOps&) = delete;
    FileOps& operator=(const FileOps&) = delete;
    FileOps(FileOps&&) = delete;
    FileOps& operator=(FileOps&&) = delete;
    virtual ~FileOps() = default;

    // open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644).
    [[nodiscard]] virtual int create_file(const char* path) = 0;
    // open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC).
    [[nodiscard]] virtual int open_directory(const char* path) = 0;
    // write(fd, data, size). ssize_t is long on every host we build for, as std::ptrdiff_t
    // is, and naming it would need a header glibc and macOS each define it in differently.
    [[nodiscard]] virtual std::ptrdiff_t write(int fd, const std::byte* data, std::size_t size) = 0;
    // fcntl(fd, F_FULLFSYNC), or -1 with errno ENOTSUP where the OS has no such call.
    [[nodiscard]] virtual int full_fsync(int fd) = 0;
    [[nodiscard]] virtual int fsync(int fd) = 0;
    // fdatasync(fd), or -1 with errno ENOTSUP where the OS has no such call (macOS).
    [[nodiscard]] virtual int fdatasync(int fd) = 0;
    [[nodiscard]] virtual int close(int fd) = 0;
    [[nodiscard]] virtual int rename(const char* from, const char* to) = 0;
    [[nodiscard]] virtual int unlink(const char* path) = 0;
};

[[nodiscard]] FileOps& posix_file_ops() noexcept;

// How a flush to the device is made. Both are compiled everywhere, so a test can drive
// either through a recording FileOps on any host. native_sync_policy() is the one for
// this OS, and the default everywhere a policy is taken.
enum class SyncPolicy : std::uint8_t {
    // F_FULLFSYNC, then fsync only if F_FULLFSYNC failed with ENOTSUP, EINVAL or
    // ENOTTY. Files and directories alike.
    darwin_full_fsync,
    // fdatasync on files, which flushes the data and the size needed to read it back,
    // and fsync on directories.
    linux_fdatasync,
};

[[nodiscard]] constexpr SyncPolicy native_sync_policy() noexcept {
#ifdef __APPLE__
    return SyncPolicy::darwin_full_fsync;
#else
    return SyncPolicy::linux_fdatasync;
#endif
}

// Flushes an open file's data to the device. A failure is returned as an io error
// naming the call that failed, and is never retried (M3): after a failed flush the
// kernel may have dropped the dirty pages, so a second flush that succeeds proves
// nothing about the data.
[[nodiscard]] std::expected<void, StoreError> sync_file(
    FileOps& ops, int fd, SyncPolicy policy = native_sync_policy()) noexcept;

// Flushes a directory, so that a rename into it survives a crash. An empty path means
// the current directory.
[[nodiscard]] std::expected<void, StoreError> sync_directory(
    FileOps& ops, const std::filesystem::path& directory,
    SyncPolicy policy = native_sync_policy());

// Writes every byte, retrying a short write and EINTR, and returns an io error naming
// "write" on any other failure.
[[nodiscard]] std::expected<void, StoreError> write_all(FileOps& ops, int fd,
                                                        std::span<const std::byte> bytes) noexcept;

}  // namespace tick_store::store
