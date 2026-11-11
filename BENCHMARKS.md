# tick-store benchmarks and targets

Status: Phase 3. The decode targets, DS-1 to DS-9, are measured under `measure.sh` at `bcbf873`, with verdicts under "Measured decode results": DS-6 is met on timestamps and missed on prices, DS-8 is met, DS-1, DS-2, DS-3 and DS-5 are missed, DS-7 and DS-9 are reported with no target figure, and DS-4 has no reported figure because the Linux kernel file was refused five times by the sibling rule, twice at `bcbf873` and three times at `b053aff`. The other targets' verdicts are under "Measured results for the other targets", on Linux from the full day at `67ae574` and on macOS from a full day of tick-store alone. Of the targets judged on the measured hosts, CR-1, CR-2, G-1, G-2, G-3, G-4, DS-8 and WT-2 on macOS are met. WT-1, QL-1, QL-2 and QL-3 are missed on both hosts, and WT-2 is missed on Linux. CR-3 to CR-7, DS-6, WT-3, WT-4 and QL-4 are met on some of their cases and missed on others. G-3 is met from 30-minute runs of every fuzz target at `ea1dde4`, whose store code is unchanged since `67ae574`. WT-3 and QL-4 are not judged on macOS. Every source figure below is a published figure, or
arithmetic on one, used as a target or as context. The exceptions are the file facts in CR-1 and Data, which come from the downloaded
files. A research subagent opened each source, and the `Checked` column records that the project session then opened it too and
recomputed any derived figure from the raw values.

Full evidence, with quotes and locations, is in `research/evidence/`.

## How to read this file

- **Direct**: measured on Nasdaq ITCH data, or a fact about it.
- **Context**: measured on other hardware or another workload. It shows what good looks like and proves nothing about this
  target. Almost every target here is context, because no published source measures columnar storage of ITCH.
- **Relative**: the target is to beat a comparison system on the same harness, so the number comes from our own run.
- **UNCONFIRMED**: no source supports a number. The row says where we looked. No figure has been invented to fill the gap.
- **Derived**: arithmetic on published figures, shown in the row. The source does not state the result.
- A missed target stays in this file, with the measured number, the gap and the reason. Targets are not lowered to fit.

## Hosts

Results from the two hosts are kept apart and never mixed.

| Label | Machine | Measurement limits |
|---|---|---|
| macOS | Apple M4 Max, 12 P and 4 E cores, 64 GiB, macOS 27.2, APFS on internal SSD | No pinning. 24 MHz timer, about 42 ns per tick. Results are unpinned and have no hardware counters. NEON only: no SVE, no bit-deposit instructions. Run under the machine's measurement lock (`measure.sh`). |
| Linux | `debian`: AMD Ryzen Embedded V1605B (Zen 1), 4 cores and 8 threads, 2.0 GHz base, 4 MB L3, 15 GB RAM, Debian 13, kernel 6.12 | Pin with `taskset -c 4`, `cpu5` idle. TSC resolution about 10 ns. `perf` `:u` events only. AVX2 runs as two 128-bit halves; PEXT and PDEP are microcoded. Run under `~/quant-projects/bin/measure.sh`. Headline pinned numbers come from here. |

## Definitions

- **Record**: one ITCH message of the source file.
- **Source bytes**: the decompressed ITCH stream, including the 2-byte length prefix on every message. The `.gz` is not the baseline.
- **Data**: the reference day is `12302019.NASDAQ_ITCH50.gz`, 3,524,013,057 bytes compressed, 8,251,407,909 decompressed, 268,744,780
  messages. The second day is `10302019.NASDAQ_ITCH50.gz`, 3,872,931,242 bytes compressed, 9,035,016,843 decompressed, 293,989,079
  messages. Both days' figures are measured from the files. The SHA-256 pins are in `scripts/data_pins.json`.
- **Ratio**: source bytes divided by stored bytes. Stored bytes include everything needed to re-serialise the source exactly:
  indexes, permutations, exceptions and metadata. Every system is measured against the same baseline.
- **Durability class A**: returns once data is in the OS page cache. **Class B**: returns after a flush to the device.
  Classes are never compared with each other. See `research/evidence/column-systems.md` section 2.4.
- **Warm**: the files a query reads are proven resident, not assumed. **Cold**: proven evicted. See `TASKS.md` for the methods.
- **Query set**: Q1 one second of all symbols at the open; Q2 one hour of one symbol; Q3 per-minute aggregate of one symbol over
  one hour; Q4 one millisecond of all symbols; Q5 full-day scan of one column. Rows returned and bytes decoded are reported with
  each latency.

## Correctness gates

These come from the project brief, so they need no published source. Each one must pass before any speed number counts.

| ID | Gate | Check |
|---|---|---|
| G-1 | Every record of the full pinned day round-trips bit-exact | Re-serialise every record, compare bytes with the source across the whole day, and also compare the SHA-256 of the whole stream |
| G-2 | The check catches corruption | An injected single-bit corruption is detected, with the location reported |
| G-3 | Every parser and decoder is fuzzed | Each libFuzzer target runs clean for 30 minutes under the heavy-job lock (`heavy.sh`) |
| G-4 | Gorilla is built and measured | Delta-of-delta timestamps and XOR floats exist and run on this data, whatever the result |

## Compression targets

Ratio does not depend on the host.

| ID | Target | Source value | Source | Measured on | Fit | Checked |
|---|---|---|---|---|---|---|
| CR-1 | Whole store ratio beats the pinned file's own `.gz`: at least 2.3415 on 12302019 | 8,251,407,909 decompressed / 3,524,013,057 `.gz` = 2.34148, so 2.3415 is the floor rounded up to exceed it. Second day, 10302019: 2.333, from 9,035,016,843 / 3,872,931,242. Derived | Sizes measured from the downloaded file, whose gzip trailer agrees; https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ | That file. gzip level not stated. Sibling days: 2.314 to 2.341 | **Direct**. A floor, not an ambition. The only whole-file ratio published on ITCH | 12302019 downloaded and measured 2026-10-30. 10302019 downloaded and measured 2026-10-30 |
| CR-2 | Whole store is smaller than Parquet, ArcticDB and QuestDB, each tuned per its docs | None | Our harness | Same records, same machine | **Relative**. No published ratio exists on ITCH for any of them | n/a |
| CR-3 | Timestamp column at most 20.0 bits per value | `Time` 31.3% of an 8-byte timespan at zstd 1. 20.0 bits is derived | KX, https://code.kx.com/q/kb/compression/fsicasestudy/ | NYSE TAQ `quote` table, one day in 2022, 1.78 B rows, parted by symbol, 128 KB blocks. Vendor | Context. Real US equity timestamps under a generic codec, so a dedicated codec should beat it. ITCH file order differs from per-symbol order | opened 2026-10-30 |
| CR-4 | Add-order price column at most 7.9 bits per value | ALP 7.9 bits/value on Stocks-USA, against a 64-bit baseline | ALP, Table 4, PDF p. 9, https://ir.cwi.nl/pub/33334/33334.pdf | INFORE stock prices as doubles, 282 M values, 2 decimal places. Ratios are independent of hardware | Context. Closest published data to tick prices, but not ITCH and not integers | opened 2026-10-30 |
| CR-5 | Best integer scheme at least 1.53x smaller than Gorilla XOR on the same price column | 1.82x vs 1.19x. 1.53 is derived | BOS, ICDE 2025, Fig. 10(a), printed p. 1384, https://sxsong.github.io/doc/25ICDE-BOS.pdf | Nifty-Stocks, same harness, granularity unknown | Context. The closest XOR vs integer result on stock prices. If integer loses, that is a finding | opened 2026-10-30 |
| CR-6 | Order reference columns at most 11.2 bits per value | `Sequence_Number` 35.1% of a 4-byte `int` at zstd 14. 11.2 bits is derived | KX, same page | Same table. Its order-0 entropy is 26.2 bits | Context. A sequence number, not an order reference, and stated in bits because the columns differ in width: ITCH references are 8 bytes. They are the largest byte class in ITCH, 28 to 32% | opened 2026-10-30 |
| CR-7 | Shares columns at most 3.8 bits per value | `Offer_Size` 11.8% of a 4-byte `int` at zstd 22. 3.8 bits is derived | KX, same page | Same table | Context | opened 2026-10-30 |

Gorilla XOR on ITCH prices is measured and reported beside CR-4 and CR-5. For scale, published Gorilla on stock doubles is 37.7
bits/value (ALP Table 4, Stocks-USA), and the original 1.37 bytes per point is on regular monitoring data (see Reference numbers).

## Decode targets

| ID | Target | Host | Source value | Source | Measured on | Fit | Checked |
|---|---|---|---|---|---|---|---|
| DS-1 | Sequential Gorilla XOR decode of doubles at most 2.12 ns/value | macOS | 2.12 ns/value, batched scalar, format unchanged | TsFile dev list, http://www.mail-archive.com/dev@tsfile.apache.org/msg00679.html | "ARM64 macOS", chip not stated, 500,000 smooth synthetic values, single stream. Not peer reviewed | Context. ITCH prices will mispredict more | opened 2026-10-30 |
| DS-2 | Sequential Gorilla decode at most 21.3 cycles/value, from `perf` `cycles:u` | Linux | 0.047 tuples/cycle. 21.3 is derived | ALP, Table 5, PDF p. 10, https://ir.cwi.nl/pub/33334/33334.pdf | Ice Lake 8375C 3.5 GHz, all data sets, L1 resident | Context. A newer core; a gap on Zen 1 is expected and will be stated | opened 2026-10-30 |
| DS-3 | 64-bit block delta decode, unpack plus prefix sum, at least 3.6 G values/s | macOS | 0.335 + 0.551 = 0.886 cycles/value at 3.2 GHz. Derived | FastLanes raw CSVs, github.com/cwida/FastLanes commit f0edc10, `publications/data_parallelized_encodings/fls_pub/results/m1/` | Apple M1 NEON, T=64, L1 resident, median across widths. Two passes, not one kernel | Context. M1, not M4 Max. Our kernel benchmark must match the L1-resident setup | opened 2026-10-30 |
| DS-4 | Same kernel at most 0.377 cycles/value on AVX2 | Linux | 0.191 + 0.186 = 0.377. Derived | Same commit, `results/zen3/` | Zen 3 EPYC 7R13 AVX2, T=64, L1 resident | Context. Zen 1 splits 256-bit operations, so a miss is expected. Keep the target and state the gap | opened 2026-10-30 |
| DS-5 | Price block decode at least 4,605 MB/s of 8-byte values, on one core | macOS, Linux | ALP 4,605 MB/s on Stocks-USA | VIREL, Table I, p. 9, https://arxiv.org/pdf/2607.22433 | AMD EPYC 9554, one core. Preprint | Context. A real-data decode, unlike DS-3. Zen 1 will fall short | opened 2026-10-30 |
| DS-6 | SIMD layout decodes the same column at least 14x faster than the sequential Gorilla stream | macOS | 5.920 / 0.423 µs per 1,000 values = 14x. Derived | apache/datafusion discussion 24935, https://github.com/apache/datafusion/discussions/24935 | Apple M2 Max, third-party Rust benchmark by the fastalp author, three data sets | Context. Vendor-style, and ALP against Gorilla rather than our layout | opened 2026-10-30 |
| DS-7 | SIMD layout speedup over the Gorilla stream | Linux | None | **UNCONFIRMED** | Looked at ALP Table 5 and Fig. 4, FastLanes raw data, Arrow PRs. The 55x in ALP Table 5 is AVX-512 on Ice Lake, which this host lacks; nothing covers AVX2 on Zen 1 | Report the measured figure and the ISA | n/a |
| DS-8 | Delta-of-delta reconstruct kernel at least 8.20 GB/s of 32-bit values, with no bit parsing | macOS | 8.20 GB/s pipelined; 3.65 GB/s naive | Ashton Six, https://github.com/ashtonsix/perf-portfolio/tree/main/delta, README Results | Neoverse V2, NEON, 4 KiB, single thread, L1 hot. One unreviewed author | Context. The only delta-of-delta kernel figure found | opened 2026-10-30 |
| DS-9 | Delta-of-delta decode including bit parsing | Both | None | **UNCONFIRMED** | Looked at Gorilla, Chimp, Elf, ALP, FastLanes, Sprintz Fig. 6 (plot only), TurboPFor, Beringei. Gorilla states no per-core rate | Report the measured figure | n/a |

### How each decode target is measured

Written before the first reported run. Two binaries hold every case, each run through `scripts/run_bench.py` under the
host's `measure.sh`, one process per case (README.md, "Benchmarks"). `bench/simd_kernel_bench` times kernels on seeded
input, one vector of 1,024 values in L1, as FastLanes and Ashton Six measure. `bench/column_decode_bench` builds two
columns of real data from the pinned slice `12302019-open` and times their decode, after the slice matches its pin and
each decode gives its column back bit for bit. The price column is the store's own add-order price column, 3,153,991
values grouped by (locate, side) as the store's segments keep them; the timestamp column is the slice's 6,125,294
timestamps in file order. Every rate is the median of ten timed passes.

| ID | Cases | Figure | Host |
|---|---|---|---|
| DS-1 | `gorilla_xor/add_price`: the price column as dollar doubles, price / 10^4, one Gorilla XOR stream in blocks of 4,096 | ns a value, 10^9 over the median values a second | macOS |
| DS-2 | `gorilla_xor_l1/add_price`: the same coding, as many 1,024-value vectors evenly spaced through the column as fit 16 KiB encoded, one block each, decoded in turn, so input and output stay in Zen 1's 32 KiB L1 | `cycles:u` over values decoded, from the same timed passes | Linux |
| DS-3 | `pfor_delta_two_pass/w=0..64/neon`, unpack then inverse delta, the form the NEON set uses; `pfor_delta_fused/w=0..64/neon` beside it | G values a second, the median across the 65 widths, FastLanes' own statistic | macOS |
| DS-4 | `pfor_delta_fused/w=0..64/avx2`, the form the AVX2 set uses; `pfor_delta_two_pass/w=0..64/avx2` beside it | `cycles:u` a value, the median across the 65 widths | Linux |
| DS-5 | `store_decode/add_price`: the store's chunks of the price column from `encode_column`, PFOR delta with an adaptive scale and zstd where smaller, decoded by `decode_column` piece by piece as the reader does | MB/s of 8-byte values, the median values a second times 8 | both |
| DS-6 | Timestamps: `gorilla_dod/timestamp` against `pfor_delta/timestamp`, the same values. Prices: `gorilla_xor/add_price` against `store_decode/add_price`, the same values | Gorilla's ns a value over the layout's, from one results file. Judged on each column | macOS |
| DS-7 | As DS-6 | As DS-6, with the instruction set | Linux |
| DS-8 | `dod_reconstruct/scalar`: `delta_of_delta_decode`, the two running sums PFOR runs after unpacking, on 1,024 values, no bit parsing. Each call restores its input from a copy, and `restore_copy/scalar` times that copy alone | GB/s counted as 32-bit values, values a second times 4, since the source counts 32-bit values and ours are 64-bit lanes | macOS |
| DS-9 | `gorilla_dod/timestamp`, Gorilla's bit parsing; `pfor_dod/timestamp`, delta-of-delta on the vector layout | ns a value and values a second, with the instruction set | both |

### Measured decode results

At `bcbf873`, the cases above through `scripts/run_bench.py` under each host's `measure.sh`:
`TICK_STORE_MEASURE=<measure.sh> python3 -I scripts/run_bench.py build/bench/bench/column_decode_bench`, and the same for
`simd_kernel_bench`. macOS is the M4 Max, NEON, unpinned, no counters, at load 1.26 to 3.92 at the start of a case. Linux is Zen 1,
AVX2, pinned to cpu4 with `cycles:u`, at load 1.27 to 1.49 at the start of a case, cpu5 at most 1.12% busy. Results: `results/bench/macOS-column_decode-bcbf8738d892.json`,
`results/bench/macOS-simd_kernel-bcbf8738d892.json`, `results/bench/Linux-column_decode-bcbf8738d892.json`. The Linux kernel file
does not exist: two complete runs were refused for cpu5 over 5% busy in 4 and in 7 of 314 cases (NOTES.md, "Decode targets under
measure.sh"), and three more at `b053aff` in 9, 7 and 13 (NOTES.md, "DS-4: three more refusals"). Gaps and reasons are there too.

| ID | Measured | Host, instruction set | Verdict |
|---|---|---|---|
| DS-1 | 8.393 ns a value | macOS, NEON build (Gorilla is scalar code) | Missed: 3.96 times the 2.12 ns |
| DS-2 | 60.91 cycles a value | Linux, AVX2 build (Gorilla is scalar code) | Missed: 2.86 times the 21.3 cycles |
| DS-3 | 3.35 G values/s, two passes, median of 65 widths | macOS, NEON | Missed: 7.0% under 3.6 |
| DS-4 | No reported figure. The five refused runs read 2.714 to 2.720 cycles a value, fused | Linux, AVX2 | Not judged. The refused runs are 7.2 times the 0.377 cycles |
| DS-5 | 3,674 MB/s on macOS, 1,280 MB/s on Linux | macOS NEON, Linux AVX2 | Missed on both: 20.2% under 4,605, and 3.6 times under |
| DS-6 | Timestamps 16.06 times; prices 3.86 times | macOS, NEON | Met on timestamps, missed on prices |
| DS-7 | Timestamps 7.58 times; prices 3.29 times | Linux, AVX2 | No published figure |
| DS-8 | 11.77 GB/s counted as 32-bit values, 2.94 G values/s | macOS, scalar code in every set | Met: 1.44 times the 8.20 GB/s |
| DS-9 | Gorilla 9.650 ns a value on macOS, 22.204 ns on Linux; PFOR delta-of-delta 0.880 and 3.151 ns | macOS NEON, Linux AVX2 | No published figure |

## Write targets

Every write number states its durability class and flush granularity.

| ID | Target | Host | Source value | Source | Measured on | Fit | Checked |
|---|---|---|---|---|---|---|---|
| WT-1 | Full-day ingest, class A, one thread, at least 8.59 M records/s | Both | QuestDB 8.59 M rows/s at 100,000 hosts | https://questdb.com/blog/clickhouse-vs-questdb-comparison/ ingestion table | Vendor. r8a.8xlarge, 32 vCPU, 32 workers, TSBS `cpu-only`, uncompressed hot tier, default `nosync` | Context. Theirs is 32 workers on a server with no compression. Ours is one thread with compression, so Linux may fall short | opened 2026-10-30 |
| WT-2 | Sustained ingest above 2,732,762 messages/s, the highest TotalView rate published | Both | `1ms_mps` 2,732,762, Dec 2024, row `Nasdaq TotalView-ITCH (FPGA)`. The plain `Nasdaq TotalView` row is 2,547,190 for Nov and 2,439,000 for Dec | Nasdaq bandwidth report, sheet `BandwidthDec24`, https://web.archive.org/web/20250123185610/https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/bandwidthreport.xls | Nasdaq live feed. The file calls these figures an average trading day in continuous trading, warns the open can run significantly higher, and does not define the columns | **Direct** for the domain. A floor, not a true peak | opened 2026-10-30 |
| WT-3 | Class A ingest at least as fast as each comparison system | Both | None | Our harness | Rows visible to queries, not rows sent | **Relative** | n/a |
| WT-4 | Class B ingest at least as fast as each comparison system, at stated flush granularity | Linux | None | **UNCONFIRMED** for any absolute figure | Looked at QuestDB docs and blogs: no throughput under `sync`. ArcticDB: no ingest figure. Arrow: no fsync in the writer | **Relative**. macOS class B is not like-for-like, because LMDB uses `F_FULLFSYNC` and QuestDB plain `fsync` | n/a |

## Query targets

Warm and cold are reported separately, with how cold was achieved and proven.

| ID | Target | Host | Source value | Source | Measured on | Fit | Checked |
|---|---|---|---|---|---|---|---|
| QL-1 | Q2 warm median at most 16.9 ms | Both | 0.0169 s, hour slice, single ticker | tse_tick, Table 6, PDF p. 10, https://arxiv.org/abs/2608.23053 | DuckDB on Hive-partitioned Parquet, i5-14400F, 1.5 M-row store, 52 rows returned. Preprint | Context. Far smaller store | opened 2026-10-30 |
| QL-2 | Q3 warm median at most 0.41 ms | Both | 0.41 ms, `single-groupby-1-1-1` | https://questdb.com/blog/clickhouse-vs-questdb-comparison/ query table | Vendor. r8a.8xlarge, TSBS, two days of 4,000 hosts, median of 3, warm, `--workers=1` | Context. The closest published shape to a filtered short-range aggregate | opened 2026-10-30 |
| QL-3 | Q4 cold mean at most 10 ms | Both | 10 ms, TimescaleDB, `O-T`: top of the book at a random time. kdb+ on-disk 11, InfluxDB 124, ClickHouse 706. kdb+ in-memory 5 is not a cold read | Barez et al., Table 6, PDF p. 23, https://arxiv.org/pdf/2301.12561 | i7-1065G7 PC, 16 GB, SSD, 33 M order-book records, mean of 10 executions, standby list cache cleared before each. Versions not stated. Preprint | Context. The only published cold-cache point lookup on order-book data | opened 2026-10-30 |
| QL-4 | Lowest median and p99 of the four systems on every query, warm and cold | Both | None | Our harness | Same queries, same machine, same run conditions | **Relative** | n/a |

## Measured results for the other targets

The README's Targets block is generated from `scripts/targets.json` and the committed results
files, and `scripts/check.sh` fails if it goes stale. It holds every figure below. The decode
targets are under "Measured decode results". Linux figures are pinned to cpu4, from
`results/compare/Linux-12302019-full-67ae574f5666.json`, at `67ae574`. macOS figures are
unpinned, from `results/compare/macOS-12302019-full-tickstore-2503d1e7c5f6.json`, a full day of
tick-store alone at `2503d1e`, with store code unchanged since `67ae574`. The CR figures come
from `results/codec-eval-12302019.json` and `results/store-size-12302019.json`.

| ID | Host | Measured | Verdict |
|---|---|---|---|
| G-1 | Any | Bit-exact on 12302019 on both hosts and 10302019 on macOS, both stream hashes equal | Met |
| G-2 | Any | A flipped bit is caught in a data block and in the footer, with CRCs on and off | Met |
| G-3 | Any | All 13 targets clean for at least 1,801 s on macOS (NEON), and the 6 whose decode reaches the SIMD kernels clean for at least 1,801 s on Linux at AVX2, each comparing its kernel set with the scalar set inside the run, at `ea1dde4` (`results/fuzz-g3-ea1dde4c02e4.json`) | Met |
| G-4 | Any | Gorilla delta-of-delta and XOR built, exact, and measured | Met |
| CR-1 | Any | 4.6930 | Met |
| CR-2 | Linux | 1,758,240,768 bytes, against Parquet's smallest 3,191,406,592, ArcticDB's 5,666,115,584, QuestDB's 22,897,786,880 | Met |
| CR-3 | Any | 18.76 bits in file order, 25.49 per locate, 22.82 in the store as built | Met in file order only; missed per locate and by the store |
| CR-4 | Any | 15.25 bits in file order, 6.26 per locate, 4.61 in the store | Met per locate and by the store; missed in file order |
| CR-5 | Any | Over Gorilla XOR on dollars 2.99 to 5.98 times; over the best Gorilla 1.24 to 2.12 | Met over XOR on dollars everywhere; over the best Gorilla, met per locate only |
| CR-6 | Any | 6.11 to 9.22 bits in the estimate; in the store, `creation_ref` 11.36, `ref_distance` 7.56, `import_ref` 25.94 | Met in the estimate; the store misses on `creation_ref` and `import_ref` |
| CR-7 | Any | Adds 2.61 to 3.69 bits and `U` 2.57 in the estimate, `E`, `C`, `X`, `P`, `Q`, `I` 3.91 to 7.76; in the store, `add_shares` 4.17, `reduce_shares` 7.28, `replace_shares` 2.90 | Met for adds and replaces in the estimate; the store meets on `replace_shares` only |
| WT-1 | Linux | 1.376 M records/s, class A | Missed, 16.0% of 8.59 M |
| WT-1 | macOS | 3.215 M records/s, class A | Missed, 37.4% of 8.59 M |
| WT-2 | Linux | 1.376 M messages/s class A, 1.423 M class B | Missed |
| WT-2 | macOS | 3.215 M messages/s class A, 3.208 M class B | Met, 17.6% and 17.4% over 2,732,762 |
| WT-3 | Linux | 195.3 s, against Parquet 158.4, ArcticDB 120.1, QuestDB 273.6 | Met against QuestDB only |
| WT-3 | macOS | No four-system day: it runs on Linux only | Not judged |
| WT-4 | Linux | 188.9 s, against Parquet 266.3, ArcticDB 123.6, QuestDB 1,269.2 (not like-for-like) | Met against Parquet and QuestDB; missed against ArcticDB |
| QL-1 | Linux | Q2 warm median 74.32 ms | Missed, 4.4 times 16.9 ms |
| QL-1 | macOS | Q2 warm median 25.79 ms | Missed, 1.53 times 16.9 ms |
| QL-2 | Linux | Q3 warm median 35.81 ms | Missed against 0.41 ms |
| QL-2 | macOS | Q3 warm median 14.69 ms | Missed against 0.41 ms |
| QL-3 | Linux | Q4 cold mean 308.83 ms | Missed against 10 ms |
| QL-3 | macOS | Q4 cold mean 133.63 ms | Missed against 10 ms |
| QL-4 | Linux | Lowest median and p99 on Q2 warm and cold, Q3 warm and cold, Q5 cold | Met on those five; missed on Q1 and Q4 warm and cold and on Q5 warm |
| QL-4 | macOS | No four-system day: it runs on Linux only | Not judged |

## Reference numbers

These are context only. They are recorded so the write-up can cite them, and are not targets. A research subagent opened each
source. The project session also opened the TritanDB, KX, ALP, VIREL and datafusion rows, but not Gorilla, Lemire, Stream VByte,
Agner Fog or TSM-Bench, since no target rests on them.

| Value | Counted | Source | Measured on |
|---|---|---|---|
| 23.2 bits per timestamp, derived from 35.9 MB over 12.4 M rows | Delta-of-delta at nanosecond resolution; 5.2 bits at milliseconds | TritanDB, Table 5, https://arxiv.org/pdf/1801.07947 | Shelburne data, regular with jitter. Preprint |
| Bid price 3.6 to 4.9 bits/value, from 5.6 to 7.6% of an 8-byte `float`. Trade price 4.9 to 6.5, from 15.2 to 20.2% of a 4-byte `real`. Derived | zstd 22 down to zstd 1, generic codec | KX, https://code.kx.com/q/kb/compression/fsicasestudy/ | NYSE TAQ, parted by symbol. Bid prices repeat heavily, unlike add orders, so these bracket CR-4 rather than replace it |
| 1.37 bytes per point, 12x | Timestamp plus value, against 16 raw bytes | Gorilla, Sec. 2.1, https://www.vldb.org/pvldb/vol8/p1816-teller.pdf | Facebook monitoring data, fixed intervals |
| 96.39% of timestamps in 1 bit | Delta-of-delta zero case | Gorilla, Fig. 3 | 440,000 regular timestamps |
| Gorilla 37.7, Chimp128 12.2, Elf 8.8, Zstd 7.8 bits/value | Stocks-USA, 64-bit baseline | ALP, Table 4 | INFORE stock doubles |
| Gorilla decode 766 MB/s, ALP 4,605 MB/s | One core, Stocks-USA | VIREL, Table I | EPYC 9554. Preprint |
| 5.92 ns/value Gorilla decode | Average of three data sets | apache/datafusion discussion 24935 | M2 Max, third party |
| 3.9 scalar, 3.6 naive, 8.9 fast billion values/s | 32-bit prefix sum, 1,000 values all 1 | https://lemire.me/blog/2026/03/08/prefix-sums-at-tens-of-gigabytes-per-second-with-arm-neon/ | Apple M4 4.5 GHz, NEON |
| 39.6 / 32.0 / 36.2 GB/s | Stream VByte decode, random 32-bit, no delta | https://fast-pack.github.io/streamvbyte/ | Apple M4 Max, NEON |
| PEXT latency 18, reciprocal throughput 18 | Cycles, Zen 1 | Agner Fog instruction tables, p. 90, https://www.agner.org/optimize/instruction_tables.pdf | Ryzen 7 1800X |
| 18.4 M and 2.9 M points/s | ClickHouse and QuestDB 6.2.1 bulk load | TSM-Bench, Table 3, https://www.vldb.org/pvldb/vol16/p3363-khelifati.pdf | Xeon E5-2620 v4, HDD, synthetic. Peer reviewed |

## Code-quality bar

Drawn from reading Beringei, Prometheus chunkenc, ClickHouse, DuckDB, cwida/ALP, streamvbyte, simdcomp, FastPFor, Arrow, Highway,
ArcticDB and QuestDB, with file and line evidence pinned to commits. No single project read meets every item, so the bar is
stricter than any of them. A reviewer fails a change if an item is unmet. The full list, with the check for each item and the
code it traces to, is `research/evidence/code-quality.md` section 16.

- **Decoding (D1-D11)**: one bounds-checked reader; typed errors carrying field, offset, expected and actual; every size checked
  before use; XOR window validated before any shift; unsigned delta arithmetic; widths 0 and 64 explicit in every backend; size
  arithmetic that cannot overflow; allocations bounded before trusting a count; exact count or an error; truncation at every byte
  length raises; unknown ids and widths rejected.
- **On-disk format (F1-F7)**: fixed header with magic, version and size; explicit little-endian; per-block CRC32C verified by
  default; one codec registry with retired ids reserved; old files stay readable; golden bytes both ways; incomplete files detected.
- **API (A1-A3)**: spans and `[[nodiscard]]` results; a written over-read contract; view lifetimes tied to the mapping.
- **SIMD (S1-S7)**: forcible scalar path; one shared prolog, kernel and tail driver; exact-size buffers under ASan; every width,
  offset and tail per kernel; a direct scalar-vs-kernel `memcmp`; no silent skips; no typed unaligned loads.
- **Tests (T1-T4)**: one corrupt fixture per validated field; bit-exact float comparison; boundary lists at every bucket edge;
  encoded size pinned.
- **Fuzzing (Z1-Z7)**: raw bytes reach decoders below the checksum; decode, re-encode, compare oracle; memory capped; seeds from the
  real encoder; reach measured; crashers become tests; at least 4 minutes per target on each change.
- **CI (C1-C2)**: ASan and UBSan with no recover on all tests; TSan on concurrent tests; warnings as errors; every matrix axis
  changes the build.
- **Mmap and durability (M1-M9)**: data durable before the commit point; atomic metadata replace; sync failure is fatal;
  `F_FULLFSYNC` on macOS; mapped length proved at open; torn-write tests; a written SIGBUS stance; named durability modes; `munmap`
  checked with the mapped length.
- **Benchmarks (B1-B6)**: seeded stable inputs; verify before measuring; elision prevented; ratio plus counters from the same run,
  with unavailable counters marked; production path measured; predictor not trained on one input.
