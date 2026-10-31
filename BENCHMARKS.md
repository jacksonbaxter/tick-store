# tick-store benchmarks and targets

Status: Phase 1. Nothing here has been measured on tick-store yet: there is no code. Every number below is a published figure, or
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
| CR-1 | Whole store ratio beats the pinned file's own `.gz`: at least 2.3415 on 12302019 | 8,251,407,909 decompressed / 3,524,013,057 `.gz` = 2.34148, so 2.3415 is the floor rounded up to exceed it. Second day, 10302019: 2.333, from 9,035,016,843 / 3,872,931,242. Derived | Sizes measured from the downloaded file, whose gzip trailer agrees; https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ | That file. gzip level not stated. Sibling days: 2.314 to 2.341 | **Direct**. A floor, not an ambition. The only whole-file ratio published on ITCH | 12302019 downloaded and measured 2026-10-30. 10302019: `.gz` size confirmed by HEAD, file not yet measured |
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
