#include "tick_store/harness/page_cache.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
