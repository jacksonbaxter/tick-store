// The oracle against hand-made readers: passing, every way a walk can end, what a
// mismatch reports, and the hash comparisons.

#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "test_reader.hpp"
#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/sha256.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tick_store::harness {
namespace {

using itch::test::bytes;
using test::Bytes;
using test::frames_of;
using test::ListReader;
using test::list_of;

TEST(Oracle, PassesWhenEveryRecordAndTheTailAgree) {
    const Bytes source = test::source_with_tail();
    ListReader reader = list_of(source);
    const VerifyReport report = verify(source, reader, {.expected_sha256 = sha256(source)});

    EXPECT_TRUE(report.passed());
    EXPECT_EQ(report.source_records, test::kSourceRecordCount);
    EXPECT_EQ(report.stored_records, test::kSourceRecordCount);
    EXPECT_EQ(report.source_bytes, source.size());
    EXPECT_EQ(report.stored_bytes, source.size());
    EXPECT_EQ(report.source_tail_bytes, test::truncated_tail().size());
    EXPECT_EQ(report.stored_tail_bytes, test::truncated_tail().size());
    EXPECT_TRUE(report.tail_matches);
    EXPECT_EQ(report.source_sha256, sha256(source));
    EXPECT_EQ(report.stored_sha256, sha256(source));
    EXPECT_EQ(report.mismatched_records(), 0U);
    EXPECT_FALSE(report.first_difference.has_value());
    EXPECT_GE(report.wall_seconds, 0.0);
}

TEST(Oracle, PassesOnAnEmptySourceAndAnEmptyStore) {
    const Bytes source;
    ListReader reader{{}, {}};
    const VerifyReport report = verify(source, reader);
    EXPECT_TRUE(report.passed());
    EXPECT_EQ(report.source_records, 0U);
    EXPECT_EQ(to_hex(report.source_sha256),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Oracle, PassesOnASourceThatIsAllTail) {
    const Bytes source = bytes({0x00, 0x00, 0x41});  // a zero-length frame and a stray byte
    ListReader reader{{}, source};
    const VerifyReport report = verify(source, reader);
    EXPECT_TRUE(report.passed());
    EXPECT_EQ(report.source_records, 0U);
    EXPECT_EQ(report.source_tail_bytes, 3U);
}

TEST(Oracle, ReportsTheFirstDifferenceInFullAndKeepsCounting) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    const std::size_t third = frames_of(source).frames[3].offset;
    ASSERT_GT(reader.records[3].size(), 12U);

    reader.records[3][12] ^= std::byte{0x10};  // payload byte 10, in the timestamp
    reader.records[7][5] ^= std::byte{0x01};
    reader.records[7][6] ^= std::byte{0x80};
    reader.records[20][2] ^= std::byte{0xFF};

    const VerifyReport report = verify(source, reader);
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 3U);
    EXPECT_EQ(report.mismatched_records(), 3U);
    EXPECT_EQ(report.source_records, test::kSourceRecordCount);
    EXPECT_FALSE(report.hashes_match());

    ASSERT_TRUE(report.first_difference.has_value());
    const Difference& first = *report.first_difference;
    EXPECT_EQ(first.kind, DifferenceKind::bytes);
    EXPECT_EQ(first.record_index, 3U);
    EXPECT_EQ(first.source_offset, third + 12U);
    EXPECT_EQ(first.bit, 4U);
    EXPECT_EQ(first.expected_byte, std::to_integer<std::uint8_t>(source[third + 12U]));
    EXPECT_EQ(first.actual_byte, std::to_integer<std::uint8_t>(source[third + 12U]) ^ 0x10U);
    EXPECT_EQ(first.message_type, 'Y');
}

TEST(Oracle, ReportsTheLowestDifferingBitOfTheFirstDifferingByte) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    reader.records[0][4] ^= std::byte{0b0101'0000};
    const VerifyReport report = verify(source, reader);
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->bit, 4U);
}

TEST(Oracle, CatchesARecordThatStopsShort) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    reader.records[2].pop_back();
    const VerifyReport report = verify(source, reader);
    EXPECT_FALSE(report.passed());
    ASSERT_TRUE(report.first_difference.has_value());
    const Difference& first = *report.first_difference;
    EXPECT_EQ(first.kind, DifferenceKind::length);
    EXPECT_EQ(first.record_index, 2U);
    EXPECT_EQ(first.source_offset, frames_of(source).frames[3].offset - 1U);
    EXPECT_EQ(first.expected_size, frames_of(source).frames[2].payload.size() + 2U);
    EXPECT_EQ(first.actual_size, first.expected_size - 1U);
    EXPECT_FALSE(first.bit.has_value());
}

TEST(Oracle, CatchesARecordThatRunsLong) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    reader.records[2].push_back(std::byte{0x00});
    const VerifyReport report = verify(source, reader);
    EXPECT_FALSE(report.passed());
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::length);
    EXPECT_EQ(report.first_difference->actual_size, report.first_difference->expected_size + 1U);
}

TEST(Oracle, CatchesAnEmptyRecord) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    reader.records[1].clear();
    const VerifyReport report = verify(source, reader);
    EXPECT_FALSE(report.passed());
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::length);
    EXPECT_EQ(report.first_difference->actual_size, 0U);
}

TEST(Oracle, StopsAStoreThatErrorsAndStillHashesTheSource) {
    const Bytes source = test::source_with_tail();
    ListReader reader = list_of(source);
    reader.fail_at = 5;
    reader.error = {.message = "broken", .store_offset = 1234};
    const VerifyReport report = verify(source, reader, {.expected_sha256 = sha256(source)});

    EXPECT_FALSE(report.passed());
    ASSERT_TRUE(report.store_error.has_value());
    EXPECT_EQ(report.store_error->store_offset, 1234U);
    EXPECT_EQ(report.source_sha256, sha256(source));
    EXPECT_FALSE(report.stored_sha256.has_value());
    EXPECT_EQ(report.stored_records, 5U);
    EXPECT_EQ(report.missing_records, test::kSourceRecordCount - 5U);
    EXPECT_FALSE(report.tail_matches);
    EXPECT_TRUE(report.source_matches_pin());
    EXPECT_FALSE(report.stored_matches_pin());
    EXPECT_EQ(reader.calls_after_end, 0U);

    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::store_error);
    EXPECT_EQ(report.first_difference->record_index, 5U);
    EXPECT_EQ(report.first_difference->source_offset, frames_of(source).frames[5].offset);
}

TEST(Oracle, AnEarlierDifferenceStaysFirstWhenTheStoreLaterErrors) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    reader.records[1][8] ^= std::byte{0x02};
    reader.fail_at = 4;
    const VerifyReport report = verify(source, reader);
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::bytes);
    EXPECT_EQ(report.first_difference->record_index, 1U);
    EXPECT_TRUE(report.store_error.has_value());
}

TEST(Oracle, AStoreErrorAfterTheLastSourceRecordIsStillAFailure) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    reader.fail_at = test::kSourceRecordCount;
    const VerifyReport report = verify(source, reader);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.store_error.has_value());
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::store_error);
    EXPECT_EQ(report.first_difference->record_index, test::kSourceRecordCount);
}

TEST(Oracle, AStoreErrorOnTheTailIsAFailure) {
    const Bytes source = test::source_with_tail();
    ListReader reader = list_of(source);
    reader.fail_tail = true;
    const VerifyReport report = verify(source, reader);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.store_error.has_value());
    EXPECT_FALSE(report.stored_sha256.has_value());
    EXPECT_FALSE(report.tail_matches);
}

TEST(Oracle, ADifferentPinFailsEvenWhenTheStreamsAgree) {
    const Bytes source = test::source_records();
    ListReader reader = list_of(source);
    const VerifyReport report = verify(source, reader, {.expected_sha256 = sha256(bytes({1}))});
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.mismatched_records(), 0U);
    EXPECT_TRUE(report.hashes_match());
    EXPECT_FALSE(report.source_matches_pin());
    EXPECT_FALSE(report.stored_matches_pin());
}

}  // namespace
}  // namespace tick_store::harness
