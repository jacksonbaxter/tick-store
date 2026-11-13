#pragma once

// The query worker, tools/tick_query, as a function so tests drive it without a process.
//
//   tick_query <store.tsc> --day <MMDDYYYY> [--path indexed|exact]
//
// Opens the store once, prints one JSON line saying it is ready, then answers one JSON line
// of stdin with one JSON line of stdout until stdin ends or a line asks to quit. A request
// is a flat object, the fields of compare/queries.py's Query that matter here:
//
//   {"name": "Q2", "kind": "rows", "start_ns": N, "end_ns": N, "locate": L}
//       kind is rows, minute_aggregate or column_sum, as in compare/queries.py; locate -1
//       or absent means every locate. start_ns and end_ns are nanoseconds from midnight,
//       required by every kind but column_sum. A field present as anything but an integer,
//       null included, is an error, never a default.
//   {"kind": "busiest_locate", "start_ns": N, "end_ns": N}
//       The locate with most rows in the window, 0 excluded, the lowest on a tie: the
//       parameter compare/queries.py's busiest_locate() picks.
//   {"kind": "quit"}
//
// A rows, minute_aggregate or column_sum request may add "ack": true. The worker then writes
// {"name", "kind", "answered": true, "rows", "engine_ns"} as soon as the engine has the answer
// in memory, and the full answer line after it, once the digest is made. The comparison harness
// stops its clock on the first line, so the digest falls outside it, as the other systems'
// digests do. An error has no first line.
//
// The ready line holds the store's records and segments as the reader parsed them, and what the
// harness reads back as settings in force: `store_version` and `segment_records` from the bytes
// of the store section, `kernel_level`, the kernel set this process chose (codec/simd.hpp, so a
// TICK_STORE_SIMD cap is applied before the line is printed), and `kernel_best`, the best set
// this build and processor have.
//
// Every answer carries `name` and `kind` back, then:
//
//   rows, sha256      for rows, the row count and the digest compare/queries.py's
//                     rows_digest() gives the same rows: sorted by seq, each ROW_COLUMNS
//                     column by name, int64 little-endian values, ts on the trading day,
//                     stock as ASCII each followed by a zero byte
//   rows, sha256,     for the aggregates, aggregate_digest()'s row count and digest of the
//   answer            compact JSON of the answer, and the answer itself
//   engine_ns         the query call alone: everything from the store's bytes to the
//                     answer as values in memory, every block read and decoded inside it
//   digest_ns         the digest, after the engine
//   segments, exact_segments, blocks, bytes_decoded, values_decoded
//                     what the engine read (store/itch_query.hpp's QueryStats)
//   kernel_runs       calls into each kernel set during this request, by level name
//
// An error answers {"error": "..."} and the worker goes on. Exit codes are those of
// roundtrip_cli.hpp: 0 after a quit or the end of stdin, 1 if the store does not open, 2 for
// usage. Output holds no paths.

#include "tick_store/harness/roundtrip_cli.hpp"

#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>

namespace tick_store::harness {

// Nanoseconds from the Unix epoch to UTC midnight of a MMDDYYYY label, as
// compare/schema.py's TradingDay.midnight_ns, or nullopt for a label that is not a date.
[[nodiscard]] std::optional<std::int64_t> midnight_ns_of(std::string_view label) noexcept;

[[nodiscard]] int run_query_cli(std::span<const std::string_view> arguments, std::istream& in,
                                std::ostream& out, CliOutput& output);

}  // namespace tick_store::harness
