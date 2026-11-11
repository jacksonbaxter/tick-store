#!/usr/bin/env bash
# Runs every check of the project, one step at a time, then prints a summary table.
#
#     scripts/check.sh [--quick] [--fuzz-seconds N] [--log-dir DIR] [--only a,b,c]
#     scripts/check.sh --list
#
# The toolchain comes from the environment: CXX and CC, clang-tidy, llvm-cov and the rest
# found on PATH, as an environment script sets them up. Nothing here names a toolchain path.
#
# Steps run in sequence, and never in parallel, because a clang-tidy run and a sanitizer
# build at once have run a shared machine out of memory. Every step runs even after a
# failure. Exit 1 if any step failed or could not run for want of a step before it, exit 2
# on a usage or toolchain error. A skipped step is reported as skipped and is not a pass.
#
# Options:
#   --quick           skip the sanitizers, fuzzing and coverage, and say so in the summary
#   --fuzz-seconds N  each fuzz target's budget, 20 by default
#   --fuzz-seed N     a fixed libFuzzer seed, for a repeatable run
#   --fresh-corpus    drop the saved fuzz corpus first, so a run starts from the seeds
#   --log-dir DIR     where raw step logs go, or set CHECK_LOG_DIR. Default build/check
#   --only a,b,c      run these steps, plus the build each one needs
#   --test-regex RE   run only the ctest tests that match
#   --tidy-files a,b  run clang-tidy on these repo-relative files only
#   --list            print the step names
#
# CHECK_CMAKE_ARGS holds extra arguments for each configure, and ACTIONLINT a path to the
# actionlint binary if it is neither in .tools/ nor on PATH.

# Step lists split on purpose. A step reads its own log only after writing it.
# shellcheck disable=SC2086,SC2094

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT" || exit 2

FUZZ_TARGETS="fuzz-columns fuzz-container fuzz-dictionary fuzz-gorilla-timestamps fuzz-gorilla-xor
fuzz-itch-frame fuzz-itch-message fuzz-itch-query fuzz-itch-store fuzz-pfor fuzz-run-length
fuzz-stream-vbyte fuzz-stub-store"
ALL_STEPS="dev-build dev-test oracle asan-build asan-test tsan-build tsan-test clang-tidy
fuzz-build $FUZZ_TARGETS coverage py-scripts py-compare check-results check-compare
render-check actionlint hygiene"
JOBS=6
TEST_TIMEOUT=900

QUICK=0
FUZZ_SECONDS=20
FUZZ_SEED=
FRESH_CORPUS=0
LOG_DIR=${CHECK_LOG_DIR:-$ROOT/build/check}
ONLY=
TEST_REGEX=
TIDY_FILES=

usage() {
    sed -n '2,/^# actionlint binary/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}" >&2
    exit 2
}

value_of() {
    [[ $# -ge 2 ]] || { echo "check.sh: $1 needs a value" >&2; exit 2; }
    printf '%s\n' "$2"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick) QUICK=1 ;;
        --fresh-corpus) FRESH_CORPUS=1 ;;
        --fuzz-seconds) FUZZ_SECONDS=$(value_of "$@") || exit 2; shift ;;
        --fuzz-seed) FUZZ_SEED=$(value_of "$@") || exit 2; shift ;;
        --log-dir) LOG_DIR=$(value_of "$@") || exit 2; shift ;;
        --only) ONLY=$(value_of "$@") || exit 2; shift ;;
        --test-regex) TEST_REGEX=$(value_of "$@") || exit 2; shift ;;
        --tidy-files) TIDY_FILES=$(value_of "$@") || exit 2; shift ;;
        --list) printf '%s\n' $ALL_STEPS; exit 0 ;;
        -h|--help) usage ;;
        *) echo "check.sh: unknown argument: $1" >&2; usage ;;
    esac
    shift
done

if ! [[ $FUZZ_SECONDS =~ ^[1-9][0-9]*$ ]]; then
    echo "check.sh: --fuzz-seconds needs a positive whole number, not '$FUZZ_SECONDS'" >&2
    exit 2
fi

# Named exactly, so a missing tool stops the run here and not as a confusing step failure.
require_tools() {
    if [[ -z ${CXX:-} ]]; then
        echo "check.sh: CXX is not set. Set CC and CXX to an upstream clang, for example by" >&2
        echo "sourcing your toolchain's environment script." >&2
        exit 2
    fi
    if ! command -v "$CXX" >/dev/null 2>&1; then
        echo "check.sh: CXX is set, but '$CXX' cannot be run." >&2
        exit 2
    fi
    if ! command -v clang-tidy >/dev/null 2>&1; then
        echo "check.sh: clang-tidy is not on PATH. Use the one from the same LLVM as CXX." >&2
        exit 2
    fi
    local tool
    for tool in cmake ctest ninja python3 git; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "check.sh: $tool is not on PATH." >&2
            exit 2
        fi
    done
}
require_tools

# Steps a step cannot run without. A step blocked this way is a failure, not a skip.
needs() {
    case $1 in
        dev-test|oracle) echo dev-build ;;
        asan-test) echo asan-build ;;
        tsan-test) echo tsan-build ;;
        fuzz-build) ;;
        fuzz-*) echo fuzz-build ;;
    esac
}

# What --quick leaves out: everything that needs a sanitizer or instrumented build.
quick_skips() {
    case $1 in
        asan-*|tsan-*|fuzz-*|coverage) return 0 ;;
        *) return 1 ;;
    esac
}

selected_steps() {
    local step wanted
    if [[ -z $ONLY ]]; then
        printf '%s\n' $ALL_STEPS
        return
    fi
    wanted=" ${ONLY//,/ } "
    for step in $wanted; do
        if ! printf '%s\n' $ALL_STEPS | grep -qx -- "$step"; then
            echo "check.sh: unknown step: $step. Use --list to see them." >&2
            return 2
        fi
    done
    for step in $ALL_STEPS; do
        if [[ $wanted == *" $step "* ]]; then
            printf '%s\n' "$step"
        elif printf '%s\n' $wanted | while read -r want; do needs "$want"; done |
                grep -qx -- "$step"; then
            printf '%s\n' "$step"
        fi
    done
}

STEPS=$(selected_steps) || exit 2

mkdir -p "$LOG_DIR" "$ROOT/build/tmp" || exit 2
LOG_DIR=$(cd "$LOG_DIR" && pwd -P) || exit 2
rm -f "$LOG_DIR"/*.log "$LOG_DIR/summary.tsv"
# Every tool that wants a scratch file gets it inside the gitignored build folder.
export TMPDIR=$ROOT/build/tmp
export TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
# LeakSanitizer comes with ASan, on by default on Linux and off on macOS, where a leaking test
# or fuzz target passed. It is asked for on both. libFuzzer checks each input for a leak
# itself; its check at exit stays off on macOS, where it reports libFuzzer's own RSS thread,
# which is never freed. Options set in ASAN_OPTIONS come after these, so they win.
if [[ $(uname -s) == Darwin ]]; then
    FUZZ_LEAKS=detect_leaks=1:leak_check_at_exit=0
else
    FUZZ_LEAKS=detect_leaks=1
fi

SUMMARY=$LOG_DIR/summary.tsv
: >"$SUMMARY"

# The status a step ended with, or nothing if it has not run.
status_of() {
    awk -F'\t' -v step="$1" '$1 == step { print $2 }' "$SUMMARY"
}

record() {
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >>"$SUMMARY"
    printf '      %s, %ss%s\n' "$2" "$4" "${5:+: $5}"
}

# A step function sets NOTE for the summary, and calls skip_step to report that it could
# not run for a reason outside the code under test.
NOTE=
SKIP_REASON=
skip_step() { SKIP_REASON=$1; }

run_step() {
    local name=$1 log=$LOG_DIR/$1.log blocker start code
    blocker=$(needs "$name")
    if [[ -n $blocker && $(status_of "$blocker") != PASS ]]; then
        record "$name" BLOCKED - 0 "$blocker did not pass"
        return
    fi
    NOTE=
    SKIP_REASON=
    start=$SECONDS
    "step_${name//-/_}" "$log" >"$log" 2>&1
    code=$?
    if [[ -n $SKIP_REASON ]]; then
        record "$name" SKIP - $((SECONDS - start)) "$SKIP_REASON"
    elif [[ $code -eq 0 ]]; then
        record "$name" PASS 0 $((SECONDS - start)) "$NOTE"
    else
        record "$name" FAIL "$code" $((SECONDS - start)) "${NOTE:-see $name.log}"
    fi
}

build_preset() {
    cmake --preset "$1" ${CHECK_CMAKE_ARGS:-} && cmake --build --preset "$1"
}

# ctest exits 0 when it finds no tests at all, unless told otherwise, and a skipped test
# counts as passed in its percentage. So nothing is ever taken from that line alone.
test_preset() {
    local preset=$1 log=$2 code skipped total
    local args=(--preset "$preset" --no-tests=error --timeout "$TEST_TIMEOUT")
    [[ -z $TEST_REGEX ]] || args+=(-R "$TEST_REGEX")
    ctest "${args[@]}"
    code=$?
    total=$(sed -n 's/.*tests passed, [0-9]* tests failed out of \([0-9]*\).*/\1/p' "$log")
    [[ -n $total ]] || total=$(sed -n 's/.*tests passed out of \([0-9]*\).*/\1/p' "$log")
    skipped=$(grep -c '(Skipped)' "$log")
    NOTE="${total:-no} tests"
    if [[ $skipped -gt 0 ]]; then
        NOTE="$NOTE, $skipped SKIPPED: $(sed -n 's/.* - \(.*\) (Skipped).*/\1/p' "$log" |
            paste -sd, -)"
    fi
    return "$code"
}

step_dev_build() { build_preset dev; }
step_dev_test() { test_preset dev "$1"; }
step_asan_build() { build_preset asan-ubsan; }
step_asan_test() {
    local -x ASAN_OPTIONS=detect_leaks=1${ASAN_OPTIONS:+:$ASAN_OPTIONS}
    test_preset asan-ubsan "$1"
}
step_tsan_build() { build_preset tsan; }
step_tsan_test() { test_preset tsan "$1"; }

step_oracle() {
    python3 -I scripts/check_oracle.py build/dev/tools/tick_roundtrip
}

# The sources this host does not compile, which clang-tidy therefore cannot see: those of
# the other operating system, and the CRC32C and SIMD kernels of the other processor.
files_of_other_platforms() {
    if [[ $(uname -s) == Linux ]]; then
        printf '%s\n' src/bench_support/affinity_unsupported.cpp src/harness/page_cache_darwin.cpp
    else
        printf '%s\n' src/bench_support/affinity_linux.cpp src/harness/page_cache_linux.cpp
    fi
    case $(uname -m) in
        arm64|aarch64) printf '%s\n' src/store/crc32c_sse42.cpp src/codec/simd/kernels_avx2.cpp ;;
        x86_64|amd64) printf '%s\n' src/store/crc32c_armv8.cpp src/codec/simd/kernels_neon.cpp ;;
        *)
            printf '%s\n' src/store/crc32c_armv8.cpp src/store/crc32c_sse42.cpp \
                src/codec/simd/kernels_neon.cpp src/codec/simd/kernels_avx2.cpp
            ;;
    esac
}

# The tracked C++ sources. A tree exported with git archive, as a sync to the Linux host
# makes, has no .git and carries a COMMIT file instead, and every file in it is tracked.
# Without this, clang-tidy on that host found no files, and so never saw the Linux-only ones.
tracked_sources() {
    if [[ $(git rev-parse --show-toplevel 2>/dev/null) == "$ROOT" ]]; then
        git ls-files 'src/*.cpp' 'tests/*.cpp' 'fuzz/*.cpp' 'tools/*.cpp' 'bench/*.cpp'
    elif [[ -f COMMIT ]]; then
        find src tests fuzz tools bench -type f -name '*.cpp' | LC_ALL=C sort
    else
        echo "neither a git work tree nor an exported tree with a COMMIT file, so no sources"
        return 1
    fi
}

# Each tracked source has to be found in a compile database, or be named above. A new file
# in no build fails here rather than going unchecked. Headers are covered through the
# sources that include them, by HeaderFilterRegex in .clang-tidy.
step_clang_tidy() {
    local file preset code=0 other analysed=0 missing=0 selected
    local presets="dev fuzz bench"
    for preset in $presets; do
        cmake --preset "$preset" ${CHECK_CMAKE_ARGS:-} || return 1
        : >"$LOG_DIR/.tidy-$preset"
    done
    other=$(files_of_other_platforms)
    if [[ -n $TIDY_FILES ]]; then
        selected=${TIDY_FILES//,/ }
    else
        selected=$(tracked_sources) || { echo "$selected"; return 1; }
    fi
    for file in $selected; do
        if printf '%s\n' "$other" | grep -qx -- "$file"; then
            continue
        fi
        for preset in $presets; do
            if grep -q "\"file\": \"$ROOT/$file\"" "build/$preset/compile_commands.json"; then
                echo "$file" >>"$LOG_DIR/.tidy-$preset"
                analysed=$((analysed + 1))
                continue 2
            fi
        done
        echo "no compile database holds $file, so clang-tidy cannot analyse it"
        missing=$((missing + 1))
    done
    for preset in $presets; do
        if [[ -s $LOG_DIR/.tidy-$preset ]]; then
            echo "== clang-tidy -p build/$preset:"
            sed 's/^/   /' "$LOG_DIR/.tidy-$preset"
            xargs -n 1 -P "$JOBS" clang-tidy --quiet -p "build/$preset" \
                <"$LOG_DIR/.tidy-$preset" || code=1
        fi
        rm -f "$LOG_DIR/.tidy-$preset"
    done
    NOTE="$analysed files"
    if [[ -z $TIDY_FILES ]]; then
        NOTE="$NOTE, not built on this host so NOT analysed: $(printf '%s\n' "$other" | paste -sd, -)"
    fi
    [[ $analysed -gt 0 && $missing -eq 0 ]] || code=1
    return "$code"
}

step_fuzz_build() { build_preset fuzz; }

# Flags and their reasons are in fuzz/README.md. libFuzzer exits 0 after no runs at all if
# it is handed nothing to do, so the run count from its final stats has to be above zero.
run_fuzzer() {
    local log=$1 target=$2 corpus=$3 seeds=build/fuzz/fuzz-seeds/$4 max_len=$5
    local corpus_dir=build/fuzz/corpus/$corpus code runs
    if [[ -z $(ls -A "$seeds" 2>/dev/null) ]]; then
        echo "no seeds in $seeds"
        return 1
    fi
    [[ $FRESH_CORPUS -eq 0 ]] || rm -rf "$corpus_dir"
    mkdir -p "$corpus_dir" build/fuzz/artifacts
    local args=("-max_total_time=$FUZZ_SECONDS" "-max_len=$max_len" -rss_limit_mb=2048
                -malloc_limit_mb=256 -timeout=10 -print_final_stats=1
                -artifact_prefix=build/fuzz/artifacts/)
    [[ -z $FUZZ_SEED ]] || args+=("-seed=$FUZZ_SEED")
    # ASan aborts on macOS and exits 1 on Linux. Pinned to libFuzzer's own code for a defect.
    ASAN_OPTIONS=$FUZZ_LEAKS:abort_on_error=0:exitcode=77${ASAN_OPTIONS:+:$ASAN_OPTIONS} \
        "build/fuzz/fuzz/$target" "${args[@]}" "$corpus_dir" "$seeds"
    code=$?
    runs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$log")
    NOTE="${runs:-no} runs in ${FUZZ_SECONDS}s"
    [[ $code -ne 0 || ${runs:-0} -gt 0 ]] || code=1
    return "$code"
}

step_fuzz_columns() { run_fuzzer "$1" fuzz_columns columns columns 70000; }
step_fuzz_container() { run_fuzzer "$1" fuzz_container container container 70000; }
step_fuzz_dictionary() { run_fuzzer "$1" fuzz_dictionary dictionary dictionary 16384; }
step_fuzz_gorilla_timestamps() {
    run_fuzzer "$1" fuzz_gorilla_timestamps gorilla_timestamps gorilla_timestamps 70000
}
step_fuzz_gorilla_xor() { run_fuzzer "$1" fuzz_gorilla_xor gorilla_xor gorilla_xor 70000; }
step_fuzz_itch_frame() { run_fuzzer "$1" fuzz_itch_frame itch_frame frame 70000; }
step_fuzz_itch_message() { run_fuzzer "$1" fuzz_itch_message itch_message message 256; }
step_fuzz_itch_query() { run_fuzzer "$1" fuzz_itch_query itch_query itch_query 131072; }
step_fuzz_itch_store() { run_fuzzer "$1" fuzz_itch_store itch_store itch_store 70000; }
step_fuzz_pfor() { run_fuzzer "$1" fuzz_pfor pfor pfor 16384; }
step_fuzz_run_length() { run_fuzzer "$1" fuzz_run_length run_length run_length 16384; }
step_fuzz_stream_vbyte() { run_fuzzer "$1" fuzz_stream_vbyte stream_vbyte stream_vbyte 70000; }
step_fuzz_stub_store() { run_fuzzer "$1" fuzz_stub_store stub_store store 70000; }

step_coverage() {
    scripts/coverage.sh --output build/coverage/coverage.json
    local code=$?
    NOTE=$(sed -n 's/^coverage summary: //p' "$1" | tail -n 1)
    return "$code"
}

# Runs unittest files one by one with the given interpreter. A file that ran no test has
# proved nothing, so it fails. Skipped tests go in the note, and a file's failure does not
# stop the next file.
run_unittests() {
    local log=$1 python=$2 code=0 file ran skipped total=0 skips=0 output
    shift 2
    for file in "$@"; do
        echo "== $file"
        output=$("$python" -I "$file" 2>&1) || code=1
        printf '%s\n' "$output"
        ran=$(printf '%s\n' "$output" | sed -n 's/^Ran \([0-9]*\) tests\{0,1\} in .*/\1/p')
        skipped=$(printf '%s\n' "$output" | sed -n 's/.*skipped=\([0-9]*\).*/\1/p' | tail -n 1)
        if [[ ${ran:-0} -eq 0 ]]; then
            echo "$file ran no tests"
            code=1
        fi
        total=$((total + ${ran:-0}))
        skips=$((skips + ${skipped:-0}))
    done
    NOTE="$total tests"
    [[ $skips -eq 0 ]] || NOTE="$NOTE, $skips SKIPPED"
    return "$code"
}

# The guard's tests need nothing beyond the standard library, so they run here, on CI too.
step_py_scripts() { run_unittests "$1" python3 tests/scripts/test_*.py tests/compare/test_guard.py; }

# test_real_systems.py starts QuestDB and is no part of a quick check.
step_py_compare() {
    if [[ ! -x .venv/bin/python ]]; then
        skip_step "no .venv, so tests/compare did not run"
        return 0
    fi
    run_unittests "$1" .venv/bin/python tests/compare/test_coldcache.py \
        tests/compare/test_schema.py tests/compare/test_settings.py \
        tests/compare/test_system_worker.py tests/compare/test_tickstore.py
    local code=$?
    NOTE="$NOTE, fast set; test_real_systems.py not run here"
    return "$code"
}

# Each validator refuses an empty list only if it is handed one, so an empty folder fails
# here. A glob with no match would otherwise reach it as a literal path.
run_validator() {
    local validator=$1 folder=$2 files=()
    local file
    for file in "$folder"/*.json; do
        [[ -f $file ]] && files+=("$file")
    done
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "no results files in $folder"
        return 1
    fi
    python3 -I "$validator" "${files[@]}"
    local code=$?
    NOTE="${#files[@]} files"
    return "$code"
}

step_check_results() { run_validator scripts/check_results.py results/bench; }
step_check_compare() { run_validator scripts/check_compare.py results/compare; }
step_render_check() { python3 -I scripts/render_results.py --check; }

step_actionlint() {
    local tool=${ACTIONLINT:-}
    if [[ -z $tool && -x .tools/actionlint ]]; then
        tool=.tools/actionlint
    fi
    [[ -n $tool ]] || tool=$(command -v actionlint)
    if [[ -z $tool ]]; then
        echo "actionlint not found: run scripts/install_tools.sh actionlint"
        return 1
    fi
    if [[ ! -f .github/workflows/ci.yml ]]; then
        echo ".github/workflows/ci.yml is missing"
        return 1
    fi
    # Finds shellcheck on PATH, and checks the workflow's shell with it.
    PATH=$ROOT/.tools:$PATH "$tool" -no-color .github/workflows/ci.yml
}

step_hygiene() { python3 -I scripts/check_hygiene.py; }

BEGIN=$SECONDS
TOTAL=$(printf '%s\n' "$STEPS" | wc -l | tr -d ' ')
INDEX=0

echo "compiler: $("$CXX" --version | head -n 1 | sed 's/ (.*//')"
for step in $STEPS; do
    INDEX=$((INDEX + 1))
    printf '[%s/%s] %s\n' "$INDEX" "$TOTAL" "$step"
    if [[ $QUICK -eq 1 ]] && quick_skips "$step"; then
        record "$step" SKIP - 0 "--quick"
    else
        run_step "$step"
    fi
done

count() {
    awk -F'\t' -v status="$1" '$2 == status { n++ } END { print n + 0 }' "$SUMMARY"
}

{
    echo
    awk -F'\t' 'BEGIN { printf "%-18s %-8s %-5s %6s  %s\n", "step", "status", "exit", "time", "note" }
                { printf "%-18s %-8s %-5s %5ss  %s\n", $1, $2, $3, $4, $5 }' "$SUMMARY"
    echo
    if [[ $QUICK -eq 1 ]]; then
        echo "QUICK RUN: the sanitizers, fuzzing and coverage did NOT run."
    fi
    echo "pass $(count PASS), fail $(count FAIL), blocked $(count BLOCKED), skipped $(count SKIP)" \
         "(a skip is not a pass), wall time $((SECONDS - BEGIN))s"
    echo "logs: $LOG_DIR"
} | tee "$LOG_DIR/summary.txt"

[[ $(count FAIL) -eq 0 && $(count BLOCKED) -eq 0 ]]
