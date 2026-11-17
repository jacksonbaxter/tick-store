# tick-store: one day of Nasdaq ITCH in a columnar store

On a trading day of Nasdaq TotalView-ITCH 5.0, 268,744,780 messages in 8.25 GB,
tick-store keeps a store 4.69 times smaller than the feed, takes the least disk of the
systems compared (met), and gives every message back bit-exact (met).
Keeping each stock's records together makes an hour of a stock and its per-minute aggregate
quicker to query than in the other systems' fixed setups (met against each system's chosen setup, while ArcticDB with a table per stock is faster; swap-in: ArcticDB, QuestDB), while
short windows across every stock are answered quicker by ArcticDB or QuestDB
([targets](RESULTS.md#targets)).
Gorilla, the codec Facebook published for time series, lost to plain integer
codecs in size on every column but the near-empty `mwcb_level`, and the writer, on a single
thread, ingests below Nasdaq's peak published message rate on the older test machine
(missed on the unflushed write).

## What was measured, and against what

**Data and correctness.** The reference day is 30 December 2019, from Nasdaq's public sample files;
a second day, 30 October 2019, serves the round trip and the bit-flip check. The round trip
re-encodes every record and requires the SHA-256 of the rebuilt stream to equal the source's:
met. A flipped bit in a data block and in the footer is caught with block checksums on
and off (met), and every parser and decoder is fuzzed for
30 minutes under AddressSanitizer and UndefinedBehaviorSanitizer
(met; a backup run and two remote logins during its bound-seed gate, which could only have slowed them; three remote logins during its timed fuzzing runs, which could only have slowed them).

**Hosts.** Every pinned figure comes from a Linux host whose processor is AMD Ryzen Embedded V1605B, 4 cores, 8 threads, made in
2018, with 15.33 GiB of memory: "the Zen 1". Each benchmark is pinned to a core, and
refused if the core's hyperthread sibling was over 5% busy. Context
figures come from an Apple M4 Max laptop, "the M4": unpinned, no hardware counters.

**Clock.** The Zen 1 ran with frequency boost on and no fixed clock, and no run had boost off, so
systems timed hours apart may have run at different clocks; what that costs every time-based
figure here, decode, write and query, was not measured. The
coefficient of variation in the Zen 1's benchmarks is 0.21% to 12.50%, and the
range of a tick-store query's runs over their median 2.7% to
13.7% warm and 5.2% to
20.4% cold; each day write has a repetition count of 1.

**The targets.** Nothing published measures a columnar store of ITCH, so each speed and size target
is a figure published for something near it, mostly on newer processors; the rest compare tick-store with the other systems. [BENCHMARKS.md](BENCHMARKS.md) gives every source. 

**The comparison.** Parquet, ArcticDB and QuestDB each ran the setup the comparison fixed for it in advance, plus
the setups a selection run on a slice kept. The queries are a second of every stock at the open
(Q1), an hour of a stock (Q2), its per-minute aggregate (Q3), a millisecond of every stock (Q4)
and the day's sum of a shares column (Q5), each run 30 times warm, proven to read
nothing from storage, and 7 times cold, proven evicted. An unflushed write returns
when data is in the page cache, a flushed write after a proven flush to the device.
A verdict marked "swap-in" rests on figures of the systems named that were timed while pages were
swapped in; a verdict marked with a login or a backup run rests on a phase that held it, beside
the replaced run's figure where the replaced run has it.

**The inputs are not alike.** tick-store's ITCH file is resident in the page cache before its
clock starts; the other systems read the exported columns, 5,825,603 pages
(23.86 GB), as the
cache holds them, mostly from the device inside their clocks. Engine-only time, the total minus the
harness's reading and conversion, still holds what the engine reads, so neither clock removes the
difference; each write verdict, with the targets, gives every system's resident pages and device reads
([Zen 1 comparison](results/compare/Linux-12302019-full-ede0176b10bb.json)).

Every figure is filled in by `scripts/render_writeup.py` from the results file and field the
[figure list](docs/writeup/figures.json) names for it; nothing checks that the field is what the sentence
describes.
Every figure here was measured at commit `ede0176b10bb`, except the fuzz result, at `8616263b7561`, where the code every other figure measures is unchanged; the prover's record, a run of the checks of its own; and the record of what else ran during the fuzz run, at `8616263b7561`, where the code every other figure measures is unchanged.

## Data layout

**The file, in bytes.** A store is a container file: a 16-byte
header, the blocks' payloads, a footer, and a 24-byte trailer ([container.hpp](include/tick_store/store/container.hpp)).
The footer gives each block a 32-byte entry, and each segment's first block
a summary of its times and a bitmap of its stocks, up to 8,192 bytes, so
a time or stock filter reads the footer alone.

**Segments and stored order.** Records are cut into segments of 4,194,304 messages,
65 on the day ([docs/store.md](docs/store.md)).
Inside a segment, records are sorted by stock locate, the day's 16-bit number for each stock, and
kept in file order within each stock, so a single-stock query reads a range of each of the
39 columns. The `locate` column holds each run's positions in file order, so a
window across every stock must decode it whole. Columns are cut into pieces of
16,384 values, the unit read and checked, each holding 4
codec blocks of 4,096 values behind an 8-byte frame.

**Where the bytes go.** Of the store's 1,758,234,758 bytes, timestamps take 753.6 MB,
43.0% of the column payloads; then come the locate column (315.4 MB),
new-order references (199.1 MB), references to existing orders
(136.6 MB) and message types (68.8 MB)
([store sizes](results/store-size-12302019.json)).

**References as distances within the stock.** An execute, cancel, delete or replace names an order
by a 64-bit reference. The best codec measured needs 25.68 bits for it in file
order, 15.6 to 20.7 as a distance back in the day's
orders, and 6.1 to 9.2 as a
distance back in the stock's own list, so I chose the last ([codec evaluation](results/codec-eval-12302019.json)).
Distance 0 means a full reference to an order created before the segment: the day holds
10,498,192 imports, and distances cost 7.563 bits as built.

**Prices as integers, per stock.** ITCH prices are integers in units of 10^-4 dollars. The store
groups add prices by stock and side and codes them with PFOR delta (patched frame of reference on
the deltas), trying each block at a few scales. The best codec needs 15.254 bits per
price in file order, 6.264 bits per stock and 5.480 bits
per stock and side; as built, prices cost 4.606 bits. The per-stock figures leave out
the sequence that restores file order, whose order-0 entropy, 12.04 bits
per add, the store pays in the `locate` column, at 9.395 bits per record.

**Timestamps: delta, not delta-of-delta.** ITCH timestamps do not tick at regular intervals: the
ideal code for a delta in file order is 17.147 bits, for a delta-of-delta
18.437. PFOR delta reaches 18.757 bits, patching saving
8.4 bits; per stock, PFOR patches 28.2% of
values. I keep timestamps per stock as residuals from a `time_base` column, the earliest
time of each 64 records in file order.

**Segment size, the layout choice measured on the full day.** The comparison ran 1,048,576-record segments beside the default 4,194,304. At the default, each stock's run is 4 times
as long, Q2 touches 16 segments instead of 62, and the
store takes 1,758,234,758 bytes against 1,808,255,218; at 1,048,576 Q2
takes 97.96 ms warm against 74.04 ms. A window across
every stock decodes the segment's whole `locate` column: 25,558,344 values for Q4's
338 rows at the default, 7,104,138 at 1,048,576, and Q4 takes
207.95 ms warm against 55.81, Q1
296.78 against 115.78.

## Codecs, and whether Gorilla lost

Gorilla was built as published, delta-of-delta for timestamps and XOR of successive doubles for
values, and is exact on the whole day (met). On `mwcb_level`, with 3
values a day, it takes 35 bytes against 39 for the
integer codecs ([codec evaluation](results/codec-eval-12302019.json)).

| Column and order | Gorilla, bits per value | Gorilla over the best integer codec |
|---|---|---|
| Timestamps, file order | 20.404 | 1.09x |
| Timestamps, per stock* | 44.476 | 1.74x |
| Add prices, file order, dollar doubles | 54.218 | 3.554x |
| Add prices, file order, integer-valued doubles | 22.925 | 1.503x |
| Add prices, per stock*, integer-valued doubles | 13.265 | 2.117x |

\* Leaves out the cost of the sequence that restores file order.

A price with cents has no finite binary form, so successive dollar doubles XOR to many
significant bits; an integer tick count stored as a double removes that. The price-ratio target
is met over XOR on dollar doubles on every price case, and
missed over the best Gorilla variant in file order.

In decode time Gorilla takes 15.44x as long as the store's codecs on timestamps on the
M4 and 3.74x on prices ([M4 column decodes](results/bench/macOS-column_decode-ede0176b10bb.json)), against a bar of
≥ 14x on each: met on timestamps and missed on prices. On the
Zen 1 the same ratios are 7.76 and 3.35.

**What the writer chooses.** Each column has a fixed codec family: PFOR (plain, delta, head delta,
scaled), a dictionary, run-length, raw bytes, or ranked PFOR, and zstd over a piece where it saves
bytes (7,269 of 7,270 price pieces).

## Decode: kernels, SIMD, and what it cost

**The vector layout.** A full PFOR vector is 1,024 values at a bit width in
16 lanes of 64-bit words, value i in lane i mod 16, so
a row of 16 values unpacks from 16 consecutive words with a shift
(8 NEON or 4 AVX2 registers). At width
w the packed values take 128 × w bytes, after a
3-byte header and the base, then a
2-byte exception count ([pfor.cpp](src/codec/pfor.cpp)). Deltas
stay in value order, so the inverse delta is a prefix sum across each row's lanes.

**Which kernels the store calls.** On every instruction set the decoder calls the interleaved and
linear unpacks, the frame-of-reference add and the two-pass delta. On AVX2 it also fuses unpack,
base and prefix sum for a full delta vector with no exceptions; on NEON and scalar that fused
kernel is benchmarked and never called. The narrow fused unpack, on every set, Stream VByte and
the delta-of-delta reconstruct are benchmark-only as well: the writer never chooses delta of
delta, the block form that would reach the last.

**What SIMD bought.** On the M4, NEON over scalar, medians over widths: linear unpack
5.68x, two-pass delta 1.80x, fused delta
2.09x (not called on NEON), interleaved unpack 1.02x,
frame-of-reference add 1.06x, and Stream VByte, a codec the store never writes,
8.52x ([M4 kernels](results/bench/macOS-simd_kernel-ede0176b10bb.json)). On the Zen 1, AVX2 over scalar: interleaved
unpack 1.24x, two-pass delta 1.25x, fused
1.35x. A delta-of-delta reconstruct target is met at 11.09 GB/s,
by a kernel the store never calls.

On queries, the same Zen 1 binary at AVX2 against capped to scalar changes the warm engine median
by -2.3%, -3.6%, -4.3%, -3.7% and
-7.3% on Q1 to Q5, and the whole-day write runs at 1.43 IPC with
21.996 mispredicts per message and 2.4% of its CPU time in the
kernel ([counters run](results/diag/counters-simd-12302019-ede0176b10bb.json): summaries of profiler and counter runs, assembled by a script that refuses inconsistent recordings; no separate validator re-reads them).

**What SIMD cost.** 1,870 lines of kernels and dispatch, 6.6% of the
28,526 in `src/` and `include/`; a 875-line kernel benchmark and
551 lines of kernel tests.

**The store's price column.** Per piece, the reader inflates any zstd frame, allocates, decodes
PFOR with scale and patches, range-checks every value and appends. On the Zen 1 those steps take
11.6%, 20.4% (unpack), 25.3% (patches),
4.6% (scale and range check) and 0.6% (allocation and copy, read
from the profile's memory and containers groups only; a copy's addresses can land unresolved in the
C library, which holds 19.4%, so that share does not bound what copies cost) of the decode's user cycles ([price-decode profile](results/diag/price-decode-profile-12302019-ede0176b10bb.json): summaries of profiler and counter runs, assembled by a script that refuses inconsistent recordings; no separate validator re-reads them).

## Branch prediction and instruction counts

On the Zen 1, in user mode in the timed passes, Gorilla held in L1 runs
198.40 instructions per price at 3.27 per cycle, with
0.427 mispredicts per value; the cycles a mispredict costs were not measured. On
the whole column it takes 73.72 cycles, 218.11
instructions and 0.403 mispredicts per price, the store's price column
21.99, 47.00 and 0.051 at
2.14 IPC. Over prices and timestamps, Gorilla's mispredicts are
7.9 to 20.3 times the store's codecs' and its instructions
4.6 to 22.7 times. PFOR delta on timestamps runs at
1.17 IPC; with no memory counters, nothing here says what limits it
([Zen 1 column decodes](results/bench/Linux-column_decode-ede0176b10bb.json)).

## Memory

The writer holds a segment's messages, 8 bytes per column value, then
sorts and codes them: a peak footprint of 1,229 MB on the M4 at segments of
4,194,304 records, and at 1,048,576 a peak resident set, which counts
the mapped source, 900 MB smaller. The Zen 1's query worker peaks at
472.0 MB resident, mapped store pages included. QuestDB's day store,
22.90 GB, is larger than the Zen 1's 15.33 GiB of memory, so warm
queries run singly, and a timed query during which a page was swapped in is marked: for
tick-store, Q5 cold.

## Writes and queries against three systems

**Size.** tick-store's flushed store takes 1.76 GB on disk against the smallest
setup of each other system: Parquet 3.09 GB, ArcticDB
5.67 GB, QuestDB 22.90 GB
([Zen 1 comparison](results/compare/Linux-12302019-full-ede0176b10bb.json)).

**Writes.** tick-store parses the ITCH file inside its clock; the others spend
52% to 71% of their unflushed total reading
and converting the exported columns. Unflushed, each input where the last paragraph of the first section puts it, in seconds total
and engine-only: tick-store
211.7 and 211.3, Parquet 168.7 and
80.9, ArcticDB 111.5 and 35.1,
QuestDB 678.7 and 193.8. QuestDB's flushed write is
not like-for-like: its flush proof on the day reads NOT PROVEN: 1 data pages were still dirty when the commit returned.

**Where the writer's time goes.** Alone, parsing runs at 59.85 M messages a second on
the slice in memory, about 4.5 s of the engine-only write. Of the write's user cycles,
PFOR encoding takes 41.0%, dictionary encoding 6.8%, book
state 18.7%, segment assembly 19.1%, sorting
4.8%, parsing 2.7% and zstd 1.2%
([writer profile](results/diag/writer-profile-12302019-ede0176b10bb.json): summaries of profiler and counter runs, assembled by a script that refuses inconsistent recordings; no separate validator re-reads them).

**Queries.** Among the fixed setups, tick-store has the lowest median and p99 on Q2 and Q3,
warm and cold (met against each system's chosen setup, while ArcticDB with a table per stock is faster; swap-in: ArcticDB, QuestDB), and on Q5 cold (met; swap-in: tick-store, QuestDB);
the next fastest takes 283.77 ms on Q2 warm, against tick-store's
74.04 ms, and 64.95 ms on Q3, against
36.07 ms. ArcticDB with a table per stock takes 44.58
and 25.00 ms warm on them, and 10.2 s on Q1.
ArcticDB answers Q1 in 34.16 ms warm and
QuestDB Q4 in 1.24 ms; on Q5 warm tick-store's
912.75 ms against the fastest other's 54.46 ms leaves it
missed; swap-in: QuestDB.

## Tried and not kept

I tried and did not keep Gorilla for timestamps and prices, which lost to the integer codecs in size (Codecs); delta-of-delta
timestamps; references in file order and as distances in the day's orders; the comparison's
second segment size, which cost Q2 time and disk (Segment size); the fused delta kernel on NEON, and Stream
VByte. Not built: a stored per-stock minute aggregate (Q3), a sum over ranked dictionary counts
(Q5), checkpoints inside a run (Q1, Q4), and a second writer thread.

## Missed targets

On the Zen 1 unless the M4 is named, units as the bar's; "x" is how many times off the bar.

| Asked | Bar (source) | Measured | Gap | Verdict | Why |
|---|---|---|---|---|---|
| Timestamps, store as built | ≤ 20 bits (KX case study, NYSE quotes) | 22.819 bits; per stock* 25.488 bits | 2.82; 5.49 bits | missed in part | Not measured |
| Add prices, file order | ≤ 7.9 bits (ALP paper, stock doubles) | 15.254 bits | 7.35 bits | missed in part | Per stock met |
| Gorilla's bits over the integer codec's, file order | ≥ 1.53x (2025 paper, Indian stocks) | 1.237x to 1.511x | 0.019 to 0.293 | missed in part | Not measured |
| References, store as built | ≤ 11.2 bits (KX case study) | new orders 11.363 bits; imports 25.943 bits | 0.16; 14.74 bits | missed in part | Codec alone 7.797 |
| Shares | ≤ 3.8 bits (KX case study) | executions, trades, imbalances 3.907 to 7.758; store, adds 4.165 bits, reductions 7.284 bits | 0.11 to 3.96 bits | missed in part | Not measured |
| Gorilla decode, M4 | ≤ 2.12 ns (mailing-list post, smooth synthetic values) | 8.897 ns | 4.20x | missed | Not measured |
| Gorilla decode in L1 | ≤ 21.3 cycles (ALP paper, Intel Ice Lake) | 60.66 cycles | 2.85x | missed | 198.40 instructions a value |
| Delta decode, M4 | ≥ 3.6 G values/s (FastLanes, Apple M1) | 3.40 G values/s | 5.6% | missed | 7 of 65 widths reach it |
| Delta decode | ≤ 0.377 cycles (FastLanes, Zen 3 server) | 2.709 cycles | 7.18x | missed; a remote login during one of the 65 cases its median takes | Not measured |
| Price-column decode | ≥ 4,605 MB/s (ALP, an AMD EPYC 9554 core) | 1,301 MB/s; M4 3,367 MB/s | 3.54x; 26.9% | missed; M4 missed | Steps under Decode |
| Store against Gorilla decode, prices, M4 | ≥ 14x (third-party benchmark, Apple M2 Max) | 3.74x | 3.74x | missed in part | Steps under Decode |
| Single-thread ingest | ≥ 8.59 M records/s (QuestDB vendor figure, 32 workers) | 1.270 M records/s; M4 3.302 M records/s | 6.77x; 2.60x | missed; M4 missed | Parsing 2.1% of it |
| Ingest above Nasdaq's peak rate | > 2.732762 M messages/s (Nasdaq bandwidth report) | unflushed 1.270 M messages/s; flushed 1.410 M messages/s | 2.15x; 1.94x | missed; two remote logins during tick-store's write; replaced run 1.423 M messages/s | Not measured |
| Q2 warm median | ≤ 16.9 ms (DuckDB on a small Parquet store, preprint) | 74.04 ms; M4 25.28 ms | 4.38x; 8.38 ms | missed; M4 missed | 100 ns a row |
| Q3 warm median | ≤ 0.41 ms (QuestDB vendor benchmark) | 36.07 ms; M4 15.06 ms | 88x; 37x | missed; M4 missed | Not measured |
| Q4 cold mean | ≤ 10 ms (TimescaleDB, order-book preprint) | 356.09 ms; M4 136.16 ms | 36x; 14x | missed; M4 missed | 35.1 MB read cold |
| Lowest median and p99 of the fixed setups, each query | same machine | warm medians, Q1 296.78 against 34.16 ms; Q4 207.95 against 1.24; Q5 912.75 against 54.46 | 8.7x; 167x; 16.8x | missed in part; met on Q2 warm, Q2 cold, Q3 warm and Q3 cold against each system's chosen setup, while ArcticDB with a table per stock is faster; swap-in: ArcticDB, QuestDB, tick-store | Q5 unpacks 152,004,760 values, 6.0 ns each |

\* Leaves out the cost of the sequence that restores file order.

On the unflushed write, the target of writing no slower than each other system's fixed setup on
both clocks is missed; inputs not alike, by each write's record: tick-store's input is in memory before the clock starts, with 250,916,864 B read from the device inside the clock; Parquet's, ArcticDB's and QuestDB's are read from disk inside it, with 18,411,094,016 to 21,521,625,088 B read from the device inside the clock; engine-only, the clock less reading the input and putting it in the form the system's call takes, may still hold those reads, which are counted over the whole clock, so the two-clock rule does not take the difference out. On the flushed write, where tick-store took
190.7 s and QuestDB 1,595.6 s, it is met against QuestDB, not like-for-like; missed against Parquet; against ArcticDB met on this run and missed on the replaced one, a change for tick-store, counted as missed; two remote logins during tick-store's write; replaced run 188.9 s; a backup run and a remote login during QuestDB's write; replaced run 1,269.2 s; inputs not alike, by each write's record: tick-store's input is in memory before the clock starts, with 274,239,488 B read from the device inside the clock; Parquet's, ArcticDB's and QuestDB's are read from disk inside it, with 23,846,309,888 to 24,631,062,528 B read from the device inside the clock; engine-only, the clock less reading the input and putting it in the form the system's call takes, may still hold those reads, which are counted over the whole clock, so the two-clock rule does not take the difference out.
The [targets table](RESULTS.md#targets) gives both runs' figures; the cause of ArcticDB's move
between them was not measured and is not known.

## Not judged

With no published figure: the store's speed-up over Gorilla on the Zen 1
(not judged) and delta-of-delta decode with bit parsing (not judged on the M4,
not judged on the Zen 1); and, with no comparison day of every system on the M4, its comparison targets
(not judged, not judged) ([RESULTS.md](RESULTS.md#targets)).

## Commits and the fuzz result

**The fuzz oracle's work.** At the measured commit the fuzz run stopped on the harness's per-input time limit,
10 s, on a maximal input, so that run is no result. The oracle's work is
now bounded: fuzzing compares the SIMD kernels against the scalar ones on codec blocks of up to
1,048,576 values, a whole block of the format; every check runs on the blocks within an
input's first 262,144 values (dictionary) or its first maximal block (run-length, PFOR),
and a maximal block after them gets the kernel comparison and the sanitizers alone; the store
writes blocks of 4,096 values, inside every cap. Each host's fuzz run is
judged at the second commit: 13 of 13 targets ran clean on the M4 and
7 of 7 on the Zen 1, the shortest run 1,801 s
([fuzz results](results/fuzz-g3-8616263b7561.json)).

## How the results were checked

`scripts/check.sh` runs the builds, tests, sanitizers, fuzzing and results validators. A prover applies a fault to a throwaway clone and
requires the named step to fail; it lists 456 faults and a harmless control. Its
record ([prover record](results/check-proofs.json)) is of a run over 99 of the listed
faults, 99 of them proven, and its control passed. The final review recorded
193 findings, each fixed with a test seen failing first or closed with its reason
in `docs/REVIEW.md`.

## Limits

Performance comes from a pinned day on a single host, the Zen 1, whose 15.33 GiB of memory
is smaller than the other systems' 23.86 GB of input, and with a repetition count of 1 for
each day write; 31 events the host's logs record, a backup job and logins among
them, fell inside the campaign's timed phases and 10 inside the fuzz run's,
each named beside the verdict it touches, and the host's own scheduled jobs ran through every
phase and were not stopped. A fair protocol would
give every system its input resident before its clock, or none, and repeat every write. Sizes on
the M4 are file-system allocation, so sizes come from the Zen 1. Beyond boost, not measured and not
estimated: the cycles a mispredict costs and any memory-level counter.

<!-- generated by scripts/render_writeup.py from docs/writeup/WRITEUP.template.md and docs/writeup/figures.json: edit those, never this file -->
