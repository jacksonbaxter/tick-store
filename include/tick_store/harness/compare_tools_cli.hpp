#pragma once

// The command lines of tools/tick_export and tools/tick_coldcache, as functions, for the
// same reason as roundtrip_cli.hpp: tests run every path without starting a process.
//
//   tick_export <source> <directory>
//       Writes an ITCH file as the column form of store/columns.hpp, then reads the
//       directory back through the oracle before saying it passed. Prints a JSON summary
//       holding no paths. Refuses a directory that already has a manifest.
//
//   tick_coldcache evict <paths...>
//       Evicts every regular file under the paths and proves it, on a fresh mapping,
//       retrying as needed. The harness must have stopped whatever uses them.
//   tick_coldcache residency <paths...>
//       Reports resident and, where the platform can say, unflushed pages.
//   tick_coldcache warm <paths...>
//       Reads every file through, then reports as residency does.
//
// Exit codes are those of roundtrip_cli.hpp. `evict` exits 1 if any page stayed resident
// after its last pass, and `tick_export` exits 1 if the oracle refused what it wrote.
// tick_coldcache's JSON names up to a few files that kept pages, as relative as the
// paths it was given. That output is for the harness, which never publishes it.

#include "tick_store/harness/roundtrip_cli.hpp"

#include <span>
#include <string_view>

namespace tick_store::harness {

[[nodiscard]] int run_export_cli(std::span<const std::string_view> arguments, CliOutput& output);
[[nodiscard]] int run_coldcache_cli(std::span<const std::string_view> arguments, CliOutput& output);

}  // namespace tick_store::harness
