// The G-1 and G-2 tool. Usage and exit codes are in harness/roundtrip_cli.hpp.

#include "tick_store/harness/roundtrip_cli.hpp"

#include <cstddef>
#include <cstdio>
#include <print>
#include <span>
#include <string_view>
#include <vector>

// NOLINTNEXTLINE(bugprone-exception-escape): an unexpected exception should terminate and say why.
int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string_view> arguments(args.begin() + 1, args.end());
    tick_store::harness::CliOutput output;
    const int code = tick_store::harness::run_roundtrip_cli(arguments, output);
    std::print(stdout, "{}", output.out);
    std::print(stderr, "{}", output.err);
    return code;
}
