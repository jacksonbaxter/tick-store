#pragma once

// What built this binary, for a results file. CMake generates the definition, and
// compile_flags has already been through tick_store_sanitise_flags, so no path or
// username can reach a results file from here.

#include <string_view>

namespace tick_store::bench_support {

struct BuildInfo {
    std::string_view compiler_id;
    std::string_view compiler_version;
    std::string_view compile_flags;
    std::string_view build_type;
    // The TICK_STORE_SANITIZE value. Only "none" can produce a reported number.
    std::string_view sanitizers;
    // Full commit hash, or "unknown".
    std::string_view commit;
    // Tracked files differed from `commit` when this was built, or git could not say. Only
    // a git checkout is compared: for an exported tree, commit_source "export", nothing
    // was compared and this is false because no difference is known, not because none
    // was looked for (review A-8). The Linux host builds from such an export, made from
    // a committed HEAD by the sync script.
    bool tree_dirty = true;
    // How `commit` was established: "git" (rev-parse, and the tree compared with it by
    // git status), "export" (the tree's COMMIT file, not compared) or "unknown".
    std::string_view commit_source;
    // The instruction set and kernel level the measured code uses.
    std::string_view isa;
};

[[nodiscard]] const BuildInfo& build_info() noexcept;

}  // namespace tick_store::bench_support
