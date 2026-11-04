#include "tick_store/store/stub_store.hpp"

#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

namespace tick_store::store {
namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 8;
constexpr std::size_t kHeaderSizeOffset = 12;
constexpr std::size_t kRecordCountOffset = 16;
constexpr std::size_t kRecordsBytesOffset = 24;
constexpr std::size_t kTailBytesOffset = 32;

static_assert(kTailBytesOffset + 8 == kStubHeaderSize);
static_assert(kStubMagic.size() == kVersionOffset - kMagicOffset);

// A flush happens before an append could run out of room for the largest frame.
constexpr std::size_t kWriteBufferSize = std::size_t{1} << 20U;
constexpr std::size_t kLargestFrame = itch::kFramePrefixSize + itch::kMaxPayloadSize;
static_assert(kLargestFrame < kWriteBufferSize);

[[nodiscard]] constexpr std::uint64_t magic_value(std::span<const std::byte> bytes) noexcept {
    return read_le<8>(bytes, kMagicOffset).value_or(0);
}

[[gnu::cold, gnu::noinline]] StoreError bad_field(StoreErrorKind kind, std::string_view field,
                                                  std::uint64_t offset, std::uint64_t expected,
                                                  std::uint64_t actual) noexcept {
    return {.kind = kind,
            .field = field,
            .offset = offset,
            .expected = expected,
            .actual = actual};
}

[[gnu::cold, gnu::noinline]] StoreError io_error(std::string_view call) noexcept {
    return {.kind = StoreErrorKind::io,
            .field = call,
            .actual = static_cast<std::uint64_t>(errno)};
}

// The offset is pwrite's off_t, spelled std::int64_t as in flip_bit.cpp, and a narrower off_t
// would fail -Wconversion at the call.
[[nodiscard]] std::expected<void, StoreError> write_all_at(int fd,
                                                           std::span<const std::byte> bytes,
                                                           std::int64_t offset) noexcept {
    while (!bytes.empty()) {
        const auto written = ::pwrite(fd, bytes.data(), bytes.size(), offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(io_error("pwrite"));
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
        offset += written;
    }
    return {};
}

}  // namespace

std::array<std::byte, kStubHeaderSize> encode_stub_header(const StubHeader& header) noexcept {
    std::array<std::byte, kStubHeaderSize> bytes{};
    // Every offset below is in bounds, as the static_asserts above prove, so the
    // results of the checked writers carry no information.
    std::ranges::copy(kStubMagic, bytes.begin());
    std::ignore = write_le<4>(bytes, kVersionOffset, kStubVersion);
    std::ignore = write_le<4>(bytes, kHeaderSizeOffset, kStubHeaderSize);
    std::ignore = write_le<8>(bytes, kRecordCountOffset, header.record_count);
    std::ignore = write_le<8>(bytes, kRecordsBytesOffset, header.records_bytes);
    std::ignore = write_le<8>(bytes, kTailBytesOffset, header.tail_bytes);
    return bytes;
}

std::expected<StubWriter, StoreError> StubWriter::create(const std::filesystem::path& path) {
    std::filesystem::path partial_path = path;
    partial_path += ".partial";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
    const int fd = ::open(partial_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return std::unexpected(io_error("open"));
    }
    StubWriter writer{fd, path, std::move(partial_path)};
    // The header goes in last. Until then this file has an all-zero header, so it
    // could never parse even if it were opened by mistake.
    constexpr std::array<std::byte, kStubHeaderSize> kNoHeader{};
    if (auto written = write_all(posix_file_ops(), fd, kNoHeader); !written.has_value()) {
        return std::unexpected(written.error());
    }
    return writer;
}

StubWriter::StubWriter(int fd, std::filesystem::path path, std::filesystem::path partial_path)
    : fd_(fd),
      path_(std::move(path)),
      partial_path_(std::move(partial_path)),
      buffer_(kWriteBufferSize) {}

StubWriter::StubWriter(StubWriter&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      path_(std::move(other.path_)),
      partial_path_(std::move(other.partial_path_)),
      buffer_(std::move(other.buffer_)),
      buffered_(other.buffered_),
      failed_(other.failed_),
      header_(other.header_) {}

StubWriter::~StubWriter() { abandon(); }

void StubWriter::abandon() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        // Best effort on the way out: a stale partial file is harmless, and the next
        // writer overwrites it.
        ::unlink(partial_path_.c_str());
    }
}

std::expected<void, StoreError> StubWriter::flush() {
    auto written = write_all(posix_file_ops(), fd_, std::span{buffer_}.first(buffered_));
    buffered_ = 0;
    return written;
}

std::expected<void, StoreError> StubWriter::append(const itch::Message& message) {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    const auto fail = [&](StoreError error) -> std::expected<void, StoreError> {
        failed_ = error;
        abandon();
        return std::unexpected(error);
    };

    if (buffer_.size() - buffered_ < kLargestFrame) {
        if (auto flushed = flush(); !flushed.has_value()) {
            return fail(flushed.error());
        }
    }
    const auto written = itch::encode_frame(message, std::span{buffer_}.subspan(buffered_));
    if (!written.has_value()) {
        return fail(bad_field(StoreErrorKind::unencodable_record, written.error().field,
                              header_.record_count, written.error().expected,
                              written.error().actual));
    }
    if (*written == itch::kFramePrefixSize) {
        return fail(bad_field(StoreErrorKind::unencodable_record, "empty record",
                              header_.record_count, 1, 0));
    }
    buffered_ += *written;
    ++header_.record_count;
    header_.records_bytes += *written;
    return {};
}

std::expected<void, StoreError> StubWriter::commit(std::span<const std::byte> tail) {
    // Step 1: every byte of the records and the tail is written to the partial file.
    if (auto flushed = flush(); !flushed.has_value()) {
        return flushed;
    }
    if (auto written = write_all(posix_file_ops(), fd_, tail); !written.has_value()) {
        return written;
    }
    header_.tail_bytes = tail.size();

    // Step 2: the header, which until now was all zeros.
    const auto header = encode_stub_header(header_);
    if (auto written = write_all_at(fd_, header, 0); !written.has_value()) {
        return written;
    }

    // Step 3: the whole file reaches the device, before anything can name it (M1).
    if (auto synced = sync_file(posix_file_ops(), fd_); !synced.has_value()) {
        return synced;
    }

    // Step 4: close, checked, because some file systems report a late failure here.
    const int fd = std::exchange(fd_, -1);
    if (::close(fd) != 0) {
        const StoreError error = io_error("close");
        ::unlink(partial_path_.c_str());
        return std::unexpected(error);
    }

    // Step 5: the rename publishes the file. This is the commit point (M2).
    if (::rename(partial_path_.c_str(), path_.c_str()) != 0) {
        const StoreError error = io_error("rename");
        ::unlink(partial_path_.c_str());
        return std::unexpected(error);
    }

    // Step 6: the rename itself reaches the device.
    return sync_directory(posix_file_ops(), path_.parent_path());
}

std::expected<void, StoreError> StubWriter::finish(std::span<const std::byte> tail) {
    if (failed_.has_value()) {
        return std::unexpected(*failed_);
    }
    auto committed = commit(tail);
    if (!committed.has_value()) {
        failed_ = committed.error();
        abandon();
        return committed;
    }
    // A later append or finish must fail rather than write to a closed descriptor.
    failed_ = StoreError{.kind = StoreErrorKind::io, .field = "writer already finished"};
    return committed;
}

std::expected<StoreParser, StoreError> StoreParser::open(std::span<const std::byte> file) noexcept {
    if (file.size() < kStubHeaderSize) {
        return std::unexpected(bad_field(StoreErrorKind::file_too_small, "file", 0,
                                         kStubHeaderSize, file.size()));
    }
    // The size check above covers every read below, so value_or never picks its
    // argument, and the checked reader stays the only way to these bytes.
    if (!std::ranges::equal(file.first(kStubMagic.size()), kStubMagic)) {
        return std::unexpected(bad_field(StoreErrorKind::bad_magic, "magic", kMagicOffset,
                                         magic_value(kStubMagic), magic_value(file)));
    }
    const std::uint64_t version = read_le<4>(file, kVersionOffset).value_or(0);
    if (version != kStubVersion) {
        return std::unexpected(bad_field(StoreErrorKind::unsupported_version, "version",
                                         kVersionOffset, kStubVersion, version));
    }
    const std::uint64_t header_size = read_le<4>(file, kHeaderSizeOffset).value_or(0);
    if (header_size != kStubHeaderSize) {
        return std::unexpected(bad_field(StoreErrorKind::bad_header_size, "header_size",
                                         kHeaderSizeOffset, kStubHeaderSize, header_size));
    }

    const StubHeader header{.record_count = read_le<8>(file, kRecordCountOffset).value_or(0),
                            .records_bytes = read_le<8>(file, kRecordsBytesOffset).value_or(0),
                            .tail_bytes = read_le<8>(file, kTailBytesOffset).value_or(0)};

    // Comparing each size with what is left, not their sum, so no value can wrap (D7).
    const std::uint64_t after_header = file.size() - kStubHeaderSize;
    if (header.records_bytes > after_header) {
        return std::unexpected(bad_field(StoreErrorKind::bad_section_sizes, "records_bytes",
                                         kRecordsBytesOffset, header.records_bytes, after_header));
    }
    const std::uint64_t after_records = after_header - header.records_bytes;
    if (header.tail_bytes != after_records) {
        return std::unexpected(bad_field(StoreErrorKind::bad_section_sizes, "tail_bytes",
                                         kTailBytesOffset, header.tail_bytes, after_records));
    }

    const auto body = file.subspan(kStubHeaderSize);
    const auto records_size = static_cast<std::size_t>(header.records_bytes);
    return StoreParser{header, body.first(records_size), body.subspan(records_size)};
}

std::expected<std::optional<itch::Message>, StoreError> StoreParser::next() noexcept {
    const auto frame = cursor_.next();
    if (frame.has_value()) [[likely]] {
        ++records_read_;
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) [[unlikely]] {
            // The cursor returns no empty payload, and only that fails to decode.
            return std::unexpected(bad_field(StoreErrorKind::zero_length_record, "record",
                                             kStubHeaderSize + frame->offset, 1, 0));
        }
        return *message;
    }

    const itch::FrameStop& stop = frame.error();
    const std::uint64_t offset = kStubHeaderSize + stop.offset;
    switch (stop.kind) {
        case itch::FrameStopKind::end:
            if (records_read_ != header_.record_count) {
                return std::unexpected(bad_field(StoreErrorKind::record_count_mismatch,
                                                 "record_count", kRecordCountOffset,
                                                 header_.record_count, records_read_));
            }
            return std::nullopt;
        case itch::FrameStopKind::truncated_prefix:
            return std::unexpected(bad_field(StoreErrorKind::truncated_record, "frame prefix",
                                             offset, stop.expected, stop.actual));
        case itch::FrameStopKind::truncated_body:
            return std::unexpected(bad_field(StoreErrorKind::truncated_record, "frame body", offset,
                                             stop.expected, stop.actual));
        case itch::FrameStopKind::zero_length:
            return std::unexpected(
                bad_field(StoreErrorKind::zero_length_record, "record", offset, 1, 0));
    }
    return std::unexpected(bad_field(StoreErrorKind::truncated_record, "frame stop", offset, 0, 0));
}

}  // namespace tick_store::store
