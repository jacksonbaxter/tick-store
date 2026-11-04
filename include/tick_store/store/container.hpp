#pragma once

// The store's container format: a fixed header, blocks and named sections back to
// back, a footer that describes them, and a fixed trailer that finds the footer. The
// full byte layout, the version policy and the reasons are in docs/format.md; the
// constants below are the same table in code.
//
//   [header 16][block and section bytes, in the order written][footer][trailer 24]
//
// A file is published whole, by rename, and never changed after (container_writer.hpp),
// so the footer at the end is the commit record: a file without its trailer and a
// footer whose CRC matches is incomplete, and open refuses it (F7).
//
// Every multi-byte field is little-endian and goes through little_endian.hpp (F2).
// Every size is checked against the bytes that hold it before it is used, by
// subtraction or division, so no sum can wrap (D3, D7).
//
// This file holds the parts that work on bytes in memory: the layout builder the
// writer and the tests share, and the parser the reader and the fuzzer share.
// container_writer.hpp writes a file, and container_reader.hpp maps one.

#include "tick_store/codec/registry.hpp"
#include "tick_store/store/store_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::store {

// --- Fixed parts ----------------------------------------------------------------

// The first 16 bytes of every version: magic, version, header size (F1).
inline constexpr std::array<std::byte, 8> kContainerMagic{
    std::byte{'T'}, std::byte{'I'}, std::byte{'C'}, std::byte{'K'},
    std::byte{'S'}, std::byte{'T'}, std::byte{'O'}, std::byte{'R'}};
// The header magic backwards, so a trailer cannot be mistaken for a header.
inline constexpr std::array<std::byte, 8> kTrailerMagic{
    std::byte{'R'}, std::byte{'O'}, std::byte{'T'}, std::byte{'S'},
    std::byte{'K'}, std::byte{'C'}, std::byte{'I'}, std::byte{'T'}};

// The version this code writes, and the oldest it reads. See docs/format.md for the
// policy: a change to any byte's meaning is a new version, and every version from
// kOldestReadableVersion on keeps a checked-in golden file that must still open.
inline constexpr std::uint32_t kContainerVersion = 1;
inline constexpr std::uint32_t kOldestReadableVersion = 1;

inline constexpr std::size_t kContainerHeaderSize = 16;
inline constexpr std::size_t kHeaderMagicOffset = 0;
inline constexpr std::size_t kHeaderVersionOffset = 8;
inline constexpr std::size_t kHeaderSizeOffset = 12;

// The last 24 bytes of every version.
inline constexpr std::size_t kTrailerSize = 24;
inline constexpr std::size_t kTrailerFooterOffset = 0;
inline constexpr std::size_t kTrailerFooterCrc = 8;
inline constexpr std::size_t kTrailerReserved = 12;
inline constexpr std::size_t kTrailerMagicOffset = 16;

// The footer: three counts, then the block entries, the section entries and the
// metadata area.
inline constexpr std::size_t kFooterFixedSize = 24;
inline constexpr std::size_t kFooterBlockCount = 0;
inline constexpr std::size_t kFooterSectionCount = 8;
inline constexpr std::size_t kFooterMetadataSize = 16;

inline constexpr std::size_t kBlockEntrySize = 32;
inline constexpr std::size_t kBlockPayloadOffset = 0;
inline constexpr std::size_t kBlockMetadataOffset = 8;
inline constexpr std::size_t kBlockPayloadSize = 16;
inline constexpr std::size_t kBlockMetadataSize = 20;
inline constexpr std::size_t kBlockCrc = 24;
inline constexpr std::size_t kBlockCodec = 28;
inline constexpr std::size_t kBlockReserved = 30;

inline constexpr std::size_t kSectionEntrySize = 64;
inline constexpr std::size_t kSectionOffset = 0;
inline constexpr std::size_t kSectionSize = 8;
inline constexpr std::size_t kSectionCrc = 16;
inline constexpr std::size_t kSectionNameLength = 20;
inline constexpr std::size_t kSectionName = 21;
// A name is 1 to 43 bytes of a-z, 0-9, '_', '.' and '-', zero-padded to the entry's end.
inline constexpr std::size_t kMaxSectionName = kSectionEntrySize - kSectionName;

// The smallest file: a header, an empty footer and a trailer.
inline constexpr std::size_t kMinContainerSize =
    kContainerHeaderSize + kFooterFixedSize + kTrailerSize;

static_assert(kHeaderSizeOffset + 4 == kContainerHeaderSize);
static_assert(kTrailerMagicOffset + kTrailerMagic.size() == kTrailerSize);
static_assert(kFooterMetadataSize + 8 == kFooterFixedSize);
static_assert(kBlockReserved + 2 == kBlockEntrySize);
static_assert(kSectionName + kMaxSectionName == kSectionEntrySize);

// True if `name` may name a section.
[[nodiscard]] bool valid_section_name(std::string_view name) noexcept;

// The bytes a block's CRC covers are its codec id, as two little-endian bytes, then
// its payload, so a block cannot be read under the wrong codec unnoticed (F3).
[[nodiscard]] std::uint32_t block_crc(codec::CodecId codec,
                                      std::span<const std::byte> payload) noexcept;

[[nodiscard]] std::array<std::byte, kContainerHeaderSize> encode_container_header() noexcept;

// --- Building ------------------------------------------------------------------

// The footer of a file being built: what each block and section is and where it sits.
// The writer feeds it as it writes, and build_container() feeds it in memory, so a
// file has one encoding wherever it is made. Every add checks its arguments and
// changes nothing on a refusal.
class ContainerLayout {
public:
    // Records a block whose payload goes next in the file, and returns its index.
    // Refuses a codec a writer may not use, and a payload or metadata over 2^32 - 1
    // bytes.
    [[nodiscard]] std::expected<std::uint64_t, StoreError> add_block(
        codec::CodecId codec, std::span<const std::byte> metadata,
        std::span<const std::byte> payload);

    // Records a section whose bytes go next in the file. Refuses an invalid or
    // repeated name.
    [[nodiscard]] std::expected<void, StoreError> add_section(std::string_view name,
                                                              std::span<const std::byte> bytes);

    // Where the next block or section starts, which is where the footer goes once
    // nothing more is added.
    [[nodiscard]] std::uint64_t data_end() const noexcept { return data_end_; }

    [[nodiscard]] std::uint64_t block_count() const noexcept;

    // The footer and the trailer, for a footer at data_end().
    [[nodiscard]] std::vector<std::byte> encode_footer_and_trailer() const;

private:
    std::uint64_t data_end_ = kContainerHeaderSize;
    std::vector<std::byte> block_entries_;
    std::vector<std::byte> section_entries_;
    std::vector<std::byte> metadata_;
    std::vector<std::string> section_names_;
};

// One block or section, for build_container().
struct ContainerPart {
    // A block if `section_name` is empty, otherwise a section.
    std::string section_name;
    codec::CodecId codec = codec::CodecId::raw;
    std::vector<std::byte> metadata;
    std::vector<std::byte> bytes;
};

// A whole file in memory, parts in the order given. The writer's bytes for the same
// parts are identical.
[[nodiscard]] std::expected<std::vector<std::byte>, StoreError> build_container(
    std::span<const ContainerPart> parts);

// --- Parsing -------------------------------------------------------------------

// Whether CRCs are checked. verify is the default everywhere. skip exists for the
// fuzzer, which must reach the structural checks below the CRCs (Z1), and for a
// caller that has verified the file once already; it must be named to be had (F3).
enum class Checksums : std::uint8_t {
    verify,
    skip,
};

struct ReadOptions {
    Checksums checksums = Checksums::verify;
};

// What the directory says about a block, read from the footer alone, so a zone-map
// scan never touches a payload page.
struct BlockInfo {
    codec::CodecId codec = codec::CodecId::none;
    std::uint64_t payload_offset = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t crc = 0;
    std::span<const std::byte> metadata;
};

// A block's bytes. From ContainerParser the spans borrow the parser's input. From
// ContainerReader `owner` holds the mapping, so the spans stay valid for as long as
// the view does, even after the reader is gone (A3).
struct BlockView {
    codec::CodecId codec = codec::CodecId::none;
    std::span<const std::byte> metadata;
    std::span<const std::byte> payload;
    std::shared_ptr<const void> owner;
};

struct SectionInfo {
    std::string_view name;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
};

// A section's bytes, with the same lifetime rule as BlockView.
struct SectionView {
    std::string_view name;
    std::span<const std::byte> bytes;
    std::shared_ptr<const void> owner;
};

// Parses a whole container held in memory. open() checks everything but the block and
// section CRCs: the header, the trailer, the footer's CRC, its counts and every entry,
// and that the blocks and sections tile the data region exactly. A block's or a
// section's CRC is checked when it is looked up, so opening a file reads only its
// ends. verify_all() checks them all at once.
//
// Over-read contract (A2): nothing here reads outside the span it was given, and a
// view's span holds exactly the payload, with no padding guaranteed after it. A
// decoder handed a payload must stay inside it.
//
// Lifetime: the parser and every view it returns borrow the input span. Keep the bytes
// alive for as long as either is in use.
class ContainerParser {
public:
    [[nodiscard]] static std::expected<ContainerParser, StoreError> open(
        std::span<const std::byte> file, ReadOptions options = {});

    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    [[nodiscard]] std::uint64_t block_count() const noexcept { return block_count_; }
    [[nodiscard]] std::uint64_t section_count() const noexcept { return section_count_; }
    [[nodiscard]] std::uint64_t footer_offset() const noexcept { return footer_offset_; }

    [[nodiscard]] std::expected<BlockInfo, StoreError> block_info(std::uint64_t index) const;

    // The block's bytes, CRC checked unless the options said skip.
    [[nodiscard]] std::expected<BlockView, StoreError> block(std::uint64_t index) const;

    // Sections in file order.
    [[nodiscard]] std::expected<SectionInfo, StoreError> section_info(std::uint64_t index) const;

    // The section of that name, CRC checked unless the options said skip, or not_found.
    [[nodiscard]] std::expected<SectionView, StoreError> section(std::string_view name) const;

    // Checks every block's and section's CRC, whatever the options said.
    [[nodiscard]] std::expected<void, StoreError> verify_all() const;

private:
    ContainerParser() = default;

    [[nodiscard]] std::expected<SectionView, StoreError> section_at(std::uint64_t index,
                                                                    Checksums checksums) const;
    [[nodiscard]] std::expected<BlockView, StoreError> block_with(std::uint64_t index,
                                                                  Checksums checksums) const;

    std::span<const std::byte> file_;
    ReadOptions options_;
    std::uint32_t version_ = 0;
    std::uint64_t footer_offset_ = 0;
    std::uint64_t block_count_ = 0;
    std::uint64_t section_count_ = 0;
    std::span<const std::byte> block_entries_;
    std::span<const std::byte> section_entries_;
    std::span<const std::byte> metadata_;
    // Section indexes sorted by name, for lookup by binary search.
    std::vector<std::uint64_t> sections_by_name_;
};

}  // namespace tick_store::store
