// One corrupt fixture per validated field of the ITCH store's own encoding: the store and
// symbols sections, the block metadata and segment summary, the directory the reader builds
// from the footer, the column chunks and the zstd wrapper, and the rules that tie a
// segment's columns together. Each file is rebuilt with valid CRCs, so the store's check is
// what refuses it, and each refusal is checked for the field it names. Then every cut of a
// file, and the golden file both ways.

#include "itch_fixtures.hpp"
#include "itch_store_fixture.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/registry.hpp"
#include "tick_store/codec/zstd_frame.hpp"
#include "tick_store/harness/itch_store_record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

using test::Bytes;
using test::get_field;
using test::put_field;
using test::StoreParts;

// A small store with every kind of column in use: the 26-record source at 8 records a
// segment, so four segments and a short last one.
constexpr std::uint32_t kN = 8;

Bytes base_file() { return test::store_of_source(tick_store::test::source_with_tail(), kN); }

// A store whose segment 0 holds creations, imports, an exception and a raw record.
std::vector<itch::Message> reference_messages() {
    static const Bytes raw = itch::test::bytes({'z', 7, 7});
    return {
        test::directory(1, 10, "AAPL"), test::add(1, 11, 100, "AAPL"),
        test::add(1, 12, 101, "MSFT"),  test::executed(1, 13, 100),
        test::deleted(1, 14, 555),      test::deleted(1, 15, 555),
        itch::RawMessage{.bytes = raw, .reason = itch::RawReason::unknown_type},
        test::add(2, 16, 102, "IBM"),
    };
}

Bytes reference_file() { return test::store_of_messages(reference_messages()); }

// The error a file gives: at open, or else reading its segments in order.
std::optional<ItchStoreError> first_error(std::span<const std::byte> file,
                                          ReadOptions options = {}) {
    auto records = test::all_payloads(file, options);
    if (records.has_value()) {
        return std::nullopt;
    }
    return records.error();
}

void expect_refused(const Bytes& file, std::string_view field, ItchStoreErrorKind kind) {
    const auto error = first_error(file);
    ASSERT_TRUE(error.has_value()) << "accepted; wanted " << field;
    EXPECT_EQ(error->field, field) << to_string(*error);
    EXPECT_EQ(error->kind, kind) << to_string(*error);
    // Without CRCs too: the store's own check, not a checksum, refused it.
    const auto unchecked = first_error(file, {.checksums = Checksums::skip});
    ASSERT_TRUE(unchecked.has_value());
    EXPECT_EQ(unchecked->field, field);
}

// --- The store section ----------------------------------------------------------------

TEST(ItchStoreCorrupt, StoreSectionFields) {
    struct Case {
        std::string_view field;
        std::function<void(Bytes&)> edit;
    };
    const std::vector<Case> cases{
        {"store section size", [](Bytes& b) { b.push_back(std::byte{0}); }},
        {"store version", [](Bytes& b) { put_field(b, kStoreVersionOffset, 4, 3); }},
        // Version 1, before the locate runs and the seek index, is no longer read.
        {"store version", [](Bytes& b) { put_field(b, kStoreVersionOffset, 4, 1); }},
        {"segment records", [](Bytes& b) { put_field(b, kStoreSegmentRecordsOffset, 4, 0); }},
        {"segment records",
         [](Bytes& b) { put_field(b, kStoreSegmentRecordsOffset, 4, kMaxSegmentRecords + 1); }},
        {"segment count", [](Bytes& b) { put_field(b, kStoreSegmentCountOffset, 8, 5); }},
        // More records than the segments hold, with the count still consistent.
        {"value count",
         [](Bytes& b) {
             put_field(b, kStoreRecordCountOffset, 8, 27);
         }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string{c.field});
        StoreParts parts = test::parts_of(base_file());
        c.edit(parts.section(kStoreSection).bytes);
        expect_refused(parts.build(), c.field,
                       c.field == "value count" ? ItchStoreErrorKind::bad_metadata
                                                : ItchStoreErrorKind::bad_section);
    }
}

TEST(ItchStoreCorrupt, SectionsMissingOrExtra) {
    {
        StoreParts parts = test::parts_of(base_file());
        parts.sections.push_back({.section_name = "extra", .codec = codec::CodecId::raw,
                                  .metadata = {}, .bytes = {}});
        expect_refused(parts.build(), "section count", ItchStoreErrorKind::bad_section);
    }
    for (const std::string_view name : {kStoreSection, kSymbolsSection, kTailSection}) {
        SCOPED_TRACE(std::string{name});
        StoreParts parts = test::parts_of(base_file());
        // Renamed, so the count is right and the name is missing.
        for (ContainerPart& part : parts.sections) {
            if (part.section_name == name) {
                part.section_name = "other";
            }
        }
        const auto error = first_error(parts.build());
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->kind, ItchStoreErrorKind::container);
        EXPECT_EQ(error->field, std::string{name} + " section");
    }
}

// --- The symbols section ------------------------------------------------------------

TEST(ItchStoreCorrupt, SymbolsSectionFields) {
    const auto edit_symbols = [](const std::function<void(Bytes&)>& edit) {
        StoreParts parts = test::parts_of(reference_file());
        edit(parts.section(kSymbolsSection).bytes);
        return parts.build();
    };
    expect_refused(edit_symbols([](Bytes& b) { b.resize(3); }), "symbols size",
                   ItchStoreErrorKind::bad_section);
    expect_refused(edit_symbols([](Bytes& b) { put_field(b, 0, 4, 3); }), "symbols count",
                   ItchStoreErrorKind::bad_section);
    expect_refused(edit_symbols([](Bytes& b) { b.push_back(std::byte{0}); }), "symbols count",
                   ItchStoreErrorKind::bad_section);
    // Two entries, locates 1 and 2: make the second 1.
    expect_refused(edit_symbols([](Bytes& b) { put_field(b, 14, 2, 1); }),
                   "symbols locate order", ItchStoreErrorKind::bad_section);
    // Locate 1's stock changed to the exception's: the exception now equals the symbol.
    expect_refused(edit_symbols([](Bytes& b) {
                       const auto msft = test::stock_of("MSFT");
                       std::ranges::copy(msft, b.begin() + 6);
                   }),
                   "stock exception equals symbol", ItchStoreErrorKind::bad_value);
    // Locate 2 removed: its add has no stock to derive.
    expect_refused(edit_symbols([](Bytes& b) {
                       put_field(b, 0, 4, 1);
                       b.resize(kSymbolsCountSize + kSymbolEntrySize);
                   }),
                   "symbol for locate", ItchStoreErrorKind::bad_value);
}

// --- Block metadata and the segment summary ---------------------------------------

TEST(ItchStoreCorrupt, MetadataFields) {
    struct Case {
        std::string_view field;
        Column column;
        std::function<void(Bytes&)> edit;
    };
    const std::vector<Case> cases{
        {"metadata size", Column::timestamp, [](Bytes& b) { b.push_back(std::byte{0}); }},
        {"metadata size", Column::timestamp, [](Bytes& b) { b.resize(11); }},
        {"column id", Column::timestamp,
         [](Bytes& b) { put_field(b, kMetaColumnOffset, 2, kColumnCount); }},
        {"metadata reserved", Column::timestamp, [](Bytes& b) { put_field(b, kMetaReservedOffset, 2, 1); }},
        {"value count", Column::timestamp, [](Bytes& b) { put_field(b, kMetaValueCountOffset, 4, 0); }},
        {"metadata size", Column::type, [](Bytes& b) { b.resize(kSegmentMetadataFixedSize - 1); }},
        {"metadata size", Column::type, [](Bytes& b) { b.push_back(std::byte{1}); }},
        {"locate bitmap size", Column::type,
         [](Bytes& b) { put_field(b, kMetaBitmapSizeOffset, 2, kMaxLocateBitmapBytes + 1); }},
        {"typed records", Column::type, [](Bytes& b) { put_field(b, kMetaTypedOffset, 4, kN + 1); }},
        {"max time", Column::type, [](Bytes& b) { put_field(b, kMetaMaxTimeOffset, 8, kMax48 + 1); }},
        {"min time", Column::type,
         [](Bytes& b) {
             put_field(b, kMetaMinTimeOffset, 8, get_field(b, kMetaMaxTimeOffset, 8) + 1);
         }},
        {"first time", Column::type,
         [](Bytes& b) { put_field(b, kMetaFirstTimeOffset, 8, get_field(b, kMetaMinTimeOffset, 8) - 1); }},
        {"last time", Column::type,
         [](Bytes& b) { put_field(b, kMetaLastTimeOffset, 8, get_field(b, kMetaMaxTimeOffset, 8) + 1); }},
        {"locate bitmap last byte", Column::type,
         [](Bytes& b) {
             b.push_back(std::byte{0});
             put_field(b, kMetaBitmapSizeOffset, 2, b.size() - kSegmentMetadataFixedSize);
         }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string{c.field});
        StoreParts parts = test::parts_of(base_file());
        c.edit(parts.block(0, c.column).metadata);
        expect_refused(parts.build(), c.field, ItchStoreErrorKind::bad_metadata);
    }
}

TEST(ItchStoreCorrupt, SummaryRulesWithNoTypedRecord) {
    // A segment of one raw record: no times, no bitmap.
    const Bytes raw = itch::test::bytes({'z'});
    const std::vector<itch::Message> one{itch::RawMessage{.bytes = raw}};
    const Bytes file = test::store_of_messages(one);
    {
        StoreParts parts = test::parts_of(file);
        Bytes& metadata = parts.block(0, Column::type).metadata;
        put_field(metadata, kMetaMaxTimeOffset, 8, 5);
        put_field(metadata, kMetaLastTimeOffset, 8, 5);
        put_field(metadata, kMetaFirstTimeOffset, 8, 5);
        expect_refused(parts.build(), "max time", ItchStoreErrorKind::bad_metadata);
    }
    {
        StoreParts parts = test::parts_of(file);
        Bytes& metadata = parts.block(0, Column::type).metadata;
        metadata.push_back(std::byte{1});
        put_field(metadata, kMetaBitmapSizeOffset, 2, 1);
        expect_refused(parts.build(), "locate bitmap size", ItchStoreErrorKind::bad_metadata);
    }
}

TEST(ItchStoreCorrupt, ASummaryThatDisagreesWithItsColumnsIsRefusedWithTheColumn) {
    struct Case {
        std::string_view field;
        std::string_view column;
        std::function<void(Bytes&)> edit;
    };
    // Segment 0's times are all the fixtures' one timestamp; the min and max can only move
    // outward, which the summary's own rules allow.
    const std::vector<Case> cases{
        {"typed records", "type", [](Bytes& b) { put_field(b, kMetaTypedOffset, 4, kN - 1); }},
        {"min time", "timestamp",
         [](Bytes& b) {
             put_field(b, kMetaMinTimeOffset, 8, get_field(b, kMetaMinTimeOffset, 8) - 1);
         }},
        {"max time", "timestamp",
         [](Bytes& b) {
             put_field(b, kMetaMaxTimeOffset, 8, get_field(b, kMetaMaxTimeOffset, 8) + 1);
         }},
        {"first time", "timestamp",
         [](Bytes& b) {
             put_field(b, kMetaMaxTimeOffset, 8, get_field(b, kMetaMaxTimeOffset, 8) + 1);
             put_field(b, kMetaFirstTimeOffset, 8, get_field(b, kMetaMaxTimeOffset, 8));
             put_field(b, kMetaLastTimeOffset, 8, get_field(b, kMetaMaxTimeOffset, 8));
         }},
        {"locate bitmap", "locate",
         [](Bytes& b) { b[kMetaBitmapOffset] ^= std::byte{0x01}; }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string{c.field});
        StoreParts parts = test::parts_of(base_file());
        c.edit(parts.block(0, Column::type).metadata);
        const Bytes file = parts.build();
        const auto error = first_error(file);
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->field, c.field) << to_string(*error);
        EXPECT_EQ(error->column, c.column) << to_string(*error);
        EXPECT_EQ(error->segment, 0U);
    }
    // The last time alone: the first and last are moved together above, so here only last.
    StoreParts parts = test::parts_of(test::store_of_messages(reference_messages()));
    Bytes& metadata = parts.block(0, Column::type).metadata;
    put_field(metadata, kMetaLastTimeOffset, 8, get_field(metadata, kMetaLastTimeOffset, 8) - 1);
    const auto error = first_error(parts.build());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->field, "last time") << to_string(*error);
    EXPECT_EQ(error->column, "timestamp");
}

// --- The directory --------------------------------------------------------------------

TEST(ItchStoreCorrupt, DirectoryFields) {
    {
        // Segment 1's type block claims segment 2.
        StoreParts parts = test::parts_of(base_file());
        put_field(parts.block(1, Column::type).metadata, kMetaSegmentOffset, 4, 2);
        expect_refused(parts.build(), "segment index", ItchStoreErrorKind::bad_metadata);
    }
    {
        // A column block of segment 1 claims segment 0.
        StoreParts parts = test::parts_of(base_file());
        put_field(parts.block(1, Column::timestamp).metadata, kMetaSegmentOffset, 4, 0);
        expect_refused(parts.build(), "segment index", ItchStoreErrorKind::bad_metadata);
    }
    {
        StoreParts parts = test::parts_of(base_file());
        put_field(parts.block(2, Column::type).metadata, kMetaFirstRecordOffset, 8, 17);
        expect_refused(parts.build(), "first record", ItchStoreErrorKind::bad_metadata);
    }
    {
        // Two blocks of one column in a segment: the second's id does not ascend.
        StoreParts parts = test::parts_of(base_file());
        const std::size_t b = parts.block_index(0, Column::timestamp);
        parts.blocks.insert(parts.blocks.begin() + static_cast<std::ptrdiff_t>(b) + 1,
                            parts.blocks[b]);
        expect_refused(parts.build(), "column id", ItchStoreErrorKind::bad_metadata);
    }
    {
        // A column block before any type block.
        StoreParts parts = test::parts_of(base_file());
        std::swap(parts.blocks[0], parts.blocks[1]);
        expect_refused(parts.build(), "column id", ItchStoreErrorKind::bad_metadata);
    }
    {
        // A segment's blocks removed whole: the footer has fewer segments than the header.
        StoreParts parts = test::parts_of(base_file());
        const std::size_t first = parts.block_index(3, Column::type);
        parts.blocks.resize(first);
        expect_refused(parts.build(), "segment count", ItchStoreErrorKind::bad_segment);
    }
    {
        // A type block past the header's segment count.
        StoreParts parts = test::parts_of(base_file());
        put_field(parts.section(kStoreSection).bytes, kStoreRecordCountOffset, 8, 24);
        put_field(parts.section(kStoreSection).bytes, kStoreSegmentCountOffset, 8, 3);
        expect_refused(parts.build(), "segment index", ItchStoreErrorKind::bad_metadata);
    }
    {
        // A block under a codec its column does not take.
        StoreParts parts = test::parts_of(base_file());
        parts.block(0, Column::timestamp).codec = codec::CodecId::dictionary;
        expect_refused(parts.build(), "column codec", ItchStoreErrorKind::bad_segment);
    }
    {
        // zstd on a column that never takes it.
        StoreParts parts = test::parts_of(base_file());
        ContainerPart& block = parts.block(0, Column::timestamp);
        Bytes wrapped;
        ASSERT_TRUE(codec::encode_zstd_payload(static_cast<std::uint16_t>(block.codec),
                                               block.bytes, wrapped)
                        .has_value());
        block.codec = codec::CodecId::zstd;
        block.bytes = std::move(wrapped);
        expect_refused(parts.build(), "column codec", ItchStoreErrorKind::bad_segment);
    }
}

// --- A segment's columns ----------------------------------------------------------------

TEST(ItchStoreCorrupt, ColumnCountsAgainstTheTypeColumn) {
    {
        // A column the records need is missing.
        StoreParts parts = test::parts_of(base_file());
        parts.blocks.erase(parts.blocks.begin() +
                           static_cast<std::ptrdiff_t>(parts.block_index(0, Column::timestamp)));
        expect_refused(parts.build(), "column missing", ItchStoreErrorKind::bad_segment);
    }
    {
        // A column holds one value more than its records need.
        StoreParts parts = test::parts_of(base_file());
        auto values = test::column_values(base_file(), 0, Column::timestamp);
        values.push_back(values.back());
        test::set_column(parts, 0, Column::timestamp, values);
        expect_refused(parts.build(), "column value count", ItchStoreErrorKind::bad_segment);
    }
    {
        // A column no record of the segment needs. Segment 3 is the two raw records and
        // the one-byte unknown: no match values.
        StoreParts parts = test::parts_of(base_file());
        const std::size_t at = parts.block_index(3, Column::type);
        auto chunk = encode_column(Column::match, std::vector<std::uint64_t>{1});
        ASSERT_TRUE(chunk.has_value());
        const ColumnMetadata metadata{.segment = 3, .column = Column::match, .values = 1,
                                      .summary = {}};
        parts.blocks.insert(parts.blocks.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                            ContainerPart{.section_name = {}, .codec = chunk->codec,
                                          .metadata = encode_column_metadata(metadata),
                                          .bytes = chunk->bytes});
        expect_refused(parts.build(), "column not expected", ItchStoreErrorKind::bad_segment);
    }
    {
        // More stock exceptions than records.
        StoreParts parts = test::parts_of(reference_file());
        put_field(parts.block(0, Column::stock_exception_record).metadata, kMetaValueCountOffset, 4,
                  9);
        expect_refused(parts.build(), "column value count", ItchStoreErrorKind::bad_segment);
    }
    {
        // An import too many: the distances say one, the import list holds two.
        StoreParts parts = test::parts_of(reference_file());
        test::set_column(parts, 0, Column::import_ref, std::vector<std::uint64_t>{555, 556});
        expect_refused(parts.build(), "column value count", ItchStoreErrorKind::bad_segment);
    }
    {
        // Raw bytes that do not sum to the raw lengths.
        StoreParts parts = test::parts_of(reference_file());
        test::set_column(parts, 0, Column::raw_length, std::vector<std::uint64_t>{4});
        expect_refused(parts.build(), "column value count", ItchStoreErrorKind::bad_segment);
    }
}

TEST(ItchStoreCorrupt, ValuesThatBreakTheRulesTyingColumnsTogether) {
    const Bytes file = reference_file();
    const auto values = [&](Column column) { return test::column_values(file, 0, column); };
    const auto with = [&](Column column, std::vector<std::uint64_t> replacement) {
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, column, replacement);
        return parts.build();
    };
    // A type symbol that is no ITCH type, in place of the R, whose rare bytes go too so the
    // other columns' counts still hold.
    {
        auto types = values(Column::type);
        ASSERT_EQ(types[0], static_cast<std::uint64_t>('R'));
        types[0] = 'a';
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, Column::type, types);
        parts.blocks.erase(parts.blocks.begin() +
                           static_cast<std::ptrdiff_t>(parts.block_index(0, Column::rare_bytes)));
        expect_refused(parts.build(), "type symbol", ItchStoreErrorKind::bad_value);
    }
    // A distance past the locate's list. Grouped D, D, E: E (100) is at distance 2.
    {
        auto distances = values(Column::ref_distance);
        ASSERT_EQ(distances, (std::vector<std::uint64_t>{0, 1, 2}));
        distances[2] = 4;
        const Bytes broken = with(Column::ref_distance, distances);
        expect_refused(broken, "ref distance", ItchStoreErrorKind::bad_value);
        const auto error = first_error(broken);
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->record, 3U);
        EXPECT_EQ(error->column, "ref_distance");
    }
    // Stock exceptions out of order or past the segment.
    {
        expect_refused(with(Column::stock_exception_record, {8}), "stock exception record",
                       ItchStoreErrorKind::bad_value);
    }
    // An exception on a record whose type has no derived stock (record 3 is an E).
    {
        expect_refused(with(Column::stock_exception_record, {3}), "stock exception record",
                       ItchStoreErrorKind::bad_value);
    }
    // The exception's stock equal to the symbol table's.
    {
        std::vector<std::uint64_t> stock;
        for (const std::byte b : test::stock_of("AAPL")) {
            stock.push_back(std::to_integer<std::uint64_t>(b));
        }
        expect_refused(with(Column::stock_exception_value, stock), "stock exception equals symbol",
                       ItchStoreErrorKind::bad_value);
    }
    // A raw length of 0, with the raw bytes moved to keep the counts right.
    {
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, Column::raw_length, std::vector<std::uint64_t>{0});
        parts.blocks.erase(parts.blocks.begin() +
                           static_cast<std::ptrdiff_t>(parts.block_index(0, Column::raw_bytes)));
        expect_refused(parts.build(), "raw length", ItchStoreErrorKind::bad_value);
    }
    // Raw bytes that are a conforming message: an S of the right length.
    {
        const auto& event = itch::test::fixture('S').bytes;
        std::vector<std::uint64_t> bytes;
        for (const std::byte b : event) {
            bytes.push_back(std::to_integer<std::uint64_t>(b));
        }
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, Column::raw_length, std::vector<std::uint64_t>{bytes.size()});
        test::set_column(parts, 0, Column::raw_bytes, bytes);
        expect_refused(parts.build(), "raw record conforms", ItchStoreErrorKind::bad_value);
    }
    // A value past its field's range: a side of 256, coded by the same dictionary codec
    // through a column that holds 32 bits.
    {
        std::vector<std::uint64_t> sides = values(Column::add_side);
        sides[0] = 256;
        auto chunk = encode_column(Column::add_shares, sides);
        ASSERT_TRUE(chunk.has_value());
        ASSERT_EQ(chunk->codec, primary_codec(Column::add_side));
        StoreParts parts = test::parts_of(file);
        parts.block(0, Column::add_side).bytes = chunk->bytes;
        const Bytes broken = parts.build();
        expect_refused(broken, "column value", ItchStoreErrorKind::bad_value);
        const auto error = first_error(broken);
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->column, "add_side");
        EXPECT_EQ(error->index, 0U);
    }
}

// A rare record's bytes are the payload after the header of a type whose every field
// decode() takes at the right length, so the "rare record" refusal is a guard no file
// reaches; what a file can break is the count of those bytes.
TEST(ItchStoreCorrupt, RareBytesOneShortAreRefused) {
    const std::vector<itch::Message> one{test::directory(1, 10, "AAPL")};
    const Bytes file = test::store_of_messages(one);
    StoreParts parts = test::parts_of(file);
    auto bytes = test::column_values(file, 0, Column::rare_bytes);
    bytes.pop_back();
    test::set_column(parts, 0, Column::rare_bytes, bytes);
    expect_refused(parts.build(), "column value count", ItchStoreErrorKind::bad_segment);
}

// --- Column chunks and zstd ----------------------------------------------------------

TEST(ItchStoreCorrupt, AChunkTheCodecRefusesNamesTheColumnAndTheFileByte) {
    StoreParts parts = test::parts_of(base_file());
    ContainerPart& block = parts.block(0, Column::timestamp);
    ASSERT_EQ(block.codec, codec::CodecId::pfor);
    block.bytes.resize(block.bytes.size() / 2);
    const Bytes file = parts.build();
    const auto error = first_error(file);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->kind, ItchStoreErrorKind::codec) << to_string(*error);
    EXPECT_EQ(error->column, "timestamp");
    EXPECT_EQ(error->segment, 0U);
    EXPECT_NE(error->offset, kNoIndex);
    EXPECT_LT(error->offset, file.size());
}

TEST(ItchStoreCorrupt, ARawByteChunkOfTheWrongSizeIsRefused) {
    const std::vector<std::uint64_t> values{1, 2, 3};
    const Bytes three{std::byte{1}, std::byte{2}};
    const auto refused = decode_column(Column::rare_bytes, codec::CodecId::raw, three, 3);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().field, "column bytes");
    EXPECT_EQ(refused.error().column, "rare_bytes");
    EXPECT_FALSE(decode_column(Column::rare_bytes, codec::CodecId::pfor, three, 2).has_value());
}

TEST(ItchStoreCorrupt, ZstdWrapperFields) {
    // A column that took zstd in the base file.
    const Bytes file = base_file();
    StoreParts clean = test::parts_of(file);
    std::optional<std::pair<std::uint32_t, Column>> wrapped;
    for (std::uint32_t s = 0; s < 4 && !wrapped; ++s) {
        for (const ColumnInfo& info : kColumns) {
            if (!info.zstd) {
                continue;
            }
            const auto b = clean.find_block(s, info.id);
            if (b.has_value() && clean.blocks[*b].codec == codec::CodecId::zstd) {
                wrapped = std::pair{s, info.id};
                break;
            }
        }
    }
    // The fixture's columns are small; wrap one by hand if the writer kept none.
    if (!wrapped) {
        ContainerPart& block = clean.block(0, Column::locate);
        Bytes outer;
        ASSERT_TRUE(codec::encode_zstd_payload(static_cast<std::uint16_t>(block.codec), block.bytes,
                                               outer)
                        .has_value());
        block.codec = codec::CodecId::zstd;
        block.bytes = outer;
        wrapped = std::pair{0U, Column::locate};
        ASSERT_FALSE(first_error(clean.build()).has_value());
    }
    const auto [segment, column] = *wrapped;
    struct Case {
        std::string_view field;
        std::function<void(Bytes&)> edit;
    };
    const std::vector<Case> cases{
        {"zstd header", [](Bytes& b) { b.resize(7); }},
        {"zstd inner codec", [](Bytes& b) { put_field(b, 0, 2, 0); }},
        {"zstd inner codec", [](Bytes& b) { put_field(b, 0, 2, 8); }},
        {"zstd reserved", [](Bytes& b) { put_field(b, 2, 2, 1); }},
        {"zstd inner size", [](Bytes& b) { put_field(b, 4, 4, 0); }},
        {"zstd inner size", [](Bytes& b) { put_field(b, 4, 4, 0xFFFF'FFFFU); }},
        {"zstd frame", [](Bytes& b) { put_field(b, 4, 4, get_field(b, 4, 4) - 1); }},
        {"zstd frame", [](Bytes& b) { b.resize(b.size() - 1); }},
        {"zstd frame", [](Bytes& b) { b.push_back(std::byte{0}); }},
        {"zstd frame", [](Bytes& b) { b[8] ^= std::byte{0xFF}; }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string{c.field});
        StoreParts parts = clean;
        c.edit(parts.block(segment, column).bytes);
        const auto error = first_error(parts.build());
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->field, c.field) << to_string(*error);
        EXPECT_EQ(error->column, column_info(column).name);
    }
    {
        // A readable inner codec, but not the column's.
        StoreParts parts = clean;
        Bytes& bytes = parts.block(segment, column).bytes;
        const codec::CodecId other = primary_codec(column) == codec::CodecId::pfor
                                         ? codec::CodecId::dictionary
                                         : codec::CodecId::pfor;
        put_field(bytes, 0, 2, static_cast<std::uint64_t>(other));
        const auto error = first_error(parts.build());
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->field, "zstd inner codec") << to_string(*error);
        EXPECT_EQ(error->kind, ItchStoreErrorKind::bad_segment);
    }
}

TEST(ItchStoreCorrupt, ZstdPayloadRoundTripsAndRefusesItsOwnFields) {
    const Bytes inner(1000, std::byte{0x5A});
    Bytes payload;
    ASSERT_TRUE(codec::encode_zstd_payload(static_cast<std::uint16_t>(codec::CodecId::raw), inner,
                                           payload)
                    .has_value());
    auto decoded = codec::decode_zstd_payload(payload, inner.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->inner_codec, static_cast<std::uint16_t>(codec::CodecId::raw));
    EXPECT_EQ(decoded->bytes, inner);
    // A limit below the inner size is refused before any inflation.
    const auto limited = codec::decode_zstd_payload(payload, inner.size() - 1);
    ASSERT_FALSE(limited.has_value());
    EXPECT_EQ(limited.error().field, "zstd inner size");
    // The writer refuses an inner codec that cannot be read back, and empty bytes.
    Bytes out;
    EXPECT_FALSE(codec::encode_zstd_payload(static_cast<std::uint16_t>(codec::CodecId::zstd),
                                            inner, out)
                     .has_value());
    EXPECT_FALSE(
        codec::encode_zstd_payload(static_cast<std::uint16_t>(codec::CodecId::raw), {}, out)
            .has_value());
}

// --- Truncation ---------------------------------------------------------------------

TEST(ItchStoreCorrupt, EveryCutOfAFileIsRefused) {
    const Bytes file = base_file();
    for (std::size_t size = 0; size < file.size(); ++size) {
        const std::span<const std::byte> cut{file.data(), size};
        EXPECT_FALSE(ItchStoreReader::open_bytes(cut).has_value()) << size;
        EXPECT_FALSE(ItchStoreReader::open_bytes(cut, {.checksums = Checksums::skip}).has_value())
            << size;
    }
}

TEST(ItchStoreCorrupt, EverySingleBitFlipFailsTheOracleWithChecksumsOn) {
    const Bytes source = tick_store::test::source_with_tail();
    const Bytes file = test::store_of_source(source, kN);
    for (std::size_t at = 0; at < file.size(); ++at) {
        Bytes flipped = file;
        flipped[at] ^= std::byte{0x10};
        EXPECT_FALSE(harness::verify_itch_store(source, flipped).passed()) << at;
    }
}

// --- The locate runs and the seek index ---------------------------------------------------

// Segment 0 of the reference store: 8 records, 7 typed, locate 1 on typed records 0 to 5
// (two adds) and locate 2 on typed record 6 (one add). One seek group.
TEST(ItchStoreCorrupt, LocateRunFields) {
    const Bytes file = reference_file();
    const auto values = [&](Column column) { return test::column_values(file, 0, column); };
    ASSERT_EQ(values(Column::locate), (std::vector<std::uint64_t>{0, 0, 0, 0, 0, 0, 6}));
    ASSERT_EQ(values(Column::locate_run), (std::vector<std::uint64_t>{1, 2}));
    ASSERT_EQ(values(Column::locate_run_records), (std::vector<std::uint64_t>{6, 1}));
    ASSERT_EQ(values(Column::locate_run_adds), (std::vector<std::uint64_t>{2, 1}));
    struct Case {
        std::string_view field;
        Column column;
        std::vector<std::uint64_t> replacement;
        ItchStoreErrorKind kind = ItchStoreErrorKind::bad_value;
    };
    const std::vector<Case> cases{
        // Ids out of order, and one the bitmap does not have.
        {"locate run order", Column::locate_run, {2, 1}},
        {"locate bitmap", Column::locate_run, {1, 3}, ItchStoreErrorKind::bad_metadata},
        // A run with no record, and runs holding more indices than the typed records.
        {"locate run records", Column::locate_run_records, {0, 7}},
        {"locate run records", Column::locate_run_records, {6, 2}},
        // An index past the last typed record, and one named by two runs.
        {"locate run index", Column::locate, {0, 0, 0, 0, 0, 0, 7}},
        {"locate run index", Column::locate, {0, 0, 0, 0, 0, 2, 6}},
        {"locate run index repeated", Column::locate, {0, 0, 0, 0, 0, 0, 0}},
        // An add count the types do not give.
        {"locate run adds", Column::locate_run_adds, {1, 1}},
        // The directory's columns at different lengths.
        {"column value count", Column::locate_run_records, {7}, ItchStoreErrorKind::bad_segment},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(testing::Message() << c.field << " in " << column_info(c.column).name);
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, c.column, c.replacement);
        expect_refused(parts.build(), c.field, c.kind);
        const auto error = first_error(parts.build());
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->segment, 0U);
    }
    {
        // More runs than typed records, refused from the block's count before decoding.
        StoreParts parts = test::parts_of(file);
        put_field(parts.block(0, Column::locate_run).metadata, kMetaValueCountOffset, 4, 8);
        expect_refused(parts.build(), "column value count", ItchStoreErrorKind::bad_segment);
    }
}

TEST(ItchStoreCorrupt, SeekIndexFields) {
    const Bytes file = reference_file();
    const std::vector<std::uint64_t> seek = test::column_values(file, 0, Column::seek);
    ASSERT_EQ(seek.size(), seek_value_count(8));
    // One group: the maxima, then its min time, max time, imports and symbol counts.
    constexpr std::size_t kMin = kSeekMaxima;
    constexpr std::size_t kMax = kMin + 1;
    constexpr std::size_t kImports = kMax + 1;
    constexpr std::size_t kCounts = kImports + 1;
    ASSERT_EQ(seek[kMin], 10U);
    ASSERT_EQ(seek[kMax], 16U);
    ASSERT_EQ(seek[kImports], 1U);
    ASSERT_EQ(seek[kCounts + seek_symbol_index(0)], 1U);
    ASSERT_EQ(seek[kCounts + seek_symbol_index('D')], 2U);
    ASSERT_EQ(seek[static_cast<std::size_t>(Column::type)], static_cast<std::uint64_t>('R'));
    struct Case {
        std::string_view field;
        std::size_t index;
        std::uint64_t value;
    };
    const std::vector<Case> cases{
        // Structure, refused as the index is parsed.
        {"seek column maximum", static_cast<std::size_t>(Column::type), kMax8 + 1},
        {"seek count", kCounts + seek_symbol_index('D'), 9},
        {"seek group records", kCounts + seek_symbol_index(0), 2},
        {"seek max time", kMax, kMax48 + 1},
        {"seek min time", kMin, 17},
        {"seek imports", kImports, 4},
        // Values in range that the columns do not give, refused after the last record.
        {"seek value", static_cast<std::size_t>(Column::type), 'Q'},
        {"seek value", kMin, 11},
        {"seek value", kMax, 15},
        {"seek value", kImports, 2},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(testing::Message() << c.field << " at " << c.index);
        std::vector<std::uint64_t> changed = seek;
        changed[c.index] = c.value;
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, Column::seek, changed);
        expect_refused(parts.build(), c.field, ItchStoreErrorKind::bad_value);
        const auto error = first_error(parts.build());
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->column, "seek");
        EXPECT_EQ(error->index, c.index);
    }
    {
        // Swapped counts that still sum to the group: D for E, which the types refuse.
        std::vector<std::uint64_t> changed = seek;
        --changed[kCounts + seek_symbol_index('D')];
        ++changed[kCounts + seek_symbol_index('E')];
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, Column::seek, changed);
        expect_refused(parts.build(), "seek value", ItchStoreErrorKind::bad_value);
    }
    {
        // One value short of the layout, refused as a count before anything is decoded.
        std::vector<std::uint64_t> changed = seek;
        changed.pop_back();
        StoreParts parts = test::parts_of(file);
        test::set_column(parts, 0, Column::seek, changed);
        expect_refused(parts.build(), "column value count", ItchStoreErrorKind::bad_segment);
    }
}

TEST(ItchStoreCorrupt, SeekIndexRulesParsedAlone) {
    // One raw record: a group with no typed record must have no times.
    std::vector<std::uint64_t> values(seek_value_count(1), 0);
    const std::size_t counts = kSeekMaxima + 3;
    values[counts + seek_symbol_index(0)] = 1;
    ASSERT_TRUE(parse_seek(values, 1).has_value());
    values[kSeekMaxima + 1] = 5;
    auto refused = parse_seek(values, 1);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().field, "seek max time");
    EXPECT_EQ(refused.error().index, kSeekMaxima + 1);
    values[kSeekMaxima + 1] = 0;
    // The wrong number of values, and no records at all.
    values.push_back(0);
    refused = parse_seek(values, 1);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().field, "seek value count");
    EXPECT_FALSE(parse_seek({}, 0).has_value());
    // Two groups, the second short: each group's counts sum to its own records.
    std::vector<std::uint64_t> two(seek_value_count(kSeekGroupRecords + 3), 0);
    const std::size_t two_counts = kSeekMaxima + 6;
    two[two_counts + (seek_symbol_index(0) * 2)] = kSeekGroupRecords;
    two[two_counts + (seek_symbol_index(0) * 2) + 1] = 3;
    auto parsed = parse_seek(two, kSeekGroupRecords + 3);
    ASSERT_TRUE(parsed.has_value()) << to_string(parsed.error());
    EXPECT_EQ(parsed->group_records(1), 3U);
    EXPECT_EQ(parsed->symbol_before(seek_symbol_index(0), 1), kSeekGroupRecords);
    EXPECT_EQ(parsed->symbol_total(seek_symbol_index(0)), kSeekGroupRecords + 3);
    two[two_counts + (seek_symbol_index(0) * 2) + 1] = 4;
    refused = parse_seek(two, kSeekGroupRecords + 3);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().field, "seek count");
}

// --- The golden file ------------------------------------------------------------------

std::filesystem::path golden_path() {
    return std::filesystem::path{TICK_STORE_GOLDEN_DIR} / "itch-store-v2.tsc";
}

Bytes read_golden() {
    std::ifstream in{golden_path(), std::ios::binary};
    const std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
    Bytes out(chars.size());
    std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// The golden file is the 26-record source and its tail at 8 records a segment.
TEST(ItchStoreGolden, TheWriterProducesTheGoldenFileByteForByte) {
    const Bytes golden = read_golden();
    ASSERT_FALSE(golden.empty()) << golden_path();
    EXPECT_EQ(base_file(), golden);
}

TEST(ItchStoreGolden, TheGoldenFileReadsBackToTheSource) {
    const Bytes golden = read_golden();
    ASSERT_FALSE(golden.empty());
    const Bytes source = tick_store::test::source_with_tail();
    EXPECT_TRUE(harness::verify_itch_store(source, golden).passed());
    auto reader = ItchStoreReader::open_bytes(golden);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader->header().segment_records, kN);
    EXPECT_EQ(reader->segment_count(), 4U);
    ASSERT_TRUE(reader->container().verify_all().has_value());
}

}  // namespace
}  // namespace tick_store::store
