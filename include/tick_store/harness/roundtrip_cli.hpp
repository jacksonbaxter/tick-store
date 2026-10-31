#pragma once

// The command line of tools/tick_roundtrip, as a function, so tests can run every path
// and read its exit code and output without starting a process.
//
//   tick_roundtrip ingest <source> <store>
//       Decodes every source record and writes it through the stub store's writer.
//   tick_roundtrip verify <source> <store> [--expect-sha256 HEX]
//       Runs the oracle. Prints a JSON summary. On a failure, also describes the first
//       difference on stderr.
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
