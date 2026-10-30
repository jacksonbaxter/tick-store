# tick-store notes

A running record of what was found and tried, including what did not help. Evidence, with quotes and exact locations, is in
`research/evidence/`.

## Phase 1 findings

### No published number measures this problem

Nobody has published columnar storage of ITCH. No source compares Parquet, ArcticDB and QuestDB on the same tick data. The only
whole-file ratio on ITCH is the gzip ratio implied by Nasdaq's own files, 2.31 to 2.34. So almost every target in BENCHMARKS.md is
context, and the census of the pinned file is the first real measurement this project makes.

### Prices: XOR looks like a poor fit, but not on data like ours

- Every source that compares them finds XOR on doubles does badly on decimal-origin values. On Stocks-USA, Gorilla takes 37.7
  bits/value and ALP 7.9 (ALP Table 4).
- The closest same-data result is BOS Fig. 10(a) on Nifty-Stocks: Gorilla 1.19x, scaled-integer TS2DIFF+BP 1.82x, SPRINTZ+BOS 2.07x.
- It does not always lose: on integer set TC, Gorilla beats TS2DIFF+BP, 6.39 to 4.00, and IoTDB reports Gorilla does better on
  integers than on floats. Nobody explains TC.
- No source tests tick-level exchange prices. ITCH prices are already integers, so ALP's decimal-to-integer step has nothing to
  recover here. The remaining ideas are delta or frame of reference, bit-packing, and GCD. This has to be measured.
- Tick premise: $0.01 is 100 raw units, but nobody publishes the share of ITCH prices that are multiples of 100. Sub-penny midpoint
  executions are allowed, and a $0.005 tick arrives by November 2027. Stub quotes span nearly the whole field: 1 to 1,999,999,900
  on S061226. So the scale must be adaptive and outliers patched.

### Timestamps: Gorilla's headline depends on regularity ITCH does not have

- Gorilla's 96.39% one-bit share comes from fixed-interval monitoring data. Its 1.37 bytes per point is the same.
- The only same-data comparison across resolutions is TritanDB: delta-of-delta grows from 5.2 to 23.2 bits per timestamp going from
  milliseconds to nanoseconds, on regular data with jitter (derived).
- On uneven data in the same paper, a delta scheme beat delta-of-delta. Delta-of-delta may lose to plain delta on ITCH.
- Mean gap is about 83 µs over market hours on 10302019, across the whole venue (derived). The busiest millisecond in the Nasdaq
  report implies a gap near 393 ns. Several messages can share a nanosecond, and no text guarantees timestamps never go backwards,
  so signed deltas stay unless the census proves otherwise.
- Gorilla's and Prometheus's bucket widths must not be reused for nanosecond data without measuring.

### SIMD: nobody vectorises an unmodified Gorilla stream

- No SIMD decoder for the unmodified bit stream was found. ALP says the branches and data dependencies "prevent SIMD". Redis and
  Intel looked and declined. The TsFile profile puts 81.8% of samples in control-bit parsing and variable-width reads.
- Two routes work. A better scalar decoder with the format unchanged gained 1.76x to 2.57x (TsFile). Otherwise the format changes,
  and every format that vectorises does the same thing: fix the width per block, move control data out of line, split into lanes.
- Costs where sources give them: D4 instead of D1 costs up to 2 bits per integer; the FastLanes layout costs 1 bit per value, or
  0.21 bit with delta-coded bases (arithmetic, not measured); Parquet-style min-delta blocks give up delta-of-delta's 1-bit case.
- The two-accumulation step costs 2.4x to 3.0x over delta, on 32-bit lanes on Neoverse V2 (derived, reconstruct only).
- 64-bit lanes are the weakest case everywhere. On the M4 the classic 4-lane recipe was slower than scalar (3.6 against 3.9 billion
  values/s), and only the interleaved load won, on 32-bit values. Arm's engineers report a scalar dependency tree beating NEON at
  64 bits. Timestamps are 48 bits, but block-relative values fit 32-bit lanes, which is worth testing.
- Fused kernels repeatedly beat two passes.
- On M1, explicit NEON was not faster than scalar for ALP decode. Auto-vectorised scalar code is competitive (FastLanes).

### Hosts shape the kernels

- macOS: NEON only. SVE and SVE2 raise SIGILL. SME exists, but with no BDEP and no FA64 in streaming mode, and the literature says
  it is slower than NEON for this kind of work. No bit-deposit instruction exists on this CPU.
- Zen 1: PEXT and PDEP take 18 cycles, against 0.25 for BZHI or SHRX, so CPUID's BMI2 flag is not a safe test (OpenJDK and Arrow
  reached the same conclusion). `VPSRLVD` on 256 bits has reciprocal throughput 4 against 2 on 128 bits, so 256-bit gives no gain
  per byte for that instruction. Gather is very slow.
- x86-64 from the Mac: AVX2 and AVX-512 VBMI2 compile, but nothing runs, because Rosetta is absent. Hosted Linux cross-compiles fail
  for lack of libc headers, and only freestanding translation units compile. x86 kernels first run on `debian`.

### What a bit-exact round trip really needs

- BinaryFILE says a zero-length message ends the session, and a file without one is incomplete. **The sample files contradict
  that.** Byte accounting across three days matches the gzip ISIZE exactly with no terminator, and third-party censuses agree. The
  last bytes of a file were not seen directly. Completeness is therefore last message `S` code `C`, and the tail is stored verbatim.
- Global order is implicit and timestamps cannot rebuild it. Splitting by type or symbol needs a stored ordinal or permutation, and
  that cost belongs in the ratio.
- All body bytes must survive, meaningful or not: `H` Reserved, `P`'s zero reference and constant side, `R` flags, space padding.
  A prior project's first round trip failed with 8,906 differences because undecoded directory bytes were zeroed.
- Unknown types or lengths need a raw exception path.
- Compare per record and also hash the whole stream, so order, prefixes and tail are all proven.
- The `.gz` is out of scope for the round trip: its bytes cannot be regenerated. Pin both hashes.
- Order references are the largest byte class, 28 to 32%. The header plus prefix is 41 to 43%, and the timestamp alone 19 to 20%.
  Databento's 56-byte record is larger than raw ITCH at about 31 bytes.

### Comparison systems

All three run natively on the M4 Max from inside the repo: pyarrow 26.0.0, ArcticDB 6.28.2 on LMDB, QuestDB 10.1.0 on Temurin
25.0.5, with CPython 3.13.18 because ArcticDB has no 3.15 wheel. Smoke tests only. What this means for a fair run:

- Defaults are not like for like. Parquet never fsyncs, default QuestDB is `nosync`, and default ArcticDB syncs every storage key.
- `pyarrow` does not use the page index or Bloom filters on read, so row groups are the smallest pruning unit. Do not credit it with more.
- ArcticDB `date_range` walks every index row under the default static schema. No public zstd option was found. LMDB is not its
  fastest backend, so results carry that label.
- QuestDB's vector kernels run scalar on arm64, so a macOS result understates its x86 path. Linux is the fair comparison.
- ILP auto-create silently truncated 10,000 timestamps to microseconds, so tables must be pre-created.
- `cairo.commit.mode=not_a_mode` is accepted with no warning and silently becomes `nosync`, while `SHOW PARAMETERS` echoes the bad
  value. Whether `sync` really flushes is unverified. QuestDB's own docs conflict on what WAL commits guarantee.
- A `tick-store` build tuned for the host has a compiler-target advantage over generic wheels, which must be disclosed.

### Durability and cold cache

- macOS (Q2a, plain APFS, one 1 GiB file, under `measure.sh`): `msync(MS_INVALIDATE)` evicts, proven three ways: 0 of 65,536 pages
  by `mincore`, 1,024 MiB of device reads on the next pass, and timing, 39 ms warm against about 170 ms cold. It works while another
  process holds the file mapped, and lost no dirty data in the test. `F_NOCACHE` and every `madvise` variant tried do not evict.
- A server's own user-space buffers cannot be evicted from outside, so servers restart between cold runs.
- Linux: `posix_fadvise` always returns 0 and skips dirty, mapped and writeback pages, so eviction must be proven.
- Linux `fsync` on ext4 corresponds to macOS `F_FULLFSYNC`, not macOS `fsync`.

### Open question for the orchestrator

The brief keeps sanitizer runs and fuzzing on the Mac, but x86-64 kernels cannot execute there. Unless told otherwise, the plan runs
the exact-size-buffer tests and a short ASan and UBSan pass of the AVX2 kernels on `debian`, treating them as Linux-only code.

## Process notes

- A runner fault killed all eight research subagents before any wrote its evidence file. Fresh agents rebuilt the files from
  transcript digests and cached sources. They now create their file first and add to it.
- One subagent ended its turn with a background job running, which killed the job partway through Q2b. A subagent's final message
  ends its background work.
- A usage limit stopped the session at about 13:00, with nothing committed. Findings are now committed as they arrive.
- A clone in the working tree exposed its own skills to this session. Nothing from it was invoked. Third-party files now stay in
  the research cache, and `output/docs-recheck/` was moved there before committing.
