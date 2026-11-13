#include "tick_store/store/container.hpp"

#include "tick_store/codec/registry.hpp"
#include "tick_store/store/crc32c.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

constexpr std::uint64_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();

[[gnu::cold, gnu::noinline]] StoreError fail(StoreErrorKind kind, std::string_view field,
                                             std::uint64_t offset, std::uint64_t expected,
                                             std::uint64_t actual) noexcept {
    return {.kind = kind,
            .field = field,
            .offset = offset,
            .expected = expected,
            .actual = actual};
}

// A field the caller has already proved lies inside `bytes`, so value_or never picks
// its argument, and the checked reader stays the only way to the bytes (D1).
template <std::size_t Width>
[[nodiscard]] std::uint64_t field(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return read_le<Width>(bytes, offset).value_or(0);
}

[[nodiscard]] std::uint64_t magic_value(std::span<const std::byte> bytes) noexcept {
    return read_le<8>(bytes, 0).value_or(0);
}

[[nodiscard]] constexpr bool name_character(std::byte value) noexcept {
    const auto c = std::to_integer<unsigned char>(value);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

[[nodiscard]] std::string_view as_chars(std::span<const std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): viewing bytes as chars is allowed.
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// `bytes` from `offset` for `size`, or nullopt if that leaves the span. Compared by
// subtraction, so nothing wraps.
[[nodiscard]] std::optional<std::span<const std::byte>> checked_subspan(
    std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t size) noexcept {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        return std::nullopt;
    }
    return bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}

template <std::size_t Width>
void append_le(std::vector<std::byte>& out, std::uint64_t value) {
    std::array<std::byte, Width> bytes{};
    // A Width-byte write into a Width-byte array always fits.
    std::ignore = write_le<Width>(bytes, 0, value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace

bool valid_section_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= kMaxSectionName &&
           std::ranges::all_of(name, [](char c) { return name_character(std::byte(c)); });
}

std::uint32_t block_crc(codec::CodecId codec, std::span<const std::byte> payload) noexcept {
    std::array<std::byte, 2> id{};
    std::ignore = write_le<2>(id, 0, static_cast<std::uint16_t>(codec));
    return crc32c(payload, crc32c(id));
}

std::array<std::byte, kContainerHeaderSize> encode_container_header() noexcept {
    std::array<std::byte, kContainerHeaderSize> header{};
    std::ranges::copy(kContainerMagic, header.begin() + kHeaderMagicOffset);
    // Every offset is inside the array, as the static_asserts in the header prove.
    std::ignore = write_le<4>(header, kHeaderVersionOffset, kContainerVersion);
    std::ignore = write_le<4>(header, kHeaderSizeOffset, kContainerHeaderSize);
    return header;
}

// --- ContainerLayout --------------------------------------------------------------

std::uint64_t ContainerLayout::block_count() const noexcept {
    return block_entries_.size() / kBlockEntrySize;
}

std::expected<std::uint64_t, StoreError> ContainerLayout::add_block(
    codec::CodecId codec, std::span<const std::byte> metadata,
    std::span<const std::byte> payload) {
    const auto id = static_cast<std::uint16_t>(codec);
    if (!codec::codec_writable(id)) {
        return std::unexpected(
            StoreError{.kind = StoreErrorKind::invalid_argument, .field = "codec id", .actual = id});
    }
    if (payload.size() > kMaxU32) {
        return std::unexpected(
            fail(StoreErrorKind::invalid_argument, "payload size", 0, kMaxU32, payload.size()));
    }
    if (metadata.size() > kMaxU32) {
        return std::unexpected(fail(StoreErrorKind::invalid_argument, "metadata size", 0,
                                    kMaxU32, metadata.size()));
    }
    if (payload.size() > kMaxU64 - data_end_) {
        return std::unexpected(fail(StoreErrorKind::invalid_argument, "file size", 0,
                                    kMaxU64 - data_end_, payload.size()));
    }

    const std::uint64_t index = block_count();
    append_le<8>(block_entries_, data_end_);
    append_le<8>(block_entries_, metadata_.size());
    append_le<4>(block_entries_, payload.size());
    append_le<4>(block_entries_, metadata.size());
    append_le<4>(block_entries_, block_crc(codec, payload));
    append_le<2>(block_entries_, id);
    append_le<2>(block_entries_, 0);
    metadata_.insert(metadata_.end(), metadata.begin(), metadata.end());
    data_end_ += payload.size();
    return index;
}

std::expected<void, StoreError> ContainerLayout::add_section(std::string_view name,
                                                             std::span<const std::byte> bytes) {
    if (!valid_section_name(name)) {
        return std::unexpected(fail(StoreErrorKind::invalid_argument, "section name", 0,
                                    kMaxSectionName, name.size()));
    }
    // A linear search: a file holds a handful of sections, and the reader's lookup is
    // the one that has to be fast.
    if (std::ranges::find(section_names_, name) != section_names_.end()) {
        return std::unexpected(
            StoreError{.kind = StoreErrorKind::invalid_argument, .field = "section name repeated"});
    }
    if (bytes.size() > kMaxU64 - data_end_) {
        return std::unexpected(fail(StoreErrorKind::invalid_argument, "file size", 0,
                                    kMaxU64 - data_end_, bytes.size()));
    }

    append_le<8>(section_entries_, data_end_);
    append_le<8>(section_entries_, bytes.size());
    append_le<4>(section_entries_, crc32c(bytes));
    append_le<1>(section_entries_, name.size());
    for (const char c : name) {
        section_entries_.push_back(static_cast<std::byte>(c));
    }
    section_entries_.resize(section_entries_.size() + kMaxSectionName - name.size());
    section_names_.emplace_back(name);
    data_end_ += bytes.size();
    return {};
}

std::vector<std::byte> ContainerLayout::encode_footer_and_trailer() const {
    std::vector<std::byte> out;
    out.reserve(kFooterFixedSize + block_entries_.size() + section_entries_.size() +
                metadata_.size() + kTrailerSize);
    append_le<8>(out, block_count());
    append_le<8>(out, section_entries_.size() / kSectionEntrySize);
    append_le<8>(out, metadata_.size());
    out.insert(out.end(), block_entries_.begin(), block_entries_.end());
    out.insert(out.end(), section_entries_.begin(), section_entries_.end());
    out.insert(out.end(), metadata_.begin(), metadata_.end());

    const std::uint32_t footer_crc = crc32c(out);
    append_le<8>(out, data_end_);
    append_le<4>(out, footer_crc);
    append_le<4>(out, 0);
    out.insert(out.end(), kTrailerMagic.begin(), kTrailerMagic.end());
    return out;
}

std::expected<std::vector<std::byte>, StoreError> build_container(
    std::span<const ContainerPart> parts) {
    const auto header = encode_container_header();
    std::vector<std::byte> out(header.begin(), header.end());
    ContainerLayout layout;
    for (const ContainerPart& part : parts) {
        if (part.section_name.empty()) {
            if (auto added = layout.add_block(part.codec, part.metadata, part.bytes);
                !added.has_value()) {
                return std::unexpected(added.error());
            }
        } else if (auto added = layout.add_section(part.section_name, part.bytes);
                   !added.has_value()) {
            return std::unexpected(added.error());
        }
        out.insert(out.end(), part.bytes.begin(), part.bytes.end());
    }
    const auto tail = layout.encode_footer_and_trailer();
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

// --- ContainerParser --------------------------------------------------------------

std::expected<ContainerParser, StoreError> ContainerParser::open(std::span<const std::byte> file,
                                                                 ReadOptions options) {
    // Length, magic and version, in that order, then the header size (F1).
    const std::uint64_t size = file.size();
    if (size < kContainerHeaderSize) {
        return std::unexpected(
            fail(StoreErrorKind::file_too_small, "file", 0, kContainerHeaderSize, size));
    }
    if (!std::ranges::equal(file.first(kContainerMagic.size()), kContainerMagic)) {
        return std::unexpected(fail(StoreErrorKind::bad_magic, "magic", kHeaderMagicOffset,
                                    magic_value(kContainerMagic), magic_value(file)));
    }
    const std::uint64_t version = field<4>(file, kHeaderVersionOffset);
    if (version < kOldestReadableVersion || version > kContainerVersion) {
        return std::unexpected(fail(StoreErrorKind::unsupported_version, "version",
                                    kHeaderVersionOffset, kContainerVersion, version));
    }
    // Version 1 is the only one, so its header size is the only one.
    const std::uint64_t header_size = field<4>(file, kHeaderSizeOffset);
    if (header_size != kContainerHeaderSize) {
        return std::unexpected(fail(StoreErrorKind::bad_header_size, "header size",
                                    kHeaderSizeOffset, kContainerHeaderSize, header_size));
    }

    // The trailer: present, whole and pointing inside the file (F7, M5).
    if (size < kMinContainerSize) {
        return std::unexpected(
            fail(StoreErrorKind::file_too_small, "file", 0, kMinContainerSize, size));
    }
    const std::uint64_t trailer_start = size - kTrailerSize;
    const auto trailer = file.last(kTrailerSize);
    if (!std::ranges::equal(trailer.subspan(kTrailerMagicOffset), kTrailerMagic)) {
        return std::unexpected(fail(StoreErrorKind::bad_magic, "trailer magic",
                                    trailer_start + kTrailerMagicOffset,
                                    magic_value(kTrailerMagic),
                                    magic_value(trailer.subspan(kTrailerMagicOffset))));
    }
    const std::uint64_t reserved = field<4>(trailer, kTrailerReserved);
    if (reserved != 0) {
        return std::unexpected(fail(StoreErrorKind::bad_field, "trailer reserved",
                                    trailer_start + kTrailerReserved, 0, reserved));
    }
    const std::uint64_t footer_offset = field<8>(trailer, kTrailerFooterOffset);
    const std::uint64_t last_footer_offset = trailer_start - kFooterFixedSize;
    if (footer_offset < kContainerHeaderSize) {
        return std::unexpected(fail(StoreErrorKind::bad_footer, "footer offset",
                                    trailer_start + kTrailerFooterOffset, kContainerHeaderSize,
                                    footer_offset));
    }
    if (footer_offset > last_footer_offset) {
        return std::unexpected(fail(StoreErrorKind::bad_footer, "footer offset",
                                    trailer_start + kTrailerFooterOffset, last_footer_offset,
                                    footer_offset));
    }
    const auto footer = file.subspan(static_cast<std::size_t>(footer_offset),
                                     static_cast<std::size_t>(trailer_start - footer_offset));
    if (options.checksums == Checksums::verify) {
        const std::uint64_t stored = field<4>(trailer, kTrailerFooterCrc);
        const std::uint32_t computed = crc32c(footer);
        if (stored != computed) {
            return std::unexpected(
                fail(StoreErrorKind::bad_checksum, "footer", footer_offset, stored, computed));
        }
    }

    // The counts must fill the footer exactly. Each is compared with the room left by
    // division, so no product or sum can wrap (D7).
    const std::uint64_t blocks = field<8>(footer, kFooterBlockCount);
    const std::uint64_t sections = field<8>(footer, kFooterSectionCount);
    const std::uint64_t metadata_size = field<8>(footer, kFooterMetadataSize);
    std::uint64_t room = footer.size() - kFooterFixedSize;
    if (blocks > room / kBlockEntrySize) {
        return std::unexpected(fail(StoreErrorKind::bad_footer, "block count",
                                    footer_offset + kFooterBlockCount, room / kBlockEntrySize,
                                    blocks));
    }
    room -= blocks * kBlockEntrySize;
    if (sections > room / kSectionEntrySize) {
        return std::unexpected(fail(StoreErrorKind::bad_footer, "section count",
                                    footer_offset + kFooterSectionCount,
                                    room / kSectionEntrySize, sections));
    }
    room -= sections * kSectionEntrySize;
    if (metadata_size != room) {
        return std::unexpected(fail(StoreErrorKind::bad_footer, "metadata size",
                                    footer_offset + kFooterMetadataSize, room, metadata_size));
    }

    ContainerParser parser;
    parser.file_ = file;
    parser.options_ = options;
    parser.version_ = static_cast<std::uint32_t>(version);
    parser.footer_offset_ = footer_offset;
    parser.block_count_ = blocks;
    parser.section_count_ = sections;
    const auto entries = footer.subspan(kFooterFixedSize);
    parser.block_entries_ = entries.first(static_cast<std::size_t>(blocks * kBlockEntrySize));
    parser.section_entries_ = entries.subspan(parser.block_entries_.size(),
                                              static_cast<std::size_t>(sections * kSectionEntrySize));
    parser.metadata_ = entries.subspan(parser.block_entries_.size() + parser.section_entries_.size());

    // Blocks and sections, each list in file order, must tile the data region from the
    // header to the footer with no gap and no overlap, and blocks' metadata must tile
    // the metadata area the same way. One walk takes whichever entry starts at the
    // cursor. An empty section there goes first, so a block and an empty section that
    // share an offset are taken the same way whatever order wrote them.
    const std::uint64_t block_entries_at = footer_offset + kFooterFixedSize;
    const std::uint64_t section_entries_at = block_entries_at + parser.block_entries_.size();
    std::uint64_t cursor = kContainerHeaderSize;
    std::uint64_t metadata_cursor = 0;
    std::uint64_t block = 0;
    std::uint64_t section = 0;
    while (block < blocks || section < sections) {
        const auto block_entry =
            block < blocks ? parser.block_entries_.subspan(
                                 static_cast<std::size_t>(block * kBlockEntrySize), kBlockEntrySize)
                           : std::span<const std::byte>{};
        const auto section_entry =
            section < sections
                ? parser.section_entries_.subspan(
                      static_cast<std::size_t>(section * kSectionEntrySize), kSectionEntrySize)
                : std::span<const std::byte>{};
        const bool block_here =
            block < blocks && field<8>(block_entry, kBlockPayloadOffset) == cursor;
        const bool section_here =
            section < sections && field<8>(section_entry, kSectionOffset) == cursor;
        const bool empty_section_here =
            section_here && field<8>(section_entry, kSectionSize) == 0;

        if (block_here && !empty_section_here) {
            const std::uint64_t at = block_entries_at + (block * kBlockEntrySize);
            const std::uint64_t entry_reserved = field<2>(block_entry, kBlockReserved);
            if (entry_reserved != 0) {
                return std::unexpected(fail(StoreErrorKind::bad_field, "block reserved",
                                            at + kBlockReserved, 0, entry_reserved));
            }
            const std::uint64_t codec_id = field<2>(block_entry, kBlockCodec);
            if (!codec::codec_readable(static_cast<std::uint16_t>(codec_id))) {
                return std::unexpected(StoreError{.kind = StoreErrorKind::unknown_codec,
                                                  .field = "codec id",
                                                  .offset = at + kBlockCodec,
                                                  .actual = codec_id});
            }
            const std::uint64_t metadata_offset = field<8>(block_entry, kBlockMetadataOffset);
            if (metadata_offset != metadata_cursor) {
                return std::unexpected(fail(StoreErrorKind::bad_extent, "block metadata offset",
                                            at + kBlockMetadataOffset, metadata_cursor,
                                            metadata_offset));
            }
            const std::uint64_t block_metadata = field<4>(block_entry, kBlockMetadataSize);
            if (block_metadata > metadata_size - metadata_cursor) {
                return std::unexpected(fail(StoreErrorKind::bad_extent, "block metadata size",
                                            at + kBlockMetadataSize,
                                            metadata_size - metadata_cursor, block_metadata));
            }
            const std::uint64_t payload = field<4>(block_entry, kBlockPayloadSize);
            if (payload > footer_offset - cursor) {
                return std::unexpected(fail(StoreErrorKind::bad_extent, "block size",
                                            at + kBlockPayloadSize, footer_offset - cursor,
                                            payload));
            }
            cursor += payload;
            metadata_cursor += block_metadata;
            ++block;
        } else if (section_here) {
            const std::uint64_t at = section_entries_at + (section * kSectionEntrySize);
            const std::uint64_t name_length = field<1>(section_entry, kSectionNameLength);
            if (name_length == 0 || name_length > kMaxSectionName) {
                return std::unexpected(fail(StoreErrorKind::bad_field, "section name length",
                                            at + kSectionNameLength, kMaxSectionName,
                                            name_length));
            }
            const auto name = section_entry.subspan(kSectionName);
            for (std::size_t i = 0; i < name.size(); ++i) {
                const auto value = std::to_integer<std::uint64_t>(name[i]);
                if (i < name_length && !name_character(name[i])) {
                    // No one value is expected: any character of the alphabet.
                    return std::unexpected(StoreError{.kind = StoreErrorKind::bad_field,
                                                      .field = "section name",
                                                      .offset = at + kSectionName + i,
                                                      .actual = value});
                }
                if (i >= name_length && value != 0) {
                    return std::unexpected(fail(StoreErrorKind::bad_field,
                                                "section name padding", at + kSectionName + i, 0,
                                                value));
                }
            }
            const std::uint64_t bytes = field<8>(section_entry, kSectionSize);
            if (bytes > footer_offset - cursor) {
                return std::unexpected(fail(StoreErrorKind::bad_extent, "section size",
                                            at + kSectionSize, footer_offset - cursor, bytes));
            }
            cursor += bytes;
            ++section;
        } else if (block < blocks) {
            return std::unexpected(fail(StoreErrorKind::bad_extent, "block offset",
                                        block_entries_at + (block * kBlockEntrySize), cursor,
                                        field<8>(block_entry, kBlockPayloadOffset)));
        } else {
            return std::unexpected(fail(StoreErrorKind::bad_extent, "section offset",
                                        section_entries_at + (section * kSectionEntrySize),
                                        cursor, field<8>(section_entry, kSectionOffset)));
        }
    }
    if (cursor != footer_offset) {
        return std::unexpected(fail(StoreErrorKind::bad_extent, "footer offset",
                                    trailer_start + kTrailerFooterOffset, cursor, footer_offset));
    }
    if (metadata_cursor != metadata_size) {
        return std::unexpected(fail(StoreErrorKind::bad_extent, "metadata area",
                                    footer_offset + kFooterMetadataSize, metadata_cursor,
                                    metadata_size));
    }

    // Names are unique. Sorting an index by name, held for lookups, shows a repeat as
    // two neighbours. The index is bounded by the footer: one entry per 64 bytes (D8).
    parser.sections_by_name_.resize(static_cast<std::size_t>(sections));
    std::ranges::iota(parser.sections_by_name_, std::uint64_t{0});
    const auto name_of = [&parser](std::uint64_t index) {
        const auto entry = parser.section_entries_.subspan(
            static_cast<std::size_t>(index * kSectionEntrySize), kSectionEntrySize);
        return as_chars(entry.subspan(kSectionName,
                                      static_cast<std::size_t>(field<1>(entry, kSectionNameLength))));
    };
    std::ranges::stable_sort(parser.sections_by_name_, {}, name_of);
    const auto repeat = std::ranges::adjacent_find(
        parser.sections_by_name_,
        [&](std::uint64_t a, std::uint64_t b) { return name_of(a) == name_of(b); });
    if (repeat != parser.sections_by_name_.end()) {
        const std::uint64_t later = *std::next(repeat);
        return std::unexpected(fail(StoreErrorKind::bad_field, "section name repeated",
                                    section_entries_at + (later * kSectionEntrySize) + kSectionName,
                                    *repeat, later));
    }
    return parser;
}

std::expected<BlockInfo, StoreError> ContainerParser::block_info(std::uint64_t index) const {
    if (index >= block_count_) {
        return std::unexpected(
            fail(StoreErrorKind::not_found, "block index", 0, block_count_, index));
    }
    const auto entry = block_entries_.subspan(static_cast<std::size_t>(index * kBlockEntrySize),
                                              kBlockEntrySize);
    // open() proved every extent. These checks hold the line if the bytes changed
    // since, which a file published by rename never does (docs/format.md, SIGBUS).
    const auto metadata = checked_subspan(metadata_, field<8>(entry, kBlockMetadataOffset),
                                          field<4>(entry, kBlockMetadataSize));
    if (!metadata.has_value()) {
        return std::unexpected(StoreError{
            .kind = StoreErrorKind::bad_extent,
            .field = "block metadata offset",
            .offset = footer_offset_ + kFooterFixedSize + (index * kBlockEntrySize),
            .actual = field<8>(entry, kBlockMetadataOffset)});
    }
    return BlockInfo{
        .codec = static_cast<codec::CodecId>(field<2>(entry, kBlockCodec)),
        .payload_offset = field<8>(entry, kBlockPayloadOffset),
        .payload_size = static_cast<std::uint32_t>(field<4>(entry, kBlockPayloadSize)),
        .crc = static_cast<std::uint32_t>(field<4>(entry, kBlockCrc)),
        .metadata = *metadata,
    };
}

std::expected<BlockView, StoreError> ContainerParser::block_with(std::uint64_t index,
                                                                 Checksums checksums) const {
    const auto info = block_info(index);
    if (!info.has_value()) {
        return std::unexpected(info.error());
    }
    const auto payload = checked_subspan(file_.first(static_cast<std::size_t>(footer_offset_)),
                                         info->payload_offset, info->payload_size);
    if (!payload.has_value()) {
        return std::unexpected(StoreError{
            .kind = StoreErrorKind::bad_extent,
            .field = "block offset",
            .offset = footer_offset_ + kFooterFixedSize + (index * kBlockEntrySize),
            .actual = info->payload_offset});
    }
    if (checksums == Checksums::verify) {
        const std::uint32_t computed = block_crc(info->codec, *payload);
        if (computed != info->crc) {
            return std::unexpected(fail(StoreErrorKind::bad_checksum, "block",
                                        info->payload_offset, info->crc, computed));
        }
    }
    return BlockView{.codec = info->codec, .metadata = info->metadata, .payload = *payload, .owner = {}};
}

std::expected<BlockView, StoreError> ContainerParser::block(std::uint64_t index) const {
    return block_with(index, options_.checksums);
}

std::expected<SectionInfo, StoreError> ContainerParser::section_info(std::uint64_t index) const {
    if (index >= section_count_) {
        return std::unexpected(
            fail(StoreErrorKind::not_found, "section index", 0, section_count_, index));
    }
    const auto entry = section_entries_.subspan(
        static_cast<std::size_t>(index * kSectionEntrySize), kSectionEntrySize);
    const auto name_length = static_cast<std::size_t>(field<1>(entry, kSectionNameLength));
    return SectionInfo{
        .name = as_chars(entry.subspan(kSectionName, std::min(name_length, kMaxSectionName))),
        .offset = field<8>(entry, kSectionOffset),
        .size = field<8>(entry, kSectionSize),
        .crc = static_cast<std::uint32_t>(field<4>(entry, kSectionCrc)),
    };
}

std::expected<SectionView, StoreError> ContainerParser::section_at(std::uint64_t index,
                                                                   Checksums checksums) const {
    const auto info = section_info(index);
    if (!info.has_value()) {
        return std::unexpected(info.error());
    }
    const auto bytes = checked_subspan(file_.first(static_cast<std::size_t>(footer_offset_)),
                                       info->offset, info->size);
    if (!bytes.has_value()) {
        return std::unexpected(fail(StoreErrorKind::bad_extent, "section offset",
                                    footer_offset_ + kFooterFixedSize +
                                        (block_count_ * kBlockEntrySize) +
                                        (index * kSectionEntrySize),
                                    0, info->offset));
    }
    if (checksums == Checksums::verify) {
        const std::uint32_t computed = crc32c(*bytes);
        if (computed != info->crc) {
            return std::unexpected(
                fail(StoreErrorKind::bad_checksum, "section", info->offset, info->crc, computed));
        }
    }
    return SectionView{.name = info->name, .bytes = *bytes, .owner = {}};
}

std::expected<SectionView, StoreError> ContainerParser::section(std::string_view name) const {
    const auto found = std::ranges::lower_bound(
        sections_by_name_, name, {}, [this](std::uint64_t index) {
            return section_info(index).value_or(SectionInfo{}).name;
        });
    if (found == sections_by_name_.end() ||
        section_info(*found).value_or(SectionInfo{}).name != name) {
        return std::unexpected(StoreError{.kind = StoreErrorKind::not_found, .field = "section"});
    }
    return section_at(*found, options_.checksums);
}

std::expected<void, StoreError> ContainerParser::verify_all() const {
    for (std::uint64_t index = 0; index < block_count_; ++index) {
        if (auto view = block_with(index, Checksums::verify); !view.has_value()) {
            return std::unexpected(view.error());
        }
    }
    for (std::uint64_t index = 0; index < section_count_; ++index) {
        if (auto view = section_at(index, Checksums::verify); !view.has_value()) {
            return std::unexpected(view.error());
        }
    }
    return {};
}

}  // namespace tick_store::store
