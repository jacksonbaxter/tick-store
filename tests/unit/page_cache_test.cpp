// Page-cache residency and eviction, and tick_coldcache, on real files on the real file
// system. A negative control comes first wherever one exists: the tool must be able to
// report a file as not evicted, not only as evicted.

#include "scratch_dir.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/compare_tools_cli.hpp"
#include "tick_store/harness/page_cache.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::harness {
namespace {

using test::Bytes;
using test::ScratchDir;
using test::write_file;

constexpr std::size_t kFilePages = 64;

struct CliRun {
    int code = -1;
    CliOutput output;
};

CliRun run(std::initializer_list<std::string> arguments) {
    const std::vector<std::string> owned{arguments};
    const std::vector<std::string_view> views{owned.begin(), owned.end()};
    CliRun result;
    result.code = run_coldcache_cli(views, result.output);
    return result;
}

Bytes file_bytes() { return Bytes(kFilePages * page_size(), std::byte{0x5A}); }

// A whole file mapped and every page touched, then held for one scope.
class HeldMapping {
public:
    explicit HeldMapping(const std::filesystem::path& path)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
        : fd_(::open(path.c_str(), O_RDONLY)),
          length_(std::filesystem::file_size(path)),
          base_(::mmap(nullptr, length_, PROT_READ, MAP_SHARED, fd_, 0)) {
        const auto* bytes = static_cast<const volatile unsigned char*>(base_);
        for (std::size_t i = 0; i < length_; i += page_size()) {
            sum_ += bytes[i];
        }
    }
    HeldMapping(const HeldMapping&) = delete;
    HeldMapping& operator=(const HeldMapping&) = delete;
    HeldMapping(HeldMapping&&) = delete;
    HeldMapping& operator=(HeldMapping&&) = delete;
    ~HeldMapping() {
        ::munmap(base_, length_);
        ::close(fd_);
    }

private:
    int fd_;
    std::size_t length_;
    void* base_;
    std::uint64_t sum_ = 0;
};

TEST(PageCache, AnEmptyFileHasNoPages) {
    const ScratchDir scratch;
    const auto path = scratch / "empty";
    write_file(path, Bytes{});
    const auto residency = file_residency(path);
    ASSERT_TRUE(residency.has_value()) << residency.error();
    EXPECT_EQ(residency->pages, 0U);
    EXPECT_EQ(residency->bytes, 0U);
    EXPECT_TRUE(evict_file(path).has_value());
}

TEST(PageCache, AMissingFileIsAnError) {
    const ScratchDir scratch;
    EXPECT_FALSE(file_residency(scratch / "absent").has_value());
    EXPECT_FALSE(evict_file(scratch / "absent").has_value());
}

TEST(PageCache, WarmFillsEvictEmptiesAndAFreshReadFillsOnlyWhatItRead) {
    const ScratchDir scratch;
    const auto path = scratch / "file";
    write_file(path, file_bytes());

    const CliRun warmed = run({"warm", path.string()});
    ASSERT_EQ(warmed.code, kExitPass) << warmed.output.err;
    EXPECT_TRUE(warmed.output.out.contains("\"fully_resident\": true"));

    const auto before = file_residency(path);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->pages, kFilePages);
    EXPECT_EQ(before->resident_pages, kFilePages);

    const CliRun evicted = run({"evict", path.string()});
    ASSERT_EQ(evicted.code, kExitPass) << evicted.output.err;
    EXPECT_TRUE(evicted.output.out.contains("\"proven\": true"));
    EXPECT_TRUE(evicted.output.out.contains("\"resident_pages\": 0"));

    const auto after = file_residency(path);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->resident_pages, 0U);

    const CliRun reported = run({"residency", path.string()});
    ASSERT_EQ(reported.code, kExitPass);
    EXPECT_TRUE(reported.output.out.contains("\"fully_resident\": false"));
    EXPECT_TRUE(reported.output.out.contains("\"resident_pages\": 0"));
}

TEST(PageCache, DirtyPagesAreSeenAndAFlushClearsThem) {
    const ScratchDir scratch;
    const auto path = scratch / "file";
    write_file(path, file_bytes());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
    const int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::fsync(fd), 0);
    const auto clean = file_residency(path);
    ASSERT_TRUE(clean.has_value());
    if (!clean->dirty_pages.has_value()) {
        ::close(fd);
        GTEST_SKIP() << "this kernel has no cachestat, so dirty pages cannot be counted";
    }
    EXPECT_EQ(*clean->dirty_pages, 0U);

    const std::size_t length = kFilePages * page_size();
    void* const base = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(base, MAP_FAILED);
    auto* bytes = static_cast<volatile unsigned char*>(base);
    for (std::size_t i = 0; i < length; i += page_size()) {
        bytes[i] = 0xA5;
    }
    const auto dirty = file_residency(path);
    ASSERT_TRUE(dirty.has_value());
    EXPECT_GT(dirty->dirty_pages.value_or(0), 0U);

    ASSERT_EQ(::msync(base, length, MS_SYNC), 0);
    ::munmap(base, length);
    ::close(fd);
    const auto flushed = file_residency(path);
    ASSERT_TRUE(flushed.has_value());
    EXPECT_EQ(flushed->dirty_pages.value_or(1), 0U);
}

TEST(PageCache, AHeldMappingIsReportedAsItBehavesOnThisPlatform) {
    const ScratchDir scratch;
    const auto path = scratch / "file";
    write_file(path, file_bytes());
    const HeldMapping held{path};

    const CliRun evicted = run({"evict", path.string()});
    if (live_mappings_block_eviction()) {
        // The negative control: the call returns 0, and only the proof can refuse.
        EXPECT_EQ(evicted.code, kExitMismatch);
        EXPECT_TRUE(evicted.output.out.contains("\"proven\": false"));
        EXPECT_TRUE(evicted.output.out.contains("files_with_resident_pages"));
        EXPECT_TRUE(evicted.output.err.contains("stayed resident"));
    } else {
        EXPECT_EQ(evicted.code, kExitPass);
    }
}

TEST(ColdcacheCli, WalksADirectoryAndCountsEveryFile) {
    const ScratchDir scratch;
    std::filesystem::create_directories(scratch / "nested/deeper");
    write_file(scratch / "nested/one", file_bytes());
    write_file(scratch / "nested/deeper/two", file_bytes());
    std::filesystem::create_symlink(scratch / "nested/one", scratch / "nested/link");

    const CliRun reported = run({"residency", (scratch / "nested").string()});
    ASSERT_EQ(reported.code, kExitPass) << reported.output.err;
    EXPECT_TRUE(reported.output.out.contains("\"files\": 2"));
    EXPECT_TRUE(reported.output.out.contains(
        std::format("\"bytes\": {}", 2 * kFilePages * page_size())));
}

// A subdirectory the walk cannot open is an error with exit 2, not an exception out of the
// tool. Root can open any directory, so the case cannot be made there.
TEST(ColdcacheCli, ASubdirectoryItCannotOpenIsAnErrorNotACrash) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root opens every directory";
    }
    const ScratchDir scratch;
    const std::filesystem::path locked = scratch / "d/locked";
    std::filesystem::create_directories(locked);
    write_file(scratch / "d/file", file_bytes());
    std::filesystem::permissions(locked, std::filesystem::perms::none);
    CliRun reported;
    EXPECT_NO_THROW(reported = run({"residency", (scratch / "d").string()}));
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
    EXPECT_EQ(reported.code, kExitUsageOrIo);
    EXPECT_TRUE(reported.output.err.contains("cannot walk")) << reported.output.err;
    EXPECT_TRUE(reported.output.out.empty());
}

TEST(ColdcacheCli, RefusesBadUsageAndAMissingPath) {
    EXPECT_EQ(run({}).code, kExitUsageOrIo);
    EXPECT_EQ(run({"evict"}).code, kExitUsageOrIo);
    const ScratchDir scratch;
    write_file(scratch / "file", file_bytes());
    EXPECT_EQ(run({"frobnicate", (scratch / "file").string()}).code, kExitUsageOrIo);
    const CliRun missing = run({"evict", (scratch / "absent").string()});
    EXPECT_EQ(missing.code, kExitUsageOrIo);
    EXPECT_FALSE(missing.output.err.empty());
}

}  // namespace
}  // namespace tick_store::harness
