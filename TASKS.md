# tick-store tasks

The plan through to the done criteria. Phases are gated: each ends with a phase report and waits for a go-ahead.

## Phase 1: Research

- [x] Model and effort check passed
- [x] Evidence: time-series compression (`research/evidence/ts-compression.md`)
- [x] Evidence: comparison systems' published numbers, durability, tuning (`column-systems.md`)
- [x] Evidence: SIMD decoding and instruction sets (`simd-decoding.md`)
- [x] Evidence: code-quality bar from open-source code (`code-quality.md`)
- [x] Evidence: ITCH format, sample files, prior art (`itch-data.md`)
- [x] Comparison systems tried on macOS (`system-feasibility.md`)
- [x] Comparison systems assessed for `debian` from published packaging (`debian-host.md`)
- [ ] Comparison systems actually run on `debian`
- [ ] Machine probe finished: Q2b-e, measured Q3, Q4 (`machine-probe.md`, unfinished)
- [ ] Project session opens the source of every number that became a target
- [ ] BENCHMARKS.md, TASKS.md and NOTES.md finished
- [ ] Phase report

## Phase 2: Harness, before any implementation

Scaffold
- [ ] CMake project, C++23, presets for dev, ASan+UBSan, TSan, fuzz and bench. The toolchain comes from the environment, never a hardcoded path
- [ ] Dependencies through pinned FetchContent: Google Benchmark, HdrHistogram, GoogleTest. Nothing vendored in git

Data
- [ ] Data script: same file and SHA-256 as itch-orderbook, read from that repo's script once it exists. Stands alone, and accepts a path to an existing copy, verified the same way
- [ ] Verify the `.gz` SHA-256, the gzip CRC32 and ISIZE, and the decompressed SHA-256. Nasdaq publishes no usable checksum: every `.md5sum` returned 404
- [ ] Same data on `debian` under `~/quant-projects/tick-store/data`, through the repo's own script
- [ ] Choose the benchmark day only after checking stored size against `debian`'s 15 GB RAM. A 41.7 GB day cannot be fully warm there

Census of the pinned file, because no source publishes these
- [ ] Message count and bytes per type, reconciled against the gzip ISIZE
- [ ] Unknown types, frame length against spec length, tail bytes, zero-length terminator
- [ ] Tracking number: share non-zero
- [ ] Timestamps: non-decreasing or not, share repeating the previous nanosecond, delta and delta-of-delta histograms, global and per locate
- [ ] Prices: share that are multiples of 100, per message type, and range
- [ ] Order references: delta histograms by message type
- [ ] Stock field against locate: any mismatch

Correctness oracle
- [ ] Own small ITCH 5.0 decoder and re-serialiser, covering all 23 types, with a raw exception path for anything unknown
- [ ] README defines "record" as one ITCH message of the source file
- [ ] Oracle compares every re-serialised record with the source bytes and hashes the whole stream, so order, prefixes and tail are proven too
- [ ] Injected single-bit corruption test shows the check catches it and reports where
- [ ] Oracle fails correctly on a deliberately broken store, proving it is not vacuous

Tests and fuzzing
- [ ] Unit tests, including corrupt fixtures and boundary lists (T1-T4)
- [ ] One libFuzzer target per parser and decoder, including the ITCH decoder, following Z1-Z7
- [ ] Coverage report shows every kernel and validator is reached

Benchmarks
- [ ] Google Benchmark plus HdrHistogram, warm-up, seeded inputs, verify before measuring
- [ ] Results header: host label, CPU model, kernel, compiler version, flags, ISA, commit, core, governor, frequency, turbo state, pinning status, load lines from `measure.sh`
- [ ] macOS records "not pinned". The pinning and perf-counter code is built anyway
- [ ] Linux: `taskset -c 4`, `cpu5` idle, output written to a file on `debian`. TSC calibrated on every run. Batch anything below about 100 ns; publish no single-shot percentile the timer cannot resolve
- [ ] macOS: report no latency below a few ticks of the 42 ns timer
- [ ] Linux counters through `perf stat` with `:u` events. Confirm what `cache-misses` counts on Zen 1 against AMD documentation before quoting it
- [ ] Results table in the README generated from results files, never typed

Comparison harness
- [ ] One schema covering the same records for all systems, so the ratio baseline is identical. Decide how the 23 message types map to columns and how unused fields are stored
- [ ] Read-back check for each comparison system against the source records
- [ ] Parquet: streaming `ParquetWriter`, settings from `column-systems.md` section 7, row-group sweep
- [ ] ArcticDB on LMDB: settings from section 7, including rows-per-segment and columns-per-segment sweeps
- [ ] QuestDB: pre-created tables with `TIMESTAMP_NS`. Never rely on ILP auto-create, which silently lost nanoseconds. Clock stops at `wait_wal_table`
- [ ] Each result records exact version and all settings
- [ ] Durability classes A and B for every system, flush granularity stated. Verify that QuestDB `sync` really flushes, and validate the config value, because a typo silently gives `nosync`
- [ ] macOS cold-cache tool built on the probe's result, proving eviction per file
- [ ] Linux cold-cache tool: stop the process, fsync, `posix_fadvise(POSIX_FADV_DONTNEED)`, prove with `fincore` or `mincore`
- [ ] A warm result counts only if residency is proven before and after the run
- [ ] One system runs at a time. Record peak RSS, heap settings, `MemAvailable` and stored bytes. Keep the Python overhead visible rather than hidden in engine time

Checks
- [ ] ASan and UBSan with no recover, TSan on concurrent code
- [ ] clang-tidy, and `-Wall -Wextra -Werror`
- [ ] GitHub Actions config, validated locally with actionlint, and recorded as never executed on GitHub
- [ ] One local script that runs every check

## Phase 3: Implementation

Storage
- [ ] File format: header, per-block CRC32C, footer, codec registry, little-endian helpers (F1-F7)
- [ ] Writer with named durability modes and a commit ordering that survives torn writes (M1-M8)
- [ ] Memory-mapped reader with mapped length proved at open and view lifetimes tied to the mapping (A3, M5, M7, M9)
- [ ] Time-range index with block zone maps, and a symbol filter whose cost counts in the ratio

Layout experiments, measured and recorded in NOTES.md whichever way they go
- [ ] File-order blocks with per-type columns, against per-symbol columns plus a stored permutation
- [ ] Block size sweep: ratio against range-query latency
- [ ] Separate columns per message type against shared columns

Required codecs
- [ ] Gorilla delta-of-delta timestamps. Prometheus widened Gorilla's buckets for milliseconds, so choose nanosecond buckets from the census
- [ ] Gorilla XOR on prices as doubles, proving each value round-trips to its exact integer
- [ ] XOR on the raw integers, for comparison

Competing codecs
- [ ] Timestamps: delta against delta-of-delta, frame of reference per block, Stream VByte
- [ ] Prices: per-block frame of reference, delta, per-symbol delta, adaptive scale (never a hard-coded 100), patched outliers for stub quotes
- [ ] Order references: delta against the running maximum, patched outliers, per-type streams
- [ ] Shares, locate, type and flags: dictionary, run-length and bit-packing as the census suggests
- [ ] Decide whether a general-purpose block codec on top pays for itself

SIMD on both architectures
- [ ] Scalar kernels always built, written to auto-vectorise, because FastLanes found clang's output matched intrinsics
- [ ] One dispatch table, one shared prolog, kernel and tail driver, callers free of `#ifdef`. CMake selects kernel files by `CMAKE_SYSTEM_PROCESSOR`, so a Linux build needs no code change
- [ ] arm64: NEON is baseline, so no runtime dispatch. No SVE or SME, and no bit-deposit instructions exist on the M4
- [ ] x86-64: runtime dispatch to AVX2 from cached CPUID, with AVX2 kernels in their own translation units to avoid the ODR problem Arrow documents. A CPU check for BMI2 is not enough on Zen 1, so kernels use no PEXT, PDEP or gather
- [ ] AVX-512: not shipped, because neither host can run it, and an unexecuted kernel is untested code
- [ ] An environment variable caps the kernel level. A test fails if the requested kernel did not run (S1, S6)
- [ ] Width, offset and tail matrix and scalar-vs-kernel `memcmp` for every kernel, on both architectures (S3-S5)
- [ ] x86-64 kernels cannot execute on the Mac: Rosetta is absent, and only compile checks are possible there. They must first run on `debian`, and their tests must pass there before any AVX2 number counts. See the open question in NOTES.md about sanitizers for these kernels
- [ ] Compare 64-bit lanes with 32-bit lanes relative to a block base, since 64-bit lanes are the weakest case in every source
- [ ] Fused unpack-plus-prefix kernel against two passes
- [ ] 64-bit prefix sum: NEON against a scalar dependency tree, since Arm's engineers report scalar wins on Neoverse
- [ ] Record what the layout gave up, what speedup it bought, and which instruction set each number used

Measure on both hosts
- [ ] Every target in BENCHMARKS.md measured on its stated hosts, from committed code, with raw outputs on disk
- [ ] Sync to `debian` with `debian_sync.sh`. Every Linux number cites its commit
- [ ] Full comparison runs, one system at a time, with Linux as the headline for pinned numbers
- [ ] Note in any macOS comparison that QuestDB runs scalar `Vanilla` kernels on arm64

## Phase 4: Final review and done criteria

- [ ] Every target met or beaten, or the write-up states the measured number, the gap and why
- [ ] Local check script passes
- [ ] Each fuzzer clean for 30 minutes, run under `heavy.sh`
- [ ] Full-day round trip passes on the pinned file, on both hosts
- [ ] README: what the project is, how to build and run it, generated results table, definition of "record"
- [ ] WRITEUP.md: design trade-offs with numbers on data layout, memory and branch prediction, what was tried and what did not work, including whether Gorilla lost on this data and what SIMD cost
- [ ] Full repo review against the code-quality bar finds nothing that would block a merge. Each problem gets file and line, why it is wrong and how to show it fails, then a fix
- [ ] Final phase report
