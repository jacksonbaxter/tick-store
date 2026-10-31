#include "tick_store/harness/data_tools_cli.hpp"

#include "tick_store/harness/census.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/harness/slice.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::harness {
namespace {

constexpr std::string_view kCensusUsage = "usage: tick_census <source> <output.json>\n";
constexpr std::string_view kSliceUsage =
    "usage: tick_slice <source> <output> [--from HH:MM:SS] [--to HH:MM:SS]\n";

constexpr std::string_view kDefaultFrom = "09:29:30";
constexpr std::string_view kDefaultTo = "09:34:30";

int fail_usage(CliOutput& output, std::string_view usage) {
    output.err += usage;
    return kExitUsageOrIo;
}

int fail_io(CliOutput& output, std::string_view what) {
    output.err += what;
    output.err += '\n';
    return kExitUsageOrIo;
}

int fail_source(CliOutput& output, const store::StoreError& error) {
    return fail_io(output, std::format("cannot read the source: {}", to_string(error)));
}

// True once every byte is in the file, and the file has been closed.
[[nodiscard]] bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return !file.fail();
}

struct SliceOptions {
    std::string_view from = kDefaultFrom;
    std::string_view to = kDefaultTo;
};

// Fills `options` from the arguments after the two paths. False on anything unknown.
[[nodiscard]] bool parse_slice_options(std::span<const std::string_view> flags,
                                       SliceOptions& options) {
    if (flags.size() % 2 != 0) {
        return false;
    }
    for (std::size_t i = 0; i < flags.size(); i += 2) {
        if (flags[i] == "--from") {
            options.from = flags[i + 1];
        } else if (flags[i] == "--to") {
            options.to = flags[i + 1];
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int run_census_cli(std::span<const std::string_view> arguments, CliOutput& output) {
    if (arguments.size() != 2) {
        return fail_usage(output, kCensusUsage);
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto source = store::MappedFile::open(arguments[0]);
    if (!source.has_value()) {
        return fail_source(output, source.error());
    }
    const Census census = take_census(source->bytes());
    const double seconds = std::chrono::duration<double>{Clock::now() - start}.count();
    if (!write_text(arguments[1], to_json(census, seconds))) {
        return fail_io(output, "cannot write the census");
    }
    return kExitPass;
}

int run_slice_cli(std::span<const std::string_view> arguments, CliOutput& output) {
    SliceOptions options;
    if (arguments.size() < 2 || !parse_slice_options(arguments.subspan(2), options)) {
        return fail_usage(output, kSliceUsage);
    }
    const auto from = parse_time_of_day(options.from);
    const auto to = parse_time_of_day(options.to);
    if (!from.has_value() || !to.has_value() || *from >= *to) {
        return fail_usage(output, kSliceUsage);
    }

    const auto source = store::MappedFile::open(arguments[0]);
    if (!source.has_value()) {
        return fail_source(output, source.error());
    }
    const auto report =
        write_slice(source->bytes(), arguments[1], {.from_ns = *from, .to_ns = *to});
    if (!report.has_value()) {
        return fail_io(output, report.error());
    }

    const auto written = store::MappedFile::open(arguments[1]);
    if (!written.has_value()) {
        return fail_io(output, "cannot read the slice back");
    }
    const auto check = check_slice(written->bytes());
    if (!check.has_value()) {
        output.err += std::format("the slice does not check out: {}\n", check.error());
        return kExitMismatch;
    }
    if (check->messages != report->messages || check->bytes != report->bytes) {
        output.err += "the slice read back does not match what was written\n";
        return kExitMismatch;
    }

    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_slice"});
    json.field("from", options.from);
    json.field("to", options.to);
    json.field("messages", check->messages);
    json.field("bytes", check->bytes);
    json.field("sha256", to_hex(check->sha256));
    json.field("window_messages", report->window_messages);
    json.field("system_messages", check->system_messages);
    json.field("directory_messages", check->directory_messages);
    json.field("system_messages_outside_window", report->system_messages_outside);
    json.field("directory_messages_outside_window", report->directory_messages_outside);
    json.field("window_first_source_offset", report->window_first_offset);
    json.field("window_end_source_offset", report->window_end_offset);
    json.field("in_window_outside_range", report->in_window_outside_range);
    json.field("in_range_outside_window", report->in_range_outside_window);
    json.field("raw_outside_window", report->raw_outside_window);
    json.field("decodes_cleanly", true);
    json.end_object();
    output.out += json.finish();
    return kExitPass;
}

}  // namespace tick_store::harness
