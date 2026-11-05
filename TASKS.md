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
- [x] Repeat the full-day G-1 on `debian` (`results/roundtrip-12302019-linux.json`). Repeated on the real store at `4302ce8`, where a store written on the Mac also verifies and is byte-for-byte the store debian writes
- [x] Phase 3's store plugs into `RecordReader` with the oracle unchanged: `ItchStoreRecordReader` walks the segments and `tick_roundtrip verify` runs as before
- [x] G-1 and G-2 pass on the full `12302019` and `10302019` days on macOS through the real store, at N = 2^20 (`results/roundtrip-*.json`). G-2 flips one bit in a data block and one in the footer, each caught with CRCs on (a block or footer CRC failure naming the offset) and with them off (the oracle names the record, or the decoder the field). The stub no longer counts for G-1 or G-2

Tests and fuzzing
- [x] Unit tests for everything built so far, including corrupt fixtures and boundary lists (T1, T3). T2 and T4 wait for the Phase 3 codecs
- [x] One libFuzzer target per parser and decoder built so far: the ITCH message decoder, the frame cursor and the stub store, following Z1-Z7
- [ ] Phase 3's parsers and decoders get theirs. The two Gorilla decoders have theirs, `fuzz_gorilla_timestamps` and `fuzz_gorilla_xor`, and so do PFOR and Stream VByte, `fuzz_pfor` and `fuzz_stream_vbyte`, the dictionary and run-length codecs, `fuzz_dictionary` and `fuzz_run_length`, and the container parser, `fuzz_container`, each clean for 240 s under `heavy.sh`. The ITCH store has `fuzz_itch_store`, which reads below the checksum and checks decode, re-encode and compare: 904,149 runs in 240 s, clean. The queries have `fuzz_itch_query`, which feeds whole store files below the CRCs into the indexed and exact paths: 38,445 runs in 270 s, clean. On store version 3, from the new encoder's seeds, `fuzz_itch_store` ran 873,182 runs and `fuzz_itch_query` 49,206 in 300 s each, clean
- [ ] The PFOR fuzz oracle checks values, not bytes, since the encoder makes choices. A field the decoder wrongly accepts without changing a value, such as non-zero tail padding, is caught only by the unit tests' corrupt fixtures
- [x] Coverage report shows every validator is reached (`scripts/coverage.sh`, `results/coverage.json`). It gates on 204 required functions: `decode_as` and `encode_as` for all 23 types, the parsers, the validators, the codecs' decoders and encoders, PFOR, Stream VByte, lane packing and patch lists among them, the container's parser, writer and durability calls, the software CRC32C kernel and this processor's hardware one, the fuzz oracles and the oracle's compare path, and the ITCH store's writer, reader, segment coder and metadata parsers. All 204 reached at `4302ce8`, lines 93.83%, functions 97.39%. No SIMD kernels exist yet, so none are claimed
- [x] Phase 3's kernels join the required list in `scripts/coverage_report.py`: the dispatch, the scalar set, and this processor's own set, NEON on arm64 or AVX2 and its CPUID check on x86-64 (NOTES.md, SIMD; required 255 of 255 reached on the Mac)

Slice for the comparison harness
- [x] `tools/tick_slice`: every `S` and `R` of `12302019` plus the window 09:29:30 to 09:34:30, 6,125,294 messages and 206,138,339 bytes, proved clean by the strict decoder and by a round trip, pinned under `slices` in `scripts/data_pins.json`
- [x] The comparison harness verifies the slice against its pin before loading it: `Runner.verify_slice` in `compare/run_system.py` hashes the slice against the pin and rebuilds the pin from the export through the oracle, first thing in `Runner.run`, and stops the run on a difference

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
- [x] Record whether `cpu5` really stayed idle during a run: `sibling_busy_percent` per case in the bench and compare headers, from `/proc/stat` inside the lock, with the sibling read from the kernel's topology. The validators refuse over 5%, a limit of our choosing. Nothing enforces idleness. First real readings at `1b7cfe1`: 0.34% and 0.15% for the two bench cases, 1.25%, 0.37% and 0.41% for Parquet, ArcticDB and QuestDB
- [ ] Boost cannot be turned off without root, so pinned numbers ran near 3.57 GHz with the clock free to move. Decide before Phase 3 whether to ask for a fixed-clock run, or to report cycles as the headline
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
- [x] Record GraalVM and jemalloc in the results header: `questdb_runtime`, with the vendor and version from the running JVM's own properties and jemalloc from the libraries mapped into the running server, on both hosts. `scripts/check_compare.py` requires it. Shown on the real server on both hosts: Temurin 25.0.5+8 without jemalloc on macOS, GraalVM CE 25.0.2+10.1 with jemalloc preloaded on `debian`
- [x] Check QuestDB's `debian` heap against the full day before relying on any `-Xmx1g` result: adequate in classes A and B, and the Phase 3 setting. See `results/fullday-capacity-12302019.json`
- [x] ArcticDB and pyarrow on `debian`: check for SIGILL at full size, since the wheels' zmm code is untraced and the CPU has no AVX-512. No SIGILL on streamed write, one column read and one window read, the paths that ran
- [x] Name the sync primitive beside every macOS class B number: `fsync`, `F_BARRIERFSYNC` and `F_FULLFSYNC` differ by about 80x at p50
- [x] A warm result counts only if residency is proven before and after the run
- [x] One system runs at a time. Record peak RSS, heap settings, `MemAvailable` and stored bytes. Keep the Python overhead visible rather than hidden in engine time
- [x] The real-system failures in `tests/compare/test_real_systems.py` ran on both hosts: 4 tests pass with no skip, and `not_a_mode` raises `SettingsMismatch`
- [x] `fuzz_columns` covers the column directory and manifest parsers: clean for 60 s, 2,318,532 runs. The 30 minutes stay under the G-3 task below
- [x] Parquet sweep gaps against section 7: no `none` codec variant, and no run of `ts` on its default encoding against `DELTA_BINARY_PACKED`. `none-rg1Mi` and `zstd1-rg1Mi-ts-default` ran on both hosts at `25a462b`, cited in compare/README.md; the headline is unchanged (NOTES.md, tick-store in the comparison)
- [x] ArcticDB sweep gap against section 7: only one symbol for the whole table ran, not one symbol per security. `rows100k-cols127-per-security` ran on both hosts at `25a462b` through `write_batch` and `read_batch`: 134 s to write in class B on Linux and 217 s on macOS, against 1.2 and 0.5 s for one symbol
- [x] `pq.read_table` imported `pyarrow.dataset` inside the timed call, so `cold_includes` was wrong for Parquet. `ParquetSystem.open` now imports it first, and `pyarrow.acero` too, and `tests/compare/test_system_worker.py` fails if any module loads inside the clock. On macOS that import was the whole cold floor, 116 ms: see "The Parquet cold numbers at `53a3317` held an import" in `NOTES.md`
- [x] Re-run the comparison and the benchmarks on both hosts at the current harness: macOS bench at `ad2bab5` and compare at `8251c0b`, Linux bench and compare at `1b7cfe1`, all one code state. The validators accept all four, and the files they superseded are removed
- [ ] macOS `st_blocks` reads 17% to 24% above the apparent size on all six Parquet variants, and on ArcticDB `rows100k-cols1` and `rows10M-cols127` in class B. Find why before quoting a macOS stored ratio for them
- [ ] Repeat the read-back on QuestDB's converted Parquet partition before quoting its size as proven

Checks
- [x] ASan and UBSan with no recover, TSan on concurrent code
- [x] clang-tidy, and `-Wall -Wextra -Werror`
- [x] GitHub Actions config, validated locally with actionlint, and recorded as never executed on GitHub. There is no remote yet, so no job has ever run there, and the Linux jobs have run nowhere
- [x] First real run of `.github/workflows/ci.yml` on GitHub, once a remote exists, and any Linux-only failure it finds. Run 41770320877 at `7ebb81b` failed 10 of 14 jobs; the fixes are in NOTES.md, "The first runs on GitHub Actions", and run 41825067372 at `ad542fd` passed all 14
- [x] One local script that runs every check (`scripts/check.sh`), in sequence, with a summary table
- [x] Every check shown failing on an injected fault: `scripts/prove_checks.py`, `faults/`, `results/check-proofs.json`. Each fault must pass its baseline, then fail with the expected exit code and string

## Phase 3: Implementation

Checks carried from Phase 2
- [x] A `taskset-removed` fault patch, so `scripts/prove_checks.py` covers `scripts/run_bench.py`'s pinning. On macOS `test_a_linux_case_records_its_siblings_busy_share` catches it
- [ ] A fault patch for `scripts/run_bench.py`'s sibling meter. No patch in `faults/` targets it
- [x] clang-tidy on the Linux-only sources, `affinity_linux.cpp` and `page_cache_linux.cpp`: `scripts/check.sh` lists its sources without git in an exported tree, so its clang-tidy step on Linux covers every translation unit Linux builds, and fails on an injected violation in either file
- [ ] `scripts/check.sh` on the Linux host, in an exported tree, at `469e9cd`: 17 of 21 steps pass, but `coverage` and `hygiene` call git, which has no repository there, `actionlint` is not installed there, and `py-compare` skips for want of a `.venv`
- [x] CI has never run on GitHub, and the Linux `run_bench` fixture bug means its Linux job would have failed until `a6a82fa`. The first real run is the open item under Phase 2 Checks: done, green at run 41825067372
- [ ] Refresh `results/check-proofs.json` with a full `scripts/prove_checks.py` run: the fault `results-linux-turbo-unmeasured` (`5c78dcf`) was proven alone with `--only`, so it is not in the committed record yet

Storage
- [x] File format: header, per-block CRC32C, footer, codec registry, little-endian helpers (F1-F7)
- [x] Writer with named durability modes and a commit ordering that survives torn writes (M1-M8)
- [x] Memory-mapped reader with mapped length proved at open and view lifetimes tied to the mapping (A3, M5, M7, M9) (`docs/format.md`)
- [ ] CRC32C runs one dependent chain of crc32 instructions, about 3 cycles a step. Three interleaved streams joined by a shift table would run near one a cycle; measure verification against decode before adding them
- [ ] No codec is `read_only` yet, so the path that keeps a retired codec readable but unwritable is tested only through the registry's own table. Test it with a fixture when the first codec retires
- [x] zstd takes codec id 8, which the store writer emits on the locate, creation_ref, add_price, imbalance_flags, rare_bytes and raw_bytes columns where it is smaller (`docs/format.md`)
- [ ] SIGBUS from a file cut under a live mapping is not caught, by design (`docs/format.md`). No test shows the process dying; add a death test that truncates a mapped file if the stance is ever revisited
- [x] Block CRCs are checked at lookup, not at open, so a scan that reads only zone maps never verifies payloads. The query engine looks every block up through the reader (`ItchStoreReader::column_view`, CRC-checked), never by offset
- [x] Time-range index with block zone maps, and a symbol filter whose cost counts in the ratio. Store version 2: the footer's segment summaries, a seek column per segment (a time zone map, imports and type counts per 4,096 records) and the locate column held as runs with a run directory. The index is 5.97 MB on `12302019` and 6.46 MB on `10302019`, and the run-form locate column 40.1 and 45.0 MB more than the file-order one; ratio 5.517 and 5.508 (`results/store-size-*.json`, NOTES.md)
- [x] Queries Q1 to Q5 with the harness's semantics (`ItchQuery`, `tools/tick_query`): every slice digest in `results/compare/` and every whole-day 12302019 answer of compare/queries.py's numpy ground truth match, on both paths on the Mac and on the indexed path on debian. Four faults proven: a time-window off-by-one, a symbol filter skipping a run, an unordered run directory, a digest moved by a nanosecond
- [x] Add tick-store as a system in `compare/`, driving `tools/tick_query`, and run the comparison: `compare/systems/tickstore.py`, checked by `scripts/check_compare.py`, on the slice on both hosts at `25a462b`. Lowest on Q2 and Q3 warm and cold on both hosts, slowest on Q1 warm and Q4; smallest store, 4.89 on Linux; slowest writer except in Linux class B, with ITCH parsing in its time (NOTES.md)
- [x] Store version 3 for the queries: records stored by locate in 16,384-value pieces, a run directory with add and import starts, timestamps as residuals over 64-record time bases. 1,750,915,467 bytes on `12302019` (ratio 4.713, +255 MB) and 1,928,632,798 on `10302019` (4.685); CR-1 and CR-2 still pass. Whole-day Q2 warm 2,257 to 45 ms on the Mac, Q3 1,262 to 22 ms; G-1 and G-2 pass on both days, G-1 on debian (NOTES.md, Query iteration)
- [ ] QL-1, QL-2 and QL-3 still miss at iteration, unpinned, on store version 3: Q2 warm 45.0 ms on the Mac (116.2 on debian) against 16.9 ms, Q3 21.7 ms (48.2) against 0.41 ms, Q4 cold mean 39.2 ms (105.2) against 10 ms (NOTES.md, Query iteration). Measure them under `measure.sh` before any verdict
- [x] Q2 and Q3 decoded every record of each segment up to the locate's last one. Store version 3 stores each locate's records as one run, so they read their own run: Q2 decodes 16.3 MB instead of 337.9 MB, at a cost of 255 MB a day, mostly timestamps and creation references out of file order (NOTES.md)
- [ ] QL-2: a per-locate minute aggregate of the adds would answer Q3 from about 60 entries. Measured on a prototype serialization, 14.6 MB per day or 27.7 MB per segment (zlib). Build it only with its bytes counted, and time it
- [ ] Q1 and Q4 decode their segment's whole locate column, every run, to put records back in file order: 9.0 MB, Q4 warm 24.7 ms and cold 39.2 ms on version 3, slower than version 2's 21.6 ms warm. A file-order copy would cost up to the 276 MB the version 1 column took; find something cheaper
- [ ] Q4 on the slice is tick-store's worst query against the others: warm 53.6 ms on Linux against QuestDB's 1.7 and ArcticDB's 6.2, and 22.9 ms on macOS against 0.30 and 1.70. It decodes the segment's whole locate column for 338 rows; the Q1 and Q4 item above is where to fix it
- [ ] Measure the whole-day comparison with tick-store, which has run on the slice only
- [ ] tick-store's ingest is the slowest in class A on both hosts, 5.5 s on Linux and 2.4 s on macOS, against 0.5 to 4.1 s for the others, but holds ITCH parsing. Measure how much of it is parsing, with a timer inside `tick_roundtrip ingest`, before saying how its encoding compares
- [ ] Q5 is bound by the scalar dictionary decoder: 152 M share values at 3.35 ns each, 510 ms warm. SIMD did not move it (502 ms warm after the kernels): Huffman decoding is a chain of dependent lookups with no vector form, and a multi-symbol table cost more to build per block than it saved. Decoding several Huffman streams at once needs a format change; try it, or a different code for the share columns. A stored share sum would be a benchmark special
- [x] debian has no `perf`, so the version 3 queries have no Linux profile. Profile them there if `perf` becomes available. Done with the project toolchain's `perf` at `43aa7d2`: the dictionary decoder is about 55% of the engine's time there too (NOTES.md, SIMD)
- [ ] The store offers no zstd on timestamps, add_shares or tracking, which the estimate allowed where smaller. Timestamps are 13.8 MB of the 69.5 MB gap to the estimate. Measure the saving per column against the decode time before adding any
- [ ] E, C and X shares are stored plain, 2.1 MB a day more than coding them against the remaining shares, because an imported order carries no book state. Try importing an order's remaining shares with its reference, counting both
- [ ] The writer keeps one locate per reference, so a reference named under alternating locates is imported each time. Count how often that happens on the real days before changing the map
- [ ] CR-2 on `10302019` is "no comparison": the capacity check stored only `12302019` in the other systems. Run them on the second day if CR-2 is ever wanted there

Layout experiments, measured and recorded in NOTES.md whichever way they go
- [ ] File-order blocks with per-type columns, against per-symbol columns plus a stored permutation
- [ ] Block size sweep: ratio against range-query latency
- [ ] Separate columns per message type against shared columns
- [ ] Per-locate layouts cannot trust locate 0: `K` records carry it while naming real stocks, so a symbol filter has to read the stock field there

Required codecs
- [x] Gorilla delta-of-delta timestamps. Prometheus widened Gorilla's buckets for milliseconds, so choose nanosecond buckets from the census. Buckets of 16, 19 and 23 bits; 20.40 and 20.42 bits per value in file order, missing CR-3 by 0.4 (NOTES.md, "Gorilla on ITCH")
- [x] Gorilla XOR on prices as doubles, proving each value round-trips to its exact integer. Every value on both days exact, and all 2^32 Price(4) values; add-order prices 54.2 and 54.9 bits per value
- [x] XOR on the raw integers, for comparison. 29.2 and 29.5 bits per value on add-order prices, and the integer as a double 22.9 and 23.1

Competing codecs
- [x] Timestamps: delta against delta-of-delta, frame of reference per block, Stream VByte. PFOR on deltas meets CR-3 in file order at 18.76 and 18.69 bits; delta beats delta-of-delta by 1.2 bits; per locate every codec misses, Stream VByte best at 25.5 (NOTES.md, "Integer codecs on ITCH")
- [x] Prices: per-block frame of reference, delta, per-symbol delta, adaptive scale (never a hard-coded 100), patched outliers for stub quotes. Add prices 15.25 and 15.03 bits in file order, missing CR-4 by 7.1 to 7.4 against order-0 floors of 13.1 (delta) and 13.9 (value); per locate 6.26 and 6.42, and per locate and side 5.48 and 5.62, meeting CR-4 with the locate column stored. CR-5 met against XOR on dollars everywhere, and against the best Gorilla per locate; in file order 1.50 and 1.54
- [x] Per-locate CR-4 rests on the store keeping a file-order locate column. Without one, the permutation costs at least 11.05 bits per add, its order-0 entropy, and per locate loses to file order. The layout experiment below must count that column. Counted: every layout in the estimate stores it, at 9.28 and 9.31 bits per message by dictionary, and the mixed layout with per-locate add prices is still the smallest
- [ ] Record each block's chosen scale in `tick_codec_eval`, so the factor the adaptive scale finds is shown, not only the remainders it leaves
- [ ] Per-locate timestamps: PFOR patches 28% of values. Try a smaller vector or a second patch width there before giving them up to Stream VByte
- [ ] The PFOR encoder made a whole-day `tick_codec_eval` run 3.7 times longer, 207 s against 56 s, mostly trying each candidate scale. Its speed is unmeasured; measure it before choosing it for the writer
- [x] Order references: delta against the running maximum, patched outliers, per-type streams. The census favours the previous value in the same field over the running maximum on both days, so find what `A`'s two modes of delta follow before choosing. `A`'s deltas follow about four interleaved counters stepping by 4, not stock, side or participant. Head delta meets CR-6 on creations at 7.80 and 7.64 bits; references that name an order meet it as distances within their stock, 6.0 to 9.2 bits, with the locate stored (NOTES.md, "Every field")
- [x] Shares, locate, type and flags: dictionary, run-length and bit-packing as the census suggests. A dictionary codec wins on nearly all of them. CR-7 met on add shares (3.69 and 3.64, per locate 2.61 and 2.55) and `U` shares, missed on `E`, `C` and `X` by 0.11 and 0.19 at best
- [x] Decide whether a general-purpose block codec on top pays for itself. zstd level 1 on three columns only: creation references, stock locate and per-locate add prices, 89 and 95 MB a day at 0.05 to 0.54 times their decode time. Elsewhere it saves 2% or less, or costs about twice the decode time
- [x] Estimate the whole store: 5.50 and 5.49 times smaller than the source in the mixed layout, 4.97 and 4.98 in file order, 4.51 and 4.50 per locate. CR-1 and CR-2 met by every layout on `12302019` (`results/store-estimate.json`, an estimate)
- [ ] CR-7 on `E`, `C` and `X` shares: 3.91 and 3.99 bits coded against the remaining shares, above order-0 floors of 3.89 and 3.99. Try the book's context: the remaining shares as a bound, or the order's add shares as a predictor
- [ ] `U`'s new reference alone misses CR-6 by 0.007 on `12302019`. It meets inside the creation stream; keep it there unless a reader needs it apart
- [ ] Locate by recency rank: its entropy is 6.76 and 6.80 bits against the dictionary's 9.28 and 9.31. Build a rank codec over about 8,900 entries and measure its decode cost before choosing it
- [x] Per-stock reference distances need each stock's creation list while decoding. Built per segment with an import list, measured by `tick_reference_eval` on the real codecs: 173.9 MB a day at N = 2^20, against 464 to 494 MB for references stored whole, and a book checkpoint for day-wide distances cost 2,016 MB (NOTES.md, "The ITCH store"). The reader holds one list per locate for one segment only. Its time per reference has not been measured on its own; that belongs with the speed benchmarks
- [ ] Measure the head-delta, dictionary and run-length encoders' and decoders' speed before the writer and reader choose them
- [ ] A whole-day `tick_codec_eval` run now takes 950 s with a 6.0 GB footprint, from 207 s and 1.36 GB. Find what holds the memory, and keep `--columns` for exploring
- [ ] Re-run the estimate at 1,024-value blocks when the file format fixes its block size and index

SIMD on both architectures
- [x] Scalar kernels always built, written to auto-vectorise, because FastLanes found clang's output matched intrinsics. The interleaved pack and unpack loops compile to NEON on arm64 at `-O2` and `-O3`, and the x86-64 SSE2 baseline output of the unpack, base add and zigzag loops is vector code too (`psrlq`, `psllq`, `por`, `paddq`). That is why NEON buys only 9% on the interleaved unpack and nothing on the add (NOTES.md, SIMD)
- [x] One dispatch table, one shared prolog, kernel and tail driver, callers free of `#ifdef`. CMake selects kernel files by `CMAKE_SYSTEM_PROCESSOR`, so a Linux build needs no code change (`include/tick_store/codec/simd.hpp`, `src/codec/simd/`)
- [x] arm64: NEON is baseline, so no runtime dispatch. No SVE or SME, and no bit-deposit instructions exist on the M4
- [x] x86-64: runtime dispatch to AVX2 from cached CPUID and XGETBV, with AVX2 kernels in their own translation unit under a `target("avx2")` pragma that starts after every standard header, and shared code in unnamed namespaces, to avoid the ODR problem Arrow documents; checked by disassembly. No PEXT, PDEP or gather
- [x] AVX-512: not shipped, because neither host can run it, and an unexecuted kernel is untested code
- [x] An environment variable, `TICK_STORE_SIMD`, caps the kernel level. A test fails if the requested kernel did not run, and ctest runs the codec, store, query and fuzz-regression tests again capped to scalar (S1, S6)
- [x] Width, offset and tail matrix and scalar-vs-kernel `memcmp` for every kernel, on both architectures (S3-S5): `tests/unit/simd_kernels_test.cpp`, passing on the Mac with NEON and on debian with AVX2 (620 of 620 on both hosts at `f8e7a68`)
- [x] x86-64 kernels cannot execute on the Mac: Rosetta is absent, and only compile checks are possible there. They ran on `debian`, and their tests passed there before any AVX2 number was taken
- [x] AVX2 kernels: short ASan and UBSan pass on `debian`, through `~/quant-projects/bin/heavy.sh`, avoiding `cpu4` and `cpu5`, never overlapping a measurement (found the width 0 `memcpy`, fixed in `a961486`; 97 tests clean at `43aa7d2`)
- [x] AVX2 decoders: fuzz runs on `debian`, because the Mac cannot execute them, under the same three conditions (240 s each at `43aa7d2`, and `fuzz_run_length` again at `f8e7a68` after its timeout fix, NOTES.md, SIMD)
- [x] Compare 64-bit lanes with 32-bit lanes relative to a block base, since 64-bit lanes are the weakest case in every source. No gain: 3.62 against 3.64 G values/s on the M4, 1.16 against 1.25 on Zen 1, widths 0 to 21. Not used
- [x] Fused unpack-plus-prefix kernel against two passes. Two passes win on the M4, 3.42 against 3.30 G values/s; fused wins on Zen 1, 1.29 against 1.19. Each set uses its winner
- [x] 64-bit prefix sum: NEON against a scalar dependency tree, since Arm's engineers report scalar wins on Neoverse. NEON wins on the M4, 4.84 against 3.25 G values/s. On Zen 1 the AVX2 in-register form loses to the tree, 1.57 against 2.04, so AVX2 uses the tree
- [x] Record what the layout gave up, what speedup it bought, and which instruction set each number used (NOTES.md, SIMD). All iteration-only
- [ ] Kernel numbers under `measure.sh`: `bench/simd_kernel_bench` cases, such as `pfor_unpack/w=13/neon`, run through `scripts/run_bench.py` as a seeded input kind, which `scripts/check_results.py` validates. Not yet measured
- [ ] DS-3 and DS-4 miss at iteration: 3.42 G values/s two-pass on the M4 against 3.6, and 2.71 cycles a value fused on Zen 1 against 0.377. The cross-lane prefix sum the value-order layout needs is the main cost; FastLanes' transposed layout avoids it, at the price of a store format change. Measure first, then decide
- [ ] DS-8 misses at iteration: delta-of-delta decodes at 4.8 GB/s of 32-bit values on the M4 against 8.20, with no kernel of its own. Delta beats delta-of-delta on this data, so build one only if a column moves to it
- [ ] The queries barely move with SIMD: Q1 to Q4 3 to 6% faster warm on both hosts, Q5 0.4 to 1.1%. The time-base residual add is a max-plus recurrence with wrapping residuals, so it stays scalar (rewritten, 0.94 to 0.69 ns a value); the dictionary decoder is the bound
- [ ] `results/check-proofs.json` predates five fault patches: `neon-add-base-tail-off-by-one` and `fuzz-svb-neon-canonical-dropped` are new, and `fuzz-pfor-zigzag-order`, `fuzz-stream-vbyte-length-check` and `store-time-past-48-bits` were made again on moved code. Each was proven with `prove_checks.py --only` and the file restored from git. Re-run the full prover to record them
- [ ] The AVX2 tail fault is shown on debian by hand, on a scratch copy, since the prover needs a git checkout and debian holds an exported tree. A host-aware fault list would let the prover run there

Measure on both hosts
- [ ] Every target in BENCHMARKS.md measured on its stated hosts, from committed code, with raw outputs on disk
- [ ] Sync to `debian` with `debian_sync.sh`. Every Linux number cites its commit
- [ ] Full comparison runs, one system at a time, with Linux as the headline for pinned numbers
- [x] Note in any macOS comparison that QuestDB runs scalar `Vanilla` kernels on arm64: README.md's comparison and NOTES.md say so

## Phase 4: Final review and done criteria

- [ ] Every target met or beaten, or the write-up states the measured number, the gap and why
- [ ] Local check script passes
- [ ] Each fuzzer clean for 30 minutes, run under `heavy.sh`
- [ ] Full-day round trip passes on the pinned file, on both hosts
- [ ] README: what the project is, how to build and run it, generated results table, definition of "record"
- [ ] WRITEUP.md: design trade-offs with numbers on data layout, memory and branch prediction, what was tried and what did not work, including whether Gorilla lost on this data and what SIMD cost
- [ ] Full repo review against the code-quality bar finds nothing that would block a merge. Each problem gets file and line, why it is wrong and how to show it fails, then a fix
- [ ] Final phase report
