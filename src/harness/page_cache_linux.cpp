#include "tick_store/harness/page_cache.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tick_store::harness {
namespace {

// cachestat(2), from Linux 6.5. glibc has no wrapper yet, so the call and its two
// structures are spelled out from the kernel's uapi header.
constexpr long kCachestatSyscall = 451;

struct CachestatRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct Cachestat {
    std::uint64_t cache = 0;
    std::uint64_t dirty = 0;
    std::uint64_t writeback = 0;
    std::uint64_t evicted = 0;
    std::uint64_t recently_evicted = 0;
};

[[nodiscard]] std::string failure(std::string_view call, const std::filesystem::path& path) {
    return std::format("{} failed on {}: {}", call, path.string(), std::generic_category().message(errno));
}

class Descriptor {
public:
    explicit Descriptor(int fd) noexcept : fd_(fd) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    ~Descriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

[[nodiscard]] int open_read_only(const std::filesystem::path& path) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
}

// Since 5.2, Linux reveals a file's page-cache state only to a caller who owns the file or
// could open it for writing (mm/mincore.c, can_do_mincore()). For any other file mincore
// fills its vector with 1, so every page reads as resident and eviction can never be seen,
// and newer kernels refuse cachestat on it with EPERM. Neither answer can be told from a
// real one, so such a file is refused. Root passes the kernel's owner check by CAP_FOWNER;
// a non-root caller holding CAP_FOWNER is refused here, which errs on the safe side.
[[nodiscard]] std::optional<std::string> unobservable(const std::filesystem::path& path,
                                                      const struct stat& info) {
    const uid_t user = ::geteuid();
    if (user == 0 || info.st_uid == user ||
        ::faccessat(AT_FDCWD, path.c_str(), W_OK, AT_EACCESS) == 0) {
        return std::nullopt;
    }
    return std::format(
        "page-cache residency is not observable for {}: Linux's mincore reports every page "
        "of a file resident unless the caller owns it or could open it for writing",
        path.string());
}

// Dirty plus writeback pages, or nullopt on a kernel without cachestat.
[[nodiscard]] std::optional<std::uint64_t> unflushed_pages(int fd) noexcept {
    const CachestatRange range{};
    Cachestat stat{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the syscall interface is variadic.
    if (::syscall(kCachestatSyscall, fd, &range, &stat, 0U) != 0) {
        return std::nullopt;
    }
    return stat.dirty + stat.writeback;
}

}  // namespace

std::uint64_t page_size() noexcept { return static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)); }

std::string_view eviction_method() noexcept {
    return "fsync, then posix_fadvise(POSIX_FADV_DONTNEED) over the whole of every file";
}

bool live_mappings_block_eviction() noexcept { return true; }

std::expected<FileResidency, std::string> file_residency(const std::filesystem::path& path) {
    const Descriptor fd{open_read_only(path)};
    if (fd.get() < 0) {
        return std::unexpected(failure("open", path));
    }
    struct stat info{};
    if (::fstat(fd.get(), &info) != 0) {
        return std::unexpected(failure("fstat", path));
    }
    if (auto refused = unobservable(path, info); refused.has_value()) {
        return std::unexpected(std::move(*refused));
    }
    const auto length = static_cast<std::size_t>(info.st_size);
    FileResidency result{.bytes = length,
                         .pages = 0,
                         .resident_pages = 0,
                         .dirty_pages = unflushed_pages(fd.get())};
    if (length == 0) {
        return result;
    }
    void* const base = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd.get(), 0);
    if (base == MAP_FAILED) {
        return std::unexpected(failure("mmap", path));
    }
    const std::uint64_t size = page_size();
    result.pages = (length + size - 1) / size;
    std::vector<unsigned char> vector(result.pages);
    const int status = ::mincore(base, length, vector.data());
    const int saved = errno;
    ::munmap(base, length);
    if (status != 0) {
        errno = saved;
        return std::unexpected(failure("mincore", path));
    }
    for (const unsigned char flags : vector) {
        result.resident_pages += flags & 1U;
    }
    return result;
}

std::expected<void, std::string> evict_file(const std::filesystem::path& path) {
    const Descriptor fd{open_read_only(path)};
    if (fd.get() < 0) {
        return std::unexpected(failure("open", path));
    }
    struct stat info{};
    if (::fstat(fd.get(), &info) != 0) {
        return std::unexpected(failure("fstat", path));
    }
    // Eviction of such a file could never be proven, so it fails at once with the reason.
    if (auto refused = unobservable(path, info); refused.has_value()) {
        return std::unexpected(std::move(*refused));
    }
    if (::fsync(fd.get()) != 0) {
        return std::unexpected(failure("fsync", path));
    }
    if (const int status = ::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_DONTNEED); status != 0) {
        errno = status;
        return std::unexpected(failure("posix_fadvise", path));
    }
    return {};
}

}  // namespace tick_store::harness
