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
  docs/TASKS.md decides that, with the column's own cost counted.

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
  Context the book holds, such as the remaining shares as an upper bound, is the next thing to try. It is in docs/TASKS.md.

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
  recency rank over 8,900 entries needs a Fenwick tree or similar per value. It is in docs/TASKS.md, with its decode cost unknown.
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
  offset, instead of through the reader, gets no check (docs/TASKS.md).
- **The reader's views hold the mapping.** Each one carries a `shared_ptr`, so a lookup costs an atomic increment. In
  return, a view that outlives its reader stays valid, where a borrowed span would fault after `munmap`.
- **One dependent chain of crc32 instructions.** Three interleaved streams would run about three times faster on long
  payloads. Verification has not yet been measured against decode, so this is not done (docs/TASKS.md).
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

Store version 3 has since replaced the layout this section measures; "Query iteration" below has
its numbers.

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

### Query iteration

This work made Q1 to Q5 faster against the harness, one profile at a time. The table below gives
each step's byte cost and latency change, including the steps that did not help.

Every timing is **iteration-only**. None went through `measure.sh`, and none is a reported result.

- **Mac:** macOS, unpinned, no counters. Each job ran alone through `heavy.sh`.
- **debian:** under `taskset -c 2,6,7` through `heavy.sh`. Another project kept that host's load
  average between 6.7 and 10.2 throughout (`uptime-*.txt`), so its figures are noisier still.
- **Warm:** the median of the rounds after the first, in one worker.
- **Cold:** the mean over trials. Each trial runs a proven `tick_coldcache evict` of the store,
  then a fresh worker answers one query.
- Every whole-day run checked each answer's rows and SHA-256 against compare/queries.py's numpy
  ground truth for 12302019 (`truth-12302019.json`), so no timing comes from a wrong answer.

Latencies are in ms, Q1 / Q2 / Q3 / Q4 / Q5. Bytes are for 12302019. Raw output is in
`phase3-raw/query-iter/`.

**Steps that were timed:**

| Step | Change | Bytes per day, ratio | Q1–Q5, iteration-only | Kept? | Why |
|---|---|---|---|---|---|
| 0 | Baseline: store version 2 at `6576b0a` | 1,495,541,951, 5.517 | Mac warm 51.8 / 2,256.9 / 1,261.8 / 31.0 / 1,165.7; cold 69.1 / 2,673.9 / 1,600.9 / 43.0 / 1,334.3 (8 rounds, 3 trials, `mac/step0-baseline-6576b0a.json`). debian, same format and decoders at `3325f81` (above): warm 190.7 / 4,525.7 / 2,383.3 / 83.7 / 2,225.2; cold 304.0 / 7,916.1 / 4,409.4 / 187.3 / 3,554.2 | n/a | Reference point |
| 1 | Faster scalar decoders (`da20832`): Huffman codes read from one loaded word, two short codes per lookup, per-thread scratch, a hashed repeat check, PFOR loads by `memcpy`, head delta by selects | Unchanged | Mac warm 38.3 / 1,523.0 / 823.5 / 21.6 / 529.1; cold 55.2 / 1,919.1 / 1,144.3 / 33.3 / 669.1 (`mac/step1-rerun-v2store.json`; the first run, `mac/step1-decoders.json`, agrees within 6%) | Yes | Dictionary decoding went from 7.05–7.42 to 2.41–3.53 ns a value (`colbench`, below). Q5 is 2.2 times faster, Q2 1.5 times |
| 2 | v3a: records stored by locate, in pieces, with a run directory; timestamps delta-coded within each locate | 1,969,472,013, 4.190 | Mac warm 38.9 / 42.9 / 20.8 / 25.2 / 496.0 (`warm.py`, 6 rounds, `mac/v3a-warm.txt`) | Replaced by 3 | Q2 35 times faster than step 1, Q3 40 times. But the file is 474 MB larger, and timestamps alone account for 356 MB |
| 3 | v3b: as step 2, but timestamps stored as residuals over 64-record time bases | 1,750,915,467, 4.713 | Mac warm 37.9 / 42.9 / 21.2 / 23.5 / 488.1 (`mac/v3b-warm.txt`) | Yes, as store version 3 | 219 MB smaller than step 2, at the same speed |
| 4 | Minute-aggregate buckets in a sorted vector instead of a map | Unchanged | Q3 warm 21.2 to 20.2 (`mac/v3b-minutevec-warm.txt`) | Yes | Fewer allocations. The 1 ms is within the 1 to 2 ms spread between Mac runs |
| Final | Version 3 as committed at `e257f80`, timed by `timing.py`: 20 rounds, 10 cold trials | 1,750,915,467, 4.713; 10302019: 1,928,632,798, 4.685 | Mac warm 39.3 / 45.0 / 21.7 / 24.7 / 509.7; cold 55.8 / 218.2 / 116.1 / 39.2 / 665.9 (`final/timing/mac-indexed.json`). debian warm 126.1 / 116.2 / 48.2 / 62.3 / 1,202.2; cold 193.6 / 994.0 / 496.9 / 105.2 / 1,971.4 (`debian/linux-indexed.json`) | n/a | Against step 0 on the Mac: Q2 warm is 50 times faster and cold 12 times; Q3 warm 58 times faster; Q5 warm 2.3 times. Q1 and Q4 are slower than step 1 |

**Steps measured on bytes alone.** Bytes decided each of these, so none was timed:

| Step | Change | Bytes per day, ratio | Kept? | Why |
|---|---|---|---|---|
| — | Time bases every 16 records instead of 64 | Timestamps 751.4 MB, 16.3 MB less than at 64 | No | Four times the bases to read and check, for 2% of the column |
| — | Time bases every 4, 256, 1,024 or 4,096 records | 813.5, 802.5, 833.6 and 861.1 MB, against 767.7 at 64 | No | 64 is the smallest |
| — | Residuals interpolated between two bases | 758.0 MB at 64 (9.8 MB less); 742.4 MB at 16 | No | Every residual would need two bases and a division, for about 3% of the column |
| — | Locates with fewer than T records kept in one mixed file-order run | Timestamps 959.4 MB at T = 64, with 19.0 M records mixed. At T = 1,024, 746.3 MB, with 171.7 M records (64%) mixed | No | The cost sits in busy locates, not rare ones. Moving enough records to matter puts most of the day back in file order, where Q2 pays for the other symbols again |
| — | Creation references as residuals over 64-record bases | 254.5 MB before zstd, against 323.2 MB delta-coded; about 3% smaller after zstd | No | Too little to justify a second base column |
| — | zstd frames over whole columns instead of per piece | 4.07 MB less (`mac/zexp-v3b-12302019.txt`) | No | Q1 and Q4 would inflate whole columns of locate, creation_ref and add_price to read a few thousand values |

The time-base figures are timestamps plus bases. Those rows were measured by `tsexp`, a scratch tool
over the columns of the 12302019 store (`mac/tsexp-v3a-12302019.txt`).

**Decoder speed** (step 1). `colbench` is a scratch benchmark that decodes the dictionary columns
of every eighth segment of the version 2 store. It was built against each commit's libraries and
run on the Mac, unpinned. Raw: `mac/colbench-v0.txt` and `mac/colbench-v2.txt`.

| Column | 6576b0a | da20832 |
|---|---|---|
| type | 7.29 ns a value | 2.57 |
| ref_distance | 7.42 | 3.53 |
| add_side | 7.05 | 2.41 |
| add_shares | 7.37 | 3.23 |

**What version 3 changed, and what it cost.** Version 2 kept each segment's records in file order.
So Q2 ranked every record of a segment up to the locate's last, and decoded every column it
touched whole. Version 3 (`docs/store.md`) changes that in five ways:

- **Stored order.** Each segment stores its typed records by ascending locate, in file order
  within a locate, then the raw records.
- **Locate column.** It holds each record's file position, as gaps within its run.
- **Run directory.** It gives each run's start, add start and import start, so a single-locate
  query reads its own run and no other locate's records.
- **Pieces.** Columns are cut into 16,384-value pieces, with CRC32C and zstd applied per piece.
- **Time bases and seek index.** Timestamps are residuals over time bases. The seek index also
  holds each file-order group's least and greatest time, for the all-locate queries.

On 12302019 the file grew by 255,373,516 bytes. On 10302019 it grew by 288,170,320. By part, on
12302019, payload plus block metadata, from the two `layout.json`:

| Part | Version 2 | Version 3 | Change |
|---|---|---|---|
| timestamp | 630,118,879 | 755,133,603 | +125,014,724 |
| time_base (new) | 0 | 12,805,508 | +12,805,508 |
| creation_ref | 91,779,973 | 201,635,458 | +109,855,485 |
| match | 445,906 | 9,779,206 | +9,333,300 |
| run directory and seek | 5,586,890 | 7,695,087 | +2,108,197 |
| container block entries, 7,022 to 116,188 blocks | 224,704 | 3,718,016 | +3,493,312 |
| every other column | | | −7,237,010 |

Most of the cost is order. Values that were monotone, or nearly so, in file order are not in
stored order:

- **Timestamps.** Delta-coded within a locate, they took 986 MB, against 630 MB in file order.
  Residuals over bases take 768 MB.
- **Creation references.** They lose their file-order closeness: 202 MB with zstd, against 92 MB.
- **Match numbers.** They no longer ascend.
- **Imports and imbalance prices.** These shrink. Fewer references need an import, and a
  locate's imbalance prices are close to each other.

Per-piece framing has two costs: 4.07 MB of zstd output, and 4.8 MB of block entries and block
metadata.

- **CR-1** still passes: 4.713 against 2.3415.
- **CR-2** still passes: the store is 0.549 of Parquet's stored bytes (`results/store-size-*.json`).
- **Query index.** The seek column and run directory are 8,075,919 bytes on 12302019 (0.46% of
  the file) and 8,719,289 on 10302019.

**Final layout, checked.** The final layout passed every check on both days and on both hosts:

- **G-1 and G-2 on the Mac.** Both ran on both days, one run at a time through `heavy.sh` under
  `/usr/bin/time -l` (`final/<day>/`, `results/roundtrip-*.json`). Every run passed. In each G-2
  run, both flipped stores exit 1 and the restored store passes.

  | Run | 12302019 | 10302019 |
  |---|---|---|
  | ingest | 103.4 s, 395 MB footprint, 8.58 GB max RSS | 115.4 s, 384 MB, 9.37 GB |
  | verify (G-1) | 33.7 s, 501 MB, 10.41 GB | 37.1 s, 531 MB, 11.36 GB |

- **G-1 on debian, 12302019.** It passed under `taskset -c 2,6,7` through `heavy.sh`. Ingest took
  254.4 s with a 6.66 GB VmHWM, and verify 146.0 s with 9.86 GB
  (`results/roundtrip-12302019-linux.json`). The store debian wrote has the same SHA-256 as the
  Mac's, `86989cd3…`, so the bytes verified there are the Mac's.
- **Answers on the slice.** On the version 3 store of the slice, every digest in
  `results/compare/*.json` (both files) matches on both paths: 20 of 20 (`slice/check.log`).
- **Answers on the whole day.** On 12302019, `tick_query` matched every answer and the busiest
  locate, 6556, on the indexed and exact paths on the Mac (`final/oracle/`) and on the indexed
  path on debian (`debian/check-indexed.log`). Bytes decoded on the indexed path:

  | Query | Version 3 | Version 2 |
  |---|---|---|
  | Q1 | 9.02 MB | 5.04 |
  | Q2 | 16.27 MB | 337.9 |
  | Q3 | 10.27 MB | 258.4 |
  | Q4 | 9.02 MB | 3.71 |
  | Q5 | 64.85 MB | 64.86 |

- **Fuzzing.** `fuzz_itch_store` and `fuzz_itch_query` each ran 300 s on corpora from the
  version 3 encoder's seeds, and both were clean: 873,182 and 49,206 runs, with peak RSS of 456
  and 667 MB (`fuzz/`).

**Why Q1 and Q4 got slower.** Each reads one segment, but it must decode that segment's whole
locate column, every run, to put the window's records back in file order. That is 9.0 MB in 1,745
codec blocks, against 5.0 and 3.7 MB in version 2. On the Mac, Q1 warm is 39.3 ms against step 1's
38.3, and Q4 24.7 ms against 21.6.

**Why Q5 is slow.** Q5 sums every record's shares over the whole day: 152,012,056 dictionary
values in 257 segments, 64.85 MB. The scalar decoder Huffman-decodes each one, at 3.35 ns a value
over the 509.7 ms warm figure. The profile (`prof/v3b-q5.sample`) is almost all
`decode_dict_block`.

Q5 reads no column it does not need, so no index makes it faster. Three things would:

- SIMD decoding, which is out of scope here.
- A different code for the share columns, which would change the bytes.
- A stored per-segment share sum. That is an aggregate index, so it would be a benchmark special
  in the headline, and it is not built.

**Where Q2's time goes.** From the engine's samples in `prof/v3b-q2.sample`, excluding the digest:

| Part | Share of samples |
|---|---|
| Dictionary decoding of the run's types, sides and distances | 32.7% |
| `scan_run` | 19.2% |
| PFOR | about 8% |
| zstd's Huffman | 6.9% |
| CRC32C | 6.2% |
| memmove | 5.1% |

**Likely verdicts.** These are iteration-only, not measured under `measure.sh`, and every one
misses:

- **QL-1** (Q2 warm median at most 16.9 ms) misses at 45.0 ms on the Mac (2.7 times the target)
  and 116.2 ms on debian (6.9 times). The gap was 134 times at step 0. Q2 now reads only its
  locate's runs, so what is left is the work per row: 739,944 rows at 61 ns a row. The target
  needs 23 ns a row with every column decoded. Dictionary decoding alone takes about 20 ns a row
  (32.7% of the samples), so everything else would have to be nearly free.
- **QL-2** (Q3 warm median at most 0.41 ms) misses at 21.7 ms on the Mac (53 times) and 48.2 ms on
  debian (118 times). Q3 must decode every add of the locate in the hour, with its price, shares
  and side. Only the per-locate minute aggregate costed above (14.6 MB per day by zlib) would
  reach the target. That is an index built for this query, so it stays costed and unbuilt.
- **QL-3** (Q4 cold mean at most 10 ms) misses at 39.2 ms on the Mac (3.9 times) and 105.2 ms on
  debian (10.5 times). Opening the store adds 8.5 ms more, outside `engine_ns`. The rest is
  dominated by the whole locate column that Q4 decodes to restore file order. A file-order copy of
  locates would remove that at up to 276 MB, the version 1 column.

**Fault proofs.** Twenty-four faults were proven with `prove_checks.py --only`, and
`results/check-proofs.json` was restored from git after each run (`faults/prove-run1.log`,
`faults/prove-run2.log`).

- **Rebased.** Five old faults were rebased onto the new code: the dictionary repeat, the
  head-delta escape, the run order, the first run skipped and the window boundary.
- **New.** Nineteen new faults cover each check the pieces, locate runs, time bases and stored
  order added.
- **Undetected at first.** Three query-side faults went undetected on the first run: add start,
  run adds and import start. Each was backed by another check on the only path the test took.
  New corrupt cases now reach each one (a run's adds one early and one late, for rows and for the
  minute aggregate, and a run's imports ending one early), and all three are PROVEN.
- **Not provable.** The query's own piece value count check cannot fail once the reader's
  directory rules hold, so no fault can show it failing. The reader's check is proven
  (`store-piece-value-count`).

**Gaps.** debian had no `perf`, so there was no Linux profile of the version 3 queries. The project toolchain there has one
now, and the SIMD section below has the profile.

### SIMD

Decode kernels for NEON on arm64 and AVX2 on x86-64, behind one dispatch table, from `3155b2e` on. Every number here is
iteration-only. The Mac numbers are macOS on the M4 Max, unpinned, with no counters and a 24 MHz timer (about 42 ns). The debian
numbers are Linux on the Ryzen Embedded V1605B (Zen 1), run through `heavy.sh` with `taskset -c 2,6,7`, not pinned to one core.
debian is shared, so each debian figure names the load it ran under. Zen 1 splits each 256-bit operation into two 128-bit
halves, so AVX2 there does at most the work of 128-bit vectors per cycle. Raw outputs are kept with the logs, not in the repo.

**Dispatch** (`include/tick_store/codec/simd.hpp`, `src/codec/simd/`).

- One table, `simd::Kernels`, of function pointers: interleaved unpack, linear unpack, frame-of-reference add with optional
  zigzag, prefix sum, the inverse delta, fused unpack and inverse delta in 64-bit and 32-bit lanes, and Stream VByte. The codecs
  call `active_kernels()` and hold no `#ifdef`. CMake picks `kernels_neon.cpp` or `kernels_avx2.cpp` by
  `CMAKE_SYSTEM_PROCESSOR`. The scalar set is always built.
- arm64: NEON is baseline, so the choice is made at compile time. No SVE, SME or bit-deposit instructions.
- x86-64: AVX2 is chosen at run time from CPUID leaves 1 and 7 and XGETBV, read once and cached. No PEXT, PDEP, gather or
  AVX-512. The AVX2 file is not compiled with `-mavx2`. Its functions take `target("avx2")` from a pragma that starts after
  every standard header, and the shared driver and tables sit in unnamed namespaces, so no inline function with external linkage
  gets an AVX2 body. That is the ODR problem Arrow documents. Checked by cross-compiling for x86-64 and disassembling: no
  external symbol from the standard library holds VEX code.
- One driver, `drive<Step>`, runs a set's whole steps while the input fits and hands the rest to the scalar kernel. The scalar
  set is the driver with no whole steps.
- `TICK_STORE_SIMD=scalar|neon|avx2` caps the level. An unknown or unavailable level aborts before the first decode. Per-thread
  run counters show which set ran, and `SimdDispatch.TheRequestedSetIsTheOneThatRuns` fails if another did. ctest runs the codec,
  store, query and fuzz-regression tests a second time capped to scalar (`simd_capped_to_scalar`).
- `ScalarKernelsOnThisThread` forces the scalar set on one thread while it lives. The fuzz oracles use it.

**Kernels per instruction set.**

| Kernel | NEON | AVX2 | In the codecs |
|---|---|---|---|
| Interleaved unpack, every width 0 to 64 | Width-templated, immediate shifts | Same | PFOR full vectors |
| Linear unpack, widths up to 56 | `tbl` shuffle, per-lane shift | `pshufb`, `vpsrlvq` | Tails, patch lists, scale remainders |
| Frame of reference add, zigzag | Yes | Yes | PFOR vectors, Stream VByte zigzag |
| Prefix sum | In-register (Kogge-Stone) | Scalar tree | Stream VByte delta |
| Inverse delta, from the block base | In-register | In-register | PFOR delta, two passes |
| Fused unpack and inverse delta, 64-bit lanes | Yes, not chosen | Yes, chosen | PFOR delta vectors with no exceptions |
| Fused, 32-bit lanes relative to the block base | Yes, not chosen | Yes, not chosen | None |
| Stream VByte decode with the length check | `tbl` | `pshufb` on 128 bits | Stream VByte |

Dictionary decoding and the time-base residual add stay scalar, for the reasons below.

**Tests.** `tests/unit/simd_kernels_test.cpp` compares every kernel of every set this processor runs with the scalar set by
`memcmp`: interleaved unpack at every width 0 to 64 and 16 byte offsets; linear unpack at widths 1 to 64, counts 0 to 33 and
1,000, and offsets; the add, prefix and delta kernels at lengths 0 to 40, 1,023 to 1,025 and 4,099, from 8 starting offsets,
with several bases; the fused kernels at every width, base, zigzag and offset; Stream VByte at every tail and offset, with a
long value refused at every position. Every input is an exact-size heap buffer, so ASan sees a read one byte past it, and packed
bytes sit at odd offsets, so UBSan's alignment check sees a typed load. At `f8e7a68` the dev suite passes 620 of 620 on the Mac
with NEON, in the full `check.sh` through `heavy.sh` (30 of 30 steps), and 620 of 620 on debian with AVX2 selected, the capped
run included; at `43aa7d2` debian passed 618 of 618. The short ASan and UBSan pass over the kernel, codec and fuzz-regression
tests on debian found `load_words_le` passing a null pointer to `memcpy` at width 0, fixed in `a961486`; at `43aa7d2` its 97
tests passed in 233 s.

**Fuzzing.** Every codec fuzz target decodes each block, and each column of part 2, a second time with the scalar set forced on
the thread, and the two must agree: the same refusal, or the same values. `fuzz_itch_store` reads each file a second time that
way, and `fuzz_itch_query` answers its queries a second time. `fuzz_columns` reads no kernel. The oracle is shown failing on a
fault only it can see: the NEON Stream VByte kernel stops refusing a value stored in more bytes than it needs
(`fuzz-svb-neon-canonical-dropped`), caught as "the kernel set and the scalar set decoded a block differently". On the Mac at
`57ec582` every touched target ran 240 s clean through `heavy.sh`: `fuzz_pfor` 16,563 runs, `fuzz_stream_vbyte` 96,647,
`fuzz_dictionary` 17,415, `fuzz_run_length` 60,777, `fuzz_gorilla_timestamps` 646,447, `fuzz_gorilla_xor` 309,410,
`fuzz_itch_store` 535,769 and `fuzz_itch_query` 37,360 (`mac-fuzz/`). On debian at `43aa7d2`, where the AVX2 paths are the
ones under test, each decoder whose AVX2 path is new ran 240 s through `heavy.sh`: `fuzz_pfor` 4,292 runs,
`fuzz_stream_vbyte` 17,187, `fuzz_dictionary` 6,818, `fuzz_itch_store` 87,528 and `fuzz_itch_query` 7,914, all clean.
`fuzz_run_length` stopped at 12 s on a timeout. The input was the seed of 2^20 equal values in one 48-byte block, which the
oracle re-encodes in blocks of 3: 349,526 encoder runs, 7.5 s on the Mac with sanitizers and over the 10 s limit on Zen 1.
`encode_dict` had been made to copy a block that repeats the one before after the same seed timed out on CI; `encode_rle`
now does the same (`f64d4f3`), and the seed takes 1.1 s on the Mac. After the fix `fuzz_run_length` ran 240 s clean on both
hosts: 68,922 runs on the Mac and 13,044 on debian (`mac-fuzz-rle/`, `debian/verify3/`).

**Faults.** A tail off-by-one in the NEON frame-of-reference kernel, its scalar tail starting one value late
(`neon-add-base-tail-off-by-one`), is caught by `SimdKernels.AddBaseMatchesScalarOnEveryTail`, proven with
`prove_checks.py --only` on the Mac. The same fault in the AVX2 kernel was shown on debian, where `prove_checks.py` cannot run (it
clones with git, and debian holds an exported tree): on a scratch copy of the tree at `a961486`, the kernel tests passed, the
patched copy failed `SimdKernels.AddBaseMatchesScalarOnEveryTail` (418 failed assertions), and the copy and its build were
removed. The kernel rewrite had moved the code under two older fault patches,
`fuzz-pfor-zigzag-order` and `fuzz-stream-vbyte-length-check`, and the time-base rewrite a third, `store-time-past-48-bits`.
All three were made again on the new code and proven again. `results/check-proofs.json` was restored from git after each proof
run, so it does not yet list the new faults. The first full Mac check after the kernels failed its clang-tidy step: no compile database
on the Mac holds `kernels_avx2.cpp`, and `check.sh` did not yet list it with the other processor's files. It does from
`f8e7a68`, and `kernels_neon.cpp` on x86-64 likewise.

**What the layout gave up.** From the C1 table in "Integer codecs on ITCH", timestamps in file order on `12302019` and
`10302019`:

- Lost adaptivity: 0.621 and 0.568 bits a value that a width per 128 values would save, 1.474 and 1.451 per locate.
- The 1,024-value floor: no values in short vectors in file order, 1.8% and 1.6% per locate. Short vectors take the linear
  layout, which now has a kernel of its own.
- Padding: none, because tails are linear. Padding them to 1,024 would have cost 0.42 and 0.39 bits a value per locate.
- Per-lane order: values stay in value order across the 16 lanes, with one base per vector. Decoding a delta therefore needs a
  prefix sum across each row, in the register. FastLanes transposes the values so that each lane holds its own run of deltas and
  the inverse delta is a vertical add with no cross-lane step. That transposed layout is what DS-3 and DS-4 measure, and the
  main reason this layout misses them (below). Moving to it would change the store format.

**Kernel throughput**, G values a second, the median over widths 0 to 64 where a kernel has a width, one vector of 1,024 values
in L1 decoded over and over (`bench/simd_kernel_bench`, by hand):

| Kernel | Mac scalar | Mac NEON | Speedup | debian scalar | debian AVX2 | Speedup |
|---|---|---|---|---|---|---|
| Interleaved unpack | 6.49 | 7.05 | 1.09 | 2.25 | 2.89 | 1.28 |
| Linear unpack | 2.01 | 7.31 | 3.64 | 0.66 | 1.97 | 3.0 |
| Unpack and inverse delta, two passes | 1.99 | 3.42 | 1.72 | 0.97 | 1.19 | 1.23 |
| Unpack and inverse delta, fused | 1.66 | 3.30 | 1.99 | 0.97 | 1.29 | 1.33 |
| Fused in 32-bit lanes, widths 0 to 21 | 1.76 | 3.62 | 2.06 | 1.06 | 1.16 | 1.09 |
| Prefix sum, in-register form, with an 8 KB copy | 3.21 | 4.84 | 1.51 | 2.12 | 1.57 | 0.74 |
| Prefix sum, scalar tree form, with the copy | 3.20 | 3.25 | 1.02 | 2.05 | 2.04 | 1.00 |
| Frame of reference add | 5.24 | 5.21 | 0.99 | 2.18 | 2.25 | 1.03 |
| Frame of reference add with zigzag | 3.96 | 3.66 | 0.92 | 1.99 | 2.17 | 1.09 |
| Stream VByte decode | 0.71 | 6.23 | 8.8 | 0.39 | 2.19 | 5.6 |

- Mac: the tree at `e97c453`, load 1.5 to 3.4 (`mac/kbench-allwidths.json`). debian: `4840312`, load under 2, two runs within
  3% of each other; `perf stat` gave 3.50 GHz over the run (`debian/simd-kbench-1.json`, `-2.json`). The scalar column is the
  scalar set in the same binary.
- "Scalar" is not naive code. Clang auto-vectorises the scalar unpack and the add loops: on arm64 to `ushl.2d` and `add.2d`, on
  x86-64's SSE2 baseline to `psrlq`, `psllq`, `por` and `paddq`. That is why NEON buys only 9% on the interleaved unpack and
  nothing on the add. The kernels that win are those the compiler cannot write: shuffles (Stream VByte, the linear layout) and
  the cross-lane prefix sum.
- The linear kernel stops at width 56, where one value can span nine bytes; wider values take the scalar path, at 2.00 G/s on
  the Mac either way.
- The debian linear row comes from a later run at `57ec582`, the first to have it, which started under a load of 10.7 from
  another project and ended at 3.2, at 3.15 GHz: every figure in that run is 10 to 15% below the clean one, and the ratios of
  the other rows match it (`debian/after/simd-after/kbench2.json`). A run at `57ec582` under load 6 to 8 at 2.75 GHz gave
  numbers about half the clean run's and is not used (`debian/chain/kbench.json`).

**Experiments.**

- 64-bit lanes against 32-bit lanes relative to the block base: no gain. The narrow form must first prove that every value fits
  in 32 bits from the base, then widens on the store. On the Mac it is 3.62 G/s against 3.64 for two passes over the same
  widths 0 to 21; on Zen 1, 1.16 against 1.25 for two passes and 1.29 fused over those widths. Not used.
- Fused unpack and prefix against two passes: two passes win on the M4, 3.42 against 3.30, and the fused form is ahead at only 17
  of the 65 widths. The fused form unpacks with a shift count in a register, since its rows run in a loop, where the two-pass
  unpack uses immediate shifts. On Zen 1 fused wins, 1.29 against 1.19, ahead at 64 and 61 of the 65 widths in two runs, so the
  AVX2 set fuses and the NEON set does not.
- The 64-bit prefix sum, NEON against a scalar dependency tree: NEON wins on the M4, 4.84 against 3.25, the opposite of what
  Arm's engineers report on Neoverse. On Zen 1 the in-register AVX2 form loses to the scalar tree, 1.57 against 2.04: each
  cross-lane `vpermq` is slow there, and a 256-bit add costs two. The AVX2 set uses the tree for the prefix sum.

**Per-column decode on the real day**, ns a value over every fourth segment of the `12302019` store, the second of two runs
(`mac/colbench-final.txt`):

| Column | Before, `a7c3742` | After, `57ec582`, NEON | After, capped to scalar |
|---|---|---|---|
| timestamp (PFOR delta, patched) | 0.90 | 0.60 | 0.79 |
| time_base | 0.65 | 0.39 | 0.63 |
| add_price | 1.00 | 0.78 | 0.86 |
| type (dictionary) | 2.29 | 2.26 | 2.24 |
| ref_distance (dictionary) | 3.01 | 2.83 | 2.82 |
| add_shares (dictionary) | 2.75 | 2.61 | 2.63 |

- The timestamp column went from 0.90 to 0.60 ns a value, 1.5 times. Most of its time had been the linear unpack of its patch
  lists and the checked loop over the patch positions, not the full vectors: the profile before showed `unpack_linear` with 442
  samples and `read_patches` with 330, against 135 for the block decoder and 58 for each unpack width. Patch positions are now
  formed without per-value checks when one sum shows they fit, with the checked loop kept for the first error.
- Capped to scalar, the same build is still faster than before (0.79), from the patch-list change alone.

**Queries Q1 to Q5**, whole day `12302019`, indexed path, `build/scratch/query/timing.py`, every answer checked against the
ground truth. Mac, warm median ms, before (`a7c3742`) and after (`57ec582`) run alternately twice, 20 warm and 5 cold trials each:

| | Q1 | Q2 | Q3 | Q4 | Q5 |
|---|---|---|---|---|---|
| Warm before | 38.97, 39.14 | 44.48, 44.63 | 21.37, 21.54 | 24.46, 24.59 | 503.47, 504.22 |
| Warm after, NEON | 37.97, 37.75 | 43.06, 43.34 | 20.38, 20.32 | 23.25, 23.19 | 502.63, 501.22 |
| Change | -3.1% | -3.0% | -5.1% | -5.3% | -0.4% |
| Cold before | 56.42, 55.60 | 210.31, 210.88 | 111.96, 112.30 | 38.45, 38.16 | 663.73, 665.55 |
| Cold after | 54.54, 54.79 | 209.95, 209.17 | 111.64, 111.52 | 37.39, 37.52 | 660.71, 661.53 |

- The queries barely move. Q1 to Q4 gain 3 to 5% warm. Q5 is 80% Huffman decoding of the share column, which has no vector
  form. Cold queries are bound by reading the file. Peak RSS 379 MB, peak footprint 11.6 MB.
- debian at `43aa7d2`, load about 1, 20 warm and 5 cold trials, every answer checked, AVX2 against the same build capped to
  scalar (`debian/day/simd-day/timing-{avx2,scalar}.json`). Warm medians: Q1 118.6 against 122.2 ms (-3.0%), Q2 108.7 against
  111.8 (-2.7%), Q3 43.9 against 45.9 (-4.4%), Q4 57.1 against 60.8 (-6.2%), Q5 1,134.4 against 1,146.8 (-1.1%). Cold medians
  move from -19% to +5% between the two runs, which is the spread of cold reads on that disk, not the kernels. Peak RSS of
  each timing run 416 MB and 413 MB, from `getrusage` of its children, since debian has no `/usr/bin/time`. Ingest at the same
  commit took 224 s with a peak RSS of 7.68 GB.

**Where SIMD lost, or was not tried.**

- Dictionary decoding. The Huffman decode is a chain of dependent lookups, about 8 cycles each with two codes a lookup, and it
  is 80% of Q5 (`decode_dict_block`, 6,075 of about 7,600 samples). Split by phase on add_shares and ref_distance: counting
  0.4 to 0.6 ns a value, gathering 0.3 to 0.5, Huffman about 2. A multi-symbol table was tried and rejected: building it for each
  4,096-value block costs what it saves. Decoding several streams at once would need a format change. TASKS keeps it open.
- The time-base residual add, `t = r + max(t before, base)`. A max-plus recurrence can be scanned in parallel only if addition
  is monotone, and residuals wrap modulo 2^64 by design, so no exact vector form exists. The loop was rewritten instead, with
  only the add and the max on the chain: 0.94 to 0.69 ns a value on 65,536 values and 1.03 to 0.73 on a million
  (`mac/timebase-scalar-rewrite.txt`).
- The frame of reference add and zigzag, and on the M4 the interleaved unpack: the compiler's scalar output already matches.
- The AVX2 in-register prefix sum, which loses to the scalar tree on Zen 1.

**Against the targets** (iteration-only, so no verdict; the measured verdicts are in "Decode targets under measure.sh", below):

- DS-3, 64-bit block delta decode at least 3.6 G values/s on NEON: two passes reach 3.42, the median over widths 0 to 64 on the
  M4 Max. Short by 5%, against FastLanes on an M1, whose transposed layout needs no cross-lane prefix sum.
- DS-4, the same kernel at most 0.377 cycles a value on AVX2: 1.29 G/s fused at 3.50 GHz is 2.71 cycles a value, 7.2 times
  over, on Zen 1 against FastLanes on Zen 3. Zen 1 splits each 256-bit operation in two and is slow on cross-lane permutes, and
  the layout needs a prefix sum across lanes.
- DS-6, the SIMD layout at least 14 times faster than the Gorilla stream on the same column: met on the Mac. The open slice's
  6,125,294 timestamps in file order, one column, 4,096-value blocks: Gorilla delta-of-delta 9.46 to 9.57 ns a value, PFOR
  delta 0.565 to 0.574 with NEON, 16.6 to 16.7 times; capped to scalar 0.817 to 0.821, 11.6 to 11.7 times
  (`mac/ds6-ds8-gorilla-pfor.txt`).
- DS-7, the same on Linux, which has no published figure: 8.0 to 8.5 times with AVX2 on Zen 1, Gorilla 23.6 to 25.9 ns
  a value against PFOR delta 2.95 to 3.03; capped to scalar 7.8 to 8.4 times, PFOR at 3.10 to 3.22 (`a961486`'s tree, load
  about 3, `debian/after/simd-after/ds7.txt`).
- DS-8, delta-of-delta reconstruct at least 8.20 GB/s of 32-bit values: PFOR delta-of-delta on the same column decodes at 0.83 ns
  a value on the M4, 4.8 GB/s counted as 32-bit values, short by 1.7 times; on Zen 1, 3.19 to 3.25 ns, 1.2 GB/s. The second
  difference is undone by two scalar passes at the block level, with no kernel of its own; delta beats delta-of-delta on this data
  anyway (C1), so it was not given one.

**A Linux profile of the queries.** The project toolchain on debian now has `perf`, which the version 3 queries lacked
(Gaps, above). `perf record -e cycles:u -g` over Q1 to Q5, five times each, with AVX2 at `43aa7d2`, 40,451 samples
(`debian/day/simd-day/perf-queries-report.txt`): `decode_dict_block` takes 41.8% of all samples, and the SHA-256 and row digest
of the answers, which `tick_query` computes outside `engine_ns`, take 24.3%. The dictionary decoder is therefore about 55% of
the engine's own time, the bound it is on the Mac. Of the kernels, the AVX2 linear unpack takes 1.2%, `decode_pfor_block` 1.0%
and `read_patches` 0.7%; CRC32C takes 1.1%.

### Decode targets under measure.sh

Every decode target, DS-1 to DS-9, measured at `bcbf873` through `scripts/run_bench.py` under each host's `measure.sh`, one
process per case, with the cases BENCHMARKS.md maps to each target ("How each decode target is measured", written before the
first run). Two benchmarks were added for it: `bench/column_decode_bench` (`7efbad3`) for the real-data targets, and in
`bench/simd_kernel_bench` (`bdf5e47`) the two 64-bit delta forms listed at every width 0 to 64, so the median across widths is
FastLanes' own statistic, and `dod_reconstruct` for DS-8. The harness took a third input kind, `column`, for them (`7c12c3e`).
Raw outputs, per case, are kept with the logs (`decode-targets/mac/`, `decode-targets/debian/`).

- macOS: Apple M4 Max, NEON, unpinned, no counters. Column cases at load 3.22 to 3.92 at start, kernel cases at 1.26 to 3.84;
  another project's `check.sh` was running before and during the column cases, and `measure.sh` admits a case below load 4.
  `results/bench/macOS-column_decode-bcbf8738d892.json`, `results/bench/macOS-simd_kernel-bcbf8738d892.json`.
- Linux: Zen 1, AVX2, `taskset -c 4`, perf `cycles:u`, `instructions:u` and `branch-misses:u` around the timed passes,
  `cache-misses` marked unavailable and not quoted, governor `performance`, turbo enabled, 3,562 to 3,577 MHz from cycles over
  wall time. Column cases at load 1.27 to 1.49 at start, cpu5 busy 0.64% median and 1.12% at most.
  `results/bench/Linux-column_decode-bcbf8738d892.json`.
- Commands, from the repository root, after `cmake --build --preset bench` on the Mac and the same Release `-O3 -DNDEBUG` build in
  `build/bench-ds` on debian (`heavy.sh`, `taskset -c 2,6,7`):
  `TICK_STORE_MEASURE=<measure.sh> python3 -I scripts/run_bench.py build/bench/bench/column_decode_bench` and the same for
  `simd_kernel_bench`; on debian with `--slice-path` to the slice there.

**The Linux kernel file was refused twice.** Both runs of `simd_kernel_bench` on debian finished all 314 cases and failed
`check_results.py` on the sibling rule: 4 cases over 5% in the first run (5.2% to 8.5%), 7 in the rerun (5.4% to 9.4%), the rest
under. A kernel case lasts about 1.1 s, so its window is about 117 jiffies of cpu5 and 6 busy jiffies pass the limit. During the
first run another project's test job (`aadp_run_oracle` under `ctest`) was using most cores; during the rerun the load was 0.86
to 1.44 and nothing else was seen. Six idle one-second commands under `measure.sh` read cpu5 at 0%, 0%, 0%, 1.68%, 0% and 2.52%,
so the wrapper's own start and end are not enough by themselves; what took cpu5 in the rerun is not established. The rule says
rerun once, so there is no reported Linux kernel file, and DS-4 has no reported figure. The two refused runs agree closely, and
their figures below are labelled as refused: they are what the pinned host measured, not results this repository publishes.

**Verdicts.** Ratios of ns a value between cases come from one results file each.

| ID | Target | Measured | Host, set | Verdict |
|---|---|---|---|---|
| DS-1 | Gorilla XOR on doubles at most 2.12 ns a value | 8.393 ns a value (CV 2.33%), 37.18 bits a value | macOS, NEON build, Gorilla is scalar | Missed, 3.96 times over |
| DS-2 | Gorilla decode at most 21.3 cycles a value, L1-resident | 60.91 `cycles:u` a value (CV 0.50%), 198.4 instructions, 0.419 branch misses | Linux, AVX2 build, Gorilla is scalar | Missed, 2.86 times over |
| DS-3 | 64-bit delta decode at least 3.6 G values/s | 3.35 G values/s, median over widths 0 to 64, two passes; fused 3.27 | macOS, NEON | Missed by 7.0% |
| DS-4 | Same kernel at most 0.377 cycles a value | No reported figure. Refused runs: 2.716 and 2.720 cycles a value fused, 2.950 and 2.955 two passes | Linux, AVX2 | Not judged: no reported file. The refused runs miss by 7.2 times |
| DS-5 | Price block decode at least 4,605 MB/s of 8-byte values | 3,674 MB/s (CV 0.86%) on macOS; 1,280 MB/s (CV 0.66%), 22.3 cycles and 47.0 instructions a value, on Linux | macOS NEON, Linux AVX2 | Missed on both: by 20.2% and 3.6 times |
| DS-6 | Layout at least 14 times faster than Gorilla on the same column | Timestamps 16.06 times (9.650 against 0.601 ns); prices 3.86 times (8.393 against 2.177 ns) | macOS, NEON | Met on timestamps, missed on prices |
| DS-7 | Layout speedup over Gorilla, no published figure | Timestamps 7.58 times (22.204 against 2.931 ns); prices 3.29 times (20.558 against 6.252 ns) | Linux, AVX2 | Measured, no target figure |
| DS-8 | Delta-of-delta reconstruct at least 8.20 GB/s of 32-bit values | 2.94 G values/s, 11.77 GB/s counted as 32-bit values, the restore copy included | macOS, scalar code, as every set runs it | Met, 1.44 times over the figure |
| DS-9 | Delta-of-delta decode with bit parsing, no published figure | Gorilla: 9.650 ns a value on macOS, 22.204 ns and 79.5 cycles on Linux. PFOR delta-of-delta: 0.880 and 3.151 ns | macOS NEON, Linux AVX2 | Measured, no target figure |

**Why each miss.**

- DS-1 and DS-2: the data, mostly. The source's 2.12 ns is 500,000 smooth synthetic values, and ALP's 21.3 cycles is its data
  sets on Ice Lake. Add-order prices as dollar doubles take 37.18 bits a value in the store's (locate, side) order, beside the
  37.7 published for Gorilla on Stocks-USA, because a price over 10^4 fills its mantissa (docs/NOTES.md, Gorilla on ITCH). On Zen 1 the
  L1 case runs 198 instructions a value at 3.3 instructions a cycle, so the cost is mainly instruction count, with 0.42 mispredicted
  branches a value on top; the whole column, out of L1, takes 73.4 cycles. The M4 decodes the same stream 2.45 times faster in
  wall time than Zen 1.
- DS-3: 7% short, against FastLanes on an M1, and the M4 is the faster core. The layout keeps values in value order across the 16
  lanes, so the inverse delta needs a prefix sum across lanes in the register; FastLanes' transposed layout does a vertical add
  instead (docs/NOTES.md, SIMD, "What the layout gave up"). 18 of the 65 widths reach 3.6 two-pass; one does fused. The iteration figure
  was 3.42; this is 3.35 at a higher load.
- DS-4: the same layout cost, on Zen 1, which splits every 256-bit operation in two and is slow at cross-lane permutes, against
  FastLanes on Zen 3. The refused runs put the AVX2 set's fused form at 2.72 cycles a value, as at iteration (2.71).
- DS-5: VIREL's figure is ALP on an EPYC 9554. Ours is the store's own column, 6.30 bits a value, through the reader's
  `decode_column`: per piece, any zstd frame inflated, a vector allocated, PFOR with its scale and patches decoded, and every value
  range-checked, then appended. The shares of those steps are not separated here. Zen 1 runs it at 2.1 instructions a cycle.
- DS-6 on prices: the store's price column is PFOR delta with an adaptive scale and zstd, 6.30 bits a value, and decodes at 2.18 ns,
  over three times the 0.601 ns of PFOR delta on timestamps; Gorilla on prices costs 8.39 ns, less than on timestamps. Timestamps
  are the column the iteration verdict used, and they meet the target; prices do not, and both are stated.

**Kernel figures beside the targets**, G values a second, median over the widths each ran (README table): on the M4, interleaved
unpack 7.78 NEON and 7.49 scalar, linear unpack 7.39 and 1.40, two-pass delta 3.35 and 2.02, fused 3.27 and 1.65, prefix sum 4.72
NEON in-register against 3.08 for the tree, Stream VByte 6.21 and 0.74. `restore_copy` alone, the 8 KiB copy DS-8's case makes before
each call, runs at 13.36 G values a second, so about a fifth of `dod_reconstruct`'s time is the copy. In the refused Zen 1 runs:
interleaved unpack 3.39 AVX2 against 2.81 scalar, two-pass delta 1.215 against 0.968, fused 1.313 against 0.962, Stream VByte
2.23 against 0.43, `dod_reconstruct` 1.80 at 1.98 cycles a value.

**What did not work.** The first `gorilla_xor_l1` took 16 vectors at 16 positions and kept the first few that fit 16 KiB, which put
them all near the start of the column; it now takes the largest count, up to 16, whose evenly spaced vectors fit, which is 3 on
this column (12,801 bytes). The Mac run waited 14 minutes before its first case, and again partway through the kernel cases, for
another project's `check.sh` to bring the load under 4.

### tick-store in the comparison (slice)

Slice only: the five-minute open of `12302019`, 6,125,294 messages, 206,138,339 source bytes. These
numbers prove the harness and do not claim anything about a full day; no target changes. Both
files are at `25a462b` under `measure.sh`, one system at a time, and pass `scripts/check_compare.py`:
`results/compare/Linux-12302019-open-25a462bc73c2.json` (pinned to cpu4 and cpu6, cpu5 busy 0.29% to
0.83% per system, load at start 1.39 to 1.49) and `results/compare/macOS-12302019-open-25a462bc73c2.json`
(unpinned, no counters, 24 MHz timer, load at start 1.45 to 3.80). A first pair at `67621b0` is
superseded: a clang-tidy fix on Linux changed the C++ after it, so both hosts ran again. Each
system's headline variant; queries on its class B store; median / p99 in ms, warm over 30 runs,
cold over 7.

Linux, Ryzen V1605B (Zen 1):

| Query | Parquet | ArcticDB | QuestDB | tick-store |
|---|---|---|---|---|
| Q1 warm | 54.84 / 86.09 | 27.40 / 55.03 | 57.38 / 284.27 | 118.55 / 147.68 |
| Q1 cold | 89.95 / 92.10 | 73.79 / 75.26 | 469.83 / 583.41 | 161.40 / 162.44 |
| Q2 warm | 243.31 / 289.56 | 139.66 / 171.13 | 27.65 / 143.45 | 10.62 / 25.84 |
| Q2 cold | 361.43 / 371.24 | 458.93 / 461.87 | 1168.44 / 1231.37 | 108.93 / 112.21 |
| Q3 warm | 149.63 / 194.07 | 96.26 / 114.07 | 16.32 / 101.68 | 4.75 / 5.43 |
| Q3 cold | 227.26 / 243.49 | 371.97 / 377.15 | 605.87 / 663.47 | 58.33 / 59.18 |
| Q4 warm | 47.42 / 79.34 | 6.16 / 6.79 | 1.71 / 7.27 | 53.59 / 56.94 |
| Q4 cold | 81.28 / 84.47 | 20.53 / 21.38 | 68.17 / 91.04 | 89.38 / 90.69 |
| Q5 warm | 25.00 / 43.59 | 49.20 / 77.42 | 2.90 / 14.07 | 33.19 / 42.63 |
| Q5 cold | 37.74 / 38.81 | 262.77 / 266.99 | 117.37 / 140.10 | 55.61 / 60.87 |

macOS, M4 Max:

| Query | Parquet | ArcticDB | QuestDB | tick-store |
|---|---|---|---|---|
| Q1 warm | 16.94 / 21.03 | 7.36 / 8.78 | 14.51 / 56.47 | 38.13 / 45.82 |
| Q1 cold | 21.31 / 21.56 | 18.14 / 18.65 | 91.48 / 92.68 | 55.29 / 56.81 |
| Q2 warm | 79.40 / 84.95 | 39.31 / 45.30 | 7.80 / 24.88 | 4.11 / 5.43 |
| Q2 cold | 86.45 / 87.24 | 114.49 / 116.03 | 319.49 / 321.90 | 23.45 / 23.81 |
| Q3 warm | 54.75 / 59.09 | 25.78 / 31.81 | 2.03 / 25.25 | 1.86 / 2.03 |
| Q3 cold | 59.54 / 61.17 | 85.52 / 90.01 | 117.11 / 119.05 | 13.36 / 13.43 |
| Q4 warm | 16.02 / 16.90 | 1.70 / 1.91 | 0.30 / 1.03 | 22.86 / 23.59 |
| Q4 cold | 20.61 / 21.28 | 6.51 / 7.05 | 10.78 / 12.77 | 36.48 / 38.41 |
| Q5 warm | 7.23 / 8.62 | 11.66 / 13.39 | 1.00 / 5.02 | 14.52 / 15.04 |
| Q5 cold | 10.54 / 10.98 | 59.05 / 60.16 | 23.14 / 24.16 | 19.91 / 20.69 |

**Lowest per query.** tick-store has the lowest median and p99, warm and cold, on Q2 and Q3 on
both hosts: the one-security queries, which read one locate's run. It has the slowest median of the
four on Q1 warm and on Q4 warm and cold on both hosts, and on Q5 warm on macOS, and it is not lowest
on Q5. Q1 and Q4 decode the segment's whole locate column to put records back in file order
(TASKS). ArcticDB has the lowest Q1 and Q4 cold on both hosts; QuestDB the lowest Q5 warm on both
and Q4 warm on macOS; on Linux QuestDB has the lowest Q4 warm median and ArcticDB the lowest p99,
so neither is lowest on both; Parquet has the lowest Q5 cold. The engine's own time sits 0.15 to
0.25 ms under each round trip on Linux and 0.02 to 0.10 ms on macOS, so the Python worker is not
what makes it slow. On macOS QuestDB runs its scalar `Vanilla` kernels.

**Ingest, median of three writes.** tick-store's write holds ITCH parsing, which the others'
writes do not; nothing is subtracted.

| Host, class | Parquet | ArcticDB | QuestDB | tick-store |
|---|---|---|---|---|
| Linux A | 1.863 s | 1.049 s | 4.127 s | 5.497 s |
| Linux B | 2.025 s | 1.212 s | 9.021 s | 5.618 s |
| macOS A | 1.046 s | 0.478 s | 0.804 s | 2.386 s |
| macOS B | 1.040 s | 0.518 s | 0.918 s | 2.394 s |

tick-store is the slowest writer in class A on both hosts and in class B on macOS, and second
slowest in class B on Linux, ahead of QuestDB. Its class B adds one flush of the file and one of its
directory, `fdatasync` and `fsync` on Linux and `F_FULLFSYNC` twice on macOS, 0.12 s and 0.008 s
over class A. QuestDB's class B is not like-for-like on either host.

**Stored bytes**, allocated at rest, ratio source over stored: tick-store is the smallest on both
hosts, 42,127,360 bytes on Linux (4.893) and 42,741,760 on macOS (4.823), from 42,126,581 apparent.
Parquet `zstd1-rg1Mi` takes 70,479,872 on Linux (2.925); on macOS `st_blocks` gives 86,265,856
(2.390) for its 70,478,425 apparent bytes, the open macOS `st_blocks` item in TASKS, where the first
pair at `67621b0` had read 70,479,872. ArcticDB `rows100k-cols127` takes 141,168,640 on Linux (1.460)
and 141,860,864 on macOS (1.453), QuestDB `plain` 537,550,848 on Linux and 537,772,032 on macOS
(0.383).

**Tuning variants.** Parquet with no codec, `none-rg1Mi`, stores 155,033,218 bytes apparent. Warm, it
is faster than the headline on every query, by 3% to 29% on Linux and 23% to 45% on macOS (Q2 warm
202.8 ms against 243.3, and 57.4 against 79.4). Cold, it is faster on every query on macOS, and on
Linux slower on Q1, Q4 and Q5, by 8% to 14%, with 2.2 times the bytes to read; the cause is not
measured. `ts` on its default dictionary encoding stores 74,022,380 bytes apparent against 70,478,425
with `DELTA_BINARY_PACKED`, and is 3% to 13% slower on Q1 to Q4 on both hosts, Q5 within 5% either
way. ArcticDB with one symbol per security writes in 14.95 s (A) and 133.7 s (B) on Linux, 5.85 s
and 216.9 s on macOS, stores 208,986,112 and 235,167,744 bytes, and answers Q2 warm in 8.4 and 1.8 ms
but Q1 warm in 5.77 and 1.05 s. The headlines stay as they were. Raw logs:
`phase3-raw/compare-slice/`.

### Layout experiments

This section measures layouts against each other on the full day, then picks the store's final
layout by the rule below. The rule was written and committed before any experiment was measured,
and it was not changed after. Every timing here is **iteration-only**: none went through
`measure.sh`.

**The decision rule.**

1. *Eligible.* A layout is eligible if its store of 12302019 keeps CR-1 (ratio at least 2.3415)
   and takes at most three quarters of Parquet's full-day stored bytes, 0.75 times 3,191,406,592,
   which is 2,393,554,944 bytes (a ratio of at least 3.447). That keeps it smaller than every other
   system with room to spare, so CR-2 holds too.
2. *Figures.* Each candidate is timed on the Mac on the whole day 12302019, indexed path, with every
   answer checked against the oracle: Q1 to Q5, warm median and cold mean, ten figures. Warm is the
   median of rounds 2 to 20 in one worker; cold is the mean of 10 trials, each a proven eviction of
   the store and then a fresh worker.
3. *Score.* On each figure, a candidate scores a point if its time is within 5% of the lowest
   eligible time on that figure. Mac runs differ by 1% to 6% from run to run, so a smaller gap is
   not a difference.
4. *Order of the experiments.* The experiments change one thing at a time from the current
   layout, version 3 at N = 2^20. Within each experiment the rule picks that dimension's value,
   holding the others at their current choice. The combination of the winners is then timed and
   must itself win the rule against each experiment's winner and the version 3 baseline;
   otherwise the best of those wins.
5. *Finalists.* The candidates within one point of the top Mac score are finalists. They are timed
   on debian too, through `heavy.sh` with `taskset -c 2,6,7`, and scored the same way on its ten
   figures. The finalist with the most points over both hosts wins.
6. *Ties.* A tie on points goes to the smaller store.

The other systems' slice figures and the debian timings are compared in the table, but they do not
enter the score: the slice is a different store, and the other systems were not run on the whole
day.

**How each candidate was timed.** `build/layout/cand.sh` (scratch, not tracked) ingests 12302019
with `page_cache` durability through `heavy.sh` under `/usr/bin/time -l`, writes the layout, then
runs the query timing script (the one "Query iteration" used: 20 warm rounds, 10 cold trials per
query, each answer's rows and SHA-256 checked against `truth-12302019.json`) through `heavy.sh`
under `/usr/bin/time -l`. Writer memory is the ingest's peak memory footprint. Raw output is in
`phase3-raw/layout/<candidate>/`. Latencies are in ms, Q1 / Q2 / Q3 / Q4 / Q5.

| Experiment | Option | Bytes and ratio | Writer memory | Q1–Q5 warm and cold, Mac and debian, iteration-only | Chosen? |
|---|---|---|---|---|---|
| 1, segment size | N = 2^14, version 3 tools at `8ac4aab` | 2,024,871,931, 4.075 | 93 MB | Mac warm 38.04 / 622.65 / 344.59 / 1.02 / 693.20; cold 51.77 / 1,190.15 / 858.70 / 1.92 / 1,998.14 | No: 2 points (Q4) |
| 1 | N = 2^16 | 1,903,523,711, 4.335 | 52 MB | Mac warm 32.34 / 226.69 / 103.01 / 2.18 / 559.07; cold 42.80 / 698.50 / 522.63 / 3.33 / 1,681.87 | No: 2 points (Q1) |
| 1 | N = 2^18 | 1,819,101,831, 4.536 | 173 MB | Mac warm 32.30 / 88.62 / 38.17 / 6.07 / 517.94; cold 43.79 / 422.44 / 275.56 / 9.28 / 922.05 | No: 3 points (Q1, Q5 warm) |
| 1 | N = 2^20, version 3 as it was (baseline) | 1,750,915,467, 4.713 | 401 MB | Mac warm 37.10 / 42.34 / 19.94 / 22.75 / 494.52; cold 55.83 / 215.12 / 114.78 / 38.63 / 665.33 | No: 2 points (Q5) |
| 1 | N = 2^22 | 1,707,376,132, 4.833 | 1,236 MB | Mac warm 129.00 / 30.82 / 17.29 / 100.78 / 528.86; cold 188.91 / 109.74 / 56.07 / 156.99 / 644.71 | Won experiment 1: 5 points (Q2, Q3, Q5 cold) |
| 2, locate access for time windows | N = 2^20, version 3 (baseline), rounds 3 and 4 | 1,750,915,467, 4.713 | 401 MB | Mac warm 40.19 / 45.61 / 21.40 / 24.78 / 535.51; cold 60.16 / 220.13 / 117.88 / 40.67 / 699.63 | Won experiment 2: 8 points (Q1, Q2 warm, Q3, Q5) |
| 2 | A file-order locate column, `file_locate`, in 4,096-value blocks, ranked PFOR or Huffman; the window narrowed to 64-record chunks by the time bases where a segment's times ascend; each run scanned from its start, its records before the window read for the chain and the list alone | 2,128,851,992, 3.876 | 402 MB | Mac warm 41.49 / 45.37 / 22.57 / 11.82 / 536.35; cold 65.96 / 228.91 / 129.53 / 29.40 / 748.45 | No: 6 points (Q1 warm, Q2, Q4, Q5 warm) |
| 2 | The same, with move-to-front allowed in `file_locate` | 2,063,523,061, 3.999 | 393 MB | Mac warm 43.91 / 45.39 / 22.50 / 11.82 / 534.75; cold 66.62 / 230.14 / 128.55 / 29.98 / 749.93 | No: 5 points (Q2, Q4, Q5 warm) |
| 2 | The `file_locate` store with every column in 1,024-value codec blocks | 2,168,712,948, 3.805 | 394 MB | Mac, one run, load 10: warm 46.59 / 41.33 / 24.31 / 9.14 / 760.76; cold 71.07 / 232.32 / 185.39 (one trial 651 ms, median 133.48) / 26.89 / 977.56 | No: dropped before the side-by-side rounds, Q5 42% slower for Q4's 3 ms |
| 2 | A per-group offset directory: each locate's first record in each 4,096-record file group | not built: 50,911,193 (group, locate) pairs on 12302019, 776 a group, 0.19 a record | | not timed | No: a query still reads each run from its start (below) |
| 3, hot-column codecs | Baseline, as in experiment 2 | 1,750,915,467, 4.713 | 401 MB | as above | No: 2 points (Q2, Q3 cold) |
| 3 | The eight hot dictionary columns (`type`, `ref_distance`, `add_side`, `add_shares`, `reduce_shares`, `replace_shares`, `trade_side`, `trade_shares`) held to ranked PFOR: ranks bit-packed at one width a PFOR block, no Huffman or move-to-front | 1,808,255,218, 4.563 | 395 MB | Mac warm 36.24 / 35.98 / 18.30 / 20.84 / 289.99; cold 56.52 / 216.88 / 115.57 / 35.80 / 472.04 | Won experiment 3: 10 points |
| 3 | An interleaved multi-stream Huffman decode | not built | | not timed | No (below) |
| Final round | N = 2^20, version 3 (baseline), rounds 5 and 6 | 1,750,915,467, 4.713 | 401 MB | Mac warm 41.36 / 46.56 / 21.85 / 25.23 / 543.57; cold 62.44 / 226.00 / 118.75 / 41.48 / 710.29 | No: 0 points |
| Final round | N = 2^22, experiment 1's winner | 1,707,376,132, 4.833 | 1,236 MB | Mac warm 128.91 / 31.11 / 17.30 / 99.95 / 527.07; cold 187.25 / 109.61 / 55.84 / 157.26 / 652.02 | No: 1 point (Q2 cold) |
| Final round | Hot columns ranked PFOR at N = 2^20, experiment 3's winner | 1,808,255,218, 4.563 | 395 MB | Mac warm 37.35 / 36.89 / 18.69 / 21.36 / 297.81; cold 56.38 / 216.93 / 115.35 / 35.69 / 473.31 | No: 4 points (Q1, Q4) |
| Final round | The combination of the winners: N = 2^22, version 3's locate access, hot columns ranked PFOR | 1,758,234,758, 4.693 | 1,238 MB | Mac warm 105.98 / 25.61 / 14.79 / 78.05 / 280.19; cold 162.43 / 106.09 / 52.81 / 130.00 / 417.00. debian at `1669ae0`: warm 287.02 / 73.98 / 35.53 / 205.03 / 911.95; cold 382.65 / 451.04 / 216.11 / 287.33 / 1,141.86 | **Yes**: 6 points (Q2, Q3, Q5), the only finalist |
| Not a candidate | N = 2^22 with the `file_locate` column and ranked PFOR, which combines experiment 2's loser | 2,136,172,569, 3.863 | 1,272 MB | Mac warm 97.96 / 26.14 / 16.18 / 18.77 / 286.34; cold 176.29 / 110.28 / 57.95 / 78.68 / 414.43 | Not scored: the rule takes each experiment's winner. Entered, it would have had 5 points (Q2, Q4 warm, Q5) |

**Experiments 2 and 3, timed side by side.** Machine load from other work moved single runs by up
to 10%, so experiments 2 and 3 were timed again in interleaved rounds: `build/layout/batch.sh`
(scratch) runs every candidate's timing once per round, in turn, through `heavy.sh` under
`/usr/bin/time -l`, the same script, warm rounds and cold trials as above, raw output in
`phase3-raw/layout/batch/<candidate>-<round>.txt` with the load before each in `uptime.txt`. Each
figure in a row timed this way is the mean of its two rounds; load averages were 2.3 to 3.0 over
rounds 3 and 4. Rounds 1 and 2 ran the `file_locate` stores under a first query engine whose scan of
the records before the window took three passes; it was 5% to 6% slower on Q1 and Q2 than version
3's single pass on the same store. One pass for the chain and the list, and reference values held
directly from the first row that can be emitted on, brought Q2 back level, and rounds 3 and 4 used
that engine. Version 3's own engine was kept for the version 3 stores: the same change on them made
Q1 2% to 6% and Q3 5% slower and Q4 no faster, because there the whole `locate` column is decoded
anyway.

*Experiment 2.* The file-order column halves Q4, 24.8 to 11.8 ms warm and 40.7 to 29.4 ms cold, and
costs 378 MB. It loses on everything else: Q3 warm 5.5% slower, Q1, Q2, Q3 and Q5 cold 4% to 10%
slower. Q5 reads no locate column at all, and its warm time is level (535.5 against 536.4 ms), so the
cold loss is the file: 22% more bytes put the share columns' pieces further apart on the disk. Under
the rule the baseline takes 8 points to 6, so version 3's locate access stays. Move-to-front saves
65 MB of the 378 and costs Q1 5.8% (the list shift on every value). The 1,024-value blocks cut what
Q4 decodes from each run's start, 9.1 ms warm, but make four times the blocks, each with its header
and its own setup, in every column: Q5 is 42% slower. The per-group directory was not built: the store's references and
its time and index chains restart only at a run's start, so a query over every locate must still
read each run from its first record, whichever group it starts in. The directory could only replace
the search of the `locate` column, which the `file_locate` candidate already removes, at 0.19
entries a record. The models in `build/layout/q4model.py` (scratch) over the export agree: at
N = 2^20, Q4's window reaches all 256 `type` blocks of its segment from the 4,096-record file groups,
and 111 of 256 from the 64-record chunks. The full diff of the `file_locate` store, version 4 with
its own checks and tests, is kept with the raw logs (`phase3-raw/layout/v4-file-locate-tree.diff`).

*Experiment 3.* Holding the hot dictionary columns to ranked PFOR costs 57 MB (3.3%) and is faster on
all ten figures: Q5 warm 535.5 to 290.0 ms, Q2 45.6 to 36.0, Q3 21.4 to 18.3. A Huffman block
decodes one code at a time, each code's length depending on the last; ranked PFOR unpacks a block at
one width with the SIMD kernels and maps ranks through a table. An interleaved multi-stream Huffman
decode was not built: it would still be a variable-length decode, so it could at best approach
ranked PFOR's speed, and the rule scores time, with size only for eligibility and ties, where the
57 MB cannot matter (both are far inside 2,393,554,944 bytes). The fixed-width dictionary code that
TASKS named is ranked PFOR itself: ranks at one width per block, with patches for outliers.

*Experiment 4, the other layout items in TASKS.* "Per-locate against file order" is experiment 2
and the step from version 2 to 3: version 2's file order cost Q2 and Q3 50 to 60 times what
version 3's runs do ("Query iteration"), and a file-order locate column beside the runs lost the
rule. "Separate columns per message type against shared columns" was answered without a build.
Every field one type alone has (add, trade, cross, imbalance, replace) already has its own columns.
The fields several types share are either in every typed record (timestamp, locate, tracking), so a
split would only cut one run into one sub-run per type that the reader merges back by the `type`
column it reads anyway, or grouped by type inside the segment (`ref_distance`, `reduce_shares`,
`tracking`), so each type's values are already one range, found from the seek counts. Q1 to Q5 read
every type of the records they reach, so separate columns would add pieces and block headers and
take nothing from any query's reads.

*Experiment 5, what the profiles show.* On the Mac, `sample` of the final store: Q5 spends about 85%
of its samples in the dictionary decoder, half mapping ranks back to values and half in the nested
PFOR unpack and its patches (`phase3-raw/layout/prof/prof-q5-dpfor.txt`). Q4 on the final store
(`prof-q4-final.txt`, 5,999 samples): 20% inflating zstd pieces (the `locate` column, creation
references and add prices are zstd-wrapped, and the whole `locate` column of the segment is read),
8% rebuilding every record's position from it, 12% in PFOR unpacking and patches, 9% in dictionary
decoding, 10% in the run scans and 11% in the window's search of each run. The file-order column
of experiment 2 removes the first two, which is its halving of Q4; the rest is reading each of
about 9,000 runs from its start, for its reference list and its time and index chains
(`prof-q1-combo22.txt` shows the same for Q1 on the `file_locate` store). On debian, `perf record -e
cycles:u` of the same store at `1669ae0` (flat, `phase3-raw/layout/debian/perf/`): Q4's top symbol is
`positions_from_runs`, 14.9%, then column access 8.8%, dictionary decoding 8.3% and the run scans
7.1%; Q5 is 30.0% dictionary decoding, 11.5% column access and 18% PFOR unpacking and patches, with
about 15% at addresses perf could not resolve to a symbol. Neither host suggested a change that fits this
round; both are TASKS items.

**The final layout, chosen by the rule.** Store version 3, unchanged: no byte means anything new, so
every version 3 file still reads and the version is not raised. The writer's defaults change:
segments of 2^22 records, and the eight hot dictionary columns held to ranked PFOR (each column's
`dict_modes` in `itch_store.hpp`; the reader takes every mode). The golden file is rewritten and
the `07b020e` one kept beside it, which the reader must also read. The combination of the winners
took 6 points against 4, 1 and 0, so it is the only finalist; it was timed on debian as rule 5
asks, with nothing to score against there.

- `12302019`: 1,758,234,758 bytes, ratio 4.693 (CR-1 and CR-2 pass, 0.55 of Parquet's full day).
  `10302019`: 1,935,356,610 bytes, ratio 4.668. The store written on debian at `1669ae0` is
  byte-for-byte the Mac's (SHA-256 `350936cf...5b63`).
- Writer: peak footprint 1,238 MB on `12302019` and 1,358 MB on `10302019` on the Mac, against
  395 MB at 2^20; debian's ingest VmHWM 7.68 GB, most of it the source's mapped pages. Verify's
  footprint 1,288 MB and 1,484 MB.
- G-1 passes on both days on the Mac and on `12302019` on debian (VmHWM 10.99 GB, mapped pages of
  source and store, from the tool's own reading). G-2 is caught on both days: a data block flip and a
  footer flip, each with CRCs on (the block's or the footer's CRC, naming the offset) and off (the
  oracle names the record and bit for the data flip; for the footer, the container's extent check
  on `12302019` and the decoder's codec check on `10302019` name the field). On `10302019` the footer's middle byte fell in the last byte
  of a block entry's CRC32C: with CRCs off nothing reads that field, so verify passed, and the data
  was intact. The footer flip moved one byte on, to the entry's codec id, which the decoder refused.
  Both attempts' files are in `phase3-raw/layout/store/10302019/`.
- The slice store gives every digest of `results/compare/*.json`, both files, both paths, 5 of 5.
  The full-day store gives the oracle's answer to Q1 to Q5 and the busiest locate on both paths.
- Fuzzing at `1669ae0`, seeds from the real encoders, 300 s each through `check.sh`: `fuzz_dictionary`
  18,405 runs (its oracle now re-encodes every block under the ranked-PFOR mode set too),
  `fuzz_itch_query` 52,926 and `fuzz_itch_store` 663,308, no failure. The compare module's
  real-system tests pass on both hosts, 11 of 11.
- Faults for the new checks, each shown failing by `prove_checks.py --only`: the encoder trying
  Huffman whatever its mode set (`fuzz-dictionary-mode-set-ignored`) and taking Huffman when it is
  larger than ranked PFOR (`fuzz-dictionary-mode-set-larger-block`) are caught by the dictionary
  oracle in `fuzz_dictionary`, exit 77; the encoder held to ranked PFOR refusing a block
  (`dict-pfor-only-refuses`) stops the writer and the seed generator before any fuzzing, so it is
  proved by the dictionary unit tests, exit 8. All three passed their baselines. Every earlier fault
  patch still applies.

**Likely verdicts, iteration-only.**

- **QL-1** (Q2 warm at most 16.9 ms): misses, 25.6 ms on the Mac and 74.0 ms on debian on the whole
  day. On the slice Q2 took 2.7 ms in the digest check.
- **QL-2** (Q3 warm at most 0.41 ms): misses, 14.8 and 35.5 ms.
- **QL-3** (Q4 cold mean at most 10 ms): misses by more than before, 130.0 ms on the Mac and 287.3 ms
  on debian, against 41.5 ms (version 3 at 2^20, final round) and 105.2 ms (`e257f80`). The rule scores each figure alike, and N = 2^22 won
  the three Q2, Q3 and Q5 figures in each of warm and cold while losing Q1 and Q4.
- **QL-4** (lowest median and p99 of the four systems on every query): fails. The slice store holds
  two segments at 2^22; in the digest check Q1 took 113 ms and Q4 74 ms, against 38 and 23 ms warm
  for version 3 at 2^20 in the comparison, which was already beaten there. Q2 (2.7 ms) and Q3
  (1.4 ms) should stay the lowest. Q5 was QuestDB's warm and Parquet's cold, and stays so likely.
  The next comparison run measures it.

**What did not help.** The file-order locate column, with or without move-to-front, and 1,024-value
blocks (experiment 2); the single-pass prefix scan on version 3 stores (no faster Q4, slower Q1 and
Q3); a per-group directory (not built: it cannot start a run's scan mid-run); segments below 2^20
(Q2 and Q3 2 to 17 times slower warm, Q5 up to 3 times cold, for a smaller Q4); a multi-stream Huffman decode (not built,
above).

### Full-day comparison

The full day ran to the end at `67ae574`, and the verdicts are in "The full day at `67ae574`"
below, read from `results/compare/Linux-12302019-full-67ae574f5666.json` alone. Every attempt in
the table is lost and none of its parts is used. Up to `dd5de38` the guard stopped a run on swap
traffic over 10 MiB/s for three 2-second samples; the rule from `e0b7df2` on is in "The guard's
swap rule" below.

| Commit | Where it stopped | Why |
|---|---|---|
| `193177b`, `953f3d4` | Parquet's warm Q2, three failed attempts each | Warm proof on the device's machine-wide read count (fixed at `e2ad871`) |
| `e2ad871` | ArcticDB's first read-back, then `rows10M-cols127`'s write | Swap traffic of 18 to 30 MB/s, then MemAvailable under 3 GiB (fixed at `dd5de38`) |
| `dd5de38`, 2026-11-05 | QuestDB `plain`, class A write, 345 s in | Swap traffic 30.3, 71.5 and 23.4 MB/s |
| `dd5de38`, 2026-11-09 | QuestDB `plain`, class A read-back, 641 s in | Swap traffic 20.1, 80.7 and 15.0 MB/s |
| `e0b7df2`, 2026-11-10 | QuestDB `plain`, evicting its class B store before the warm queries, 3,249 s in | 14,080 pages still resident after 16 eviction passes 20 ms apart (fixed at `4b5ade4`) |
| `4b5ade4`, 2026-11-10 | QuestDB `plain`, the Parquet conversion after its queries, 4,470 s in | `wait_wal_table` hit QuestDB's 60 s query timeout while the day's partition converted (fixed at `7a2e092`) |
| `7a2e092`, 2026-11-10 | QuestDB `plain`, cold Q2, the fifth repetition's eviction, 4,245 s in | 27,083 pages still resident after 32 passes, in the two files the host's backup was reading (fixed at the next commit) |

The first `dd5de38` run took the systems in their usual order, and Parquet (5,569 s) and ArcticDB
(16,234 s) finished under the guard before QuestDB stopped. The second ran on a freshly booted host
with QuestDB first, so that a repeat would cost minutes and not six hours. It did repeat.

**The two QuestDB stops are one effect.** Both times, free swap went to nothing within about 6 s:
from 651 MB to 12 KB, then from 654 MB to 16 KB. MemAvailable stayed between 11.6 and 12.7 GB. What
went to swap was QuestDB's own JVM. The write's server held 648 MB in swap on 2026-11-05. On
2026-11-09 the write's server held 926 MB and the read-back's 651 MB, beside 10.3 GB of the store's
pages mapped and 1.5 GB anonymous. The session's processes together peaked at 13.8 GB of RSS on
this 15.7 GB host. QuestDB's heap is the fixed `-Xms1g -Xmx1g` of Phase 3, and the capacity check
found at most 300 MiB of it in use. The kernel pages that idle heap out to keep the store's mapped
pages, since the 23 GB store does not fit in memory, and the free swap fills in one burst. The
capacity check passed because swap was already full before it started, so it had nothing to fill.
Swappiness is 60 and swap is 975 MB. The cgroups set no memory limit.

No harness change answers this. Raising or dropping the guard's swap limit weakens a check. A
different heap is not the setting the capacity check proved. Swap and swappiness belong to the
host. The run waits for a decision on one of them. Raw logs:
`phase3-raw/compare-fullday/`.

#### The guard's swap rule, changed before the rerun

This was written and committed before any run under the new rule.

**The decision.** The user keeps debian's swap and swappiness as they are. The guard changes,
and QuestDB's heap stays `-Xms1g -Xmx1g`. The guard exists to keep the host from running out of
memory or thrashing. Moving idle anonymous pages to swap while about 12 GB is still available is
neither. It is the kernel's ordinary reclaim: it keeps the store's mapped pages, which are in
use, and moves out pages nobody has touched. The old rule counted swap-in and swap-out together,
so it could not tell that apart from thrashing. Shrinking the heap would only satisfy the guard.
It would move the system under test away from the setting Phase 2 checked, and it would not help
anyway: under a 23 GB mapped store the kernel would still page out other processes' idle pages.

**What the samples show.** The guard's CSVs record swap traffic as pswpin and pswpout added
together. Every page written out takes a free swap slot, so the fall in SwapFree over a sample,
over 2 s, is at least the swap-out rate. That makes the total minus that fall an upper bound on
the swap-in rate. Over the nine sample files in `phase3-raw/compare-fullday/`:

| Run | How it ended | Highest total, MiB/s | Swap-in bound, highest single sample, MiB/s | Swap-in bound, highest held for 3 samples, MiB/s | Lowest MemAvailable, GiB | Lowest SwapFree, MiB | Lowest MemAvailable with swap exhausted, GiB |
|---|---|---|---|---|---|---|---|
| `193177b` Parquet | warm proof | 1.1 | 0.09 | 0.00 | 11.24 | 839 | never exhausted |
| `953f3d4` Parquet | warm proof | 1.9 | 0.12 | 0.01 | 10.63 | 827 | never exhausted |
| `e2ad871` Parquet | finished | 3.5 | 1.52 | 0.11 | 11.15 | 789 | never exhausted |
| `e2ad871` ArcticDB | guard, swap | 28.4 | 28.40 | 0.14 | 10.97 | 681 | never exhausted |
| `e2ad871` ArcticDB, second | guard, MemAvailable | 64.5 | 24.54 | 0.13 | 2.75 | 581 | never exhausted |
| `dd5de38` Parquet | finished | 15.6 | 15.59 | 2.15 | 12.34 | 660 | never exhausted |
| `dd5de38` ArcticDB | finished | 41.1 | 41.11 | 0.49 | 10.21 | 436 | never exhausted |
| `dd5de38` QuestDB | guard, swap | 68.1 | 1.42 | 0.60 | 8.04 | 0.01 | 11.81 |
| `dd5de38` QuestDB, second | guard, swap | 107.8 | 4.25 | 0.38 | 8.52 | 0.01 | 8.52 |

Across both QuestDB stops, at most 98 MiB of the 1,125 MiB of traffic could have been swap-in.
The second stop gives an independent check. debian has not rebooted since, and its counters
since boot read 60,735 pages swapped in and 203,092 out: 237 MiB in and 793 MiB out. That covers
the stop's 861 MiB and everything after it, so most of the stop was swap-out. The capacity
check's lowest MemAvailable was 6.07 GiB, with swap full the whole time. debian has 15.33 GiB of
memory and 976 MiB of swap on an SSD, with 4 KiB pages.

**The new rule.** It samples every 2 s, as before. It stops the run on any of these:

- **(a)** MemAvailable under 3 GiB, the guard's existing floor, unchanged, with the existing 2 GiB
  hard floor for SIGKILL. The lowest in any run that finished was 10.21 GiB. The one run this
  floor stopped, `e2ad871`'s ArcticDB, was a day-sized frame, which was a bug.
- **(b)** Sustained swap-in: pswpin from `/proc/vmstat` over 10 MiB/s, 2,560 pages/s, for 3
  consecutive samples (6 s). The rate and the count are the old rule's, now applied to swap-in
  alone. The highest swap-in held for 3 samples in any of the nine files is at most 2.15 MiB/s,
  so the limit sits 4.6 times above it. Single samples reached 41 MiB/s in runs that finished,
  which is why one sample is not enough. At 10 MiB/s, the whole 976 MiB swap device would be
  read back in 98 s. A working set cycling through swap that fast, for 6 s, is thrashing.
- **(c)** Swap exhausted with too little memory left: SwapFree under 2% of SwapTotal (19.5 MiB on
  debian), and MemAvailable under floor (a) plus SwapTotal, which is 3.95 GiB on debian. With swap
  empty, floor (a) leaves 3 GiB available plus 976 MiB of swap. With swap exhausted, this floor
  leaves the same headroom in memory alone. Swap was exhausted only in the two QuestDB stops and
  in the capacity check. MemAvailable was then at least 8.52, 11.81 and 6.07 GiB, so none of those
  runs would have stopped. Every other run kept at least 436 MiB of swap free.
- Every stop the guard already had: free disk under 50 GiB, and SIGTERM or SIGINT to the guard.

**Swap-out alone no longer stops a run.** The guard samples swap-in and swap-out separately.
`compare/run_system.py` records each as host-wide page counts per phase, for each variant and
class: write, read-back, warm queries and cold queries. The counts come from `/proc/vmstat` on
Linux and from `vm_stat`'s Swapins and Swapouts on macOS. `scripts/check_compare.py` refuses a
results file without them.

**What the rule still catches:** memory running out, with or without swap left; thrashing, by
the swap-in it causes; a full disk.

**What it now lets through:** swap-out of idle pages, at any rate, until the device is full.
That includes other processes' idle pages: a service on the host, or another project's session,
may pay for it later in swap-in latency when it next touches them. It also lets the swap device
fill, as it did in both stops, so for the rest of the run nothing else on the host can swap out.
The device frees again as the pages come back or their owners exit.

**Swap-in during a timed query is flagged, not rerun or refused,** the same way for all four
systems. Each query's worker reads pswpin and pswpout just before and just after its timed
repetitions: all 30 warm ones together, or the accepted attempt on a full day, and each cold
run. The results record those counts under each query's warm and cold blocks as `swap_timed`,
with `flagged` true whenever any page was swapped in. The full-day table and the notes show
every flagged number beside its value. A verdict that rests on one says so.

Flagging is the choice because pswpin is host-wide. It cannot say whose page came back, so a
rerun could chase another process's swap-in for as long as that process kept doing it, and
refusing would throw away an 11-hour run over a page the query may never have touched. A flag
keeps the number and puts the doubt next to it.

**Phase 2's capacity check.** Its heap verdict covers garbage collection and running out of
memory: no collection after start-up, and at most 300 MiB of the heap in use. But swap was full
before it started and stayed full, so it never tested what the kernel does with an idle heap
while swap has room. That is what stopped both `dd5de38` runs.

#### The first three runs under the new rule, lost to the harness

The first full day under the new rule ran at `e0b7df2`, the commit that implemented it, with
QuestDB first. The guard let QuestDB through both writes and both read-backs. It saw swap-out up
to 103.7 MiB/s and free swap at 0 for 178 samples, with MemAvailable at least 8.36 GiB
throughout. Swap-in reached 35.1 MiB/s in one sample and stayed over 10 MiB/s for at most two
samples in a row, both times at the start of a phase. The most it held for three samples was
2.78 MiB/s, the same order as the samples behind the rule. Over 3,249 s, 758 MiB came back from
swap and 1,545 MiB went out. The JVMs held up to 755 MiB in swap.

Then the harness failed, not the guard. QuestDB's queries had never run on a whole day, because
every earlier attempt stopped first. Before the warm queries, the class B store, 22.9 GB in 312
files, must be evicted from the page cache and proven at 0 pages resident. `tick_coldcache evict`
still had 14,080 pages resident after its 16 passes, 20 ms apart, and the run stopped. With
nothing running afterwards, the same store took 14 passes. A 2 GB file of it, freshly read
through, took 1 pass three times out of three. So it is the pages the store's own run left
behind that take seconds to become droppable. My guess at why is ext4 releasing their buffers
only at a later journal commit; I have not shown that. At `4b5ade4` the wait between passes
doubles from 20 ms up to 1 s, for at most 32 passes, about 27 s of waiting in all. The proof is
unchanged, and every page must still go. The run is lost and none of it is used: both slice
files and the full day were run again at `4b5ade4`. Raw logs:
`phase3-raw/compare-fullday/debian-e0b7df2-questdb-eviction-lost/`.

At `4b5ade4`, QuestDB went further. Its store evicted, and it ran all its warm and cold queries.
The guard again let swap fill: free swap was at 0 for 76 samples, with MemAvailable at least
8.61 GiB, swap-out up to 41.9 MiB/s, and swap-in over 10 MiB/s for at most two samples in a row.
Over 4,470 s, 789 MiB came in from swap and 1,340 MiB went out. Then the step after the queries
failed. That step converts QuestDB's partition to its own Parquet, a size the docs say to report,
and no target. The ALTER returns at once, and the conversion is applied through the WAL. One
`wait_wal_table` call then hit QuestDB's 60 s query timeout, with a 2.36 GB Parquet file
part-written. The day's ingest already handles this: it polls the WAL state down before its
last `wait_wal_table`. At `7a2e092` the conversion does the same, for at most an hour. On the
failed run's store, iteration only, the conversion then finished in 184 s, giving one Parquet
partition of 5,622,818,382 bytes. This run is lost too, and the slices and the full day were run
again at `7a2e092`. Raw logs:
`phase3-raw/compare-fullday/debian-4b5ade4-questdb-conversion-lost/`.

At `7a2e092`, QuestDB's writes, read-backs, warm queries, every cold Q1 and four cold Q2 runs
passed. Then the eviction before the fifth cold Q2 left 27,083 pages resident after its 32
passes: 13,643 in `seq.d` and 13,440 in `rest.i`, and no other file. Nothing of ours was running.
The guard showed 14.1 GiB available, no swap traffic and 0.02 GiB of session memory. The cause
was not QuestDB. `debian` runs an hourly backup as root with read access to the home directory.
I cannot read its include list, but its run that hour started at 11:25:39 and ended at
11:29:01, and the eviction failed from 11:26:57 to 11:27:49, inside it. The store was about 20
minutes old, so all 23 GB of it was new to a backup. The two files are next to each other in name order: a backup reading two files at
a time, in name order, refills both as fast as they are evicted. With the backup finished, the same store, on
the same queries, cold Q1 and Q2 four times each through the same server start and stop,
evicted in one pass all eight times. The `e0b7df2` loss above, at 03:45, may have been the
same: its eviction did not name its files, and I cannot read the backup's log.

A reader that refills the store also makes a cold run less than cold. Pages it brings in
between the eviction and the query are pages the query does not read, and the device counter,
being machine-wide, counts the backup's reads as the query's. So at the next commit an eviction
is proven only if the store still reads 0 one second later with nothing of ours running. If
not, it waits, 5 s and doubling to 60 s, for at most 30 minutes. After each cold run, with the
service stopped, the store must not gain a page over one second. If it does, the run is not
counted and runs again, at most 10 times per query, and the pages gained are kept in the cold
block. Both are new checks, each shown failing on its own fault. A backup that reads only other
files still competes for the device; that is the host's noise, as before, and not measured.
This run is lost, and the slices and the full day were run again at that commit. Raw logs:
`phase3-raw/compare-fullday/debian-7a2e092-questdb-backup-lost/`.

The slice pair at `67ae574` met both checks at once. On `debian`, the backup ran from 12:25:54 to
12:27:05, and at 12:26:21 ArcticDB `rows100k-cols1`'s store gained 16,442 pages over a second
with nothing of ours running, after a cold Q1. That repetition was run again. On the Mac three
cold repetitions were run again, after stores gained 107, 131 and 654 pages: ArcticDB Q3 and Q2,
and QuestDB `locate-index` Q1. I have not found what read them. So slice files before `67ae574`,
on both hosts, may hold cold repetitions that ran beside another reader. None of them is used
for a verdict below.

#### The full day at `67ae574`

`results/compare/Linux-12302019-full-67ae574f5666.json`, at `67ae574`, the commit of the slice pair
whose Linux file chose the variants. It passes `scripts/check_compare.py`. Day `12302019`:
268,744,780 messages, 8,251,407,909 source bytes, nothing clipped. Each system ran alone under
`measure.sh`, pinned to cpu4 and cpu6, QuestDB first, from 13:26 to 21:58 MST on 2026-11-10:
QuestDB 10,030 s, Parquet 5,694, ArcticDB 11,548, tick-store 3,432, 8.53 hours in all. Load at
start 1.60, 1.38, 1.19 and 1.41, at end 1.38, 1.19, 1.73 and 0.80, in that order. cpu5, cpu4's
sibling, was busy 3.32%, 1.73%, 3.38% and 1.44% of each system's window. The guard never fired,
and no cold repetition ran again: each eviction was proven at its first attempt, the longest
taking 3.8 s with the one-second quiet check.

Queries run on each system's class B store only, so no query has a class A number. Each system's
headline variant, median / p99 in ms, warm over 30 runs and cold over 7. `(swap-in)` marks a
measure with any page swapped in, host-wide, while it was timed; it is kept, as registered.

| Query | Parquet `zstd1-rg1Mi` | ArcticDB `rows100k-cols127` | QuestDB `plain` | tick-store `v3-seg4Mi` |
|---|---|---|---|---|
| Q1 warm | 66.45 / 97.79 | 37.41 / 67.98 | 58.57 / 95.96 (swap-in) | 297.58 / 325.72 |
| Q1 cold | 118.82 / 141.25 | 83.18 / 89.98 | 550.62 / 588.18 | 406.96 / 461.20 |
| Q2 warm | 2,702.10 / 2,808.72 (swap-in) | 2,050.34 / 2,475.90 (swap-in) | 319.30 / 364.42 (swap-in) | 74.32 / 83.95 |
| Q2 cold | 3,957.57 / 4,081.60 (swap-in) | 5,190.59 / 5,211.48 | 17,592.34 / 17,607.18 (swap-in) | 485.13 / 500.41 |
| Q3 warm | 1,638.97 / 1,707.02 (swap-in) | 1,175.26 / 1,224.26 (swap-in) | 68.71 / 85.62 (swap-in) | 35.81 / 36.53 |
| Q3 cold | 2,112.55 / 2,513.76 | 4,389.00 / 4,475.13 | 7,613.49 / 8,834.35 (swap-in) | 214.40 / 226.00 |
| Q4 warm | 60.75 / 99.96 | 6.95 / 8.84 | 1.18 / 1.75 | 207.10 / 217.01 |
| Q4 cold | 112.02 / 248.31 | 23.46 / 25.08 | 73.26 / 97.23 | 302.45 / 341.35 |
| Q5 warm | 1,011.17 / 1,333.77 (swap-in) | 3,004.35 / 3,081.97 (swap-in) | 61.07 / 73.75 | 907.64 / 936.87 (swap-in) |
| Q5 cold | 1,718.55 / 1,725.01 | 12,209.75 / 12,272.84 (swap-in) | 4,110.39 / 4,135.53 (swap-in) | 1,233.89 / 1,283.58 |

Every variant's medians are in the README's full-day block. Of the others, tick-store `v3-seg1Mi`
answers Q1 in 115.88 / 160.05 ms warm / cold and Q4 in 51.40 / 88.76, and is slower on Q2 and
Q3, warm and cold, and on Q5 cold. ArcticDB per security answers Q2 in 49.65 / 111.23 ms and Q3 in 26.19 / 91.93, under
tick-store's headline, but Q1 and Q4 in 9.8 and 9.7 s warm.

**Ingest**, the median of one write per class, headline variants. tick-store's write holds ITCH
parsing; the others ingest columns already parsed by `tools/tick_export`, and nothing is
subtracted.

| System | Class A s | Class A M records/s | Class B s | Class B M records/s |
|---|---|---|---|---|
| Parquet | 158.4 | 1.696 | 266.3 | 1.009 |
| ArcticDB | 120.1 | 2.237 | 123.6 | 2.175 |
| QuestDB | 273.6 | 0.982 | 1,269.2 | 0.212 |
| tick-store | 195.3 | 1.376 | 188.9 | 1.423 |

QuestDB's class B is not like-for-like, and on the day its durability proof failed: the flush
probe found one data page, in `_cv`, still dirty when a commit returned. Every other class is
proven and like-for-like.

**Stored bytes**, class B, allocated at rest, and the ratio of the day's source bytes to them:
tick-store 1,758,240,768 (4.693), Parquet 3,191,406,592 (2.586), ArcticDB 6,352,642,048 (1.299),
QuestDB 22,897,786,880 (0.360). QuestDB's own conversion of the day to Parquet, after its
queries, is 5,622,818,382 bytes. The smallest variant of each other system is Parquet
`zstd1-rg1Mi`, ArcticDB per security at 5,666,115,584 and QuestDB `plain`.

**Peak memory**, across each system's whole run: session RSS 13.64, 13.46, 13.81 and 13.54 GiB
for Parquet, ArcticDB, QuestDB and tick-store, with `tick_roundtrip`, the oracle, the largest
process in every system but QuestDB, whose JVM peaked at 11.66 GiB. MemAvailable fell to 11.64,
10.59, 7.13 and 12.96 GiB.

**Swap per phase**, host-wide, MiB in / out, over every variant and class:

| System | Write | Read-back | Warm queries | Cold queries | Guard, whole run |
|---|---|---|---|---|---|
| Parquet | 1.8 / 3.3 | 3.8 / 1.2 | 0.7 / 0.0 | 0.2 / 0.0 | 6.7 / 4.5 |
| ArcticDB | 35.3 / 181.4 | 317.7 / 443.8 | 17.1 / 16.9 | 7.7 / 0.0 | 414.5 / 642.1 |
| QuestDB | 124.7 / 255.4 | 1,563.9 / 2,774.7 | 11.7 / 0.0 | 60.8 / 0.0 | 1,765.7 / 3,030.1 |
| tick-store | 4.2 / 0.0 | 8.4 / 0.2 | 0.0 / 0.0 | 0.1 / 0.0 | 12.8 / 0.2 |

45 timed measures had a page swapped in: 10 of Parquet's, 18 of ArcticDB's, 15 of QuestDB's and
2 of tick-store's, Q5 warm in both its variants. The counts are host-wide, so a flag says the host
swapped in during the timing, not that the query's own pages did.

**Against the `dd5de38` stops.** Then the guard stopped QuestDB `plain` at 345 s, in its class A
write, on swap traffic of 30.3, 71.5 and 23.4 MB/s, and at 641 s, in its read-back, on 20.1, 80.7
and 15.0 MB/s. This time QuestDB ran 10,030 s. Its guard saw swap-out up to 72.2 MiB/s and
swap-in up to 28.3 MiB/s, swap-in over 10 MiB/s in 16 samples and never more than two in a row,
and at most 3.78 MiB/s held for three samples. Free swap was under 2% for 20 samples, while
MemAvailable stayed at 7.13 GiB or more, over the 3.95 GiB floor. The old rule, in plus out over
10 MiB/s for three samples, would have stopped it at 1,105 s, on 19.6, 22.0 and 10.8 MiB/s, and
would have fired four times in the run. Most of QuestDB's swap came in its read-backs, 1,564 MiB
in and 2,775 MiB out. ArcticDB's guard saw 17.8 MiB/s in at most, for one sample; Parquet and
tick-store had almost none.

**Verdicts**, on the headline variants, on `debian` alone. A full day runs only on Linux, since
its warm proof reads the block device's own counter, so a target marked Both has no macOS
full-day verdict.

- **CR-2, met.** tick-store's store is 1,758,240,768 bytes. Parquet's smallest is 1.82 times
  that, ArcticDB's smallest 3.22 times and its headline 3.61, QuestDB's 13.02, and QuestDB's own
  Parquet conversion 3.20.
- **WT-1, missed.** Class A, one thread: 1.376 M records/s against 8.59 M, short by 7.21 M
  records/s, 16.0% of the target. `v3-seg1Mi` reached 1.472 M. The target is QuestDB's 32
  workers on uncompressed TSBS rows; this is one thread that parses ITCH and compresses.
- **WT-2, missed.** 1.376 M messages/s in class A and 1.423 M in class B, against 2,732,762:
  short by 1.36 M and 1.31 M messages/s, about half the published TotalView rate. The writer is
  one thread that parses, sorts by locate and codes; how much of its 195 s is parsing is not yet
  measured (TASKS).
- **WT-3, class A against each system, tick-store's ingest including ITCH parsing:** missed
  against Parquet, 195.3 s against 158.4, by 36.8 s; missed against ArcticDB, 120.1 s, by 75.2
  s; met against QuestDB, 273.6 s, by 78.3 s.
- **WT-4, class B against each system, at each one's stated flush:** met against Parquet,
  188.9 s against 266.3, by 77.4 s, though Parquet `none-rg1Mi` wrote class B in 153.8 s; missed
  against ArcticDB, 123.6 s, by 65.3 s; met against QuestDB, 1,269.2 s, by 1,080.3 s, but QuestDB's
  class B is not like-for-like and its durability proof failed, so that win proves little.
- **QL-1, missed.** Q2 warm median 74.32 ms against 16.9 ms, 57.42 ms over, 4.4 times. Q2
  returns 739,944 records of the busiest locate through `tools/tick_query` and Python; the
  target returns 52 rows from a 1.5 M-row store.
- **QL-2, missed.** Q3 warm median 35.81 ms against 0.41 ms, 35.40 ms over. Q3 decodes the
  locate's hour of records to aggregate them; the per-locate minute aggregate in TASKS is not
  built.
- **QL-3, missed.** Q4 cold mean 308.83 ms against 10 ms, 298.83 ms over; `v3-seg1Mi` 101.57 ms.
  Q4 decodes its segment's whole locate column to put records back in file order, and at
  2^22 records a segment the runs it reads are four times longer (TASKS).
- **QL-4, per query, both median and p99 lowest of the four:**
  - Q2 warm, met: 74.32 / 83.95 ms, against QuestDB's 319.30 / 364.42.
  - Q2 cold, met: 485.13 / 500.41, against Parquet's 3,957.57 / 4,081.60.
  - Q3 warm, met: 35.81 / 36.53, against QuestDB's 68.71 / 85.62.
  - Q3 cold, met: 214.40 / 226.00, against Parquet's 2,112.55 / 2,513.76.
  - Q5 cold, met: 1,233.89 / 1,283.58, against Parquet's 1,718.55 / 1,725.01.
  - Q1 warm, missed: 297.58 / 325.72 against ArcticDB's 37.41 / 67.98, over by 260.17 / 257.74.
  - Q1 cold, missed: 406.96 / 461.20 against ArcticDB's 83.18 / 89.98, over by 323.78 / 371.23.
  - Q4 warm, missed: 207.10 / 217.01 against QuestDB's 1.18 / 1.75, over by 205.92 / 215.26.
  - Q4 cold, missed: 302.45 / 341.35 against ArcticDB's 23.46 / 25.08, over by 278.99 / 316.27.
  - Q5 warm, missed: 907.64 / 936.87, swap-in flagged, against QuestDB's 61.07 / 73.75, over by
    846.57 / 863.12.

  Q1 and Q4 decode the segment's whole locate column, as on the slice; `v3-seg1Mi` cuts them to
  115.88 and 51.40 ms warm and is still not lowest. Q5 unpacks 268.7 M share values from ranked
  PFOR, where QuestDB sums a fixed-width column it maps; the sum over ranks in TASKS is not
  built. Q5 warm here, 907.64 ms, is in line with the 911.95 ms measured on `debian` at
  iteration for the final layout (`1669ae0`); the 280 ms there was the Mac's.

Raw logs: `phase3-raw/compare-fullday/debian-67ae574/`.

#### The macOS full day, tick-store alone

The verdicts above are Linux's. The four-system day runs only on Linux, so the targets marked
Both, WT-1, WT-2 and QL-1 to QL-3, had no macOS figure. `compare/run.py --day 12302019 --systems
tickstore` now runs every tick-store variant over the whole day off Linux, with no other system
(`63f8def`). It is the same harness: the same write, oracle read-back, warm and cold queries and
ground truth. The warm proof is the slice's residency proof: every store file is read through,
and mincore reads 100% resident before and after every repetition. That holds here because the
day's store is about 1.8 GB. The per-process proof of the Linux day reads `/proc`, which macOS
does not have. Peak memory comes from BSD `time -l` around the system's whole run.
`scripts/check_compare.py` holds the file to the full day's rules. It refuses such a file from
Linux, next to another system, short of a variant, or under the four-system name; fault
`compare-tickstore-only-on-linux`.

`results/compare/macOS-12302019-full-tickstore-2503d1e7c5f6.json`. It was run from an exported
tree of `2503d1e`; the store, codec, query and writer code is unchanged since `67ae574`, the
commit of the Linux day. Apple M4 Max, clang 23.1.4, Release `-O3 -DNDEBUG`, unpinned, no
counters. The command, from the exported tree:

    TICK_STORE_MEASURE=<measure.sh> heavy.sh time -l python3 -I compare/run.py --day 12302019 \
        --systems tickstore --data <repo>/data --vendor <repo>/vendor --work <work> \
        --tools <repo>/build/bench/tools --results-dir <results>

`measure.sh` admitted the run at load1 2.67 at 08:57:35Z on 2026-11-11, and it ended at load1
2.26 at 09:14:56Z: 1,040.7 s for both variants and both classes. It started once the Mac's load
had fallen under 3, after another session's build. Peak memory: maximum resident set
32,160,907,264 bytes (29.95 GiB), which counts the clean file-backed pages of the mapped day and
stores. `run_system.py`'s peak memory footprint was 756,584,192 bytes, and the class A write's own
peak RSS 9,430,138,880 bytes. No page was swapped in or out in any phase. Day `12302019`,
268,744,780 messages, nothing clipped. Stored bytes, class B, allocated at rest: 1,762,250,752
(4.682), against 1,758,240,768 on `debian`.

**This is the second run.** The first, at `63f8def`, 06:37 to 06:54Z at load1 1.72 to 1.76, gave
3.232 M records/s in class A, Q2 warm 25.92 ms, Q3 warm 14.64 ms and Q4 cold mean 135.29 ms, with
the same verdicts. CI refused its file (run 42984860903). The peak-memory text said "the runner",
and CI's checks run as the user `runner`, so `check_compare.py`'s leak check read the word as the
machine's name. The text now names `run_system.py`, and the day ran again, with figures to be
read from the file rather than edited into it.

| Measure | macOS `v3-seg4Mi` | macOS `v3-seg1Mi` | Linux `v3-seg4Mi` (above) |
|---|---|---|---|
| Write class A, s / M records/s | 83.60 / 3.215 | 82.46 / 3.259 | 195.3 / 1.376 |
| Write class B, s / M records/s | 83.77 / 3.208 | 82.64 / 3.252 | 188.9 / 1.423 |
| Q1 warm / cold median ms | 107.09 / 166.22 | 36.40 / 56.33 | 297.58 / 406.96 |
| Q2 warm / cold median ms | 25.79 / 106.24 | 36.78 / 208.80 | 74.32 / 485.13 |
| Q3 warm / cold median ms | 14.69 / 52.78 | 18.24 / 111.68 | 35.81 / 214.40 |
| Q4 warm median / cold mean ms | 78.92 / 133.63 | 21.07 / 34.37 | 207.10 / 308.83 |
| Q5 warm / cold median ms | 282.88 / 410.52 | 296.69 / 453.82 | 907.64 / 1,233.89 |

Warm is 30 runs and cold 7, as on Linux. The p99s are in the README's macOS full-day block.

**Verdicts on macOS**, headline `v3-seg4Mi`, with the Linux verdict beside each:

- **WT-1, missed.** Class A, one thread: 3.215 M records/s against 8.59 M, short by 5.38 M, 37.4%
  of the target; `v3-seg1Mi` 3.259 M. Linux: 1.376 M, missed.
- **WT-2, met.** 3.215 M messages/s in class A and 3.208 M in class B, against 2,732,762: over by
  482,048 and 475,212 messages/s, 17.6% and 17.4%. The target is an average trading day's rate,
  not a peak (BENCHMARKS.md). Linux: 1.376 M and 1.423 M, missed. The same code writes the day
  2.34 times as fast on the M4 as on Zen 1.
- **QL-1, missed.** Q2 warm median 25.79 ms against 16.9, 8.89 ms over, 1.53 times. Linux: 74.32 ms.
- **QL-2, missed.** Q3 warm median 14.69 ms against 0.41, 14.28 ms over. Linux: 35.81 ms.
- **QL-3, missed.** Q4 cold mean 133.63 ms against 10, 123.63 ms over; `v3-seg1Mi` 34.37 ms.
  Linux: 308.83 ms, and 101.57 for `v3-seg1Mi`.
- WT-3 and QL-4 compare systems. Only Linux ran the day with all four, so on macOS they are not
  judged, and the targets block says so.

Raw logs: `phase3-raw/mac-day2/`: the runner's log with `time -l`, each step's log and the parts
files. The first run and the smoke run are in `phase3-raw/mac-day/`.

### The targets block

The README's Targets block is generated. `scripts/targets.json` defines one row per target and
host: the target, and the path to its figure in a committed results file. Some rows name more
than one case, each judged on its own; where docs/NOTES.md judged a target per query or per baseline,
the row does too (QL-4, CR-5, WT-3, WT-4). `scripts/render_results.py` reads each figure, applies
the comparison, and writes met, missed, met on some cases and missed on others, or "not judged"
with a reason. A row with no file says why: G-3 has no 30-minute fuzz record, DS-4 no Linux
kernel file, and WT-3 and QL-4 no macOS four-system day. `--check` fails on any difference, and
exits 2 when a file or field is missing. The tests cover a hand-edited value, a changed results
file, a missing file and a missing field. Fault `targets-at-most-reversed` judges at-most targets
the wrong way round and fails the tests.

Every value read agrees with the verdicts in docs/NOTES.md except one. DS-6 prices read 3.85 times from
the file, where "Decode targets under measure.sh" gave 3.86, computed from rounded figures. The
verdict, missed against 14, is the same.

The CR rows add the store as built (`results/store-size-12302019.json`) to the per-codec figures
docs/NOTES.md judged. Three targets met by a codec in the estimate are missed by the store's own
columns:

- **CR-3**: the store's timestamps, with their per-group time bases, cost 22.82 bits a record.
  That is 2.82 over 20.0, between file order's 18.76 and per locate's 25.49. The store keeps
  records in locate order, so the per-locate miss docs/NOTES.md recorded carries through to the store.
- **CR-6**: `creation_ref` costs 11.363 bits, 0.16 over 11.2, where the estimate's A, F and U
  creations together cost 7.80. `import_ref`, each segment's list of the orders created
  before it, which lets a segment decode on its own, costs 25.94 bits. `ref_distance`, the per-stock distance, meets at 7.56.
- **CR-7**: `add_shares` costs 4.165 bits in ranked PFOR, against 3.69 in file order and 2.61
  per locate in the estimate. `reduce_shares` costs 7.28. `replace_shares` meets at 2.90.

### DS-4: three more refusals

DS-4 needs the Linux kernel file, and two runs at `bcbf873` were refused ("Decode targets under
measure.sh"). Three more ran at `b053aff`, synced to `debian` with `debian_sync.sh`. The kernel
code is the same as at `bcbf873`. Each was a complete `simd_kernel_bench` run of 314 cases through
`scripts/run_bench.py` under `~/quant-projects/bin/measure.sh`, pinned to cpu4, with
`perf_event_paranoid` at 2, after `--verify-all` passed. The runs were spaced to find a quiet
host. Before each one, the load was under 1 and nothing held the `measure.sh` lock. Before the
third, cpu5 was busy 5 jiffies in 20 s.

| Attempt | Start, UTC | Load at start / end | Cases over 5% | Their range | Fused AVX2, cycles a value, median of 65 widths |
|---|---|---|---|---|---|
| 1 | 06:26:05 | 0.20 / 1.08 | 9 | 6.19% to 15.25% | 2.716 |
| 2 | 06:48:31 | 0.05 / 1.02 | 7 | 5.26% to 7.02% | 2.714 |
| 3 | 07:05:38 | 0.55 / 1.02 | 13 | 5.41% to 11.43% | 2.717 |

`check_results.py` refused every one, so no Linux kernel file was written. DS-4 is not judged.
The refused runs agree with each other and with the two at `bcbf873` (2.716 and 2.720). Two-pass
AVX2 reads 2.968, 2.950 and 2.951. These are what the pinned host measured, labelled as refused;
this repository does not publish them as results. Against 0.377 they would be 7.2 times the
target. The likely reason is the one BENCHMARKS.md gives: Zen 1 splits each 256-bit operation
into two 128-bit halves, where the Zen 3 source does not.

Between attempts 2 and 3, another job pushed the load to 6.89 for about four minutes, and cpu5
was fully busy. A check after it found nothing pinned away from cpu5. Long-running host processes,
among them an interactive agent session, a VPN daemon and a file sync, were last scheduled on
cpu5 and cpu4. Any of them can take the 6 jiffies of cpu5 that put a 1.1 s case over the limit.
That is an observation, not a proven cause. The rule stands. What would get a file is a case long
enough that a stray jiffy does not count, or a host where cpu5 is reserved (TASKS).

Raw logs, with every case's perf, report and `/proc/stat` snapshots: `phase3-raw/ds4-b053aff/`.

### G-3: 30-minute fuzz runs

G-3 is met at `ea1dde4`, whose store, codec, query and writer code is unchanged since `67ae574`.
No fuzzer found anything, so no code changed. `results/fuzz-g3-ea1dde4c02e4.json` holds every
run. `scripts/fuzz_results.py` built it from the raw logs, and `scripts/check_fuzz.py` validates
it before the Targets block reads it.

All 13 targets in `fuzz/CMakeLists.txt` ran on the Mac at the NEON level. Six of them reach the
SIMD kernels through `codec::simd::active_kernels()`: PFOR and Stream VByte directly,
dictionary and run-length through nested PFOR blocks, and the ITCH store and the query through
their column chunks. Those six also ran on `debian`, with `TICK_STORE_SIMD=avx2`, since the Mac
cannot run AVX2. The other seven use no kernel set, and neither do Gorilla, the container
parser, the column directory, the stub store or the ITCH parsers. The container, store and
column targets do reach the CRC32C SSE4.2 path on x86-64, which is not AVX2; it ran only in
CI's 4-minute Linux runs. All six SIMD targets decode each block, or read the whole file, again
with `ScalarKernelsOnThisThread` and require the same result (`scalar_agrees` in
`codec_oracles.hpp`, and the scalar pass in the ITCH store and query oracles). So one AVX2 run
covers both sets, and no separate scalar-capped run was made.

Each run used `scripts/check.sh`'s flags: `-max_total_time=1800 -rss_limit_mb=2048
-malloc_limit_mb=256 -timeout=10`, its own `-max_len`, the existing corpus first and the seeds
the build writes from the real encoders second. On the Mac, four ran at once, unpinned, in
four batches. Each batch was its own `heavy.sh` job, and the fourth waited out another project's
`measure.sh` run. On `debian`, three ran at once, each pinned by `taskset` to cpu2, cpu6 or cpu7,
in two `heavy.sh` batches with no measurement lock held, after a fresh sync and fuzz-preset
build. The Linux corpora started as copies of the Mac's.

| Target | Host | Level | Seconds | Runs | exec/s | cov / ft | Corpus | Peak RSS, MB |
|---|---|---|---|---|---|---|---|---|
| `fuzz_columns` | macOS | neon | 1,801 | 54,069,697 | 30,022 | 829 / 2,738 | 188 | 460 |
| `fuzz_container` | macOS | neon | 1,801 | 167,158,633 | 92,814 | 526 / 1,804 | 166 | 701 |
| `fuzz_dictionary` | macOS | neon | 1,804 | 93,165 | 51 | 1,958 / 12,175 | 1,103 | 1,336 |
| `fuzz_gorilla_timestamps` | macOS | neon | 1,801 | 5,565,301 | 3,090 | 413 / 2,244 | 209 | 581 |
| `fuzz_gorilla_xor` | macOS | neon | 1,801 | 6,097,812 | 3,385 | 455 / 2,441 | 251 | 651 |
| `fuzz_itch_frame` | macOS | neon | 1,801 | 211,171,384 | 117,252 | 399 / 2,345 | 1,305 | 664 |
| `fuzz_itch_message` | macOS | neon | 1,801 | 504,472,078 | 280,106 | 635 / 690 | 396 | 491 |
| `fuzz_itch_query` | macOS | neon | 1,801 | 338,313 | 187 | 4,036 / 20,688 | 1,505 | 782 |
| `fuzz_itch_store` | macOS | neon | 1,801 | 3,563,885 | 1,978 | 4,747 / 13,734 | 795 | 487 |
| `fuzz_pfor` | macOS | neon | 1,801 | 114,498 | 63 | 1,548 / 8,201 | 1,115 | 1,176 |
| `fuzz_run_length` | macOS | neon | 1,801 | 390,986 | 217 | 1,449 / 7,954 | 727 | 1,140 |
| `fuzz_stream_vbyte` | macOS | neon | 1,801 | 1,031,497 | 572 | 750 / 4,611 | 453 | 832 |
| `fuzz_stub_store` | macOS | neon | 1,801 | 334,181,975 | 185,553 | 396 / 1,503 | 569 | 448 |
| `fuzz_dictionary` | Linux | avx2 | 1,801 | 23,477 | 13 | 1,783 / 11,018 | 1,046 | 958 |
| `fuzz_itch_query` | Linux | avx2 | 1,802 | 74,145 | 41 | 3,802 / 19,303 | 1,165 | 581 |
| `fuzz_itch_store` | Linux | avx2 | 1,801 | 535,195 | 297 | 4,449 / 12,699 | 649 | 422 |
| `fuzz_pfor` | Linux | avx2 | 1,801 | 19,690 | 10 | 1,470 / 7,621 | 986 | 613 |
| `fuzz_run_length` | Linux | avx2 | 1,801 | 35,664 | 19 | 1,357 / 7,405 | 700 | 723 |
| `fuzz_stream_vbyte` | Linux | avx2 | 1,801 | 68,783 | 38 | 710 / 4,380 | 449 | 607 |

Seconds, runs, exec/s, coverage, corpus and peak RSS are libFuzzer's own final figures. Every
run exited 0, with no sanitizer report and no artifact. BSD `time -l` agreed with libFuzzer's
peak RSS to within 1 MB on every Mac run. On `debian`, the fuzzer's `VmHWM`, read every 5 s,
agreed too: this host has no GNU `time`. The four peaks of the first Mac batch, `fuzz_dictionary`,
`fuzz_pfor`, `fuzz_itch_query` and `fuzz_itch_store`, sum to 3.8 GB, the most of any batch. The
largest `debian` batch's three peaks sum to 2.3 GB.

What the runs showed:

- **Leak checking on the Mac.** ASan on macOS leaves LeakSanitizer off, so `check.sh`'s options
  check nothing for leaks there. With `detect_leaks=1`, the check at exit reports 56 bytes from
  libFuzzer's own RSS thread, which is never freed. It was seen on `fuzz_itch_message`, and it
  comes from libFuzzer's driver, so it affects every target. The Mac runs used
  `detect_leaks=1:leak_check_at_exit=0`, so libFuzzer still checks each input for a leak. A
  five-line target that leaks 64 bytes on inputs starting with `L` was caught at once, exit 77,
  under the same options. Under `check.sh`'s options alone, the same target exits 0. The Linux runs kept `check.sh`'s options, where LeakSanitizer is on
  by default.
- **Zen 1 is slow under ASan.** The SIMD targets ran 4 to 15 times fewer inputs a second on
  `debian` than on the M4. Replaying the starting corpus took a large share of the half hour.
  For example, `fuzz_pfor` on `debian` replayed its 5,103 seeds and corpus inputs in about
  567 s, judged from libFuzzer's runs and exec/s at `INITED`. That left about 1,234 s for new
  inputs; `fuzz_dictionary` took about 262 s and `fuzz_itch_query` about 192 s. The replay runs every
  input through the full oracle, so it counts as fuzzing, but it finds nothing new. The Mac's
  slowest replay was 81 s, also `fuzz_pfor`. The PFOR seed set alone is 4,289 files. A
  `-merge=1` pass over seeds and corpus would cut the replay (TASKS).
- **Coverage on Linux is a little lower** on every SIMD target, for example 1,470 against
  1,548 for `fuzz_pfor`: fewer inputs, and the AVX2 set's code is not the NEON set's.
- **A checkout that moved.** The runner recorded `git rev-parse HEAD` as each run started.
  Nine Mac runs started after `95f7eb0`, which added only the validator and its tests. The
  binaries built at `ea1dde4` ran throughout. `fuzz_results.py` takes such a run only if
  `git diff` shows nothing a fuzz build reads, and it keeps the checkout as `checkout_at_start`.

The check: `check_fuzz.py` reads the target list from `fuzz/CMakeLists.txt`. It refuses a file
that lacks any target on macOS, or any `AVX2_PATH` target on Linux at `avx2` (a scalar run does
not stand in for one). It also refuses a clean run under 1,800 s by libFuzzer's count or by the
wall clock, and a budget under 1,800 s or a looser cap in the command. A done line, final status
line or final stats that disagree with the recorded numbers are refused, and so is a summary
that does not recount from the runs. A run that is not clean may stop early, but only if it
names its finding, and the row then reads missed. `tests/scripts/test_check_fuzz.py` has one test
for each rule. Two faults are proven with `prove_checks.py --only`. `fuzz-g3-target-missing` drops
`fuzz_columns`' Mac run and recounts the summary, and `render-check` exits 2 with "no run of
fuzz_columns on macOS at neon". `fuzz-g3-short-run-accepted` lets a clean 1,799 s run through,
and `py-scripts` fails `test_a_clean_run_under_1800_seconds_is_refused`.
`results/check-proofs.json` was restored from git after each.

The full `scripts/check.sh` passed all 30 steps at `28435e4` under `heavy.sh`, in 2,122 s.

Raw logs, with every fuzzer's log, `.meta`, BSD `time` output, the runner scripts and the leak
probe: `phase3-raw/fuzz-g3/`.

### Leak checking on the Mac

`check.sh` could not fail on a leak on macOS: ASan leaves LeakSanitizer off there, and the
script set no option to turn it on. Two faults show it. `asan-leak-trailing-bytes` makes the
strict decoder leak two sizes each time it refuses trailing bytes, and `fuzz-itch-message-leak`
makes it leak a copy of each unknown type byte. Both were registered first, at `59b7066`, and
`prove_checks.py --only` reported both UNDETECTED: `asan-test` passed all six `ItchTruncation`
tests, and `fuzz-itch-message` ran its 45 s and exited 0.

`check.sh` now asks for leak detection itself, ahead of anything in `ASAN_OPTIONS`:
`detect_leaks=1` for `asan-test`, and `detect_leaks=1:leak_check_at_exit=0` for the fuzzers on
macOS. The check at exit stays off for the fuzzers there, since it reports 56 bytes from
libFuzzer's own RSS thread; libFuzzer still checks each input for a leak and exits 77. On Linux,
where LeakSanitizer is on by default, `detect_leaks=1` is set too and changes nothing. CI's jobs
call `check.sh`, so its macOS jobs take the same options. Before the change, the whole Mac
`asan-ubsan` suite ran with `detect_leaks=1` and the check at exit on: 625 of 625 passed in
256 s, with no leak reported, so nothing in the tests or the system libraries needs a
suppression.

At `76703fb` both faults are PROVEN with `--only`: `asan-test` exits 8, with
`ItchTruncation.TrailingBytesAreRejectedNotIgnored` failing on "ERROR: LeakSanitizer: detected
memory leaks", 48 bytes in 3 objects; `fuzz-itch-message` exits 77 on the same line, the leaked
byte beside libFuzzer's thread. The two `--only` runs took 141 s and 70 s under `heavy.sh`.

### The SSE4.2 CRC32C kernel under G-3

Three fuzz targets verify CRC32C on fuzzed bytes, through `store::crc32c()`, which on x86-64
runs `crc32c_sse42`: `fuzz_container`, `fuzz_itch_store` and `fuzz_itch_query`, which read
their input with CRCs verified after recomputing every CRC its structure points at, and the
container target also as it stands. No target bypasses the CRCs entirely, so no new target was
needed. Replaying each target's seeds once on `debian` with `-runs=0 -print_coverage=1`, with
the `ea1dde4` fuzz build, lists `crc32c_sse42` as covered by those three (31, 58 and 89 calls)
and the software kernel as never called. `fuzz_columns` and `fuzz_stub_store` do not reach it:
neither format has a CRC. The G-3 note above named the column target among them; it does not.

`fuzz_itch_store` and `fuzz_itch_query` already ran 1,801 s on `debian` for G-3, so the kernel
had 30 minutes there. `fuzz_container` had run only CI's 4 minutes on Linux. It ran 1,801 s on
`debian` at `ea1dde4` through `heavy.sh`, pinned to cpu2, with no measurement lock held and
`check.sh`'s flags, from a copy of the Mac's G-3 corpus: 73,167,626 runs, 40,626 a second,
cov 521 and ft 1,797, 170 corpus units, peak RSS 555 MB by libFuzzer and 555 MiB by `VmHWM`,
exit 0 with no sanitizer report. It waited about 7.4 hours for the lock, behind another
project's two measurements. `fuzz_results.py` added it to `results/fuzz-g3-ea1dde4c02e4.json`,
and `check_fuzz.py` now requires the three `CRC32C_PATH` targets on Linux beside `AVX2_PATH`,
7 in all, so a file without the container's Linux run is refused. Fault
`fuzz-g3-crc-target-missing` drops it and recounts the summary, and `render-check` exits 2.
The README's Targets row reads 7 against 7.

What these runs check is memory safety on fuzzed lengths and offsets, under ASan and UBSan.
They do not check the kernel's value: the oracles recompute CRCs with the same kernel, so a
kernel wrong the same way every time would agree with itself. That is
`Crc32c.HardwareMatchesSoftwareOnRandomLengthsAndOffsets`, in every Linux `dev-test`. Replaying
the container's corpus after the run covers 10 of the kernel's 17 edges, as the seeds alone
did; the three uncovered points are all on line 26, the `memcpy` in its load helper. Which
branches those are was not established.

Raw logs, with the reach replays and the runner scripts: `phase3-raw/close-p3/crc-g3/`.

### check.sh on the Linux host

`debian` holds an exported tree: `debian_sync.sh` unpacks `git archive` and writes `COMMIT`, so
there is no `.git`. Phase 3's kickoff found 17 of 21 steps passing there at `469e9cd`. With 30
steps, the four that could not pass were still the same at `ea1dde4`, the tree `debian` held,
whose `check.sh` is the one this task started from. Run alone with `--only` through `heavy.sh`
pinned to cpu2, cpu6 and cpu7: `coverage` failed after 813 s, when `coverage_report.py` asked
git for the commit; `hygiene` failed at once, on `git ls-files`; `actionlint` failed, since the
host has none; `py-compare` skipped for want of a `.venv`. The other 26 were not run then.

What changed, at `21fda00`:

- `check_hygiene.py` reads an exported tree's own files: every file but `COMMIT` and those its
  `.gitignore` files exclude, listed by git against an empty repository of its own, so build
  output made after the export is left out by the same rules git uses. A home path or a data
  file in an export is refused, and a folder that is neither a checkout nor an export is an error;
  `test_check_hygiene.py` has a test for each.
- `coverage_report.py` takes the commit from `COMMIT` and the sources from `src/` there.
- `py-compare` takes `CHECK_VENV`, the Python environment to use when there is no `.venv`. On
  `debian` that is the project's own `venv`, outside the exported tree.
- `actionlint` skips, with its reason in the summary, when the host has no actionlint and the
  tree is an export. In a git checkout a missing actionlint still fails. A skip is not a pass:
  the summary counts it apart, and CI's Linux jobs run `check.sh` in a git checkout, where
  `hygiene` and `coverage` read git and actionlint is installed by the toolchain action.

The whole `check.sh` then ran on `debian` at `d307a31`, through `heavy.sh` pinned to cpu2,
cpu6 and cpu7 with `CHECK_VENV` set: 29 of 30 steps passed and `actionlint` skipped, in
7,962 s. `dev-test`, `asan-test` and `tsan-test` each ran 625 tests with the two `ItchRealDay`
tests skipped; `clang-tidy` analysed 121 files in 1,652 s and named the four it could not,
the Mac, Arm and NEON sources that Linux does not build; coverage was 93.58% of lines and
97.08% of functions, 256 of 256 required functions reached; `py-compare` ran its fast set of
132 tests, without `test_real_systems.py`. Then `docs/injected-home.txt`, holding a home path, was added to the export:
`hygiene` failed with exit 1 on `docs/injected-home.txt:1: absolute home path`, and passed
again once the file was removed.

Raw logs: `phase3-raw/close-p3/linux-check/`.

### The full fault re-proof at d307a31

`scripts/prove_checks.py --force` ran every fault through `heavy.sh` under `/usr/bin/time -l` at
source commit `d307a31`: 8,339 s, a maximum resident set of 1.50 GB and a peak memory footprint
of 113 MB. Of its 92 faults, 89 were PROVEN and the control UNDETECTED, so 90 of 92 came out as
expected. The sibling meter's new fault, `sibling-meter-whole-uptime`, was among the 89. Two
were not, and neither was a check that had broken:

- `fuzz-container-metadata-start` was WRONG REASON: `fuzz-container` ended BLOCKED, because
  `fuzz-build` failed first. Since `7ebb81b` the seed generator takes real store files apart
  with the container parser and rebuilds them, so a parser that starts the metadata one byte
  late misreads every valid file, and the generator aborts with exit 134 on `invalid argument:
  codec id`. The fault is now registered on `fuzz-build` with that exit and message. To keep a
  fault on `fuzz-container`, `fuzz-container-block-reserved` stops refusing a block entry whose
  reserved field is not zero, which only a corrupt file shows; a generated seed sets it to 1.
- `fuzz-itch-store-distance-bound` was UNDETECTED: 135,020 runs in 45 s at seed 1, clean. It
  had been proven once, alone, when it was added, and never in a full run. Four runs at seed 1
  in the prove clone: one found it, at 43 s after 84,029 runs; the full run's 45 s did not, nor
  did two runs at CI's 240 s budget, of 555,923 and 581,679 runs. With one seed and one corpus,
  libFuzzer's runs still differ, and this fault is found by chance. A longer budget is therefore
  no fix. Seven other patches, each run once for 45 s, were not caught either:
  `store-metadata-records`, `store-segment-records-sum`, `store-piece-value-count`,
  `store-piece-index-next`, the column value count, the type symbol and the writer's
  `itch-store-import-not-appended`. The decoder's checks overlap, so one removed is masked by
  another. The oracle compares only what files read as, so a check on a field that changes no
  record goes unseen. And a writer fault also writes the seeds, which the reader then refuses.
  The patch is now `store-ref-distance-bound` on `dev-test`, where
  `ItchStoreCorrupt.ValuesThatBreakTheRulesTyingColumnsTogether` sets a distance past its list.
  `fuzz-itch-store-tracking-misread` makes the decoder read every tracking number one high: a
  file read wrong comes back wrong twice through the writer, and the step fails on its 7th
  input with `check_itch_store: the rewritten file reads back differently`. A seed with a
  distance past its list is carried to Phase 4, since it changes the fuzz build and so G-3.

`prove_checks.py --only` then ran the four changed faults at the same source commit, through
`heavy.sh` under `/usr/bin/time -l`: all four PROVEN in 137 s, with a 619 MB maximum resident
set. `results/check-proofs.json` holds all 94 entries at `d307a31`: 93 PROVEN, and the control
UNDETECTED.

Raw logs: `phase3-raw/close-p3/reproof/`, with the four-run study and the seven patches under
`investigate/distance-bound/`.

### README layout and RESULTS.md

At `9f77e52` the README opens with a pitch and a generated six-row headline, and every results
table moved to `RESULTS.md`, also generated. The targets, benchmark and comparison blocks there
are byte for byte the blocks the README held at `46ab0df`. `scripts/targets.json` names the
headline under `"headline"`: a target row by id and host, a short name, and optionally which of
its parts to show. `render_results.py` judges each entry with the Targets block's own code, so
each figure and verdict is that block's. The parts no headline row shows are named under the
table with their verdicts, so the headline cannot quietly drop a miss. `--check` now covers both
files, and exits 2 when the README keeps a RESULTS.md block, a file lacks its markers, or the
headline shows no missed target.

The rows, and why:

- G-1, both days bit-exact: every other figure depends on the store giving the day back.
- CR-2, store bytes against each system's smallest variant on the Linux full day: it compares
  with real systems on the same day, where CR-1 compares with the file's own `.gz`.
- QL-4 on Q2 and Q3, met warm and cold, and QL-4 on Q1 and Q4, missed warm and cold. Together
  they say which queries tick-store wins and which it loses, and Q5, split, is named under the
  table. QL-3, also missed, was left out, because QL-4 on Q1 and Q4 already shows the loss on
  the all-symbol windows.
- WT-1 on Linux, missed at 1.376 M records/s against 8.59 M: the headline ingest target, from
  a published QuestDB figure, so the table shows the write side as well as the read side.
- G-3, every fuzz target 30 minutes clean on both hosts.

The pitch quotes the Q2 and Q3 win as a win among headline variants, the wording rule in
docs/TASKS.md, and gives ArcticDB's per-security variant's 49.65 and 26.19 ms warm from the full-day
variants table.

Seven faults are new, and `readme-hand-edit` is now `results-md-hand-edit`, on RESULTS.md's
benchmark block. Hand edits of a value in RESULTS.md's Targets block and in its comparison block,
and of WT-1 in the README's headline, each make `render-check` exit 1, naming the file. The
README without its headline markers, the README with a copy of the Targets block, and a headline
cut to G-1 and G-3 each make it exit 2. `headline-verdict-all-parts` judges a headline row on all
of its parts rather than those shown, and `test_render_results.py` fails. `prove_checks.py
--only`, through `heavy.sh` at source commit `9f77e52`, ran these eight faults, the two renderer
patches rebased onto the new file, and the three faults that run `render-check`. 12 of 13 were
PROVEN, and the control was UNDETECTED. `results/check-proofs.json` was restored from git, so it
still records the full run at `d307a31`, where the fault was `readme-hand-edit`. The renderer
changed, so the next `prove_checks.py` run re-proves every fault.

GitHub's markdown API in `gfm` mode renders each newline in a paragraph as a line break. A
repository file is not rendered that way. The previews were made in both `gfm` and `markdown`
modes, and only `markdown` mode shows the README as the repository page does.

Raw: `phase3-raw/readme/`, the renders and `prove-only-9f77e52.txt` with its logs.

### Repo layout: CLAUDE.md, docs/ and the licence

At the user's request, three changes to what the repository holds, and none to the store.

- `CLAUDE.md` is no longer tracked (`eb72b71`): `git rm --cached`, and `/CLAUDE.md` in
  `.gitignore`. The file stays on disk, and history is not rewritten, so every commit a results
  file cites still exists. No check or test named it, and it named neither moved file.
- `NOTES.md` and `TASKS.md` moved to `docs/` with `git mv` (`2596d04`). At `83cd39b`, 93
  references in 29 files named them; all 93 now read `docs/NOTES.md` or `docs/TASKS.md`, and
  `git grep -E '(^|[^/])(NOTES|TASKS)\.md'` finds none. 17 are C++ comments, and the C++ diff is
  comment-only. The reference in `scripts/targets.json` and the one string in
  `scripts/render_results.py` changed with them, and RESULTS.md was regenerated. The moved files
  held no Markdown link to `store.md` or the README, so none needed rewriting; the README's list
  now links the two files. A link checker over the eight changed Markdown files found 18
  relative links, none broken. All 77 fault patches still apply. `prove_checks.py --only`, through
  `heavy.sh` at source commit `5a810a2`, ran 30 faults, the 18 whose patches touch a changed file
  and the 12, the control among them, that mutate a changed file or run `render-check`,
  `actionlint` or `hygiene`: 29 PROVEN and the control UNDETECTED. `results/check-proofs.json`
  was restored from git, so it still recorded the full run at `d307a31` (see "The full fault
  re-proof at 5a810a2" below).
- `LICENSE` is MIT, `Copyright (c) 2026 Jackson Baxter` (`5a810a2`), and the README has a
  "Licence" section.

The full `scripts/check.sh` passed all 30 steps on the Mac before the commits, in 2,230 s.
On debian, the Linux build gate ran at `e9ee24b`, whose code is that of `5a810a2`: the dev
preset built through `heavy.sh` on CPUs 2, 6 and 7, and ctest passed 625 of 625, with none
failed or skipped, in 908 s.

The third-party audit, file by file:

- No committed file holds Nasdaq message bytes. The golden stores in `tests/unit/golden/` are the
  26-record stream of `tests/unit/stub_fixture.hpp`, built from the hand-typed records of
  `tests/unit/itch_fixtures.hpp`, and `container-v1.tsc` is `test::golden_parts()`. The two
  crashers in `tests/unit/crashers/`, of 31 and 48 bytes, are libFuzzer inputs grown from the
  seeds `fuzz/generate_seeds.cpp` makes with the real encoder at build time.
- The results files measured on the two sample days hold counts, sizes, timings, statistics and
  digests. `research/evidence/itch-data.md`, L7, found no terms for the sample files and left
  redistribution of derived data unresolved, so the README lists these files as keeping their
  source's terms. `macOS-simd_kernel-*`, on seeded input, and the proof, coverage and fuzz records
  are the project's own.
- `research/evidence/*.md` quotes third-party documentation and code, each quote with its
  location, and the README lists those quotations. One quotation in `column-systems.md` keeps the
  QuestDB docs' own site-relative link, `/docs/query/sql/copy/`, which does not resolve here. It
  is left as quoted.
- `compare/requirements.lock` holds package names, versions and hashes, and
  `compare/java/CommitModeProbe.java` reaches QuestDB through reflection alone, with none of its
  code. Neither is third-party material.
- No source says it is adapted from another project. The Stream VByte shuffle tables, the linear
  layout's tables and the CRC32C tables are computed by `constexpr` code here, and Gorilla and
  Stream VByte are implemented from their papers, which the headers cite.
- GoogleTest, HdrHistogram_c, Google Benchmark and zstd come through FetchContent, and pyarrow,
  ArcticDB, the QuestDB client and server and a Temurin JDK through `compare/setup.py` into the
  gitignored `.venv/` and `vendor/`. None is committed. Each carries its licence in its own
  release, and the README says so without copying them.

The README's CI paragraph (`72bceaf`) keeps the history, all 14 jobs passing at `ad542fd`, and
points at the workflow's runs on `main` in place of a job count that would go stale. The
workflow has 16 jobs, and all 16 passed at `83cd39b`, in run 43152738984.

At `72bceaf`, run 43171535758 on `jb-build` passed all 16 jobs. Run 43251305872, on `main` at
the same commit, failed its first attempt with 12 jobs passed and 4 cancelled, all on macOS
arm64: tsan, asan-ubsan, coverage and fuzz-stores. None of the four ran a step. Each carries
GitHub's annotation "The job was not acquired by Runner of type hosted even after multiple
attempts", with the notice "Due to capacity constraints, jobs targeting macOS arm64 runners may
experience longer queue times." That is a GitHub-side fault, so the failed jobs were rerun with
`gh run rerun 43251305872 --failed`, with no change to the code. The second attempt passed all 16.

Raw: `phase3-raw/layout-docs/`, the check.sh logs and `prove-only.txt` with its logs;
`phase3-raw/close-final/debian/`, the debian gate's script, status, build log tail and ctest
summary; `phase3-raw/close-final/ci/`, the CI run's jobs and annotations.

### The full fault re-proof at 5a810a2

`scripts/prove_checks.py --force` ran every fault through `heavy.sh` under `/usr/bin/time -l`,
from a clean tree at `e9ee24b`, keyed to source commit `5a810a2`, the last commit that changed
code: 8,532 s, a maximum resident set of 1.50 GB and a peak memory footprint of 114 MB. All 101
faults came out as expected at the first attempt: 100 PROVEN and the control,
`control-comment-only`, UNDETECTED. No baseline failed, and no check, test or fault changed.
`results/check-proofs.json` now holds all 101 entries at `5a810a2`, in place of the 94 at
`d307a31`.

A first start was stopped seconds in, on its first fault, because its runner had a two-hour
limit; the run above started again from the beginning with `--force`, and nothing was resumed
from the record.

Raw: `phase3-raw/close-final/`, `prove-full.txt` with `prove-logs/`, and the record as written,
`check-proofs-full-5a810a2.json`.

### The README's stale lines

The README's Coverage section said "No SIMD kernels exist yet, so none are reported." That has
been false since `0a03ce5`, which put the dispatch, the scalar set and this processor's own set
(NEON on arm64, AVX2 and its CPUID check on x86-64) on the required list in
`scripts/coverage_report.py`: 255 functions on arm64 and 256 on x86-64, counted from
`required_functions`. The section now lists the groups the script's docstring names, which it
had left out (the codecs, the container's and the store's readers and writers, the queries,
CRC32C, the kernels), and gives the gate's last full run: `coverage.sh` inside the full
`check.sh` of the repo layout work, on the Mac, whose summary records `83cd39b`, reached 255 of
255, with 93.68% of lines and 97.06% of functions. The C++ of `83cd39b` differs from `5a810a2`'s
only in comments. It also says that `results/coverage.json` is the summary published at
`4302ce8`: 204 required functions, with the ITCH store but before the queries and the kernels.

A read of the rest of the README against the tree found four more present-tense lines that
were false:

- `check.sh` "about 8 minutes on a warm build tree": the last full run took 2,230 s on the Mac
  with every build warm, so it now reads about 37 minutes.
- "The two sources a host does not compile": `files_of_other_platforms` in `check.sh` names
  four on each host, the other operating system's two and the other processor's CRC32C and
  SIMD kernels, and the last clang-tidy step named four.
- The bit-flip walk-through flipped byte 721,746,430, "segment 128's timestamp block". The
  store at the default 2^22 records a segment has 65 segments, so there is no segment 128. The
  offset is now 878,213,784, bit 1, the middle byte of segment 32's timestamp block, block
  56537, which is the flip `results/roundtrip-12302019.json` records under G-2. The file is from
  `1669ae0`, and no commit since has changed the store's code beyond comments.
- "Each step needs under a minute; the store is 1.45 GB, 5.69 times smaller": the same file
  records `ingest` at 85 s and `verify` at 38 s on the Mac, and 1,758,234,758 bytes, 4.69
  times. 1.45 GB and 5.69 were store version 1, at 2^20.

The fault-proof paragraph now says that a full run is 101 runs, 100 faults a check must catch
and one control no check may notice, in place of "all 101 faults".

Deferred: `scripts/coverage_report.py` still says no SIMD kernel exists, in its docstring and in
`SIMD_STATEMENT`, which it writes into each summary, so the summaries `check.sh` and CI write
under `build/` carry that line while their required list holds the kernels. Fixing it changes
a script, which moves the proof key off `5a810a2` and voids the full re-proof of 8,532 s, so it
waits for Phase 4 with the republished `results/coverage.json` and one full re-proof after
both (docs/TASKS.md).

Checks on `2c683c1`: the proof key is still `5a810a2`, all 77 fault patches apply,
`render_results.py --check` exits 0, and `check.sh` passed `check-results`, `render-check`,
`hygiene`, `py-scripts` (500 tests, 4 skipped) and `actionlint`. The three faults that edit
the README, `readme-headline-hand-edit`, `readme-headline-markers-removed` and
`readme-keeps-a-results-block`, were proven again with `prove_checks.py --force --only` through
`heavy.sh`: 3 of 3 PROVEN, and `results/check-proofs.json` came out unchanged.

Raw: `phase3-raw/readme-accuracy/`, the check logs and `prove-only.txt` with its logs.

## Phase 4 notes

### The full day's source precondition

The orchestrator's Phase 3 gate ran tick-store alone on the full day at `ca96b36` on debian,
`compare/run.py --day 12302019 --systems tickstore --variants v3-seg4Mi,v3-seg1Mi` under
`measure.sh`, three times. Each stopped at the first write: `ToolFailure: the source is not
resident before the write`. tick-store's write reads the day's ITCH file, 8,251,407,909 bytes,
2,014,504 pages, and `compare/systems/tickstore.py` requires every page resident before its clock
starts, after one `tick_coldcache warm`, which reads the file through with `read()`. The
four-system day at `67ae574` passed the same check on all four of tick-store's writes, and no
harness code on that path changed since.

**What the cache holds when it fails.** A replay on debian at `ca96b36` (`diag.py` in the raw
folder, through `heavy.sh` on cpus 2, 6 and 7, iteration only) made the reads `run_system.py`
makes before the first write, in its order: the pin's SHA-256 over the source, `tick_roundtrip
verify-columns` over the source and the 23.9 GB export, the numpy ground truth, then the warm.
Residency came from `tick_coldcache residency`, a per-page `mincore` map of the source in 64
parts, `/proc/meminfo` and `/proc/vmstat`:

| Step (start as the gate left it) | Source pages | Export pages | MemAvailable | Cached |
|---|---|---|---|---|
| start | 1,552,356 | 1,282,116 | 14.84 GB | 13.96 GB |
| after verify-columns | 627,226 | 2,317,000 | 15.01 GB | 14.26 GB |
| after the ground truth | 593,221 | 2,355,575 | 15.07 GB | 14.28 GB |
| warm 1, the precondition's | 1,709,646 | 1,252,448 | 15.06 GB | 14.34 GB |
| warm 2, nothing evicted | 1,725,924 | 1,252,410 | 15.06 GB | 14.40 GB |
| the export evicted | 1,725,924 | 0 | 15.06 GB | 9.27 GB |
| warm 3 | **2,014,504** | 0 | 15.06 GB | 10.46 GB |

The first warm left 1,709,646 of 2,014,504 pages, as the orchestrator's 1,729,873. The missing
pages were the file's own first quarter: the warm read them, then reclaim dropped them while it
read the rest. Each warm stole about 1.78 M file pages and refaulted as many, while the export
kept 1.25 M pages, 5.1 GB, untouched since the ground truth. A second warm added 16,278 pages,
so reading again is no fix. With the export evicted, one warm made the file wholly resident.
The pages the export kept follow what the ground truth read through numpy's mappings: in the
cold-start replay below, `shares.bin`, which Q5 sums over the whole day, kept 205,550 of its
262,447 pages, and the 8-byte columns 65 K to 126 K pages each. With source, export and every other file the replay read
evicted, 2.2 GB of the page cache belonged to other files on the host.

**What was ruled out.**
- *The cache as the gate left it.* The same replay from a cold start, the source and the export
  evicted first and 12.4 GB free, as tick-store's run began at `67ae574` after ArcticDB's run had
  evicted both and deleted its store, failed the same way: 1,734,378 pages after the warm, the
  export keeping 1,237,811.
- *Other inputs and stores.* At every step itch-orderbook's copy of the day, the `.gz` the data
  folder links to, the slice, the slice's export and `build/simd-day.tsc` held 0 pages. The work
  folders `build/compare`, `build/compare-iteration`, `build/compare-smoke` and the gate's own held
  no store.
- *Harness changes.* `git diff 67ae574 ca96b36` touches the off-Linux tick-store day and comments
  on this path, nothing that runs before a Linux write.

**Why `67ae574` passed is not fully explained.** Its guard recorded 15.41 GB available when
tick-store's run began, against 15.05 GB at the gate and 15.06 GB in the replay: 0.36 GB more,
short of the 1.15 to 1.25 GB the warm misses now. What else the page cache held that day was not
recorded. The precondition sits close to the host's limit: the source, 8.25 GB, has to fit beside
the 5.1 GB of the export that reclaim keeps and the other files' 2.2 GB. Reclaim here is the
multi-generation LRU (`/sys/kernel/mm/lru_gen/enabled` reads `0x0007`, `min_ttl_ms` 0); a plain
LRU would have dropped the export's older pages first. Its generations are in debugfs, which a
normal user cannot read, and turning it off is a host change, so which part of its policy keeps
those pages was not tested. What would have falsified the cause found: the warm reaching every
page with the export still resident, or still falling short with it evicted. Neither happened.

**The fix** (`compare/systems/tickstore.py`, `warm_source`). Before the clock, as before, one warm.
If it leaves the source short on a day, the day's export, which tick-store's write never reads, is
evicted through `coldcache.evict` (the proof at 0 resident, the quiet second and the refill
wait), and the source is read through once more. If it is still short, or short on the slice, the
run stops with the pages resident against those wanted, the reads made, MemAvailable, MemFree and
Cached, and the largest holders of the page cache under the data and work folders, a directory
such as the export counted as one (`coldcache.cache_holders`, new, called only here). The write's
`input_resident` records the reads, the first read's pages and the eviction. A write whose first
warm succeeds runs exactly as before, so the slice and every write of `67ae574` are unchanged.
The eviction only advances what the read-back after the write does anyway: `evict_inputs`
evicts the source and the export before every full-day read-back.

The other systems share none of it. Their writes read the export inside their clocks, and a day's
export cannot be held resident here, so there is no export precondition to match; on the slice the
export is loaded into an Arrow table before their clocks. The functions the fix calls in
`compare/coldcache.py`, `warm`, `evict`, `residency` and `meminfo_bytes`, are the ones every
system's cold and warm proofs already use, unchanged. `run_system.py` and `worker.py` are
unchanged, so no system's timing moved.

**The fault.** `tickstore-source-warm-no-eviction` reads the source again without evicting the
export. `tests/compare/test_tickstore.py` (`SourcePrecondition`) fails on it, and
`prove_checks.py --only tickstore-source-warm-no-eviction` through `heavy.sh` gave PROVEN. That run
rewrote `results/check-proofs.json`, which was restored; the full re-proof at the end of the phase
covers it. The README's fault-proof paragraph counts 101 runs, which is now 102.

**On debian after the fix**, an iteration run at `4c049c7`, run as the gate ran it (the bench
build through `heavy.sh`, then `compare/run.py --day 12302019 --systems tickstore --variants
v3-seg4Mi` under `measure.sh`), went through every step of tick-store's day. The class A write
took the new path: the first warm left 1,700,243 of 2,014,504 pages, the export was evicted,
5,825,603 pages to 0 in one pass, and the second warm left 2,014,504. Class B's write, after the
read-back's own eviction, needed one warm. Both read-backs passed the oracle. As a sanity check
only, not a measurement to report: the writes took 182.0 s in class A and 183.6 s in class B,
against 195.3 s and 188.9 s at `67ae574`, and Q2 warm's median was 74.20 ms against 74.32 ms. Then
`compare/run.py` refused its own document, as it must for a four-system day with three systems
missing and no variant selection, exited 1 and kept the `.pending` file; the gate's command can
produce no results file, and `compare/README.md` now says how to read tick-store's figures from
such a run. The work folder and the gate's `build/compare-orchestrator-gate`, which held no store,
were removed after their logs were copied.

Raw: `phase4-raw/fullday-precondition/`: `diag.py`, the replay's logs (`debian/asis.*`,
`debian/cold.*`), `67ae574`'s guard samples, the gate's work folder logs, the check logs, the
proof run, and the iteration run's script, logs, part and `.pending` file (`iter/`).

### The final review's codec findings

Reviewer B read the codecs and kernels at `ca96b36` and found one BLOCK, two SHOULD and four
NOTE findings (B-1 to B-7). Each was confirmed by reading the code and then by a test that failed
before its fix; one claim in B-6 was wrong. The record is `docs/REVIEW.md`. Timings below are
Debug builds on the Mac outside `measure.sh`: iteration figures, not results.

B-1, the dictionary decoder's repeat check. Confirmed: the hash set took bits 32 and up of
`value * 0x9E3779B97F4A7C15`, so entries agreeing in their low 32 + log2(slots) bits share a
slot. `scan_for_repeats` now reports its steps, every slot looked at, and the failing test counted
536,887,296 steps for 32,768 entries `k << 48` (K(K + 1) / 2) and 524,800 for 1,024; the same with
`k` times the multiplier's inverse, whose products are all below 2^32. The reviewer's block, both
nested PFOR blocks delta-coded, is 738 bytes and took 1,507 ms to decode; as a crasher it took
the dictionary replay from 3.8 s to 17.7 s. The fix keeps the hash set, which came in with `da20832`'s
decoder changes (dictionary decoding from 7.05–7.42 to 2.41–3.53 ns a value, all of them
together), and gives it a budget of 4 steps an entry plus 16:
past that it sorts a copy and looks for a neighbouring pair, counting the comparisons, so the
work is at most 5 K + 17 steps and the sort, O(K log K). The block now decodes in 8 ms and the
replay takes 5.2 s. What was tried: taking the product's top log2(slots) bits, the true Fibonacci
hash the comment named. On arithmetic progressions of 16 to 2^20 values with steps of 1 to 2^32
(`b1-pattern-steps*.txt`) it spread no better: the original hash ran past the budget on 16 values
of step 64, 65,536 of steps 2, 4 and 10, and 2^20 of steps 4, 64 and 1,000; the top bits on
65,536 of step 65,536 and 2^20 of steps 10,000 and 65,536, and they took up to 2.9 steps an entry
at 2,048 and 16,384 values, where the original stays under 2. Any fixed hash can be defeated, so
the budget is the fix, and the hash stayed as measured. The fuzz corpus on the Mac,
1,373 inputs, holds 75,373 ranked blocks whose dictionary decodes: the largest has 2,418 entries,
the most steps any took is 2,964, and none took more than 2 steps an entry, so the fuzzer had come
nowhere near the 16,384 an entry of the hostile block, and no block in it reaches the sort. The
block is now `tests/unit/crashers/dictionary/timeout-entries-in-one-hash-slot` and a seed from
`generate_seeds`.

B-2. The scalar-capped run's filter lacked `RunLength`, `PatchList` and `CodecRegistry`, and also
`CodecEval`, whose harness decodes PFOR and Stream VByte; 34 tests joined it, and the run went
from 257.9 s to 267.6 s. No injected kernel fault can show the gap: `SimdKernels` checks the
scalar set against its references directly on every host, so the run adds the codecs' own edge
cases under the scalar dispatch, which is what the comment promised.

B-3. The reviewer's arithmetic holds. A sorted dictionary of 0 to 2,047 and 2^20 ranks of 2,047 is
a valid 7,342-byte block; it decodes to 2,047, 2,046, ..., 0 over and over, and moves
2,146,435,072 16-bit entries, 4.29 GB, in 0.162 s. A store piece's 16,384 such values are 286
bytes and 67 MB of moves. No format change: `docs/store.md`'s worst case now states the cost and
what a reader of untrusted files should assume, and a test pins the 286-byte block.

B-4. `decode_column` decoded 1,048,576 values of a block before refusing it against a count of
1. It now reads each block's count first, as the query path's piece index already did, and refuses
with the same error and values; a count of 0 or past the cap is still the block decoder's to refuse.

B-5. The columns oracle read a frame's prefix before checking its length. Not reachable, since
`rebuild_row` always gives a whole frame; the check moved into `frame_states_its_size`, whose test
crashed on an empty frame (SIGSEGV) before the reorder.

B-6. Confirmed that no decoder calls `unpack_delta_narrow` and that PFOR calls `unpack_delta` only
where `fuse_delta` is set, AVX2 alone. But the narrow kernels check their own precondition and
fall back to `unpack_delta` (`kernels_neon.cpp`, `kernels_avx2.cpp`), and the kernel test passes
bases on both sides of it, so the reviewer's "a future caller would inherit a precondition nothing
enforces" does not hold. Only the comment changed; no kernel was deleted. Which kernels the store
calls, from every call site: on every set `unpack_vector`, `unpack_linear`, `add_base` and
`delta_pass`; on AVX2 also `unpack_delta`, for full delta vectors with no exceptions. Stream
VByte's `svb_decode` and `prefix_sum` are never on the store's path, since the store writes and
reads only PFOR, dictionary, run-length, raw and zstd. Bench and tests only: `unpack_delta` on
scalar and NEON, `unpack_delta_narrow` on every set, and each set's unchosen prefix-sum form. Of
the decode targets, DS-3's two-pass NEON form and DS-4's fused AVX2 form are the store's own;
DS-3's fused NEON companion is bench-only, and DS-8's `delta_of_delta_decode` is reached only by a
PFOR block of transform 3, which the store's writer never chooses. The full list is
`fix-codecs/kernels-in-use.md` in the raw folder.

B-7. Two 1,025-value PFOR columns joined the unit suite's integer oracle test: a clock by delta
with no exceptions and prices by frame of reference with one. Their first try, a clock starting at
34.2 * 10^12, put an exception in the vector, since a delta block's first delta is its first
value, so AVX2 would not have fused it; the clock starts at 40 now, and the encoder's stats pin
the shape. Trying every cut and flip of both took the test from 10.5 s to 74 s; a cut at each of
the first 24 bytes and every 97 after, and a flip every 7 bits there and every 257 after, take it
to 16 s. With `values[1023] ^= 1` injected in NEON's `unpack_vector`, the 40-value columns passed
and the new ones failed 92 times.

The TASKS item on PFOR's fuzz oracle checking values, not bytes, is closed with its evidence there.
With the tail-padding refusal removed from `decode_pfor_block`, `Pfor.EachVectorCorruptionHasItsError`
failed and `FuzzOracles.PassEveryCutAndFlipOfAnIntegerColumn` and the PFOR, dictionary and
run-length crasher replays passed, as the item says.

Raw: `phase4-raw/fix-codecs/`: each finding's failing and passing logs, the helpers in `tools/`
(the hostile block, the corpus scan, the hash patterns, the worst move-to-front block),
`kernels-in-use.md` and the check logs.

### The final review's store findings

Reviewer C read the container, the ITCH store and the queries at `ca96b36` and found one SHOULD
and nine NOTE findings (C-1 to C-10); the orchestrator's gate added one more, G-3 (the review's id, not
the fuzzing target), the message that read "expected 0, have 0". Each was confirmed by reading the code, then by a test seen failing for
the reason given, except C-9, whose demonstration was wrong, and C-4, which needed no code. The
file format did not change: no writer byte moved, and the golden tests passed unchanged after
every commit. The record is `docs/REVIEW.md`.

C-1. Deleting the moves, the smaller fix the triage preferred, was not possible: `create` and
`open_at` return the writer inside `std::expected` by value. The symbol table moved to the heap, so
the pointer `SegmentBuilder` keeps stays good across a move. Before: SIGSEGV in `finish()`.

C-2. A failed segment write now latches: the writer keeps the error, drops its file and returns
the error from every later `append` and `finish`, and `take_bytes` gives nothing. The test needs a
segment big enough to reach `write_all` mid-stream, 20 raw records of 65,535 random bytes, with
`RecordingFileOps` failing `write` with `EIO`.

C-3. `O_NONBLOCK` on the reader's `open` makes a FIFO open return at once, and the regular-file
check refuses it. A blocked `open(2)` has no count to bound, so the test bounds its wait at 10 s and
then opens the write end itself, so a failing test ends instead of hanging; before the fix it
waited the 10 s. The harness's own opens without `O_NONBLOCK` are area D's.

C-4. Confirmed as stated and closed without a format change: the reader bounds every column by
the record count already, and `raw_bytes` legitimately holds up to 65,535 bytes a raw record. The
true worst case is now in `docs/store.md`: about 9 bytes held a raw byte, a 666-byte file held in
8,388,480 bytes (12,595 to 1, pinned by a test), and at the limit about 130 KiB of file for about
36 GiB held, a calculation, not a run.

C-5. The oracle now builds the column form from the whole decode, every segment through
`SegmentReader` with none skipped, and compares the exact path's rows, minutes, counts and sum
with it. With `may_hold` made to skip a raw-only segment, the old oracle passed both its tests and
only `ItchQuery.SegmentsWithRawRowsTakeTheExactPath` caught it; the new oracle failed the new
test and 96 cuts and flips.

C-6. macOS refuses a `write(2)` of more than `INT_MAX` bytes with `EINVAL`; Linux writes up to
about 2 GiB and returns short. Requests are capped at 1 GiB on both. The test writes 2^31 + 1 bytes
of an anonymous mapping to `/dev/null`, so it costs no disk.

C-7. `seek()` now marks the assembler failed when it cannot read or load a segment, and `next()`
returns that error until a load succeeds. Before: on the column-read route, a record of the
segment before; on the load route, a clean empty segment.

C-8. An empty span has no address in the file, so its offset is now 0 only when its pointer is
null, and an empty metadata is reported at its place in the footer (2,549 in the test, not 0). A
header of no segments is reported as its count against the footer's, not as a segment index
against a count less one, which wrapped to 18,446,744,073,709,551,615.

C-9. Refuted as demonstrated: with both `device_flush` branches deleted from `publish`,
`ContainerTornWrite.ACrashAtAnyStepLeavesTheOldFileOrTheWholeNewOne` failed with exit status 3 at
the full-fsync crash points, and two `ContainerCommit` tests failed. The gap is narrower: a process
crash keeps the page cache, so with every flush in `PosixFileOps` doing nothing and returning 0,
all 166 store and durability tests passed. Only a power-cut test could show a missing flush, so
`docs/format.md` and the comments beside the tests now say so.

C-10. The stub store's `write_all_at` called `pwrite` directly and spun on a return of 0. It is now
in durability behind a new `FileOps::pwrite`, with the 1 GiB cap and 0 as `EIO`. `pwrite` has a
default body, so the harness's own `RecordingFileOps` still compiles. Before: 1,001 calls, then
success.

G-3. The store's errors carried `expected` and `actual` whether or not they compared anything, and
`to_string` always printed them. They are optional now. A checksum failure keeps its numbers and
offset in its container cause, so the store error no longer repeats the offset; lookups,
`append after finish` and other refusals of no value print no pair; an unknown codec prints only
what it read. The gate's message now reads "container: block, segment 32, column timestamp, block
56537 (bad checksum: block at offset 878190723: expected 2290514605, have 1031937585)". The
scripts' substring checks still match. `results/roundtrip-12302019.json` still holds the old text,
since results are not regenerated in Phase 4; the freeze rerun will replace it.

The reference-distance seed (TASKS). `generate_seeds` now writes a store of one add and its
execution with the execution's distance made 2 against a list of 1, re-encoded through
`encode_column` and rebuilt so its CRCs hold; the reader refuses it with "bad value: ref distance,
segment 0, column ref_distance, record 1, value 0: expected 1, have 2", and every other seed is
unchanged. On debian at `7855a1f`, the target ran clean for 60 s with the bound in place (39,740
runs). With `faults/store-ref-distance-bound.patch` applied and check.sh's arguments at seed 1, it
stopped on its fifth input, the new seed itself before any mutation, with UBSan's pointer overflow
in `SegmentAssembler::State::named`; the other seeds, each run once with the bound removed, passed.
Before the seed, one run in four found it, at 43 s. G-3's 30-minute runs predate the seed, so TASKS
carries running them again at the freeze.

The checks. clang-tidy's first run over this round's code failed on ten findings in the new
code and tests: unchecked optional accesses the checker could not follow, a boolean it wanted by
De Morgan, two loops without a reserve, two headers included only indirectly and a discarded
`future::get`. They were fixed in `e70d3ab` with no change in behaviour. At `e70d3ab` on the Mac,
`check.sh` passed 23 of 23 steps: dev, ASan/UBSan and TSan builds and their 644 tests each, the
round-trip oracle, clang-tidy, the fuzz build and 20 s of each of the 13 targets, and hygiene. On debian at the same commit, the dev build
and 644 of 644 tests passed, the two real-day tests skipped for want of the day's file.

Raw: `phase4-raw/fix-store/`: each finding's failing and passing logs, the fault runs for C-5,
C-9 and the seed (`tasks238-*`), and the check logs.

### The final review's harness and tools findings

Reviewer D read the harness and the tools at `ca96b36` and found seven SHOULD and eight NOTE
findings (D-1 to D-15). Each is confirmed by reading the code and then by a test seen failing for
the reason given, or shown wrong with the evidence. No file format changed. The record is
`docs/REVIEW.md`.

D-4. Before making `tick_slice` refuse a source whose walk does not stop at `end`, the two real
days' stop kinds: both census files say `"stop": "end"` and `"tail_bytes": 0`, and both
codec-eval files `end at offset <file size>`, so the real slice still builds with the same bytes.
`write_slice` now returns a `SliceError` that says whether the source stopped early, so the CLI
can exit 1 for a refused source, as it does for a slice that does not check out, and 2 for usage
and I/O; the partial file is removed and nothing is published. `evaluate_references` refuses the
same sources, rather than adding a `stop` field a reader would have to look for: a partial day
cannot pass as a whole one either way, and refusing needs no change to the JSON. Before: a body
cut after 1 of 48 bytes, a zero-length frame mid-file and a lone prefix byte were all accepted.

D-3. The reviewer's `/dev/full` exists only on Linux, so the test holds `RLIMIT_FSIZE` at 16
bytes with SIGXFSZ ignored (`tests/unit/file_size_limit.hpp`), which fails the write with `EFBIG`
on both hosts. Before: exit 0 and a 16-byte file. The fix is the `close()` before the check that
the other tools already make.

D-2. Both the CLI and `evaluate_references` now check that every checkpoint size is a segment
size; the CLI's refusal is a usage error, exit 2, with a line in the usage text saying so. Before,
`--segment-records 2 --checkpoint-segment-records 100` exited 0 and measured no checkpoint.

D-5. No script reads `tick_export`'s `source_bytes` (`git grep` over `compare/`, `scripts/` and
their tests), so no reader changed. Before: 772 for the 768-byte fixture with its 4-byte tail.

D-6. The walk now advances with `increment(error)` and returns "cannot walk" when the error is
set. An entry whose status cannot be read is still skipped, as before, under its own error code,
so it no longer shares one with the walk. Before: the test caught the `filesystem_error`; in the
tool, nothing does.

D-7. The finding's fix, `\u00XX` for every byte from 0x80, would corrupt text that is already
UTF-8: a path holding `é` as C3 A9 would read back as `Ã©`. So the writer now checks each
sequence against Unicode's table 3-7 (no overlong form, no surrogate, nothing past U+10FFFF) and
writes a well-formed one as it is. A byte outside one is written as `\udcXX`, the lone low
surrogate that Python's surrogateescape, and so `os.listdir` and `os.fsdecode`, give that byte:
`json.loads` then holds the very string Python would use for such a path, and `os.fsencode`
gives the byte back. A lone surrogate escape is valid JSON syntax, and Python reads it; a reader
that wants strict Unicode would see U+FFFD there, and the escaped output is ASCII either way, so
the document is valid UTF-8 whatever the input. Escaping byte by byte gives the same result as
Python's decoder, which replaces each byte of an ill-formed subsequence, since the bytes after a
lead byte are continuation bytes, which can never start a sequence. Before:
`tick_census` on a source of one frame of type 0x80 wrote a file `json.loads` could not decode;
after, its `type` reads `'\udc80'` and `os.fsencode` gives `b'\x80'`. `scripts/check_oracle.py`
and `compare/coldcache.py` need no change: they decode with `text=True`, which now always works,
and neither reads `message_type` or a census `type`; `coldcache.py` prints the resident files'
list by its repr, which escapes a surrogate.

D-1. The reviewer's demonstration needs `sudo`, which the host rules forbid, so it was shown read
only on debian (kernel 6.12.116, uid 1000), with `tick_coldcache` built at `1acd288`, whose
`page_cache_linux.cpp` is `ca96b36`'s. For each file, a probe asked `tick_coldcache residency`,
then asked `cachestat(2)` directly, then read the file once and took the device bytes from
`/proc/self/io` `read_bytes`. Control, a 64 MiB file of the user's own, evicted by the tool: 0 of
16,384 pages resident, `cachestat` 0, and the read cost 67,108,864 device bytes. Three large files
owned by root, mode 0644 or 0755, which the user cannot write, chosen as unlikely to be cached
(an old copy of a system tool's binary, a Rust `libcore` rlib and apt's source list): `tick_coldcache` said
20,914 of 20,914, 14,405 of 14,405 and 14,012 of 14,012 pages resident, and the reads cost
85,667,840, 59,002,880 and 57,397,248 device bytes, each the whole file. `cachestat` refused all
three with `EPERM`, so this kernel hides them from it too; the tool took that as "no
cachestat", a null `dirty_pages`. The rule is `can_do_mincore()` in `mm/mincore.c` ("make
mincore() more conservative", Linux 5.2): unless the caller owns the inode, has CAP_FOWNER, or
`file_permission(MAY_WRITE)` passes, `do_mincore` fills the vector with 1. A file on a read-only
mount that the user does not own falls under it too, since the write check fails there with
`EROFS`.

The fix checks the same thing before `mincore` and before an eviction: root, or the file's owner,
or `faccessat(W_OK, AT_EACCESS)`; otherwise an error naming the file and the rule. A non-root
caller holding CAP_FOWNER would be refused where the kernel would answer, which fails closed. The
test asks only for residency: before the fix, `evict_file` on a system file would have evicted its
pages, which nothing here may do, so the eviction refusal shares the check but has no test of its
own. The test was seen failing on debian from a local probe branch holding the test alone, which
was deleted unpushed, so no commit on `jb-build` fails. After: the test passes on debian and skips
on macOS, and the rebuilt tool refuses `/etc/passwd` and the old binary with exit 2 while
reading a file of the user's own as before. On debian no file under `build/compare` or `data` is
owned by anyone else, so the comparison's warm and cold proofs see what they saw. In
`compare/coldcache.py`, a refusal is exit 2, which `_tool` raises as `ProofFailed` with the
tool's message; the Python tests use stand-ins for the tool and pass unchanged.

D-8. `LiveOrder` keeps the side and whether it is known, in padding it already had, so the book
grows by nothing. The add heads are unchanged, since every add has a side; only
`creation_ref_heads` moves. The two shares are now over the steps and pairs whose sides are both
known, written beside them as `head_side_steps` and `consecutive_side_pairs`; a step to a head no
value has reached yet still compares with that head's empty owner, as before. No script reads
the head fields (`git grep` over `scripts/`, `compare/` and their tests). The committed
`results/codec-eval-*.json` keep the old shares until the freeze rerun (TASKS).

D-9. The mid-stream flush now passes `locate_of(stream)`, as the final flush did. Examples are
diagnostics: the committed codec-eval files hold none from a file-order column on a non-zero
locate unless a value failed, and every value on both days came back, so their numbers do not
move.

D-10. Of the reviewer's two fixes, the field beside `met` keeps every verdict where a reader
already finds it, and says what it leaves out. A per-type column rebuilds file order from the
type sequence, which the store keeps anyway as `message_type`, so it is `false` there, as
`not_counted` already treats only per-locate columns. No script reads `comparison`:
`render_results.py` judges the Targets block from each column's codecs by `scripts/targets.json`,
so its CR-3 and CR-4 "per locate" rows still show the bits with no caveat; TASKS carries what the
rendering needs.

D-11. The encoders' results go to a small sink, `EncodeErrors`, which the walker owns and passes
down; the first error fails `evaluate_references`, and the CLI exits 2. With the block size
injected as 0, the counting test failed with "an encoding failed: bad parameter: block values at
bit 0: expected 1048576, have 0"; restored, it passes. Decoding each encoding back, the other half
of the finding, was not added: the figures are sizes, the encoders are the store's own, whose
round trips the codec and store tests prove, and a decode pass would lengthen a whole-day run for
no figure. No script reads this tool's JSON and no reference-eval results file is committed, so
the nulls change no reader.

D-12. Documentation only. `first_frames` stops at N whole frames and cuts there, so a source of
exactly N records loses its tail, and only a source with fewer than N is returned whole; the
header now says that.

D-13. The harness's requests are unaffected: `compare/systems/tickstore.py` sends both bounds and
an integer locate for `rows` and `minute_aggregate`, and nothing but the kind for `column_sum`,
which still needs no window. `locate` may still be absent, meaning every locate, as the header
says.

D-14. One helper beside `CliOutput` in `roundtrip_cli.hpp`, `write_cli_output(output, code)`,
replaces the two `std::print` lines in all eight tools. It writes with `fwrite`, flushes, checks
`ferror`, writes stderr, and turns a pass into exit 2 when stdout failed; a failing code stays as
it is, so a mismatch is still 1. `tick_query`'s answers go to `std::cout` as they are made, so
`run_query_cli` stops at an answer it cannot write and returns 2. The helper's test holds the
file-size limit at 256 bytes for a 4,096-byte output, and closes the file under the limit too:
closed after the limit is lifted, the stream wrote out what its flush could not, and the file
reached 4,096 bytes.

D-15. `FlagsSeen`, beside `CliOutput` in `roundtrip_cli.hpp`, holds the flags a command line has
given, and each parser asks it before reading a flag. The Python callers (`compare/`,
`scripts/check_oracle.py`) give each flag once. The first test run took 286 s, since a doubled
`--prove-all-price4` ran the exhaustive proof of every Price4 before nothing refused it; that case
now names an absent source and checks for the usage text, so a regression costs a refused open,
not the proof.

Fault patches. Every patch in `faults/` that touches a file changed here still applies with
`git apply --check`. Three others do not, at this commit and before it: `container-block-crc-skipped`
and `fuzz-container-block-reserved` (`src/store/container.cpp`) apply only with reduced context,
and `fuzz-dictionary-repeated-entry` (`src/codec/dictionary.cpp`) not at all. Those files are the
store and codec rounds', so the patches are left for the prover's fixer (TASKS).

Results. Only the two codec-eval files change when they are regenerated, at the freeze:
`tool_version` 4 (from 3, so a file says which it is), the creation-head side shares and their
new denominators, and `excludes_permutation` beside every verdict. `scripts/check_results.py`,
`render_results.py`, `targets.json` and `store_results.py` read none of these fields, so they
work on the committed files and on the new output alike; no script changed. TASKS lists the
files.

Checks. On the Mac at b61a020, `scripts/check.sh --only`: dev build, dev tests (659, one skipped:
the Linux-only page-cache test), oracle, ASan/UBSan build and tests (659, the same skip),
clang-tidy (122 files), the Python scripts and compare tests, check-results, render-check and
hygiene all pass. The Mac does not build `page_cache_linux.cpp`, so clang-tidy ran on it on
`debian`, where it asked for `sys/types.h` for `uid_t` (50b1cba). On `debian` at 50b1cba, the dev
preset builds, ctest runs 659 tests with none failing and the two real-day tests skipped (no data there), the
Linux-only page-cache test among the passes, and clang-tidy on `page_cache_linux.cpp` passes.

Raw: `phase4-raw/fix-harness/`: each finding's failing and passing logs, and the checks.

### The final review's ITCH, bench and second-pass findings

Reviewer A read the ITCH decoder, the bench support and the benchmarks at `ca96b36` (A-1 to
A-8), and a second reviewer read the codec and store fixes at `eac0adc` (R-1 to R-8). Neither
built or ran anything, so each finding is a claim until a test shows it. The record is
`docs/REVIEW.md`.

A-1. The reviewer's mutation was run on a scratch copy of `frame.hpp`, never in the tree: a
driver built with the scratch header first on its include path, once with the old
`check_stream` and once with the new. The old oracle accepted the mutated cursor's stop on a
257-byte frame; the new one refuses it, and both accept the real cursor. The unit test object
built against the scratch header has to come first on the link line: the cursor is inline, and
with the other objects first the linker kept their unmutated copy and every test passed. Linked
first, the eight old `FrameCursor` tests pass the mutation and the new 257-byte one fails.

A-2. Documentation only, as the reviewer proposed. The test was first written to the header's
promise and failed (`decode_strict` returned an `AddOrder` with side `X`); it now asserts the
decode, so a later field check would have to change the test and the header together.

R-1. The reviewer's demonstration, the error's `actual` of 1,048,576, does not tell a decode from
a precheck: both report the stated count. So each test also gives the nested block of 2^20
values a spare byte that its header counts, which the PFOR decoder refuses only after decoding
every value; before the fix all four nested blocks came back with that trailing-byte error, and
after it with `bad_count` at the nested block's start. The check is `stated_block_count` in
`block.hpp`, which `decode_column`'s own precheck now uses too. A count of 0 or past the cap
is still the PFOR decoder's to refuse, as for the outer blocks. Huffman indices are not a
nested block and are read to the block's count. No error text changes for a well-formed block.

R-2. Fixed in code rather than by deleting the header's sentence: a default `SegmentReader` is
a value a caller can hold, and an empty segment is the answer the header gives. The check sits
after the failure check, so a failed seek still gives its error (C-7's test passes unchanged).

R-3. The parts were chosen from the Linux gate's ctest log at `50b1cba`, where each test of the
capped filter also runs on its own, uncapped and three at a time: `FuzzOracles` took 1,093.5 s
and `ItchQuery` 985.3 s of the 2,344.4 s those tests summed, the query oracle's cut-and-flip test
626.7 s and `ItchQuery.SegmentsWithRawRowsTakeTheExactPath` 534.1 s, and every other suite under
60 s. So those two tests run alone and the rest of their suites apart from the codecs and the
store. The whole filter is kept as it was, as the definition of what the capped run covers; the
partition check asks gtest to list it and each part, so a part's negative pattern means what
it means to gtest, and a renamed test that leaves a part empty fails too. Each part's time on
the Linux host is in the closing gate below.

A-3. The rule is the reviewer's stricter one, 10 batches above a percentile, not 1: with one
above it, p99.99 of the ITCH cases would be the 7th slowest of 59,806, one stray batch from
moving. Counted in hundredths of a percent, n x (10,000 - p) >= 100,000, because in doubles
10,000 batches put 9.99999999999943 above p99.9. The whole-column cases are left with min and max
only, and their batch count beside them; giving them more repetitions would lengthen the
latency run, which is the freeze's to decide. Checked without running a benchmark: the rule
applied by hand to copies of the five committed `results/bench` files passes
`check_results.py`, and `render_results.render` gives the same tables from the copies as from
the committed files (`phase4-raw/fix-itch-bench/a3-render-diff.log`). `run_bench.py` copies
the latency block whole, so the new field reaches the results file. No script changed.

A-4. The mutation was run, not only read: scratch copies of the two benchmarks, the committed
file before the fix and the tree after it, each with the reviewer's mutation, compiled and linked
with the bench build's commands, run through `heavy.sh` on the first 20,000 frames of the slice
(881,091 bytes, cut into the raw folder) and on one kernel case, `pfor_unpack/w=13/neon`. These
were iteration runs, not measurements, and no figure from them is reported. The poison sits after
warm-up in each repetition, because Google Benchmark calls the case function once per repetition
and warm-up writes the output again. `column_decode_bench` clears its output before each call
and was left alone, as the reviewer found.

R-6. No test could reach the old loops, since they called the kernel's `write` directly, so the
`FileOps` parameter went in first with the loops unchanged, the test was seen failing through it
(`RecordingFileOps`'s `zero_writes`, as for C-10, set by a hook at the chosen write), and then
the loops became `write_all`. The test lives in `durability_test.cpp`, beside `write_all`'s own:
`columns_test.cpp` names its fixtures as `test::`, which `recording_file_ops.hpp`'s namespace
would shadow. `tick_export` calls `create` with the default, so nothing else changed.

R-5. Fixed in the test seam, as the reviewer proposed, not in the stub store: making its own
call injectable would add a parameter to a tool-only writer for a header of 64 bytes. The
mutation was built as for A-1, the scratch object linked first so the linker keeps it; the test
file and `RecordingFileOps` from before the fix were compiled from `HEAD` beside it. The new
test checks the file's bytes, not only the sizes asked for, so a write that went to the wrong
place fails even where the sizes are right.

A-5. Closed with the reviewer's code change rather than a note, since the change is three lines
and a note would leave a caller able to end a stream by accident. `value_out_of_range`'s comment
now says that for "raw bytes" `expected` is the fewest allowed, not the largest. The fix was
checked with the ITCH tests alone, and the closing check's full run found the one it missed:
`StubWriter.RefusesARawMessageWithNoBytes` expected the stub writer's "empty record", which the
encoder's refusal now pre-empts. That check could not be reached any more, so it was removed
rather than kept as a second name for one error (`2258ff0`).

A-6. The signal test cannot be sure the signal lands inside `poll`: the helper waits 50 ms after
reading the command, which the gate writes just before it polls. A signal that came early would
make the test pass before the fix, never fail after it; it failed on each of three runs before.
The handler has no `SA_RESTART`, so `poll` returns `EINTR` on both hosts. The pair check is on
every platform, since a lone path is a mistyped command line wherever it is typed.

A-7. Fixed in code: each `Case` carries the text of its reference, so the text and `verify()`
sit in the same builder. No script reads `verification.against`. The iteration runs that show
the new text are in `phase4-raw/fix-itch-bench/a7.log`; no figure from them is reported.

A-8. What the validators require of `tree_dirty`: only `run_bench.py` reads it, refusing a
measured run whose build says true; `check_results.py`, `render_results.py` and the compare
scripts do not read it, and the results headers do not carry it. So the flag keeps its value
for an export, which the sync script makes from a committed HEAD, and the new `commit_source`
says it was not compared. `tick_codec_eval` and `tick_reference_eval` write `tree_dirty` from the
same build info, so they write `commit_source` too; the codec-eval files are rebuilt at the
freeze anyway (TASKS), and no reference-eval file is committed.

R-4 and R-7 are closed by correcting a comment and documenting the moved-from state, the
smaller honest change in each case. R-7's guard, an `invalid_argument` error from a writer whose
table is gone, would add a branch to every `append` for a call no caller makes; a moved-from
object that may only be destroyed is the usual C++ contract, now written down.

The closing gate. On the Mac, `scripts/check.sh` ran the dev build and tests, the oracle,
ASan/UBSan, TSan, clang-tidy, the fuzz build and the short run of all thirteen targets,
`check-results`, `render-check` and hygiene. Two of its runs found what the per-finding tests had
not: the stub writer's test of an empty RawMessage (A-5, above) and three names in the A-6 signal
test that clang-tidy's include check could not trace through the C library's private headers,
on macOS and, as CI showed, on Linux. After
those, every step passed, the unit runs at 678 tests with the one Linux-only test skipped
(`phase4-raw/fix-itch-bench/check2`, `check3`). On the Linux host the dev build and ctest at
`9ca302d` failed only the R-3 partition check: before CMake 4, script mode sets no policies, and
`if(IN_LIST)` is not an operator without CMP0057, so the check failed on its first `if` there
while it passed on the Mac's CMake 4. With the policy floor set (`d05f4e2`), the gate passed,
and again at `74c3aa4`, the last C++ commit, whose change is the comment above the signal
test's suppression, with clang-tidy run on that test there too: 676 entries passed, the two
real-day tests skipped for want of the day's file, no entry near 900 s. At `74c3aa4` R-3's parts
took 106.66 s (codecs), 124.32 s (store), 392.66 s (query), 436.90 s (the exact-path test),
379.73 s (fuzz oracles), 214.47 s (the query oracle) and 0.28 s (the partition check), against
1,613.25 s for the single entry at `50b1cba`. The slowest entries of the run were the query
oracle's cut-and-flip test uncapped, 481.11 s, the exact-path test's capped part, 436.90 s, and
`ItchQuery.SegmentsWithRawRowsTakeTheExactPath` uncapped, 428.56 s, three at a time on a host
whose load average was 9 to 12 from other work (`phase4-raw/fix-itch-bench/debian-gate/`).

### The final review's second pass over the ITCH, bench and harness fixes

Two more reviewers read the fixes: one the ITCH and bench round at `381c738` (RA-1 to RA-6), the
other the harness round and the query fuzz oracle at `c804d77` (RD-1 to RD-6). Neither built or
ran anything. The record is `docs/REVIEW.md`; raw logs are in `phase4-raw/fix-second-pass/`.

RA-1. Confirmed by running the reviewer's mutation, not only reading it: with the stop checks A-1
added deleted, all 669 unit tests passed. The fix is the reviewer's: the checks moved unchanged
into `check_stop`, and the new test hands it eleven wrong stops and six right ones. Each wrong
stop expects the exact refusal text, so the test names the check that refused it; with the A-1
lines deleted again, the seven stops only those lines refuse came back accepted, and the four the
older checks refuse were still refused. A unit run of the whole suite takes about five minutes on the
Mac; the two runs above were made directly, and every later full run goes through `heavy.sh`.

RD-6. The reviewer suspected the blind spot by reading; it was run. With a raw row's type set to
0 in `row_values`, nothing noticed: the whole unit suite, the oracle step, and `fuzz_itch_query`,
`fuzz_itch_store` and `fuzz_columns` each run once over their seeds. The column export writes
that byte for a raw row but rebuilds the record from its raw bytes, so its round trip does not
read it back. The check added is the smallest independent one: the column form takes each
record's type from the first byte of its re-encoded payload. With it, the same mutation failed
two unit tests and the query fuzzer's seeds. What the column form still shares is said in its
header and `fuzz/README.md`: which form a typed record takes and a hot or rare row's fields.
For the fields, a hot row's share count made one more in `row_values` failed 12 `Columns`, `ColumnsFlip` and
`ColumnsCli` tests, the round trip and golden export among them; which form a record takes was
not tried. On macOS, libFuzzer's crash handler hung after reporting the oracle's failure on the
first failing seed, and was stopped by hand once its message was in the log.

RA-2. Checked in code, not narrowed in the text, since both checks are a few lines. The shorter
cursor's stop is now held to the same rule as the input's, its offset where its last whole
frame ends; the old check, that it fell inside the cut, follows from that. No in-tree test can
reach the shorter-input check with a wrong stop, because the cursor is inline and `constexpr`,
so it was shown with a driver in the raw folder built against a mutated copy of `frame.hpp`
first on its include path (`phase4-raw/fix-second-pass/ra2-driver/`), as for A-1. The unit
suite's `check_stop` test covers what the shorter stop goes through.

RD-2. Fixed in code, the reviewer's option (a), not by describing the empty owner in the header:
the denominator was added to count known sides only, and the header said so. `same_locate`
still compares a first step with the empty owner's locate 0, so a first step counts as the
same locate only for an order on locate 0, which ITCH gives only to messages that name no
stock; its denominator is every head step, as the header says. `tool_version`
stays 4. It went to 4 for D-8 and D-10 at `ba7ae04`; the committed codec-eval files are both
version 3, no file of version 4 has been published, and the freeze rerun replaces both, so a
version 4 file can only ever be written by code that has this fix. Its comment now lists RD-2.
No results file was regenerated.

RD-1. The reviewer's fix: the window check lists the three kinds that read a window, so an
unknown kind reaches the error that names the kinds. A request that also has a field of the
wrong type is still told about the field first, which is true of it either way. No caller in
`compare/` sends an unknown kind.

RD-3. Shown through the error a test can reach, a source cut inside a frame (D-4's), rather
than the injected encoder fault the reviewer named: both went out under the same wrong words.
The error is written alone, as the reviewer proposed, without a tool-name prefix, since none of
this CLI's other errors has one. `itch::decode` refuses only an empty payload, which no frame
holds, so the decode error that keeps the old words is not reachable from a file today.
`tick_codec_eval` has the same line: given `--columns bogus` it says "the source did not decode:
no column is named bogus" (`phase4-raw/fix-second-pass/codec-eval-sibling/`). It is outside this
finding and is left for a TASKS item.

RD-4. Closed with the reviewer's optional guard rather than left as a note, because a note cannot
make a revert of D-11 fail. The block size rides in `EncodeErrors`, which every encoder helper
already takes, so no helper gained a parameter. The tool has no flag for it and always passes
the default, so its output is unchanged; no reference-eval results file is committed. The
revert first written for the demonstration did not compile (`errors` unused under `-Werror`),
and the unit binary from before ran instead and passed; the run that counts kept `errors` used.

RD-5. The reviewer's demonstration needs a system file on the Linux host and shows only the
exit code, which is 2 before and after. The test shows the order instead, on both hosts: a file
`warm` must refuse because it cannot be opened, a mode-000 file of the test's own, comes second,
and the first file, evicted beforehand, must still have no resident page afterwards. That is the
same up-front measure that refuses D-1's file, whose refusal `PageCache.AFileThisUserNeitherOwnsNorCanWriteIsRefusedOnLinux`
already shows. The extra measure is one fresh mapping and `mincore` per file, which touches no page.

RA-5. Fixed in the script, the reviewer's first option, rather than by narrowing A-8's row: the
row's claim is now true of the published file too. `tree_dirty` stays out of the header, since
`check_builds_match` refuses a measured run whose build says true, so every published file
would carry false. Nothing reads the new key: `check_results.py` and `render_results.py` ignore
header keys they do not know, and the script's own validator test still passes. The five
committed bench files predate the key and are replaced at the freeze, so no validator requires
it yet; TASKS has the item to make `check_results.py` require it once they are. No results file
was regenerated.

RA-4. The reviewer's fix, `std::format` of the case's `reference` in the three messages and the
two comments corrected. The reviewer's way of showing it, A-4's mutation run through the
benchmark, was not repeated: this round runs no benchmark, and the change is to text written
only when a case has already failed. No timed code changed, but it is a change to `bench/`, so
it falls under the freeze rerun that TASKS already asks for the bench files.

RA-3. The reviewer listed six lines; clang-tidy on macOS found a seventh once the block was
narrowed, `quiet.sa_handler`, which is a macro over a member in both C libraries and so names a
private header too. The probe for a hidden missing include had to compile, so it used a name
the test's headers bring in without including `<algorithm>`; deleting `<thread>`, as the
reviewer proposed, is a compile error in either form. The probe was never committed.

RA-6 is closed by moving the comment and correcting the example, as the reviewer proposed. The
same reviewer also found that REVIEW's R-1 row said a nested
block gets the same error as before in every case, which holds only for a well-formed nested
block; the row now says so, as this file's R-1 paragraph already did.

The closing check of this round. On the Mac, `scripts/check.sh` through `heavy.sh` at `d45f4a4`
ran the dev build and tests (682, the Linux-only test skipped), the oracle, the ASan and UBSan
build and tests (682, the same skip), clang-tidy on all 122 files this host builds, the fuzz
build, the 20-second runs of `fuzz_itch_frame`, `fuzz_itch_message` and `fuzz_itch_query`, the
three targets that compile a changed oracle, the Python script tests (501, 4 skipped) and
hygiene. Every step passed but clang-tidy, which found five things in RA-1's new test: three names
of `frame.hpp` it took through another header, and two C arrays. `abd1b52` fixes them; clang-tidy
on that file then passed, and the frame tests passed again under ASan and UBSan. On the Linux
host at `abd1b52`, the last C++ commit, the dev build passed, clang-tidy passed on the signal
test (RA-3's narrowed suppression, which CI's Linux run once found wanting), the page-cache test
and the frame test, and ctest passed 680 of 682 with the two real-day tests skipped for want of
the day's file, in 1,350.6 s; its slowest entries were the query oracle's cut-and-flip test
(445.8 s) and the exact-path test's capped part (432.4 s). A first gate at `d45f4a4` was stopped
near its end once clang-tidy had found the test to fix (`phase4-raw/fix-second-pass/`).

### The final review: Phase 2 items closed

The gate on the review found open items still under Phase 2 in docs/TASKS.md. Each below was
checked against the code and the committed results, then ticked or closed with its evidence.
Raw notes are in `phase4-raw/phase2-items/`.

**Phase 3's parsers and decoders have fuzz targets: ticked.** `fuzz/CMakeLists.txt` builds 13
targets, and `fuzz/README.md` names each one's oracle. Matching them against every function in
`src/` and `include/` that reads untrusted bytes leaves nothing out:

- the ITCH layer: `itch::decode` and `decode_strict` (`fuzz_itch_message`) and `FrameCursor`
  (`fuzz_itch_frame`);
- the codecs: the block decoders of Gorilla timestamps and XOR, PFOR, Stream VByte, dictionary
  and run-length, each with its own target, which also reach the shared block header reader, the
  patch lists, lane unpacking and the price-to-double mappings; `decode_xor_doubles` only
  reinterprets `decode_xor`'s words and parses nothing itself;
- the container: `ContainerParser` (`fuzz_container`), which `ContainerReader` and the ITCH store
  reader both open;
- the ITCH store: its header, symbol table, column metadata, zstd wrapper, seek index, run
  directory, run positions and time residuals, reached through `ItchStoreReader` and
  `SegmentReader` by `fuzz_itch_store` and through the query engine by `fuzz_itch_query`;
- the column directory and manifest (`fuzz_columns`) and the stub store (`fuzz_stub_store`).

The harness's readers (`ColumnsReader`, `StubRecordReader`, `ItchStoreRecordReader`) and its tools that
read ITCH (census, slice, codec and reference evaluation) call only those parsers. The 30-minute
record is `results/fuzz-g3-ea1dde4c02e4.json`: at `ea1dde4`, all 13 targets on macOS at NEON and
the 7 AVX2 or CRC32C path targets on Linux at AVX2, 1,801 to 1,804 s each, every run clean with
no finding. Code has changed since `ea1dde4`; the rerun at the freeze is the open G-3 item under
Phase 4.

**What `cache-misses` counts on Zen 1: closed.** The pinned host's processor, the V1605B, is
family 17h model 11h. In Linux 6.12, `arch/x86/kernel/cpu/amd.c` marks family 17h models 00h to
2Fh as Zen 1, and `amd_pmu_event_map` in `arch/x86/events/amd/core.c` then takes the generic
events from the table `amd_zen1_perfmon_event_map`. There `cache-misses` is raw 0x0964, event
0x64 with unit mask 0x09, and `cache-references` is 0xff60, event 0x60 with unit mask 0xFF. The
same table entries are in the 6.12.116 stable tag, the kernel the Linux results files name. The
kernel commit that added the miss entry, 25d387287cf0 (2020), gives AMD's bit names as its reason.
AMD's own documents: no public reference was found for model 11h itself, but the Open-Source
Register Reference 56255 Rev 3.03 (July 2018), which covers family 17h models 00h to 2Fh, defines
both events in its L2 cache events section (2.1.11.3.6), and the Processor Programming Reference
55570-B1 Rev 3.16 (April 2021), for the sibling model 18h, gives the clearer bit names in its own
(2.1.14.3.6). PMCx064 counts the outcome of the core's cacheable L2 requests, not counting the L2
prefetcher's: bit 3 is a data cache request that missed the L2, bit 0 an instruction cache
request that missed it. PMCx060 with every bit set counts all requests to the L2, the L2
prefetcher's and software prefetches among them. So on this host `cache-misses` is L2 demand
misses from the L1 caches, not last-level misses: some of them hit the 4 MB L3, which has its
own counters outside the core's. And the two events have different scopes, so `cache-misses`
over `cache-references` is not an L2 miss rate; it reads low by however much prefetching there
was. perf's own event list for Zen 1 agrees: `l2_cache_req_stat.ic_dc_miss_in_l2` is event 0x64,
unit mask 0x09.

No committed figure uses the event. `scripts/run_bench.py` asks perf for `cycles:u`,
`instructions:u` and `branch-misses:u` only. Every `cache_misses` field in `results/bench/` is
`unavailable`: 8 cases in the two Linux files, which give the reason "not collected until what the
event counts on Zen 1 is confirmed", and 322 in the three macOS files, which have no counters. The
comparison files hold no counters, and neither README.md, RESULTS.md nor BENCHMARKS.md quotes it.
The reason string stays as it is in the committed files; the rule for the future is that a figure
from this event on this host is called "L2 misses", with the event code (PMCx064, unit mask 0x09)
beside it, never "cache misses" or "LLC misses", and is never divided by `cache-references`.

**Boost on, no fixed clock: closed without a fixed-clock run.** The item asked for a choice
between a fixed-clock run and cycles as the headline. The user has approved no change to the
Linux host beyond the governor write inside its measuring wrapper, and turning boost off is
such a change, so there is no fixed-clock run and none will be made in this project. Cycles
did not become the headline either: the speed rows of the README's headline are Linux times. What the
repository holds:

- Units. Two Linux targets are stated in cycles, DS-2 (`cycles:u` a value) and DS-4 (cycles a
  value, which has no reported figure). Every other Linux speed target is a time or a ratio of
  times: DS-5 in MB/s, DS-9 in ns a value, DS-7 as Gorilla's time over the layout's, WT-1 and
  WT-2 in records a second, WT-3 and WT-4 in seconds against the other systems, QL-1 to QL-3 in
  ms, and QL-4 in ms against the other systems.
- The clock. `results/bench/Linux-column_decode-bcbf8738d892.json` and
  `results/bench/Linux-itch_codec-1b7cfe196483.json` record governor `performance`, turbo
  `enabled`, and an effective clock, user-mode cycles over the wall time of the timed passes, of
  3,562 to 3,577 MHz across their eight cases. Their snapshots of the reported clock at the
  wrapper's load lines, before and after each case, read 1.61 to 3.36 GHz. The two comparison
  files, `results/compare/Linux-12302019-full-67ae574f5666.json` and the slice's, record
  governor `performance` and turbo `enabled` and nothing more: no frequency and no counters. So
  the clock during each system's run in the comparison is not recorded.
- Spread between repetitions. In the two bench files, the coefficient of variation of the rate
  over each case's ten timed passes is 0.24% to 1.45%. In the full-day comparison, tick-store's
  headline queries spread, max less min over the median, 2.7% to 13.7% over 30 warm repetitions
  and 4.8% to 17.7% over 7 cold ones; each write ran once per class, so it has no spread. These
  spreads hold every source of variation together, and no file separates the clock's share.

What the results give up: every Linux figure stated in time carries whatever clock boost gave
during that run, and none can be restated at a fixed clock. A rerun on the same code could run
at another clock if the host is warmer or busier, and how far that would move a figure is not
measured. The relative targets compare systems that ran one after another across 8.53 hours,
and whether they ran at the same clock is not measured either. The cycle figures avoid most of
this for work inside the core, but a wait on memory costs more cycles at a higher clock, so
they are not wholly free of it; by how much is not measured. BENCHMARKS.md's Linux host row now
says that boost stays on and no run used a fixed clock. The write-up's statement of this is an
open Phase 4 item.

**cpu6 for the second thread: ticked, as a shared two-CPU mask.** The reported measurements
with more than one thread are the comparison runs. `compare/run.py` starts each system under
the wrapper with `taskset -c 4`, and `pin_threads` in `compare/run_system.py` then sets the
run's own mask to cpu4 and cpu6 (`LINUX_CPUS`) and reads it back, before the runner starts any
server, worker or tool, so every child inherits it; nothing in `compare/systems/` changes it.
That is not a thread pinned to cpu6 by itself: every thread of every system, client and server,
may run on either CPU, and the results say so. Both Linux comparison files,
`results/compare/Linux-12302019-full-67ae574f5666.json` and the slice's at the same commit,
record for each of the four systems `pinning` as verified, started on cpu4, running on cpu4
and cpu6, with `threads` 2, and in the header "cpu6, used by every system" and the statement
that every thread shares those two CPUs. The bench files record "cpu6 reserved, unused: every
case is single-threaded", which holds: no benchmark in `bench/` starts a thread. The one other
threaded path, `tick_roundtrip verify` with its hash worker, is the G-1 correctness gate, run as
a heavy job on cpu2, cpu6 and cpu7 (`results/roundtrip-12302019-linux.json`), and reports no
speed figure.

### The final review's comparison protocol

The triage's "Comparison protocol" binds F-1, F-2, F-3, F-4, E-3 and F-14, with F-10 and F-11
beside them. Raw logs are in `phase4-raw/fix-compare-protocol/`. New documents carry
`"protocol": 2` in their header; the committed files carry none and count as protocol 1.
`scripts/check_compare.py` holds a protocol 2 file to every rule below and keeps the committed
files to the rules they were measured by, so `check.sh` stays green until the freeze rerun
replaces them. No results file was regenerated or edited.

**F-4, the tail figure.** Confirmed: `latency` takes the nearest rank, `ceil(0.99 n)`, which is
`n` for every n below 100, so the "p99" of 30 warm and 7 cold repetitions is the slowest run. In
the four committed comparison files `p99_us` equals `max_us` in all 440 warm and cold blocks.
Repetitions stay 30 and 7. Each block now has `tail_us`, `tail_rank`, `tail_is_slowest` and
`tail` ("slowest of 30"), and `p99_us` the same figure under its old name, which the generator and
the variant rule still read; the generator's wording is another fixer's item in docs/TASKS.md.
The method block's `tail`, `compare/README.md` and `compare/variant_rule.py` say what the figure
is, and the rule's text changed with it, so a protocol 2 day must state the new text and a
protocol 1 day the old (`RULE_PROTOCOL_1`). Failing first: `TailFigure` in
`tests/compare/test_system_worker.py` (no `tail_rank`), and `Protocol2` in
`tests/scripts/test_check_compare.py`, whose six faults passed with exit 0 (`f4-fail-before.log`).

**F-14, raw repetitions.** Confirmed: the committed files keep a latency block's summary only,
and a write's total seconds per repetition with its engine and Python seconds as medians (the
`67ae574` slice's QuestDB `plain` class B write: seconds 27.03, 22.97 and 23.75, engine 23.51
alone). Every latency block now keeps `seconds_by_repetition`, in the order run, and every write
`by_repetition`. For a protocol 2 file `check_compare.py` recomputes each block's median, tail
figure, fastest, slowest and mean, and each write's three medians, to 1 part in 10^9. Six faults
in `Protocol2` passed with exit 0 before (`f14-fail-before.log`). The raw seconds add 30 floats
a warm block, 7 a cold one and 100 a floor.

**E-3, the second CPU's sibling.** Confirmed: both committed Linux comparison files record every
system on cpu4 and cpu6 and a `sibling_busy_percent` block for cpu5 alone (the slice at
`67ae574`: 0.70 to 2.09% across the four systems). `run_bench.SiblingMeter` takes the pinned core,
so `compare/run.py` keeps a second meter for cpu6 and reads it from the same first snapshot of
`/proc/stat`, which holds every CPU, when the system's wrapper exits; a resumed run needs the
second reading too. `check_results.check_sibling` takes the field and the core, its messages for
the bench files unchanged, and `check_compare.py` applies it to `second_sibling_busy_percent` with
core 6 for a protocol 2 Linux file. Five faults in `Protocol2` passed with exit 0 before
(`e3-fail-before.log`).

**F-2, recording only.** Confirmed on `results/compare/Linux-12302019-full-67ae574f5666.json`:
Parquet `zstd1-rg1Mi` read its input for 77.4 of 158.4 s in class A and 168.0 of 266.3 s in class
B, its writer 81.1 and 88.5 s, while tick-store `v3-seg4Mi` took 195.3 and 188.9 s with 0.3 and
0.4 s outside its tool. The triage keeps the write protocol, so no eviction was added. Every
write repetition now records `engine_only_seconds`, which is `seconds` less `python_seconds` for
every system (Parquet's `engine_seconds` leaves out its class B flush, so it is not the same
thing), the input's pages and resident pages read just before the clock, and the device's bytes
over the write. To read tick-store's residency after its warm and keep the warm's reads off the
device counter, the warm moved from `ingest` into `ready_input`, which the worker calls first;
`ingest` refuses to run without it. The `tickstore-source-warm-no-eviction` patch still
applies, and `SourcePrecondition` still fails on it. On macOS the device counter is `iostat`'s
`disk0` total, reads and writes, so a write's own output is in it. The cold worker test now
gives its scratch layout a stand-in `tick_coldcache`, since the write step reads residency.

**F-1, the cold clock and the store's open.** Confirmed by reading and on the Mac
(`f1_probe.py`, `f1-probe-mac-before.log`): a whole slice store, written by `tick_roundtrip
ingest` and evicted, held 0 of its 2,641 pages; `tick_query`'s open alone, before any query,
left 15 pages, 245,760 bytes, resident, and its ready line reported `open_ns` 2,224,500. The
reviewer's footer sizes for the day's store, about 5 MB, are larger. `tick_query` already reports
`open_ns`, so no C++ changed. A cold tick-store figure is that open plus the round trip,
`parts` keeping both per repetition; the open is in `ItchStoreReader::open` alone, not the tool's
process start. Each system now states whether its cold clock holds its whole open:
Parquet's does, since its client holds no file; ArcticDB's does not (the worker's open opens the
LMDB environment and reads the library's config); QuestDB's does not (the server opens the
table's files before the worker starts). The worker reads the store's residency before the open
and again before the query, both outside the clock, and the proof keeps the one where its clock
starts. Warm figures are unchanged, and `warm_worker_open` keeps the open's time once per worker.
The cold worker's new fields sit after the lines `cold-refilled-run-counted`'s patch reads, so it
still applies and still fails `ColdRuns`.

**F-3, the slice's warm-up.** Confirmed by reading and by a test: `warm_full_day` runs each query
once untimed and checks its answer before the timed repetitions, `step_warm_queries` did not, so
the slice's first timed repetition was each engine's first call of the query in that worker
(`f3-fail-before.log`: a fake engine whose first call is 0.05 s slower had a 0.059 s repetition).
The slice now runs the same `WARM_UP_RUNS`, answer-checked, after the residency read and before
the timed repetitions; the warm-up's reads fall between the two proofs, so a warm-up that evicted
or faulted anything would still fail the after-proof. tick-store's macOS day takes the same path
and gets the same warm-up.

**F-11, the flush report's moment.** Confirmed by reading the order of calls in `step_ingest`. The
report is now read straight after the write's device counter, before `settings_after_ingest`, for
all four systems. The probe's report was already read at once. No validator rule follows: a file
cannot show when its report was read, so the test holds the order.

**F-10, the slice's export before each write.** Confirmed by reading: the claim was in every slice
file's `method.input_cache` and nothing tested it. The base `ready_input`, which tick-store's own
overrides, now warms the slice's export and refuses a write it leaves short; the worker then
reads the residency the file keeps, after the Arrow table is mapped and just before the clock.
A day's export is not warmed: on a day it is about 24 GB and the write protocol is unchanged, so
it stays recorded only, and `Protocol2FullDay` passes a day file whose others' export was 60 of
100 pages resident. The cold worker's stand-in `tick_coldcache` now answers a warm with every page
resident. The real-systems tests passed on the Mac after the change (12 tests).

**The protocol 2 rules, proven.** Each `Protocol2` test injects its fault into a valid file. Nine
prover faults then take one rule of each group out of `check_compare.py` itself: the unknown
protocol, the tail's name, the recomputed repetitions, the second sibling, a write's device
reads, the bytes resident before a clock that holds the whole open, tick-store's cold figure as
its open plus its query, the slice's warm-up and the slice's export resident. `prove_checks.py
--only` with the nine, on the Mac at `db72391`: 9 of 9 PROVEN, each through `py-scripts`. Three
older patches no longer apply at this commit (`container-block-crc-skipped`,
`fuzz-container-block-reserved`, `fuzz-dictionary-repeated-entry`), as before this round. At the
same commit on debian the comparison tests pass with `heavy.sh` on cpus 2, 6 and 7, and
`test_real_systems.py` runs all 12 of its tests, none skipped, once `TICK_STORE_COMPARE_DATA`,
`_VENDOR`, `_WORK` and `_TOOLS` point at the folders beside the exported tree; without them it
skips all 12.

**A protocol 2 slice, iteration-only, on debian at `db72391`.** All four systems, every variant,
end to end, with no `TICK_STORE_MEASURE`, so the file is iteration-only and stays out of
`results/`. Since `run_system.py` pins every system to cpu4 and cpu6, the whole `compare/run.py`
ran under the host's measurement wrapper, which alone may hold cpu4: 84 minutes. `check_compare.py`
refuses the file for three header fields only the per-system wrapper writes, `measurement`
(iteration-only), `governor` and `load_lines`; with those three taken from the reported `67ae574`
file every other rule passes, the protocol 2 rules included. What the new fields held:

- Tail figures: every warm block "slowest of 30", every cold one "slowest of 7", with n and rank.
- Siblings: cpu5 at 0.52 to 1.50% and cpu7 at 0.24 to 1.31% across the four systems.
- Writes: every system's input wholly resident at every clock start, 137,741 of 137,741 pages of
  the export and tick-store's 50,327 of its ITCH slice; device reads 0 for Parquet and tick-store,
  at most 131,072 bytes for ArcticDB and 155,648 for QuestDB, on a machine-wide counter. On the
  slice Parquet's engine-only time is its total and tick-store's within 0.04 s of it; in the
  headline variants ArcticDB's is 0.26 s below the total, QuestDB's 0.23 to 0.25 s.
- Cold clocks: Parquet and tick-store, whose clocks hold the whole open, had 0 store bytes resident
  at every clock start. ArcticDB's open, outside its clock, left 147,456 to 729,088 bytes resident
  before it, by variant; QuestDB's server held 24,576 bytes before the worker opened. tick-store's
  open, inside its cold clock now, had a median of 2.9 ms over 35 repetitions in each variant, and
  left 413,696 or 421,888 bytes resident before the query, by variant; warm, the open it
  reported took 0.44 and 0.49 ms.
- Every slice warm proof records one untimed warm-up per query; class B's flush reports read 0
  dirty pages for every system.

### The second pass over the comparison protocol

A second reviewer read the protocol 2 fixes at `ab57c83` (RF-1 to RF-11), and the first review's
open comparison findings F-5 to F-8, F-13 and F-15 are taken in the same round. Raw logs are in
`phase4-raw/fix-compare-validator/`. No results file was regenerated or edited; the four
committed files pass `check_compare.py` after every change.

**RF-1, protocol 1 for the committed files only.** Confirmed by a test: a protocol 2 slice
document from another commit, with its `protocol` removed and a query's `warm_up_runs` removed,
passed with exit 0, as did one stating `1`, `true` or `2.0`. `check_compare.py` now names the two
commits the committed files carry, `67ae574` and `2503d1e`, in `PROTOCOL_1_COMMITS`: only a file
of one of them may state no protocol, and a stated protocol must be exactly the integer 2. The
test fixtures, which stand for the committed files, carry `67ae574`'s commit. The list empties
after the freeze rerun (docs/TASKS.md).

**RF-2, engine-only seconds as defined.** Confirmed by a test: every class B Parquet write repetition set to 0.5 s of engine-only time, its median 0.5, passed, though each total was 1.9 to 2.1 s with 0.1 s of Python. Each repetition's `engine_only_seconds` must now equal its `seconds` less its `python_seconds`, the worker's own subtraction, to 1 part in 10^9.

**RF-7, tick-store's engine time per repetition.** Confirmed by a test: a cold engine time of 80 and 100 us beside queries of 60 and 70 us passed, since the cold median it was held to holds the open too. Each repetition's engine time must now be within its own round trip warm and its query's cold. On the debian protocol 2 slice of the last round (`85a4912`, iteration-only) the engine's largest share of a round trip is 0.9995 warm and cold, so the rule holds on real runs; that file passes every rule of this round with the three wrapper-only header fields taken from the reported `67ae574` slice.

**RF-5, a macOS write's device reads.** Confirmed: a macOS write's `device_bytes_read` was `iostat`'s `disk0` total, which counts the write's own output. What macOS offers a Python process without installing anything, by a probe on the Mac (`rf5-probe.log`): `proc_pid_rusage(pid, RUSAGE_INFO_V2)` through `ctypes`, whose `ri_diskio_bytesread` is the bytes the device read for that process. Reading a 64 MiB file just evicted counted 67,108,864 bytes, and reading it again resident 0; the same for a running child, a process of the same user that is not a child, and a child that has exited but is not yet reaped (`waitid` with `WNOWAIT`). After the child is reaped its reads are gone: macOS adds them to no parent, where Linux puts them in `ru_inblock`. `getrusage`'s `ru_inblock` counts operations, not bytes. So a macOS write now counts the reads of the processes that did it: the worker, QuestDB's server, and tick-store's `tick_roundtrip`, which the worker runs through `coldcache.run_counting_reads` and reads before reaping; Linux keeps the block device's machine-wide sectors read. On the real tools a 200,000-record tick-store write read 0 bytes from its resident slice and 9,175,040 bytes once the slice was evicted. A process's own executable counts when it is not cached: with `tick_roundtrip` evicted, a write from a resident slice read 1,310,720 bytes, so the test runs the tool once first, as a run's flush probe does. The cold proof keeps `iostat`, where only a shortfall counts. Nothing is left as a gap for the macOS files.
**F-5 and RF-6, the protocol's repetitions, and tick-store alone on a Linux day.** Confirmed by
tests. A protocol 2 slice of 5 warm and 3 cold repetitions passed `check_compare.py` with exit 0,
as did one warm and one cold repetition, a slice written once and a day written three times; and
under `TICK_STORE_MEASURE`, `compare/run.py` ran every system with `--warm-repetitions 5`,
`--cold-repetitions 3`, `--write-repetitions 1`, `--variants plain` or two of the four
`--systems`. The counts now live in `compare/repetitions.py`, which `run_system.py` runs and
`check_compare.py` requires of every reported file: 30 warm and 7 cold per query, 3 writes per
class and variant on the slice and 1 on a day. The four committed files meet them, so the rule
holds for protocol 1 too. A measured run refuses all five narrowing arguments before anything
runs. tick-store alone on a day was a side effect: off Linux any full day was alone, and on Linux
the route `compare/README.md` gave, `--systems tickstore --variants v3-seg4Mi,v3-seg1Mi`, was
validated as a four-system day, exited 1 and kept `Linux-12302019-full-<commit>.json.pending`
(the fake-run test, `f5-run-fail-before.log`, showed it publishing under the four-system name). It
is now asked for by `--tickstore-alone`, on either host, and nothing else gives it. On Linux it
keeps the four-system day's proofs, the guard's peak memory and the warm proof by each process's
storage reads, states that QuestDB did not run, and ends as the macOS run does: validated as
tick-store alone, named `Linux-<day>-full-tickstore-<commit>.json`, exit 0. `check_compare.py`
takes a tick-store-only day from Linux with those proofs and refuses one warm by residency or
without the guard's memory, so fault `compare-tickstore-only-on-linux` gave way to
`compare-tickstore-only-linux-warm-by-residency` and `-guard-memory-unchecked`. A resumed run
reuses a system's part only if the `<system>.arguments.json` beside it, its repetition
arguments, variants, scope and measurement, matches the run's; otherwise the system runs again.
Tested with the harness's fakes; no day was run.
**F-6, class A's proof.** Confirmed by tests: Parquet's class A, `proven` with 2 harness flushes
in its probe or its full ingest, passed, and so did ArcticDB's and QuestDB's with 0 or no dirty
pages in the probe while their class B was not proven. `check_compare.py` now holds a proven
class A to the proof that made it: no harness flush for Parquet, in both scopes, and dirty pages
in the probe for ArcticDB and QuestDB, as `compare/settings.py` decides it. Every committed file
meets it: the smallest class A probe is 120 dirty pages, ArcticDB's on macOS.
**F-7, ArcticDB's LMDB flags.** Confirmed by a test on fakes of the engine: with
`get_config_int` answering `None` for every key, class B's `LMDBStorage.ExtraFlags` passed as 0,
read as `None or 0`. Before dropping the fallback, the real engine in the `.venv`, ArcticDB
6.28.2, was asked in a fresh process per case: `ARCTICDB_LMDBStorage_ExtraFlags_int=0` reads back
0, the class A flags 327680, and the key unset `None`. So an explicit 0 is 0, and `None` now
fails the read-back as a setting not in force.
**F-8, ArcticDB's one-symbol row count.** Confirmed by a test on a fake library: a description
of 5 rows passed beside 4 rows read back, since the row count was compared with itself. It is now
held to the rows read back, as the per-security path already was. `compare/fullday.py` asks for
the stored settings without reading the store back, so there the check is left out rather than
compared with nothing.
**F-13, a test that passed for the wrong reason.** Confirmed: the bare capacity report the test
wrote is refused only as "the file does not start with its header". The test now takes a
document the validator passes and sets its measurement to `compare/fullday.py`'s `MEASUREMENT`,
and requires exactly the iteration-only problem. With that refusal removed from
`check_results.py` the old test still passed and the new one failed. The file still runs only by
hand under the `.venv`, since `scripts/check.sh` does not list it (F-9, outside this round).
**F-15, the variant source and the guard's rate.** Confirmed by tests. A full day passed beside
a variant-source slice file marked iteration-only, or one with a setting not in force, since
only its host, commit and `slice` key were read. `check_compare.py` now validates that file by
every rule it holds a slice to, with the same pins and capacity file, and names each problem
under `variant_selection`. The guard divided each swap delta by the nominal 2 s, while the next
sample comes 2 s after the last one ended, so the rate was overstated, in the safe direction but
not as registered. It now divides by the time between the two samples: 1000 pages 2.5 s apart
read 1,638,400 B/s, where the nominal interval gave 2,048,000.
**RF-8, where a cold clock starts.** Confirmed by a test on a fake system with its open inside
the cold clock and no reported open, as Parquet: with 0 pages resident before the open and 3
before the query, the proof kept 0. It now keeps the reading before the open only where the open
is added to the figure, tick-store's, and the one before the query otherwise. On the committed
files the two are the same, since Parquet's `open()` reads no store file.
**RF-11, an input not whole stops the write in the worker.** Confirmed by tests on the worker's
fakes: a slice write, or tick-store's write on a day, with 7 of its input's 10 pages resident
just before the clock ran to the end, to be refused only by `check_compare.py` at publish, hours
later on a day. `step_ingest` now stops there, before the clock, for tick-store on any run and
every system on the slice; a day's export for the others is still only recorded (F-2). On Linux
a device under the input with no `/sys/dev/block/<major>:<minor>/stat`, as on tmpfs or
overlayfs, now stops the write with the path and the device named, not a bare
`FileNotFoundError`.
**RF-10, statements and the code.** Confirmed. The warm proof's `warm_up_runs` was the constant:
a test with the untimed runs made to run none still stated 1. It now states the fewest any query
ran, and `check_compare.py` holds a protocol 2 slice's warm block to `warm_up_seconds`, one
positive figure per stated run, which the debian `85a4912` slice meets. `run.py`'s docstring now
names both siblings the `/proc/stat` copy measures, and `TICKSTORE_ONLY_WARM`, the warm method of
the macOS tick-store day, says each query first runs once untimed. `compare/README.md` says a
write's device window is the whole ingest call, wider than its clock by QuestDB's `CREATE TABLE`
and its applied-rows check, and that only the device counter is read before the flush report.
**RF-9, tests and faults for the protocol 2 code.** Confirmed by reading: reverting
`run_system.py`'s assembly of the warm-up seconds, tick-store's cold parts or the warm worker's
open, or `run.py`'s second-sibling file, failed no unit test, and twelve protocol 2 rules had a
test and no prover fault. `Assembly` now builds a part from fakes of the workers' answers and
`SecondSibling` runs `run_one` twice with fake meters; a day test holds tick-store's source
whole on a day. Each of the sixteen gaps has a fault in `scripts/prove_checks.py`, every one
proven with `--only`.

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
