// The comparison harness's slice of a day. Usage is in harness/data_tools_cli.hpp.

#include "tick_store/harness/data_tools_cli.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

// NOLINTNEXTLINE(bugprone-exception-escape): an unexpected exception should terminate and say why.
int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string_view> arguments(args.begin() + 1, args.end());
    tick_store::harness::CliOutput output;
    const int code = tick_store::harness::run_slice_cli(arguments, output);
    return tick_store::harness::write_cli_output(output, code);
}
