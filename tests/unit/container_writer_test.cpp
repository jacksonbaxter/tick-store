// The container writer's commit: the system calls each durability mode makes and their
// order (M1, M2, M4, M8), an injected failure at every step, fatal and never retried
// (M3), and torn writes (M6): a crash simulated at every step of a commit that
// replaces a file, and the new file cut at every block boundary.

#include "container_fixture.hpp"
#include "recording_file_ops.hpp"
#include "scratch_dir.hpp"
#include "tick_store/codec/registry.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/container_reader.hpp"
#include "tick_store/store/container_writer.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/store_error.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace tick_store::store {
namespace {

using codec::CodecId;
using test::Bytes;
using test::RecordingFileOps;
using ::tick_store::test::ScratchDir;
using Calls = std::vector<std::string>;

StoreError io(const char* call, int error) {
    return {.kind = StoreErrorKind::io, .field = call, .actual = static_cast<std::uint64_t>(error)};
}

// The calls a commit makes after the data, by mode and OS.
Calls commit_calls(Durability durability) {
    if (durability == Durability::page_cache) {
        return {"close file", "rename"};
    }
#ifdef __APPLE__
    return {"full_fsync file", "close file", "rename", "open_directory", "full_fsync directory",
            "close directory"};
#else
    return {"fdatasync file", "close file", "rename", "open_directory", "fsync directory",
            "close directory"};
#endif
}

const char* file_sync_call() {
#ifdef __APPLE__
    return "full_fsync file";
#else
    return "fdatasync file";
#endif
}

const char* directory_sync_call() {
#ifdef __APPLE__
    return "full_fsync directory";
#else
    return "fsync directory";
#endif
}

const char* file_sync_error_field() {
#ifdef __APPLE__
    return "F_FULLFSYNC";
#else
    return "fdatasync";
#endif
}

const char* directory_sync_error_field() {
#ifdef __APPLE__
    return "F_FULLFSYNC directory";
#else
    return "fsync directory";
#endif
}

// A file of several blocks and sections, one payload larger than the writer's buffer
// so the data reaches the kernel in more than one write.
std::vector<ContainerPart> several_parts() {
    std::vector<ContainerPart> parts;
    parts.reserve(8);
    for (std::uint8_t i = 0; i < 5; ++i) {
        parts.push_back({.section_name = "",
                         .codec = CodecId::raw,
                         .metadata = Bytes(i, std::byte{i}),
                         .bytes = Bytes(100U + i, static_cast<std::byte>(0x30U + i))});
    }
    parts.push_back({.section_name = "index", .codec = {}, .metadata = {},
                     .bytes = Bytes(64, std::byte{0x77})});
    parts.push_back({.section_name = "",
                     .codec = CodecId::raw,
                     .metadata = {},
                     .bytes = Bytes((std::size_t{1} << 20U) + 5, std::byte{0x42})});
    parts.push_back({.section_name = "checkpoint.book", .codec = {}, .metadata = {},
                     .bytes = Bytes(9, std::byte{0x11})});
    return parts;
}

std::expected<void, StoreError> write_parts(ContainerWriter& writer,
                                            const std::vector<ContainerPart>& parts) {
    for (const ContainerPart& part : parts) {
        if (part.section_name.empty()) {
            if (auto added = writer.add_block(part.codec, part.metadata, part.bytes);
                !added.has_value()) {
                return std::unexpected(added.error());
            }
        } else if (auto added = writer.add_section(part.section_name, part.bytes);
                   !added.has_value()) {
            return added;
        }
    }
    return writer.commit();
}

std::expected<void, StoreError> write_file(const std::filesystem::path& path,
                                           const std::vector<ContainerPart>& parts,
                                           Durability durability, FileOps& ops) {
    auto writer = ContainerWriter::create(path, durability, ops);
    if (!writer.has_value()) {
        return std::unexpected(writer.error());
    }
    return write_parts(*writer, parts);
}

std::filesystem::path partial(const std::filesystem::path& path) {
    std::filesystem::path out = path;
    out += ".partial";
    return out;
}

// The old file is at `path`, whole, and opens with every CRC good.
void expect_intact(const std::filesystem::path& path, const Bytes& expected) {
    EXPECT_EQ(test::read_bytes(path), expected);
    const auto reader = ContainerReader::open(path);
    ASSERT_TRUE(reader.has_value()) << to_string(reader.error());
    EXPECT_TRUE(reader->verify_all().has_value());
}

// --- Durability modes -------------------------------------------------------------

TEST(ContainerCommit, EachModeMakesItsCallsInOrder) {
    for (const Durability durability : {Durability::page_cache, Durability::device_flush}) {
        SCOPED_TRACE(to_string(durability));
        const ScratchDir dir;
        RecordingFileOps ops;
        ASSERT_TRUE(write_file(dir / "file.tsc", several_parts(), durability, ops).has_value());
        Calls expected{"create_file"};
        for (const std::string& call : commit_calls(durability)) {
            expected.push_back(call);
        }
        EXPECT_EQ(ops.calls_without_writes(), expected);
        EXPECT_GE(ops.count("write"), 2);
        // M1: every write comes before the first flush and the rename.
        const auto last_write = std::ranges::find(ops.calls.rbegin(), ops.calls.rend(), "write");
        const auto first_after = last_write.base();
        ASSERT_NE(first_after, ops.calls.end());
        EXPECT_EQ(*first_after, commit_calls(durability).front());
        expect_intact(dir / "file.tsc", build_container(several_parts()).value());
        EXPECT_FALSE(std::filesystem::exists(partial(dir / "file.tsc")));
    }
}

TEST(ContainerCommit, PageCacheModeNeverFlushes) {
    const ScratchDir dir;
    RecordingFileOps ops;
    ASSERT_TRUE(write_file(dir / "file.tsc", several_parts(), Durability::page_cache, ops)
                    .has_value());
    for (const char* call : {"full_fsync file", "fsync file", "fdatasync file",
                             "full_fsync directory", "fsync directory", "open_directory"}) {
        EXPECT_EQ(ops.count(call), 0) << call;
    }
}

TEST(ContainerCommit, NothingIsAtThePathBeforeCommit) {
    const ScratchDir dir;
    auto writer = ContainerWriter::create(dir / "file.tsc", Durability::device_flush);
    ASSERT_TRUE(writer.has_value());
    EXPECT_EQ(writer->durability(), Durability::device_flush);
    ASSERT_TRUE(writer->add_block(CodecId::raw, {}, Bytes(10)).has_value());
    EXPECT_FALSE(std::filesystem::exists(dir / "file.tsc"));
    EXPECT_TRUE(std::filesystem::exists(partial(dir / "file.tsc")));
    ASSERT_TRUE(writer->commit().has_value());
    EXPECT_TRUE(std::filesystem::exists(dir / "file.tsc"));
    // A committed writer refuses more work.
    EXPECT_FALSE(writer->add_block(CodecId::raw, {}, Bytes(1)).has_value());
    EXPECT_FALSE(writer->commit().has_value());
}

TEST(ContainerCommit, AnAbandonedWriterLeavesThePathAlone) {
    const ScratchDir dir;
    const Bytes old = build_container(test::golden_parts()).value();
    test::write_bytes(dir / "file.tsc", old);
    {
        auto writer = ContainerWriter::create(dir / "file.tsc", Durability::device_flush);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer->add_block(CodecId::raw, {}, Bytes(10)).has_value());
        ContainerWriter moved = std::move(*writer);
        ASSERT_TRUE(moved.add_block(CodecId::raw, {}, Bytes(3)).has_value());
    }
    expect_intact(dir / "file.tsc", old);
    EXPECT_FALSE(std::filesystem::exists(partial(dir / "file.tsc")));
}

// --- Injected failures ------------------------------------------------------------

struct Step {
    const char* call;
    StoreError error;
    // True if the failure comes after the rename, so the new file is at the path.
    bool published;
};

std::vector<Step> failing_steps() {
    return {
        {"create_file", io("open", EACCES), false},
        {"write", io("write", ENOSPC), false},
        {file_sync_call(), io(file_sync_error_field(), EIO), false},
        {"close file", io("close", EIO), false},
        {"rename", io("rename", EXDEV), false},
        {"open_directory", io("open directory", EMFILE), true},
        {directory_sync_call(), io(directory_sync_error_field(), EIO), true},
    };
}

// M3: a failure at any step is returned once, the call is not retried, the writer stays
// failed, and before the rename the old file is untouched and the partial file gone.
TEST(ContainerCommit, AFailureAtAnyStepIsFatalAndNeverRetried) {
    const Bytes old = build_container(test::golden_parts()).value();
    const Bytes fresh = build_container(several_parts()).value();
    for (const Step& step : failing_steps()) {
        SCOPED_TRACE(step.call);
        const ScratchDir dir;
        test::write_bytes(dir / "file.tsc", old);
        RecordingFileOps ops;
        const int error_number = static_cast<int>(step.error.actual.value_or(0));
        // The last write carries the commit record, so failing it fails the commit.
        const int nth = std::string{step.call} == "write" ? 3 : 1;
        ops.fail_on(step.call, error_number, nth);

        auto writer = ContainerWriter::create(dir / "file.tsc", Durability::device_flush, ops);
        if (std::string{step.call} == "create_file") {
            ASSERT_FALSE(writer.has_value());
            EXPECT_EQ(writer.error(), step.error);
            expect_intact(dir / "file.tsc", old);
            continue;
        }
        ASSERT_TRUE(writer.has_value());
        const auto result = write_parts(*writer, several_parts());
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), step.error);
        EXPECT_EQ(ops.count(step.call), nth);
        // Fatal: every later call gives the same error.
        EXPECT_EQ(writer->commit(), std::unexpected(step.error));
        EXPECT_EQ(writer->add_block(CodecId::raw, {}, Bytes(1)), std::unexpected(step.error));
        EXPECT_EQ(writer->add_section("late", {}), std::unexpected(step.error));
        EXPECT_FALSE(std::filesystem::exists(partial(dir / "file.tsc")));
        if (step.published) {
            expect_intact(dir / "file.tsc", fresh);
        } else {
            expect_intact(dir / "file.tsc", old);
            EXPECT_EQ(ops.count("rename"), std::string{step.call} == "rename" ? 1 : 0);
        }
    }
}

// M4 through the writer: on macOS a file system without F_FULLFSYNC falls back to
// fsync, and the commit succeeds.
TEST(ContainerCommit, FallsBackToFsyncOnlyWhereFullFsyncIsUnsupported) {
#ifdef __APPLE__
    const ScratchDir dir;
    RecordingFileOps ops;
    ops.fail_on("full_fsync file", ENOTSUP);
    ops.fail_on("full_fsync directory", EINVAL);
    ASSERT_TRUE(write_file(dir / "file.tsc", several_parts(), Durability::device_flush, ops)
                    .has_value());
    EXPECT_EQ(ops.calls_without_writes(),
              (Calls{"create_file", "full_fsync file", "fsync file", "close file", "rename",
                     "open_directory", "full_fsync directory", "fsync directory",
                     "close directory"}));
#else
    // Linux has no F_FULLFSYNC: the writer must never ask for it.
    const ScratchDir dir;
    RecordingFileOps ops;
    ASSERT_TRUE(write_file(dir / "file.tsc", several_parts(), Durability::device_flush, ops)
                    .has_value());
    EXPECT_EQ(ops.count("full_fsync file"), 0);
    EXPECT_EQ(ops.count("fsync file"), 0);
    EXPECT_EQ(ops.count("fdatasync file"), 1);
#endif
}

// --- Torn writes ------------------------------------------------------------------

// The process ends at the start of `call`, its nth time, while replacing the old file,
// with no destructor run and nothing cleaned up: what a crash leaves.
void crash_at(const std::filesystem::path& path, const char* call, int nth) {
    RecordingFileOps ops;
    ops.on(call, [] { std::_Exit(0); }, nth);
    const auto result = write_file(path, several_parts(), Durability::device_flush, ops);
    // Not reached: the hook ends the process first.
    std::_Exit(result.has_value() ? 3 : 4);
}

struct CrashPoint {
    const char* call;
    int nth;
    // What `<path>.partial` holds after the crash.
    enum class Partial : std::uint8_t { torn, whole, gone } partial;
    // True if the new file is at the path.
    bool published;
};

std::vector<CrashPoint> crash_points() {
    using enum CrashPoint::Partial;
    return {
        // Before the first write: an empty partial file.
        {"write", 1, torn, false},
        // Mid-data: the large payload is out, the commit record is not.
        {"write", 3, torn, false},
        {file_sync_call(), 1, whole, false},
        {"close file", 1, whole, false},
        {"rename", 1, whole, false},
        {"open_directory", 1, gone, true},
        {directory_sync_call(), 1, gone, true},
    };
}

// M6: a crash at each step of a commit leaves the old file whole at the path, or after
// the rename the new one whole, never a mix; a torn partial file is refused at open.
TEST(ContainerTornWrite, ACrashAtAnyStepLeavesTheOldFileOrTheWholeNewOne) {
    const Bytes old = build_container(test::golden_parts()).value();
    const Bytes fresh = build_container(several_parts()).value();
    for (const CrashPoint& point : crash_points()) {
        SCOPED_TRACE(std::string{point.call} + " #" + std::to_string(point.nth));
        const ScratchDir dir;
        const auto path = dir / "file.tsc";
        test::write_bytes(path, old);
        EXPECT_EXIT(crash_at(path, point.call, point.nth), ::testing::ExitedWithCode(0), "");

        expect_intact(path, point.published ? fresh : old);
        switch (point.partial) {
            case CrashPoint::Partial::torn: {
                ASSERT_TRUE(std::filesystem::exists(partial(path)));
                const auto torn = ContainerReader::open(partial(path));
                ASSERT_FALSE(torn.has_value());
                EXPECT_TRUE(torn.error().kind == StoreErrorKind::file_too_small ||
                            torn.error().field == "trailer magic")
                    << to_string(torn.error());
                break;
            }
            case CrashPoint::Partial::whole:
                // Written in full but never published: complete, and still not at the path.
                EXPECT_EQ(test::read_bytes(partial(path)), fresh);
                break;
            case CrashPoint::Partial::gone:
                EXPECT_FALSE(std::filesystem::exists(partial(path)));
                break;
        }
    }
}

// The second half of M6: the old file is corrupt too, and the crash leaves nothing good.
// Open fails cleanly with a typed error, and nothing reads past either file.
TEST(ContainerTornWrite, ACorruptOldFileAndACrashFailCleanly) {
    Bytes old = build_container(test::golden_parts()).value();
    old[test::kGoldenFooter + 3] ^= std::byte{0x01};
    const ScratchDir dir;
    const auto path = dir / "file.tsc";
    test::write_bytes(path, old);
    EXPECT_EXIT(crash_at(path, "write", 3), ::testing::ExitedWithCode(0), "");
    const auto reader = ContainerReader::open(path);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind, StoreErrorKind::bad_checksum);
    EXPECT_EQ(reader.error().field, "footer");
    EXPECT_FALSE(ContainerReader::open(partial(path)).has_value());
}

// Every boundary of the new file, block by block and section by section, then the
// footer's parts and the trailer: a file cut there is refused, whether it sits at the
// partial path or was published torn by a page_cache crash, and the old file is
// untouched beside it.
TEST(ContainerTornWrite, TheNewFileCutAtEveryBoundaryIsRefused) {
    const Bytes old = build_container(test::golden_parts()).value();
    const Bytes fresh = build_container(several_parts()).value();
    const auto parser = ContainerParser::open(fresh);
    ASSERT_TRUE(parser.has_value());

    std::vector<std::size_t> boundaries{0, kContainerHeaderSize};
    for (std::uint64_t i = 0; i < parser->block_count(); ++i) {
        const BlockInfo info = parser->block_info(i).value();
        boundaries.push_back(static_cast<std::size_t>(info.payload_offset + info.payload_size));
    }
    for (std::uint64_t i = 0; i < parser->section_count(); ++i) {
        const SectionInfo info = parser->section_info(i).value();
        boundaries.push_back(static_cast<std::size_t>(info.offset + info.size));
    }
    const auto footer = static_cast<std::size_t>(parser->footer_offset());
    const std::size_t sections_at = footer + kFooterFixedSize +
                                    (static_cast<std::size_t>(parser->block_count()) *
                                     kBlockEntrySize);
    const std::size_t metadata_at = sections_at + (static_cast<std::size_t>(
                                                       parser->section_count()) *
                                                   kSectionEntrySize);
    for (const std::size_t at : {footer + kFooterFixedSize, sections_at, metadata_at,
                                 fresh.size() - kTrailerSize, fresh.size() - 8,
                                 fresh.size() - 1}) {
        boundaries.push_back(at);
    }

    const ScratchDir dir;
    const auto path = dir / "file.tsc";
    test::write_bytes(path, old);
    for (const std::size_t at : boundaries) {
        SCOPED_TRACE(at);
        ASSERT_LT(at, fresh.size());
        const Bytes cut(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(at));
        test::write_bytes(partial(path), cut);
        EXPECT_FALSE(ContainerReader::open(partial(path)).has_value());
        EXPECT_FALSE(ContainerParser::open(cut, {.checksums = Checksums::skip}).has_value());
        expect_intact(path, old);
    }
}

}  // namespace
}  // namespace tick_store::store
