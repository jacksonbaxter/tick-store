#include "tick_store/store/container_writer.hpp"

#include "tick_store/codec/registry.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/store_error.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

namespace tick_store::store {
namespace {

// Small blocks are gathered into one write; a payload this large or larger goes to the
// kernel directly, after whatever is buffered.
constexpr std::size_t kWriteBufferSize = std::size_t{1} << 20U;

[[gnu::cold, gnu::noinline]] StoreError io_error(std::string_view call) noexcept {
    return {.kind = StoreErrorKind::io,
            .field = call,
            .actual = static_cast<std::uint64_t>(errno)};
}

}  // namespace

std::expected<ContainerWriter, StoreError> ContainerWriter::create(
    const std::filesystem::path& path, Durability durability, FileOps& ops) {
    std::filesystem::path partial_path = path;
    partial_path += ".partial";
    const int fd = ops.create_file(partial_path.c_str());
    if (fd < 0) {
        return std::unexpected(io_error("open"));
    }
    ContainerWriter writer{ops, fd, path, std::move(partial_path), durability};
    if (auto written = writer.append(encode_container_header()); !written.has_value()) {
        return std::unexpected(written.error());
    }
    return writer;
}

ContainerWriter::ContainerWriter(FileOps& ops, int fd, std::filesystem::path path,
                                 std::filesystem::path partial_path, Durability durability)
    : ops_(&ops),
      fd_(fd),
      path_(std::move(path)),
      partial_path_(std::move(partial_path)),
      durability_(durability),
      buffer_(kWriteBufferSize) {}

ContainerWriter::ContainerWriter(ContainerWriter&& other) noexcept
    : ops_(other.ops_),
      fd_(std::exchange(other.fd_, -1)),
      path_(std::move(other.path_)),
      partial_path_(std::move(other.partial_path_)),
      durability_(other.durability_),
      layout_(std::move(other.layout_)),
      buffer_(std::move(other.buffer_)),
      buffered_(std::exchange(other.buffered_, 0)),
      failed_(other.failed_) {}

ContainerWriter::~ContainerWriter() { abandon(); }

void ContainerWriter::abandon() noexcept {
    if (fd_ >= 0) {
        // Best effort on the way out: the file was never published, a stale partial
        // file is harmless, and the next writer for this path overwrites it.
        static_cast<void>(ops_->close(fd_));
        fd_ = -1;
        static_cast<void>(ops_->unlink(partial_path_.c_str()));
    }
}

std::unexpected<StoreError> ContainerWriter::fail(StoreError error) {
    failed_ = error;
    abandon();
    return std::unexpected(error);
}

std::expected<void, StoreError> ContainerWriter::flush() {
    auto written = write_all(*ops_, fd_, std::span{buffer_}.first(buffered_));
    buffered_ = 0;
    return written;
}

std::expected<void, StoreError> ContainerWriter::append(std::span<const std::byte> bytes) {
    if (bytes.size() >= buffer_.size()) {
        if (auto flushed = flush(); !flushed.has_value()) {
            return fail(flushed.error());
        }
        if (auto written = write_all(*ops_, fd_, bytes); !written.has_value()) {
            return fail(written.error());
        }
        return {};
    }
    if (bytes.size() > buffer_.size() - buffered_) {
        if (auto flushed = flush(); !flushed.has_value()) {
            return fail(flushed.error());
        }
    }
    std::ranges::copy(bytes, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
    buffered_ += bytes.size();
    return {};
}

std::expected<std::uint64_t, StoreError> ContainerWriter::add_block(
    codec::CodecId codec, std::span<const std::byte> metadata,
    std::span<const std::byte> payload) {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    const auto index = layout_.add_block(codec, metadata, payload);
    if (!index.has_value()) {
        return index;
    }
    if (auto written = append(payload); !written.has_value()) {
        return std::unexpected(written.error());
    }
    return index;
}

std::expected<void, StoreError> ContainerWriter::add_section(std::string_view name,
                                                             std::span<const std::byte> bytes) {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    if (auto added = layout_.add_section(name, bytes); !added.has_value()) {
        return added;
    }
    return append(bytes);
}

std::expected<void, StoreError> ContainerWriter::publish() {
    // Step 1: the commit record goes last, after every block and section.
    if (auto written = append(layout_.encode_footer_and_trailer()); !written.has_value()) {
        return written;
    }
    if (auto flushed = flush(); !flushed.has_value()) {
        return fail(flushed.error());
    }

    // Step 2: the whole file reaches the device before anything names it (M1).
    if (durability_ == Durability::device_flush) {
        if (auto synced = sync_file(*ops_, fd_); !synced.has_value()) {
            return fail(synced.error());
        }
    }

    // Step 3: close, checked.
    const int fd = std::exchange(fd_, -1);
    if (ops_->close(fd) != 0) {
        const StoreError error = io_error("close");
        static_cast<void>(ops_->unlink(partial_path_.c_str()));
        failed_ = error;
        return std::unexpected(error);
    }

    // Step 4: the rename publishes the file. This is the commit point (M2).
    if (ops_->rename(partial_path_.c_str(), path_.c_str()) != 0) {
        const StoreError error = io_error("rename");
        static_cast<void>(ops_->unlink(partial_path_.c_str()));
        failed_ = error;
        return std::unexpected(error);
    }

    // Step 5: the rename reaches the device. The file is already at `path`, so a
    // failure here removes nothing.
    if (durability_ == Durability::device_flush) {
        if (auto synced = sync_directory(*ops_, path_.parent_path()); !synced.has_value()) {
            failed_ = synced.error();
            return synced;
        }
    }
    return {};
}

std::expected<void, StoreError> ContainerWriter::commit() {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    auto published = publish();
    if (published.has_value()) {
        // A later call must fail rather than write to a closed descriptor.
        failed_ = StoreError{.kind = StoreErrorKind::io, .field = "writer already committed"};
    }
    return published;
}

}  // namespace tick_store::store
