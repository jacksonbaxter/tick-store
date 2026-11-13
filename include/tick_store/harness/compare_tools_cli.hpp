#pragma once

// The command lines of tools/tick_export and tools/tick_coldcache, as functions, for the
// same reason as roundtrip_cli.hpp: tests run every path without starting a process.
//
//   tick_export <source> <directory>
//       Writes an ITCH file as the column form of store/columns.hpp, then reads the
//       directory back through the oracle before saying it passed. Prints a JSON summary
//       holding no paths. Refuses a directory that already has a manifest.
//   tick_export --store <store.tsc> <directory>
//       Writes an ITCH store's records as the same column form: each record comes through
//       the store's whole read path and the ITCH encoder, as the oracle reads it. It is
//       given no source, so the comparison harness proves the directory against the source
//       with tick_roundtrip verify-columns. Exits 1 if the store refuses to give a record.
//
//   tick_coldcache evict <paths...>
//       Evicts every regular file under the paths and proves it, on a fresh mapping,
//       retrying as needed. The harness must have stopped whatever uses them.
//   tick_coldcache residency <paths...>
//       Reports resident and, where the platform can say, unflushed pages.
//   tick_coldcache warm <paths...>
//       Measures every file, then reads every file through and reports as residency does.
//       A file it cannot measure is refused before any file is read.
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
