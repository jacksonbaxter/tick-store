// Deliberately broken stores, proving the oracle is not vacuous. Each goes through the
// real writer, parser, decoder and encoder wherever it can, so the fault is seen where
// it would really appear. Corruption at the level of single bits is in
// oracle_flip_test.cpp.

#include "itch_fixtures.hpp"
#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/stub_store.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::harness {
namespace {

using itch::test::bytes;
using store::kStubHeaderSize;
using test::Bytes;
using test::frames_of;
using test::messages_of;
using test::ScratchDir;
using test::store_bytes;

// The index in source_records() of the first record of a type.
std::size_t index_of(char type) {
    const auto& fixtures = itch::test::fixtures();
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        if (fixtures[i].type == type) {
            return i;
        }
    }
    throw std::runtime_error("no such fixture");
}

class BrokenStore : public ::testing::Test {
protected:
    // The source every test here runs against, with its pin.
    const Bytes source = test::source_with_tail();
    const Sha256Digest pin = sha256(source);
    std::vector<itch::Message> messages = messages_of(source);
    Bytes tail = test::truncated_tail();
    ScratchDir dir;

    [[nodiscard]] VerifyReport check() {
        const Bytes file = store_bytes(messages, tail, dir / "store");
        return verify_stub_store(source, file, {.expected_sha256 = pin});
    }

    [[nodiscard]] std::uint64_t offset_of(std::size_t record) const {
        return frames_of(source).frames[record].offset;
    }
};

TEST_F(BrokenStore, TheUnbrokenStorePasses) {
    const VerifyReport report = check();
    EXPECT_TRUE(report.passed());
    EXPECT_EQ(report.stored_sha256, pin);
}

TEST_F(BrokenStore, DroppedRecordIsCaught) {
    const std::size_t dropped = 7;
    messages.erase(std::next(messages.begin(), dropped));
    const VerifyReport report = check();

    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.stored_records, test::kSourceRecordCount - 1);
    EXPECT_EQ(report.missing_records, 1U);
    EXPECT_FALSE(report.hashes_match());
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->record_index, dropped);
    EXPECT_GE(report.first_difference->source_offset, offset_of(dropped));
    EXPECT_LT(report.first_difference->source_offset, offset_of(dropped + 1));
}

TEST_F(BrokenStore, DroppingTheLastRecordIsCaughtAsMissing) {
    messages.pop_back();
    const VerifyReport report = check();
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 0U);
    EXPECT_EQ(report.missing_records, 1U);
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::missing_record);
    EXPECT_EQ(report.first_difference->record_index, test::kSourceRecordCount - 1);
    EXPECT_EQ(report.first_difference->message_type, test::kUnknownType);
}

TEST_F(BrokenStore, DuplicatedRecordIsCaught) {
    const std::size_t duplicated = 9;
    messages.insert(std::next(messages.begin(), duplicated), messages[duplicated]);
    const VerifyReport report = check();

    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.stored_records, test::kSourceRecordCount + 1);
    EXPECT_EQ(report.extra_records, 1U);
    EXPECT_FALSE(report.hashes_match());
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->record_index, duplicated + 1);
}

TEST_F(BrokenStore, DuplicatingTheLastRecordIsCaughtAsExtra) {
    messages.push_back(messages.back());
    const VerifyReport report = check();
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 0U);
    EXPECT_EQ(report.extra_records, 1U);
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::extra_record);
    EXPECT_EQ(report.first_difference->record_index, test::kSourceRecordCount);
    EXPECT_EQ(report.first_difference->message_type, test::kUnknownType);
}

TEST_F(BrokenStore, SwappedAdjacentRecordsAreCaughtAsExactlyTwo) {
    const std::size_t first = 11;
    std::swap(messages[first], messages[first + 1]);
    const VerifyReport report = check();

    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.stored_records, test::kSourceRecordCount);
    EXPECT_EQ(report.differing_records, 2U);
    EXPECT_EQ(report.mismatched_records(), 2U);
    EXPECT_TRUE(report.tail_matches);
    EXPECT_FALSE(report.hashes_match());
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->record_index, first);
}

TEST_F(BrokenStore, SwappedRecordsOfTheSameLengthAreCaught) {
    // 'S' and the 12-byte 'W' have the same length, so only their content differs.
    const std::size_t a = index_of('S');
    const std::size_t b = index_of('W');
    std::swap(messages[a], messages[b]);
    const VerifyReport report = check();
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 2U);
    EXPECT_EQ(report.stored_bytes, report.source_bytes);
    EXPECT_EQ(report.first_difference.value_or(Difference{}).kind, DifferenceKind::bytes);
    EXPECT_EQ(report.first_difference.value_or(Difference{}).bit, 2U);  // 'S' ^ 'W' is 0x04
}

// A cut file no longer adds up, so the parser refuses it before any record.
TEST_F(BrokenStore, TruncatedFileIsRefused) {
    const Bytes file = store_bytes(messages, tail, dir / "store");
    const auto cut = std::span{file}.first(file.size() - 40);
    const VerifyReport report = verify_stub_store(source, cut, {.expected_sha256 = pin});

    EXPECT_FALSE(report.passed());
    ASSERT_TRUE(report.store_error.has_value());
    EXPECT_EQ(report.source_sha256, pin);
    EXPECT_EQ(report.first_difference.value_or(Difference{}).kind, DifferenceKind::store_error);
    EXPECT_EQ(report.first_difference.value_or(Difference{}).record_index, 0U);
}

// A cut that repairs the header too, as a writer that died and lied would leave.
TEST_F(BrokenStore, TruncatedRecordsWithAConsistentHeaderAreCaughtAtTheCut) {
    const Bytes file = store_bytes(messages, {}, dir / "store");
    const std::size_t kept = 20;
    const std::uint64_t kept_bytes = offset_of(kept);
    const auto header = store::encode_stub_header(
        {.record_count = kept, .records_bytes = kept_bytes, .tail_bytes = 0});
    Bytes cut(header.begin(), header.end());
    cut.insert(cut.end(), file.begin() + kStubHeaderSize,
               file.begin() + static_cast<std::ptrdiff_t>(kStubHeaderSize + kept_bytes));

    const VerifyReport report = verify_stub_store(source, cut);
    EXPECT_FALSE(report.passed());
    EXPECT_FALSE(report.store_error.has_value());
    EXPECT_EQ(report.stored_records, kept);
    EXPECT_EQ(report.missing_records, test::kSourceRecordCount - kept);
    ASSERT_TRUE(report.first_difference.has_value());
    EXPECT_EQ(report.first_difference->kind, DifferenceKind::missing_record);
    EXPECT_EQ(report.first_difference->record_index, kept);
    EXPECT_EQ(report.first_difference->source_offset, offset_of(kept));
}

// Cut mid-record, with the sizes repaired to fit: only the frame walk can see it.
TEST_F(BrokenStore, RecordCutInTheMiddleIsRefusedAtThatRecord) {
    const Bytes file = store_bytes(messages, {}, dir / "store");
    const std::size_t kept = 5;
    const std::uint64_t cut_bytes = offset_of(kept) + 7;
    const auto header = store::encode_stub_header(
        {.record_count = kept + 1, .records_bytes = cut_bytes, .tail_bytes = 0});
    Bytes cut(header.begin(), header.end());
    cut.insert(cut.end(), file.begin() + kStubHeaderSize,
               file.begin() + static_cast<std::ptrdiff_t>(kStubHeaderSize + cut_bytes));

    const VerifyReport report = verify_stub_store(source, cut);
    EXPECT_FALSE(report.passed());
    ASSERT_TRUE(report.store_error.has_value());
    EXPECT_EQ(report.store_error->store_offset, kStubHeaderSize + offset_of(kept));
    EXPECT_EQ(report.first_difference.value_or(Difference{}).record_index, kept);
    EXPECT_FALSE(report.stored_sha256.has_value());
}

TEST_F(BrokenStore, AlteredHReservedByteIsCaught) {
    const std::size_t index = index_of('H');
    auto& action = std::get<itch::StockTradingAction>(messages[index]);
    const std::byte original = action.reserved;
    action.reserved = original ^ std::byte{0x01};
    const VerifyReport report = check();

    constexpr std::size_t kReservedOffset = 20;  // from the specification's table
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 1U);
    ASSERT_TRUE(report.first_difference.has_value());
    const Difference& first = *report.first_difference;
    EXPECT_EQ(first.kind, DifferenceKind::bytes);
    EXPECT_EQ(first.record_index, index);
    EXPECT_EQ(first.source_offset, offset_of(index) + itch::kFramePrefixSize + kReservedOffset);
    EXPECT_EQ(first.bit, 0U);
    EXPECT_EQ(first.expected_byte, std::to_integer<std::uint8_t>(original));
    EXPECT_EQ(first.message_type, 'H');
}

TEST_F(BrokenStore, ZeroedTrackingNumberIsCaught) {
    const std::size_t index = index_of('A');
    auto& add = std::get<itch::AddOrder>(messages[index]);
    ASSERT_NE(add.header.tracking_number, 0U) << "the fixture must be able to show this fault";
    add.header.tracking_number = 0;
    const VerifyReport report = check();

    constexpr std::size_t kTrackingOffset = 3;  // from the specification's table
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 1U);
    ASSERT_TRUE(report.first_difference.has_value());
    const Difference& first = *report.first_difference;
    EXPECT_EQ(first.record_index, index);
    EXPECT_EQ(first.source_offset, offset_of(index) + itch::kFramePrefixSize + kTrackingOffset);
    EXPECT_EQ(first.message_type, 'A');
}

TEST_F(BrokenStore, EveryTrackingNumberZeroedIsCaughtInEveryRecordThatHasOne) {
    for (itch::Message& message : messages) {
        std::visit(
            []<class M>(M& typed) {
                if constexpr (!std::same_as<M, itch::RawMessage>) {
                    typed.header.tracking_number = 0;
                }
            },
            message);
    }
    const VerifyReport report = check();
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, itch::kMessageTypeCount);
}

TEST_F(BrokenStore, WrongTailByteIsCaught) {
    tail[2] ^= std::byte{0x40};
    const VerifyReport report = check();

    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.mismatched_records(), 0U);
    EXPECT_FALSE(report.tail_matches);
    EXPECT_FALSE(report.hashes_match());
    ASSERT_TRUE(report.first_difference.has_value());
    const Difference& first = *report.first_difference;
    EXPECT_EQ(first.kind, DifferenceKind::tail_bytes);
    EXPECT_EQ(first.record_index, test::kSourceRecordCount);
    EXPECT_EQ(first.source_offset, source.size() - tail.size() + 2U);
    EXPECT_EQ(first.bit, 6U);
    EXPECT_FALSE(first.message_type.has_value());
}

TEST_F(BrokenStore, MissingTailIsCaught) {
    tail.clear();
    const VerifyReport report = check();
    EXPECT_FALSE(report.passed());
    EXPECT_FALSE(report.tail_matches);
    EXPECT_EQ(report.stored_tail_bytes, 0U);
    EXPECT_EQ(report.first_difference.value_or(Difference{}).kind, DifferenceKind::tail_length);
}

TEST_F(BrokenStore, TailWhereTheSourceHasNoneIsCaught) {
    const Bytes clean = test::source_records();
    const Bytes file = store_bytes(messages_of(clean), bytes({0x41}), dir / "store");
    const VerifyReport report = verify_stub_store(clean, file);
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.source_tail_bytes, 0U);
    EXPECT_EQ(report.stored_tail_bytes, 1U);
    EXPECT_EQ(report.first_difference.value_or(Difference{}).kind, DifferenceKind::tail_length);
}

// A record stored one byte longer, with a true prefix for its new length, so the
// walk of the store stays perfectly in step and only the length tells.
TEST_F(BrokenStore, WrongFrameLengthIsCaughtAtThePrefix) {
    const std::size_t index = index_of('D');
    const Bytes longer = [&] {
        const auto payload = frames_of(source).frames[index].payload;
        Bytes out(payload.begin(), payload.end());
        out.push_back(std::byte{0x00});
        return out;
    }();
    messages[index] = itch::RawMessage{.bytes = longer, .reason = itch::RawReason::wrong_length};
    const VerifyReport report = check();

    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 1U);
    EXPECT_EQ(report.stored_records, test::kSourceRecordCount);
    ASSERT_TRUE(report.first_difference.has_value());
    const Difference& first = *report.first_difference;
    EXPECT_EQ(first.kind, DifferenceKind::bytes);
    EXPECT_EQ(first.record_index, index);
    EXPECT_EQ(first.source_offset, offset_of(index) + 1U);  // the prefix's low byte
    EXPECT_EQ(first.expected_size + 1U, first.actual_size);
    EXPECT_EQ(first.message_type, 'D');
}

// Whatever a reader is, it cannot ask for the source: no argument carries it.
static_assert(std::is_same_v<decltype(&RecordReader::next),
                             std::expected<RecordReader::Record, RecordReadError> (RecordReader::*)()>);
static_assert(std::is_same_v<
              decltype(&RecordReader::tail),
              std::expected<std::span<const std::byte>, RecordReadError> (RecordReader::*)()>);

// Wired to the source by hand, which no tool does: it returns views of the source.
class EchoReader final : public RecordReader {
public:
    explicit EchoReader(std::span<const std::byte> source) noexcept
        : source_(source), cursor_(source) {}

    [[nodiscard]] std::expected<Record, RecordReadError> next() override {
        const auto frame = cursor_.next();
        if (!frame.has_value()) {
            return Record{std::nullopt};
        }
        return Record{
            source_.subspan(frame->offset, itch::kFramePrefixSize + frame->payload.size())};
    }

    [[nodiscard]] std::expected<std::span<const std::byte>, RecordReadError> tail() override {
        return cursor_.remaining();
    }

private:
    std::span<const std::byte> source_;
    itch::FrameCursor cursor_;
};

TEST_F(BrokenStore, AReaderEchoingTheSourceIsCaughtEvenThoughEveryByteMatches) {
    EchoReader echo{source};
    const VerifyReport report = verify(source, echo, {.expected_sha256 = pin});
    EXPECT_EQ(report.mismatched_records(), 0U);
    EXPECT_TRUE(report.tail_matches);
    EXPECT_TRUE(report.hashes_match());
    EXPECT_EQ(report.aliased_records, test::kSourceRecordCount + 1U);  // the tail too
    EXPECT_FALSE(report.passed());
}

// The way a tool could be fooled is to be given the source as the store. The parser
// refuses it at the magic, so bytes that are not a store never reach the comparison.
TEST_F(BrokenStore, TheSourceGivenAsTheStoreIsRefused) {
    const VerifyReport report = verify_stub_store(source, source, {.expected_sha256 = pin});
    EXPECT_FALSE(report.passed());
    ASSERT_TRUE(report.store_error.has_value());
    EXPECT_NE(report.store_error->message.find("bad magic"), std::string::npos);
    EXPECT_EQ(report.stored_records, 0U);
}

// The store answers from what it stored: changing the source after ingest shows up,
// which it could not if any reader were looking at the source.
TEST_F(BrokenStore, TheStoreIgnoresTheSourceItIsComparedWith) {
    const Bytes file = store_bytes(messages, tail, dir / "store");
    Bytes changed = source;
    changed[frames_of(source).frames[3].offset + 12] ^= std::byte{0x08};
    const VerifyReport report = verify_stub_store(changed, file);
    EXPECT_FALSE(report.passed());
    EXPECT_EQ(report.differing_records, 1U);
    EXPECT_EQ(report.stored_sha256, pin);
    EXPECT_EQ(report.source_sha256, sha256(changed));
}

}  // namespace
}  // namespace tick_store::harness
