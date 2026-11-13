// Usage is in harness/query_cli.hpp.

#include "tick_store/harness/query_cli.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"

#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

// NOLINTNEXTLINE(bugprone-exception-escape): an unexpected exception should terminate and say why.
int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string_view> arguments(args.begin() + 1, args.end());
    tick_store::harness::CliOutput output;
    std::ios::sync_with_stdio(false);
    const int code = tick_store::harness::run_query_cli(arguments, std::cin, std::cout, output);
    std::cout.flush();
    return tick_store::harness::write_cli_output(output, code);
}
