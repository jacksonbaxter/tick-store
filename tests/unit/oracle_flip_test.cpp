// G-2 on a small fixture, exhaustively: every bit of a real store file, flipped in
// turn on disk, must be caught by the oracle or refused by the parser. A bit that
// could be flipped undetected is a bug in the oracle, so nothing here is relaxed to
// suit the data. The source side gets the same treatment.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/flip_bit.hpp"
#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/stub_store.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace tick_store::harness {
namespace {

using store::kStubHeaderSize;
using test::Bytes;
using test::frames_of;
using test::messages_of;
using test::read_file;
using test::ScratchDir;
using test::store_bytes;

struct FlipCounts {
    std::uint64_t flips = 0;
    std::uint64_t in_header = 0;
    std::uint64_t in_prefix_located = 0;
    std::uint64_t in_prefix_refused = 0;
    std::uint64_t in_payload = 0;
    std::uint64_t in_tail = 0;
};

// The frame of `source` holding source byte `offset`, as an index.
std::size_t frame_holding(const std::vector<itch::Frame>& frames, std::uint64_t offset) {
    std::size_t index = 0;
    while (index + 1 < frames.size() && frames[index + 1].offset <= offset) {
        ++index;
    }
    return index;
}

void expect_located(const VerifyReport& report, DifferenceKind kind, std::uint64_t record,
                    std::uint64_t source_offset, unsigned bit, std::span<const std::byte> source) {
    ASSERT_TRUE(report.first_difference.has_value());
    const Difference& first = *report.first_difference;
    EXPECT_EQ(first.kind, kind);
    EXPECT_EQ(first.record_index, record);
    EXPECT_EQ(first.source_offset, source_offset);
    EXPECT_EQ(first.bit, bit);
    const auto value = std::to_integer<std::uint8_t>(source[source_offset]);
    EXPECT_EQ(first.expected_byte, value);
    EXPECT_EQ(first.actual_byte, value ^ (1U << bit));
}

FlipCounts flip_every_bit(const std::filesystem::path& path, std::span<const std::byte> source) {
    const auto [frames, tail] = frames_of(source);
    const Sha256Digest pin = sha256(source);
    const std::uint64_t file_size = std::filesystem::file_size(path);
    const std::uint64_t tail_start = file_size - tail.size();
    FlipCounts counts;

    for (std::uint64_t byte = 0; byte < file_size; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            SCOPED_TRACE(std::format("store byte {} bit {}", byte, bit));
            ++counts.flips;
            const auto flipped = flip_bit(path, byte, bit);
            EXPECT_TRUE(flipped.has_value());

            const auto mapped = store::MappedFile::open(path);
            EXPECT_TRUE(mapped.has_value());
            const VerifyReport report = verify_stub_store(
                source, mapped.has_value() ? mapped->bytes() : std::span<const std::byte>{},
                {.expected_sha256 = pin});

            // The whole point: no bit may pass.
            EXPECT_FALSE(report.passed());

            if (byte < kStubHeaderSize) {
                ++counts.in_header;
                EXPECT_TRUE(report.store_error.has_value());
            } else if (byte >= tail_start) {
                ++counts.in_tail;
                const std::uint64_t offset = byte - kStubHeaderSize;
                expect_located(report, DifferenceKind::tail_bytes, frames.size(), offset, bit,
                               source);
            } else {
                const std::uint64_t offset = byte - kStubHeaderSize;
                const std::size_t record = frame_holding(frames, offset);
                const bool in_prefix = offset < frames[record].offset + itch::kFramePrefixSize;
                if (in_prefix && report.first_difference.has_value() &&
                    report.first_difference->kind == DifferenceKind::store_error) {
                    // A length the parser cannot follow. It must name this frame.
                    ++counts.in_prefix_refused;
                    EXPECT_EQ(report.first_difference->record_index, record);
                    EXPECT_EQ(report.store_error.value_or(RecordReadError{}).store_offset,
                              kStubHeaderSize + frames[record].offset);
                } else {
                    ++(in_prefix ? counts.in_prefix_located : counts.in_payload);
                    expect_located(report, DifferenceKind::bytes, record, offset, bit, source);
                    EXPECT_EQ(report.first_difference.value_or(Difference{}).message_type,
                              std::to_integer<std::uint8_t>(frames[record].payload[0]));
                }
            }

            EXPECT_TRUE(flip_bit(path, byte, bit).has_value());
        }
    }
    return counts;
}

TEST(OracleExhaustiveFlip, EveryBitOfAStoreWithATailIsCaught) {
    const ScratchDir dir;
    const Bytes source = test::source_with_tail();
    const Bytes original =
        store_bytes(messages_of(source), frames_of(source).tail, dir / "store");

    const FlipCounts counts = flip_every_bit(dir / "store", source);

    EXPECT_EQ(counts.flips, original.size() * 8);
    EXPECT_EQ(counts.in_header, kStubHeaderSize * 8);
    EXPECT_EQ(counts.in_tail, test::truncated_tail().size() * 8);
    EXPECT_EQ(counts.in_prefix_located + counts.in_prefix_refused,
              test::kSourceRecordCount * itch::kFramePrefixSize * 8);
    EXPECT_GT(counts.in_prefix_located, 0U);
    EXPECT_GT(counts.in_prefix_refused, 0U);
    EXPECT_GT(counts.in_payload, 0U);
    std::println("flips {}: header {}, prefix located {}, prefix refused {}, payload {}, tail {}",
                 counts.flips, counts.in_header, counts.in_prefix_located, counts.in_prefix_refused,
                 counts.in_payload, counts.in_tail);

    EXPECT_EQ(read_file(dir / "store"), original);
    const VerifyReport clean = verify_stub_store(source, original);
    EXPECT_TRUE(clean.passed());
}

TEST(OracleExhaustiveFlip, EveryBitOfAStoreWithNoTailIsCaught) {
    const ScratchDir dir;
    const Bytes source = test::source_records();
    const Bytes original = store_bytes(messages_of(source), {}, dir / "store");

    const FlipCounts counts = flip_every_bit(dir / "store", source);

    EXPECT_EQ(counts.flips, original.size() * 8);
    EXPECT_EQ(counts.in_tail, 0U);
}

TEST(FlipBit, TwiceRestoresTheFile) {
    const ScratchDir dir;
    const Bytes content = itch::test::bytes({0x00, 0xFF, 0x5A});
    test::write_file(dir / "file", content);

    const auto first = flip_bit(dir / "file", 1, 3);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->before, 0xFFU);
    EXPECT_EQ(first->after, 0xF7U);
    EXPECT_EQ(read_file(dir / "file"), itch::test::bytes({0x00, 0xF7, 0x5A}));

    EXPECT_TRUE(flip_bit(dir / "file", 1, 3).has_value());
    EXPECT_EQ(read_file(dir / "file"), content);
}

TEST(FlipBit, BitZeroIsTheLeastSignificant) {
    const ScratchDir dir;
    test::write_file(dir / "file", itch::test::bytes({0x00}));
    EXPECT_EQ(flip_bit(dir / "file", 0, 0).value_or(FlippedByte{}).after, 0x01U);
    EXPECT_EQ(flip_bit(dir / "file", 0, 7).value_or(FlippedByte{}).after, 0x81U);
}

TEST(FlipBit, RefusesWhatItCannotDoAndChangesNothing) {
    const ScratchDir dir;
    const Bytes content = itch::test::bytes({0x12, 0x34});
    test::write_file(dir / "file", content);

    EXPECT_FALSE(flip_bit(dir / "file", 2, 0).has_value());
    EXPECT_FALSE(flip_bit(dir / "file", 0, 8).has_value());
    EXPECT_FALSE(flip_bit(dir / "file", UINT64_MAX, 0).has_value());
    EXPECT_FALSE(flip_bit(dir / "absent", 0, 0).has_value());
    EXPECT_FALSE(flip_bit(dir.path(), 0, 0).has_value());
    EXPECT_EQ(read_file(dir / "file"), content);
}

// A copy of a source with one bit flipped, against a store of the true source: the
// record compare and the pin both fail.
TEST(OracleSourceFlip, EveryPayloadBitOfACopiedSourceFailsAgainstThePin) {
    const ScratchDir dir;
    const Bytes source = test::source_with_tail();
    const Sha256Digest pin = sha256(source);
    const auto [frames, tail] = frames_of(source);
    const Bytes file = store_bytes(messages_of(source), tail, dir / "store");

    std::uint64_t flips = 0;
    for (std::size_t record = 0; record < frames.size(); ++record) {
        const std::size_t size = frames[record].payload.size();
        const std::size_t start = frames[record].offset + itch::kFramePrefixSize;
        for (std::size_t offset = start; offset < start + size; ++offset) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                ++flips;
                Bytes copy = source;
                copy[offset] ^= static_cast<std::byte>(1U << bit);
                const VerifyReport report =
                    verify_stub_store(copy, file, {.expected_sha256 = pin});
                EXPECT_FALSE(report.passed());
                EXPECT_FALSE(report.source_matches_pin());
                EXPECT_FALSE(report.hashes_match());
                EXPECT_TRUE(report.stored_matches_pin());
                ASSERT_TRUE(report.first_difference.has_value());
                EXPECT_EQ(report.first_difference->record_index, record);
                EXPECT_EQ(report.first_difference->source_offset, offset);
                EXPECT_EQ(report.first_difference->bit, bit);
            }
        }
    }
    EXPECT_GT(flips, 4000U);
}

// The harder case: the flipped copy was ingested too, so source and store agree with
// each other record for record, and the pin is the only thing that can object.
TEST(OracleSourceFlip, AFlippedSourceIngestedWholeFailsOnlyAgainstThePin) {
    const ScratchDir dir;
    const Bytes source = test::source_with_tail();
    const Sha256Digest pin = sha256(source);

    Bytes copy = source;
    copy[frames_of(source).frames[4].offset + 9] ^= std::byte{0x20};
    const Bytes file = store_bytes(messages_of(copy), frames_of(copy).tail, dir / "store");

    const VerifyReport unpinned = verify_stub_store(copy, file);
    EXPECT_TRUE(unpinned.passed());

    const VerifyReport pinned = verify_stub_store(copy, file, {.expected_sha256 = pin});
    EXPECT_FALSE(pinned.passed());
    EXPECT_EQ(pinned.mismatched_records(), 0U);
    EXPECT_TRUE(pinned.hashes_match());
    EXPECT_FALSE(pinned.source_matches_pin());
    EXPECT_FALSE(pinned.stored_matches_pin());
}

}  // namespace
}  // namespace tick_store::harness
