# tick-store: one day of Nasdaq ITCH in a columnar store

tick-store keeps a trading day of Nasdaq TotalView-ITCH 5.0, 268,744,780 messages in
8.25 GB, in a store 4.69 times smaller than the feed, 1.76 GB
on disk against 3.09 GB for Parquet's smallest setup, and rebuilds the feed from it.
On the Zen 1, a Linux machine from 2018, it answers an hour of a stock in 74.04 ms
warm, against 44.58 ms † for ArcticDB with a table per stock, while a second and a millisecond across every
stock take it 8.7 and 167 times as long as ArcticDB and
QuestDB, and the whole-day sum of a column 16.8 times as long as
QuestDB †.
A single writer thread there ingests 1.270 M messages/s before a flush, 2.15 times
short of Nasdaq's published peak message rate.

Gorilla, the codec Facebook published for time series, lost to plain integer
codecs in size on every column but the near-empty `mwcb_level`, at 1.09x
their bits on timestamps and, on prices, 1.503x as integer-valued doubles and
3.554x as dollar doubles.

## At a glance

The Zen 1, each system in its fixed setup; queries are warm medians.

| | tick-store | The other system first on the row | tick-store over it, or another setup |
|---|---|---|---|
| Disk, GB, each system's smallest setup | 1.76 | 3.09, Parquet | 0.57 |
| Day write before a flush, s, total; engine-only | 211.7; 211.3  | 111.5; 35.1, ArcticDB  | missed; inputs not alike |
| A second of every stock (Q1), ms | 296.78  | 34.16, ArcticDB  | 8.7 |
| An hour of a stock (Q2), ms | 74.04  | 283.77, QuestDB † | not counted †; ArcticDB, a table per stock: 44.58 † |
| Its per-minute aggregate (Q3), ms | 36.07  | 64.95, QuestDB  | not counted †; ArcticDB, a table per stock: 25.00 † |
| A millisecond of every stock (Q4), ms | 207.95  | 1.24, QuestDB  | 167 |
| The day's sum of a shares column (Q5), ms | 912.75  | 54.46, QuestDB † | 16.8 |

† Here and in the opening, timed while the host's logs record a login or a backup job, or
while the host was swapping memory; the [targets table](RESULTS.md#targets) names each. On Q2 and Q3 the host was swapping memory while a
rival's queries were timed, in this run and in the replaced run, so neither run confirms the
comparison with the fixed setups, which is not counted. The write row is not like-for-like:
tick-store's input was in memory before its clock, and the others read theirs from disk inside it.

## How it was measured

**Data.** The reference day is 30 December 2019, from Nasdaq's public sample files, and a second
day is 30 October 2019; the round trip and the bit-flip check ran on both days on the M4, and the
round trip on the reference day on the Zen 1 too. The round trip compares the
SHA-256 of the rebuilt stream with the source's.

**Hosts.** Every pinned figure comes from the Zen 1, whose processor is AMD Ryzen Embedded V1605B, 4 cores, 8 threads, with
15.33 GiB of memory. Each benchmark is pinned to a core, and
refused if the core's hyperthread sibling was over 5% busy. Context
figures come from an Apple M4 Max laptop, "the M4": unpinned, no hardware counters.

**Clock and repetition.** The Zen 1 ran with frequency boost on and no fixed clock, so systems
timed hours apart may have run at different clocks; what that costs was not measured.

**The comparison.** Parquet, ArcticDB and QuestDB each ran the setup the comparison fixed for it in
advance. Each query ran 30 times
warm, proven to read nothing from storage, and 7 times cold, proven evicted. A
write without a flush returns when the data is in the page cache; a flushed write, after a proven
flush to the device.

Every figure here, read from the results files the [figure list](docs/writeup/figures.json) names, was
measured at commit `ede0176b10bb`, except the fuzz result and the host's record of what else ran, at `8616263b7561`, where the code every other figure measures is unchanged; and the prover's record, a run of the checks of its own.

## Data layout

**The file, in bytes.** A store is a container file: a 16-byte
header, the blocks' payloads, a footer, and a 24-byte trailer ([container.hpp](include/tick_store/store/container.hpp)).
The footer gives each block a 32-byte entry, and each segment's first block
a summary of its times and a bitmap of its stocks, up to 8,192 bytes, so
a time or stock filter reads the footer alone.

**Segments and stored order.** Records are cut into segments of 4,194,304 messages,
65 on the day ([docs/store.md](docs/store.md)). Inside a segment, records are sorted by
stock locate, the day's 16-bit number for each stock, and kept in file order within each stock, so
a single-stock query reads a range of each of the 39 columns. The `locate`
column, despite its name, holds each record's position in the segment's file order: the
permutation that puts the sorted runs back in file order, which a window across every stock must
decode whole. Columns are cut into pieces of 16,384 values, the unit read and
checked, each holding 4 codec blocks of 4,096
values behind an 8-byte frame.

**Where the bytes go.** Of the store's 1,758,234,758 bytes, timestamps take 753.6 MB,
43.0% of the column payloads; then come the permutation in the `locate` column
(315.4 MB), new-order references (199.1 MB), references to existing
orders (136.6 MB) and message types (68.8 MB)
([store sizes](results/store-size-12302019.json)).

**References as distances within the stock.** An execute, cancel, delete or replace names an order
by a 64-bit reference. The best codec measured needs 25.68 bits for it in file
order, 15.6 to 20.7 as a distance back in the day's
orders, and 6.1 to 9.2, by message
type, as a distance back in the stock's own list, so I chose the last ([codec evaluation](results/codec-eval-12302019.json)).
As built, with block metadata, those distances cost 7.563 bits. A distance of 0
is an import: the full reference of an order created before the segment, since no state crosses a
segment; the day holds 10,498,192, at 25.943 bits each.

**Prices as integers, per stock.** ITCH prices are integers in units of 10^-4 dollars. The store
groups add prices by stock and side and codes them with PFOR delta (patched frame of reference on
the deltas), trying each block at a few scales. The best codec needs 15.254 bits per
price in file order, 6.264 bits per stock and 5.480 bits
per stock and side; as built, prices cost 4.606 bits. Per-stock figures here leave out
the permutation, whose order-0 entropy is 12.04 bits per add; the store
pays 9.395 bits per record for it.

**Timestamps: delta, not delta-of-delta.** ITCH timestamps do not tick at regular intervals: the
ideal code for a delta in file order is 17.147 bits, for a delta-of-delta
18.437. PFOR delta reaches 18.757 bits, its patches saving
8.4 bits against the same codec unpatched. I keep timestamps per stock as
residuals from a `time_base` column, the earliest time of each 64 records
in file order, where they cost 22.819 bits.

**Sorting by stock, in bytes.** Stock order costs timestamps the difference between
18.757 bits in file order and 22.819 bits as built, and costs the permutation's
315.4 MB; it buys prices at 4.606 bits against 15.254 bits
and references at 7.563 bits against 25.68. The committed files
hold no whole store in file order, so the net was not measured.

**Segment size.** The comparison ran 1,048,576-record segments beside the default
4,194,304. At the default each stock's run is 4 times as long: Q2 touches
16 segments instead of 62 and takes
74.04 ms warm against 97.96, and the store takes
1,758,234,758 bytes against 1,808,255,218. But a window across every stock
decodes its segment's whole permutation, 25,558,344 values for Q4's 338
rows at the default against 7,104,138, so Q4 takes 207.95 ms
against 55.81 and Q1 296.78 against
115.78. The default was picked by a rule I wrote before any layout was timed:
each layout small enough on disk scores a point on each of Q1 to Q5, warm and cold, on the M4,
where it is within a few percent of the quickest, and the most points win.

## Codecs, and whether Gorilla lost

Gorilla was built as published, delta-of-delta for timestamps and XOR of successive doubles for
values. On `mwcb_level`, with 3 values a day, it takes
35 bytes against 39 for the integer codecs
([codec evaluation](results/codec-eval-12302019.json)).

| Column and order | Gorilla, bits per value | Gorilla over the best integer codec |
|---|---|---|
| Timestamps, file order | 20.404 | 1.09x |
| Timestamps, per stock* | 44.476 | 1.74x |
| Add prices, file order, dollar doubles | 54.218 | 3.554x |
| Add prices, file order, integer-valued doubles | 22.925 | 1.503x |
| Add prices, per stock*, integer-valued doubles | 13.265 | 2.117x |

\* Without the permutation's cost.

A price with cents has no finite binary form, so successive dollar doubles XOR to many
significant bits; an integer tick count stored as a double removes that. In decode time Gorilla
takes 15.44x as long as the store's codecs on timestamps on the M4 and 3.74x on
prices, and 7.76 and 3.35 on the Zen 1; why the timestamp ratio differs
between the hosts was not measured.

**What the writer chooses.** Each column has a fixed codec family: PFOR (plain, delta, head delta,
scaled), a dictionary, run-length, raw bytes, or ranked PFOR, and zstd over a piece where it saves
bytes. zstd still shrinks 7,269 of 7,270 PFOR-coded price
pieces, so the PFOR blocks leave redundancy across values that a block-local code does not reach.

## Decode: kernels and SIMD

**The vector layout.** A full PFOR vector is 1,024 values at a bit width in
16 lanes of 64-bit words, value i in lane i mod 16, so
a row of 16 values unpacks from 16 consecutive words with a shift. Deltas
stay in value order, so the inverse delta is a prefix sum across each row's lanes.

**Which kernels the store calls.** On every instruction set the decoder calls the interleaved and
linear unpacks, the frame-of-reference add and the two-pass delta. On AVX2 it also fuses unpack,
base and prefix sum for a full delta vector with no exceptions; on NEON and scalar that fused
kernel is benchmarked and never called. The narrow fused unpack, Stream VByte and the
delta-of-delta reconstruct are benchmark-only as well.

**What SIMD bought.** On the M4, NEON over scalar, medians over widths: linear unpack
5.68x, two-pass delta 1.80x, fused delta
2.09x and interleaved unpack 1.02x
([M4 kernels](results/bench/macOS-simd_kernel-ede0176b10bb.json)). On the Zen 1, AVX2 over scalar: interleaved unpack
1.24x, two-pass delta 1.25x, fused 1.35x.

**What SIMD changes end to end.** The same Zen 1 binary at AVX2, against itself capped to scalar,
changes the warm engine median by -2.3%, -3.6%, -4.3%,
-3.7% and -7.3% on Q1 to Q5, a negative change being time saved. A
query's own runs range 2.7% to 13.7% of their
median, so on whole queries SIMD's effect is inside or near the run-to-run spread
([counters run](results/diag/counters-simd-12302019-ede0176b10bb.json): a diagnostic summary no separate validator re-reads). SIMD cost 1,870 lines of kernels and dispatch, 6.6% of the
28,526 source lines.

**The store's price column.** Per piece, the reader inflates any zstd frame, allocates, decodes
PFOR with scale and patches, range-checks every value and appends. On the Zen 1 those steps take
11.6%, 20.4% (unpack), 25.3% (patches),
4.6% (scale and range check) and 0.6% (allocation and copy;
copies resolved only to the C library, 19.4%, are not counted there) of
the decode's user cycles ([price-decode profile](results/diag/price-decode-profile-12302019-ede0176b10bb.json): a diagnostic summary no separate validator re-reads).

## Branch prediction and instruction counts

On the Zen 1, in user mode, Gorilla takes on the whole price column 73.72 cycles, 218.11
instructions and 0.403 mispredicts per price, the store's price column
21.99, 47.00 and 0.051 at
2.14 IPC. Over prices and timestamps, Gorilla's mispredicts are
7.9 to 20.3 times the store's codecs' and its instructions
4.6 to 22.7 times.
([Zen 1 column decodes](results/bench/Linux-column_decode-ede0176b10bb.json)).

## Memory

On the M4 the writer's peak memory footprint, the memory it owns, is
1,229 MB at segments of 4,194,304 records; its peak resident
set, which also counts the mapped source file, is 900 MB smaller at
1,048,576. tick-store's query process (`tick_query`) peaks at 472.0 MB resident,
mapped store pages included. QuestDB's day store, 22.90 GB, is larger than
the Zen 1's 15.33 GiB of memory, so warm queries run a single system at a time.

## Writes and queries against three systems

**Writes.** Before a flush, in seconds total and engine-only: tick-store 211.7
and 211.3, Parquet 168.7 and 80.9,
ArcticDB 111.5 and 35.1, QuestDB
678.7 and 193.8. Engine-only is the total minus the
harness's reading and converting of the input, 52% to
71% of the others' totals; tick-store parses its ITCH file inside the
engine. The inputs did not start alike; the targets sections give the bytes read. QuestDB's flushed write is not like-for-like: its flush proof on the
day did not pass. With a flush, tick-store's write took 190.7 s and without a flush
211.7 s, a single run each, so that difference is not resolved.

**Where the writer's time goes.** tick-store's engine-only time is its writer's own work:
2,328 cycles and 3,334 instructions a message at
1.43 IPC (a diagnostic summary no separate validator re-reads). Of its user cycles, PFOR encoding takes
41.0%, assembling segments 19.1%, the book state, each
open order's stock and price kept to code the messages that name it, 18.7%,
dictionary encoding 6.8%, sorting 4.8%, parsing 2.7%
and zstd 1.2% ([writer profile](results/diag/writer-profile-12302019-ede0176b10bb.json): a diagnostic summary no separate validator re-reads). The others' engine-only times may still hold reading their input from disk, so
the clocks are not alike and the gap is not all tick-store's writer.

**Queries.** The table at a glance gives Q1 to Q5 warm; ArcticDB with a table per stock takes
10.2 s on Q1. Q5, the day's sum of a shares column, decodes 152,004,760 values at
6.0 ns each; of its sampled cycles, dictionary decoding takes 30.5%,
the C library 20.6% and the unpack kernels 13.7%
([query profile](results/diag/query-profile-12302019-ede0176b10bb.json): a diagnostic summary no separate validator re-reads).

## Tried and not kept, and not done

I tried and did not keep Gorilla for timestamps and prices; delta-of-delta timestamps; references
in file order and as distances in the day's orders; 1,048,576-record segments; the fused delta kernel on NEON; and Stream VByte. Not built: a stored
per-stock minute aggregate (Q3), a sum over dictionary counts that would skip Q5's decode, a time
index inside a run that would let Q1 and Q4 skip the permutation, and a second writer thread.

A store is a single file holding a single day, written from that day's ITCH file under a temporary name
and published by rename, so a crash leaves the old file or none ([format.md](docs/format.md)). It
takes no appends and does not capture a live feed.

## Targets that held

On both days the store gives every message back bit-exact (met), a flipped bit is caught
in a data block and in the footer (met), Gorilla is built and is exact on the whole day
(met), and the store takes the least disk of the systems compared, each in its smallest setup (met).

## Missed targets

On the Zen 1 unless the M4 is named, units as the bar's; "x" is how many times off the bar.
[BENCHMARKS.md](BENCHMARKS.md) classes nearly every bar as context, measured on other hardware or another
workload: they prove nothing about this store. Nasdaq's peak rate is
a direct bar, and the rows against the other systems are relative.

| Asked | Bar (source) | Measured | Gap | Verdict | Why |
|---|---|---|---|---|---|
| Timestamps, store as built | ≤ 20 bits (KX case study, NYSE quotes) | 22.819 bits; per stock, codec alone, 25.488 bits | 2.82; 5.49 bits | missed in part | Not measured |
| Add prices, file order | ≤ 7.9 bits (ALP paper, stock doubles) | 15.254 bits | 7.35 bits | missed in part | Per stock met |
| Gorilla's bits over the integer codec's, file order | ≥ 1.53x (2025 paper, Indian stocks) | 1.237x to 1.511x | 0.019 to 0.293 | missed in part | Not measured |
| References, store as built | ≤ 11.2 bits (KX case study) | new orders 11.363 bits; imports 25.943 bits | 0.16; 14.74 bits | missed in part | Imports are full references |
| Shares | ≤ 3.8 bits (KX case study) | executions, trades, imbalances 3.907 to 7.758; store, adds 4.165 bits, reductions 7.284 bits | 0.11 to 3.96 bits | missed in part | Not measured |
| Gorilla decode, M4 | ≤ 2.12 ns (mailing-list post, smooth synthetic values) | 8.897 ns | 4.20x | missed | Not measured |
| Gorilla decode in L1 | ≤ 21.3 cycles (ALP paper, Intel Ice Lake) | 60.66 cycles | 2.85x | missed | 198.40 instructions a value |
| Delta decode, M4 | ≥ 3.6 G values/s (FastLanes, Apple M1) | 3.40 G values/s | 5.6% | missed | 7 of 65 widths reach it |
| Delta decode | ≤ 0.377 cycles (FastLanes, Zen 3 server) | 2.709 cycles | 7.18x | missed; a remote login during one of the 65 cases its median takes | Not measured |
| Price-column decode | ≥ 4,605 MB/s (ALP, an AMD EPYC 9554 core) | 1,301 MB/s; M4 3,367 MB/s | 3.54x; 26.9% | missed; M4 missed | Steps under Decode |
| Store against Gorilla decode, prices, M4 | ≥ 14x (third-party benchmark, Apple M2 Max) | 3.74x | 3.74x | missed in part | Steps under Decode |
| Single-thread ingest | ≥ 8.59 M records/s (QuestDB vendor figure, 32 workers) | 1.270 M records/s; M4 3.302 M records/s | 6.77x; 2.60x | missed; M4 missed | A single thread; writer under Writes |
| Ingest above Nasdaq's peak rate | > 2.732762 M messages/s (Nasdaq bandwidth report) | unflushed 1.270 M messages/s; flushed 1.410 M messages/s | 2.15x; 1.94x | missed; two remote logins during tick-store's write; replaced run 1.423 M messages/s | Writer under Writes |
| Q2 warm median | ≤ 16.9 ms (DuckDB on a small Parquet store, preprint) | 74.04 ms; M4 25.28 ms | 4.38x; 8.38 ms | missed; M4 missed | 100 ns a row |
| Q3 warm median | ≤ 0.41 ms (QuestDB vendor benchmark) | 36.07 ms; M4 15.06 ms | 88x; 37x | missed; M4 missed | No stored aggregate |
| Q4 cold mean | ≤ 10 ms (TimescaleDB, order-book preprint) | 356.09 ms; M4 136.16 ms | 36x; 14x | missed; M4 missed | 35.1 MB read cold |
| Lowest median and p99 of the fixed setups, each query | same machine | warm medians in the table at a glance | 8.7x on Q1; 167x on Q4; 16.8x on Q5 | missed in part; met on Q2 warm, Q2 cold, Q3 warm and Q3 cold against each system's chosen setup, while ArcticDB with a table per stock is faster, and on Q5 cold; met, not counted: swapping memory in both runs (ArcticDB, QuestDB); swap-in: ArcticDB, QuestDB, tick-store | Q1 to Q5 under Queries |

The writes against each other system, on both clocks: without a flush, missed; inputs not alike, by each write's record: tick-store's input is in memory before the clock starts, with 250,916,864 B read from the device inside the clock; Parquet's, ArcticDB's and QuestDB's are read from disk inside it, with 18,411,094,016 to 21,521,625,088 B read from the device inside the clock; engine-only, the clock less reading the input and putting it in the form the system's call takes, may still hold those reads, which are counted over the whole clock, so the two-clock rule does not take the difference out.

With a flush, met against QuestDB, not like-for-like; missed against Parquet; against ArcticDB met on this run and missed on the replaced one, a change for tick-store, counted as missed; two remote logins during tick-store's write; replaced run 188.9 s; a backup run and a remote login during QuestDB's write; replaced run 1,269.2 s; met, not counted: a disturbance the replaced run has no record to rule out (QuestDB); inputs not alike, by each write's record: tick-store's input is in memory before the clock starts, with 274,239,488 B read from the device inside the clock; Parquet's, ArcticDB's and QuestDB's are read from disk inside it, with 23,846,309,888 to 24,631,062,528 B read from the device inside the clock; engine-only, the clock less reading the input and putting it in the form the system's call takes, may still hold those reads, which are counted over the whole clock, so the two-clock rule does not take the difference out.
Against QuestDB it is not like-for-like: QuestDB read 24,631,062,528 B from disk inside
its clock, tick-store 274,239,488 B, its input in memory before its clock.
The [targets table](RESULTS.md#targets) gives both runs'
figures; the cause of ArcticDB's move between them was not measured and is not known.

## Not judged

| No published figure, or no comparison day on the M4 | Verdict |
|---|---|
| The store's decode speed-up over Gorilla, Zen 1 | not judged |
| Delta-of-delta decode with bit parsing, M4 | not judged |
| The same, Zen 1 | not judged |
| Writes against the other systems, M4 | not judged |
| Queries against the other systems, M4 | not judged |

## Fuzzing and checks

Every parser and decoder ran 30 minutes of fuzzing under
AddressSanitizer and UndefinedBehaviorSanitizer: 13 of
13 targets ran clean on the M4 and 7 of 7 on the
Zen 1, the shortest run 1,801 s ([fuzz results](results/fuzz-g3-8616263b7561.json)). The fuzzing target is
met; a backup run and two remote logins during its bound-seed gate, which could only have slowed them; three remote logins during its timed fuzzing runs, which could only have slowed them.

A login or a backup run can take executions from a fuzzing run, not hide a finding; the
[runs by target](RESULTS.md#g-3-runs-by-target) give each target's executions.

The checks and the faults that prove them are in the README; the prover's
record covers 99 of its 487 listed faults.

## Limits

Performance comes from a pinned day on a single host, the Zen 1, whose 15.33 GiB of memory
is smaller than the other systems' 23.86 GB of input, with each day write run
1 time. A fair protocol would give every system its input resident before its
clock, or none, and repeat every write. The disturbance records hold 31 entries
inside the campaign's timed phases and 10 inside the fuzz run's, an event
entered for each phase it fell in. The host's own scheduled jobs ran through every timed window
on the Zen 1 and were not stopped: a repository backup every 15 minutes, a health collector and
2 small watchers every 5, 2 sync jobs every 10, a file backup about hourly, and daily maintenance;
only the runs the logs place inside a phase are listed. The Zen 1's day was measured
again: the earlier run, "the replaced run", is kept, and its figure stands beside this run's
wherever a mark names it. In the targets sections a verdict marked "swap-in" rests on figures timed
while the host was swapping memory, and a verdict marked with a login or a backup run on a phase
that held it. Where this run and the replaced run both carry the swap on a rival's side, neither
confirms the other, and such a verdict keeps its word and is not counted.
Not measured: the
cycles a mispredict costs and any memory-level counter.

<!-- generated by scripts/render_writeup.py from docs/writeup/WRITEUP.template.md and docs/writeup/figures.json: edit those, never this file -->
