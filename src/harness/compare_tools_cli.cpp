#include "tick_store/harness/compare_tools_cli.hpp"

#include "tick_store/harness/columns_reader.hpp"
#include "tick_store/harness/itch_store_record_reader.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/harness/page_cache.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace tick_store::harness {
namespace {

constexpr std::string_view kExportUsage =
    "usage: tick_export <source> <directory>\n"
    "       tick_export --store <store.tsc> <directory>\n";
constexpr std::string_view kColdcacheUsage =
    "usage: tick_coldcache evict <paths...>\n"
    "       tick_coldcache residency <paths...>\n"
    "       tick_coldcache warm <paths...>\n";

// A rare pass leaves pages behind, so the tool retries. The machine probe saw 1 pass in
// about 38 leave 128 pages. On a full day, QuestDB's 22 GB store, just written and read back,
// kept 14,080 pages after 16 passes 20 ms apart, and with nothing running took 14 passes:
// pages a store's own run left behind can take seconds to become droppable. So the wait
// between passes doubles from 20 ms up to 1 s, and 32 passes, about 27 s of waiting, mean a
// stuck page. The proof is unchanged: every page must go.
constexpr int kMaxEvictionPasses = 32;
constexpr std::chrono::milliseconds kEvictionRetryDelay{20};
constexpr std::chrono::milliseconds kEvictionRetryDelayMax{1000};
constexpr std::size_t kReportedFiles = 8;
constexpr std::size_t kReadChunk = std::size_t{1} << 20U;

int fail(CliOutput& output, std::string_view what) {
    output.err += what;
    output.err += '\n';
    return kExitUsageOrIo;
}

int fail_store(CliOutput& output, std::string_view what, const store::StoreError& error) {
    return fail(output, std::format("{}: {}", what, to_string(error)));
}

std::string export_summary_json(const store::ColumnsSummary& summary, const VerifyReport& report,
                                double wall_seconds) {
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_export"});
    json.field("verdict", std::string_view{report.passed() ? "pass" : "mismatch"});
    json.field("records", summary.records);
    json.field("source_bytes", report.source_bytes + report.source_tail_bytes);
    json.field("tail_bytes", summary.tail_bytes);
    json.field("source_sha256", to_hex(report.source_sha256));
    json.field("hot_rows", summary.hot_rows);
    json.field("rare_rows", summary.rare_rows);
    json.field("raw_rows", summary.raw_rows);
    json.field("rest_text_bytes", summary.text_rest_bytes);
    json.field("timestamps_sorted", summary.timestamps_sorted);
    json.field("max_ts_ns", summary.max_ts_ns);
    json.field("max_locate", summary.max_locate);
    json.field("max_tracking", summary.max_tracking);
    json.field("max_order_ref", summary.max_order_ref);
    json.field("max_shares", summary.max_shares);
    json.field("max_price", summary.max_price);
    json.field("max_match", summary.max_match);
    json.field("wall_seconds", wall_seconds);
    json.end_object();
    return json.finish();
}

// The regular files under `paths`, in a fixed order. A symlink is not followed, so a
// file is never counted twice.
std::expected<std::vector<std::filesystem::path>, std::string> regular_files(
    std::span<const std::string_view> paths) {
    std::vector<std::filesystem::path> files;
    for (const std::string_view text : paths) {
        const std::filesystem::path path{text};
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error) {
            return std::unexpected(std::format("cannot stat {}: {}", text, error.message()));
        }
        if (std::filesystem::is_regular_file(status)) {
            files.push_back(path);
            continue;
        }
        if (!std::filesystem::is_directory(status)) {
            return std::unexpected(std::format("not a file or directory: {}", text));
        }
        const auto walker = std::filesystem::recursive_directory_iterator{path, error};
        if (error) {
            return std::unexpected(std::format("cannot walk {}: {}", text, error.message()));
        }
        for (const auto& entry : walker) {
            if (entry.is_regular_file(error) && !entry.is_symlink(error)) {
                files.push_back(entry.path());
            }
        }
    }
    return files;
}

struct Totals {
    std::uint64_t bytes = 0;
    std::uint64_t pages = 0;
    std::uint64_t resident_pages = 0;
    std::optional<std::uint64_t> dirty_pages = 0;
    std::vector<std::pair<std::string, std::uint64_t>> resident_files;
};

std::expected<Totals, std::string> measure(const std::vector<std::filesystem::path>& files) {
    Totals totals;
    for (const std::filesystem::path& path : files) {
        const auto file = file_residency(path);
        if (!file.has_value()) {
            return std::unexpected(file.error());
        }
        totals.bytes += file->bytes;
        totals.pages += file->pages;
        totals.resident_pages += file->resident_pages;
        if (totals.dirty_pages.has_value() && file->dirty_pages.has_value()) {
            *totals.dirty_pages += *file->dirty_pages;
        } else {
            totals.dirty_pages.reset();
        }
        if (file->resident_pages != 0 && totals.resident_files.size() < kReportedFiles) {
            totals.resident_files.emplace_back(path.string(), file->resident_pages);
        }
    }
    return totals;
}

void write_totals(JsonWriter& json, const Totals& totals, std::size_t files) {
    json.field("method", eviction_method());
    json.field("page_size", page_size());
    json.field("files", static_cast<std::uint64_t>(files));
    json.field("bytes", totals.bytes);
    json.field("pages", totals.pages);
    json.field("resident_pages", totals.resident_pages);
    json.field("dirty_pages", totals.dirty_pages);
}

void write_resident_files(JsonWriter& json, const Totals& totals) {
    json.key("files_with_resident_pages");
    json.begin_array();
    for (const auto& [name, pages] : totals.resident_files) {
        json.begin_object();
        json.field("file", std::string_view{name});
        json.field("resident_pages", pages);
        json.end_object();
    }
    json.end_array();
}

std::expected<void, std::string> read_through(const std::filesystem::path& path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the POSIX interface is variadic.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(std::format("cannot read {}: {}", path.string(),
                                           std::generic_category().message(errno)));
    }
    std::vector<char> buffer(kReadChunk);
    while (true) {
        const auto got = ::read(fd, buffer.data(), buffer.size());
        if (got == 0) {
            ::close(fd);
            return {};
        }
        if (got < 0 && errno != EINTR) {
            const int saved = errno;
            ::close(fd);
            return std::unexpected(std::format("cannot read {}: {}", path.string(),
                                               std::generic_category().message(saved)));
        }
    }
}

int run_evict(const std::vector<std::filesystem::path>& files, CliOutput& output) {
    int passes = 0;
    std::chrono::milliseconds delay = kEvictionRetryDelay;
    std::expected<Totals, std::string> totals = measure(files);
    while (totals.has_value() && totals->resident_pages != 0 && passes < kMaxEvictionPasses) {
        ++passes;
        if (passes > 1) {
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, kEvictionRetryDelayMax);
        }
        for (const std::filesystem::path& path : files) {
            if (const auto evicted = evict_file(path); !evicted.has_value()) {
                return fail(output, evicted.error());
            }
        }
        totals = measure(files);
    }
    if (!totals.has_value()) {
        return fail(output, totals.error());
    }
    const bool proven = totals->resident_pages == 0;
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_coldcache"});
    json.field("command", std::string_view{"evict"});
    json.field("proven", proven);
    json.field("passes", static_cast<std::uint64_t>(passes));
    write_totals(json, *totals, files.size());
    write_resident_files(json, *totals);
    json.end_object();
    output.out += json.finish();
    if (!proven) {
        output.err += std::format("{} pages stayed resident after {} passes\n",
                                  totals->resident_pages, passes);
        return kExitMismatch;
    }
    return kExitPass;
}

int run_residency(std::string_view command, const std::vector<std::filesystem::path>& files,
                  CliOutput& output) {
    const auto totals = measure(files);
    if (!totals.has_value()) {
        return fail(output, totals.error());
    }
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_coldcache"});
    json.field("command", command);
    json.field("fully_resident", totals->resident_pages == totals->pages);
    write_totals(json, *totals, files.size());
    json.end_object();
    output.out += json.finish();
    return kExitPass;
}

// The store's records, each through its whole read path and the ITCH encoder
// (ItchStoreRecordReader), written as the column form. Nothing here sees the source, so the
// harness proves the directory against it with tick_roundtrip verify-columns.
int run_export_from_store(std::string_view store_path, std::string_view directory_text,
                          CliOutput& output) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    auto reader = store::ItchStoreReader::open(std::string{store_path});
    if (!reader.has_value()) {
        output.err += std::format("the store does not open: {}\n", to_string(reader.error()));
        return kExitMismatch;
    }
    auto writer = store::ColumnsWriter::create(std::filesystem::path{directory_text});
    if (!writer.has_value()) {
        return fail_store(output, "cannot create the columns", writer.error());
    }
    ItchStoreRecordReader records{*reader};
    while (true) {
        auto record = records.next();
        if (!record.has_value()) {
            output.err += std::format("the store cannot give its next record: {}\n",
                                      record.error().message);
            return kExitMismatch;
        }
        if (!record->has_value()) {
            break;
        }
        if (const auto added = writer->append(record->value().subspan(itch::kFramePrefixSize));
            !added.has_value()) {
            return fail_store(output, "cannot write the columns", added.error());
        }
    }
    const auto tail = records.tail();
    if (!tail.has_value()) {
        output.err += std::format("the store cannot give its tail: {}\n", tail.error().message);
        return kExitMismatch;
    }
    const auto summary = writer->finish(*tail);
    if (!summary.has_value()) {
        return fail_store(output, "cannot finish the columns", summary.error());
    }
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_export"});
    json.field("command", std::string_view{"store"});
    json.field("records", summary->records);
    json.field("tail_bytes", summary->tail_bytes);
    json.field("hot_rows", summary->hot_rows);
    json.field("rare_rows", summary->rare_rows);
    json.field("raw_rows", summary->raw_rows);
    json.field("wall_seconds", std::chrono::duration<double>{Clock::now() - start}.count());
    json.end_object();
    output.out += json.finish();
    return kExitPass;
}

}  // namespace

int run_export_cli(std::span<const std::string_view> arguments, CliOutput& output) {
    if (arguments.size() == 3 && arguments[0] == "--store") {
        return run_export_from_store(arguments[1], arguments[2], output);
    }
    if (arguments.size() != 2) {
        return fail(output, kExportUsage.substr(0, kExportUsage.size() - 1));
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();

    const auto source = store::MappedFile::open(arguments[0]);
    if (!source.has_value()) {
        return fail_store(output, "cannot read the source", source.error());
    }
    const std::filesystem::path directory{arguments[1]};
    auto writer = store::ColumnsWriter::create(directory);
    if (!writer.has_value()) {
        return fail_store(output, "cannot create the columns", writer.error());
    }

    itch::FrameCursor cursor{source->bytes()};
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        if (const auto added = writer->append(frame->payload); !added.has_value()) {
            return fail_store(output, "cannot write the columns", added.error());
        }
    }
    const auto summary = writer->finish(cursor.remaining());
    if (!summary.has_value()) {
        return fail_store(output, "cannot finish the columns", summary.error());
    }

    const VerifyReport report = verify_columns(source->bytes(), directory);
    const double seconds = std::chrono::duration<double>{Clock::now() - start}.count();
    output.out += export_summary_json(*summary, report, seconds);
    if (report.passed()) {
        return kExitPass;
    }
    output.err += "the columns just written do not rebuild the source\n";
    return kExitMismatch;
}

int run_coldcache_cli(std::span<const std::string_view> arguments, CliOutput& output) {
    if (arguments.size() < 2) {
        return fail(output, kColdcacheUsage.substr(0, kColdcacheUsage.size() - 1));
    }
    const std::string_view command = arguments.front();
    const auto files = regular_files(arguments.subspan(1));
    if (!files.has_value()) {
        return fail(output, files.error());
    }
    if (command == "evict") {
        return run_evict(*files, output);
    }
    if (command == "residency") {
        return run_residency(command, *files, output);
    }
    if (command == "warm") {
        for (const std::filesystem::path& path : *files) {
            if (const auto read = read_through(path); !read.has_value()) {
                return fail(output, read.error());
            }
        }
        return run_residency(command, *files, output);
    }
    return fail(output, kColdcacheUsage.substr(0, kColdcacheUsage.size() - 1));
}

}  // namespace tick_store::harness
