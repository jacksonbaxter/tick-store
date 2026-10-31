#include "tick_store/harness/page_cache.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::harness {
namespace {

[[nodiscard]] std::string failure(std::string_view call, const std::filesystem::path& path) {
    return std::format("{} failed on {}: {}", call, path.string(), std::strerror(errno));
}

// A read-only MAP_SHARED mapping that lasts one scope and is never touched.
class FreshMapping {
public:
    [[nodiscard]] static std::expected<FreshMapping, std::string> open(
        const std::filesystem::path& path) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(failure("open", path));
        }
        struct stat info{};
        if (::fstat(fd, &info) != 0) {
            const std::string message = failure("fstat", path);
            ::close(fd);
            return std::unexpected(message);
        }
        const auto length = static_cast<std::size_t>(info.st_size);
        void* base = nullptr;
        if (length != 0) {
            base = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED) {
                const std::string message = failure("mmap", path);
                ::close(fd);
                return std::unexpected(message);
            }
        }
        ::close(fd);
        return FreshMapping{base, length};
    }

    FreshMapping(const FreshMapping&) = delete;
    FreshMapping& operator=(const FreshMapping&) = delete;
    FreshMapping(FreshMapping&& other) noexcept : base_(other.base_), length_(other.length_) {
        other.base_ = nullptr;
        other.length_ = 0;
    }
    FreshMapping& operator=(FreshMapping&&) = delete;
    ~FreshMapping() {
        if (length_ != 0) {
            ::munmap(base_, length_);
        }
    }

    [[nodiscard]] void* base() const noexcept { return base_; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }

private:
    FreshMapping(void* base, std::size_t length) noexcept : base_(base), length_(length) {}

    void* base_;
    std::size_t length_;
};

}  // namespace

std::uint64_t page_size() noexcept { return static_cast<std::uint64_t>(::getpagesize()); }

std::string_view eviction_method() noexcept {
    return "msync(MS_INVALIDATE) on a fresh read-only MAP_SHARED mapping of every file";
}

bool live_mappings_block_eviction() noexcept { return false; }

std::expected<FileResidency, std::string> file_residency(const std::filesystem::path& path) {
    auto mapping = FreshMapping::open(path);
    if (!mapping.has_value()) {
        return std::unexpected(mapping.error());
    }
    const std::size_t length = mapping->length();
    FileResidency result{.bytes = length, .pages = 0, .resident_pages = 0, .dirty_pages = 0};
    if (length == 0) {
        return result;
    }
    const std::uint64_t size = page_size();
    result.pages = (length + size - 1) / size;
    std::vector<char> vector(result.pages);
    if (::mincore(mapping->base(), length, vector.data()) != 0) {
        return std::unexpected(failure("mincore", path));
    }
    std::uint64_t dirty = 0;
    for (const char flags : vector) {
        result.resident_pages += (flags & MINCORE_INCORE) != 0 ? 1U : 0U;
        dirty += (flags & MINCORE_MODIFIED) != 0 ? 1U : 0U;
    }
    result.dirty_pages = dirty;
    return result;
}

std::expected<void, std::string> evict_file(const std::filesystem::path& path) {
    const auto mapping = FreshMapping::open(path);
    if (!mapping.has_value()) {
        return std::unexpected(mapping.error());
    }
    if (mapping->length() != 0 && ::msync(mapping->base(), mapping->length(), MS_INVALIDATE) != 0) {
        return std::unexpected(failure("msync", path));
    }
    return {};
}

}  // namespace tick_store::harness
