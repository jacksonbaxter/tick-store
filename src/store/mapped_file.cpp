#include "tick_store/store/mapped_file.hpp"

#include "tick_store/store/store_error.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

namespace tick_store::store {
namespace {

[[nodiscard]] std::unexpected<StoreError> io_error(std::string_view call) noexcept {
    return std::unexpected(StoreError{.kind = StoreErrorKind::io,
                                      .field = call,
                                      .actual = static_cast<std::uint64_t>(errno)});
}

}  // namespace

std::expected<MappedFile, StoreError> MappedFile::open(const std::filesystem::path& path,
                                                     Access access) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return io_error("open");
    }

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        auto error = io_error("fstat");
        ::close(fd);
        return error;
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        return std::unexpected(StoreError{.kind = StoreErrorKind::io,
                                          .field = "not a regular file",
                                          .actual = static_cast<std::uint64_t>(EINVAL)});
    }

    const auto size = static_cast<std::size_t>(info.st_size);
    if (size == 0) {
        ::close(fd);
        return MappedFile{std::span<const std::byte>{}};
    }

    void* mapping = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr): MAP_FAILED is defined as a cast of -1.
    if (mapping == MAP_FAILED) {
        auto error = io_error("mmap");
        ::close(fd);
        return error;
    }
    // A mapping outlives its descriptor. The advice is only a hint, so a failure is
    // not an error: the reads work the same without it.
    ::close(fd);
    ::madvise(mapping, size, access == Access::sequential ? MADV_SEQUENTIAL : MADV_NORMAL);
    return MappedFile{std::span{static_cast<const std::byte*>(mapping), size}};
}

MappedFile::MappedFile(MappedFile&& other) noexcept : bytes_(std::exchange(other.bytes_, {})) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        unmap();
        bytes_ = std::exchange(other.bytes_, {});
    }
    return *this;
}

MappedFile::~MappedFile() { unmap(); }

void MappedFile::unmap() noexcept {
    if (bytes_.empty()) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): munmap takes a mutable pointer.
    if (::munmap(const_cast<std::byte*>(bytes_.data()), bytes_.size()) != 0) {
        std::abort();
    }
    bytes_ = {};
}

}  // namespace tick_store::store
