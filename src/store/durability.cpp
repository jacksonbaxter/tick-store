#include "tick_store/store/durability.hpp"

#include "tick_store/store/store_error.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>

namespace tick_store::store {
namespace {

// The most one write is asked for. macOS refuses a request over INT_MAX bytes with EINVAL,
// where Linux writes part of it; both write this much whole.
constexpr std::size_t kMaxWriteRequest = std::size_t{1} << 30U;

[[gnu::cold, gnu::noinline]] StoreError io_error(std::string_view call) noexcept {
    return {.kind = StoreErrorKind::io,
            .field = call,
            .actual = static_cast<std::uint64_t>(errno)};
}

class PosixFileOps final : public FileOps {
public:
    int create_file(const char* path) override {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        return ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }

    int open_directory(const char* path) override {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        return ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    std::ptrdiff_t write(int fd, const std::byte* data, std::size_t size) override {
        return ::write(fd, data, size);
    }

    int full_fsync(int fd) override {
#ifdef F_FULLFSYNC
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        return ::fcntl(fd, F_FULLFSYNC);
#else
        static_cast<void>(fd);
        errno = ENOTSUP;
        return -1;
#endif
    }

    int fsync(int fd) override { return ::fsync(fd); }

    int fdatasync(int fd) override {
#ifdef __APPLE__
        // macOS declares no fdatasync, and its policy never asks for one.
        static_cast<void>(fd);
        errno = ENOTSUP;
        return -1;
#else
        return ::fdatasync(fd);
#endif
    }

    int close(int fd) override { return ::close(fd); }

    int rename(const char* from, const char* to) override { return std::rename(from, to); }

    int unlink(const char* path) override { return ::unlink(path); }
};

// F_FULLFSYNC, and fsync only where the file system says it has no F_FULLFSYNC. Any
// other failure is returned (M4).
[[nodiscard]] std::expected<void, StoreError> full_fsync_or_fallback(
    FileOps& ops, int fd, std::string_view full_name, std::string_view fallback_name) noexcept {
    if (ops.full_fsync(fd) == 0) {
        return {};
    }
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) {
        return std::unexpected(io_error(full_name));
    }
    if (ops.fsync(fd) != 0) {
        return std::unexpected(io_error(fallback_name));
    }
    return {};
}

}  // namespace

std::string_view to_string(Durability durability) noexcept {
    switch (durability) {
        case Durability::page_cache:
            return "page_cache";
        case Durability::device_flush:
            return "device_flush";
    }
    return "invalid";
}

FileOps& posix_file_ops() noexcept {
    static PosixFileOps ops;
    return ops;
}

std::expected<void, StoreError> sync_file(FileOps& ops, int fd, SyncPolicy policy) noexcept {
    switch (policy) {
        case SyncPolicy::darwin_full_fsync:
            return full_fsync_or_fallback(ops, fd, "F_FULLFSYNC", "fsync");
        case SyncPolicy::linux_fdatasync:
            if (ops.fdatasync(fd) != 0) {
                return std::unexpected(io_error("fdatasync"));
            }
            return {};
    }
    return std::unexpected(StoreError{.kind = StoreErrorKind::io, .field = "sync policy"});
}

std::expected<void, StoreError> sync_directory(FileOps& ops,
                                               const std::filesystem::path& directory,
                                               SyncPolicy policy) {
    const std::filesystem::path name = directory.empty() ? "." : directory;
    const int fd = ops.open_directory(name.c_str());
    if (fd < 0) {
        return std::unexpected(io_error("open directory"));
    }
    std::expected<void, StoreError> synced;
    switch (policy) {
        case SyncPolicy::darwin_full_fsync:
            synced = full_fsync_or_fallback(ops, fd, "F_FULLFSYNC directory", "fsync directory");
            break;
        case SyncPolicy::linux_fdatasync:
            if (ops.fsync(fd) != 0) {
                synced = std::unexpected(io_error("fsync directory"));
            }
            break;
    }
    // A directory opened read-only has nothing to lose at close, so its result
    // carries no information about the flush.
    static_cast<void>(ops.close(fd));
    return synced;
}

std::expected<void, StoreError> write_all(FileOps& ops, int fd,
                                          std::span<const std::byte> bytes) noexcept {
    while (!bytes.empty()) {
        const std::ptrdiff_t written =
            ops.write(fd, bytes.data(), std::min(bytes.size(), kMaxWriteRequest));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(io_error("write"));
        }
        if (written == 0) {
            // POSIX allows a zero return only for a zero-byte request; treat it as a
            // device that accepts nothing, rather than loop for ever.
            errno = EIO;
            return std::unexpected(io_error("write"));
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return {};
}

}  // namespace tick_store::store
