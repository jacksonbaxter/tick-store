#!/usr/bin/env bash
# Measures what the unit tests reach, and fails if a required function is not reached.
#
#     scripts/coverage.sh [--output FILE]
#
# llvm-profdata, llvm-cov and llvm-cxxfilt come from PATH, from the same LLVM as CXX. The tests
# run as one process rather than through ctest, so there is one profile to merge. ItchRealDay
# stays out, so that the report does not depend on whether the data is on this machine. The
# fixed grid (Grid/OracleGrid) and the named seeds' tests (FuzzRegression/BoundSeeds) stay out
# too: single-threaded re-runs of the fuzz oracles on fixed inputs, run in full by check.sh's
# unit and asan-ubsan steps on each host, which took the coverage job from 9 minutes to over 55
# (fuzz/README.md, "Bounds"). FuzzOracles and the rest of FuzzRegression still run here.
#
# Writes the curated summary to FILE, by default build/coverage/coverage.json. Pass
# --output results/coverage.json to publish it. scripts/coverage_report.py holds the required
# list and the gate. Exit 1 if a test fails or a required function is not reached.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

OUTPUT=build/coverage/coverage.json
while [[ $# -gt 0 ]]; do
    case $1 in
        --output) OUTPUT=$2; shift ;;
        *) echo "coverage.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

for tool in llvm-profdata llvm-cov llvm-cxxfilt; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "coverage.sh: $tool is not on PATH. Use the one from the same LLVM as CXX." >&2
        exit 2
    fi
done

# shellcheck disable=SC2086  # CHECK_CMAKE_ARGS is a list of arguments.
cmake --preset coverage ${CHECK_CMAKE_ARGS:-}
cmake --build --preset coverage

BINARY=build/coverage/tests/unit/tick_store_unit_tests
PROFILES=build/coverage/profiles
rm -rf "$PROFILES"
mkdir -p "$PROFILES"
# What the header says stays out. The scalar-capped parts are ctest's, which this does not run.
FILTER='-ItchRealDay.*:Grid/OracleGrid.*:FuzzRegression/BoundSeeds.*'
LLVM_PROFILE_FILE=$PROFILES/unit-%p.profraw "$BINARY" --gtest_filter="$FILTER" --gtest_brief=1
llvm-profdata merge --sparse --output "$PROFILES/unit.profdata" "$PROFILES"/unit-*.profraw
llvm-cov export --format=text --skip-expansions --skip-branches \
    --instr-profile "$PROFILES/unit.profdata" "$BINARY" >"$PROFILES/export.json"
python3 -I scripts/coverage_report.py "$PROFILES/export.json" "$OUTPUT"
