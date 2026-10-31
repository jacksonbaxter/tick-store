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
- [x] Comparison systems actually run on `debian`, smoke only (`debian-host.md`; raw outputs kept outside this repo)
- [x] Machine probe finished: Q2b-e, measured Q3, Q4 (`machine-probe.md`)
- [x] Project session opens the source of every number that became a target
- [x] BENCHMARKS.md, TASKS.md and NOTES.md finished
- [x] Phase report

## Phase 2: Harness, before any implementation

Scaffold
- [x] CMake project, C++23, presets for dev, ASan+UBSan, TSan, fuzz and bench. The toolchain comes from the environment, never a hardcoded path
- [x] GoogleTest through pinned FetchContent. Nothing vendored in git
- [x] Google Benchmark and HdrHistogram the same way, pinned to exact commits. HdrHistogram is its core-only build, with no zlib
- [x] All scratch files stay inside the folders, including for subagents. A tool that needs a temp dir gets `TMPDIR` under the gitignored `build/`
- [x] Every build preset capped at 6 parallel jobs, because full builds in several repos at once ran this shared Mac out of memory

Data
- [x] Data script: `scripts/fetch_data.py`, same files as itch-orderbook. Stands alone, and accepts a path to an existing copy, verified the same way. Each check is shown failing on an injected fault in `tests/scripts/test_fetch_data.py`
- [x] SHA-256 pin computed here (`scripts/data_pins.json`), for cross-checking against itch-orderbook once its script has computed its own
- [x] Verify the `.gz` SHA-256, the gzip CRC32 and ISIZE, and the decompressed SHA-256, on `12302019`. Nasdaq publishes no usable checksum: every `.md5sum` returned 404
- [x] Download, pin and verify the second day, `10302019`
- [x] Same data on `debian` under `~/quant-projects/tick-store/data`, through the repo's own script, by `--from` on an existing copy, with every pin checked
- [x] Benchmark day decided: `12302019`, 8.25 GB decompressed, which fits `debian`'s 15 GB RAM

Census of the pinned file, because no source publishes these
- [x] Message count and bytes per type, reconciled against the gzip ISIZE (`results/census-*.json`, both days)
- [x] Unknown types, frame length against spec length, tail bytes, zero-length terminator
- [x] Tracking number: share non-zero
- [x] Timestamps: non-decreasing or not, share repeating the previous nanosecond, delta and delta-of-delta histograms, global and per locate
- [x] Prices: share that are multiples of 100, per message type, and range
- [x] Order references: delta histograms by message type
- [x] Stock field against locate: any mismatch
- [x] `BENCHMARKS.md` called the file unmeasured in CR-1's `Checked` cell, and its status line said nothing had been measured. Both predated the census, and both are corrected

Correctness oracle
- [x] Own small ITCH 5.0 decoder and re-serialiser, covering all 23 types, with a raw exception path for anything unknown
- [x] README defines "record" as one ITCH message of the source file
- [x] Oracle compares every re-serialised record with the source bytes and hashes the whole stream, so order, prefixes and tail are proven too
- [x] Injected single-bit corruption test shows the check catches it and reports where
- [x] Oracle fails correctly on a deliberately broken store, proving it is not vacuous
- [x] G-1 and G-2 pass on the full `12302019` and `10302019` days on macOS (`results/roundtrip-*.json`), through the stub store
- [x] Repeat the full-day G-1 on `debian` (`results/roundtrip-12302019-linux.json`)
- [ ] Phase 3's store plugs into `RecordReader` with the oracle unchanged

Tests and fuzzing
- [x] Unit tests for everything built so far, including corrupt fixtures and boundary lists (T1, T3). T2 and T4 wait for the Phase 3 codecs
- [x] One libFuzzer target per parser and decoder built so far: the ITCH message decoder, the frame cursor and the stub store, following Z1-Z7
- [ ] Phase 3's parsers and decoders get theirs
- [x] Coverage report shows every validator is reached (`scripts/coverage.sh`, `results/coverage.json`). It gates on 89 required functions: `decode_as` and `encode_as` for all 23 types, the parsers, the validators, the fuzz oracles and the oracle's compare path. No SIMD kernels exist yet, so none are claimed
- [ ] Phase 3's kernels join the required list in `scripts/coverage_report.py`

Slice for the comparison harness
- [x] `tools/tick_slice`: every `S` and `R` of `12302019` plus the window 09:29:30 to 09:34:30, 6,125,294 messages and 206,138,339 bytes, proved clean by the strict decoder and by a round trip, pinned under `slices` in `scripts/data_pins.json`
- [ ] The comparison harness verifies the slice against its pin before loading it

Benchmarks
- [x] Google Benchmark plus HdrHistogram, warm-up, seeded inputs, verify before measuring (`bench/itch_codec_bench.cpp`)
- [x] Results header: host label, CPU model, kernel, compiler version, flags, ISA, commit, core, governor, frequency, turbo state, pinning status, timer, load lines from `measure.sh`. Flags are sanitised at build time, and `scripts/check_results.py` refuses a file that breaks any rule, each rule shown failing in `tests/scripts/test_check_results.py`
- [x] macOS records "not pinned". The pinning and perf-counter code is built anyway, and reports not-supported
- [x] Mac results state plainly that they are unpinned and have no counters, and `proc_pid_rusage` is not used in their place. Neither is a blocker, because `debian` supplies both
- [x] Linux: `taskset -c 4`, `cpu5` idle as stated, output written to a file on `debian`. TSC calibrated on every run. Boost ceiling is unknown, so record observed frequency, not an assumed one. Batch anything below about 100 ns; publish no single-shot percentile the timer cannot resolve
- [x] macOS: report no latency below a few ticks of the 42 ns timer. The floor is 10 ticks, and the guard refuses anything under it
- [x] Linux counters through `perf stat` with `:u` events, limited to the timed passes by perf's control FIFOs
- [ ] Confirm what `cache-misses` counts on Zen 1 against AMD documentation before quoting it. Until then the results mark it unavailable
- [x] Results table in the README generated from results files, never typed. `--check` fails on a hand edit
- [ ] Record whether `cpu5` really stayed idle during a run, for example from its `/proc/stat` times, since nothing enforces it and the results only state it
- [ ] Boost cannot be turned off without root, so pinned numbers ran near 3.56 GHz with the clock free to move. Decide before Phase 3 whether to ask for a fixed-clock run, or to report cycles as the headline
- [ ] Nothing uses `cpu6` yet. The first two-thread benchmark must pin its second thread there, past the `taskset -c 4` it inherits

Comparison harness
- [x] One schema covering the same records for all systems, so the ratio baseline is identical. Decide how the 23 message types map to columns and how unused fields are stored
- [x] Read-back check for each comparison system against the source records
- [x] Parquet: streaming `ParquetWriter`, settings from `column-systems.md` section 7, row-group sweep
- [x] ArcticDB on LMDB: settings from section 7, including rows-per-segment and columns-per-segment sweeps
- [x] QuestDB: pre-created tables with `TIMESTAMP_NS`. Never rely on ILP auto-create, which silently lost nanoseconds. Clock stops at `wait_wal_table`
- [x] Each result records exact version and all settings
- [x] Durability classes A and B for every system, flush granularity stated. Verify that QuestDB `sync` really flushes, and validate the config value, because a typo silently gives `nosync`
- [x] Harness reads back the setting in force for every comparison system and fails if it differs from what was requested, because QuestDB silently accepts a bad `cairo.commit.mode`. A durability class is not proven until this passes
- [x] macOS cold-cache tool built on the probe's result: stop the server, `msync(MS_INVALIDATE)` on a fresh read-only mapping of every file, `mincore` must read 0 on every file with a retry otherwise, start, query, disk0 byte delta must reach the expected bytes. State that metadata stays warm
- [x] Linux cold-cache tool: stop the process, fsync, `posix_fadvise(POSIX_FADV_DONTNEED)`, prove with `fincore` or `mincore`. Show it on real QuestDB and ArcticDB data files, since only a fresh test file is proven so far
- [x] QuestDB on `debian`: record the heap, `-Xmx1g` for the slice, and read bind addresses with `ss`, not `SHOW PARAMETERS`
- [ ] Record GraalVM and jemalloc in the results header. Each file has them only under a class's `facts`
- [x] Check QuestDB's `debian` heap against the full day before relying on any `-Xmx1g` result: adequate in classes A and B, and the Phase 3 setting. See `results/fullday-capacity-12302019.json`
- [x] ArcticDB and pyarrow on `debian`: check for SIGILL at full size, since the wheels' zmm code is untraced and the CPU has no AVX-512. No SIGILL on streamed write, one column read and one window read, the paths that ran
- [x] Name the sync primitive beside every macOS class B number: `fsync`, `F_BARRIERFSYNC` and `F_FULLFSYNC` differ by about 80x at p50
- [x] A warm result counts only if residency is proven before and after the run
- [x] One system runs at a time. Record peak RSS, heap settings, `MemAvailable` and stored bytes. Keep the Python overhead visible rather than hidden in engine time
- [x] The real-system failures in `tests/compare/test_real_systems.py` ran on both hosts: 4 tests pass with no skip, and `not_a_mode` raises `SettingsMismatch`
- [x] `fuzz_columns` covers the column directory and manifest parsers: clean for 60 s, 2,318,532 runs. The 30 minutes stay under the G-3 task below
- [ ] Parquet sweep gaps against section 7: no `none` codec variant, and no run of `ts` on its default encoding against `DELTA_BINARY_PACKED`
- [ ] ArcticDB sweep gap against section 7: only one symbol for the whole table ran, not one symbol per security
- [ ] `pq.read_table` imports `pyarrow.dataset` inside the timed call, so `cold_includes` is wrong for Parquet. Import it before the clock, find the rest of Parquet's cold floor, then re-run
- [ ] macOS `st_blocks` reads 17% to 24% above the apparent size on five of six Parquet variants, and on ArcticDB `rows100k-cols1` in class B. Find why before quoting a macOS stored ratio for them
- [ ] Repeat the read-back on QuestDB's converted Parquet partition before quoting its size as proven

Checks
- [x] ASan and UBSan with no recover, TSan on concurrent code
- [x] clang-tidy, and `-Wall -Wextra -Werror`
- [x] GitHub Actions config, validated locally with actionlint, and recorded as never executed on GitHub. There is no remote yet, so no job has ever run there, and the Linux jobs have run nowhere
- [ ] First real run of `.github/workflows/ci.yml` on GitHub, once a remote exists, and any Linux-only failure it finds
- [x] One local script that runs every check (`scripts/check.sh`), in sequence, with a summary table
- [x] Every check shown failing on an injected fault: `scripts/prove_checks.py`, `faults/`, `results/check-proofs.json`. Each fault must pass its baseline, then fail with the expected exit code and string

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
- [ ] Per-locate layouts cannot trust locate 0: `K` records carry it while naming real stocks, so a symbol filter has to read the stock field there

Required codecs
- [ ] Gorilla delta-of-delta timestamps. Prometheus widened Gorilla's buckets for milliseconds, so choose nanosecond buckets from the census
- [ ] Gorilla XOR on prices as doubles, proving each value round-trips to its exact integer
- [ ] XOR on the raw integers, for comparison

Competing codecs
- [ ] Timestamps: delta against delta-of-delta, frame of reference per block, Stream VByte
- [ ] Prices: per-block frame of reference, delta, per-symbol delta, adaptive scale (never a hard-coded 100), patched outliers for stub quotes
- [ ] Order references: delta against the running maximum, patched outliers, per-type streams. The census favours the previous value in the same field over the running maximum on both days, so find what `A`'s two modes of delta follow before choosing
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
- [ ] x86-64 kernels cannot execute on the Mac: Rosetta is absent, and only compile checks are possible there. They must first run on `debian`, and their tests must pass there before any AVX2 number counts
- [ ] AVX2 kernels: short ASan and UBSan pass on `debian`, through `~/quant-projects/bin/heavy.sh`, avoiding `cpu4` and `cpu5`, never overlapping a measurement
- [ ] AVX2 decoders: fuzz runs on `debian`, because the Mac cannot execute them, under the same three conditions
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
