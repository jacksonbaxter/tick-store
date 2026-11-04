// The container format: the golden file both ways (F6) and as the version 1 file that
// must always open (F5), one corrupt fixture per validated field with its exact error
// (T1, D2), truncation at every length (D10, F7), the layout builder's refusals, the
// codec registry against the codecs (F4), and view lifetimes (A3).

#include "container_fixture.hpp"
#include "scratch_dir.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/registry.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/stream_vbyte.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/container_reader.hpp"
#include "tick_store/store/container_writer.hpp"
#include "tick_store/store/crc32c.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

using codec::CodecId;
using test::Bytes;
using test::golden_block_entry;
using test::golden_section_entry;
using test::kGoldenFooter;
using test::kGoldenSize;
using test::kGoldenTrailer;
using test::le_bytes;
using test::with_footer_field;
using ::tick_store::test::ScratchDir;

constexpr std::uint64_t kMax64 = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();

const Bytes& golden() {
    static const Bytes file = test::read_bytes(test::golden_path());
    return file;
}

Bytes golden_copy() {
    Bytes copy = golden();
    copy.shrink_to_fit();
    return copy;
}

std::uint64_t magic_of(std::span<const std::byte> bytes) { return read_le<8>(bytes, 0).value_or(0); }

StoreError error(StoreErrorKind kind, std::string_view field, std::uint64_t offset,
                 std::uint64_t expected, std::uint64_t actual) {
    return {.kind = kind, .field = field, .offset = offset, .expected = expected, .actual = actual};
}

std::optional<StoreError> open_error(std::span<const std::byte> file,
                                     ReadOptions options = {}) {
    const auto parser = ContainerParser::open(file, options);
    if (parser.has_value()) {
        return std::nullopt;
    }
    return parser.error();
}

// The kind of error opening gives, or io, which no parse returns, if it opens.
StoreErrorKind kind_of(std::span<const std::byte> file) {
    return open_error(file).value_or(StoreError{}).kind;
}

void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

std::uint32_t software_crc(std::span<const std::byte> bytes) {
    return crc32c_with(Crc32cKernel::software, bytes).value_or(0);
}

// The golden file typed out field by field from docs/format.md, with its own
// little-endian writer and the software CRC, sharing nothing with ContainerLayout.
Bytes hand_typed_golden() {
    const Bytes timestamps = test::golden_timestamp_block();
    const Bytes stock = test::ascii_bytes("ZVZZT   ");
    const Bytes raw{std::byte{0xAB}, std::byte{0xCD}};

    Bytes file = test::ascii_bytes("TICKSTOR");
    append(file, le_bytes(1, 4));   // version
    append(file, le_bytes(16, 4));  // header size
    append(file, timestamps);       // block 0 at 16
    append(file, stock);            // section 0 at 37
    append(file, raw);              // block 1 at 45; section 1, empty, at 47

    Bytes footer;
    append(footer, le_bytes(2, 8));   // blocks
    append(footer, le_bytes(2, 8));   // sections
    append(footer, le_bytes(18, 8));  // metadata bytes
    const auto block_entry = [&](std::uint64_t offset, std::uint64_t metadata_offset,
                                 std::uint64_t size, std::uint64_t metadata_size,
                                 std::uint16_t codec, const Bytes& payload) {
        Bytes covered = le_bytes(codec, 2);
        append(covered, payload);
        append(footer, le_bytes(offset, 8));
        append(footer, le_bytes(metadata_offset, 8));
        append(footer, le_bytes(size, 4));
        append(footer, le_bytes(metadata_size, 4));
        append(footer, le_bytes(software_crc(covered), 4));
        append(footer, le_bytes(codec, 2));
        append(footer, le_bytes(0, 2));
    };
    block_entry(16, 0, 21, 16, 2, timestamps);
    block_entry(45, 16, 2, 2, 1, raw);
    const auto section_entry = [&](std::uint64_t offset, const Bytes& bytes,
                                   std::string_view name) {
        append(footer, le_bytes(offset, 8));
        append(footer, le_bytes(bytes.size(), 8));
        append(footer, le_bytes(software_crc(bytes), 4));
        append(footer, le_bytes(name.size(), 1));
        append(footer, test::ascii_bytes(name));
        append(footer, Bytes(43 - name.size()));
    };
    section_entry(37, stock, "dict.stock");
    section_entry(47, {}, "empty");
    append(footer, test::golden_zone_map());
    append(footer, {std::byte{0x01}, std::byte{0x02}});

    const std::uint32_t footer_crc = software_crc(footer);
    append(file, footer);
    append(file, le_bytes(47, 8));  // footer offset
    append(file, le_bytes(footer_crc, 4));
    append(file, le_bytes(0, 4));  // reserved
    append(file, test::ascii_bytes("ROTSKCIT"));
    return file;
}

// --- Golden bytes both ways ---------------------------------------------------------

TEST(ContainerGolden, FileMatchesTheFormatTypedByHand) {
    ASSERT_EQ(golden().size(), kGoldenSize);
    EXPECT_EQ(golden(), hand_typed_golden());
}

TEST(ContainerGolden, WriterReproducesTheFileByteForByte) {
    for (const Durability durability : {Durability::page_cache, Durability::device_flush}) {
        SCOPED_TRACE(to_string(durability));
        const ScratchDir dir;
        auto writer = ContainerWriter::create(dir / "golden.tsc", durability);
        ASSERT_TRUE(writer.has_value());
        for (const ContainerPart& part : test::golden_parts()) {
            if (part.section_name.empty()) {
                ASSERT_TRUE(writer->add_block(part.codec, part.metadata, part.bytes).has_value());
            } else {
                ASSERT_TRUE(writer->add_section(part.section_name, part.bytes).has_value());
            }
        }
        ASSERT_TRUE(writer->commit().has_value());
        EXPECT_EQ(test::read_bytes(dir / "golden.tsc"), golden());
    }
}

TEST(ContainerGolden, InMemoryBuilderReproducesTheFile) {
    const auto built = build_container(test::golden_parts());
    ASSERT_TRUE(built.has_value());
    EXPECT_EQ(*built, golden());
}

TEST(ContainerGolden, TimestampPayloadIsTheCodecsOwnBlock) {
    Bytes encoded;
    ASSERT_TRUE(codec::encode_timestamp_block(test::kGoldenTimes, encoded).has_value());
    EXPECT_EQ(encoded, test::golden_timestamp_block());
}

// F5: the version 1 file stays readable by every later reader, with every value.
TEST(ContainerGolden, Version1FileOpensWithEveryValue) {
    const Bytes file = golden_copy();
    const auto parser = ContainerParser::open(file);
    ASSERT_TRUE(parser.has_value()) << to_string(parser.error());
    EXPECT_EQ(parser->version(), 1U);
    EXPECT_EQ(parser->block_count(), 2U);
    EXPECT_EQ(parser->section_count(), 2U);
    EXPECT_EQ(parser->footer_offset(), kGoldenFooter);

    const auto first = parser->block_info(0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->codec, CodecId::gorilla_timestamps);
    EXPECT_EQ(first->payload_offset, 16U);
    EXPECT_EQ(first->payload_size, 21U);
    EXPECT_EQ(Bytes(first->metadata.begin(), first->metadata.end()), test::golden_zone_map());

    const auto block = parser->block(0);
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(Bytes(block->payload.begin(), block->payload.end()), test::golden_timestamp_block());
    EXPECT_EQ(block->owner, nullptr);
    const codec::CodecInfo* info = codec::find_codec(static_cast<std::uint16_t>(block->codec));
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->decoder, nullptr);
    std::vector<std::uint64_t> values;
    const auto used = info->decoder(block->payload, values);
    ASSERT_TRUE(used.has_value());
    EXPECT_EQ(*used, block->payload.size());
    EXPECT_EQ(values, (std::vector<std::uint64_t>(test::kGoldenTimes.begin(),
                                                  test::kGoldenTimes.end())));

    const auto raw = parser->block(1);
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(raw->codec, CodecId::raw);
    EXPECT_EQ(Bytes(raw->metadata.begin(), raw->metadata.end()),
              (Bytes{std::byte{0x01}, std::byte{0x02}}));
    EXPECT_EQ(Bytes(raw->payload.begin(), raw->payload.end()),
              (Bytes{std::byte{0xAB}, std::byte{0xCD}}));

    const auto dict_info = parser->section_info(0);
    ASSERT_TRUE(dict_info.has_value());
    EXPECT_EQ(dict_info->name, "dict.stock");
    EXPECT_EQ(dict_info->offset, 37U);
    EXPECT_EQ(dict_info->size, 8U);
    const auto dict = parser->section("dict.stock");
    ASSERT_TRUE(dict.has_value());
    EXPECT_EQ(Bytes(dict->bytes.begin(), dict->bytes.end()), test::ascii_bytes("ZVZZT   "));
    const auto empty = parser->section("empty");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->bytes.empty());
    EXPECT_EQ(empty->name, "empty");

    EXPECT_TRUE(parser->verify_all().has_value());
}

TEST(ContainerGolden, MappedReaderOpensTheFile) {
    const auto reader = ContainerReader::open(test::golden_path());
    ASSERT_TRUE(reader.has_value()) << to_string(reader.error());
    EXPECT_EQ(reader->version(), 1U);
    EXPECT_EQ(reader->block_count(), 2U);
    EXPECT_EQ(reader->section_count(), 2U);
    const auto block = reader->block(0);
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(Bytes(block->payload.begin(), block->payload.end()), test::golden_timestamp_block());
    EXPECT_NE(block->owner, nullptr);
    EXPECT_TRUE(reader->verify_all().has_value());
    EXPECT_EQ(reader->section_info(1).value_or(SectionInfo{}).name, "empty");
    EXPECT_EQ(reader->block_info(1).value_or(BlockInfo{}).codec, CodecId::raw);
}

// --- Lookups --------------------------------------------------------------------

TEST(ContainerLookup, OutOfRangeAndMissingAreNotFound) {
    const Bytes file = golden_copy();
    const auto parser = ContainerParser::open(file);
    ASSERT_TRUE(parser.has_value());
    EXPECT_EQ(parser->block(2).error(), error(StoreErrorKind::not_found, "block index", 0, 2, 2));
    EXPECT_EQ(parser->block_info(kMax64).error(),
              error(StoreErrorKind::not_found, "block index", 0, 2, kMax64));
    EXPECT_EQ(parser->section_info(2).error(),
              error(StoreErrorKind::not_found, "section index", 0, 2, 2));
    for (const std::string_view name : {"", "dict", "dict.stocks", "emptz", "a"}) {
        EXPECT_EQ(parser->section(name).error(), error(StoreErrorKind::not_found, "section", 0, 2, 0))
            << name;
    }
}

// --- Header -----------------------------------------------------------------------

TEST(ContainerOpen, RefusesABadMagic) {
    for (std::size_t i = 0; i < kContainerMagic.size(); ++i) {
        Bytes file = golden_copy();
        file[i] ^= std::byte{0x20};
        EXPECT_EQ(open_error(file), error(StoreErrorKind::bad_magic, "magic", 0,
                                          magic_of(kContainerMagic), magic_of(file)))
            << i;
    }
}

TEST(ContainerOpen, RefusesAnUnsupportedVersion) {
    for (const std::uint64_t version : {std::uint64_t{0}, std::uint64_t{2}, kMax32}) {
        Bytes file = golden_copy();
        test::put(file, kHeaderVersionOffset, 4, version);
        EXPECT_EQ(open_error(file), error(StoreErrorKind::unsupported_version, "version", 8, 1,
                                          version));
    }
}

TEST(ContainerOpen, RefusesABadHeaderSize) {
    for (const std::uint64_t size : {std::uint64_t{0}, std::uint64_t{15}, std::uint64_t{17},
                                     kMax32}) {
        Bytes file = golden_copy();
        test::put(file, kHeaderSizeOffset, 4, size);
        EXPECT_EQ(open_error(file), error(StoreErrorKind::bad_header_size, "header size", 12, 16,
                                          size));
    }
}

// Length, magic, version, header size, in that order (F1).
TEST(ContainerOpen, ChecksTheHeaderInOrder) {
    Bytes file = golden_copy();
    test::put(file, kHeaderSizeOffset, 4, 99);
    test::put(file, kHeaderVersionOffset, 4, 99);
    EXPECT_EQ(kind_of(file), StoreErrorKind::unsupported_version);
    file[0] = std::byte{'X'};
    EXPECT_EQ(kind_of(file), StoreErrorKind::bad_magic);
    EXPECT_EQ(kind_of(std::span{file}.first(15)), StoreErrorKind::file_too_small);
}

// --- Trailer ----------------------------------------------------------------------

TEST(ContainerOpen, RefusesABadTrailerMagic) {
    for (std::size_t i = 0; i < kTrailerMagic.size(); ++i) {
        Bytes file = golden_copy();
        file[kGoldenTrailer + kTrailerMagicOffset + i] ^= std::byte{0x01};
        EXPECT_EQ(open_error(file),
                  error(StoreErrorKind::bad_magic, "trailer magic",
                        kGoldenTrailer + kTrailerMagicOffset, magic_of(kTrailerMagic),
                        magic_of(std::span{file}.subspan(kGoldenTrailer + kTrailerMagicOffset))))
            << i;
    }
}

TEST(ContainerOpen, RefusesANonZeroTrailerReserved) {
    for (const std::uint64_t value : {std::uint64_t{1}, kMax32}) {
        Bytes file = golden_copy();
        test::put(file, kGoldenTrailer + kTrailerReserved, 4, value);
        EXPECT_EQ(open_error(file), error(StoreErrorKind::bad_field, "trailer reserved",
                                          kGoldenTrailer + kTrailerReserved, 0, value));
    }
}

TEST(ContainerOpen, RefusesAFooterOffsetOutsideTheFile) {
    const std::uint64_t last = kGoldenTrailer - kFooterFixedSize;
    for (const std::uint64_t footer : {std::uint64_t{0}, std::uint64_t{15}}) {
        Bytes file = golden_copy();
        test::put(file, kGoldenTrailer + kTrailerFooterOffset, 8, footer);
        EXPECT_EQ(open_error(file), error(StoreErrorKind::bad_footer, "footer offset",
                                          kGoldenTrailer + kTrailerFooterOffset, 16, footer));
    }
    for (const std::uint64_t footer : {last + 1, std::uint64_t{kGoldenTrailer}, kMax64}) {
        Bytes file = golden_copy();
        test::put(file, kGoldenTrailer + kTrailerFooterOffset, 8, footer);
        EXPECT_EQ(open_error(file), error(StoreErrorKind::bad_footer, "footer offset",
                                          kGoldenTrailer + kTrailerFooterOffset, last, footer));
    }
}

TEST(ContainerOpen, RefusesAFooterWhoseCrcDiffers) {
    const std::uint32_t stored = static_cast<std::uint32_t>(
        read_le<4>(golden(), kGoldenTrailer + kTrailerFooterCrc).value_or(0));
    // A flipped CRC.
    Bytes file = golden_copy();
    test::put(file, kGoldenTrailer + kTrailerFooterCrc, 4, stored ^ 1U);
    EXPECT_EQ(open_error(file), error(StoreErrorKind::bad_checksum, "footer", kGoldenFooter,
                                      stored ^ 1U, stored));
    // A flipped bit in each footer byte.
    for (std::size_t at = kGoldenFooter; at < kGoldenTrailer; ++at) {
        Bytes flipped = golden_copy();
        flipped[at] ^= std::byte{0x10};
        const auto found = open_error(flipped);
        ASSERT_TRUE(found.has_value()) << at;
        EXPECT_EQ(found->kind, StoreErrorKind::bad_checksum) << at;
        EXPECT_EQ(found->field, "footer") << at;
    }
    // A footer offset moved inside the file, which the CRC catches first.
    Bytes moved = golden_copy();
    test::put(moved, kGoldenTrailer, 8, kGoldenFooter + 1);
    EXPECT_EQ(kind_of(moved), StoreErrorKind::bad_checksum);
}

TEST(ContainerOpen, SkippingChecksumsReachesTheStructureBehindTheFooterCrc) {
    Bytes file = golden_copy();
    test::put(file, kGoldenTrailer + kTrailerFooterCrc, 4, 0);
    EXPECT_EQ(kind_of(file), StoreErrorKind::bad_checksum);
    EXPECT_EQ(open_error(file, {.checksums = Checksums::skip}), std::nullopt);
}

// --- Footer counts ----------------------------------------------------------------

TEST(ContainerOpen, RefusesABlockCountTheFooterCannotHold) {
    // 210 bytes after the counts hold at most 6 block entries.
    for (const std::uint64_t count : {std::uint64_t{7}, kMax64}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), kGoldenFooter + kFooterBlockCount, 8,
                                               count)),
                  error(StoreErrorKind::bad_footer, "block count",
                        kGoldenFooter + kFooterBlockCount, 6, count));
    }
}

TEST(ContainerOpen, RefusesASectionCountTheFooterCannotHold) {
    for (const std::uint64_t count : {std::uint64_t{3}, kMax64}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), kGoldenFooter + kFooterSectionCount, 8,
                                               count)),
                  error(StoreErrorKind::bad_footer, "section count",
                        kGoldenFooter + kFooterSectionCount, 2, count));
    }
    // A third block entry leaves room for one section, not two.
    EXPECT_EQ(open_error(with_footer_field(golden(), kGoldenFooter + kFooterBlockCount, 8, 3)),
              error(StoreErrorKind::bad_footer, "section count",
                    kGoldenFooter + kFooterSectionCount, 1, 2));
}

TEST(ContainerOpen, RefusesAMetadataSizeThatDoesNotFillTheFooter) {
    for (const std::uint64_t size : {std::uint64_t{0}, std::uint64_t{17}, std::uint64_t{19},
                                     kMax64}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), kGoldenFooter + kFooterMetadataSize, 8,
                                               size)),
                  error(StoreErrorKind::bad_footer, "metadata size",
                        kGoldenFooter + kFooterMetadataSize, 18, size));
    }
}

// --- Block entries ----------------------------------------------------------------

TEST(ContainerOpen, RefusesABlockOffsetOffTheCursor) {
    for (const std::uint64_t offset : {std::uint64_t{0}, std::uint64_t{15}, std::uint64_t{17},
                                       kMax64}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0), 8, offset)),
                  error(StoreErrorKind::bad_extent, "block offset", golden_block_entry(0), 16,
                        offset));
    }
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(1), 8, 44)),
              error(StoreErrorKind::bad_extent, "block offset", golden_block_entry(1), 45, 44));
}

TEST(ContainerOpen, RefusesABlockMetadataOffsetOffTheCursor) {
    for (const std::uint64_t offset : {std::uint64_t{1}, kMax64}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0) + kBlockMetadataOffset,
                                               8, offset)),
                  error(StoreErrorKind::bad_extent, "block metadata offset",
                        golden_block_entry(0) + kBlockMetadataOffset, 0, offset));
    }
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(1) + kBlockMetadataOffset,
                                           8, 15)),
              error(StoreErrorKind::bad_extent, "block metadata offset",
                    golden_block_entry(1) + kBlockMetadataOffset, 16, 15));
}

TEST(ContainerOpen, RefusesABlockSizeThatOverrunsOrLeavesAGap) {
    // Past the footer: 31 bytes from 16 to the footer at 47.
    for (const std::uint64_t size : {std::uint64_t{32}, kMax32}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0) + kBlockPayloadSize,
                                               4, size)),
                  error(StoreErrorKind::bad_extent, "block size",
                        golden_block_entry(0) + kBlockPayloadSize, 31, size));
    }
    // Into the next extent, or short of it: the next block no longer starts at the cursor.
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0) + kBlockPayloadSize, 4,
                                           22)),
              error(StoreErrorKind::bad_extent, "block offset", golden_block_entry(1), 38, 45));
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0) + kBlockPayloadSize, 4,
                                           20)),
              error(StoreErrorKind::bad_extent, "block offset", golden_block_entry(1), 36, 45));
}

TEST(ContainerOpen, RefusesABlockMetadataSizeThatOverrunsOrLeavesAGap) {
    for (const std::uint64_t size : {std::uint64_t{19}, kMax32}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0) + kBlockMetadataSize,
                                               4, size)),
                  error(StoreErrorKind::bad_extent, "block metadata size",
                        golden_block_entry(0) + kBlockMetadataSize, 18, size));
    }
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0) + kBlockMetadataSize, 4,
                                           15)),
              error(StoreErrorKind::bad_extent, "block metadata offset",
                    golden_block_entry(1) + kBlockMetadataOffset, 15, 16));
    // The last block's metadata stops short of the area's end.
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(1) + kBlockMetadataSize, 4,
                                           1)),
              error(StoreErrorKind::bad_extent, "metadata area",
                    kGoldenFooter + kFooterMetadataSize, 17, 18));
}

TEST(ContainerOpen, RefusesAnUnknownOrReservedCodec) {
    for (const std::uint64_t id : {std::uint64_t{0}, std::uint64_t{codec::kCodecs.size()},
                                   std::uint64_t{0xFFFF}}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(0) + kBlockCodec, 2,
                                               id)),
                  error(StoreErrorKind::unknown_codec, "codec id",
                        golden_block_entry(0) + kBlockCodec, 0, id));
    }
}

TEST(ContainerOpen, RefusesANonZeroBlockReserved) {
    for (const std::uint64_t value : {std::uint64_t{1}, std::uint64_t{0xFFFF}}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_block_entry(1) + kBlockReserved, 2,
                                               value)),
                  error(StoreErrorKind::bad_field, "block reserved",
                        golden_block_entry(1) + kBlockReserved, 0, value));
    }
}

// The block CRC is checked at lookup, so opening reads only the file's ends (F3).
TEST(ContainerBlock, RefusesAPayloadWhoseCrcDiffers) {
    const BlockInfo info = ContainerParser::open(golden()).value().block_info(0).value();
    for (std::size_t at = 16; at < 37; ++at) {
        Bytes file = golden_copy();
        file[at] ^= std::byte{0x04};
        const auto parser = ContainerParser::open(file);
        ASSERT_TRUE(parser.has_value()) << at;
        const auto block = parser->block(0);
        ASSERT_FALSE(block.has_value()) << at;
        EXPECT_EQ(block.error().kind, StoreErrorKind::bad_checksum);
        EXPECT_EQ(block.error().field, "block");
        EXPECT_EQ(block.error().offset, 16U);
        EXPECT_EQ(block.error().expected, info.crc);
        EXPECT_EQ(parser->verify_all().error(), block.error());
        EXPECT_TRUE(parser->block(1).has_value());
    }
}

TEST(ContainerBlock, RefusesAStoredCrcThatDiffers) {
    const Bytes file = with_footer_field(golden(), golden_block_entry(1) + kBlockCrc, 4, 0);
    const auto parser = ContainerParser::open(file);
    ASSERT_TRUE(parser.has_value());
    const std::uint32_t real = block_crc(CodecId::raw, std::span{golden()}.subspan(45, 2));
    EXPECT_EQ(parser->block(1).error(),
              error(StoreErrorKind::bad_checksum, "block", 45, 0, real));
    EXPECT_EQ(parser->verify_all().error(), parser->block(1).error());
}

// The CRC covers the codec id, so a block relabelled with another valid codec is
// refused before any decoder sees it.
TEST(ContainerBlock, RefusesABlockRelabelledWithAnotherCodec) {
    const Bytes file = with_footer_field(golden(), golden_block_entry(0) + kBlockCodec, 2,
                                         static_cast<std::uint16_t>(CodecId::pfor));
    const auto parser = ContainerParser::open(file);
    ASSERT_TRUE(parser.has_value());
    const auto block = parser->block(0);
    ASSERT_FALSE(block.has_value());
    EXPECT_EQ(block.error().kind, StoreErrorKind::bad_checksum);
}

TEST(ContainerBlock, SkipIsTheOnlyWayPastABadBlockCrc) {
    Bytes file = golden_copy();
    file[20] ^= std::byte{0x01};
    EXPECT_FALSE(ContainerParser::open(file).value().block(0).has_value());
    const auto skipping = ContainerParser::open(file, {.checksums = Checksums::skip});
    ASSERT_TRUE(skipping.has_value());
    EXPECT_TRUE(skipping->block(0).has_value());
    // verify_all checks whatever the options said.
    EXPECT_FALSE(skipping->verify_all().has_value());
}

// --- Section entries --------------------------------------------------------------

TEST(ContainerOpen, RefusesASectionOffsetOffTheCursor) {
    for (const std::uint64_t offset : {std::uint64_t{46}, std::uint64_t{48}, kMax64}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_section_entry(1) + kSectionOffset,
                                               8, offset)),
                  error(StoreErrorKind::bad_extent, "section offset", golden_section_entry(1), 47,
                        offset));
    }
}

TEST(ContainerOpen, RefusesASectionSizeThatOverrunsOrLeavesAGap) {
    for (const std::uint64_t size : {std::uint64_t{11}, kMax64}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_section_entry(0) + kSectionSize, 8,
                                               size)),
                  error(StoreErrorKind::bad_extent, "section size",
                        golden_section_entry(0) + kSectionSize, 10, size));
    }
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_section_entry(0) + kSectionSize, 8, 7)),
              error(StoreErrorKind::bad_extent, "block offset", golden_block_entry(1), 44, 45));
    // The empty section grows into the footer.
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_section_entry(1) + kSectionSize, 8, 1)),
              error(StoreErrorKind::bad_extent, "section size",
                    golden_section_entry(1) + kSectionSize, 0, 1));
}

TEST(ContainerOpen, RefusesABadSectionNameLength) {
    for (const std::uint64_t length : {std::uint64_t{0}, std::uint64_t{44}, std::uint64_t{255}}) {
        EXPECT_EQ(open_error(with_footer_field(golden(),
                                               golden_section_entry(0) + kSectionNameLength, 1,
                                               length)),
                  error(StoreErrorKind::bad_field, "section name length",
                        golden_section_entry(0) + kSectionNameLength, 43, length));
    }
}

TEST(ContainerOpen, RefusesASectionNameCharacterOutsideTheAlphabet) {
    for (const std::uint64_t value : {std::uint64_t{'A'}, std::uint64_t{' '}, std::uint64_t{'/'},
                                      std::uint64_t{0}, std::uint64_t{0xFF}}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_section_entry(0) + kSectionName + 4,
                                               1, value)),
                  error(StoreErrorKind::bad_field, "section name",
                        golden_section_entry(0) + kSectionName + 4, 0, value));
    }
}

TEST(ContainerOpen, RefusesNonZeroSectionNamePadding) {
    // "dict.stock" is 10 bytes, so padding runs from 10 to the entry's end.
    for (const std::size_t at : {std::size_t{10}, std::size_t{42}}) {
        EXPECT_EQ(open_error(with_footer_field(golden(), golden_section_entry(0) + kSectionName + at,
                                               1, 'x')),
                  error(StoreErrorKind::bad_field, "section name padding",
                        golden_section_entry(0) + kSectionName + at, 0, 'x'));
    }
    // A shortened length turns name bytes into padding.
    EXPECT_EQ(open_error(with_footer_field(golden(), golden_section_entry(0) + kSectionNameLength,
                                           1, 4)),
              error(StoreErrorKind::bad_field, "section name padding",
                    golden_section_entry(0) + kSectionName + 4, 0, '.'));
}

TEST(ContainerOpen, RefusesARepeatedSectionName) {
    Bytes file = golden_copy();
    test::put(file, golden_section_entry(1) + kSectionNameLength, 1, 10);
    const Bytes name = test::ascii_bytes("dict.stock");
    std::ranges::copy(name, file.begin() + static_cast<std::ptrdiff_t>(golden_section_entry(1) +
                                                                       kSectionName));
    test::fix_footer_crc(file);
    EXPECT_EQ(open_error(file), error(StoreErrorKind::bad_field, "section name repeated",
                                      golden_section_entry(1) + kSectionName, 0, 1));
}

TEST(ContainerSection, RefusesBytesWhoseCrcDiffers) {
    for (std::size_t at = 37; at < 45; ++at) {
        Bytes file = golden_copy();
        file[at] ^= std::byte{0x80};
        const auto parser = ContainerParser::open(file);
        ASSERT_TRUE(parser.has_value());
        const auto section = parser->section("dict.stock");
        ASSERT_FALSE(section.has_value()) << at;
        EXPECT_EQ(section.error().kind, StoreErrorKind::bad_checksum);
        EXPECT_EQ(section.error().field, "section");
        EXPECT_EQ(section.error().offset, 37U);
        EXPECT_EQ(parser->verify_all().error(), section.error());
        EXPECT_TRUE(parser->section("empty").has_value());
    }
    const Bytes stored = with_footer_field(golden(), golden_section_entry(1) + kSectionCrc, 4, 7);
    const auto parser = ContainerParser::open(stored);
    ASSERT_TRUE(parser.has_value());
    EXPECT_EQ(parser->section("empty").error(),
              error(StoreErrorKind::bad_checksum, "section", 47, 7, 0));
    EXPECT_TRUE(ContainerParser::open(stored, {.checksums = Checksums::skip})
                    .value()
                    .section("empty")
                    .has_value());
}

// --- Extents in built files -------------------------------------------------------

Bytes built(const std::vector<ContainerPart>& parts) {
    auto file = build_container(parts);
    EXPECT_TRUE(file.has_value());
    file->shrink_to_fit();
    return file.value_or(Bytes{});
}

ContainerPart raw_block(Bytes payload, Bytes metadata = {}) {
    return {.section_name = "", .codec = CodecId::raw, .metadata = std::move(metadata),
            .bytes = std::move(payload)};
}

ContainerPart section_part(std::string name, Bytes bytes) {
    return {.section_name = std::move(name), .codec = {}, .metadata = {}, .bytes = std::move(bytes)};
}

TEST(ContainerOpen, RefusesExtentsThatStopShortOfTheFooter) {
    Bytes file = built({raw_block({std::byte{1}, std::byte{2}})});
    // Footer at 18, entry at 18 + 24.
    test::put(file, 18 + kFooterFixedSize + kBlockPayloadSize, 4, 1);
    test::fix_footer_crc(file);
    EXPECT_EQ(open_error(file),
              error(StoreErrorKind::bad_extent, "footer offset",
                    file.size() - kTrailerSize, 17, 18));
}

TEST(ContainerOpen, AcceptsTheSmallestFileAndEmptyParts) {
    const Bytes empty = built({});
    EXPECT_EQ(empty.size(), kMinContainerSize);
    const auto parser = ContainerParser::open(empty);
    ASSERT_TRUE(parser.has_value());
    EXPECT_EQ(parser->block_count(), 0U);
    EXPECT_EQ(parser->section_count(), 0U);

    // Empty blocks and sections share offsets with their neighbours, in any order.
    const Bytes mixed = built({section_part("a", {}), raw_block({}), section_part("b", {}),
                               raw_block({std::byte{9}}), raw_block({}), section_part("c", {})});
    const auto parsed = ContainerParser::open(mixed);
    ASSERT_TRUE(parsed.has_value()) << to_string(parsed.error());
    EXPECT_EQ(parsed->block_count(), 3U);
    EXPECT_EQ(parsed->section_count(), 3U);
    EXPECT_TRUE(parsed->verify_all().has_value());
    EXPECT_EQ(parsed->block(1).value().payload.size(), 1U);
}

// Sections are found by name whatever order they were written in.
TEST(ContainerLookup, FindsEverySectionByName) {
    std::vector<ContainerPart> parts;
    std::vector<std::string> names;
    for (int i = 0; i < 40; ++i) {
        // Written in an order unlike name order.
        const std::string name = "s" + std::to_string((i * 17) % 40);
        names.push_back(name);
        parts.push_back(section_part(name, le_bytes(static_cast<std::uint64_t>(i), 3)));
        parts.push_back(raw_block(le_bytes(static_cast<std::uint64_t>(i), 1)));
    }
    const Bytes file = built(parts);
    const auto parser = ContainerParser::open(file);
    ASSERT_TRUE(parser.has_value());
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto section = parser->section(names[i]);
        ASSERT_TRUE(section.has_value()) << names[i];
        EXPECT_EQ(Bytes(section->bytes.begin(), section->bytes.end()),
                  le_bytes(static_cast<std::uint64_t>(i), 3));
    }
    EXPECT_FALSE(parser->section("s40").has_value());
}

// --- Truncation -------------------------------------------------------------------

// Every length short of the whole file is refused, in a heap block of exactly that
// size so ASan sees any read past it (D10, F7).
TEST(ContainerOpen, RefusesTheGoldenFileCutAtEveryLength) {
    for (std::size_t length = 0; length < golden().size(); ++length) {
        const Bytes cut(golden().begin(), golden().begin() + static_cast<std::ptrdiff_t>(length));
        const auto found = open_error(cut);
        ASSERT_TRUE(found.has_value()) << length;
        if (length < kContainerHeaderSize) {
            EXPECT_EQ(found, error(StoreErrorKind::file_too_small, "file", 0, 16, length));
        } else if (length < kMinContainerSize) {
            EXPECT_EQ(found, error(StoreErrorKind::file_too_small, "file", 0, 64, length));
        } else {
            EXPECT_EQ(found->kind, StoreErrorKind::bad_magic) << length;
            EXPECT_EQ(found->field, "trailer magic") << length;
        }
        // Skipping checksums does not let a cut file through either.
        EXPECT_TRUE(open_error(cut, {.checksums = Checksums::skip}).has_value()) << length;
    }
}

// --- The layout builder's refusals ------------------------------------------------

TEST(ContainerLayout, RefusesACodecAWriterMayNotUse) {
    ContainerLayout layout;
    const Bytes payload{std::byte{1}};
    for (const std::uint16_t id : {std::uint16_t{0}, static_cast<std::uint16_t>(codec::kCodecs.size()),
                                   std::uint16_t{0xFFFF}}) {
        EXPECT_EQ(layout.add_block(static_cast<CodecId>(id), {}, payload),
                  std::unexpected(error(StoreErrorKind::invalid_argument, "codec id", 0, 0, id)));
    }
    EXPECT_EQ(layout.block_count(), 0U);
    EXPECT_EQ(layout.data_end(), kContainerHeaderSize);
}

TEST(ContainerLayout, RefusesABadOrRepeatedSectionName) {
    ContainerLayout layout;
    const std::string longest(kMaxSectionName, 'a');
    for (const std::string_view name : {std::string_view{""}, std::string_view{"Upper"},
                                        std::string_view{"a b"}, std::string_view{"a/b"},
                                        std::string_view{longest + "a"}}) {
        EXPECT_EQ(layout.add_section(name, {}),
                  std::unexpected(error(StoreErrorKind::invalid_argument, "section name", 0, 43,
                                        name.size())))
            << name;
    }
    EXPECT_TRUE(layout.add_section(longest, {}).has_value());
    EXPECT_TRUE(layout.add_section("0_.-z", {}).has_value());
    EXPECT_EQ(layout.add_section("0_.-z", {}),
              std::unexpected(error(StoreErrorKind::invalid_argument, "section name repeated", 0,
                                    0, 0)));
    EXPECT_TRUE(valid_section_name(longest));
    EXPECT_FALSE(valid_section_name(longest + "a"));
}

TEST(ContainerWriterArguments, ARefusedArgumentChangesNothing) {
    const ScratchDir dir;
    auto writer = ContainerWriter::create(dir / "file.tsc", Durability::page_cache);
    ASSERT_TRUE(writer.has_value());
    const auto parts = test::golden_parts();
    ASSERT_TRUE(writer->add_block(parts[0].codec, parts[0].metadata, parts[0].bytes).has_value());
    EXPECT_FALSE(writer->add_block(CodecId::none, {}, parts[2].bytes).has_value());
    ASSERT_TRUE(writer->add_section(parts[1].section_name, parts[1].bytes).has_value());
    EXPECT_FALSE(writer->add_section("dict.stock", parts[1].bytes).has_value());
    EXPECT_FALSE(writer->add_section("Bad", {}).has_value());
    ASSERT_TRUE(writer->add_block(parts[2].codec, parts[2].metadata, parts[2].bytes).has_value());
    ASSERT_TRUE(writer->add_section(parts[3].section_name, parts[3].bytes).has_value());
    ASSERT_TRUE(writer->commit().has_value());
    EXPECT_EQ(test::read_bytes(dir / "file.tsc"), golden());
}

// --- The codec registry -----------------------------------------------------------

TEST(CodecRegistry, NamesAreUniqueAndReservedIdsDecodeNothing) {
    std::set<std::string_view> names;
    for (const codec::CodecInfo& info : codec::kCodecs) {
        EXPECT_TRUE(names.insert(info.name).second) << info.name;
        if (info.status == codec::CodecStatus::reserved) {
            EXPECT_EQ(info.decoder, nullptr) << info.name;
            EXPECT_FALSE(codec::codec_readable(static_cast<std::uint16_t>(info.id)));
            EXPECT_FALSE(codec::codec_writable(static_cast<std::uint16_t>(info.id)));
        }
    }
    EXPECT_EQ(codec::find_codec(0)->status, codec::CodecStatus::reserved);
    EXPECT_EQ(codec::find_codec(static_cast<std::uint16_t>(codec::kCodecs.size())), nullptr);
    EXPECT_EQ(codec::find_codec(1)->decoder, nullptr);
    EXPECT_TRUE(codec::codec_writable(1));
}

// Each id's decoder takes the block its codec's encoder writes: the registry and the
// codecs agree on what every id means.
TEST(CodecRegistry, EachDecoderReadsItsOwnEncodersBlock) {
    std::vector<std::uint64_t> values;
    values.reserve(300);
    for (std::uint64_t i = 0; i < 300; ++i) {
        values.push_back(34'200'000'000'000ULL + (i * 1'000) + ((i * 7) % 13));
    }
    const auto encode = [&](CodecId id) -> Bytes {
        Bytes out;
        const std::size_t block = values.size();
        switch (id) {
            case CodecId::gorilla_timestamps:
                EXPECT_TRUE(codec::encode_timestamps(values, block, out).has_value());
                break;
            case CodecId::gorilla_xor:
                EXPECT_TRUE(codec::encode_xor(values, block, out).has_value());
                break;
            case CodecId::pfor:
                EXPECT_TRUE(codec::encode_pfor(values, block, {}, out).has_value());
                break;
            case CodecId::stream_vbyte:
                EXPECT_TRUE(codec::encode_svb(values, block, true, out).has_value());
                break;
            case CodecId::dictionary:
                EXPECT_TRUE(codec::encode_dict(values, block, out).has_value());
                break;
            case CodecId::run_length:
                EXPECT_TRUE(codec::encode_rle(values, block, out).has_value());
                break;
            case CodecId::none:
            case CodecId::raw:
            case CodecId::zstd:
                break;
        }
        return out;
    };
    int decoders = 0;
    for (const codec::CodecInfo& info : codec::kCodecs) {
        if (info.decoder == nullptr) {
            continue;
        }
        SCOPED_TRACE(info.name);
        ++decoders;
        const Bytes block = encode(info.id);
        ASSERT_FALSE(block.empty());
        std::vector<std::uint64_t> decoded;
        const auto used = info.decoder(block, decoded);
        ASSERT_TRUE(used.has_value());
        EXPECT_EQ(*used, block.size());
        EXPECT_EQ(decoded, values);
    }
    EXPECT_EQ(decoders, 6);
}

// --- View lifetimes ---------------------------------------------------------------

// A3: views from the mapped reader hold the mapping, so a view outlives its reader.
// Under ASan a read of an unmapped page would fault, so this read passing is the proof.
TEST(ContainerReaderLifetime, ViewsOutliveTheReader) {
    std::optional<BlockView> block;
    std::optional<SectionView> section;
    {
        auto reader = ContainerReader::open(test::golden_path());
        ASSERT_TRUE(reader.has_value());
        block = reader->block(0).value();
        section = reader->section("dict.stock").value();
    }
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(Bytes(block->payload.begin(), block->payload.end()), test::golden_timestamp_block());
    EXPECT_EQ(Bytes(block->metadata.begin(), block->metadata.end()), test::golden_zone_map());
    EXPECT_EQ(section->name, "dict.stock");
    EXPECT_EQ(Bytes(section->bytes.begin(), section->bytes.end()), test::ascii_bytes("ZVZZT   "));
    EXPECT_EQ(block->owner.use_count(), 2);
    block.reset();
    EXPECT_EQ(section->owner.use_count(), 1);
}

TEST(ContainerReaderLifetime, AMovedReaderKeepsWorking) {
    auto opened = ContainerReader::open(test::golden_path());
    ASSERT_TRUE(opened.has_value());
    const ContainerReader moved = std::move(*opened);
    EXPECT_TRUE(moved.verify_all().has_value());
    EXPECT_EQ(moved.section("empty").value().name, "empty");
}

TEST(ContainerReader, ReportsAMissingFileAndRefusesAnEmptyOne) {
    const ScratchDir dir;
    const auto missing = ContainerReader::open(dir / "absent.tsc");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind, StoreErrorKind::io);
    EXPECT_EQ(missing.error().field, "open");
    test::write_bytes(dir / "empty.tsc", {});
    EXPECT_EQ(ContainerReader::open(dir / "empty.tsc").error(),
              error(StoreErrorKind::file_too_small, "file", 0, 16, 0));
}

TEST(StoreErrorText, NamesEveryContainerKind) {
    EXPECT_EQ(to_string(error(StoreErrorKind::bad_footer, "block count", 47, 6, 7)),
              "bad footer: block count at offset 47: expected 6, have 7");
    EXPECT_EQ(to_string(error(StoreErrorKind::bad_extent, "x", 1, 2, 3)),
              "bad extent: x at offset 1: expected 2, have 3");
    EXPECT_EQ(to_string(error(StoreErrorKind::bad_checksum, "x", 1, 2, 3)),
              "bad checksum: x at offset 1: expected 2, have 3");
    EXPECT_EQ(to_string(error(StoreErrorKind::unknown_codec, "x", 1, 2, 3)),
              "unknown codec: x at offset 1: expected 2, have 3");
    EXPECT_EQ(to_string(error(StoreErrorKind::bad_field, "x", 1, 2, 3)),
              "bad field: x at offset 1: expected 2, have 3");
    EXPECT_EQ(to_string(error(StoreErrorKind::not_found, "x", 1, 2, 3)),
              "not found: x at offset 1: expected 2, have 3");
    EXPECT_EQ(to_string(error(StoreErrorKind::invalid_argument, "x", 1, 2, 3)),
              "invalid argument: x at offset 1: expected 2, have 3");
}

}  // namespace
}  // namespace tick_store::store
