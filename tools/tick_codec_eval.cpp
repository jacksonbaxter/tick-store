// The Gorilla codecs, and any codec registered beside them, on the columns of a whole
// ITCH day. Usage is in harness/codec_eval.hpp.

#include "tick_store/bench_support/build_info.hpp"
#include "tick_store/harness/codec_eval.hpp"
#include "tick_store/harness/roundtrip_cli.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// NOLINTNEXTLINE(bugprone-exception-escape): an unexpected exception should terminate and say why.
int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string_view> arguments(args.begin() + 1, args.end());
    const auto& build = tick_store::bench_support::build_info();
    const std::string compiler =
        std::string{build.compiler_id} + " " + std::string{build.compiler_version};
    const tick_store::harness::EvalProvenance provenance{.commit = build.commit,
                                                         .tree_dirty = build.tree_dirty,
                                                         .compiler = compiler,
                                                         .build_type = build.build_type,
                                                         .sanitizers = build.sanitizers};
    tick_store::harness::CliOutput output;
    const int code = tick_store::harness::run_codec_eval_cli(arguments, provenance, output);
    return tick_store::harness::write_cli_output(output, code);
}
