#pragma once

// A slice of an ITCH file, small enough for the comparison harness to load: every `S`
// and `R` of the whole day, so the stream stays self-describing, plus one contiguous
// window of records, all in file order. Frames are copied verbatim, so a slice is a
// valid ITCH stream.
//
// The window is a run of the file, not a filter on time: it starts at the first record
// timestamped t0 or later, and ends before the first record after that timestamped t1
// or later. If the file's timestamps ever step backwards a record could fall on the
// wrong side of that, so the report counts any that do.

#include "tick_store/harness/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::harness {

struct SliceWindow {
    // Nanoseconds since midnight: [from_ns, to_ns).
    std::uint64_t from_ns = 0;
    std::uint64_t to_ns = 0;
};

// "HH:MM:SS", or nullopt.
[[nodiscard]] std::optional<std::uint64_t> parse_time_of_day(std::string_view text);

struct SliceReport {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    // Records of the window, of any type.
    std::uint64_t window_messages = 0;
    // `S` and `R` from outside the window, in the slice for what they say.
    std::uint64_t system_messages_outside = 0;
    std::uint64_t directory_messages_outside = 0;
    // The window's place in the source: [first, end), in bytes.
    std::uint64_t window_first_offset = 0;
    std::uint64_t window_end_offset = 0;
    // Time order broke the window's rule. Both are 0 on a non-decreasing file.
    std::uint64_t in_window_outside_range = 0;
    std::uint64_t in_range_outside_window = 0;
    // Raw-path records outside the window, which have no timestamp and are left out.
    std::uint64_t raw_outside_window = 0;
};

// Why write_slice wrote nothing.
struct SliceError {
    // The source's frames stop before its end: cut mid-frame, or a zero-length frame. Such a
    // source is part of a day, and a slice of it would claim the whole day's S and R.
    bool source_stops_early = false;
    std::string message;
};

// Writes the slice of `source` to `path`, through `<path>.partial` and a rename, so a
// failure leaves no file that looks finished. Refuses a source whose frames do not end
// exactly at its end.
[[nodiscard]] std::expected<SliceReport, SliceError> write_slice(
    std::span<const std::byte> source, const std::filesystem::path& path, SliceWindow window);

struct SliceCheck {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t system_messages = 0;
    std::uint64_t directory_messages = 0;
    Sha256Digest sha256{};
};

// Proves a slice is a clean ITCH stream from its bytes alone: frames that end exactly
// at the end of the file, and every payload a conforming message by the strict decoder.
[[nodiscard]] std::expected<SliceCheck, std::string> check_slice(std::span<const std::byte> slice);

}  // namespace tick_store::harness
