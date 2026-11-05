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
**Linux, slice only.** AMD Ryzen Embedded V1605B with Radeon Vega Gfx, commit `25a462bc73c2`. Each system ran under taskset -c 4, and the run then widened its own mask to cpu4 and cpu6. Every thread of every system shares those two CPUs, client and server alike. QuestDB's JVM has more threads than CPUs. These numbers come from a five-minute slice and prove that the harness works. They are not claims about a full day, and no target changes because of them.

Queries, on each system's class B store, in milliseconds. "Lowest" names the measures, warm or cold, on which a system had both the lowest median and the lowest p99 of the four.

| Query | System | Variant | Rows | Warm median ms | Warm p99 ms | Cold median ms | Cold p99 ms | Lowest |
|---|---|---|---|---|---|---|---|---|
| Q1 | Parquet | `zstd1-rg1Mi` | 527,644 | 54.84 | 86.09 | 89.95 | 92.10 | no |
| Q1 | ArcticDB | `rows100k-cols127` | 527,644 | 27.40 | 55.03 | 73.79 | 75.26 | warm, cold |
| Q1 | QuestDB | `plain` | 527,644 | 57.38 | 284.27 | 469.83 | 583.41 | no |
| Q1 | tick-store | `v3-seg1Mi` | 527,644 | 118.55 | 147.68 | 161.40 | 162.44 | no |
| Q2 | Parquet | `zstd1-rg1Mi` | 66,741 | 243.31 | 289.56 | 361.43 | 371.24 | no |
| Q2 | ArcticDB | `rows100k-cols127` | 66,741 | 139.66 | 171.13 | 458.93 | 461.87 | no |
| Q2 | QuestDB | `plain` | 66,741 | 27.65 | 143.45 | 1,168.44 | 1,231.37 | no |
| Q2 | tick-store | `v3-seg1Mi` | 66,741 | 10.62 | 25.84 | 108.93 | 112.21 | warm, cold |
| Q3 | Parquet | `zstd1-rg1Mi` | 5 | 149.63 | 194.07 | 227.26 | 243.49 | no |
| Q3 | ArcticDB | `rows100k-cols127` | 5 | 96.26 | 114.07 | 371.97 | 377.15 | no |
| Q3 | QuestDB | `plain` | 5 | 16.32 | 101.68 | 605.87 | 663.47 | no |
| Q3 | tick-store | `v3-seg1Mi` | 5 | 4.75 | 5.43 | 58.33 | 59.18 | warm, cold |
| Q4 | Parquet | `zstd1-rg1Mi` | 338 | 47.42 | 79.34 | 81.28 | 84.47 | no |
| Q4 | ArcticDB | `rows100k-cols127` | 338 | 6.16 | 6.79 | 20.53 | 21.38 | cold |
| Q4 | QuestDB | `plain` | 338 | 1.71 | 7.27 | 68.17 | 91.04 | no |
| Q4 | tick-store | `v3-seg1Mi` | 338 | 53.59 | 56.94 | 89.38 | 90.69 | no |
| Q5 | Parquet | `zstd1-rg1Mi` | 1 | 25.00 | 43.59 | 37.74 | 38.81 | cold |
| Q5 | ArcticDB | `rows100k-cols127` | 1 | 49.20 | 77.42 | 262.77 | 266.99 | no |
| Q5 | QuestDB | `plain` | 1 | 2.90 | 14.07 | 117.37 | 140.10 | warm |
| Q5 | tick-store | `v3-seg1Mi` | 1 | 33.19 | 42.63 | 55.61 | 60.87 | no |

Ingest and size, class A: returns once data is in the OS page cache. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.863 | 3.29 | 70,479,872 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 1.049 | 5.84 | 141,164,544 | 1.460 | proven | yes |
| QuestDB | `plain` | 4.127 | 1.48 | 537,550,848 | 0.383 | proven | yes |
| tick-store (parses ITCH) | `v3-seg1Mi` | 5.497 | 1.11 | 42,127,360 | 4.893 | proven | yes |

Ingest and size, class B: returns after a flush to the device. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 2.025 | 3.02 | 70,479,872 | 2.925 | proven | yes |
| ArcticDB | `rows100k-cols127` | 1.212 | 5.05 | 141,168,640 | 1.460 | proven | yes |
| QuestDB | `plain` | 9.021 | 0.68 | 537,550,848 | 0.383 | proven | no |
| tick-store (parses ITCH) | `v3-seg1Mi` | 5.618 | 1.09 | 42,127,360 | 4.893 | proven | yes |

tick-store ingests the ITCH slice itself, so its write time holds ITCH parsing. The other systems ingest the columns tools/tick_export wrote from the same slice, already parsed, and their times do not. Nothing is subtracted.

**macOS, slice only.** Apple M4 Max, commit `25a462bc73c2`. Unpinned. No hardware counters. macOS has no thread pinning, and no unprivileged hardware counters. Each system gets the same thread budget. These numbers come from a five-minute slice and prove that the harness works. They are not claims about a full day, and no target changes because of them.

Queries, on each system's class B store, in milliseconds. "Lowest" names the measures, warm or cold, on which a system had both the lowest median and the lowest p99 of the four.

| Query | System | Variant | Rows | Warm median ms | Warm p99 ms | Cold median ms | Cold p99 ms | Lowest |
|---|---|---|---|---|---|---|---|---|
| Q1 | Parquet | `zstd1-rg1Mi` | 527,644 | 16.94 | 21.03 | 21.31 | 21.56 | no |
| Q1 | ArcticDB | `rows100k-cols127` | 527,644 | 7.36 | 8.78 | 18.14 | 18.65 | warm, cold |
| Q1 | QuestDB | `plain` | 527,644 | 14.51 | 56.47 | 91.48 | 92.68 | no |
| Q1 | tick-store | `v3-seg1Mi` | 527,644 | 38.13 | 45.82 | 55.29 | 56.81 | no |
| Q2 | Parquet | `zstd1-rg1Mi` | 66,741 | 79.40 | 84.95 | 86.45 | 87.24 | no |
| Q2 | ArcticDB | `rows100k-cols127` | 66,741 | 39.31 | 45.30 | 114.49 | 116.03 | no |
| Q2 | QuestDB | `plain` | 66,741 | 7.80 | 24.88 | 319.49 | 321.90 | no |
| Q2 | tick-store | `v3-seg1Mi` | 66,741 | 4.11 | 5.43 | 23.45 | 23.81 | warm, cold |
| Q3 | Parquet | `zstd1-rg1Mi` | 5 | 54.75 | 59.09 | 59.54 | 61.17 | no |
| Q3 | ArcticDB | `rows100k-cols127` | 5 | 25.78 | 31.81 | 85.52 | 90.01 | no |
| Q3 | QuestDB | `plain` | 5 | 2.03 | 25.25 | 117.11 | 119.05 | no |
| Q3 | tick-store | `v3-seg1Mi` | 5 | 1.86 | 2.03 | 13.36 | 13.43 | warm, cold |
| Q4 | Parquet | `zstd1-rg1Mi` | 338 | 16.02 | 16.90 | 20.61 | 21.28 | no |
| Q4 | ArcticDB | `rows100k-cols127` | 338 | 1.70 | 1.91 | 6.51 | 7.05 | cold |
| Q4 | QuestDB | `plain` | 338 | 0.30 | 1.03 | 10.78 | 12.77 | warm |
| Q4 | tick-store | `v3-seg1Mi` | 338 | 22.86 | 23.59 | 36.48 | 38.41 | no |
| Q5 | Parquet | `zstd1-rg1Mi` | 1 | 7.23 | 8.62 | 10.54 | 10.98 | cold |
| Q5 | ArcticDB | `rows100k-cols127` | 1 | 11.66 | 13.39 | 59.05 | 60.16 | no |
| Q5 | QuestDB | `plain` | 1 | 1.00 | 5.02 | 23.14 | 24.16 | warm |
| Q5 | tick-store | `v3-seg1Mi` | 1 | 14.52 | 15.04 | 19.91 | 20.69 | no |

Ingest and size, class A: returns once data is in the OS page cache. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.046 | 5.86 | 86,265,856 | 2.390 | proven | yes |
| ArcticDB | `rows100k-cols127` | 0.478 | 12.82 | 141,860,864 | 1.453 | proven | yes |
| QuestDB | `plain` | 0.804 | 7.62 | 537,772,032 | 0.383 | proven | yes |
| tick-store (parses ITCH) | `v3-seg1Mi` | 2.386 | 2.57 | 42,741,760 | 4.823 | proven | yes |

Ingest and size, class B: returns after a flush to the device. Stored bytes are allocated at rest, and the ratio is the slice's source bytes over them.

| System | Variant | Write s, median | M records/s | Stored bytes | Ratio | Proof | Like-for-like |
|---|---|---|---|---|---|---|---|
| Parquet | `zstd1-rg1Mi` | 1.040 | 5.89 | 86,265,856 | 2.390 | proven | yes |
| ArcticDB | `rows100k-cols127` | 0.518 | 11.82 | 141,860,864 | 1.453 | proven | yes |
| QuestDB | `plain` | 0.918 | 6.67 | 537,772,032 | 0.383 | proven | no |
| tick-store (parses ITCH) | `v3-seg1Mi` | 2.394 | 2.56 | 42,741,760 | 4.823 | proven | yes |

tick-store ingests the ITCH slice itself, so its write time holds ITCH parsing. The other systems ingest the columns tools/tick_export wrote from the same slice, already parsed, and their times do not. Nothing is subtracted.
<!-- compare:end -->
