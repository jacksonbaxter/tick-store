#pragma once

// STUB. The smallest store that lets the correctness oracle run end to end. Phase 3
// replaces all of it with the real columnar store. Nothing here is the design of that
// store: there are no columns, no compression and no index.
//
// A writer takes decoded Messages and serialises them with the ITCH encoder. A parser
// walks the file and decodes each record back into a Message. No API here takes or
// returns a record as bytes, so a round trip through this store is a real decode and
// encode, not a copy of the source.
//
// There is deliberately no checksum. The oracle has to be what catches corruption, and
// a CRC here would catch it first and leave the oracle untested. The real store will
// have one (F3), checked below the oracle.
//
// File layout, little-endian (F2), all sizes in bytes:
//
//   offset  size  field
//   0       8     magic "TICKSTUB"
//   8       4     version
//   12      4     header size, 40
//   16      8     record count
//   24      8     size of the records section
//   32      8     size of the tail section
//   40            records section: ITCH frames, as encode_frame() writes them
//                 tail section: the source's trailing bytes, verbatim
//
// Every bit of the header is validated: the magic, the version and the header size by
// value, the two sizes against the file size, and the count against the frames.
//
// Over-read contract: the parser reads only inside the span it was given, and needs no
// padding after it.
//
// Lifetime: StoreParser borrows its span, and the Messages it returns follow the rule
// in itch/message.hpp, so a RawMessage borrows from the file. Keep the MappedFile, or
// whatever else owns the bytes, alive for as long as either is in use.

#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/store_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace tick_store::store {

inline constexpr std::array<std::byte, 8> kStubMagic{
    std::byte{'T'}, std::byte{'I'}, std::byte{'C'}, std::byte{'K'},
    std::byte{'S'}, std::byte{'T'}, std::byte{'U'}, std::byte{'B'}};
inline constexpr std::uint32_t kStubVersion = 1;
inline constexpr std::size_t kStubHeaderSize = 40;

struct StubHeader {
    std::uint64_t record_count = 0;
    std::uint64_t records_bytes = 0;
    std::uint64_t tail_bytes = 0;

    friend bool operator==(const StubHeader&, const StubHeader&) = default;
};

[[nodiscard]] std::array<std::byte, kStubHeaderSize> encode_stub_header(
    const StubHeader& header) noexcept;

// Writes a store file.
//
// Durability: class B. finish() returns after the file and the rename that publishes
// it have been flushed to the device. A crash before that leaves any earlier file at
// `path` untouched, plus at most a stale `<path>.partial`, which the next writer
// overwrites. The steps, in order, are commented in finish() (M1, M2, M4, M8).
class StubWriter {
public:
    [[nodiscard]] static std::expected<StubWriter, StoreError> create(
        const std::filesystem::path& path);

    StubWriter(const StubWriter&) = delete;
    StubWriter& operator=(const StubWriter&) = delete;
    StubWriter(StubWriter&& other) noexcept;
    StubWriter& operator=(StubWriter&& other) = delete;

    // An unfinished writer removes its partial file, so `path` never changes.
    ~StubWriter();

    // A Message is encoded here, once. Refuses a RawMessage with no bytes, which has
    // no valid frame. A failed append or finish is fatal to the writer: any later call
    // fails too, and nothing is published (M3).
    [[nodiscard]] std::expected<void, StoreError> append(const itch::Message& message);

    // Stores `tail` verbatim and publishes the file.
    [[nodiscard]] std::expected<void, StoreError> finish(std::span<const std::byte> tail);

private:
    StubWriter(int fd, std::filesystem::path path, std::filesystem::path partial_path);

    [[nodiscard]] std::expected<void, StoreError> flush();
    [[nodiscard]] std::expected<void, StoreError> commit(std::span<const std::byte> tail);
    void abandon() noexcept;

    int fd_ = -1;
    std::filesystem::path path_;
    std::filesystem::path partial_path_;
    std::vector<std::byte> buffer_;
    std::size_t buffered_ = 0;
    std::optional<StoreError> failed_;
    StubHeader header_;
};

// Parses a store file held in memory. Everything comes through checked readers, and a
// bad file gives a StoreError rather than a short success (D9).
class StoreParser {
public:
    // Checks length, magic, version, header size and section sizes, in that order.
    [[nodiscard]] static std::expected<StoreParser, StoreError> open(
        std::span<const std::byte> file) noexcept;

    // The next record, or nullopt after the last. Once it has reported an error or
    // the end, it keeps reporting the same thing.
    [[nodiscard]] std::expected<std::optional<itch::Message>, StoreError> next() noexcept;

    [[nodiscard]] const StubHeader& header() const noexcept { return header_; }

    // The tail section. Its size is proved at open.
    [[nodiscard]] std::span<const std::byte> tail() const noexcept { return tail_; }

private:
    StoreParser(StubHeader header, std::span<const std::byte> records,
                std::span<const std::byte> tail) noexcept
        : header_(header), cursor_(records), tail_(tail) {}

    StubHeader header_;
    itch::FrameCursor cursor_;
    std::span<const std::byte> tail_;
    std::uint64_t records_read_ = 0;
};

}  // namespace tick_store::store
