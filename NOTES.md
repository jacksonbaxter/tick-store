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
sysfs suggests: `scaling_max_freq` says 2.0 GHz, single reads at the load lines of the run at
`1b7cfe1` ranged from 2.11 to 3.17 GHz, and cycles over wall time gave 3.57 GHz, so both are
recorded and only the second is a measurement. In the run at `40bc4b4`, the first process after
the governor change ran its two warm-up passes in 134 ms against a median timed pass of 102.7 ms.
The cause is not established, but warm-up absorbed it and no timed pass showed it. In the run at
`1b7cfe1` it did not recur: 102.8 and 103.3 ms against a median of 102.4 ms. Batch timers cost
about 2.5% (105 against 102.5 ms a pass), which is why the distribution comes from separate passes
and never touches the throughput figure or the counters. One result worth carrying forward:
encoding adds 45 cycles and 136 instructions a message on Zen 1 but no branch misses at all (0.51
a message in both cases), so every miss is in the decode half. Why encode's own dispatch on type predicts so well is not established. Full-day G-1 on `debian` took
63 s to ingest and 116 s to verify, 3.0x and 3.6x the Mac, at 9.2 GB peak RSS of clean mapped
pages, with `MemAvailable` steady at 12.9 GB. The first Linux build needed no code change.

The current files measure one code state: Linux at `1b7cfe1` and macOS at `ad2bab5`, with only
results files changed between them. Decode runs at 59.79 M messages/s with a CV of 0.25% on Linux
and 130.06 M with a CV of 0.80% on macOS. Decode then encode runs at 33.88 M, CV 0.24%, and
69.86 M, CV 0.67%. On Linux, cpu5 was busy 0.34% of the decode case and 0.15% of decode then
encode, the first real readings of `sibling_busy_percent`, well under the 5% limit. macOS decode
is 4.8% above the 124.12 M of the run at `40bc4b4`, with neither the codec's sources nor the
benchmark's changed between the two commits, against a CV of 0.80% in the newer run and 0.67% in
the older. On the unpinned Mac the spread between runs is several times the spread within one,
so a macOS difference of a few percent between runs is not evidence of a change. Linux decode
moved 0.3%, from 59.64 M.

### Comparison harness

These are slice results, from `results/compare/`: Linux at commit `1b7cfe1` and macOS at `8251c0b`, which measure the code at `ad2bab5`,
since the commits after it change only results files. They replace the files at `53a3317`, which predate the harness fixes in `c8ad140`,
`121803a` and `ad2bab5` and which the validators now refuse. The slice is 6,125,294 records and 206,138,339 source bytes of one five-minute
window. These files prove the harness works and are not full-day claims. `compare/README.md` has the method. Each host ran 22
variant-and-class stores: Parquet 12, ArcticDB 6, QuestDB 4. Ratios are stored bytes over source bytes, to six places.

**Runtime, heap and sibling.** QuestDB 10.1.0 ran on Temurin 25.0.5+8 on macOS, with no jemalloc mapped into the server, and on GraalVM CE
25.0.2+10.1 on Linux, with jemalloc preloaded. Both are the header's `questdb_runtime`, read from the running server. Both hosts ran
`-Xms1g -Xmx1g`. On Linux that is the heap `results/fullday-capacity-12302019.json` records as checked and adequate for a full day, and
`scripts/check_compare.py` refuses any other there. On macOS it was set to match Linux, and the file states that the full-day check is
Linux only. On both, the validator requires every class's running command to hold the stated heap. On Linux, cpu5 was busy 1.25% of the
Parquet run, 0.37% of ArcticDB's and 0.41% of QuestDB's, against the 5% limit.

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
- QuestDB, both hosts: 15 settings, with the commit mode from the conf, the echo and the same jar's parser, and the initial heap beside
  the maximum. Class A left 2,390 dirty pages in the probe on Linux and 602 on macOS, class B 0 data pages.
- Not provable on macOS: that QuestDB's class B reached the device. 0 dirty pages shows the page cache is clean, and its plain `fsync`
  stops short of the device's cache, so the file marks class B not like-for-like. Median write time barely moves, 0.83 s in A and 0.86 s
  in B, for `plain`. `MemAvailable`, pinning and hardware counters are not available there.
- QuestDB's class B is marked not like-for-like on Linux too, for another reason. `sync` flushes the WAL at commit and not the table:
  after a full write 31,344 pages of the table's own files were still dirty on Linux and 2,832 on macOS, once the rows were readable. The
  flush proof exempts them by name, and the small probe left 21 and 6 pages in such files. Parquet and ArcticDB are like-for-like in
  every class on both hosts.

**Stored size.**

| Store | Linux bytes | Linux ratio | macOS bytes | macOS ratio |
|---|---|---|---|---|
| Parquet `zstd1-rg1Mi` | 70,483,968 | 0.341926 | 84,086,784 | 0.407914 |
| ArcticDB `rows100k-cols127` | 141,168,640 | 0.684825 | 141,860,864 | 0.688183 |
| ArcticDB `rows100k-cols1` | 504,856,576 in A, 504,860,672 in B | 2.449115, 2.449135 | 510,402,560 in A, 520,101,888 in B | 2.476020, 2.523072 |
| QuestDB `plain` | 537,550,848 | 2.607719 | 537,772,032 | 2.608792 |
| QuestDB `plain`, partition converted | 129,056,768 | 0.626069 | 139,776,000 | 0.678069 |
| QuestDB `locate-index` | 597,078,016 | 2.896492 | 597,344,256 | 2.897783 |
| QuestDB `locate-index`, partition converted | 188,964,864 | 0.916690 | 200,302,592 | 0.971690 |

- QuestDB's hot tier is larger than the source, at 2.6 times it. The twelve fixed columns alone are 60 bytes a row, 367,517,640 bytes,
  against 33.65 bytes a record at source. Converting the partition to Parquet gives a file of 128,174,106 bytes on both hosts, 0.621787 of
  the source, and 128,555,797 bytes with the index. The converted ratios above are computed here from the file's `allocated_bytes_after`,
  which covers the whole table directory. The read-back was not repeated on the converted form, so no converted size is oracle-proven.
- **macOS's Parquet ratios are not the files' sizes.** `apparent_bytes` is the same on both hosts: 70,478,425 for `zstd1-rg1Mi`, 0.341899
  of the source. macOS's `st_blocks` total is 17.43% to 23.90% above that on all six variants, and on four of them it differs between
  classes for the same bytes. At `53a3317` the sixth, `zstd1-rg1Mi-delta-price`, matched its apparent size, and it is now 20.35% above
  it in class A, so the excess is not a fixed property of a variant. The macOS column makes zstd level 9 look no better than level 1
  (84,045,824 against 84,086,784 in class A), while the apparent sizes give 67,854,290 against 70,478,425. Read Parquet's sweep from
  Linux. ArcticDB shows the same in class B, where its class A matches: `rows100k-cols1` at 520,101,888 allocated over 510,394,624
  apparent, and `rows10M-cols127` at 154,771,456 over 138,100,992, 12.07% above. The cause is not established.

**Cold proof.** All 385 cold proofs on each host passed. Every eviction took one pass but one, a macOS QuestDB `locate-index` Q2 proof
that took 4. On Linux it is tight, with no shortfall: device reads equal the resident bytes exactly in 273 proofs, and exceed them by
8,192 in 104 of the 105 ArcticDB proofs and 73,728 in the last, by 53,248 in one Parquet proof, and by at most 110,592 for QuestDB. On
macOS it is weak: `iostat` is machine-wide reads and writes, in hundredths of a MiB. Its excess runs from -656 to 68,441,211 bytes, and
is never under 11,207,311 for QuestDB. The -656, in a Parquet `zstd1-rg128k` Q1 proof, is inside the 20,972 bytes of rounding the check
allows, so the macOS counter can only catch a shortfall larger than that.

**The Parquet cold numbers at `53a3317` held an import.** `pq.read_table` imported `pyarrow.dataset` inside the timed call, and
`Table.group_by` imported `pyarrow.acero` inside Q3's, so those files' `cold_includes` line, that imports fall before the clock, was wrong
for Parquet. On macOS, in a fresh process outside any run, Q5 took 126 ms cold and 125 ms warm with the import left to `pq.read_table`,
10.3 ms and 9.1 ms with it before the clock, and 7.8 ms on a second call, while the import timed alone took 116 ms, cold or warm, 6
repetitions each. An earlier spot check had put it at 16 to 22 ms, and undercounted. Page residency does not move it, which is why warm
Q1's first repetition showed it too. `c8ad140` imports both modules in `ParquetSystem.open`, and `tests/compare/test_system_worker.py`
fails if any module loads inside the clock.

The current files show the size of it. Against the files at `53a3317`, Parquet `zstd1-rg1Mi`'s cold medians fell by 133 to 140 ms on
macOS and by 201 to 207 ms on Linux, across all five queries, where Linux's floor had not been measured apart. Q5 reads 4,259,840 bytes
cold on macOS, and its cold median is now 9,997 µs against 7,551 µs warm, down from 148,133 µs. On Linux it reads 4,182,016 bytes, at
37,382 µs against 25,000 µs warm, down from 240,523 µs. ArcticDB's Q4, which reads fewer bytes, is 6,846 µs and 20,910 µs cold. Parquet's
warm Q1, the first query a worker runs, has a maximum of 22,236 µs on macOS and 83,260 µs on Linux, against medians of 17,558 µs and
54,981 µs, where it was 156,078 µs and 281,651 µs. What is left on macOS is about 1 ms of first-call start-up, which `cold_includes` now
states, and the cold bytes.

### The Linux run_bench tests had never passed

The tests in `tests/scripts/test_run_bench.py` that run the whole of `scripts/run_bench.py` on its Linux path, with stand-in `taskset`
and `perf`, run only on a Linux host, and macOS takes the unpinned path instead. They had never passed on Linux, and failed there three
ways, all in the fixtures. The fake `perf` copied one fixed file over the stand-in for `/proc/stat`, so from the second case on
no jiffies passed on the sibling, and `run_bench.py` rightly refused the run. The expected commit did not match in an exported tree,
which carries a `COMMIT` file and no git. And the fake `taskset` and `perf` wrote the binary's arguments to the same record, so each
case was counted three times. `1279de5` and `a6a82fa` fix the fixtures, with `run_bench.py` unchanged, and the fixed tests were shown
failing again on injected faults: a frozen stat file, and `taskset -c 4` removed. No `faults/*.patch` targets `run_bench.py`'s pinning or
its sibling meter, so `scripts/prove_checks.py` does not cover them, which is a gap.

## Phase 3 notes

### Rulings on the Phase 2 report

- **A sync to the Linux host rebuilds nearly everything, and that is expected.** Each sync replaces the source folder there with a
  fresh `git archive` extract, and every file in it carries the commit's timestamp, which is newer than the build outputs. So ninja
  rebuilds almost the whole tree after every sync: in the Phase 2 re-run the bench build ran 89 steps, all of it. That is the cost of
  building only committed code, and no timestamp workaround is used.
- **The 4.8% move in macOS decode is unpinned-host noise.** Decode ran at 124.12 M messages/s at `40bc4b4` and 130.06 M at `ad2bab5`,
  4.8% higher, with neither the codec's sources nor the benchmark's changed between the two, against CVs within each run of 0.67% and
  0.80%. Decode then encode moved 1.2%, from 69.02 M to 69.86 M. The difference between runs is several times the spread within one,
  which is what an unpinned host gives, and nothing to chase. macOS figures are context. Headline numbers come from the pinned Linux
  host.

### clang-tidy had never seen the Linux-only sources

Linux builds two sources macOS does not, `affinity_linux.cpp` and `page_cache_linux.cpp`, and no clang-tidy run had analysed either. The
Mac's compile databases do not hold them, and on the Linux host `scripts/check.sh` listed its sources with `git ls-files`, which finds
nothing in the exported tree a sync makes: at `4d5c14f` the step failed there with 0 files analysed. It now lists them from the tree
when there is no git and a `COMMIT` file, and at `1727ba7` it analysed 57 files on three cores in 725 s, the 59 tracked sources less
the two only macOS builds.

Both Linux-only files were clean. Seven findings came from six files every host builds: `misc-include-cleaner` asked for a header
providing `ssize_t` in five and `off_t` in two. glibc defines both in whichever libc header is included first, `stdio.h` in the three
files whose preprocessed source was checked, and `<unistd.h>`, which POSIX says provides both and which every one of the six
includes, does not count. Adding `<sys/types.h>` did not help: macOS's include-cleaner called it unused in all six, because
`IgnoreHeaders` hides the private headers macOS defines the types in, and on Linux the definition would still have been in
`stdio.h`. `469e9cd` stops naming the two types instead: `auto` for the results of `read`, `write` and `pwrite`, `std::ssize` in two
comparisons, and `std::int64_t` for two file offsets, where `-Wconversion` would catch an `off_t` narrower than 64 bits.

With no file list given, the step fails on the Linux host on an injected C-style cast in either file: in `page_cache_linux.cpp` in
741 s, and in `affinity_linux.cpp` in 717 s. A first cast in `affinity_linux.cpp`, inside an argument of the `CPU_SET` macro, went
unreported, so the check does not look inside that macro's expansion, and the proof used a cast outside any macro.

The whole of `scripts/check.sh` ran on the Linux host at `469e9cd`, in 1,491 s on three cores: 17 of 21 steps pass, clang-tidy
among them. `coverage` and `hygiene` fail because `coverage_report.py` and `check_hygiene.py` call git, and the exported tree has
none. `actionlint` fails because it is not installed there, and `py-compare` skips with no `.venv`. On macOS all 21 pass at
`55ab1d2`, whose code differs only by a workflow comment, and a checkout on a Linux CI runner has git.

### Gorilla on ITCH

G-4 is met: Gorilla delta-of-delta timestamps and Gorilla XOR prices are built, fuzzed, and measured on both full days. Every
value of every column came back exact. The figures below are from `results/codec-eval-*.json`, written by `tools/tick_codec_eval`
at `b9f4407`. "Bits per value" counts everything a reader needs: block payloads, the 8-byte block headers, and 8 bytes per stream
for its count, plus 2 for its locate in a per-locate column. A per-locate column also needs a permutation to rebuild file order.
That cost is not counted, so per-locate figures flatter per-locate layouts. Days are `12302019` then `10302019` throughout.

**Choices.** The nanosecond buckets are 16, 19 and 23 bits, with Gorilla's control codes and a 64-bit escape. An exhaustive search
over every three widths from 2 to 44, costing the census's delta-of-delta histograms, puts them first on both days together at
20.385 bits per value. Taken alone, `10302019` prefers 16, 20 and 25 by 0.006 bits. On the same histograms Prometheus's 14, 17
and 20 cost 22.3 and 22.1, and Gorilla's 7, 9 and 12 cost 59.1 and 59.2. Measured, the codec costs 0.027 bits more than the
search predicts. The block header and each block's first value, stored whole, add 128 bits per block, or 0.031 bits per value at the
default block of 4,096 values. The search costs a value on a power-of-two edge only approximately, so the two need not agree exactly.
A block is that size so a one-millisecond read near the open touches one or two blocks. The size is a parameter, from 1 to 2^20.
The search script and its output are kept with the raw logs, not in the repository.

**Timestamps miss CR-3 by 0.4 bits.**

| Column | Gorilla DoD | Census ideal, delta | Census ideal, DoD | CR-3, at most 20.0 |
|---|---|---|---|---|
| All messages, file order | 20.404, 20.418 | 17.147, 17.139 | 18.437, 18.420 | missed by 0.404, 0.418 |
| Per locate | 44.476, 44.574 | 23.366, 23.387 | 26.782, 26.787 | missed by 24.5, 24.6 |

- In file order, 57.2% of values take the 16-bit bucket, 29.8% the 19-bit one, 11.8% the 23-bit one, 0.75% are zero and 0.51%
  escape, on `12302019`. Gorilla is 2.0 bits above the ideal for its own transform and 3.3 above the ideal for plain delta.
- DoD against delta: no delta codec is built yet, so the census figure stands. Plain delta's ideal is 1.3 bits below
  delta-of-delta's on both days, 17.1 against 18.4. The tool computes these ideals itself, the same way, and they match the census
  to every printed digit on both days.
- Per-locate streams hurt timestamps. 49% of per-locate values escape to 64 bits, since a stock's gaps are far wider than the
  venue's. Even the best widths for that view, 21, 30 and 36, would cost about 29.6 on the census histograms, half again the
  file-order figure.

**Prices: XOR on the dollar doubles is the worst choice measured.** Add-order prices, `A` and `F`, 118,631,456 and 128,638,557 values:

| Representation | File order | Per locate | XOR on dollars over this |
|---|---|---|---|
| Dollars, price / 10^4, as a double | 54.218, 54.904 | 36.893, 37.881 | |
| Integer as a double | 22.925, 23.115 | 13.265, 13.370 | 2.37x, 2.38x file order; 2.78x, 2.83x per locate |
| Raw integer, XOR on the 64-bit word | 29.232, 29.506 | 22.976, 23.506 | 1.86x, 1.86x file order; 1.61x, 1.61x per locate |
| Ideal code of the integer delta | 18.646, 18.712 | 9.546, 9.822 | |

- On dollars, a non-zero XOR carries 62.2 data bits on average, so the stream saves 15% against raw 64-bit doubles. A price over
  10^4 is not a short binary fraction, so its mantissa fills all 52 bits and consecutive prices share almost none of them.
  The integer as a double has at least 21 trailing zero bits in its mantissa, and its non-zero XORs carry 24.7 bits. It also beats XOR
  on the raw integer, which carries 32.5. A likely reason is that the double's exponent sorts prices by magnitude first. That is not
  proven.
- Per locate, XOR on dollars is 36.9 and 37.9 bits, beside the published 37.7 for Gorilla on Stocks-USA. File order interleaves
  every stock, and that costs about 17 more bits.
- CR-4, at most 7.9 bits: missed by every Gorilla variant. The best is the integer as a double per locate, 13.265 and 13.370, a gap
  of 5.4 and 5.5 bits. In file order the best is 22.9 and 23.1. Even the per-locate delta ideal, 9.5 and 9.8, is above 7.9, so
  delta alone will not reach CR-4. The scale and the layout will have to.
- CR-5 is not judged here, because no integer scheme is built. To meet it on add-order prices, the best integer scheme must reach
  35.4 and 35.9 bits in file order, or 24.1 and 24.8 per locate. XOR on the raw integers is already 1.86x smaller than XOR on
  dollars in file order, but it is still Gorilla.
- Other price columns, file order, XOR on dollars, then integer as a double, then raw integer: `U` 55.3, 23.5, 29.7 and 55.5, 23.6,
  29.8; `C` 43.9, 18.2, 23.6 and 43.8, 17.9, 23.3; `P` 50.7, 22.5, 27.4 and 50.1, 21.8, 27.1. The MWCB levels are three values a
  day, too few to mean anything.

**Exactness.** On both days, 0 blocks refused, 0 words changed, and 0 values that failed to map back, in all 20 column and codec
pairs. All 2^32 Price(4) values were sent through each double and back, 4,294,967,296 per representation, with 0 failures. A
Price(8) above 2^53 does not survive a double, and the unit tests show that this is counted and reported with its value.

**Cost of a run.** 56.4 s and 60.5 s per day, the two exhaustive proofs included. Max RSS is 8.73 and 9.51 GB, which is the source
mapped. The peak footprint, what the process held beyond the mapping, is 462 and 459 MB, because the tool keeps one block per
stream and never the day.

**Checks.** Both decoders have libFuzzer targets, and each ran clean for 240 s under `heavy.sh`. `fuzz_gorilla_timestamps` did
732,198 runs and `fuzz_gorilla_xor` 276,557, peaking at 562 and 533 MB. Neither found a crash, so there are no crasher files.
Three injected faults are proven caught: a decoder that accepts a wider bucket than it needs, an encoder that writes a leading
count of 32 into a 5-bit field, and a bit reader without its bounds check, caught by ASan. The whole of `scripts/check.sh` passes
on macOS at `f4d99b8`, all 23 steps, in 454 s, and coverage reaches all 108 required functions.

### Integer codecs on ITCH

CR-3 is met in file order, by patched frame of reference on deltas. CR-4 is met per locate and missed in file order. CR-5 is met
against Gorilla XOR on the dollar doubles on every price column. Against the best Gorilla variant it is met per locate, and only
borderline in file order. The figures are from `results/codec-eval-*.json`, written by `tools/tick_codec_eval` at `7ad720f`,
with the same accounting as the Gorilla section: block payloads, 8-byte block headers and per-stream counts. Per-stream locates
are counted too, 2 bytes each, plus 1 byte for side in the per-locate-and-side column. Every value of every column, codec and day
came back exact. Days are `12302019` then `10302019` throughout.

**What was built** (`include/tick_store/codec/`):

- `int_transform.hpp`: zigzag, delta and delta-of-delta, in unsigned arithmetic that wraps.
- `lane_pack.hpp`: the bit-packed layout. `pfor.hpp`: patched frame of reference. One decoder covers no transform, delta and
  delta-of-delta, an optional scale with remainders, and patched exceptions.
- `stream_vbyte.hpp`: Stream VByte on the low 32 bits, with the high halves patched.
- `patch_list.hpp`: the exception and remainder lists both use.

The decoders check every field and every size before trusting a count, and widths 0 and 64 are explicit. PFOR's encoder chooses
the scale, the mode and the width, so a sequence has more than one valid encoding. The tests therefore pin golden bytes from an
independent Python model of the format, and pin encoded sizes.

**The layout and its cost.** A full vector is 1,024 values in 16 lanes of 64-bit words, FastLanes style. Value i goes to lane
i mod 16, row i / 16. Word k of lane l is stored at index 16k + l. Each row unpacks to 16 consecutive values, so a NEON or AVX2
kernel needs no transpose and no format change. Deltas stay in value order, with no per-lane bases, so decoding needs a prefix sum
across each row. Speed was not measured. A vector shorter than 1,024 values, at the end of a block, is packed linearly and padded
to a byte.

The scalar pack and unpack loops now auto-vectorise. Clang 23 at `-O2` and `-O3` on arm64 had compiled all 16 lanes as scalar
code: it fully unrolled them, with no NEON, because it had to assume the input and output spans overlapped. Each row now loads
its lanes into local arrays, computes there, and stores them whole. The loops then compile to `ushl.2d` and `orr.16b` on paired
128-bit loads. The assembly is kept with the raw logs. The linear tail and the inverse delta stay scalar.

Measured costs:

| Cost | Timestamps, file order (`pfor-delta`) | Timestamps, per locate (`pfor-delta`) | Add prices, file order (`pfor-for-scaled`) | Add prices, per locate (`pfor-delta-scaled`) | Add prices, per locate and side (`pfor-delta-scaled`) |
|---|---|---|---|---|---|
| Lost adaptivity: bits per value saved if each 128 values had its own width | 0.621, 0.568 | 1.474, 1.451 | 0.031, 0.027 | 0.196, 0.214 | 0.262, 0.276 |
| Block-size floor: share of values in vectors under 1,024 | 0.0%, 0.0% | 1.8%, 1.6% | 0.0%, 0.0% | 3.1%, 2.9% | 5.6%, 5.2% |
| Padding: bits per value if those tails were padded to 1,024 | 0.00, 0.00 | 0.42, 0.39 | 0.00, 0.00 | 0.37, 0.33 | 0.58, 0.54 |
| Exceptions patched, share of values | 9.2%, 8.8% | 28.2%, 28.0% | 5.7%, 6.7% | 6.8%, 7.9% | 8.7%, 9.0% |

- The adaptivity yardstick charges each 128-value group a width byte and a 2-byte exception count, against one of each per 1,024
  values. The vector's 3-byte header is not counted on either side.
- The layout pays no padding, because tails are linear. That is not the same as costing nothing: a kernel will need a separate
  tail path. The padding row is what one interleaved layout for every vector would have cost.
- Patching is essential. Without it, timestamps cost 27.16 and 27.06 bits, 8.4 more, and add prices per locate 10.33 and 10.26,
  4.1 and 3.8 more.
- Patch positions are gap coded: a gap width byte, then each position's distance from the one before. On the open slice this saved
  0.26 bits per value on timestamps against 10-bit positions.

**CR-3, timestamps, at most 20.0 bits: met in file order. Delta beats delta-of-delta.**

| Codec | File order | Per locate |
|---|---|---|
| `pfor-delta` | **18.757, 18.691** | 27.745, 27.743 |
| `svb-delta` | 19.889, 19.921 | **25.488, 25.504** |
| `pfor-dod` | 19.982, 19.898 | 31.141, 31.090 |
| `gorilla-dod` | 20.404, 20.418 | 44.476, 44.574 |
| `pfor-delta-unpatched` | 27.159, 27.063 | 36.693, 36.600 |
| `pfor-dod-unpatched` | 27.831, 27.734 | 37.434, 37.334 |
| Exact order-0 entropy of the delta | 16.932, 16.941 | not counted |
| Exact order-0 entropy of the delta-of-delta | 18.344, 18.329 | not counted |

- CR-3 is met by `pfor-delta`, 1.24 and 1.31 bits under 20.0. Stream VByte on deltas and PFOR on delta-of-deltas are also under,
  by 0.02 to 0.11. PFOR delta is 1.65 and 1.73 bits smaller than Gorilla.
- Delta beats delta-of-delta by 1.22 and 1.21 bits under the same PFOR, and the entropy floors agree: delta's order-0 entropy is
  1.41 and 1.39 bits below delta-of-delta's. ITCH timestamps are not regular, so a second difference only adds noise. The census
  said this, and the codecs confirm it.
- `pfor-delta` is 1.83 and 1.75 bits above its order-0 floor. That floor counts every distinct delta with no context: 3,430,981
  and 3,604,584 of them. About 0.6 of the gap is the 1,024-value width.
- Per locate, CR-3 is missed by every codec. The best is Stream VByte, 25.49 and 25.50, a gap of 5.5. A stock's gaps are wide
  and heavy-tailed, so PFOR patches 28% of values. Stream VByte's whole bytes cost less there. Exact entropies were not counted per
  locate, because the distinct deltas would not fit in memory comfortably. The census ideal for delta there is 23.4.

**CR-4, add-order prices, at most 7.9 bits: met per locate, missed in file order.** `A` and `F` prices:

| Codec | File order | Per locate | Per locate and side |
|---|---|---|---|
| `pfor-for-scaled` | **15.254, 15.030** | 7.373, 7.478 | 6.624, 6.745 |
| `pfor-delta-scaled` | 16.229, 16.009 | **6.264, 6.421** | **5.480, 5.617** |
| `pfor-delta-scaled-unpatched` | 18.982, 18.651 | 10.332, 10.262 | 10.076, 10.039 |
| `pfor-for` | 21.889, 21.641 | 13.968, 14.064 | 13.174, 13.302 |
| `svb-delta` | 22.197, 22.311 | 14.334, 14.469 | 13.474, 13.605 |
| `pfor-delta` | 22.800, 22.616 | 12.579, 12.742 | 11.273, 11.494 |
| `gorilla-xor-f64-integer` | 22.925, 23.115 | 13.265, 13.370 | 11.628, 11.848 |
| `pfor-delta-unpatched` | 25.638, 25.447 | 16.871, 16.845 | 16.642, 16.621 |
| `gorilla-xor-u64-integer` | 29.232, 29.506 | 22.976, 23.506 | 20.557, 21.207 |
| `gorilla-xor-f64-dollars` | 54.218, 54.904 | 36.893, 37.881 | 32.746, 33.900 |
| Exact order-0 entropy of the delta | 13.090, 13.062 | 5.123, 5.278 | 4.256, 4.422 |
| Exact order-0 entropy of the value | 13.987, 13.880 | the same | the same |

- The rows are sorted by file order. Per locate, `pfor-delta-scaled` meets CR-4 at 6.264 and 6.421, 1.64 and 1.48 bits under.
  Split by side as well, it reaches 5.480 and 5.617. Side does lower it, by 0.78 and 0.80 bits.
- In file order the best is `pfor-for-scaled`, 15.254 and 15.030, which misses by 7.35 and 7.13 bits. The reason is the ordering.
  File order interleaves about 8,900 stocks, so consecutive add prices belong to different stocks. With the previous value in file
  order as context, the delta's order-0 entropy is 13.09 and 13.06 bits. With no context, the value's own entropy is 13.99 and
  13.88. Both are 5 to 6 bits above 7.9, so no code that sees file-order values or deltas one at a time can reach CR-4. Frame of
  reference without delta beats delta in file order for the same reason: a delta between two stocks is as wide as either price.
- Per locate, `pfor-delta-scaled` is 1.14 bits above the per-locate delta floor of 5.12 and 5.28.
- The adaptive scale finds the factor in each block from the data: 1, the block's common divisor, and the three commonest divisors
  of runs of 8 with their shared divisor, keeping whichever encodes smallest. Nothing is built around 100. In file order every add
  price block took a scale above 1, with remainders on 0.51% and 0.43% of values, the sub-penny prices. Per locate, 98.7% of blocks
  were scaled. Blocks the factor fits nowhere fall back to scale 1. The tests round-trip a factor of 7, partial factors, and scales
  next to 2^64. Without the scale, the same codecs cost 6.6 bits more in file order on both days, and 6.3 more per locate.
- Stub quotes are patched exceptions. In file order 5.7% and 6.7% of add-price offsets are patched.
- The other price columns follow file order, with the best integer codec, then the best Gorilla, which is the integer as a double,
  then Gorilla on dollars. `U` is 15.53, 23.48, 55.29 and 15.30, 23.63, 55.52. `C` is 14.68, 18.17, 43.86 and 14.57, 17.89, 43.79.
  `P` is 16.82, 22.52, 50.74 and 16.42, 21.81, 50.12. The best integer codec is `pfor-for-scaled` on each. The MWCB levels are three
  values a day, too few to judge.

**Per-locate figures exclude the permutation back to file order.** The tool estimates what it would cost:

- The exact order-0 entropy of the locate of each add in file order is 11.05 and 11.08 bits per add. It is 12.04 and 12.07 with
  side, and 11.17 and 11.20 for the locate of every message. The largest locate is 8,906 and 8,886, so a fixed width is 14 bits.
- A store that keeps a file-order locate column pays nothing more. Re-serialising every message needs that column anyway, and a
  file-order layout stores the same column.
- A store that keeps locates only as its partitions must add at least 11.05 bits per add. That makes the per-locate add price 17.3
  and 17.5 bits, worse than file order.
- So CR-4 is met per locate only if the locate column is stored, as the planned schema stores it. The layout experiment in
  TASKS.md decides that, with the column's own cost counted.

**CR-5, best integer scheme at least 1.53x smaller than Gorilla on the same column and ordering.**

| Column | Best integer, bits | Over `gorilla-xor-f64-dollars` | Over the best Gorilla, `gorilla-xor-f64-integer` |
|---|---|---|---|
| Add, file order | 15.254, 15.030 | 3.554x, 3.653x: met | 1.503x, 1.538x: missed by 0.027, then met by 0.008 |
| Add, per locate | 6.264, 6.421 | 5.889x, 5.900x: met | 2.117x, 2.082x: met |
| Add, per locate and side | 5.480, 5.617 | 5.976x, 6.035x: met | 2.122x, 2.109x: met |
| `U`, file order | 15.534, 15.300 | 3.559x, 3.629x: met | 1.511x, 1.545x: missed by 0.019, then met |
| `C`, file order | 14.684, 14.567 | 2.987x, 3.006x: met | 1.237x, 1.228x: missed by 0.29 and 0.30 |
| `P`, file order | 16.816, 16.415 | 3.017x, 3.053x: met | 1.339x, 1.329x: missed by 0.19 and 0.20 |

- Against XOR on the dollar doubles, which is what a system storing prices as doubles would run, integers win by 3 to 6 times.
- Against Gorilla on the integer as a double, the scaled integer codec wins by 2.1 per locate. In file order it wins by 1.5,
  on the threshold. File order leaves little for either side to exploit.

**Exactness and cost of a run.** 0 blocks refused, 0 words changed and 0 representation failures, in all 82 column and codec pairs
on both days. All 2^32 Price(4) values came back through each double. Each run took 206.7 and 225.2 s, against 56.4 and 60.5 s
before. The difference is the PFOR encoder trying each candidate scale. Max RSS was 9.54 and 10.33 GB, which is the source
mapped. The peak footprint was 1.36 and 1.27 GB, up from 462 and 459 MB, because of the exact-entropy tables: 16 bytes a slot,
at most half full, over 3.4 million distinct timestamp deltas and 5.8 million delta-of-deltas. Raw `/usr/bin/time -l` output is
kept with the raw logs.

**Checks.**

- `fuzz_pfor` and `fuzz_stream_vbyte` ran clean for 240 s each under `heavy.sh`: 23,681 and 74,662 runs, with 1.03 GB peak RSS
  for the two. Neither found a crash.
- PFOR's encoder makes choices, so the oracle cannot demand byte equality. It checks values through every encoder instead, all
  twelve PFOR option sets on columns and as values, and the four of a block's own transform on each accepted block. It also checks
  the encoder is deterministic. For Stream VByte, whose only open choice is zigzag, it demands the exact bytes when the encoder
  makes the same choice.
- The PFOR oracle still cannot see a field the decoder wrongly accepts without changing values, such as non-zero tail padding.
  The unit tests' corrupt fixtures, one per validated field, are the check for those.
- `fuzz_pfor` runs at about 100 runs per second under ASan, against Stream VByte's 300 to 400, because each input goes through
  twelve encoders. Its `-max_len` is 16,384 for that reason. Inputs of 70,000 bytes halved its rate, and 16,384 bytes still holds
  a full vector at width 64.
- Two faults are proven caught:
  - A PFOR decoder that zigzag-decodes before adding the base. `check_pfor_column` caught it after 614 runs: "a re-encoded column
    did not decode to the same values".
  - A Stream VByte decoder that accepts an over-long length. It was caught after 121 runs: "a block re-encoded with its own flags
    differs from the input".
- CI's job timeout rose from 180 to 300 minutes. The scheduled fuzz job runs eight targets for 30 minutes each, and six already
  filled 180.
- The whole of `scripts/check.sh` passes on macOS at `203d9dc`, all 25 steps, in 572 s under `heavy.sh`, peaking at 1.03 GB
  resident. Coverage reaches all 126 required functions, now including the PFOR, Stream VByte, lane-packing and patch-list
  decoders and encoders and the new oracles, with lines at 93.9% and functions at 98.1%.

### Every field: references, shares and the rest

CR-6 is met for the references that create orders, and for the references that name an order once each is stored as a
distance within its stock. CR-7 is met for add and replace shares and missed for execute and cancel shares, by 0.11 and 0.19
bits at best, above a floor no context-free code can pass. The whole store is estimated at 5.50 and 5.49 times smaller than
the source with the codecs alone, and 5.98 and 5.96 times with zstd level 1 where it pays: CR-1 and CR-2 are met with room.
The figures are from `results/codec-eval-*.json`, written by `tools/tick_codec_eval` at `0d55bfe`, and from
`results/store-estimate.json`, written by `scripts/store_estimate.py`. Every value of every column, codec and zstd frame came
back exact on both days, and both 2^32 Price(4) proofs had 0 failures. Days are `12302019` then `10302019`, as before.

**What was built.**

- Head delta in PFOR (`c5b0e6a`): each value against one of its seven most recently used heads, a 3-bit token per value and
  token 7 an escape to a plain value. It falls back to delta where it cannot code.
- A dictionary codec (`dictionary.hpp`). A block's distinct values once, then each value as an index. Three modes, the smallest
  kept: a dictionary ranked by use with PFOR indices, so the common values take few bits and PFOR patches the rest as
  exceptions; the same with canonical Huffman indices; and a sorted dictionary with move-to-front ranks, for up to 2,048
  entries. The dictionary's order is part of the format and every entry must be used, so each set of values has one dictionary.
- A run-length codec (`run_length.hpp`): run values as one PFOR block, plain or delta, and run lengths as another. Runs must be
  maximal.
- `tick_codec_eval` now covers every field, with 54 columns, Stream VByte on plain values, zstd 1.5.7 at level 1 through a
  pinned FetchContent tarball and hash, and a pass that keeps the order book by reference, so it can measure distances back to
  each order's creation and check what a field could be derived from. `--columns` runs a subset; the committed results are each
  one full run.

**What `A`'s two modes of delta follow: about four interleaved counters, not the stock, side or participant.** The census found
a previous-value delta that is either small or very large. With seven heads, 46.3% and 45.7% of add references are within
reach of the most recent head, and 99.9% of a head of the seven:

| Head delta token | 0 | 1 | 2 | 3 | 4 | 5 | 6 | Escape |
|---|---|---|---|---|---|---|---|---|
| Add references, `12302019` | 54,870,023 | 26,630,228 | 20,048,508 | 16,746,218 | 63,550 | 66,940 | 95,588 | 110,401 |
| Add references, `10302019` | 58,732,562 | 29,078,870 | 22,069,675 | 18,446,426 | 58,029 | 62,881 | 89,490 | 100,624 |

- Tokens 0 to 3 take 99.7% of values, and tokens 4 to 6 fewer than 0.2%. So there are about four live sequences at a time.
  The token's entropy is 1.86 and 1.87 bits, the gap's after it 2.47 and 2.41, and the two together 4.11 and 4.09.
- Plain deltas step in multiples of 4: +4 for 38.6 and 41.2 million adds, +8 for 9.8 and 10.3 million, +12 for 3.3 and 3.6
  million, +16 for 1.3 and 1.5 million. Every delta between -16 and +16 that is not a multiple of 4 occurs fewer than 10 times.
  The other 55% are outside ±16, about equally above and below: that is the jump between counters.
- The sequences barely follow anything a message carries. When a head is reused, its previous owner had the same locate 36.0%
  and 34.9% of the time, against 26.7% and 25.6% for consecutive adds. Same side 62.5% and 62.2%, against 60.8% and 60.4%. Same
  participant, among attributed adds, 66.6% and 66.7%, against 63.3% and 62.9%. Each is within 10 points of the baseline.
- So references come from a few counters that advance in steps of 4, interleaved in file order by something not in the
  message. Head delta follows them without knowing what they are.

**CR-6, order references, at most 11.2 bits per value: met for creations, and for the rest as per-stock distances.**

| Column | Best codec | Bits per value | With zstd | CR-6 |
|---|---|---|---|---|
| `A` references, file order | `pfor-head-delta` | 8.465, 8.371 | 5.847, 5.804 | met |
| `A`, `F` and `U` new references together | `pfor-head-delta` | **7.797, 7.641** | 5.420, 5.332 | met |
| `U` new reference alone | `pfor-head-delta` | 11.207, 11.027 | 8.487, 8.330 | missed by 0.007, then met |
| `A` references per locate | `svb-delta` | 14.053, 14.078 | 10.813, 10.884 | missed |
| `A` reference less the running maximum | `dict` | 11.500, 11.519 | 10.171, 10.168 | missed by 0.30 and 0.32 |
| `U` new less original | `rle` | 17.656, 17.657 | 15.963, 16.054 | missed |
| References of `E`, `C`, `X`, `D` and `U` original, as values | `svb-delta` | 25.680, 23.994 | 24.090, 22.924 | missed |
| The same, one stream per type | `svb-delta` | 25.729, 23.959 | 24.084, 22.845 | missed |

- Against the running maximum, the best codec is 3.0 and 3.1 bits worse than head delta. Plain delta, patched, is 23.95 and
  Stream VByte on delta 21.41 on `12302019`. Patched outliers are in every PFOR figure; unpatched delta is 26.47.
- `U`'s new reference is a creation, and coded in the creation stream it costs what an add does. On its own it misses by
  0.007 bits on `12302019` and meets on `10302019` by 0.17. Coding it against the original costs 17.7.
- A reference that names an order is better stored as how far back its order was created, counting creations (`A`, `F`, `U`
  new). Counting all creations, that misses. Counting only creations in the order's stock, it meets, for every type on both
  days:

| Type | Messages | Distance in all creations | Its order-0 entropy | Distance in the stock's creations (`dict`) | Its order-0 entropy |
|---|---|---|---|---|---|
| `E` | 5,722,824, 7,316,703 | 20.704, 20.080 | 17.677, 17.344 | **9.217, 8.732** | 7.608, 7.344 |
| `C` | 99,917, 129,729 | 15.587, 14.954 | 12.066, 11.567 | **6.110, 6.023** | 5.624, 5.507 |
| `X` | 2,787,676, 3,568,735 | 17.664, 17.335 | 14.580, 14.516 | **7.068, 7.442** | 6.049, 6.370 |
| `D` | 114,360,997, 123,296,742 | 18.303, 18.355 | 15.870, 15.914 | **7.478, 7.543** | 6.371, 6.425 |
| `U` original | 21,639,067, 25,513,651 | 17.607, 17.783 | 15.300, 15.517 | **6.755, 6.806** | 5.987, 6.044 |

- The per-stock distance needs the message's locate to find the stream, so it holds only with the locate column stored. The
  store keeps that column anyway, since every message carries a locate.
- Every reference resolved: 0 messages named an order not on the book, on either day, and 0 had a locate other than its order's.
  The book peaked at 1,924,078 and 1,824,830 live orders and was empty at the end of both days, with 0 duplicate creations.
- A decoder rebuilds the reference from the distance by keeping each stock's creations, not the book: the order it names is
  the one created that many creations back in the same stock.

**Locality of referencing messages.** The share of each type whose order was created in the same block of creations, or the
same aligned window of time:

| Type | Same 1,024 creations | Same 4,096 | Same 65,536 | Same 1 ms | Same 1 s | Same 60 s |
|---|---|---|---|---|---|---|
| `E` | 15.6%, 16.8% | 19.1%, 20.6% | 37.8%, 41.3% | 7.1%, 8.0% | 21.4%, 23.6% | 61.5%, 67.4% |
| `C` | 45.0%, 50.3% | 54.7%, 60.1% | 79.5%, 83.1% | 18.6%, 22.9% | 60.4%, 67.0% | 90.0%, 92.5% |
| `X` | 29.4%, 31.0% | 46.2%, 45.2% | 73.7%, 73.3% | 3.1%, 3.9% | 45.5%, 42.1% | 83.9%, 84.5% |
| `D` | 24.5%, 24.6% | 33.0%, 32.6% | 63.7%, 62.5% | 8.7%, 9.0% | 36.3%, 36.4% | 82.3%, 82.4% |
| `U` original | 32.1%, 30.7% | 41.1%, 39.4% | 65.7%, 64.6% | 10.6%, 10.2% | 45.1%, 44.3% | 82.1%, 82.0% |

- Most referencing messages fall outside the block their order was created in, at any block size a store would use. A store
  cannot keep an order's life in one block, so a reference must be resolvable across blocks, as the per-stock distance is.
- Executions are the least local: 61.5% and 67.4% within the same minute, against 82% for deletes.

**CR-7, shares, at most 3.8 bits per value: met for adds and replaces, missed for executions and cancels.**

| Column | Best codec | Bits per value | With zstd | Order-0 entropy | CR-7 |
|---|---|---|---|---|---|
| `A` and `F` shares, file order | `dict` | **3.687, 3.643** | 3.680, 3.633 | 3.149, 3.095 | met |
| `A` and `F` shares, per locate | `dict` | **2.614, 2.549** | 2.550, 2.481 | same values | met |
| `U` shares | `dict` | **2.570, 2.655** | 2.433, 2.533 | 2.140, 2.220 | met |
| `E` executed shares | `dict` | 6.077, 6.007 | 6.079, 6.008 | 5.362, 5.307 | missed |
| `C` executed shares | `dict` | 4.842, 4.541 | 4.839, 4.539 | 4.187, 3.982 | missed |
| `X` cancelled shares | `dict` | 5.275, 5.324 | 5.271, 5.319 | 4.589, 4.616 | missed |
| `E`, `C` and `X` shares as 0 when all remaining shares, else shares + 1 | `dict` | 3.907, 3.992 | 3.895, 3.989 | 3.886, 3.987 | missed by 0.107 and 0.192 |
| `P` shares | `dict` | 5.711, 5.383 | 5.712, 5.384 | 5.005, 4.723 | missed |
| `Q` shares | `pfor-for` | 7.758, 7.717 | 6.633, 6.463 | 4.508, 4.440 | missed |
| `I` paired shares | `pfor-for` | 5.068, 5.031 | 4.366, 4.292 | 3.298, 3.223 | missed |

- On add shares the dictionary beats run-length (5.56), the scaled frame of reference (5.60) and plain bit-packing by PFOR
  (10.22) on `12302019`. Every shares column picks the dictionary except `Q` and `I`, where frame of reference is smaller.
- An execution of all the order's remaining shares is common: 73.6% and 71.9% of `E`, 60.9% and 64.6% of `C`. No `X` cancels
  all of them, since that is a `D`. Coding that case as 0 brings the three to 3.91 and 3.99 bits. Their order-0 entropy is 3.89
  and 3.99, so the miss is not the codec: a code that sees each value with no context cannot reach 3.8 on these values.
  Context the book holds, such as the remaining shares as an upper bound, is the next thing to try. It is in TASKS.md.

**The other fields.** Each field a day needs to be re-serialised exactly, with its best codec, its bits per value, and what
the result depends on:

| Field | Best column and codec | Bits per value | With zstd | Depends on |
|---|---|---|---|---|
| Message type | `dict` | 1.799, 1.824 | 1.776, 1.808 | Order-0 entropy 1.708 and 1.736; order 1 is 1.561 and 1.590, so the previous type would save 0.15 |
| Stock locate, every message | `dict` | 9.284, 9.306 | 8.343, 8.381 | Order-0 entropy 11.17. Recency rank's entropy is 6.76 and 6.80: a stock just used is likely next |
| Stock locate, only messages that name no order | `dict` | 11.365, 11.434 | 10.151, 10.196 | Costs more per value and loses: referencing messages then need their locate from the book |
| Tracking number | per type, `dict` | 0.156, 0.163 | 0.147, 0.154 | One stream per type; in file order it costs 0.402 and 0.450 |
| `A` and `F` side | `dict` | 1.066, 1.066 | 1.065, 1.065 | One bit of entropy; per locate it is no better, 1.080 and 1.077 |
| `F` attribution | `dict` | 2.941, 3.122 | 2.850, 3.037 | Below its order-0 entropy of 4.60 and 4.84: a participant repeats within a block |
| Match number | `pfor-delta` | 0.499, 0.357 | 0.152, 0.132 | Its delta's order-0 entropy is 0.039 on `12302019`: it rises by the same step almost always |
| `C` printable | `rle` | 0.557, 0.399 | 0.498, 0.369 | Long runs of one value |
| `P` reference and side | `pfor-delta`, `dict` | 0.061, 0.061 | 0.037, raw zstd | One value each all day: order-0 entropy 0 |
| `Q` price, shares, cross type | `pfor-for-scaled`, `pfor-for`, `dict` | 7.012, 7.758, 0.091 | 6.363, 6.633, 0.092 | 17,836 and 17,775 crosses: small enough that the choice hardly matters |
| `I` prices, flags | `pfor-delta-scaled`, `dict` | 5.194, 2.239 | 4.466, 1.219 raw | Three prices per message; zstd on the raw flag bytes halves them |
| `S`, `R`, `H`, `Y`, `L`, `W`, `K`, `J`, `h`, `N`, `O` payloads | bytes, `dict` | 3.930, 3.928 | 2.729, 2.728 raw | 3.7 million bytes a day; zstd wins on the raw bytes |
| `V` levels | `gorilla-xor-f64-integer` | 93.3, 90.7 | the same | Three values a day |

- Timestamps and the price columns are as in "Integer codecs on ITCH".
- Locate: move-to-front over the 8,907 and 8,887 locates in use would cost at least its recency entropy, 6.76 and 6.80 bits,
  2.5 below the dictionary. No codec for it was built: the dictionary's move-to-front mode stops at 2,048 entries, and a
  recency rank over 8,900 entries needs a Fenwick tree or similar per value. It is in TASKS.md, with its decode cost unknown.
- Stock symbols in `A`, `F`, `P`, `Q` and `I` all agree with the stock directory's symbol for their locate, so they are derived
  and not stored. On `12302019`, 3 `K` messages on locate 0 have no directory entry; `K` is stored whole with the other rare
  types. Lengths are derived from the type: no message on either day had another length. The estimate charges an exception
  for any of these that fails; none did.

**zstd: worth it on three columns, not as a layer over everything.** zstd 1.5.7 at level 1 was run on every encoded block
and on every raw block of every column, and each frame was decompressed and compared with the input. Taken wherever it is
smaller, in the mixed layout below, it saves 121.6 and 128.4 MB, 8.1% and 7.8% of the store. Most of that is a few columns:

| Column | Saved by zstd on the encoded blocks | Share of the column | zstd decompress time over the codec's decode time |
|---|---|---|---|
| Creation references (`pfor-head-delta`) | 41.7, 44.5 MB | 30.5%, 30.2% | 0.16, 0.16 |
| Stock locate (`dict`) | 31.6, 34.0 MB | 10.1%, 9.9% | 0.05, 0.05 |
| Add prices per locate and side (`pfor-delta-scaled`) | 15.8, 16.8 MB | 19.5%, 18.6% | 0.54, 0.54 |
| Timestamps (`pfor-delta`) | 13.8, 13.8 MB | 2.2%, 2.0% | 1.09, 1.05 |
| Per-stock distances, `D` (on plain Stream VByte, against `dict` alone) | 7.6, 8.1 MB | 7.1%, 7.0% | 0.92, 0.95, over Stream VByte's decode |
| Add prices, file order (on `pfor-delta-scaled`, against `pfor-for-scaled` alone) | 12.2, 10.0 MB | 5.4%, 4.1% | 1.04, 0.99 |

- The times are relative, from the same run, and are not reported as speeds. A reader decoding a zstd block pays the
  decompression on top of the codec's decode: 1.05 to 2.09 times the codec's time alone on the rows above.
- On shares, side and type, zstd saves under 1.5%: 0.2% on add shares in file order, 1.3% and 0.8% on message types.
- Raw values under zstd lose to the codecs on every large column but one: 33.8 bits on timestamps, 14.4 on creation
  references, 11.5 on locate. The exception is near-constant columns: per-type tracking numbers, 0.119 against the dictionary's
  0.156, where zstd's run matching wins. It also wins on the small byte columns, `I` flags and the rare payloads.
- So the store should apply zstd level 1 to the blocks of three columns: creation references, stock locate and per-locate add
  prices. They save 89.1 and 95.3 MB a day, at 0.05 to 0.54 times their decode time. Timestamps are left without it: 2% for
  twice the decode time. With zstd on those three only, the mixed layout comes to about 1,412.5 and 1,549.0 MB, ratios of
  about 5.84 and 5.83, by subtracting their savings from the codecs-alone estimate.

**The whole store, estimated.** `scripts/store_estimate.py` adds the best exact codec for each field, block headers and
counts included, an index entry of 24 bytes per block of each column, and the exceptions a derived field would need. It is an
estimate, not a store: no store file exists, the index size is a stated guess, and a file format will add a header and its
own framing. Blocks are 4,096 values; the 1,024 granularity was not run.

| Layout | Codecs alone | Ratio to the source | With zstd where it pays | Ratio |
|---|---|---|---|---|
| (a) file order, per-type columns, locate stored | 1,662,011,815, 1,812,775,943 | 4.965, 4.984 | 1,544,822,619, 1,692,076,518 | 5.341, 5.340 |
| (b) per locate, file-order locate stored | 1,830,342,637, 2,009,558,058 | 4.508, 4.496 | 1,688,026,813, 1,857,270,328 | 4.888, 4.865 |
| mixed: each column in its smaller order | **1,501,612,644, 1,644,285,347** | **5.495, 5.495** | **1,380,003,932, 1,515,912,474** | **5.979, 5.960** |

- **CR-1, at least 2.3415 on `12302019`: met by every layout.** The weakest, per locate with codecs alone, is 4.508, 1.93 times
  the floor. `10302019`'s gzip ratio is 2.333, and every layout there is at least 4.496.
- **CR-2, smaller than Parquet, ArcticDB and QuestDB on `12302019`: met by every layout.** The mixed layout with zstd is 0.432
  of Parquet's 3,191,406,592 bytes, 0.217 of ArcticDB's 6,352,666,624 and 0.060 of QuestDB's 22,897,786,880. The weakest,
  per locate with codecs alone, is 0.574 of Parquet.
- Per locate loses to file order by 168 and 197 MB because timestamps per locate cost 25.49 bits against 18.76 in file
  order, 226 MB a day. It wins on add prices by 145 MB and on add shares by 16 MB, and loses on add references by 102 MB.
- The mixed layout keeps timestamps and creation references in file order and add prices and shares per locate. Side is
  smaller in file order with the codecs alone and per locate with zstd, by under 0.4 MB either way. A reader
  follows the stored locate column to know which stream holds a row's next value, so columns need not share an order.
- Index entries are 10.6 to 12.5 MB a day, under 1% of any layout. 0 exceptions were charged.
- **Recommendation: the mixed layout, with zstd on the three columns above.** File order for timestamps, creation references
  and every column of rare types; add prices and shares per locate; referencing messages' references as distances within
  their stock; and the file-order locate column stored. If one order for every column is required, file order: with the
  codecs alone it is 10.7% larger than mixed on `12302019`, and per locate 21.9% larger.

**Exactness and cost of a run.** 0 blocks refused, 0 values changed and 0 zstd frames differing, over 54 columns on both days.
Each run took 947.3 and 950.2 s, 4.6 and 4.2 times the previous 206.7 and 225.2 s. Peak resident size was 14.1 and 14.9 GB, the source
mapping included; the peak footprint beyond that was 6.01 and 5.76 GB, against 1.36 and 1.27 GB. The growth comes with the 45 new columns,
each held whole for the day, and the book pass; which part dominates was not measured. `--columns` runs only
the columns named, for exploring.

**Checks.**

- `fuzz_dictionary` and `fuzz_run_length` are new, with seeds from the real encoders on short, repeated, low-cardinality and
  wide inputs, and a hand-built block for each refusal the decoders make. Their oracles decode, re-encode with every mode and
  check values, and also require a dictionary's entry count to equal the block's distinct values and a run-length block's run
  count to equal its maximal runs, since a decoder that accepts a repeated entry or a split run would otherwise pass.
- They are registered in `scripts/check.sh`, CI's scheduled fuzz matrix, whose timeout rose from 300 to 360 minutes for ten
  targets, and the coverage list, which gains 14 required functions.
- 240 s each under `heavy.sh`: `fuzz_dictionary` 15,519 runs, `fuzz_pfor` 19,753 and `fuzz_run_length` 57,493, with no crash,
  1.37 GB peak resident for the three.
- Three faults are proven caught, one per codec family, by `scripts/prove_checks.py --only` in throwaway clones. Each was
  caught while libFuzzer was still running the seed corpus, so the real-encoder and hand-built seeds alone find them:
  - A head-delta inverse that overwrites the front head on an escape instead of pushing the heads down. `check_pfor_column`:
    "a re-encoded column did not decode to the same values".
  - A dictionary decoder that accepts a repeated entry. `check_dict_column`: "a dictionary block's entry count is not its count
    of distinct values".
  - A run-length decoder that accepts two neighbouring runs of one value. `check_rle_column`: "a run-length block's run count is
    not its count of maximal runs".
- The seed generator first rejected a refusal seed that the faulty decoder accepted, which stopped the build before the fuzzer
  ran. It now checks only the seeds meant to be valid; the unit fixtures already check each refusal (`358d571`).
- `tick_codec_eval`'s new tests hold the book columns to hand-computed values. Seven injected faults in the tool, and eight in
  `store_estimate.py`, each failed a test.
- The whole of `scripts/check.sh` passes on macOS at `e61dbce`, all 27 steps, in 804 s under `heavy.sh`, peaking at 1.24 GB
  resident. Coverage reaches all 140 required functions, now including the dictionary and run-length decoders, encoders and
  oracles, with lines at 93.95% and functions at 98.03%. One Python test is skipped by design on macOS: it checks the pinned
  path, which exists only on Linux.

### The container format, its writer and its reader

`docs/format.md` has the byte layout. The trade-offs, and what did not work:

- **Block metadata lives in the footer, not beside each payload.** A scan of every block's zone map then reads the footer
  alone and never touches a payload page. The cost is that a block's metadata and payload are in two places, so the
  footer grows with the store's metadata. That is about 16 bytes a block for a first-and-last-time zone map.
- **Blocks and sections tile the data region exactly, in file order.** Open makes one merge walk over the two lists and
  proves there is no gap, no overlap and no unclaimed byte. That leaves one encoding for any set of parts, which is what
  lets the fuzzer's oracle rebuild an accepted file through the writer's own encoding and compare it byte for byte.
- **No padding or alignment.** Payloads start wherever the last part ended, and decoders load with `memcpy`. A kernel
  that wants aligned loads has to copy first, or wait for a version 2.
- **A block's CRC32C covers its codec id as well as its payload.** So a block relabelled with another codec fails its
  CRC, and is not decoded as the wrong format.
- **CRCs are checked at lookup, not at open.** Opening a file reads its two ends: header, trailer and footer, with the
  footer's own CRC. `verify_all()` checks every block and section. The price is that a caller reading a payload by
  offset, instead of through the reader, gets no check (TASKS.md).
- **The reader's views hold the mapping.** Each one carries a `shared_ptr`, so a lookup costs an atomic increment. In
  return, a view that outlives its reader stays valid, where a borrowed span would fault after `munmap`.
- **One dependent chain of crc32 instructions.** Three interleaved streams would run about three times faster on long
  payloads. Verification has not yet been measured against decode, so this is not done (TASKS.md).
- **Durability.** `device_flush` uses `F_FULLFSYNC` on macOS, because `fsync` there does not flush the drive's cache. On
  Linux it calls `fdatasync` on the file and `fsync` on the directory. A failed flush is returned once and never
  retried, following PostgreSQL's fsyncgate ruling. Flush granularity is one flush per file, at commit.

What did not work:

- **"Every cut of a valid file is refused" is false.** A section can hold a whole container's bytes at just the right
  offset, and cutting the file there leaves that container, which is valid. The fuzzer's first oracle failed on such an
  input. It now requires a cut to be refused, or valid in full. The unit tests still cut the golden file at every
  length and require each cut to be refused, since that file has no nested container.
- **A footer-offset bound loosened by one is a poor fuzz fault.** The bound is loose by one, but the footer must still be
  exactly `24 + 32 B + 64 S + M` bytes, so the metadata check refuses every file that gets past it. The bound tightened
  by one is a fault a unit test catches (`container-footer-offset-bound`), and the fuzz fault starts the metadata area
  one byte late instead (`fuzz-container-metadata-start`).
- **gtest's "threadsafe" death tests could not simulate a crash.** They re-run the test in a new process, which makes a
  new scratch directory, so the crashed child's files were never where the parent looked. The torn-write tests use the
  default fork style, and end the child with `_Exit(0)` at each system call.
- **Two existing tools were broken.** `scripts/prove_checks.py` never copied the zstd sources into its clone, so every
  dev build there had failed since zstd became a dependency; `68a6223` fixes it. And the Linux dev build had failed since
  `7771c35`: a test's initializer list mixed `std::uint64_t` with `unsigned long long`, which are one type on macOS and
  two types on Linux. `4dcea24` fixes it.

**Checks.**

- Five faults are proven caught by `scripts/prove_checks.py --only` in throwaway clones:
  - `container-block-crc-skipped` fails four `ContainerBlock` tests.
  - `container-footer-offset-bound` fails `ContainerOpen.AcceptsTheSmallestFileAndEmptyParts` and nothing else. The
    golden file's cuts do not catch it.
  - `container-directory-sync-dropped` fails four `ContainerCommit` tests.
  - `crc32c-armv8-last-byte` fails four `Crc32c` tests.
  - `fuzz-container-metadata-start` is caught by `check_container` after 18 executions, in the seed corpus, with "an
    accepted file has extents the walk cannot follow".
- `fuzz_container` ran clean for 240 s under `heavy.sh` at `4dcea24`: 27,722,076 runs, 890 MB peak resident against
  its 2,048 MB cap, with inputs up to 70,000 bytes. Its 52 seeds come from the real writer.
- On `debian`, at `4dcea24`, the dev build ran through `heavy.sh` with `taskset -c 2,6,7`. All 141 format, CRC32C,
  durability and fuzz-regression tests pass, and so do all 516 unit tests other than the real-day ones. That covers the
  SSE4.2 kernel, picked by CPUID and matched against software at every offset, and the Linux path:
  `fdatasync` on the file, `fsync` on the directory, and a real flush. With `TICK_STORE_CRC32C=software`, the test
  requiring the hardware kernel fails, as it should, and the other six CRC32C tests pass.
- The whole of `scripts/check.sh` passes on macOS at `4dcea24`, all 28 steps, in 916 s under `heavy.sh`, peaking at
  1.24 GB resident. That includes 519 tests under dev, ASan with UBSan, and TSan. Coverage reaches all 169 required
  functions, with lines at 93.94% and functions at 98.14%. clang-tidy cannot analyse the SSE4.2 kernel on the Mac.
  It compiles there for x86-64, and its tests run on `debian`.

### The ITCH store

`docs/store.md` has the layout as built. In short: a segment is N consecutive records, N = 2^20 by
default; each segment is one container block per column, 33 columns, the `type` block's footer
metadata carrying the segment's summary (first record, first, last, lowest and highest timestamp,
typed records, a locate bitmap); three sections hold the store header, the symbol table and the
tail. Each segment decodes from its own blocks and the symbol table alone.

**What a segment needs to decode on its own.** Everything but the orders its messages name that
were created before it. Those it carries as its import list: a reference that names an order is a
distance back in that locate's list of the segment's creations and imports, and distance 0 takes the
next value from the import list. So the only per-segment state is the import list, and every byte
of it is in the file (the `import_ref` column). There is no book checkpoint.

**The reference encoding, chosen on the numbers.** `tick_reference_eval` measured every candidate on
12302019 with the real codecs, block by block, counting each one's state as well as its column
(144,610,481 references; raw output `phase3-raw/store/iteration/reference-eval-12302019.json`,
from the uncommitted tree on `cc93641`, whose writer and evaluator are the committed ones). Bytes
per day, column + state:

| N | within stock, imports (built) | all creations, imports | values, PFOR delta | values, head delta | values, Stream VByte | day distance + book checkpoint | day distance + creation index |
|---|---|---|---|---|---|---|---|
| 2^16 | 69.9 + 206.7 = 276.7 MB | 414.6 MB | 470.8 MB | 494.4 MB | 464.2 MB | not run | 134.7 + 67.3 = 202.0 MB |
| 2^18 | 88.2 + 130.7 = 218.9 MB | 416.6 MB | 470.8 MB | 494.3 MB | 464.2 MB | not run | 134.4 + 16.8 = 151.3 MB |
| 2^20 | 103.7 + 70.3 = 173.9 MB | 423.1 MB | 470.8 MB | 494.3 MB | 464.2 MB | 134.3 + 2,015.7 = 2,150.0 MB | 134.3 + 4.2 = 138.5 MB |
| 2^22 | 115.0 + 35.3 = 150.2 MB | 428.9 MB | 470.8 MB | 494.3 MB | 464.2 MB | 134.3 + 504.0 = 638.3 MB | 134.3 + 1.1 = 135.4 MB |
| 2^24 | 122.6 + 17.2 = 139.9 MB | 432.6 MB | 470.8 MB | 494.3 MB | 464.2 MB | 134.3 + 122.8 = 257.1 MB | 134.3 + 0.3 = 134.6 MB |

- Within-stock distance with a per-segment import list saves 296.9 MB a day against the cheapest
  stateless encoding at 2^20 (173.9 against 470.8 MB for PFOR on deltas), so it is the one built.
  The import list is 70.3 MB of that 173.9, 21.1 million imports, and the store's own
  `ref_distance` and `import_ref` columns come to exactly the evaluator's bytes.
- **A book checkpoint eats the saving.** Distances over the whole day are 134.3 MB, but a segment
  that decodes on its own then needs every order live at its start, 1.9 million at the peak: 2,016 MB
  of checkpoints at 2^20, fifteen times the column, and still 123 MB at 2^24. This is the finding the
  brief asked for: a checkpoint of the book costs more than the distance saves, at every segment
  size measured.
- Day-wide distances with only a per-locate creation count per segment cost 138.5 MB, 35.4 MB (2.4%
  of the file) less than what is built, but a segment is then no longer decodable on its own: an
  order created in segment j and named in segment k needs segment j decoded, and in the worst case
  every segment back to the first. That fails the random-access requirement, so it is not built.
- One list over all creations, rather than one per locate, costs 423 MB: the locate is what makes
  the distances short.

**The segment size, chosen on the numbers.** The real store at four sizes, 12302019, same writer
(raw `phase3-raw/store/iteration/ingest-12302019-n*.json`, `layout-12302019-n*.json` and the
`.time` files, `/usr/bin/time -l`):

| N | Segments | Stored bytes | Ratio | References + imports | add_price | Footer | Writer footprint |
|---|---|---|---|---|---|---|---|
| 2^16 | 4,101 | 1,594,523,642 | 5.175 | 276.7 MB | 96.7 MB | 8.78 MB | 40 MB |
| 2^18 | 1,026 | 1,508,225,974 | 5.471 | 218.9 MB | 80.6 MB | 2.21 MB | 136 MB |
| 2^20 | 257 | 1,449,464,212 | 5.693 | 173.9 MB | 71.7 MB | 0.56 MB | 283 MB |
| 2^22 | 65 | 1,417,650,455 | 5.821 | 150.2 MB | 67.4 MB | 0.15 MB | 819 MB |

2^20 is the default. Going to 2^22 would save 31.8 MB (2.2%) at nearly three times the writer's
memory and four times the work to decode one random segment; 2^18 would cost 58.8 MB (4.1%) more.
Most of the difference is the import list and the per-locate price runs, which both restart at each
segment.

**Full days on the store as built.** Each day was ingested at N = 2^20 with `page_cache`
durability, then G-1 and G-2 were run, one after another, through `heavy.sh` under
`/usr/bin/time -l`, using the bench build at `f3e9d84`. The results are in
`results/roundtrip-*.json` and `results/store-size-*.json`, assembled by `scripts/store_results.py`.
Raw output is in `phase3-raw/store/<day>/`. The wall times are from runs during iteration.

| Day | Records | Stored bytes | Ratio | CR-1 (≥ 2.3415) | Estimate | Gap |
|---|---|---|---|---|---|---|
| 12302019 | 268,744,780 | 1,449,464,212 | 5.693 | pass | 1,380,003,932 (5.979) | +69.5 MB, 5.0% |
| 10302019 | 293,989,079 | 1,588,982,528 | 5.686 | not a CR-1 day | 1,515,912,474 (5.960) | +73.1 MB, 4.8% |

The parts of 12302019, which add up to the file:
- column payloads: 1,448,813,003 bytes;
- footer: 562,071 bytes, made of 191,808 bytes of block entries, 370,047 of block metadata (the segment summaries included), 192 of section entries and 24 fixed;
- symbols section: 89,074 bytes;
- store section: 24 bytes;
- header and trailer: 40 bytes.

The per-segment state is the import list: 70,275,493 bytes holding 21,140,903 imports, 4.8% of the file. On 10302019 it is 75,876,740 bytes for 23,562,881 imports. No other checkpoint exists.

**CR-2: pass on 12302019.** It is judged against the debian full-day stores in
`results/fullday-capacity-12302019.json`:

| System | Variant | Its stored bytes | Ours as a fraction of it |
|---|---|---|---|
| Parquet | zstd1-rg1Mi | 3,191,406,592 | 0.454 |
| ArcticDB | rows100k-cols127 | 6,352,666,624 | 0.228 |
| QuestDB | plain | 22,897,786,880 | 0.063 |

The ITCH store's bytes depend only on the records: debian, at `4302ce8`, wrote byte-for-byte the store the Mac wrote at `f3e9d84` (SHA-256 `2122a277…`). No capacity file exists for 10302019, so CR-2 there is "no comparison". It is not estimated.

**Ratio against the estimate.** Against `results/store-estimate.json` (the mixed layout, with zstd), the gap is 69.5 MB on 12302019 and 73.1 MB on 10302019. Two parts net out to that.

The column payloads are 80.0 MB and 84.6 MB larger than the estimate's. The store saves 10.6 MB and 11.6 MB on its index, because the estimate charged 24 bytes for every block of 4,096 values, while the store's whole footer and sections come to 0.65 MB and 0.68 MB.

By field, on 12302019 (10302019 in brackets):
- **Locate and references, +44.5 MB (+47.7).** The estimate costed references as distances over the whole day, 125.1 MB. Those distances are not self-contained (above). The store's within-segment distances plus imports cost 173.9 MB, +48.8 MB. The locate column is 4.4 MB smaller than the estimate's.
- **Timestamp, +13.8 MB (+13.8).** The estimate let zstd wrap a timestamp block wherever it came out smaller. The store does not offer zstd on timestamps. Apart from that, the coding differs only at the 257 segment boundaries.
- **Add shares, +10.9 MB (+11.7); add price, +6.0 MB (+6.8).** These columns are grouped by locate (and, for prices, by side) within each segment, not over the whole day, so every run restarts at each segment. The estimate let zstd wrap add_shares where smaller; the store has no zstd on add_shares. At N = 2^22, add_price falls to 67.4 MB (the sweep above).
- **E, C and X shares, +2.1 MB (+2.5).** The estimate coded each against the order's remaining shares. Those are book state, which an imported order does not carry, so the store keeps the shares plain.
- **Tracking, +1.7 MB (+1.8).** The estimate used Stream VByte with zstd per type. The store uses a dictionary per type.
- **The rest, within ±1.5 MB each.** Creation references come out 3.3 MB (3.7) smaller, and imbalance flags 1.2 MB (1.1) smaller.

**Peak memory per whole-day run.** Measured on macOS by `/usr/bin/time -l` (raw `.time` files):

| Run | 12302019 footprint | 12302019 max RSS | 10302019 footprint | 10302019 max RSS |
|---|---|---|---|---|
| ingest | 279 MB | 8.46 GB | 275 MB | 9.24 GB |
| verify (G-1) | 363 MB | 10.00 GB | 363 MB | 10.93 GB |
| G-2, worst run | 372 MB | 10.01 GB | 369 MB | 10.93 GB |

Max RSS counts the clean, file-backed pages of the read-only source mapping, and on verify the store's mapping too. The kernel can drop those pages. The footprint is the process's own memory.

The writer's footprint, 279 MB, is within the bound in `docs/store.md`. At 2^20 records, the worst case of 19 values of 8 bytes per record is 159 MB, and the reference table is at most 32 MB plus 8 bytes per reference. The fixed locate tables are about 0.6 MB. The rest of the footprint is not broken down here.

**Worst-case work for one random segment.** The bound is in `docs/store.md`, under "Worst-case work for one segment". In short:
- at most 33 blocks and their CRCs;
- at most N values per column, with at most 3 per record in the multi-value columns, plus the rare and raw bytes;
- a counting sort for the columns grouped by type, and a sort of the adds by key;
- one list lookup per reference;
- nothing outside the segment except the symbols section, which is parsed once at open.

On the real days, the largest segment is 8,164,713 bytes in 28 blocks (12302019, segment 5), and the largest on 10302019 is 7,913,048 bytes. The average is 5.6 MB.

**Fuzzing and fault proofs.**
- `fuzz_itch_store` ran 240 s through `heavy.sh` under `/usr/bin/time -l`: 904,149 runs, clean, with libFuzzer's peak RSS at 444 MB and the process maximum at 466 MB. Raw: `phase3-raw/store/fuzz/fuzz-itch-store-240s.log`.
- The three new faults were proven with `prove_checks.py --only`. Raw: `phase3-raw/store/faults/`, with the first run in `run1/`.
  - `g1-store-alters-a-byte` was PROVEN: the oracle exits 1 and names the first differing record and byte.
  - `itch-store-import-not-appended` was PROVEN: the dev tests fail on `ItchStoreReferences`.
  - `fuzz-itch-store-distance-bound` first came back WRONG REASON. The fuzzer did exit 77 on the fault, but UBSan reported it, not ASan: an empty list made the read a non-zero offset from a null pointer. The expectation now names the frame of the function whose check the patch removes, `SegmentAssembler::State::named`, which either sanitizer's trace contains. Re-run, it was PROVEN.
- `coverage-required-removed` no longer applied: the unit test list had gained `bit_stream_test.cpp` and `codec_eval_test.cpp` after the patch was written. With its context refreshed, it was PROVEN: the coverage step fails with `verify_round_trip` not reached. Raw: `phase3-raw/store/faults/coverage/`.
- `results/check-proofs.json` was restored from git after each proof run.

**The full check.** `scripts/check.sh` ran through `heavy.sh` under `/usr/bin/time -l` at `4302ce8`, with no other sanitizer build running: all 29 steps passed in 993 s, with a 1.24 GB maximum resident set. That covers 570 tests under dev, ASan with UBSan, and TSan; clang-tidy on 110 files; 12 fuzz targets at 20 s each; and coverage reaching all 204 required functions (lines 93.83%, functions 97.39%, published in `results/coverage.json`). Raw logs are in `phase3-raw/store/check/`.

The first two attempts failed, and the fixes are committed:
- At `c64ca36`, ASan stopped `ReferenceEval.TheCommandLineWritesJsonWithoutPathsAndRefusesBadUsage` with a heap use after free. Two of its bad-usage argument lists held `string_view`s into temporary path strings. On debian the same test had failed outright, because the freed memory still read as valid arguments.
- At `67d2055`, clang-tidy refused 109 distinct findings in the store's code and tests: missing includes, static members read through instances, a recursive coder, unchecked optionals, and the column table without designated initialisers. All were fixed at `e45d08c` without changing a stored byte: the golden store file still matches both ways.

**debian.**
- The Linux build gate passed at `4302ce8`: the dev build, then ctest, 570 of 570 tests.
- The gate had failed one test at `7ebb81b`: the `ReferenceEval` use after free described above, which exited 0 there.
- G-1 on 12302019 passed through `~/quant-projects/bin/heavy.sh` with `taskset -c 2,6,7` (`results/roundtrip-12302019-linux.json`, now on the real store):

| Run | Wall time | Peak RSS (VmHWM) |
|---|---|---|
| ingest | 300 s | 6.21 GB |
| verify | 119 s | 6.43 GB |

  debian has no `/usr/bin/time`, so the peaks are the tool's own `ru_maxrss`, which on Linux is VmHWM. It counts the clean, file-backed pages of the mapped source and store.
- The store written on the Mac, copied over, also verifies on debian (116 s, VmHWM 6.95 GB).
- Its SHA-256 equals the debian-written store's, so the layout is the same on both hosts.
- 10302019 is not on debian, and nothing was fetched.
- Raw output: `phase3-raw/store/debian/`.

**What did not help.**
- A book checkpoint per segment cost 2,016 MB at 2^20, fifteen times the distances it would allow.
- One creation list over all locates cost 423 MB against 174 MB.
- Every stateless value coding of references cost 464 to 494 MB.
- Day-wide distances with a creation index came to 35 MB less, but they give up decoding one segment on its own.
- Segments larger than 2^20 save 2.2% at 2^22, for nearly three times the writer's memory and four times the work to decode a random segment.

### The first runs on GitHub Actions

The repository went public at `7ebb81b`, and `.github/workflows/ci.yml` ran on GitHub for the first time. Each run below is the
whole matrix, 7 checks on 2 hosts. Raw logs and artifacts: `phase3-raw/ci/`.

| Run | Commit | Failed | Which |
|---|---|---|---|
| [41770320877](https://github.com/jacksonbaxter/tick-store/actions/runs/41770320877) | `7ebb81b` | 10 of 14 | unit, asan-ubsan, tsan, coverage, clang-tidy, fuzz and repo on Linux; asan-ubsan, clang-tidy and fuzz on macOS |
| [41816297315](https://github.com/jacksonbaxter/tick-store/actions/runs/41816297315) | `fe19edc` | 6 of 14 | tsan, coverage, clang-tidy, fuzz and repo on Linux; fuzz on macOS |
| [41825067372](https://github.com/jacksonbaxter/tick-store/actions/runs/41825067372) | `ad542fd` | 0 of 14 | all green |

What failed, and what fixed it:

- **A use after free in a test** failed unit, asan-ubsan, tsan and coverage on Linux and asan-ubsan on macOS. Two of
  `ReferenceEval.TheCommandLineWritesJsonWithoutPathsAndRefusesBadUsage`'s bad-usage argument lists held `string_view`s into
  `(dir / "absent").string()` and `(dir / "no" / "dir" / "out.json").string()`, temporaries freed once the list was built. The
  macOS ASan report is that frame: a 160-byte string copied and freed in the test body, then read by `run_reference_eval_cli` in
  `string::append`. On Linux the freed bytes still read as the source path, so a refused usage exited 0. `67d2055` names the two
  paths; run 41816297315 passed all five jobs' tests. Why the macOS ASan preset did not stop it before the push: the test came in
  with `f8ebc4b`, and `7ebb81b` was pushed at 03:58 UTC, before any sanitizer run on that code. The first local `check.sh`, at
  `c64ca36` 47 minutes later, stopped at `asan-test` on exactly this test, and the debian gate at `7ebb81b` failed it too. The
  preset caught it on its first run; it had not been run before the push.
- **clang-tidy.** At `7ebb81b` both hosts failed on the ITCH store's findings, which `e45d08c` had already fixed: the macOS job
  passed at `fe19edc`. CI's LLVM is `install_tools.sh`'s pin, 23.1.4, the same as the local one, so the findings were not a version
  difference. Linux raised four that macOS cannot, because libstdc++ and glibc spell things differently: `ssize_t` and `rusage`
  come from headers no file includes directly, a `<cstddef>` was unused, and `frames[3].offset` did not count as a use of
  `itch::Frame`, since libstdc++'s `operator[]` returns its own alias for the element type. `052d9fd` returns `std::ptrdiff_t`
  from `FileOps::write`, which is `long` as `ssize_t` is on both hosts, gives `rusage` a NOLINT with its reason, names the
  frames' type and drops the include. Adding `<sys/types.h>` was not tried again: macOS calls it unused (see "clang-tidy had
  never seen the Linux-only sources"). The eight touched files were clean under clang-tidy on debian at `052d9fd`.
- **fuzz-dictionary timed out on Linux**, after 11 s on the 31-byte input `AAAQABcAAAABAQAAAAEAAAAJAAAAAAEBAAABKgAAAA==`. It is
  not a decoder bug. The input is a valid block: 2^20 values, the most a block may hold (D8's named cap), with one dictionary
  entry and Huffman codes of no bits, which is the block the encoder itself writes for 2^20 equal values. The decoder takes it in
  29 ms with the fuzz build's sanitizers. The time went to the oracle re-encoding the values as columns: in blocks of 3, 349,526
  calls of the dictionary encoder took 5.65 s, against 0.43 s in blocks of 64 and 0.20 s in blocks of 4,096 (497, 40 and 22 ms
  in a release build). The whole input took 7,959 ms on the Mac, under the 10 s timeout, and more than 10 s on the slower runner.
  `cc012b6` makes `encode_dict` copy a block whose values repeat the block before, which is the encoder's own output for it, the
  counts in `DictStats` included, and counts the copies as `repeated`. The input now takes 1,475 ms on the Mac, in each of three
  runs, and the whole dictionary seed corpus passes in 6.6 s. The oracle is unchanged. The input was already a generated seed,
  a header corruption of a 50-value block with its count set to 2^20; it is now also a named seed (the largest block of equal
  values, for the dictionary and run-length targets) and a crasher, `tests/unit/crashers/dictionary/timeout-one-entry-codes-of-no-bits`.
  With the copy disabled, `Dictionary.TwoToTheTwentyEqualValuesInBlocksOfThreeEncodeOneBlock` and
  `Dictionary.ABlockRepeatingTheOneBeforeIsTheSameBytes` fail, and replaying the crasher takes 14.0 s in the dev build, against
  2.7 s with it. Raw: `phase3-raw/ci/repro/`.
- **No fuzz target linked on macOS.** Xcode 16.4's linker on the runner refused every object clang 23 instruments for the fuzzer,
  `invalid r_symbolnum`, in our library and in zstd. The other presets linked. `758dded` links the fuzz targets on macOS with the
  LLVM install's own `ld64.lld`, through `-fuse-ld=lld`; the other presets keep Apple's linker. The local fuzz build links and runs
  with it.
- **repo on Linux.** `RunBenchTest`'s whole-script tests read the runner's own sysfs, where a virtual machine has no cpufreq boost
  switch, so turbo read `unavailable` and the validator refused the stand-in Linux run, rightly. `5c78dcf` reads the switch under
  `run_bench.py`'s `--sysfs-cpu`, as the sibling's topology already was, and the fixture writes one. With no switch, the header now
  says why: `unavailable: no cpufreq boost switch, as on a virtual machine`. The rule is unchanged, and is the narrowest that does
  both: `check_linux` already requires Linux results to be pinned, so every Linux file it accepts is from debian, and debian must
  record `enabled` or `disabled`. The reason form is refused as `unavailable` is. A new fault, `results-linux-turbo-unmeasured`,
  sets the committed debian bench file's turbo to the reason form, and `prove_checks.py --only` reported it PROVEN: check-results
  fails with `header.turbo: Linux must record it`. `results/check-proofs.json` was restored from git, so the fault is not in it
  until the next full run. Raw: `phase3-raw/ci/faults/`.
- **Coverage on Linux**, which the use after free had hidden in the first run, failed at 203 of 204 required functions:
  `verify_round_trip` returns a type holding `std::string`, so with libstdc++ it demangles as `verify_round_trip[abi:cxx11](...)`
  and its pattern matched nothing. `e1afb59` drops ABI tags from the demangled names before matching.
- **tsan on Linux ran out of disk** in the toolchain step in the second run: `No space left on device`. The cache had no entry
  for the key, so each of the 14 jobs downloaded the 1.6 GB LLVM release and unpacked it whole, 12 GB on Linux. `24c5cef`,
  `9c61d31` and `ad542fd` unpack only what the checks use: the compiler with its headers and runtimes, clang-tidy, lld, the
  coverage tools, `llvm-symbolizer`, the binutils CMake finds beside the compiler, macOS's libc++ and libLTO. That is 1.3 GB on
  Linux and 1.0 GB on macOS, and the install fails if a tool the checks run is missing. The first version failed on debian
  before any push: GNU tar matched each member through its listed directory, then called the member's own name not found, which
  macOS's bsdtar had not minded; members are now extracted with `--no-recursion`. On debian the trimmed install built and ran an
  ASan program with symbolised frames, a libFuzzer target, a TSan and a coverage build, clang-tidy and llvm-cov.

The third run, [41825067372](https://github.com/jacksonbaxter/tick-store/actions/runs/41825067372) at `ad542fd`, passed all 14
jobs: coverage 93.78% of lines on Linux and 93.84% on macOS with 204 of 204 required functions, 240 s of fuzzing per target on
each host, clang-tidy clean on 110 files per host. Before that push, the full local `check.sh` passed 29 of 29 steps on macOS at
`ad542fd` (988 s, maximum resident set 1.18 GB, through heavy.sh), and on debian the dev build passed ctest, 572 tests with the
2 `ItchRealDay` tests skipped for want of data.

### Queries over the ITCH store

`ItchQuery` and `tools/tick_query` (docs/store.md, "Queries") answer Q1 to Q5 through the index
version 2 added: per segment a seek column, and a locate column held as runs with a run directory.
Every number below is **iteration-only and unpinned**. None of them is a reported result, and none
went through `measure.sh`.

**What the index costs.** Store version 2 was ingested on both days and assembled by
`scripts/store_results.py` into `results/store-size-*.json`, which now list the query index. The
tools were the bench build at `3325f81`. Raw output is in `phase3-raw/query/store/<day>/`.

| Day | Version 1 bytes | Version 2 bytes | Ratio | Seek column | Run directory | Locate column, v1 to v2 |
|---|---|---|---|---|---|---|
| 12302019 | 1,449,464,212 | 1,495,541,951 | 5.693 to 5.517 | 1,972,413 | 3,995,309 | 275,922,975 to 315,999,839 |
| 10302019 | 1,588,982,528 | 1,640,462,478 | 5.686 to 5.508 | 2,156,058 | 4,300,525 | 303,040,299 to 348,027,417 |

- The index itself, the seek column plus the three run directory columns with their metadata, is
  5,967,722 bytes on 12302019 (0.399% of the file) and 6,456,583 on 10302019 (0.394%).
- Most of the growth is the locate column. Held as each locate's record indices, with gaps coded
  by PFOR and zstd, it costs 40.1 MB more than the file-order dictionary did on 12302019, and 45.0 MB
  more on 10302019. It is the symbol filter and also the only copy of the records' locates.
- On 12302019 the file grew by 46,077,739 bytes. That is the locate column's 40,076,864, plus
  5,955,386 of new payloads, plus 45,489 of footer for the new blocks and the plain flag.
- CR-1 still passes on 12302019 (5.517 against 2.3415).
- CR-2 still passes. The store is 0.469 of Parquet's stored bytes, 0.235 of ArcticDB's and 0.065
  of QuestDB's.

**G-1 and G-2 on version 2.** They ran on both days on the Mac, one run at a time through
`heavy.sh` under `/usr/bin/time -l`, and every run passed. `results/roundtrip-*.json` hold each
run.

| Run | 12302019 | 10302019 |
|---|---|---|
| ingest | 106.4 s, 280 MB footprint, 8.49 GB max RSS | 113.4 s, 269 MB, 9.26 GB |
| verify (G-1) | 36.8 s, 375 MB, 10.05 GB | 43.1 s, 380 MB, 10.99 GB |

In each G-2 run, both flipped stores exit 1 and the restored store passes.

On debian, G-1 on 12302019 passed under `taskset -c 2,6,7` through `heavy.sh`: ingest took 305.8 s
with a 4.56 GB peak RSS, and verify 175.5 s with 7.01 GB. The store debian wrote has the same
SHA-256 as the Mac's (`175f089e…`), and the Mac's store, copied over, also passed verify there.
`results/roundtrip-12302019-linux.json` holds these. Raw output is in `phase3-raw/query/debian/g1/`.

**Answers equal the harness's, on the slice and on the whole day.**

- **Slice.** On the version 2 store of the slice, every digest in `results/compare/*.json` matches
  on both paths, indexed and exact:

  | Query | Rows | Digest |
  |---|---|---|
  | Q1 | 527,644 | `230d7f3f…` |
  | Q2 | 66,741 | `2d9259f7…` |
  | Q3 | 5 | `f57ae03d…` |
  | Q4 | 338 | `2ea6acc5…` |
  | Q5 | 1 | `d68a6d60…` |

  The busiest locate is 6556.
- **Whole day.** For 12302019, `tick_export` wrote the day's columns, and its oracle passed
  (`timestamps_sorted: true`, so the harness's binary search and the query's filter pick the same
  rows). Then compare/queries.py's own numpy `build_queries` and `ground_truth` ran without
  clipping, with the window ending at 10:30:00, through `heavy.sh` under `/usr/bin/time -l`. That
  took 1.93 s, with a 750 MB peak footprint and a 4.76 GB maximum resident set, most of it the
  mapped columns. The busiest locate in [09:30, 10:30) is 6556 again. `tick_query` matched every
  answer, on both paths:

  | Query | Rows | Digest | Indexed: bytes decoded, segments |
  |---|---|---|---|
  | Q1 | 527,644 | `8a576f4b…` | 5,041,159, 1 |
  | Q2 | 739,944 | `5905cebd…` | 337,889,598, 62 |
  | Q3 | 60 | `7c2fb741…` | 258,441,700, 62 |
  | Q4 | 338 | `f65405f7…` | 3,708,239, 1 |
  | Q5 | 1 | `b56f72ae…` | 64,862,984, 257 |

  The exact path decodes whole segments: 359.7 MB for Q2 and Q3, and all 1,494.8 MB of the
  payloads for Q5, in 31.4 s.
- The same check passed on debian.
- Raw output is in `phase3-raw/query/oracle/` (`truth-12302019.json`, `check-*.log`, the `.time`
  files) and `phase3-raw/query/debian/timing/linux-check-indexed.log`.

**Latency, iteration-only and unpinned.** These are `engine_ns` from `tick_query` on the
whole-day 12302019 store, indexed path. Warm figures are the median of rounds 2 to 20 in one
worker. Cold figures come from 10 trials per query, each a proven `tick_coldcache evict` of the
store, then a fresh worker answering one query.

The Mac ran through `heavy.sh` while another project's test used two cores. debian ran under
`taskset -c 2,6,7` through `heavy.sh`, with its load average rising from 0.0 to 7.5 during the
run as another project started. Neither run is a measurement.

Raw output: `phase3-raw/query/timing/mac-indexed.json` and
`phase3-raw/query/debian/timing/linux-indexed.json`.

| Query | Rows | Bytes decoded | Mac warm median | Mac cold mean | debian warm median | debian cold mean |
|---|---|---|---|---|---|---|
| Q1 | 527,644 | 5.04 MB | 63.7 ms | 75.6 ms | 190.7 ms | 304.0 ms |
| Q2 | 739,944 | 337.9 MB | 2,605.7 ms | 2,887.1 ms | 4,525.7 ms | 7,916.1 ms |
| Q3 | 60 | 258.4 MB | 1,409.6 ms | 1,694.1 ms | 2,383.3 ms | 4,409.4 ms |
| Q4 | 338 | 3.71 MB | 36.7 ms | 53.7 ms | 83.7 ms | 187.3 ms |
| Q5 | 1 | 64.9 MB | 1,315.0 ms | 1,460.8 ms | 2,225.2 ms | 3,554.2 ms |

Opening the store took 1.6 to 2.7 ms on the Mac and 6.7 to 11.6 ms on debian, outside
`engine_ns`.

**Likely verdicts, iteration-only.** Not one is met:

- **QL-1** (Q2 warm median at most 16.9 ms) misses by about 150 times on the Mac and 270 times on
  debian.
- **QL-2** (Q3 warm median at most 0.41 ms) misses by more than three orders of magnitude.
- **QL-3** (Q4 cold mean at most 10 ms) misses at 53.7 ms on the Mac and 187.3 ms on debian.

On the slice, clipped to four and a half minutes, Q2 took 240 ms. A whole hour of one symbol
crosses 62 segments.

**Where the time goes.** In a `sample` profile of Q2 on the whole day (raw
`phase3-raw/query/prof/q2-full.txt`):

- 37% is dictionary decoding. That is the whole type column of every segment Q2 reaches, plus the
  reference distances and the add columns.
- 32% is the single pass over each segment's records up to the locate's last one. The pass ranks
  every record's type to find each column's value index, and keeps every locate's order list,
  because a reference names an order by its distance back in its locate's list, and that list
  includes the segment's imports.
- CRC32C, zstd's Huffman and the PFOR unpack make up most of the rest. The answer's SHA-256, about
  5% of the samples, is outside `engine_ns`, since the profile covers the whole worker.

So Q2's cost is set by records of other symbols as much as by its own.

**The index that would close QL-2, costed but not built.** Q3 over whole minutes could come from a
per-locate minute aggregate of the adds, with each entry holding locate, minute, adds, share sum,
and least and greatest price, 24 bytes raw. On 12302019 it was measured on a prototype
serialization by numpy from the export:

| Granularity | Entries | Raw | zlib -9 | lzma -6 | Share of the file, zlib |
|---|---|---|---|---|---|
| Per segment | 3,208,555 | 77.0 MB | 27.7 MB | 18.5 MB | 1.85% |
| Per day | 2,343,750 | 56.3 MB | 14.6 MB | 10.3 MB | 0.98% |

The codecs are the standard library's, so they bound what a store codec might do, and are not the
store's coding. Raw output: `phase3-raw/query/oracle/minute-index-12302019.json`. With that index,
Q3 would read 60 entries for each of 62 segments, or 60 entries in all, instead of 258 MB. Whether
that reaches 0.41 ms is not measured. A window that does not start and end on whole minutes would
still decode its edge minutes.

**Considered and not built, so not measured:**

- **A walk-back for references.** It would resolve each of the locate's references by decoding
  backwards from its position, instead of keeping every locate's list. Imports make the list
  depend on every earlier record of the locate in the segment, and a C, E, X, D or U of another
  locate between them still has to be ranked, so most of the pass stays.
- **A file-order copy of the locate column for Q4.** Q4 reads one segment's first groups, and the
  run form makes it decode every run to put locates back in file order (about 4.5 ms of Q4's 36
  ms). A copy would cost up to the v1 column, 276 MB.
- **A Huffman decode of several symbols per table lookup.** Today it decodes one per lookup.
- **A counting sort when ungrouping.** It would replace the sort of add places, which profiles
  under 1%.

**Fuzzing and faults.**

- `fuzz_itch_query` ran 270 s through `check.sh --only fuzz-itch-query --fresh-corpus` under
  `heavy.sh` and `/usr/bin/time -l`, and was clean: 38,445 runs, 141 a second, with libFuzzer's
  peak RSS at 618 MB. Raw: `phase3-raw/query/fuzz/`.
- Four faults were proven with `prove_checks.py --only`, and `results/check-proofs.json` was
  restored from git after each run. Raw: `phase3-raw/query/faults/`.
  - `query-window-group-boundary`, the time-window off-by-one: PROVEN, failing
    `ItchQuery.BothPathsEqualTheColumnFormAtEveryBoundary`.
  - `fuzz-itch-query-first-run-skipped`, the symbol filter skipping a segment's first run: PROVEN.
    The fuzzer exits 77 on "the indexed path's rows differ from the exact path's".
  - `query-run-order-unchecked`: PROVEN, failing `ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery`.
  - `query-cli-digest-ts`: PROVEN, failing `QueryCli.ServesEveryKindWithTheHarnessDigests`.

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
