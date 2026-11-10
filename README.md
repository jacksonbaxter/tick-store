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

`compare/run.py` runs tick-store, Parquet, ArcticDB and QuestDB on the same slice and
writes one file per host to `results/compare/`, which `scripts/check_compare.py` must pass.
`compare/README.md` describes the method: the durability classes, how each setting is read
back, the read-back oracle, the queries and how warm and cold are proven. The tables below
are generated from those files by `scripts/render_results.py`. Each host has its own tables,
and class A and class B ingest never share one. Queries run on each system's class B store.
tick-store's write time holds the parsing of the ITCH input, which the others' write times do
not, since they are handed columns already parsed. On macOS, QuestDB 10.1.0's vector kernels run
at its `Vanilla` level, scalar-dispatch C++ and not the AVX2 path it takes on x86-64
(`research/evidence/system-feasibility.md`). QuestDB's class B is not like-for-like on either
host here: under `sync` it flushes its WAL at commit, and table pages stayed dirty once the rows
were readable, and on macOS its plain `fsync` does not reach the device's cache. These are slice
numbers, not claims about a full day.

<!-- compare:begin, generated by scripts/render_results.py, do not edit -->
**Linux, slice only.** AMD Ryzen Embedded V1605B with Radeon Vega Gfx, commit `e0b7df266a73`. Each system ran under taskset -c 4, and the run then widened its own mask to cpu4 and cpu6. Every thread of every system shares those two CPUs, client and server alike. QuestDB's JVM has more threads than CPUs. These numbers come from a five-minute slice and prove that the harness works. They are not claims about a full day, and no target changes because of them.

Queries, on each system's class B store, in milliseconds. "Lowest" names the measures, warm or cold, on which a system had both the lowest median and the lowest p99 of the four.

| Query | System | Variant | Rows | Warm median ms | Warm p99 ms | Cold median ms | Cold p99 ms | Lowest |
|---|---|---|---|---|---|---|---|---|
| Q1 | Parquet | `zstd1-rg1Mi` | 527,644 | 55.39 | 85.82 | 87.07 | 89.82 | no |
| Q1 | ArcticDB | `rows100k-cols127` | 527,644 | 27.45 | 59.75 | 75.35 | 79.33 | warm, cold |
| Q1 | QuestDB | `plain` | 527,644 | 57.73 | 281.22 | 550.50 | 604.73 | no |
| Q1 | tick-store | `v3-seg4Mi` | 527,644 | 276.85 | 335.68 | 365.74 | 368.72 | no |
| Q2 | Parquet | `zstd1-rg1Mi` | 66,741 | 243.53 | 291.75 | 356.14 | 364.33 | no |
| Q2 | ArcticDB | `rows100k-cols127` | 66,741 | 137.58 | 160.99 | 457.57 | 471.07 | no |
| Q2 | QuestDB | `plain` | 66,741 | 28.88 | 91.46 | 1,689.18 | 1,788.87 | no |
| Q2 | tick-store | `v3-seg4Mi` | 66,741 | 7.35 | 11.35 | 80.63 | 82.34 | warm, cold |
| Q3 | Parquet | `zstd1-rg1Mi` | 5 | 150.61 | 180.48 | 223.44 | 244.54 | no |
| Q3 | ArcticDB | `rows100k-cols127` | 5 | 97.80 | 117.13 | 371.86 | 377.63 | no |
| Q3 | QuestDB | `plain` | 5 | 17.06 | 122.19 | 835.72 | 853.70 | no |
| Q3 | tick-store | `v3-seg4Mi` | 5 | 3.70 | 3.85 | 42.49 | 44.39 | warm, cold |
| Q4 | Parquet | `zstd1-rg1Mi` | 338 | 47.62 | 61.65 | 79.70 | 83.60 | no |
| Q4 | ArcticDB | `rows100k-cols127` | 338 | 6.22 | 6.69 | 20.64 | 21.11 | cold |
| Q4 | QuestDB | `plain` | 338 | 1.55 | 4.32 | 48.55 | 71.08 | warm |
| Q4 | tick-store | `v3-seg4Mi` | 338 | 196.66 | 199.54 | 278.54 | 279.83 | no |
| Q5 | Parquet | `zstd1-rg1Mi` | 1 | 24.93 | 40.09 | 39.22 | 44.84 | no |
| Q5 | ArcticDB | `rows100k-cols127` | 1 | 62.84 | 91.63 | 264.81 | 269.66 | no |
| Q5 | QuestDB | `plain` | 1 | 3.06 | 23.48 | 161.80 | 214.72 | warm |
| Q5 | tick-store | `v3-seg4Mi` | 1 | 24.86 | 32.03 | 38.73 | 42.35 | cold |

Ingest and size, class A: returns once data is in the OS page cache. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.865 | 3.28 | 70,479,872 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 1.040 | 5.89 | 141,164,544 | 1.460 | proven | yes |
| QuestDB | `plain` | 4.263 | 1.44 | 537,550,848 | 0.383 | proven | yes |
| tick-store (parses ITCH) | `v3-seg4Mi` | 4.907 | 1.25 | 43,266,048 | 4.764 | proven | yes |

Ingest and size, class B: returns after a flush to the device. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 2.015 | 3.04 | 70,479,872 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 1.212 | 5.05 | 141,168,640 | 1.460 | proven | yes |
| QuestDB | `plain` | 22.980 | 0.27 | 537,550,848 | 0.383 | proven | no |
| tick-store (parses ITCH) | `v3-seg4Mi` | 4.985 | 1.23 | 43,266,048 | 4.764 | proven | yes |

tick-store ingests the ITCH slice itself, so its write time holds ITCH parsing. The other systems ingest the columns tools/tick_export wrote from the same slice, already parsed, and their times do not. Nothing is subtracted.

**macOS, slice only.** Apple M4 Max, commit `e0b7df266a73`. Unpinned. No hardware counters. macOS has no thread pinning, and no unprivileged hardware counters. Each system gets the same thread budget. These numbers come from a five-minute slice and prove that the harness works. They are not claims about a full day, and no target changes because of them.

Queries, on each system's class B store, in milliseconds. "Lowest" names the measures, warm or cold, on which a system had both the lowest median and the lowest p99 of the four.

| Query | System | Variant | Rows | Warm median ms | Warm p99 ms | Cold median ms | Cold p99 ms | Lowest |
|---|---|---|---|---|---|---|---|---|
| Q1 | Parquet | `zstd1-rg1Mi` | 527,644 | 16.46 | 19.40 | 21.35 | 21.75 | no |
| Q1 | ArcticDB | `rows100k-cols127` | 527,644 | 7.46 | 9.03 | 17.60 | 18.72 | warm, cold |
| Q1 | QuestDB | `plain` | 527,644 | 14.56 | 55.27 | 91.03 | 91.38 | no |
| Q1 | tick-store | `v3-seg4Mi` | 527,644 | 92.49 | 107.68 | 152.57 | 153.60 | no |
| Q2 | Parquet | `zstd1-rg1Mi` | 66,741 | 77.84 | 84.70 | 86.91 | 88.12 | no |
| Q2 | ArcticDB | `rows100k-cols127` | 66,741 | 40.06 | 44.98 | 111.14 | 112.23 | no |
| Q2 | QuestDB | `plain` | 66,741 | 7.93 | 25.96 | 312.41 | 318.78 | no |
| Q2 | tick-store | `v3-seg4Mi` | 66,741 | 2.41 | 2.63 | 18.01 | 18.20 | warm, cold |
| Q3 | Parquet | `zstd1-rg1Mi` | 5 | 53.64 | 58.73 | 58.86 | 60.28 | no |
| Q3 | ArcticDB | `rows100k-cols127` | 5 | 25.85 | 31.95 | 84.63 | 86.09 | no |
| Q3 | QuestDB | `plain` | 5 | 2.04 | 25.33 | 116.40 | 119.35 | no |
| Q3 | tick-store | `v3-seg4Mi` | 5 | 1.25 | 1.37 | 11.03 | 11.81 | warm, cold |
| Q4 | Parquet | `zstd1-rg1Mi` | 338 | 15.58 | 17.71 | 20.29 | 20.54 | no |
| Q4 | ArcticDB | `rows100k-cols127` | 338 | 1.72 | 2.36 | 6.39 | 6.54 | cold |
| Q4 | QuestDB | `plain` | 338 | 0.30 | 1.16 | 10.20 | 11.35 | warm |
| Q4 | tick-store | `v3-seg4Mi` | 338 | 69.57 | 71.22 | 127.36 | 127.74 | no |
| Q5 | Parquet | `zstd1-rg1Mi` | 1 | 6.98 | 9.39 | 10.09 | 10.18 | cold |
| Q5 | ArcticDB | `rows100k-cols127` | 1 | 11.90 | 15.95 | 57.91 | 58.78 | no |
| Q5 | QuestDB | `plain` | 1 | 0.99 | 5.55 | 23.48 | 24.67 | warm |
| Q5 | tick-store | `v3-seg4Mi` | 1 | 7.20 | 7.70 | 12.54 | 13.16 | no |

Ingest and size, class A: returns once data is in the OS page cache. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 0.997 | 6.15 | 70,479,872 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 0.485 | 12.62 | 141,860,864 | 1.453 | proven | yes |
| QuestDB | `plain` | 0.817 | 7.50 | 537,772,032 | 0.383 | proven | yes |
| tick-store (parses ITCH) | `v3-seg4Mi` | 2.069 | 2.96 | 43,638,784 | 4.724 | proven | yes |

Ingest and size, class B: returns after a flush to the device. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.001 | 6.12 | 70,479,872 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 0.516 | 11.86 | 141,860,864 | 1.453 | proven | yes |
| QuestDB | `plain` | 0.887 | 6.91 | 537,772,032 | 0.383 | proven | no |
| tick-store (parses ITCH) | `v3-seg4Mi` | 2.075 | 2.95 | 43,638,784 | 4.724 | proven | yes |

tick-store ingests the ITCH slice itself, so its write time holds ITCH parsing. The other systems ingest the columns tools/tick_export wrote from the same slice, already parsed, and their times do not. Nothing is subtracted.
<!-- compare:end -->
