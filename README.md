# tick-store

A C++23 columnar time-series store for Nasdaq TotalView-ITCH 5.0 tick data. A
**record** is one ITCH message of the source file.

Status: Phase 2, harness. `BENCHMARKS.md` holds the targets and `TASKS.md` the plan.

## Build and test

CMake 3.28 or newer and Ninja. The compiler comes from the environment (`CC`,
`CXX`), so nothing here names a toolchain path.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Presets, each building into `build/<preset>`: `dev`, `asan-ubsan`, `tsan`, `fuzz`,
`coverage`, `bench`. Every build preset caps itself at 6 parallel jobs. Fuzzing is described in
`fuzz/README.md`.

## Checks

`scripts/check.sh` runs every check, one step at a time, because a clang-tidy run and a
sanitizer build at once have run this shared machine out of memory. Every step runs even after
a failure. It then prints a summary table, and exits 1 if any step failed.

```sh
scripts/check.sh                          # everything, about 8 minutes on a warm build tree
scripts/check.sh --quick                  # no sanitizers, fuzzing or coverage, and it says so
scripts/check.sh --fuzz-seconds 60        # each fuzz target's budget, 20 s by default
scripts/check.sh --only dev-test,hygiene  # chosen steps, plus the builds they need
scripts/check.sh --list                   # the step names
```

`CC`, `CXX` and `clang-tidy` come from the environment, from the same upstream LLVM. A missing
one is a clear error, and so is a missing `actionlint`, which `scripts/install_tools.sh
actionlint shellcheck` installs into the gitignored `.tools/` after checking its SHA-256. Raw
step logs go under `build/check/`, or `--log-dir` or `CHECK_LOG_DIR`.

The steps: the `dev` build with `-Wall -Wextra -Werror` and its tests, G-1 and G-2 through
`tick_roundtrip` on a small generated stream, the `asan-ubsan` and `tsan` builds and their
tests, clang-tidy over all our C++, each libFuzzer target, coverage, the Python tests,
`scripts/check_results.py`, `scripts/check_compare.py`, `scripts/render_results.py --check`,
actionlint, and a check of what git tracks: no home path, no data file, nothing over 1 MB but
the named results. No step loads a whole day. `ItchRealDay` reads the first 2 million messages.

A skip is never a pass. A step that cannot run shows as `SKIP`, as `py-compare` does with no
`.venv`, and a test skipped inside a passing step is named in that row. `ItchRealDay` skips
this way where the data is absent. The two sources a host does not compile, such as
`src/harness/page_cache_linux.cpp` on macOS, are named as not analysed by clang-tidy.

A warning in a dependency cannot fail the build. `-Werror` is a `PRIVATE` option on our own
targets (`cmake/Warnings.cmake`), so a dependency's sources never see it, and FetchContent's
`SYSTEM` puts their headers on `-isystem`, which silences a warning from a header we include.
The fault `werror-dependency-tolerated` shows both: unused variables added to a GoogleTest
source and header print a warning and the build passes.

## Coverage

`scripts/coverage.sh` builds the `coverage` preset, runs the unit tests and reports line and
function coverage per file for `src/` and `include/`, with llvm-cov. It fails if a function on
the required list in `scripts/coverage_report.py` is missing or was never called: every
`decode_as` and `encode_as` for the 23 types, every parser and validator entry point, the fuzz
oracles and the oracle's compare path. `results/coverage.json` is the curated summary, written
with `--output results/coverage.json`. No SIMD kernels exist yet, so none are reported. llvm-cov
sees only the C++, so the Python validators are covered by their tests in `tests/scripts/`
instead.

## Proof that each check can fail

A check that cannot fail proves nothing, so `scripts/prove_checks.py` shows each one failing on
an injected fault.

```sh
python3 -I scripts/prove_checks.py            # resumes, skipping what is already proven
python3 -I scripts/prove_checks.py --force    # redoes every fault
python3 -I scripts/prove_checks.py --only ubsan-shift-64
```

Each fault is a patch in `faults/` or a data mutation in the script. It runs in a throwaway
clone under `build/fault-proofs/`, never the working tree, and no production code knows about
it. The step must pass on the clean clone, then fail with the expected exit code and the
expected string in its own log, so a build error cannot pass for a detection. The clone keeps
one incremental build per preset, and proving every fault on those builds takes about 22 minutes.
`results/check-proofs.json` records each fault, and an undetected one is a bug in the check.

## Continuous integration

`.github/workflows/ci.yml` runs `scripts/check.sh` on Linux x86-64 and macOS arm64, one job per
step group, on a pinned LLVM. The steps are the same as a local run, and
`tests/scripts/test_check_sh.py` fails if one of them is missing from the workflow. The data is
absent on CI, so `ItchRealDay` skips visibly, and with no `.venv`, `py-compare` shows as `SKIP`.

**This workflow has never executed on GitHub: the repository has no remote.** It is validated
by actionlint with shellcheck. By hand on macOS arm64, a fresh clone ran `install_tools.sh`,
the environment step and each job's `check.sh` line, with 240 s of fuzzing per target, on the
pinned LLVM, and all passed. The Linux jobs, `apt-get` and the GitHub actions themselves have
never run.

## Round-trip check

Gates G-1 and G-2 of `BENCHMARKS.md`. `tick_roundtrip` stores a day, then proves that
every record comes back bit-exact. Fetch and verify the data with `scripts/fetch_data.py`
first, and take the pin from `decompressed_sha256` in `scripts/data_pins.json`.

```sh
cmake --preset bench && cmake --build --preset bench
tools=build/bench/tools
day=data/12302019.NASDAQ_ITCH50
pin=5d81c2e14a0f748b29c674b6a342796932702034b4dd341e39e9a9ec5bac610f

$tools/tick_roundtrip ingest-stub $day build/12302019.stub
$tools/tick_roundtrip verify $day build/12302019.stub --expect-sha256 $pin
```

`verify` walks the source's frames and the stored records in lock step, comparing every
byte, and hashes both whole streams on worker threads. It prints a JSON summary, with
records, bytes, both hashes, the verdict and the wall time. It exits 0 on a pass, 1 on a
mismatch and 2 on a usage or I/O error. A mismatch names the first difference: record
index, source byte offset, bit, expected and actual byte, and message type.

To see it catch corruption, flip one bit of the store, where bit 0 is the least
significant. Flipping the same bit again restores the file.

```sh
$tools/tick_roundtrip flip-bit build/12302019.stub 1390407067 1
$tools/tick_roundtrip verify $day build/12302019.stub --expect-sha256 $pin   # exits 1
$tools/tick_roundtrip flip-bit build/12302019.stub 1390407067 1
```

The store here is a stub with no checksum, on purpose, so the oracle is what catches
the fault. Each step needs about 30 seconds and the store takes as much disk as the
day. Results for both pinned days are in `results/`, and for one day on Linux in
`results/roundtrip-12302019-linux.json`.

`tick_census <source> <output.json>` writes the aggregate statistics in `results/`.
`tick_slice <source> <output> [--from HH:MM:SS] [--to HH:MM:SS]` writes a small valid
stream for the comparison harness: every `S` and `R` of the day plus one window.

## Benchmarks

`bench/itch_codec_bench` measures ITCH decode, and decode then encode, over the pinned
slice, `data/slices/12302019-open.itch`. Build the slice with `tick_slice` as
`scripts/data_pins.json` records. `scripts/run_bench.py` drives the benchmark and writes
one results file per host, `results/bench/<host>-<benchmark>-<commit>.json`:

```sh
cmake --preset bench && cmake --build --preset bench
TICK_STORE_MEASURE=/path/to/measure.sh \
    python3 -I scripts/run_bench.py build/bench/bench/itch_codec_bench
python3 -I scripts/check_results.py results/bench/*.json
python3 -I scripts/render_results.py
```

`TICK_STORE_MEASURE` names a wrapper that takes the machine, runs its arguments and prints
a load line at each end. Exit 75 from it means retry later. Without the variable there is
no lock, so the file is marked iteration-only, goes under `build/`, and the validator
refuses it. Each case runs as its own process, so `perf stat` counts one workload. On
Linux, the case runs on `cpu4` under `taskset`, and `perf` counts only the timed passes.
Nothing keeps that core's SMT sibling idle, so each case records in `sibling_busy_percent`
how busy the sibling was, from `/proc/stat` once the lock is held and again at the end. The
validator refuses a reported result where it exceeds 5% in any case. That limit is a choice
of ours, not a measured threshold.

Before any time counts, the input must match its SHA-256 pin, and decoding then encoding
it must rebuild exactly those bytes. Each timed pass is checked afterwards too. Batch
times go into an HdrHistogram in timer ticks. Nothing is published below 10 timer steps
or 5 timer reads: the binary refuses to, and the validator refuses the file.

The tables below are generated from the results files by `scripts/render_results.py`.
`--check` fails if they are stale. These numbers are harness evidence, not targets:
`BENCHMARKS.md` holds the targets.

<!-- results:begin, generated by scripts/render_results.py, do not edit -->
**Linux.** AMD Ryzen Embedded V1605B with Radeon Vega Gfx, Clang 23.1.4, `-O3 -DNDEBUG -std=c++23`, x86-64 baseline, scalar kernels. Pinned with taskset -c 4 and verified by sched_getcpu. Counters come from perf stat user-mode events. perf cycles and timer ticks are different units.

| Case | Commit | Passes | Messages/s | Spread (CV) | Batch | p50 µs | p99 µs | p99.9 µs | Cycles/msg | Instr/msg | Branch misses/msg | Load at start |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `itch_decode` | `1b7cfe196483` | 10 | 59.79 M | 0.25% | 1024 msgs | 17.44 | 21.80 | 35.08 | 59.76 | 134.96 | 0.51 | 0.78 |
| `itch_decode_encode` | `1b7cfe196483` | 10 | 33.88 M | 0.24% | 1024 msgs | 29.98 | 39.27 | 51.19 | 105.18 | 271.38 | 0.51 | 0.81 |

Percentiles are the time for one whole batch, in microseconds, with the timer resolving 10.01 ns.

**macOS.** Apple M4 Max, Clang 23.1.4, `-O3 -DNDEBUG -std=c++23`, arm64 baseline, scalar kernels. Unpinned. No hardware counters. macOS has no thread pinning, and no unprivileged hardware counters.

| Case | Commit | Passes | Messages/s | Spread (CV) | Batch | p50 µs | p99 µs | p99.9 µs | Cycles/msg | Instr/msg | Branch misses/msg | Load at start |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `itch_decode` | `ad2bab57a412` | 10 | 130.06 M | 0.80% | 1024 msgs | 8.21 | 9.46 | 12.79 | n/a | n/a | n/a | 3.80 |
| `itch_decode_encode` | `ad2bab57a412` | 10 | 69.86 M | 0.67% | 1024 msgs | 14.42 | 17.08 | 21.92 | n/a | n/a | n/a | 3.65 |

Percentiles are the time for one whole batch, in microseconds, with the timer resolving 41.67 ns.
<!-- results:end -->
