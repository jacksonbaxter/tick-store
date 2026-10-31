#include "tick_store/harness/flip_bit.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tick_store::harness {
namespace {

[[nodiscard]] std::unexpected<std::string> system_failure(std::string_view call) {
    return std::unexpected(
        std::format("{} failed: {}", call, std::generic_category().message(errno)));
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

}  // namespace

std::expected<FlippedByte, std::string> flip_bit(const std::filesystem::path& path,
                                                 std::uint64_t byte_offset, unsigned bit) {
    if (bit > 7U) {
        return std::unexpected(std::format("bit {} is not in 0..7", bit));
    }
    if (std::cmp_greater(byte_offset, std::numeric_limits<off_t>::max())) {
        return std::unexpected(
            std::format("byte offset {} is past the end of the file", byte_offset));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
    const Descriptor file{::open(path.c_str(), O_RDWR | O_CLOEXEC)};
    if (file.get() < 0) {
        return system_failure("open");
    }
    struct stat info{};
    if (::fstat(file.get(), &info) != 0) {
        return system_failure("fstat");
    }
    if (!S_ISREG(info.st_mode)) {
        return std::unexpected("not a regular file");
    }
    if (std::cmp_greater_equal(byte_offset, info.st_size)) {
        return std::unexpected(std::format("byte offset {} is past the end of the file, {} bytes",
                                           byte_offset, info.st_size));
    }

    const auto offset = static_cast<off_t>(byte_offset);
    std::array<std::uint8_t, 1> value{};
    if (::pread(file.get(), value.data(), value.size(), offset) != 1) {
        return system_failure("pread");
    }
    const std::uint8_t before = value[0];
    value[0] ^= static_cast<std::uint8_t>(1U << bit);
    if (::pwrite(file.get(), value.data(), value.size(), offset) != 1) {
        return system_failure("pwrite");
    }
    return FlippedByte{.before = before, .after = value[0]};
}

}  // namespace tick_store::harness
