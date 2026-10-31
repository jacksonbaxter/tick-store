# tick-store notes

A running record of what was found and tried, including what did not help. Evidence, with quotes and exact locations, is in
`research/evidence/`.

## Phase 1 findings

### No published number measures this problem

Nobody has published columnar storage of ITCH. No source compares Parquet, ArcticDB and QuestDB on the same tick data. The only
whole-file ratio on ITCH is the gzip ratio implied by Nasdaq's own files, 2.31 to 2.34. So almost every target in BENCHMARKS.md is
context, and the census of the pinned file is the first real measurement this project makes.

### Pinned data

- The reference day is `12302019`, because it is the same data as itch-orderbook, so the comparison between the two projects holds.
  It is 8.25 GB decompressed, which fits `debian`'s 15 GB RAM. `10302019` is the second day, and figures labelled with it are
  second-day figures.
- `12302019` was downloaded and measured here: 3,524,013,057 bytes compressed, 8,251,407,909 decompressed, 268,744,780 messages, all
  as stated beforehand. The gz ratio is 8,251,407,909 / 3,524,013,057 = 2.34148, not the 2.3414 or 2.34146 quoted earlier.
- Nasdaq publishes no usable checksum, so `scripts/data_pins.json` proves a copy matches our download, not what Nasdaq first wrote.
  Both `12302019` hashes do match the third-party values in `research/evidence/itch-data.md` L5.
- Walking the length prefixes of `12302019` ends exactly at end of file, with no frame off its spec length, no zero-length message,
  and a last message of `S` code `C`. That confirms directly, for this day, what the byte accounting below could only infer.

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
- The tightest real-data anchor is KX on NYSE TAQ: 31.3% of an 8-byte timespan at zstd 1, about 20.0 bits. Percentages cannot be
  copied across columns: KX's `Sequence_Number` and `Offer_Size` are 4-byte `int` and `TradePrice` is a 4-byte `real`, so those targets
  are stated in bits.
- Second-day figure: mean gap is about 83 µs over market hours on 10302019, across the whole venue (derived). The busiest millisecond in the Nasdaq
  report implies a gap near 366 ns, and that report describes an average day, with the open able to run higher. Several messages can share a nanosecond, and no text guarantees timestamps never go backwards,
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

All three run natively on both hosts from inside the permitted folders, in smoke tests of 10,000 rows, so this says nothing about
scale or speed. pyarrow 26.0.0, ArcticDB 6.28.2 on LMDB and QuestDB 10.1.0, with CPython 3.13.18 because ArcticDB has no 3.15 wheel.

| System | macOS | `debian`, no Docker |
|---|---|---|
| pyarrow | Runs | Runs, 35 of 35 checks, runtime SIMD `avx2` |
| ArcticDB | Runs | Runs, LMDB write and read exact. Wheel's zmm code untraced, and the CPU has no AVX-512, so a larger run must watch for SIGILL |
| QuestDB | Runs on Temurin 25.0.5, scalar kernels | Runs on bundled GraalVM 25.0.2 with jemalloc preloaded, AVX2 tier, capped at `-Xmx1g`, which holds for a full day. Default heap untested |

What this means for a fair run:

- Defaults are not like for like. Parquet never fsyncs, default QuestDB is `nosync`, and default ArcticDB syncs every storage key.
- `pyarrow` does not use the page index or Bloom filters on read, so row groups are the smallest pruning unit. Do not credit it with more.
- ArcticDB `date_range` walks every index row under the default static schema. No public zstd option was found. LMDB is not its
  fastest backend, so results carry that label.
- QuestDB's vector kernels run scalar on arm64, so a macOS result understates its x86 path. Linux is the fair comparison.
- QuestDB's runtime differs by host, GraalVM and jemalloc on Linux against Temurin on macOS, which is one more reason never to compare
  across hosts. On `debian`, `SHOW PARAMETERS` shows `http.min.bind.to = 0.0.0.0:9003` while the real listener is loopback: trust `ss`.
- ILP auto-create silently truncated 10,000 timestamps to microseconds, so tables must be pre-created.
- `cairo.commit.mode=not_a_mode` is accepted with no warning and silently becomes `nosync`, while `SHOW PARAMETERS` echoes the bad
  value. Whether `sync` really flushes is unverified. QuestDB's own docs conflict on what WAL commits guarantee.
- A `tick-store` build tuned for the host has a compiler-target advantage over generic wheels, which must be disclosed.

### Durability and cold cache

- macOS (Q2a, plain APFS, one 1 GiB file, under `measure.sh`): `msync(MS_INVALIDATE)` evicts, proven three ways: 0 of 65,536 pages
  by `mincore`, 1,024 MiB of device reads on the next pass, and timing, 39 ms warm against about 170 ms cold. It works while another
  process holds the file mapped, and lost no dirty data in the test. `F_NOCACHE` and every `madvise` variant tried do not evict.
- A separate process with read access can evict a file another process holds open or mapped (3.5 ms and 17 to 20 ms for 1 GiB). The
  holder then really re-reads from disk. Evicting another process's dirty, unsynced mapping lost nothing in 3 of 3 rounds.
- A tree costs about 13 µs per file, and data goes cold but metadata stays warm, because the evictor must open every file.
- A disk image takes metadata cold too, but cuts sequential reads 2.7x and raises `F_BARRIERFSYNC` p50 42x, so it must not be used to
  compare systems.
- Chosen macOS method: stop the server, `msync(MS_INVALIDATE)` across the tree, prove, start, query, then check the disk0 byte delta.
  The proof is not optional: 1 pass of about 38 left 128 of 65,536 pages resident, and 60 targeted repeats did not reproduce it, cause
  unknown. So `mincore` must read 0 on every file, with a retry otherwise.
- A server's own user-space buffers cannot be evicted from outside, so servers restart between cold runs.
- Linux: `posix_fadvise` always returns 0 and skips dirty, mapped and writeback pages, so eviction must be proven. On `debian` a 64 MiB
  file on ext4 went from fully resident to 0 under `fincore`. That has not been shown on a comparison system's own files.
- Linux `fsync` on ext4 corresponds to macOS `F_FULLFSYNC`, not macOS `fsync`.
- macOS sync latency, 4 KiB append on plain APFS, 4,000 samples per mode: `fsync` p50 49 µs, p99 316 µs, 1 device op. `F_BARRIERFSYNC`
  288 µs and 826 µs, 16 ops. `F_FULLFSYNC` 4.0 ms and 5.1 ms, 16 ops. So plain `fsync` is roughly 80x cheaper than the call that matches
  Linux, and class B needs the primitive named beside every macOS number. Power-loss behaviour was not tested.

### Timers and placement

- macOS: `steady_clock` claims 1 ns but resolves one 41.667 ns tick, and costs about 12 ns a read against 4.0 ns for
  `mach_absolute_time`.
- `THREAD_AFFINITY_POLICY` is rejected, kr=46. QoS hints were accepted but moved nothing: BACKGROUND stayed on P cores, while
  `taskpolicy -b` did move threads to E cores. So nothing here pins, and thread QoS does not choose a cluster.
- `kpc` is denied, EPERM. `proc_pid_rusage` returns process-wide instructions and cycles with no privilege, but no cache or branch
  events. Decision: it is not used in place of counters. Mac results state plainly that they are unpinned and have no counters.
- `debian`: TSC 1997.94 MHz, `cycles:u` and `instructions:u` work, and `perf` comes from `.toolchain`.
- `debian` clock facts differ from the evidence: the governor floor is 1.6 GHz, one snapshot showed 2.79 GHz, and the boost ceiling is
  still unknown because `cpuinfo_cur_freq` is root-only.

### Decisions

- AVX2 kernels get a short ASan and UBSan pass on `debian`. AVX2 decoders also get their fuzz runs on `debian`, because the Mac cannot
  execute them. These run through `~/quant-projects/bin/heavy.sh`, avoid `cpu4` and `cpu5`, and must not overlap a measurement.
- macOS counters and pinning are not blockers, because `debian` supplies both. Results from the Mac state plainly that they are
  unpinned and have no counters. `proc_pid_rusage` is not used in their place.
- QuestDB silently accepts a bad `cairo.commit.mode`, so the harness must read back the setting in force for every comparison system
  and fail if it differs from what was requested. A durability class is not proven until that passes.
- QuestDB's `debian` heap must be checked against the full day before any `-Xmx1g` result is relied on. Done: see "Full-day capacity on
  `debian`" under Phase 2 findings. `-Xmx1g` is the Phase 3 setting.
- All scratch files stay inside the folders from now on, including for subagents.

## Phase 2 findings

First measurements on the two pinned days, `12302019` and `10302019`, in that order throughout. The figures are in
`results/census-*.json` and `results/roundtrip-*.json`. "Ideal code" below is the entropy of a value's sign and power-of-two bucket
plus the low bits left over: a fair way to compare two transforms of one column, not the size of a real codec.

### The round trip holds

- G-1 passes on both full days: 268,744,780 and 293,989,079 records, bit-exact, both stream hashes equal to the pin, about 33 s to
  verify. G-2 passes on both: a seeded flip in the store was named at the exact record, source byte and bit, and the store passed again
  once it was flipped back.
- Peak RSS for a verify is 16.6 GB and 18.1 GB, which is both files fully mapped. Those are clean file-backed pages. The process's own
  footprint peaks near 70 MB, so nothing holds a day on the heap. Read `peak_rss_bytes` with `peak_memory_footprint_bytes` beside it.

### Prices: the tick premise holds for orders, and fails for trades

- Of non-zero `A`, `F` and `U` prices, 99.53% and 99.61% are multiples of 100: `A` 99.53% and 99.60%, `F` 96.78% and 96.77%, `U` 99.74%
  and 99.82%.
- Every non-zero `A`, `F` or `U` price off a whole cent is below $1.00: all 656,931 and all 605,403. So sub-penny order prices are the
  sub-dollar tick, and nothing else.
- `P` is different: only 59.84% and 66.84% are whole cents, but 97.77% and 99.31% are multiples of 50. About a third of hidden trades
  print on a half cent, which fits the midpoint executions Phase 1 expected. A price codec with one global scale of 100 would treat a
  third of `P` as exceptions. `C` sits at 99.41% and 99.79%, `Q` at 96.49% and 96.38%.
- The range is as wide as Phase 1 feared: 1 to 1,999,999,900 across `A`, `F` and `U` on both days. So the adaptive scale and patched
  outliers stay in the plan. In `I`, 73.5% and 71.5% of far prices are zero, and in `Q`, 68.3% and 68.0% of cross prices are.

### Timestamps: delta-of-delta looks worse than delta

- Timestamps never step back on either day, in file order or within any locate: 0 decreases in 562,733,859 values.
- 1.24% and 1.31% repeat the previous nanosecond. The gap between records is usually tens of microseconds: the median falls in
  [16.4, 32.8) µs on both days. Non-zero gaps start in [64, 128) ns, with almost nothing there: 1 and 2 values, then millions from 128 ns.
- Delta-of-delta looks worse than delta on every view:

| View | Ideal code, delta | Ideal code, delta-of-delta | Mean bits, delta | Mean bits, delta-of-delta |
|---|---|---|---|---|
| File order | 17.15, 17.14 | 18.44, 18.42 | 14.53, 14.50 | 14.65, 14.65 |
| Per locate | 23.37, 23.39 | 26.78, 26.79 | 19.81, 19.83 | 22.09, 22.10 |

- Gorilla's one-bit case, a delta-of-delta of zero, covers 0.75% of values here, against its 96.39% on fixed-interval data. The Gorilla
  codec is still required by G-4. Its buckets need choosing from these histograms, and a loss here would be a finding.
- File order costs about 6 bits less per value than per-locate order, so a per-symbol layout pays for it on timestamps. On this rough
  measure, file-order delta is under CR-3's 20.0 bits and per-locate delta is not.

### Order references: the previous value beats the running maximum

- `A` references do not simply count up. 73% of steps are positive, and the deltas have two modes. 43% of all steps are +1 to +15, most
  of those +4 to +7. The rest are large jumps, up to about 2^25, nearly equal in number in each direction. That looks like several
  sequences interleaved, with the stream hopping between them, but the cause is not proven.
- Delta against the previous value in the same field beats delta against the running maximum in every field that carries references:
  ideal code 16.8 against 20.6 bits for `A`, 23.2 against 24.1 for `D`, on `12302019`, and the same order on `10302019`. Phase 3 lists the
  running maximum first, so it now has to earn its place.
- `D` is the hardest column, at 23.2 and 22.3 bits. CR-6's 11.2 bits is well beyond a plain delta, so it depends on the layout.
- `E` repeats the previous reference 12.8% and 15.0% of the time, as it would if one order fills in several parts. Every `P` reference
  is zero.

### Other census facts

- The files are clean: no unknown type, no frame off its spec length, no tail, no zero-length terminator. Frames sum to the file size on
  both days, and that size agrees with the gzip ISIZE.
- 18 of the 23 types appear on each day. `N`, `O`, `W` and `h` appear on neither, `B` only on `10302019`, and `K` only on `12302019`. So real
  days cannot exercise every decoder path, and the hand-built fixtures have to.
- 2.73% and 3.15% of tracking numbers are non-zero, and they are concentrated: nearly all of `E`, `P`, `Q` and `C`, about 8% of `F`, and under
  0.3% of anything else. A zero-or-not flag per type would cover most of this column.
- Stock against locate: 124,125,385 and 134,154,625 comparisons, with 2 and 0 mismatches. Both mismatches are the `K` records below.
  No locate changes its stock during a day, and no stock has two locates.
- `10302019` has 8,887 `R` records for 8,886 locates: one locate is sent twice, with the same stock both times. `12302019` has one per locate.

### The comparison slice

`data/slices/12302019-open.itch` holds every `S` and `R` of `12302019` plus the window 09:29:30 to 09:34:30: 6,125,294 messages, of which
6,116,383 are the window, and 206,138,339 bytes. That is about 6 million, so the default was not narrowed. The strict decoder accepts every
record, it round-trips through the oracle, and `shasum` agrees with our SHA-256. The window has no record out of its time range, as a
non-decreasing file guarantees. Definition, sizes and hash are under `slices` in `scripts/data_pins.json`.

### Full-day capacity on `debian`

Each system took all 268,744,780 records of `12302019` once, through streamed ingest, on three CPUs and two engine threads, in class B
unless stated, with a watchdog on the host. This is a capacity check, not a benchmark, and `results/fullday-capacity-12302019.json` holds
it. All three survived: every step exited 0, no process saw SIGILL, no `hs_err` file appeared, the watchdog never fired and
`MemAvailable` never fell under 6.0 GiB. A full-column scan counted every row on each, and it and a one-second window at the open matched
the ground truth. Each system's stored-form checks passed. That SIGILL answer covers the paths run here and no others: streamed write,
one column read, one window read.

- **QuestDB's `-Xmx1g` is adequate, and is the Phase 3 setting.** In both class A and class B the server ran to WAL fully applied with no
  collection after start-up. The only pauses are the three full collections of every server start, under 0.25 s in total. The most
  used after a collection was 116 MiB, and at most 300 MiB of 982 MiB was in use when ingest ended. WAL lag stayed at 3 transactions or
  fewer, memory pressure at 0, and the server log has no error line. The default heap was not tested, since nothing pointed to it.
- **Neither the heap nor mapped pages is the risk.** The server's VmHWM was 1.4 GiB in ingest and 2.4 GiB in queries, of which at most
  1.1 GiB was mapped. The largest process is the Python ingest client, at 5.5 GiB. Its anonymous memory climbs about 300 MiB a chunk to
  about 5.4 GiB, falls and repeats, while Arrow's pool stays under 514 MiB, so the native client holds it. It is bounded rather than
  day-sized, but it is the first thing to shrink if `debian` ever runs short.
- **A day needs more than the slice's settings in one place.** ArcticDB's 2 GB LMDB map cannot hold a day, which stores 5.9 GiB, so the
  full-day path maps 64 GB. ArcticDB's full-column scan is the largest read, at 5.4 GiB, against 1.4 GiB for Parquet's.
- **Peak disk was small.** QuestDB used about 1 GiB more at peak than at rest, nothing like a second copy of the table, so applied WAL
  segments must go during ingest. At rest it holds 2.78x the source, against 2.61x on the slice, and Parquet 0.39x and ArcticDB 0.77x.
- Not covered: no whole-store read-back through G-1, which needs a day on the heap, no warm or cold proof, and no durability proof at
  full size. The server ran through `bin/questdb.sh` and `JVM_PREPEND`, while reported runs start `java` directly with the same options.
- `ru_maxrss` from `wait4` read no lower than the runner's own VmHWM here: a worker `/proc` put at 0.3 GiB reported 1.1 GiB. The figures
  here use `/proc`, or `ru_maxrss` only where it exceeds the runner's peak. The reported slice runs read `ru_maxrss` the same way, and were not rechecked.

### What the data contradicted

- **Locate 0 is not only for records with no stock.** `research/evidence/itch-data.md` D6 has locate 0 meaning "not stock dependent". All
  3 `K` records of `12302019` carry locate 0, while naming `MKD`, `ZWZZT` and `ZJZZT`, each of which has a real locate in the directory
  from about 03:09, hours earlier. The census check was first written too loosely and hid this, by skipping any record whose locate had
  no `R`. It now holds each locate to the first stock named for it, and the 2 mismatches stand in the results. A per-symbol layout cannot
  take a `K`'s symbol from its locate.
- **Order references are 27.7% and 27.9% of the file**, just under Phase 1's 28 to 32%. Without the 2-byte prefixes they are 29.7% and 29.9%.
  They are still the largest field class. Header plus prefix at 42.3% and the timestamp at 19.5% both land inside Phase 1's ranges.
- **Signed timestamp deltas are not needed on these two days**, and Phase 1 kept them "unless the census proves otherwise". Two days prove
  nothing about the next one, and no text guarantees the order, so the format should keep a way to represent a step back.

### Benchmark harness

Numbers are in the README tables, and they are harness evidence, not targets. Seven things
differed from what the plan assumed. The Mac's raw `cntvct_el0` counts 1 GHz units in steps of about
42, so a greatest common divisor of its deltas would report a 1 ns timer: the harness reads
`mach_absolute_time`, whose units are the real 24 MHz steps. On `debian`, `rdtscp` then `lfence`
costs 25.2 ns, not the 19 ns of `rdtscp` alone, so the read cost and not the 10 ns step sets the
floor, at 126 ns. A whole-process `perf stat` would have charged SHA-256 of 412 MB to the codec,
so perf starts disabled and the binary opens it around the timed passes through perf's control
FIFOs; that needs one process per workload, so each results file holds one lock per case, and
four load lines. `measure.sh` sets the governor only once it has the lock, so sysfs read outside
the run says `schedutil` while the run used `performance`; governor and frequency are taken from
the load lines, which also print the hostname, and that token is stripped. Core clock is not what
sysfs suggests: `scaling_max_freq` says 2.0 GHz, single reads at the load lines ranged from 2.0 to
3.43 GHz, and cycles over wall time gave 3.56 GHz, so both are recorded and only the second is a
measurement. The first process after the governor change ran its two warm-up passes in 134 ms
against 102.5 ms for every timed pass. The cause is not established, but warm-up absorbed it and
no timed pass shows it. Batch timers cost about 2.5% (105 against 102.5 ms a pass), which is why the distribution
comes from separate passes and never touches the throughput figure or the counters. One result
worth carrying forward: encoding adds 45 cycles and 136 instructions a message on Zen 1 but no
branch misses at all (0.51 a message in both cases), so every miss is in the decode half. Why
encode's own dispatch on type predicts so well is not established. Full-day G-1 on `debian` took
63 s to ingest and 116 s to verify, 3.0x and 3.6x the Mac, at 9.2 GB peak RSS of clean mapped
pages, with `MemAvailable` steady at 12.9 GB. The first Linux build needed no code change.

### Comparison harness

These are slice results, from `results/compare/` at commit `53a3317`: 6,125,294 records and 206,138,339 source bytes of one five-minute
window. They prove the harness works and are not full-day claims. `compare/README.md` has the method. Each host ran 22 variant-and-class
stores: Parquet 12, ArcticDB 6, QuestDB 4. Ratios are stored bytes over source bytes, to six places.

**Read-back.** The oracle's verdict is `pass` for all 22 on both hosts, so Parquet, ArcticDB and QuestDB each rebuilt the slice with its
stored SHA-256 equal to the pin. Query agreement has no surprise: all 110 digests (11 variants, 5 queries, 2 hosts) equal the ground
truth, and the ground truth is the same on both hosts.

**Setting in force.** `tests/compare/test_real_systems.py` has now run on the real systems, 4 tests with no skip on either host. A QuestDB
conf holding `not_a_mode` while `sync` was requested started anyway, echoed the text with source `conf`, and its own parser said `nosync`.
The harness raised `SettingsMismatch`: "cairo.commit.mode: the conf holds 'not_a_mode', which is not one of ('nosync', 'async', 'sync'),
and QuestDB would silently run nosync". All 22 classes read `proven` on both hosts, by these means:

- Parquet, both hosts: thread counts from pyarrow, then the footer and first page header for the stored form. The zstd level is not
  provable on either host, since the footer has no level. On Linux the detector works: class A left 231 dirty pages in the probe and 17,207
  after the full write of `zstd1-rg1Mi`, class B 0. On macOS dirty pages show nothing: 0 in class A and class B, probe and full write, for
  every variant. So macOS rests only on the count of harness flushes: 0 in A, 2 by `F_FULLFSYNC` in B.
- ArcticDB, both hosts: 10 settings from `lib.options()`, the library config in storage and the engine's config. Class A left 438 dirty
  pages in the probe on Linux and 120 on macOS, class B 0. LZ4 is shown from stored bytes. Its acceleration is not readable on either host.
- QuestDB, both hosts: 14 settings, with the commit mode from the conf, the echo and the same jar's parser. Class A left 2,390 dirty pages
  in the probe on Linux and 602 on macOS, class B 0 data pages.
- Not provable on macOS: that QuestDB's class B reached the device. 0 dirty pages shows the page cache is clean, and its plain `fsync`
  stops short of the device's cache, so the file marks class B not like-for-like. Median write time barely moves, 0.82 s in A and 0.84 s
  in B, for `plain`. `MemAvailable`, pinning and hardware counters are not available there.
- QuestDB's class B is marked not like-for-like on Linux too, for another reason. `sync` flushes the WAL at commit and not the table:
  after a full write 31,344 pages of the table's own files were still dirty on Linux and 2,832 on macOS, once the rows were readable. The
  flush proof exempts them by name, and the small probe left 21 and 6 pages in such files. Parquet and ArcticDB are like-for-like in
  every class on both hosts.

**Stored size.**

| Store | Linux bytes | Linux ratio | macOS bytes | macOS ratio |
|---|---|---|---|---|
| Parquet `zstd1-rg1Mi` | 70,483,968 | 0.341926 | 83,968,000 in A, 84,471,808 in B | 0.407338, 0.409782 |
| ArcticDB `rows100k-cols127` | 141,168,640 | 0.684825 | 141,860,864 | 0.688183 |
| ArcticDB `rows100k-cols1` | 504,848,384 in A, 504,864,768 in B | 2.449076, 2.449155 | 510,418,944 in A, 521,445,376 in B | 2.476099, 2.529589 |
| QuestDB `plain` | 537,550,848 | 2.607719 | 537,772,032 | 2.608792 |
| QuestDB `plain`, partition converted | 129,056,768 | 0.626069 | 137,641,984 | 0.667717 |
| QuestDB `locate-index` | 597,078,016 | 2.896492 | 597,344,256 | 2.897783 |
| QuestDB `locate-index`, partition converted | 188,964,864 | 0.916690 | 194,932,736 | 0.945640 |

- QuestDB's hot tier is larger than the source, at 2.6 times it. The twelve fixed columns alone are 60 bytes a row, 367,517,640 bytes,
  against 33.65 bytes a record at source. Converting the partition to Parquet gives a file of 128,174,106 bytes on both hosts, 0.621787 of
  the source, and 128,555,797 bytes with the index. The converted ratios above are computed here from the file's `allocated_bytes_after`,
  which covers the whole table directory. The read-back was not repeated on the converted form, so no converted size is oracle-proven.
- **macOS's Parquet ratios are not the files' sizes.** `apparent_bytes` is the same on both hosts: 70,478,425 for `zstd1-rg1Mi`, 0.341899
  of the source. macOS's `st_blocks` total is 17.47% to 23.90% above that on five of the six variants, and on some it differs between
  classes for the same bytes. The sixth, `zstd1-rg1Mi-delta-price`, is at 82,264,064 against 82,262,783. So the macOS column makes zstd level 9 look no
  better than level 1 (84,074,496 against 83,968,000) and delta encoding look best, while the apparent sizes give 67,854,290, 70,478,425
  and 82,262,783, the other way round. Read Parquet's sweep from Linux. ArcticDB's `rows100k-cols1` shows the same in class B, at
  521,445,376 allocated over 510,394,624 apparent. The cause is not established.

**Cold proof.** All 385 cold proofs on each host passed, each eviction in one pass, with no shortfall. On Linux it is tight: device reads
equal the resident bytes exactly in 273 proofs, and exceed them by 8,192 in every ArcticDB proof and by at most 659,456 for QuestDB.
On macOS it is weak: `iostat` is machine-wide reads and writes, and its excess runs from 2,621 to 109,320,602 bytes, and is never under
11,193,548 for QuestDB. It can only catch a shortfall.

**A fault in the Parquet cold numbers.** Parquet has a floor that does not depend on bytes read. Q5 reads 4,259,840 bytes cold on macOS,
with a median of 148,133 µs against 7,800 µs warm, and 4,182,016 bytes on Linux, 240,523 µs against 24,901 µs. ArcticDB's Q4 is 6,326 µs
and 21,307 µs cold. Parquet's warm Q1, the first query a worker runs, has a maximum of 156,078 µs and 281,651 µs against medians of 18,086
µs and 55,365 µs. Part of this is the harness: `pq.read_table` imports `pyarrow.dataset` itself, inside the timed call, so the files'
`cold_includes` line, that imports fall before the clock, is wrong for Parquet. A spot check on macOS, outside any run, put that first read
at 16 to 22 ms with warm caches, against under 1 ms for the second. That spot check undercounted, and the import is the whole floor. On
macOS, in a fresh process, Q5 took 126 ms cold and 125 ms warm with the import left to `pq.read_table`, 10.3 ms and 9.1 ms with it before
the clock, and 7.8 ms on a second call, while the import timed alone took 116 ms, cold or warm, 6 repetitions each. Page residency does
not move it, which is why warm Q1's first repetition showed it too. An iteration run of `zstd1-rg1Mi` with the import before the clock
had a warm Q1 maximum of 25 ms against a median of 18 ms. What is left is about 1 ms of first-call start-up and the cold bytes, and the
cause was not traced further. `Table.group_by` likewise imported `pyarrow.acero` inside Q3's clock. Linux's floor was not measured apart.
The numbers quoted above, and every Parquet cold median and Q1 tail in the files at `53a3317`, hold the import.

## Process notes

- A runner fault killed all eight research subagents before any wrote its evidence file. Fresh agents rebuilt the files from
  transcript digests and cached sources. They now create their file first and add to it.
- One subagent ended its turn with a background job running, which killed the job partway through Q2b. A subagent's final message
  ends its background work.
- A usage limit stopped the session at about 13:00, with nothing committed. Findings are now committed as they arrive.
- Checking sources after the draft changed five targets. Two claims were wrong in a way only the opened source showed: `O-T`'s best on-disk
  figure is TimescaleDB, not kdb+, and the feed-rate anchor was not the highest row in the workbook. `TradePrice` was also read as 8 bytes
  in a draft reference line. The lesson is to read the whole table, not the cell the subagent quoted.
- `12302019` downloaded at about 50 MB/s, on one connection. Then, with itch-orderbook fetching over four connections, every
  connection to the same host fell to about 85 KB/s. Cause not proven. A slow day needs planning for, not more connections.
- Audit outputs can name private files. Keep them out of the repo, or redact them, before committing.
- A clone in the working tree exposed its own skills to this session. Nothing from it was invoked. Third-party files now stay in
  the research cache, and `docs-recheck/` was moved there before committing.
