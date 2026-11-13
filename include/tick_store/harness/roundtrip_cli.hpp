#pragma once

// The command line of tools/tick_roundtrip, as a function, so tests can run every path
// and read its exit code and output without starting a process.
//
//   tick_roundtrip ingest <source> <store> --durability page_cache|device_flush
//                         [--segment-records N] [--first-records N]
//       Decodes every source record and writes the ITCH store (store/itch_store.hpp), in the
//       durability mode named: it is never a default. Prints a JSON summary, which lists in
//       `flushes` every flush the writer made, through the real system calls, with what it was
//       called on, the file or its directory, and its result. --first-records stores only the
//       source's first N records, with no tail: the comparison harness's small commit. A source
//       of fewer than N records is stored whole, its tail included.
//   tick_roundtrip ingest-stub <source> <store>
//       The same through the Phase 2 stub store, which the stub's own tests still use.
//   tick_roundtrip verify <source> <store> [--expect-sha256 HEX] [--no-checksums]
//       Runs the oracle against a store file: the ITCH store, or the stub store by its
//       magic. Block and section CRCs are checked unless --no-checksums says otherwise, so a
//       flipped bit is caught by its CRC with the block named, or with checksums skipped,
//       by the decoder or the oracle with the record named. Prints a JSON summary. On a
//       failure, also describes the first difference on stderr.
//   tick_roundtrip verify-columns <source> <columns-dir> [--expect-sha256 HEX]
//       The same oracle, over a column directory (store/columns.hpp) in place of a store.
//   tick_roundtrip layout <store> [--blocks]
//       What an ITCH store's bytes are spent on, from its footer: per column, per section
//       and the container's own. --blocks lists every block with its offset.
//   tick_roundtrip flip-bit <file> <byte_offset> <bit>
//       Flips one bit of a file in place. Bit 0 is the least significant bit.
//
// Exit codes: 0 pass; 1 mismatch, including a store its parser refuses; 2 usage or I/O
// error. JSON on stdout holds no paths.

#include <span>
#include <string>
#include <string_view>

namespace tick_store::harness {

inline constexpr int kExitPass = 0;
inline constexpr int kExitMismatch = 1;
inline constexpr int kExitUsageOrIo = 2;

struct CliOutput {
    std::string out;
    std::string err;
};

// `arguments` excludes the program name. Returns the exit code.
[[nodiscard]] int run_roundtrip_cli(std::span<const std::string_view> arguments, CliOutput& output);

}  // namespace tick_store::harness
