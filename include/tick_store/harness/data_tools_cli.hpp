#pragma once

// The command lines of tools/tick_census and tools/tick_slice, as functions, for the
// same reason as roundtrip_cli.hpp: tests run every path without starting a process.
//
//   tick_census <source> <output.json>
//       Writes the census of a whole ITCH file. The JSON holds no paths.
//   tick_slice <source> <output> [--from HH:MM:SS] [--to HH:MM:SS]
//       Writes every S and R of the day plus the window [from, to), by default the
//       open, 09:29:30 to 09:34:30. Then reads the result back and proves it decodes,
//       and prints a JSON summary holding no paths.
//
// Exit codes are those of roundtrip_cli.hpp: 2 for usage or I/O, and 1 from tick_slice
// if the slice it wrote does not check out.

#include "tick_store/harness/roundtrip_cli.hpp"

#include <span>
#include <string_view>

namespace tick_store::harness {

[[nodiscard]] int run_census_cli(std::span<const std::string_view> arguments, CliOutput& output);
[[nodiscard]] int run_slice_cli(std::span<const std::string_view> arguments, CliOutput& output);

}  // namespace tick_store::harness
