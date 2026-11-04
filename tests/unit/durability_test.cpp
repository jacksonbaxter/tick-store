// Durability: each sync policy makes exactly the calls it states, falls back only where
// it says, and reports a failed flush once without retrying it (M3, M4), on any host,
// through a recording FileOps. Then the native policy against the real kernel.

#include "recording_file_ops.hpp"
#include "scratch_dir.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/store_error.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace tick_store::store {
namespace {

using test::RecordingFileOps;
using ::tick_store::test::ScratchDir;
using Calls = std::vector<std::string>;

StoreError io(const char* call, int error) {
    return {.kind = StoreErrorKind::io, .field = call, .actual = static_cast<std::uint64_t>(error)};
}

// A real file to flush, closed when the test ends.
class OpenFile {
public:
    explicit OpenFile(const ScratchDir& dir)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        : fd_(::open((dir / "file").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) {}
    OpenFile(const OpenFile&) = delete;
    OpenFile& operator=(const OpenFile&) = delete;
    OpenFile(OpenFile&&) = delete;
    OpenFile& operator=(OpenFile&&) = delete;
    ~OpenFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int fd() const { return fd_; }

private:
    int fd_;
};

TEST(Durability, NativePolicyIsThisOperatingSystems) {
#ifdef __APPLE__
    EXPECT_EQ(native_sync_policy(), SyncPolicy::darwin_full_fsync);
#else
    EXPECT_EQ(native_sync_policy(), SyncPolicy::linux_fdatasync);
#endif
}

TEST(Durability, ModesHaveTheirNames) {
    EXPECT_EQ(to_string(Durability::page_cache), "page_cache");
    EXPECT_EQ(to_string(Durability::device_flush), "device_flush");
}

TEST(SyncFile, DarwinPolicyMakesOneFullFsync) {
    RecordingFileOps ops;
    ops.forward_syncs = false;
    EXPECT_TRUE(sync_file(ops, 99, SyncPolicy::darwin_full_fsync).has_value());
    EXPECT_EQ(ops.calls, (Calls{"full_fsync file"}));
}

TEST(SyncFile, DarwinPolicyFallsBackToFsyncOnlyWhereFullFsyncIsUnsupported) {
    for (const int error : {ENOTSUP, EINVAL, ENOTTY}) {
        SCOPED_TRACE(error);
        RecordingFileOps ops;
        ops.forward_syncs = false;
        ops.fail_on("full_fsync file", error);
        EXPECT_TRUE(sync_file(ops, 99, SyncPolicy::darwin_full_fsync).has_value());
        EXPECT_EQ(ops.calls, (Calls{"full_fsync file", "fsync file"}));
    }
}

TEST(SyncFile, DarwinPolicyReportsAnyOtherFullFsyncFailureWithoutFallingBack) {
    for (const int error : {EIO, ENOSPC, EINTR, EBADF}) {
        SCOPED_TRACE(error);
        RecordingFileOps ops;
        ops.forward_syncs = false;
        ops.fail_on("full_fsync file", error);
        EXPECT_EQ(sync_file(ops, 99, SyncPolicy::darwin_full_fsync),
                  std::unexpected(io("F_FULLFSYNC", error)));
        EXPECT_EQ(ops.calls, (Calls{"full_fsync file"}));
    }
}

TEST(SyncFile, DarwinPolicyReportsAFailedFallback) {
    RecordingFileOps ops;
    ops.forward_syncs = false;
    ops.fail_on("full_fsync file", ENOTSUP);
    ops.fail_on("fsync file", EIO);
    EXPECT_EQ(sync_file(ops, 99, SyncPolicy::darwin_full_fsync),
              std::unexpected(io("fsync", EIO)));
    EXPECT_EQ(ops.calls, (Calls{"full_fsync file", "fsync file"}));
}

TEST(SyncFile, LinuxPolicyMakesOneFdatasync) {
    RecordingFileOps ops;
    ops.forward_syncs = false;
    EXPECT_TRUE(sync_file(ops, 99, SyncPolicy::linux_fdatasync).has_value());
    EXPECT_EQ(ops.calls, (Calls{"fdatasync file"}));
}

// fsyncgate: after a failed flush the kernel may have dropped the dirty pages, so a
// retry that succeeds would report data as safe that is gone. EINTR included.
TEST(SyncFile, LinuxPolicyReportsAFailureOnceAndNeverRetries) {
    for (const int error : {EIO, ENOSPC, EINTR}) {
        SCOPED_TRACE(error);
        RecordingFileOps ops;
        ops.forward_syncs = false;
        ops.fail_on("fdatasync file", error);
        EXPECT_EQ(sync_file(ops, 99, SyncPolicy::linux_fdatasync),
                  std::unexpected(io("fdatasync", error)));
        EXPECT_EQ(ops.calls, (Calls{"fdatasync file"}));
    }
}

TEST(SyncDirectory, DarwinPolicyFullFsyncsTheDirectory) {
    const ScratchDir dir;
    RecordingFileOps ops;
    ops.forward_syncs = false;
    EXPECT_TRUE(sync_directory(ops, dir.path(), SyncPolicy::darwin_full_fsync).has_value());
    EXPECT_EQ(ops.calls, (Calls{"open_directory", "full_fsync directory", "close directory"}));
}

TEST(SyncDirectory, DarwinPolicyFallsBackAndReportsLikeAFile) {
    const ScratchDir dir;
    RecordingFileOps fallback;
    fallback.forward_syncs = false;
    fallback.fail_on("full_fsync directory", EINVAL);
    EXPECT_TRUE(sync_directory(fallback, dir.path(), SyncPolicy::darwin_full_fsync).has_value());
    EXPECT_EQ(fallback.calls, (Calls{"open_directory", "full_fsync directory", "fsync directory",
                                     "close directory"}));

    RecordingFileOps failing;
    failing.forward_syncs = false;
    failing.fail_on("full_fsync directory", EIO);
    EXPECT_EQ(sync_directory(failing, dir.path(), SyncPolicy::darwin_full_fsync),
              std::unexpected(io("F_FULLFSYNC directory", EIO)));
    EXPECT_EQ(failing.calls, (Calls{"open_directory", "full_fsync directory", "close directory"}));
}

TEST(SyncDirectory, LinuxPolicyFsyncsTheDirectory) {
    const ScratchDir dir;
    RecordingFileOps ops;
    ops.forward_syncs = false;
    EXPECT_TRUE(sync_directory(ops, dir.path(), SyncPolicy::linux_fdatasync).has_value());
    EXPECT_EQ(ops.calls, (Calls{"open_directory", "fsync directory", "close directory"}));

    RecordingFileOps failing;
    failing.forward_syncs = false;
    failing.fail_on("fsync directory", EIO);
    EXPECT_EQ(sync_directory(failing, dir.path(), SyncPolicy::linux_fdatasync),
              std::unexpected(io("fsync directory", EIO)));
    EXPECT_EQ(failing.calls, (Calls{"open_directory", "fsync directory", "close directory"}));
}

TEST(SyncDirectory, ReportsADirectoryThatWillNotOpen) {
    const ScratchDir dir;
    RecordingFileOps ops;
    EXPECT_EQ(sync_directory(ops, dir / "absent"), std::unexpected(io("open directory", ENOENT)));
    EXPECT_EQ(ops.calls, (Calls{"open_directory"}));
}

// The native policy through the real kernel, on this host's file system.
TEST(SyncFile, NativePolicyFlushesARealFileAndDirectory) {
    const ScratchDir dir;
    const OpenFile file{dir};
    ASSERT_GE(file.fd(), 0);
    RecordingFileOps ops;
    const std::vector<std::byte> data(4096, std::byte{0x5A});
    ASSERT_TRUE(write_all(ops, file.fd(), data).has_value());
    EXPECT_TRUE(sync_file(ops, file.fd()).has_value());
    EXPECT_TRUE(sync_directory(ops, dir.path()).has_value());
#ifdef __APPLE__
    EXPECT_EQ(ops.calls_without_writes(),
              (Calls{"full_fsync file", "open_directory", "full_fsync directory",
                     "close directory"}));
#else
    EXPECT_EQ(ops.calls_without_writes(),
              (Calls{"fdatasync file", "open_directory", "fsync directory", "close directory"}));
#endif
}

TEST(WriteAll, FinishesShortWritesAndRetriesEintr) {
    const ScratchDir dir;
    const OpenFile file{dir};
    ASSERT_GE(file.fd(), 0);
    RecordingFileOps ops;
    ops.write_limit = 3;
    ops.fail_on("write", EINTR, 2);
    std::vector<std::byte> data(10);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i);
    }
    ASSERT_TRUE(write_all(ops, file.fd(), data).has_value());
    // 3 + EINTR + 3 + 3 + 1.
    EXPECT_EQ(ops.count("write"), 5);
    struct stat info{};
    ASSERT_EQ(::fstat(file.fd(), &info), 0);
    EXPECT_EQ(info.st_size, 10);
}

TEST(WriteAll, ReportsAnyOtherError) {
    const ScratchDir dir;
    const OpenFile file{dir};
    RecordingFileOps ops;
    ops.fail_on("write", ENOSPC);
    const std::vector<std::byte> data(10);
    EXPECT_EQ(write_all(ops, file.fd(), data), std::unexpected(io("write", ENOSPC)));
    EXPECT_EQ(ops.count("write"), 1);
}

}  // namespace
}  // namespace tick_store::store
