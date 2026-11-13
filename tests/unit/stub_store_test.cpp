// The stub store: its format pinned as bytes, one corrupt fixture per check in the
// parser, truncation at every length, and the writer's commit behaviour.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"
#include "tick_store/store/stub_store.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::store {
namespace {

using itch::test::bytes;
using itch::test::exact_copy;
using test::Bytes;
using test::messages_of;
using test::read_file;
using test::ScratchDir;
using test::store_bytes;
using test::write_file;

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

// One System Event, code 'C', and a two-byte tail, all typed by hand.
const Bytes& golden_store() {
    static const Bytes file = bytes({
        'T', 'I', 'C', 'K', 'S', 'T', 'U', 'B',                  // magic
        0x01, 0x00, 0x00, 0x00,                                  // version
        0x28, 0x00, 0x00, 0x00,                                  // header size
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,          // record count
        0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,          // records bytes
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,          // tail bytes
        0x00, 0x0C,                                              // frame prefix
        'S',                                                     // type
        0x00, 0x00,                                              // stock locate
        0x00, 0x07,                                              // tracking number
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,                      // timestamp
        'C',                                                     // event code
        0xAB, 0xCD,                                              // tail
    });
    return file;
}

const itch::SystemEvent kGoldenEvent{
    .header = {.stock_locate = 0, .tracking_number = 7, .timestamp_ns = 0x010203040506ULL},
    .event_code = std::byte{'C'}};

// A valid file with the given header fields overwritten, as raw little-endian values.
Bytes with_field(Bytes file, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) {
        file[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
    return exact_copy(file);
}

// The error a whole walk of `file` ends in, at open or on the way.
std::optional<StoreError> walk_error(std::span<const std::byte> file) {
    auto parser = StoreParser::open(file);
    if (!parser.has_value()) {
        return parser.error();
    }
    for (auto record = parser->next();; record = parser->next()) {
        if (!record.has_value()) {
            return record.error();
        }
        if (!record->has_value()) {
            return std::nullopt;
        }
    }
}

TEST(LittleEndian, ReadsAndWritesLeastSignificantByteFirst) {
    const Bytes data = bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    EXPECT_EQ(read_le<1>(data, 0), 0x01U);
    EXPECT_EQ(read_le<4>(data, 1), 0x05040302U);
    EXPECT_EQ(read_le<8>(data, 0), 0x0807060504030201ULL);

    Bytes out(8);
    ASSERT_TRUE(write_le<8>(out, 0, 0x0807060504030201ULL));
    EXPECT_EQ(out, data);
}

TEST(LittleEndian, RefusesAnythingThatDoesNotFit) {
    const Bytes data(4);
    EXPECT_FALSE(read_le<4>(data, 1).has_value());
    EXPECT_FALSE(read_le<1>(data, 4).has_value());
    EXPECT_FALSE(read_le<8>(data, 0).has_value());
    EXPECT_FALSE(read_le<1>(data, std::numeric_limits<std::size_t>::max()).has_value());

    Bytes out(4, std::byte{0x5A});
    EXPECT_FALSE(write_le<4>(out, 1, 0));
    EXPECT_EQ(out, Bytes(4, std::byte{0x5A}));
}

TEST(StubStoreFormat, HeaderPinnedAsBytes) {
    const auto header =
        encode_stub_header({.record_count = 1, .records_bytes = 14, .tail_bytes = 2});
    EXPECT_TRUE(std::ranges::equal(header, std::span{golden_store()}.first(kStubHeaderSize)));
}

TEST(StubStoreFormat, WriterProducesTheGoldenBytes) {
    const ScratchDir dir;
    const Bytes tail = bytes({0xAB, 0xCD});
    EXPECT_EQ(store_bytes({kGoldenEvent}, tail, dir / "store"), golden_store());
}

TEST(StubStoreFormat, ParserReadsTheGoldenBytes) {
    const Bytes file = exact_copy(golden_store());
    auto parser = StoreParser::open(file);
    ASSERT_TRUE(parser.has_value());
    EXPECT_EQ(parser->header(),
              (StubHeader{.record_count = 1, .records_bytes = 14, .tail_bytes = 2}));
    EXPECT_EQ(Bytes(parser->tail().begin(), parser->tail().end()), bytes({0xAB, 0xCD}));

    auto record = parser->next();
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->has_value());
    EXPECT_EQ(**record, itch::Message{kGoldenEvent});

    const auto end = parser->next();
    ASSERT_TRUE(end.has_value());
    EXPECT_FALSE(end->has_value());
}

TEST(StubStoreFormat, EmptyStoreHoldsOnlyAHeader) {
    const ScratchDir dir;
    const Bytes file = store_bytes({}, {}, dir / "store");
    EXPECT_EQ(file.size(), kStubHeaderSize);
    EXPECT_EQ(walk_error(file), std::nullopt);
}

// One corrupt fixture per check in the parser, each asserting the exact error (T1).

TEST(StubStoreParser, RefusesAFileShorterThanAHeader) {
    const Bytes empty;
    EXPECT_EQ(StoreParser::open(empty).error(),
              (StoreError{.kind = StoreErrorKind::file_too_small,
                          .field = "file",
                          .offset = 0,
                          .expected = 40,
                          .actual = 0}));
    const Bytes short_file = exact_copy(std::span{golden_store()}.first(39));
    EXPECT_EQ(StoreParser::open(short_file).error().actual, 39U);
}

TEST(StubStoreParser, RefusesBadMagic) {
    const Bytes file = with_field(golden_store(), 7, 1, 'X');
    EXPECT_EQ(StoreParser::open(file).error(),
              (StoreError{.kind = StoreErrorKind::bad_magic,
                          .field = "magic",
                          .offset = 0,
                          .expected = 0x425554534B434954ULL,
                          .actual = 0x585554534B434954ULL}));
}

TEST(StubStoreParser, RefusesAnyOtherVersion) {
    for (const std::uint64_t version : {0ULL, 2ULL, 0xFFFFFFFFULL}) {
        const Bytes file = with_field(golden_store(), 8, 4, version);
        EXPECT_EQ(StoreParser::open(file).error(),
                  (StoreError{.kind = StoreErrorKind::unsupported_version,
                              .field = "version",
                              .offset = 8,
                              .expected = 1,
                              .actual = version}));
    }
}

TEST(StubStoreParser, RefusesAnyOtherHeaderSize) {
    for (const std::uint64_t size : {0ULL, 39ULL, 41ULL}) {
        const Bytes file = with_field(golden_store(), 12, 4, size);
        EXPECT_EQ(StoreParser::open(file).error(),
                  (StoreError{.kind = StoreErrorKind::bad_header_size,
                              .field = "header_size",
                              .offset = 12,
                              .expected = 40,
                              .actual = size}));
    }
}

// M5: a stored length past the end of the file is refused before any read of it.
TEST(StubStoreParser, RefusesRecordsBytesPastTheEndOfTheFile) {
    const Bytes file = with_field(golden_store(), 24, 8, 17);
    EXPECT_EQ(StoreParser::open(file).error(),
              (StoreError{.kind = StoreErrorKind::bad_section_sizes,
                          .field = "records_bytes",
                          .offset = 24,
                          .expected = 17,
                          .actual = 16}));
}

TEST(StubStoreParser, RefusesTailBytesThatDisagreeWithTheFile) {
    for (const std::uint64_t tail : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{3}, kMax}) {
        const Bytes file = with_field(golden_store(), 32, 8, tail);
        EXPECT_EQ(StoreParser::open(file).error(),
                  (StoreError{.kind = StoreErrorKind::bad_section_sizes,
                              .field = "tail_bytes",
                              .offset = 32,
                              .expected = tail,
                              .actual = 2}));
    }
}

// D7: section sizes whose sum wraps to the right answer must still fail.
TEST(StubStoreParser, RefusesSectionSizesThatWrapAround) {
    Bytes file = with_field(golden_store(), 24, 8, kMax);
    file = with_field(file, 32, 8, 17);  // kMax + 17 wraps to 16, the real body size
    EXPECT_EQ(StoreParser::open(file).error().kind, StoreErrorKind::bad_section_sizes);
    EXPECT_EQ(StoreParser::open(file).error().field, "records_bytes");
}

TEST(StubStoreParser, RefusesACountTooHigh) {
    const Bytes file = with_field(golden_store(), 16, 8, 2);
    EXPECT_EQ(walk_error(file), (StoreError{.kind = StoreErrorKind::record_count_mismatch,
                                            .field = "record_count",
                                            .offset = 16,
                                            .expected = 2,
                                            .actual = 1}));
}

TEST(StubStoreParser, RefusesACountTooLow) {
    const Bytes file = with_field(golden_store(), 16, 8, 0);
    EXPECT_EQ(walk_error(file), (StoreError{.kind = StoreErrorKind::record_count_mismatch,
                                            .field = "record_count",
                                            .offset = 16,
                                            .expected = 0,
                                            .actual = 1}));
}

// D8: a huge count allocates nothing, and is refused when the frames run out.
TEST(StubStoreParser, RefusesTheLargestCountInATinyFile) {
    const Bytes file = with_field(golden_store(), 16, 8, kMax);
    EXPECT_EQ(StoreError{walk_error(file).value_or(StoreError{})}.kind,
              StoreErrorKind::record_count_mismatch);
}

// Layout: a header claiming `count` records and these record bytes, with no tail.
Bytes file_with_records(std::uint64_t count, const Bytes& records) {
    const auto header = encode_stub_header(
        {.record_count = count, .records_bytes = records.size(), .tail_bytes = 0});
    Bytes file(header.begin(), header.end());
    file.insert(file.end(), records.begin(), records.end());
    return exact_copy(file);
}

TEST(StubStoreParser, RefusesAHalfPrefixAtTheEndOfTheRecords) {
    const Bytes file = file_with_records(1, bytes({0x00}));
    EXPECT_EQ(walk_error(file), (StoreError{.kind = StoreErrorKind::truncated_record,
                                            .field = "frame prefix",
                                            .offset = 40,
                                            .expected = 2,
                                            .actual = 1}));
}

TEST(StubStoreParser, RefusesAFrameRunningPastTheEndOfTheRecords) {
    const Bytes file = file_with_records(1, bytes({0x00, 0x05, 'S', 0x00}));
    EXPECT_EQ(walk_error(file), (StoreError{.kind = StoreErrorKind::truncated_record,
                                            .field = "frame body",
                                            .offset = 40,
                                            .expected = 5,
                                            .actual = 2}));
}

// A frame cannot read into the tail, however much tail there is.
TEST(StubStoreParser, FrameCannotRunIntoTheTail) {
    Bytes file = exact_copy(golden_store());
    file[41] = std::byte{0x0D};  // the frame now wants 13 bytes, and 12 are left
    EXPECT_EQ(StoreError{walk_error(file).value_or(StoreError{})}.kind,
              StoreErrorKind::truncated_record);
}

TEST(StubStoreParser, RefusesAZeroLengthFrame) {
    const Bytes file = file_with_records(1, bytes({0x00, 0x00}));
    EXPECT_EQ(walk_error(file), (StoreError{.kind = StoreErrorKind::zero_length_record,
                                            .field = "record",
                                            .offset = 40,
                                            .expected = 1,
                                            .actual = 0}));
}

TEST(StubStoreParser, AnErrorRepeats) {
    const Bytes file = file_with_records(1, bytes({0x00, 0x00}));
    auto parser = StoreParser::open(file);
    ASSERT_TRUE(parser.has_value());
    const StoreError first = parser->next().error();
    EXPECT_EQ(parser->next().error(), first);
}

TEST(StubStoreParser, TheEndRepeats) {
    const Bytes file = exact_copy(golden_store());
    auto parser = StoreParser::open(file);
    ASSERT_TRUE(parser.has_value());
    ASSERT_TRUE(parser->next()->has_value());
    EXPECT_FALSE(parser->next()->has_value());
    EXPECT_FALSE(parser->next()->has_value());
}

// D10: every cut of a real file is refused, at every length, in an exact-size buffer.
TEST(StubStoreParser, EveryTruncationIsRefused) {
    const ScratchDir dir;
    const Bytes source = test::source_with_tail();
    const Bytes file = store_bytes(messages_of(source), test::frames_of(source).tail, dir / "store");
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_TRUE(walk_error(cut).has_value()) << "cut at " << length;
    }
    EXPECT_EQ(walk_error(exact_copy(file)), std::nullopt);
}

TEST(StubStoreParser, ErrorsReadWell) {
    const Bytes file = with_field(golden_store(), 24, 8, 17);
    EXPECT_EQ(to_string(StoreParser::open(file).error()),
              "bad section sizes: records_bytes at offset 24: expected 17, have 16");
    EXPECT_EQ(to_string(StoreError{.kind = StoreErrorKind::io, .field = "open", .actual = 2}),
              "I/O error: open failed with errno 2");
}

TEST(StubStore, EveryRecordComesBackAsTheSameMessage) {
    const ScratchDir dir;
    const Bytes source = test::source_with_tail();
    const auto messages = messages_of(source);
    ASSERT_EQ(messages.size(), test::kSourceRecordCount);
    const auto tail = test::frames_of(source).tail;
    store_bytes(messages, tail, dir / "store");

    const auto mapped = MappedFile::open(dir / "store");
    ASSERT_TRUE(mapped.has_value());
    auto parser = StoreParser::open(mapped->bytes());
    ASSERT_TRUE(parser.has_value());
    EXPECT_EQ(parser->header().record_count, messages.size());
    EXPECT_TRUE(std::ranges::equal(parser->tail(), tail));

    std::vector<itch::Message> read;
    for (auto record = parser->next(); record.has_value() && record->has_value();
         record = parser->next()) {
        read.push_back(**record);
    }
    EXPECT_EQ(read, messages);
}

TEST(StubStore, ARawRecordBorrowsFromTheFile) {
    const ScratchDir dir;
    const Bytes source = test::source_records();
    store_bytes(messages_of(source), {}, dir / "store");

    const auto mapped = MappedFile::open(dir / "store");
    ASSERT_TRUE(mapped.has_value());
    auto parser = StoreParser::open(mapped->bytes());
    ASSERT_TRUE(parser.has_value());
    const auto file = mapped->bytes();
    for (auto record = parser->next(); record->has_value(); record = parser->next()) {
        if (const auto* raw = std::get_if<itch::RawMessage>(&**record)) {
            EXPECT_GE(raw->bytes.data(), file.data());
            EXPECT_LE(raw->bytes.data() + raw->bytes.size(), file.data() + file.size());
        }
    }
}

TEST(StubWriter, RefusesARawMessageWithNoBytes) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    const auto appended = writer->append(itch::RawMessage{});
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(appended.error().kind, StoreErrorKind::unencodable_record);
    // The encoder refuses it, so the error names the encoder's field (review A-5).
    EXPECT_EQ(appended.error().field, "raw bytes");
    EXPECT_EQ(appended.error().expected, 1U);
    EXPECT_EQ(appended.error().actual, 0U);
}

TEST(StubWriter, RefusesATimestampTheWireCannotHold) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    itch::SystemEvent event;
    event.header.timestamp_ns = itch::kMaxTimestampNs + 1;
    const auto appended = writer->append(event);
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(appended.error().kind, StoreErrorKind::unencodable_record);
    EXPECT_EQ(appended.error().field, "timestamp_ns");
}

TEST(StubWriter, RefusesARawMessageNoFrameCanDeclare) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    const Bytes huge(itch::kMaxPayloadSize + 1);
    const auto appended = writer->append(itch::RawMessage{.bytes = huge});
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(appended.error().kind, StoreErrorKind::unencodable_record);
}

TEST(StubWriter, AFailedAppendIsFatal) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    const auto first = writer->append(itch::RawMessage{});
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(writer->append(kGoldenEvent).error(), first.error());
    EXPECT_EQ(writer->finish({}).error(), first.error());
    EXPECT_FALSE(std::filesystem::exists(dir / "store"));
    EXPECT_FALSE(std::filesystem::exists(dir / "store.partial"));
}

TEST(StubWriter, NothingIsPublishedBeforeFinish) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->append(kGoldenEvent).has_value());
    EXPECT_FALSE(std::filesystem::exists(dir / "store"));
    EXPECT_TRUE(std::filesystem::exists(dir / "store.partial"));
    ASSERT_TRUE(writer->finish({}).has_value());
    EXPECT_TRUE(std::filesystem::exists(dir / "store"));
    EXPECT_FALSE(std::filesystem::exists(dir / "store.partial"));
}

// M6: a writer that never reaches its commit point loses nothing that was published.
TEST(StubWriter, AnAbandonedWriterLeavesTheEarlierFileAlone) {
    const ScratchDir dir;
    const Bytes tail = bytes({0xAB, 0xCD});
    const Bytes earlier = store_bytes({kGoldenEvent}, tail, dir / "store");
    {
        auto writer = StubWriter::create(dir / "store");
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->append(kGoldenEvent).has_value());
        ASSERT_TRUE(writer->append(kGoldenEvent).has_value());
    }
    EXPECT_EQ(read_file(dir / "store"), earlier);
    EXPECT_FALSE(std::filesystem::exists(dir / "store.partial"));
}

// M6: what a crash before the commit point leaves behind cannot be taken for a store.
TEST(StubWriter, APartialFileNeverParses) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->append(kGoldenEvent).has_value());
    const Bytes partial = read_file(dir / "store.partial");
    EXPECT_EQ(partial.size(), kStubHeaderSize);  // the record is still in the buffer
    EXPECT_EQ(StoreParser::open(partial).error().kind, StoreErrorKind::bad_magic);
}

TEST(StubWriter, ALaterAppendOrFinishFails) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->finish({}).has_value());
    EXPECT_FALSE(writer->append(kGoldenEvent).has_value());
    EXPECT_FALSE(writer->finish({}).has_value());
}

TEST(StubWriter, ReportsAPathThatCannotBeCreated) {
    const ScratchDir dir;
    const auto writer = StubWriter::create(dir / "missing" / "store");
    ASSERT_FALSE(writer.has_value());
    EXPECT_EQ(writer.error().kind, StoreErrorKind::io);
    EXPECT_EQ(writer.error().field, "open");
    EXPECT_EQ(writer.error().actual, static_cast<std::uint64_t>(ENOENT));
}

// A failure at the commit point is reported, and no partial file stays behind.
TEST(StubWriter, ReportsARenameThatFails) {
    const ScratchDir dir;
    std::filesystem::create_directory(dir / "store");
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    const auto finished = writer->finish({});
    ASSERT_FALSE(finished.has_value());
    EXPECT_EQ(finished.error().field, "rename");
    EXPECT_FALSE(std::filesystem::exists(dir / "store.partial"));
}

TEST(StubWriter, SurvivesAMove) {
    const ScratchDir dir;
    auto writer = StubWriter::create(dir / "store");
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->append(kGoldenEvent).has_value());
    StubWriter moved{std::move(*writer)};
    ASSERT_TRUE(moved.finish(bytes({0xAB, 0xCD})).has_value());
    EXPECT_EQ(read_file(dir / "store"), golden_store());
}

// Bigger than the write buffer, so a flush happens on the way.
TEST(StubWriter, ManyRecordsCrossTheBufferBoundary) {
    const ScratchDir dir;
    const std::vector<itch::Message> messages(200'000, kGoldenEvent);
    const Bytes file = store_bytes(messages, {}, dir / "store");
    EXPECT_EQ(file.size(), kStubHeaderSize + (messages.size() * 14));
    EXPECT_EQ(walk_error(file), std::nullopt);
}

TEST(MappedFile, MapsTheWholeFile) {
    const ScratchDir dir;
    const Bytes content = bytes({1, 2, 3, 4, 5});
    write_file(dir / "file", content);
    const auto mapped = MappedFile::open(dir / "file");
    ASSERT_TRUE(mapped.has_value());
    EXPECT_TRUE(std::ranges::equal(mapped->bytes(), content));
}

TEST(MappedFile, AnEmptyFileIsAnEmptySpan) {
    const ScratchDir dir;
    write_file(dir / "file", {});
    const auto mapped = MappedFile::open(dir / "file");
    ASSERT_TRUE(mapped.has_value());
    EXPECT_TRUE(mapped->bytes().empty());
}

TEST(MappedFile, ReportsAMissingFile) {
    const ScratchDir dir;
    const auto mapped = MappedFile::open(dir / "absent");
    ASSERT_FALSE(mapped.has_value());
    EXPECT_EQ(mapped.error(), (StoreError{.kind = StoreErrorKind::io,
                                          .field = "open",
                                          .actual = static_cast<std::uint64_t>(ENOENT)}));
}

TEST(MappedFile, RefusesADirectory) {
    const ScratchDir dir;
    const auto mapped = MappedFile::open(dir.path());
    ASSERT_FALSE(mapped.has_value());
    EXPECT_EQ(mapped.error().field, "not a regular file");
}

// A FIFO with no writer is refused at once (review C-3): open(2) without O_NONBLOCK would
// wait for a writer before the regular-file check ran. A blocked open cannot be bounded by
// counting work, so if it has not returned after a wait far past any open's cost, the test
// opens the write end itself, which releases it, and fails rather than hang the suite.
TEST(MappedFile, RefusesAFifoWithoutWaitingForAWriter) {
    const ScratchDir dir;
    const std::filesystem::path fifo = dir / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    auto opened = std::async(std::launch::async, [&fifo] { return MappedFile::open(fifo); });
    if (opened.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        const int writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        [[maybe_unused]] const auto released = opened.get();
        if (writer >= 0) {
            ::close(writer);
        }
        FAIL() << "MappedFile::open waited for a writer on a FIFO";
    }
    const auto mapped = opened.get();
    ASSERT_FALSE(mapped.has_value());
    EXPECT_EQ(mapped.error().field, "not a regular file");
}

TEST(MappedFile, AMoveKeepsTheMappingAndItsAddress) {
    const ScratchDir dir;
    const Bytes content = bytes({1, 2, 3});
    write_file(dir / "file", content);
    auto mapped = MappedFile::open(dir / "file");
    ASSERT_TRUE(mapped.has_value());
    const std::span<const std::byte> view = mapped->bytes();

    MappedFile moved{std::move(*mapped)};
    EXPECT_EQ(moved.bytes().data(), view.data());
    EXPECT_TRUE(std::ranges::equal(view, content));

    write_file(dir / "other", bytes({9}));
    auto other = MappedFile::open(dir / "other");
    ASSERT_TRUE(other.has_value());
    moved = std::move(*other);
    EXPECT_EQ(moved.bytes().size(), 1U);
}

}  // namespace
}  // namespace tick_store::store
