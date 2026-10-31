#pragma once

// Proving where a file's pages are, for the warm and cold query runs. Everything here
// reports what the kernel says about the page cache, on a fresh mapping that never
// touches a page, so looking does not change the answer.
//
// The two platforms differ in how pages are evicted, and each file of this interface has
// one implementation chosen by the build, so no caller needs an #ifdef:
//
//   macOS  msync(MS_INVALIDATE) on a fresh read-only MAP_SHARED mapping.
//   Linux  fsync, then posix_fadvise(POSIX_FADV_DONTNEED), which always returns 0 and
//          skips dirty, mapped and writeback pages. So its return value proves nothing
//          and residency has to.
//
// Neither reaches file system metadata, so "cold" means data pages only.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tick_store::harness {

struct FileResidency {
    std::uint64_t bytes = 0;
    std::uint64_t pages = 0;
    std::uint64_t resident_pages = 0;
    // Pages in the cache and not yet on the device, if the platform can say: on macOS
    // from mincore's MINCORE_MODIFIED, on Linux from cachestat, counting writeback too.
    std::optional<std::uint64_t> dirty_pages;
};

[[nodiscard]] std::uint64_t page_size() noexcept;

// What evict_file() does, for the results.
[[nodiscard]] std::string_view eviction_method() noexcept;

// True where a page another process has mapped cannot be evicted. Linux skips such
// pages. macOS does not, as the machine probe showed.
[[nodiscard]] bool live_mappings_block_eviction() noexcept;

// An empty file has no pages, and is reported as such.
[[nodiscard]] std::expected<FileResidency, std::string> file_residency(
    const std::filesystem::path& path);

// One eviction pass. Success means the call was made, not that it worked.
[[nodiscard]] std::expected<void, std::string> evict_file(const std::filesystem::path& path);

}  // namespace tick_store::harness
