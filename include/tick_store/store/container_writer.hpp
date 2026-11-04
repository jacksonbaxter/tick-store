#pragma once

// Writes a container file (container.hpp) and publishes it whole, in a named
// durability mode (durability.hpp).
//
// The file is written to `<path>.partial` and renamed to `path` by commit(). Nothing
// is visible at `path` until then, and a file once published is never written again,
// so a reader's mapping never sees it change (docs/format.md, SIGBUS stance). The
// steps of commit(), in order, with what each guarantees:
//
//   1. The footer and the trailer, the commit record, are written after every block
//      and section, so the partial file is incomplete until its last byte (F7).
//   2. device_flush only: the whole file is flushed to the device, before anything
//      can name it (M1). F_FULLFSYNC on macOS, fdatasync on Linux.
//   3. close, checked, since some file systems report a late write error there.
//   4. rename to `path`: the commit point, atomic against readers and crashes (M2).
//   5. device_flush only: the directory is flushed, so the rename itself survives a
//      crash.
//
// Flush granularity: one flush per file, at commit. Every write number states it.
//
// Failure is fatal (M3). Any I/O error, a failed flush above all, is returned once,
// never retried, and leaves the writer failed: every later call returns the same
// error, and the partial file is removed. A failure before step 4 leaves `path` as it
// was. A failure at step 5 leaves the new file at `path`, maybe not durable; commit()
// returns the error, and the caller must not count the file as durable. A refused
// argument (an unknown codec, a bad section name) changes nothing and is not fatal.
//
// A crash anywhere leaves `path` holding the old file or, after step 4, the new one;
// at most a stale `<path>.partial` stays behind, which the next writer for `path`
// overwrites. One writer per path at a time.

#include "tick_store/codec/registry.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::store {

class ContainerWriter {
public:
    // Creates `<path>.partial` and writes the header. The durability mode is chosen
    // here, once, so it cannot be left to a default by accident.
    [[nodiscard]] static std::expected<ContainerWriter, StoreError> create(
        const std::filesystem::path& path, Durability durability,
        FileOps& ops = posix_file_ops());

    ContainerWriter(const ContainerWriter&) = delete;
    ContainerWriter& operator=(const ContainerWriter&) = delete;
    ContainerWriter(ContainerWriter&& other) noexcept;
    ContainerWriter& operator=(ContainerWriter&&) = delete;

    // An uncommitted writer closes and removes its partial file, so `path` never
    // changes.
    ~ContainerWriter();

    // Appends a block and returns its index. `metadata` is the store's own bytes for
    // the block, zone maps for instance, kept in the footer so that a scan of them
    // never touches a payload.
    [[nodiscard]] std::expected<std::uint64_t, StoreError> add_block(
        codec::CodecId codec, std::span<const std::byte> metadata,
        std::span<const std::byte> payload);

    // Appends a named section: an index, a dictionary, a book checkpoint.
    [[nodiscard]] std::expected<void, StoreError> add_section(std::string_view name,
                                                              std::span<const std::byte> bytes);

    // Publishes the file, in the steps above.
    [[nodiscard]] std::expected<void, StoreError> commit();

    [[nodiscard]] Durability durability() const noexcept { return durability_; }

private:
    ContainerWriter(FileOps& ops, int fd, std::filesystem::path path,
                    std::filesystem::path partial_path, Durability durability);

    [[nodiscard]] std::expected<void, StoreError> append(std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<void, StoreError> flush();
    [[nodiscard]] std::expected<void, StoreError> publish();
    [[nodiscard]] std::unexpected<StoreError> fail(StoreError error);
    void abandon() noexcept;

    FileOps* ops_;
    int fd_ = -1;
    std::filesystem::path path_;
    std::filesystem::path partial_path_;
    Durability durability_;
    ContainerLayout layout_;
    std::vector<std::byte> buffer_;
    std::size_t buffered_ = 0;
    std::optional<StoreError> failed_;
};

}  // namespace tick_store::store
