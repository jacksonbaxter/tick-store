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

It is validated locally by actionlint with shellcheck. On GitHub, the first run failed 10 of
its 14 jobs; the causes and fixes are in `NOTES.md`, "The first runs on GitHub Actions". All 14
jobs passed at `ad542fd`, in [run 41825067372](https://github.com/jacksonbaxter/tick-store/actions/runs/41825067372).

## Round-trip check

Gates G-1 and G-2 of `BENCHMARKS.md`. `tick_roundtrip` stores a day in the ITCH store
(`docs/store.md`), then proves that every record comes back bit-exact. Fetch and verify the data
with `scripts/fetch_data.py` first, and take the pin from `decompressed_sha256` in
`scripts/data_pins.json`.

```sh
cmake --preset bench && cmake --build --preset bench
tools=build/bench/tools
day=data/12302019.NASDAQ_ITCH50
pin=5d81c2e14a0f748b29c674b6a342796932702034b4dd341e39e9a9ec5bac610f

$tools/tick_roundtrip ingest $day build/12302019.tsc --durability page_cache
$tools/tick_roundtrip verify $day build/12302019.tsc --expect-sha256 $pin
```

`ingest` takes the durability class, `page_cache` or `device_flush`, which is never a default,
and `--segment-records N`, by default 2^22. `verify` decodes the store segment by segment,
re-serialises every record through the ITCH encoder, and walks those frames and the source's in
lock step, comparing every byte, and hashes both whole streams on worker threads. Block CRCs are
checked as each block is read. It prints a JSON summary, with records, bytes, both hashes, the
verdict and the wall time. It exits 0 on a pass, 1 on a mismatch and 2 on a usage or I/O error. A
mismatch names the first difference: record index, source byte offset, bit, expected and actual
byte, and message type; a store error names the segment, column, block, file byte and record it
refused at.

To see it catch corruption, flip one bit of the store, where bit 0 is the least significant.
Flipping the same bit again restores the file. `layout --blocks` lists every block with its
segment, column and file offset, and `layout` shows the footer's offset. A flip inside a data
block is a CRC failure naming that block and its offset; with `--no-checksums` the decoder or the
oracle names the column and value, or the record, that came out wrong; a flip in the footer fails
the footer's CRC:

```sh
$tools/tick_roundtrip layout build/12302019.tsc --blocks > build/12302019-layout.json
$tools/tick_roundtrip flip-bit build/12302019.tsc 721746430 1   # segment 128's timestamp block
$tools/tick_roundtrip verify $day build/12302019.tsc --expect-sha256 $pin                  # exits 1
$tools/tick_roundtrip verify $day build/12302019.tsc --expect-sha256 $pin --no-checksums   # exits 1
$tools/tick_roundtrip flip-bit build/12302019.tsc 721746430 1
$tools/tick_roundtrip verify $day build/12302019.tsc --expect-sha256 $pin                  # exits 0
```

Byte 721,746,430 is the middle of segment 128's timestamp block in the store `ingest` writes from
the pinned day at the default settings; the writer is deterministic, so the offset is the same on
any host. Each step needs under a minute; the store is 1.45 GB, 5.69 times smaller than the day.
Results for both pinned days are in `results/roundtrip-12302019.json` and
`results/roundtrip-10302019.json`, with every byte of each store by column, section and footer in
`results/store-size-*.json`, and G-1 for one day on Linux in `results/roundtrip-12302019-linux.json`.
G-1 and G-2 are proved on the real store, not the stub: G-1 re-serialises every record the store
decodes and matches the whole stream's SHA-256 against the pin, and G-2 flips one bit in a data
block and one in the footer, each caught with CRCs on and with them off.

`tick_roundtrip ingest-stub` still writes the Phase 2 stub store, the day's frames behind a
40-byte header with no checksum, which `verify` recognises by its magic.

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

Two more binaries measure decoding, for `BENCHMARKS.md`'s decode targets, which say which
case answers which target. `bench/simd_kernel_bench` times each decode kernel of each set on
seeded input, one vector of 1,024 values in L1 (input kind `seeded`), and
`bench/column_decode_bench` times real columns built from the same slice (input kind
`column`): its add-order prices and timestamps, Gorilla against the store's own layout. Both
count values, not messages, and run the same way:

```sh
TICK_STORE_MEASURE=/path/to/measure.sh \
    python3 -I scripts/run_bench.py build/bench/bench/simd_kernel_bench
TICK_STORE_MEASURE=/path/to/measure.sh \
    python3 -I scripts/run_bench.py build/bench/bench/column_decode_bench
```

A kernel case is checked against the scalar set's output, or for `dod_reconstruct` against the
sequence it came from, before timing and after; `--verify-all` does that for every case at
every width, and ctest runs it. A column case runs only once the slice matches its pin and the
decode gives its column back bit for bit, and records the column's encoded size, so its
results carry a ratio. The README gives a column file a row a case, and a kernel file a row a
kernel and set, the median across the widths it ran.

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

**Linux, real-data column decodes of the slice.** AMD Ryzen Embedded V1605B with Radeon Vega Gfx, Clang 23.1.4, `-O3 -DNDEBUG -std=c++23`. Pinned with taskset -c 4 and verified by sched_getcpu. Counters come from perf stat user-mode events. perf cycles and timer ticks are different units.

| Case | Commit | Kernels | Values | Bits/value | ns/value | MB/s of 8-byte values | Spread (CV) | Cycles/value | Instr/value | Branch misses/value | Load at start |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `gorilla_xor/add_price` | `bcbf8738d892` | avx2 kernels | 3,153,991 | 37.18 | 20.558 | 389 | 0.52% | 73.35 | 218.11 | 0.391 | 1.49 |
| `gorilla_xor_l1/add_price` | `bcbf8738d892` | avx2 kernels | 3,072 | 33.34 | 17.115 | 467 | 0.50% | 60.91 | 198.40 | 0.419 | 1.41 |
| `store_decode/add_price` | `bcbf8738d892` | avx2 kernels | 3,153,991 | 6.30 | 6.252 | 1,280 | 0.66% | 22.32 | 47.00 | 0.051 | 1.32 |
| `gorilla_dod/timestamp` | `bcbf8738d892` | avx2 kernels | 6,125,294 | 16.95 | 22.204 | 360 | 0.63% | 79.54 | 274.19 | 0.404 | 1.29 |
| `pfor_delta/timestamp` | `bcbf8738d892` | avx2 kernels | 6,125,294 | 16.82 | 2.931 | 2,730 | 1.01% | 10.45 | 12.08 | 0.020 | 1.30 |
| `pfor_dod/timestamp` | `bcbf8738d892` | avx2 kernels | 6,125,294 | 18.07 | 3.151 | 2,539 | 1.45% | 11.21 | 14.57 | 0.020 | 1.27 |

**macOS.** Apple M4 Max, Clang 23.1.4, `-O3 -DNDEBUG -std=c++23`, arm64 baseline, scalar kernels. Unpinned. No hardware counters. macOS has no thread pinning, and no unprivileged hardware counters.

| Case | Commit | Passes | Messages/s | Spread (CV) | Batch | p50 µs | p99 µs | p99.9 µs | Cycles/msg | Instr/msg | Branch misses/msg | Load at start |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `itch_decode` | `ad2bab57a412` | 10 | 130.06 M | 0.80% | 1024 msgs | 8.21 | 9.46 | 12.79 | n/a | n/a | n/a | 3.80 |
| `itch_decode_encode` | `ad2bab57a412` | 10 | 69.86 M | 0.67% | 1024 msgs | 14.42 | 17.08 | 21.92 | n/a | n/a | n/a | 3.65 |

Percentiles are the time for one whole batch, in microseconds, with the timer resolving 41.67 ns.

**macOS, real-data column decodes of the slice.** Apple M4 Max, Clang 23.1.4, `-O3 -DNDEBUG -std=c++23`. Unpinned. No hardware counters. macOS has no thread pinning, and no unprivileged hardware counters.

| Case | Commit | Kernels | Values | Bits/value | ns/value | MB/s of 8-byte values | Spread (CV) | Cycles/value | Instr/value | Branch misses/value | Load at start |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `gorilla_xor/add_price` | `bcbf8738d892` | neon kernels | 3,153,991 | 37.18 | 8.393 | 953 | 2.33% | n/a | n/a | n/a | 3.92 |
| `gorilla_xor_l1/add_price` | `bcbf8738d892` | neon kernels | 3,072 | 33.34 | 4.649 | 1,721 | 2.80% | n/a | n/a | n/a | 3.68 |
| `store_decode/add_price` | `bcbf8738d892` | neon kernels | 3,153,991 | 6.30 | 2.177 | 3,674 | 0.86% | n/a | n/a | n/a | 3.85 |
| `gorilla_dod/timestamp` | `bcbf8738d892` | neon kernels | 6,125,294 | 16.95 | 9.650 | 829 | 1.66% | n/a | n/a | n/a | 3.85 |
| `pfor_delta/timestamp` | `bcbf8738d892` | neon kernels | 6,125,294 | 16.82 | 0.601 | 13,314 | 1.46% | n/a | n/a | n/a | 3.41 |
| `pfor_dod/timestamp` | `bcbf8738d892` | neon kernels | 6,125,294 | 18.07 | 0.880 | 9,096 | 1.32% | n/a | n/a | n/a | 3.22 |

**macOS, decode kernels on seeded L1-resident input.** Apple M4 Max, Clang 23.1.4, `-O3 -DNDEBUG -std=c++23`. Unpinned. No hardware counters. macOS has no thread pinning, and no unprivileged hardware counters.

Each row is one kernel on one set. A kernel with a width ran at each width listed, one case each, and the row gives the median across them.

| Kernel | Set | Commit | Cases | G values/s, median | Range | Largest CV | Cycles/value, median | Instr/value, median | Branch misses/value, median | Load at start |
|---|---|---|---|---|---|---|---|---|---|---|
| `pfor_unpack` | scalar | `bcbf8738d892` | 8 | 7.49 | 4.95 to 9.03 | 11.21% | n/a | n/a | n/a | 3.17 to 3.36 |
| `linear_unpack` | scalar | `bcbf8738d892` | 8 | 1.40 | 1.32 to 1.41 | 2.67% | n/a | n/a | n/a | 2.76 to 3.08 |
| `pfor_delta_two_pass` | scalar | `bcbf8738d892` | 65 | 2.02 | 1.77 to 2.44 | 3.73% | n/a | n/a | n/a | 1.28 to 3.84 |
| `pfor_delta_fused` | scalar | `bcbf8738d892` | 65 | 1.65 | 1.48 to 2.00 | 4.54% | n/a | n/a | n/a | 1.26 to 3.84 |
| `pfor_delta_narrow` | scalar | `bcbf8738d892` | 5 | 1.73 | 1.69 to 1.76 | 2.14% | n/a | n/a | n/a | 1.26 to 1.32 |
| `prefix_sum/vector` | scalar | `bcbf8738d892` | 1 | 3.25 | 3.25 to 3.25 | 1.32% | n/a | n/a | n/a | 1.29 to 1.29 |
| `prefix_sum/tree` | scalar | `bcbf8738d892` | 1 | 3.22 | 3.22 to 3.22 | 1.41% | n/a | n/a | n/a | 1.29 to 1.29 |
| `for_add` | scalar | `bcbf8738d892` | 1 | 5.27 | 5.27 to 5.27 | 2.33% | n/a | n/a | n/a | 1.29 to 1.29 |
| `for_add_zigzag` | scalar | `bcbf8738d892` | 1 | 4.00 | 4.00 to 4.00 | 2.49% | n/a | n/a | n/a | 1.35 to 1.35 |
| `svb_decode` | scalar | `bcbf8738d892` | 1 | 0.74 | 0.74 to 0.74 | 1.15% | n/a | n/a | n/a | 1.35 to 1.35 |
| `dod_reconstruct` | scalar | `bcbf8738d892` | 1 | 2.94 | 2.94 to 2.94 | 1.37% | n/a | n/a | n/a | 1.35 to 1.35 |
| `restore_copy` | scalar | `bcbf8738d892` | 1 | 13.36 | 13.36 to 13.36 | 18.64% | n/a | n/a | n/a | 1.32 to 1.32 |
| `pfor_unpack` | neon | `bcbf8738d892` | 8 | 7.78 | 6.19 to 8.88 | 4.04% | n/a | n/a | n/a | 1.29 to 1.35 |
| `linear_unpack` | neon | `bcbf8738d892` | 8 | 7.39 | 1.41 to 8.04 | 8.04% | n/a | n/a | n/a | 1.32 to 1.46 |
| `pfor_delta_two_pass` | neon | `bcbf8738d892` | 65 | 3.35 | 2.92 to 4.42 | 5.58% | n/a | n/a | n/a | 1.35 to 2.40 |
| `pfor_delta_fused` | neon | `bcbf8738d892` | 65 | 3.27 | 3.06 to 4.27 | 6.36% | n/a | n/a | n/a | 1.35 to 2.40 |
| `pfor_delta_narrow` | neon | `bcbf8738d892` | 5 | 3.63 | 3.62 to 3.64 | 1.90% | n/a | n/a | n/a | 1.39 to 1.44 |
| `prefix_sum/vector` | neon | `bcbf8738d892` | 1 | 4.72 | 4.72 to 4.72 | 2.79% | n/a | n/a | n/a | 1.44 to 1.44 |
| `prefix_sum/tree` | neon | `bcbf8738d892` | 1 | 3.08 | 3.08 to 3.08 | 1.56% | n/a | n/a | n/a | 1.48 to 1.48 |
| `for_add` | neon | `bcbf8738d892` | 1 | 5.43 | 5.43 to 5.43 | 2.06% | n/a | n/a | n/a | 1.48 to 1.48 |
| `for_add_zigzag` | neon | `bcbf8738d892` | 1 | 3.94 | 3.94 to 3.94 | 1.15% | n/a | n/a | n/a | 1.48 to 1.48 |
| `svb_decode` | neon | `bcbf8738d892` | 1 | 6.21 | 6.21 to 6.21 | 4.40% | n/a | n/a | n/a | 1.52 to 1.52 |
<!-- results:end -->

## Comparison

`compare/run.py` runs tick-store, Parquet, ArcticDB and QuestDB on the same slice, and on
Linux on a whole day, and writes one file per host and input to `results/compare/`, which
`scripts/check_compare.py` must pass.
`compare/README.md` describes the method: the durability classes, how each setting is read
back, the read-back oracle, the queries and how warm and cold are proven. The tables below
are generated from those files by `scripts/render_results.py`. Each host has its own tables,
and class A and class B ingest never share one. Queries run on each system's class B store.
tick-store's write time holds the parsing of the ITCH input, which the others' write times do
not, since they are handed columns already parsed. On macOS, QuestDB 10.1.0's vector kernels run
at its `Vanilla` level, scalar-dispatch C++ and not the AVX2 path it takes on x86-64
(`research/evidence/system-feasibility.md`). QuestDB's class B is not like-for-like on either
host here: under `sync` it flushes its WAL at commit, and table pages stayed dirty once the rows
were readable, and on macOS its plain `fsync` does not reach the device's cache; on the full day
its class B durability proof also failed, with one data page dirty when a commit returned. The
Linux full-day block comes first and is the one the targets are judged on: the verdicts are in
"Full-day comparison" in `NOTES.md`. The slice blocks after it prove the harness and are not
claims about a full day.

<!-- compare:begin, generated by scripts/render_results.py, do not edit -->
**Linux, full day 12302019.** AMD Ryzen Embedded V1605B with Radeon Vega Gfx, commit `67ae574f5666`. Each system ran under taskset -c 4, and the run then widened its own mask to cpu4 and cpu6. Every thread of every system shares those two CPUs, client and server alike. QuestDB's JVM has more threads than CPUs. A full day: every record of the pinned day in every system, every query as BENCHMARKS.md defines it with nothing clipped. These are the numbers the targets are judged on.

Queries, on each system's class B store, in milliseconds. "Lowest" names the measures, warm or cold, on which a system had both the lowest median and the lowest p99 of the four.

| Query | System | Variant | Rows | Warm median ms | Warm p99 ms | Cold median ms | Cold p99 ms | Lowest |
|---|---|---|---|---|---|---|---|---|
| Q1 | Parquet | `zstd1-rg1Mi` | 527,644 | 66.45 | 97.79 | 118.82 | 141.25 | no |
| Q1 | ArcticDB | `rows100k-cols127` | 527,644 | 37.41 | 67.98 | 83.18 | 89.98 | warm, cold |
| Q1 | QuestDB | `plain` | 527,644 | 58.57 (swap-in) | 95.96 (swap-in) | 550.62 | 588.18 | no |
| Q1 | tick-store | `v3-seg4Mi` | 527,644 | 297.58 | 325.72 | 406.96 | 461.20 | no |
| Q2 | Parquet | `zstd1-rg1Mi` | 739,944 | 2,702.10 (swap-in) | 2,808.72 (swap-in) | 3,957.57 (swap-in) | 4,081.60 (swap-in) | no |
| Q2 | ArcticDB | `rows100k-cols127` | 739,944 | 2,050.34 (swap-in) | 2,475.90 (swap-in) | 5,190.59 | 5,211.48 | no |
| Q2 | QuestDB | `plain` | 739,944 | 319.30 (swap-in) | 364.42 (swap-in) | 17,592.34 (swap-in) | 17,607.18 (swap-in) | no |
| Q2 | tick-store | `v3-seg4Mi` | 739,944 | 74.32 | 83.95 | 485.13 | 500.41 | warm, cold |
| Q3 | Parquet | `zstd1-rg1Mi` | 60 | 1,638.97 (swap-in) | 1,707.02 (swap-in) | 2,112.55 | 2,513.76 | no |
| Q3 | ArcticDB | `rows100k-cols127` | 60 | 1,175.26 (swap-in) | 1,224.26 (swap-in) | 4,389.00 | 4,475.13 | no |
| Q3 | QuestDB | `plain` | 60 | 68.71 (swap-in) | 85.62 (swap-in) | 7,613.49 (swap-in) | 8,834.35 (swap-in) | no |
| Q3 | tick-store | `v3-seg4Mi` | 60 | 35.81 | 36.53 | 214.40 | 226.00 | warm, cold |
| Q4 | Parquet | `zstd1-rg1Mi` | 338 | 60.75 | 99.96 | 112.02 | 248.31 | no |
| Q4 | ArcticDB | `rows100k-cols127` | 338 | 6.95 | 8.84 | 23.46 | 25.08 | cold |
| Q4 | QuestDB | `plain` | 338 | 1.18 | 1.75 | 73.26 | 97.23 | warm |
| Q4 | tick-store | `v3-seg4Mi` | 338 | 207.10 | 217.01 | 302.45 | 341.35 | no |
| Q5 | Parquet | `zstd1-rg1Mi` | 1 | 1,011.17 (swap-in) | 1,333.77 (swap-in) | 1,718.55 | 1,725.01 | no |
| Q5 | ArcticDB | `rows100k-cols127` | 1 | 3,004.35 (swap-in) | 3,081.97 (swap-in) | 12,209.75 (swap-in) | 12,272.84 (swap-in) | no |
| Q5 | QuestDB | `plain` | 1 | 61.07 | 73.75 | 4,110.39 (swap-in) | 4,135.53 (swap-in) | warm |
| Q5 | tick-store | `v3-seg4Mi` | 1 | 907.64 (swap-in) | 936.87 (swap-in) | 1,233.89 | 1,283.58 | cold |

Ingest and size, class A: returns once data is in the OS page cache. Stored bytes are allocated at rest, and the ratio is the day's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 158.442 | 1.70 | 3,191,406,592 | 2.586 | proven | yes |
| ArcticDB | `rows100k-cols127` | 120.112 | 2.24 | 6,352,621,568 | 1.299 | proven | yes |
| QuestDB | `plain` | 273.597 | 0.98 | 22,897,786,880 | 0.360 | proven | yes |
| tick-store (parses ITCH) | `v3-seg4Mi` | 195.262 | 1.38 | 1,758,240,768 | 4.693 | proven | yes |

Ingest and size, class B: returns after a flush to the device. Stored bytes are allocated at rest, and the ratio is the day's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 266.337 | 1.01 | 3,191,406,592 | 2.586 | proven | yes |
| ArcticDB | `rows100k-cols127` | 123.559 | 2.18 | 6,352,642,048 | 1.299 | proven | yes |
| QuestDB | `plain` | 1269.246 | 0.21 | 22,897,786,880 | 0.360 | NOT PROVEN | no |
| tick-store (parses ITCH) | `v3-seg4Mi` | 188.904 | 1.42 | 1,758,240,768 | 4.693 | proven | yes |

tick-store ingests the day's ITCH file itself, so its write time holds ITCH parsing. The other systems ingest the columns tools/tick_export wrote from the same day, already parsed, and their times do not. Nothing is subtracted.

Every variant run on the full day, by the rule in compare/README.md. Queries are medians on the class B store.

| System | Variant | Stored bytes, B | Ratio | Write s, A / B | Q1 ms, warm / cold | Q2 ms, warm / cold | Q3 ms, warm / cold | Q4 ms, warm / cold | Q5 ms, warm / cold |
|---|---|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` (headline) | 3,191,406,592 | 2.586 | 158.4 / 266.3 | 66.45 / 118.82 | 2,702.10 / 3,957.57 | 1,638.97 / 2,112.55 | 60.75 / 112.02 | 1,011.17 / 1,718.55 |
| Parquet | `zstd1-rg128k` | 3,212,275,712 | 2.569 | 174.7 / 200.4 | 122.37 / 186.42 | 2,834.14 / 5,974.43 | 1,850.52 / 3,847.62 | 101.70 / 161.28 | 1,033.71 / 2,415.21 |
| Parquet | `none-rg1Mi` | 6,569,455,616 | 1.256 | 170.6 / 153.8 | 59.09 / 122.81 | 2,225.39 / 3,433.41 | 1,318.83 / 2,018.66 | 53.18 / 114.03 | 782.03 / 2,246.75 |
| ArcticDB | `rows100k-cols127` (headline) | 6,352,642,048 | 1.299 | 120.1 / 123.6 | 37.41 / 83.18 | 2,050.34 / 5,190.59 | 1,175.26 / 4,389.00 | 6.95 / 23.46 | 3,004.35 / 12,209.75 |
| ArcticDB | `rows100k-cols1` | 25,346,945,024 | 0.326 | 305.9 / 815.9 | 69.27 / 211.36 | 3,804.07 / 16,688.59 | 2,731.90 / 7,893.66 | 15.89 / 57.67 | 2,836.61 / 8,761.21 |
| ArcticDB | `rows100k-cols127-per-security` | 5,666,115,584 | 1.456 | 543.9 / 507.3 | 9,791.10 / 23,519.03 | 49.65 / 111.23 | 26.19 / 91.93 | 9,721.33 / 23,437.79 | 5,675.31 / 22,651.92 |
| QuestDB | `plain` (headline) | 22,897,786,880 | 0.360 | 273.6 / 1269.2 | 58.57 / 550.62 | 319.30 / 17,592.34 | 68.71 / 7,613.49 | 1.18 / 73.26 | 61.07 / 4,110.39 |
| QuestDB | `locate-index` | 25,074,798,592 | 0.329 | 616.8 / 1434.3 | 147.19 / 566.94 | 365.56 / 22,771.81 | 88.30 / 12,134.42 | 1.45 / 105.86 | 56.58 / 4,137.23 |
| tick-store | `v3-seg4Mi` (headline) | 1,758,240,768 | 4.693 | 195.3 / 188.9 | 297.58 / 406.96 | 74.32 / 485.13 | 35.81 / 214.40 | 207.10 / 302.45 | 907.64 / 1,233.89 |
| tick-store | `v3-seg1Mi` | 1,808,261,120 | 4.563 | 182.6 / 197.4 | 115.88 / 160.05 | 97.58 / 1,117.32 | 40.17 / 552.37 | 51.40 / 88.76 | 896.61 / 1,774.63 |

Peak memory per system, from compare/guard.py across the system's whole run.

| System | Largest VmHWM | Peak RSS of its processes together | MemAvailable, lowest | Swap in / out over the run, MiB |
|---|---|---|---|---|
| Parquet | 13.59 GiB (`tick_roundtrip`) | 13.64 GiB | 11.64 GiB | 6.7 / 4.5 |
| ArcticDB | 13.37 GiB (`tick_roundtrip`) | 13.46 GiB | 10.59 GiB | 414.5 / 642.1 |
| QuestDB | 11.66 GiB (`java`) | 13.81 GiB | 7.13 GiB | 1,765.7 / 3,030.1 |
| tick-store | 13.51 GiB (`tick_roundtrip`) | 13.54 GiB | 12.96 GiB | 12.8 / 0.2 |

Swap per phase, host-wide, over every variant and class each system ran. Swap-out alone does not stop a run; a timed query with any page swapped in is kept and marked (swap-in). See "The guard's swap rule" in NOTES.md.

| System | Write, in / out MiB | Read-back, in / out MiB | Warm queries, in / out MiB | Cold queries, in / out MiB | Timed queries with swap-in |
|---|---|---|---|---|---|
| Parquet | 1.8 / 3.3 | 3.8 / 1.2 | 0.7 / 0.0 | 0.2 / 0.0 | `zstd1-rg1Mi` Q2 warm, `zstd1-rg1Mi` Q2 cold, `zstd1-rg1Mi` Q3 warm, `zstd1-rg1Mi` Q5 warm, `zstd1-rg128k` Q2 warm, `zstd1-rg128k` Q2 cold, `none-rg1Mi` Q2 warm, `none-rg1Mi` Q3 warm, `none-rg1Mi` Q5 warm, `none-rg1Mi` Q5 cold |
| ArcticDB | 35.3 / 181.4 | 317.7 / 443.8 | 17.1 / 16.9 | 7.7 / 0.0 | `rows100k-cols127` Q2 warm, `rows100k-cols127` Q3 warm, `rows100k-cols127` Q5 warm, `rows100k-cols127` Q5 cold, `rows100k-cols1` Q1 warm, `rows100k-cols1` Q2 warm, `rows100k-cols1` Q2 cold, `rows100k-cols1` Q3 warm, `rows100k-cols1` Q3 cold, `rows100k-cols1` Q5 warm, `rows100k-cols1` Q5 cold, `rows100k-cols127-per-security` Q1 warm, `rows100k-cols127-per-security` Q1 cold, `rows100k-cols127-per-security` Q2 warm, `rows100k-cols127-per-security` Q4 warm, `rows100k-cols127-per-security` Q4 cold, `rows100k-cols127-per-security` Q5 warm, `rows100k-cols127-per-security` Q5 cold |
| QuestDB | 124.7 / 255.4 | 1,563.9 / 2,774.7 | 11.7 / 0.0 | 60.8 / 0.0 | `plain` Q1 warm, `plain` Q2 warm, `plain` Q2 cold, `plain` Q3 warm, `plain` Q3 cold, `plain` Q5 cold, `locate-index` Q1 warm, `locate-index` Q1 cold, `locate-index` Q2 warm, `locate-index` Q2 cold, `locate-index` Q3 warm, `locate-index` Q3 cold, `locate-index` Q4 cold, `locate-index` Q5 warm, `locate-index` Q5 cold |
| tick-store | 4.2 / 0.0 | 8.4 / 0.2 | 0.0 / 0.0 | 0.1 / 0.0 | `v3-seg4Mi` Q5 warm, `v3-seg1Mi` Q5 warm |

**Linux, slice only.** AMD Ryzen Embedded V1605B with Radeon Vega Gfx, commit `67ae574f5666`. Each system ran under taskset -c 4, and the run then widened its own mask to cpu4 and cpu6. Every thread of every system shares those two CPUs, client and server alike. QuestDB's JVM has more threads than CPUs. These numbers come from a five-minute slice and prove that the harness works. They are not claims about a full day, and no target changes because of them.

Queries, on each system's class B store, in milliseconds. "Lowest" names the measures, warm or cold, on which a system had both the lowest median and the lowest p99 of the four.

| Query | System | Variant | Rows | Warm median ms | Warm p99 ms | Cold median ms | Cold p99 ms | Lowest |
|---|---|---|---|---|---|---|---|---|
| Q1 | Parquet | `zstd1-rg1Mi` | 527,644 | 55.79 | 111.74 | 136.65 | 144.26 | no |
| Q1 | ArcticDB | `rows100k-cols127` | 527,644 | 32.57 (swap-in) | 63.79 (swap-in) | 74.17 | 75.05 | warm, cold |
| Q1 | QuestDB | `plain` | 527,644 | 59.77 (swap-in) | 286.29 (swap-in) | 460.83 | 621.91 | no |
| Q1 | tick-store | `v3-seg4Mi` | 527,644 | 278.73 | 336.86 | 373.40 | 381.05 | no |
| Q2 | Parquet | `zstd1-rg1Mi` | 66,741 | 247.26 | 285.37 | 496.92 | 509.69 | no |
| Q2 | ArcticDB | `rows100k-cols127` | 66,741 | 138.73 | 174.62 | 455.71 | 462.96 | no |
| Q2 | QuestDB | `plain` | 66,741 | 27.76 | 105.66 | 1,627.08 | 1,689.42 | no |
| Q2 | tick-store | `v3-seg4Mi` | 66,741 | 7.29 | 11.08 | 82.59 | 92.25 | warm, cold |
| Q3 | Parquet | `zstd1-rg1Mi` | 5 | 149.16 | 173.30 | 309.63 | 331.46 | no |
| Q3 | ArcticDB | `rows100k-cols127` | 5 | 96.74 | 141.78 | 375.38 | 379.53 | no |
| Q3 | QuestDB | `plain` | 5 | 18.23 | 99.26 | 823.03 | 866.16 | no |
| Q3 | tick-store | `v3-seg4Mi` | 5 | 3.71 | 3.98 | 43.29 | 46.65 | warm, cold |
| Q4 | Parquet | `zstd1-rg1Mi` | 338 | 47.28 | 56.41 | 128.48 | 133.24 | no |
| Q4 | ArcticDB | `rows100k-cols127` | 338 | 6.56 | 7.48 | 20.60 | 20.99 | cold |
| Q4 | QuestDB | `plain` | 338 | 1.61 | 5.76 | 85.05 | 106.90 | warm |
| Q4 | tick-store | `v3-seg4Mi` | 338 | 197.28 | 203.18 | 293.62 (swap-in) | 295.99 (swap-in) | no |
| Q5 | Parquet | `zstd1-rg1Mi` | 1 | 24.24 | 35.47 | 43.40 | 43.82 | no |
| Q5 | ArcticDB | `rows100k-cols127` | 1 | 48.37 | 76.77 | 261.56 | 264.68 | no |
| Q5 | QuestDB | `plain` | 1 | 3.28 | 14.00 | 120.79 | 171.36 | warm |
| Q5 | tick-store | `v3-seg4Mi` | 1 | 25.33 | 25.71 | 42.80 | 62.22 | no |

Ingest and size, class A: returns once data is in the OS page cache. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.870 | 3.28 | 70,483,968 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 1.026 | 5.97 | 141,168,640 | 1.460 | proven | yes |
| QuestDB | `plain` | 4.172 | 1.47 | 537,550,848 | 0.383 | proven | yes |
| tick-store (parses ITCH) | `v3-seg4Mi` | 4.890 | 1.25 | 43,270,144 | 4.764 | proven | yes |

Ingest and size, class B: returns after a flush to the device. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 4.885 | 1.25 | 70,479,872 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 1.232 | 4.97 | 141,168,640 | 1.460 | proven | yes |
| QuestDB | `plain` | 23.747 | 0.26 | 537,550,848 | 0.383 | proven | no |
| tick-store (parses ITCH) | `v3-seg4Mi` | 5.001 | 1.22 | 43,266,048 | 4.764 | proven | yes |

tick-store ingests the ITCH slice itself, so its write time holds ITCH parsing. The other systems ingest the columns tools/tick_export wrote from the same slice, already parsed, and their times do not. Nothing is subtracted.

**macOS, slice only.** Apple M4 Max, commit `67ae574f5666`. Unpinned. No hardware counters. macOS has no thread pinning, and no unprivileged hardware counters. Each system gets the same thread budget. These numbers come from a five-minute slice and prove that the harness works. They are not claims about a full day, and no target changes because of them.

Queries, on each system's class B store, in milliseconds. "Lowest" names the measures, warm or cold, on which a system had both the lowest median and the lowest p99 of the four.

| Query | System | Variant | Rows | Warm median ms | Warm p99 ms | Cold median ms | Cold p99 ms | Lowest |
|---|---|---|---|---|---|---|---|---|
| Q1 | Parquet | `zstd1-rg1Mi` | 527,644 | 16.62 | 19.13 | 21.45 | 22.37 | no |
| Q1 | ArcticDB | `rows100k-cols127` | 527,644 | 7.02 | 8.72 | 17.84 | 18.54 | warm, cold |
| Q1 | QuestDB | `plain` | 527,644 | 14.56 | 57.21 | 92.15 | 93.39 | no |
| Q1 | tick-store | `v3-seg4Mi` | 527,644 | 88.68 | 107.62 | 154.19 | 156.31 | no |
| Q2 | Parquet | `zstd1-rg1Mi` | 66,741 | 79.10 | 88.94 | 85.32 | 87.62 | no |
| Q2 | ArcticDB | `rows100k-cols127` | 66,741 | 38.45 | 45.87 | 113.06 | 113.83 | no |
| Q2 | QuestDB | `plain` | 66,741 | 7.83 | 23.89 | 314.25 | 319.06 | no |
| Q2 | tick-store | `v3-seg4Mi` | 66,741 | 2.34 | 2.54 | 18.96 | 19.63 | warm, cold |
| Q3 | Parquet | `zstd1-rg1Mi` | 5 | 54.16 | 57.68 | 58.03 | 59.03 | no |
| Q3 | ArcticDB | `rows100k-cols127` | 5 | 25.51 | 32.81 | 84.90 | 86.22 | no |
| Q3 | QuestDB | `plain` | 5 | 2.02 | 24.88 | 119.99 | 124.86 | no |
| Q3 | tick-store | `v3-seg4Mi` | 5 | 1.23 | 1.51 | 11.74 | 12.36 | warm, cold |
| Q4 | Parquet | `zstd1-rg1Mi` | 338 | 15.94 | 18.12 | 20.16 | 21.49 | no |
| Q4 | ArcticDB | `rows100k-cols127` | 338 | 1.69 | 3.00 | 6.56 | 6.82 | cold |
| Q4 | QuestDB | `plain` | 338 | 0.31 | 1.19 | 10.91 | 11.50 | warm |
| Q4 | tick-store | `v3-seg4Mi` | 338 | 67.44 | 68.73 | 126.79 | 129.45 | no |
| Q5 | Parquet | `zstd1-rg1Mi` | 1 | 7.10 | 8.09 | 10.18 | 10.37 | cold |
| Q5 | ArcticDB | `rows100k-cols127` | 1 | 11.15 | 13.58 | 58.71 | 59.57 | no |
| Q5 | QuestDB | `plain` | 1 | 0.99 | 6.45 | 23.65 | 24.19 | warm |
| Q5 | tick-store | `v3-seg4Mi` | 1 | 7.13 | 7.63 | 12.27 | 12.89 | no |

Ingest and size, class A: returns once data is in the OS page cache. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.016 | 6.03 | 87,244,800 | 2.363 | proven | yes |
| ArcticDB | `rows100k-cols127` | 0.458 | 13.38 | 141,860,864 | 1.453 | proven | yes |
| QuestDB | `plain` | 0.860 | 7.12 | 537,772,032 | 0.383 | proven | yes |
| tick-store (parses ITCH) | `v3-seg4Mi` | 1.995 | 3.07 | 43,638,784 | 4.724 | proven | yes |

Ingest and size, class B: returns after a flush to the device. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.019 | 6.01 | 87,220,224 | 2.363 | proven | yes |
| ArcticDB | `rows100k-cols127` | 0.516 | 11.87 | 141,860,864 | 1.453 | proven | yes |
| QuestDB | `plain` | 0.899 | 6.81 | 537,772,032 | 0.383 | proven | no |
| tick-store (parses ITCH) | `v3-seg4Mi` | 2.005 | 3.06 | 43,638,784 | 4.724 | proven | yes |

tick-store ingests the ITCH slice itself, so its write time holds ITCH parsing. The other systems ingest the columns tools/tick_export wrote from the same slice, already parsed, and their times do not. Nothing is subtracted.
<!-- compare:end -->
