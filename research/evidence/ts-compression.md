# Evidence: time-series timestamp and float compression

Status: COMPLETE for Phase 1, research only. Every number below is confirmed in the primary text
(cached copy under CACHE, the research cache kept outside this repo,
or a source opened in this task). Unconfirmed items go to "Not found", never into the tables.

Sections: Short answers | Peer-reviewed (P) | Not peer-reviewed: preprints and a thesis (NP) | Vendor / project (V) | Contradictions | Not found
Entry numbers have gaps because entries were reclassified; numbers are labels, not a ranking.

---

# SHORT ANSWERS (each line points to the entry that holds the quote and location)

Facts first, then what they do not settle. Lines marked "Inference" are mine and are not published results.

(a) Gorilla: 1.37 bytes per (timestamp, value) pair against 16 raw (G1). 96.39% of timestamps took 1 bit on a 440,000-sample of fixed-interval monitoring data (G2). The paper states no encode or decode rate (P1).

(b) Successors on stock doubles, bits/value, 64-bit baseline, ALP Table 4, Stocks-USA: Gorilla 37.7, Chimp 35.0, Chimp128 12.2, Patas 19.2, PDE 26.1, Elf 8.8, ALP 7.9, Zstd 7.8 (P4). Decode on one core, VIREL Table I, Stocks-USA: ALP 4,605 MB/s, Gorilla 766 MB/s, Chimp 544 MB/s, Elf 559 MB/s (NP1, preprint).

(c) Key question, does XOR suffer on decimal-origin values: yes, in every source opened that compares them.
- Same stock data, same harness: Gorilla 1.19x vs scaled integer TS2DIFF+BP 1.82x, SPRINTZ+BOS 2.07x (P11, Nifty-Stocks).
- Same stock doubles, Gorilla is 4.8x larger than ALP on Stocks-USA (37.7 vs 7.9, derived, P4).
- Qualitative agreement: BtrBlocks "pricing data columns" (P8), BUFF Sec. 4.4.1 (P5), ALP (P4).
- Against the simple story: XOR is still better on integers than on doubles (IoTDB, P6), and Gorilla beats TS2DIFF+BP on one integer set, TC, 6.39 vs 4.00 (P11). Why it wins on TC was not established.
- Not settled: no source tests tick-level exchange prices stored as fixed-point integers against XOR. P11 is the closest, on a Nifty-50 data set of unknown granularity.
- Inference: ITCH prices are already integers, so ALP's decimal-to-integer step and PDE have nothing to recover there. What remains from this literature is the integer path: delta or frame of reference, then bit-packing, with GCD as an option (V2). This needs measuring on ITCH.

(d) Timestamps: no published figure exists for irregular ns exchange timestamps.
- TritanDB is the only same-data ms-vs-ns measurement: Shelburne DoD 8.0 MB at ms, 35.9 MB at ns, ~5.2 and ~23.2 bits/ts derived (NP5). That data is regular with jitter, so ITCH is expected to be harder. Inference.
- On the same paper's uneven data, Delta-RLE-Rice (1.5 MB) beat DoD (4.0 MB) (NP5, GreenTaxi, seconds resolution); at ns on Shelburne DoD beat it, 35.9 vs 44.1 MB.
- Gorilla's 96.39% one-bit share depends on fixed intervals (P1). ClickHouse and Tiger state the same dependence (V2, V3).
- Prometheus widened the buckets for ms (V1). BTrDB used a windowed mean of deltas because "nanosecond timestamps produce very large deltas" (P14).
- Inference: Gorilla's and Prometheus's bucket widths should not be reused for ns data without measurement.

(e) Implementations: see V1 to V6. Two production systems convert floats to scaled integers before coding (M3 V6, VictoriaMetrics V5).


# PEER-REVIEWED

Page convention: "PDF p.N" = Nth page of the file; "printed p.N" = number in the footer.
CACHE-D below = CACHE/downloads
"Derived" = my arithmetic on stated figures, marked as such; never a figure the source itself states.

## P1. Gorilla (Pelkonen et al., PVLDB 8(12), 2015)
URL: https://www.vldb.org/pvldb/vol8/p1816-teller.pdf (cached: CACHE-D/gorilla/p1816-teller.pdf). Rendered pp.3-6 read.
Common fields for all rows. Hardware: not stated per-core (only "20 machines" / "80 machines", no CPU, clock, memory per host, storage).
Data set: Facebook ODS production monitoring data. Thread count: not stated. Software version: not stated (closed system; paper dated 2015).
Applicability: context only. Monitoring counters at fixed intervals, per-series compression, not ticks. Useful only as the baseline definition of DoD + XOR.

| # | Value | Counted | Quote (verbatim) | Location |
|---|---|---|---|---|
| G1 | 1.37 bytes per point (timestamp + value together); 16 bytes uncompressed; "12x" | bytes per (ts,value) pair, baseline 16 B = 64-bit ts + 64-bit double | "compress time series to an average of 1.37 bytes per point, a 12x reduction in size" | Sec. 2.1 end, printed p.1817 (PDF p.2); repeated Sec. 4 p.1818, Sec. 4.1.2 p.1821 |
| G2 | 96.39% of timestamps in the '0' DoD case (1 bit) | % of a sample of 440,000 real timestamps; label is bits per case | Fig. 3 label "96.39 (1)"; caption "Taken from a sample of 440,000 real time stamps" | Fig. 3, Sec. 4.1.1, printed p.1820 (PDF p.5) |
| G3 | 0.19% at 9 bits ([-63,64]); 3.35% at 12 bits ([-255,256]); 0.03% at 16 bits ([-2047,2048]); 0.03% at 36 bits ("Rest") | same | Fig. 3 bar labels "0.19 (9)", "3.35 (12)", "0.03 (16)", "0.03 (36)" | Fig. 3, p.1820 |
| G4 | DoD buckets: '0'; '10'+7 bits; '110'+9 bits; '1110'+12 bits; '1111'+32 bits | format definition | "Otherwise store '1111' followed by D using 32 bits" | Sec. 4.1.1 list, p.1820 |
| G5 | XOR values, figure: 59.06% single bit (1.0 b), 28.30% '10' (26.6 b), 12.64% '11' (39.6 b) | % of sample of 1.6 million real values; label is avg bits in that case | Fig. 5 labels "59.06 (1.0)", "28.30 (26.6)", "12.64 (39.6)" | Fig. 5, printed p.1821 (PDF p.6) |
| G6 | XOR values, prose: 51% single bit, ~30% '10' (26.6 b), 19% '11' (36.9 b) | same section, different numbers | "Roughly 51% of all values are compressed to a single bit" | Sec. 4.1.2 prose, p.1821 |

**Discrepancy, must not be hidden:** G5 (figure) and G6 (prose) on the same page disagree: 59.06/28.30/12.64 vs 51/30/19, and 39.6 vs 36.9 bits for the '11' case. The paper does not explain this. A target must cite which one it uses.

Assumptions behind the 1.37 figure:
| Assumption | Quote | Location |
|---|---|---|
| Regular intervals | "the vast majority of ODS data points arrived at a fixed interval" | Sec. 4.1.1, p.1820 |
| 60-second example | "it is common for a time series to log a single point every 60 seconds" | Sec. 4.1.1, p.1820 |
| Timestamps are second-resolution in the DoD examples | "a time stamp that is 1 second early or late" | Sec. 4.1.1, p.1820. Footnote 1: 14-bit first delta spans "a bit more than 4 hours (16,384 seconds)" |
| Slow-moving and often integer values | "many data sources only store integers into ODS" | Sec. 4.1.2, p.1820 |
| 2-hour block; returns diminish beyond | "blocks that extend longer than two hours provide diminishing returns" | Sec. 4.1.2 + Fig. 6, p.1821 |
| 15-second granularity requirement | "Support time series with 15 second granularity (4 points per minute per time series)" | Sec. 2.2, p.1818 |
| Scale | "700 million data points (time stamp and value) added per minute"; 1.3TB RAM across 20 machines for 26 hours | Sec. 2.2 p.1818; Sec. 4 p.1819 |
| Per-series, no cross-series compression | "no additional compression used across time series" | Sec. 4.1, p.1819 |

Encode/decode rate: **the paper states none.** No per-core throughput anywhere (searched all 12 pages for per second/per core/GHz/Xeon). Only system-level ingest (700M points/min) and query rates (5,000 steady, 40,000 peak QPS, PDF p.10, printed p.1825, Sec. 6.3 "Read latency matters"). Do not cite a Gorilla encode rate from this paper.

G7 (project-published, not peer-reviewed): Beringei README: "a single machine is able to compress more than 1.5 million datapoints/second"; "compress real world time series data by over 90%". URL https://raw.githubusercontent.com/facebookarchive/beringei/master/README.md (cached CACHE-D/tsdoc-beringei/README.md line 17). Hardware, threads: not stated. Context only.

## P2. Chimp (Liakos et al., PVLDB 15(11), 2022)
URL: https://vldb.org/pvldb/vol15/p3058-liakos.pdf (cached: CACHE-D/chimp/p3058-liakos.pdf). Rendered PDF pp.8-11 read.
Hardware: Xubuntu 20.04, Intel Core i5-4590 3.30GHz, 6MB L3, 16GB DDR3 1600MHz (Sec. 5.1, printed p.3065). Storage: not stated. Threads: not stated. Language: Java (Sec. 5). Block: 1,000 data points (Sec. 5.1). Software: Chimp repo github.com/panagiotisl/chimp, version not stated; general-purpose codecs are HBase defaults.
Data set: Stocks-UK/USA/DE from INFORE, "exchange price data"; 1 year each; 115,146,731 / 374,428,996 / 45,403,710 values; decimal digits 1/2/3 (Table 2, printed p.3065; Sec. 5.1 p.3066). Fields beyond "price" not stated here.

STOCK ROWS, bits/value (uncompressed baseline 64), Table 3, printed p.3067 (PDF p.10). "Compression size results in bits/value."
| Dataset | Xz | Brotli | LZ4 | Zstd | Snappy | FPC | Gorilla | Chimp | Chimp128 |
|---|---|---|---|---|---|---|---|---|---|
| Stocks-UK (1 dp) | 7.61 | 8.54 | 19.84 | 10.32 | 15.80 | 46.15 | 33.45 | 31.27 | 16.70 |
| Stocks-USA (2 dp) | 7.19 | 8.11 | 18.16 | 9.92 | 14.68 | 36.02 | 36.43 | 34.67 | 12.06 |
| Stocks-DE (3 dp) | 8.80 | 9.96 | 20.63 | 12.06 | 16.83 | 44.54 | 45.63 | 42.88 | 13.46 |
| Time-series average (14) | 20.03 | 22.29 | 32.23 | 24.57 | 31.09 | 47.38 | 47.79 | 43.57 | 26.44 |
Applicability: direct evidence that single-reference XOR is poor on stock prices (33-46 bits/value) and that block/window methods do far better, but the data are low-precision prices stored as doubles, per-file, not ITCH integers.

SPEED, Table 4, printed p.3067. "Compression and decompression time per block (1,000 values) in μsec". Averages over all 14 time series, not stock-specific.
| Algo | Compress µs/1000 | Decompress µs/1000 | Derived values/s (1000/time) |
|---|---|---|---|
| Gorilla | 39.28 | 31.79 | ~25.5M / ~31.5M |
| Chimp | 31.18 | 30.68 | ~32.1M / ~32.6M |
| Chimp128 | 35.81 | 28.47 | ~27.9M / ~35.1M |
| Zstd | 163.27 | 56.64 | ~6.1M / ~17.7M |
| Snappy | 89.56 | 34.17 | ~11.2M / ~29.3M |
| LZ4 | 1,199.04 | 25.29 | ~0.8M / ~39.5M |
Applicability: context only. Java on a desktop i5-4590, averaged over mixed data. Stock-only speed not reported.

Other quotes: "offering compression ratios that are competitive against general purpose encoding algorithms" (Table 3 caption). Chimp128 ring buffer: "the respective 33KB of memory requirements are very modest" (Sec. 4.2 end, p.3065).

## P3. Elf (Li et al., PVLDB 16(7), 2023)
URL: https://www.vldb.org/pvldb/vol16/p1763-li.pdf (cached: CACHE-D/elf/p1763-li.pdf). Table 3 confirmed on a 200 dpi render of PDF p.10.
Hardware: "Windows 11, 11th Gen Intel(R) Core(TM) i5-11400 @ 2.60GHz CPU and 16GB memory" (Sec. 6.1.4, printed p.1771, PDF p.9). JDK 1.8. Storage, threads: not stated.
Block: 1,000 records; "executed on up to 100 blocks per dataset" (Sec. 6.1.4). So at most ~100k values per dataset, not the full files. Same record counts as Chimp (Table 2).
Metric: "the compression ratio is defined as the ratio of the compressed data size to the original one" (Sec. 6.1.3): lower is better, baseline 64-bit double. Times are µs per 1,000-value block.
Decimal significand count β (Table 2): SUK 5, SUSA 4, SDE 6.

STOCK ROWS, Table 3, printed p.1772 (PDF p.10). Bits/value column is derived (ratio x 64) and only 2-sig-fig accurate.
| Algo | SUK ratio | SUSA ratio | SDE ratio | Derived bits/value | Compress µs (SUK/SUSA/SDE) | Decompress µs |
|---|---|---|---|---|---|---|
| Gorilla | 0.58 | 0.68 | 0.72 | ~37/44/46 | 17/17/17 | 16/17/17 |
| Chimp | 0.52 | 0.64 | 0.67 | ~33/41/43 | 23/22/23 | 22/24/24 |
| Chimp128 | 0.29 | 0.23 | 0.27 | ~19/15/17 | 24/22/25 | 18/16/18 |
| FPC | 0.74 | 0.70 | 0.73 | ~47/45/47 | 28/28/28 | 25/24/25 |
| Elf | 0.22 | 0.24 | 0.26 | ~14/15/17 | 54/56/58 | 37/45/44 |
| Xz | 0.16 | 0.17 | 0.19 | ~10/11/12 | 877/836/900 | 156/133/148 |
| Zstd | 0.22 | 0.24 | 0.26 | ~14/15/17 | 177/112/117 | 31/31/50 |
| LZ4 | 0.39 | 0.39 | 0.41 | ~25/25/26 | 966/976/952 | 19/19/18 |
| Snappy | 0.32 | 0.32 | 0.35 | ~20/20/22 | 169/56/172 | 19/21/20 |
Applicability: direct evidence for XOR codecs on stock prices, as doubles. Java, small sample.
Quote on why Chimp128 wins SUSA: "there are many duplicate values within 128 consecutive records" (Sec. 6.2.1, p.1771). "Chimp128 can use only 9 bits to represent the same value" (same).
Cross-study caution: Gorilla on Stocks-USA is 36.43 b/v in Chimp (all data), 37.7 in ALP, ~44 in Elf (first ≤100 blocks). Same algorithm, different sample and implementation; do not mix studies in one ranking.

## P4. ALP (Afroozeh, Kuffó, Boncz; SIGMOD 2024 / PACMMOD 1(4) Art. 230, 2023)
URL: https://ir.cwi.nl/pub/33334/33334.pdf (cached: CACHE-D/alp/33334.pdf, author's version, no printed page numbers; "PDF p.N" is the locator). Rendered pp.3,4,9,10,11,12 read; Table 4 confirmed on a 220 dpi crop.
DOI per reproducibility report: 10.1145/3626717.
Hardware for ratios/micro-speed: Intel Ice Lake 8375C 3.5 GHz, AVX512 (Table 3, PDF p.8), "CPU turbo-scaling features were disabled when available" (Sec. 4.2, PDF p.10). Also Zen3 EPYC 7R13 3.6 GHz, Apple M1 3.2 GHz, Graviton2 2.5 GHz, Graviton3 2.6 GHz. Memory, storage for micro-benchmarks: not stated. Threads: micro-benchmarks single (not stated explicitly); Table 6: 1/8/16 threads, "16 cores (32 SMT) and 256GB of RAM" (Sec. 4.3, PDF p.11).
Language: C++; Gorilla/Chimp/Patas "adjusted" from "the DuckDB codebase"; PDE and Elf from original authors; Zstd default level 3 (Sec. 4, PDF p.8-9). Compiler version: not found on the pages read.
Data set: Stocks-DE 43,565,658 / Stocks-UK 59,305,326 / Stocks-USA 282,076,179 values, source INFORE (Table 1, PDF p.3). NOTE: different counts from Chimp/Elf (45.4M / 115.1M / 374.4M). Not stated why.

STOCK ROWS, bits per value, baseline 64, "uncompressed data is 64 bits per value". Table 4, PDF p.9.
| Dataset | Gor. | Chimp | Ch128 | Patas | PDE | Elf | ALP | LWC+ALP | Zstd |
|---|---|---|---|---|---|---|---|---|---|
| Stocks-DE | 46.9 | 42.9 | 13.6 | 20.8 | 25.1 | 12.3 | 11.0 | 11.0 | 9.4 |
| Stocks-UK | 35.6 | 31.3 | 16.8 | 21.5 | 26.1 | 11.0 | 12.7 | 12.7 | 10.7 |
| Stocks-USA | 37.7 | 35.0 | 12.2 | 19.2 | 26.1 | 8.8 | 7.9 | 7.9 | 7.8 |
| TS AVG | 48.1 | 42.6 | 24.5 | 30.9 | 30.0 | 18.2 | 16.4 | 13.2 | 17.2 |
| ALL AVG | 42.2 | 37.7 | 28.7 | 35.5 | 31.4 | 23.1 | 21.7 | 18.8 | 20.6 |
Applicability: direct, the strongest published comparison on stock prices as doubles. Still not ITCH: decimal precision max 2-3, doubles.

Stock data properties, Table 2, PDF p.4 (C2 max decimal precision / C6 non-unique% / C7 avg / C8 std dev within vector / C11 one-exponent success per value / C14 XOR leading zero bits / C15 trailing):
| Dataset | C2 | C6 | C7 | C8 | C11 | C14 | C15 |
|---|---|---|---|---|---|---|---|
| Stocks-DE | 3 | 89.2% | 63.8 | 9.1 | 84.2% | 24.9 | 5.8 |
| Stocks-UK | 2 | 88.1% | 1593.7 | 317.1 | 84.5% | 23.7 | 19.4 |
| Stocks-USA | 2 | 91.5% | 146.1 | 11.7 | 87.5% | 32.6 | 16.8 |
Note: C2 for Stocks-UK says 2 here, Chimp says 1 decimal digit. Column labels read from the rendered table header; C7/C8 interpreted under "Values per Vector Non-Unique % | Avg. | Std. Dev."

SPEED, Table 5, PDF p.10: "Average compression and decompression speed as tuples processed per computing cycle of all datasets on the Ice Lake architecture." All-dataset average, single vector (1024 values) repeated 300K times, "to ensure all data is L1 resident". Zstd measured on about 1 MB rowgroup.
| Algo | Compress tuples/cycle | Decompress tuples/cycle | ALP faster (dec) |
|---|---|---|---|
| ALP | 0.487 | 2.609 | - |
| Patas | 0.060 | 0.157 | 17x |
| PDE | 0.002 | 0.387 | 7x |
| Gorilla | 0.052 | 0.047 | 55x |
| Chimp | 0.042 | 0.039 | 66x |
| Chimp128 | 0.040 | 0.040 | 65x |
| Elf | 0.010 | 0.012 | 215x |
| Zstd | 0.035 | 0.101 | 26x |
Applicability: context for ranking; L1-resident microbenchmark on AVX-512, overstates sustained decode. Quote: "ALP clearly outperforms every other algorithm in both compression and decompression speed in every dataset".

END-TO-END, Table 6, PDF p.11: City-Temp in Tectorwise, tuples per CPU cycle per core, "scaled all datasets up to 1 billion doubles".
| Algo | SCAN 1T | SCAN 8T | SCAN 16T | SUM 1T | SUM 8T | SUM 16T | COMP |
| ALP | 1.337 | 1.074 | 0.882 | 0.233 | 0.230 | 0.234 | 0.147 |
| Uncompressed | 0.565 | 0.408 | 0.350 | 0.197 | 0.186 | 0.175 | N/A |
Remaining rows are in the table; Gorilla SCAN|1 0.030, Patas 0.067, Chimp 0.021, Chimp128 0.028, PDE 0.070, Zstd 0.044 (text layer, PDF p.11). Applicability: context; temperature data, in a research engine. Shows scan speed on compressed can exceed uncompressed because of RAM bandwidth at low bit width.

Reproducibility (ACM ARI report, Bindea et al., 2024): "the results about compression ratio are clearly reproduced" (Sec. 4.2, printed p.54). Table 2 stock rows, reproduced (original): Stocks-USA ALP 7.9 (7.9), Gorilla 37.2 (37.7), Zstd 8.6 (7.8); Stocks-UK ALP 12.6 (12.7), Zstd 10.3 (10.7); Stocks-DE ALP 11.0 (11.0), Zstd 10.5 (9.4). Reviewer machines: MacBook Air M2 16GB (speed figure), Intel Core i7 11th gen 2.5GHz, Xeon 2.60 GHz. URL https://ir.cwi.nl/pub/35864/35864.pdf (CACHE-D/alp-repro/35864.pdf), DOI 10.1145/3687998.3717057. PDE and Elf "not included in the repository" so those columns were not reproduced.

## P5. BUFF (Liu, Jiang, Paparrizos, Elmore; PVLDB 14(11), 2021). Same-data XOR vs fixed-point, incl. Stock
URL: https://www.vldb.org/pvldb/vol14/p2586-liu.pdf [URL checked: HTTP 200, application/pdf] (cached: CACHE-D/buff/p2586-liu.pdf). Rendered printed pp.2593-2595 read; Fig. 8 cropped at 400 dpi.
Hardware: "2 Intel(R) Xeon(R) CPUs E5-2670 v3 @ 2.30GHz, 128GB memory, 250GB HDD, and Ubuntu 18.04" (Sec. 4, printed p.2593). Rust 1.49.0, AVX2. Threads: "with a single thread" for the ingestion claim (abstract area); per-figure thread count not stated.
Data set: "Stock" = Kaggle "Huge stock market dataset" (ref [1]), precision scale 3 (Fig. 6d). Ingest "in the IEEE 64-bit float format" (Sec. 4.1). Size of the Stock file: not stated on the pages read. Not tick data as far as stated.
Counted: Fig. 8 "compression ratio (smaller is better)", baseline not spelled out on the figure; Sec. 2 says "We use 32-bits to save space even though Gorilla was originally designed for 64-bits format", in the context of an example. Baseline for Fig. 8 Gorilla is therefore unclear (32 vs 64). **Do not convert to bits/value.**

| Finding | Evidence | Location |
|---|---|---|
| On Stock, vanilla GORILLA is the tallest bar, near the 1.0 gridline. BUFF and GORILLABD are the shortest; SPRINTZ and FIXED are taller than those two and shorter than GORILLA. | Bars have **no printed values**. Ordering only. No number should be taken from this figure. | Fig. 8, printed p.2595 (PDF p.10) |
| Gorilla poor on bounded-precision data | "Gorilla compression performs poorly on most of our datasets as there are very limited repeated or similar adjacent values." | Sec. 4.4.1, p.2594 |
| Reducing to bounded precision first helps XOR | "the latter solution significantly improves compression performance" (bounded precision float as input) | Sec. 4.2.1, p.2594 |
| Fixed-point baseline | "bounded fixed-point (as FIXED) as a baseline with bounded precision applied to achieve just-enough precision and delta encoding used to offset the custom range" | Sec. 4.2.2, p.2594 |
| Sprintz/FIXED/BUFF cluster | "Buff performs similarly with Sprintz and FIXED since they leverage the bounded precision and range" | Sec. 4.4.1, p.2595 |
Applicability: direct evidence that float XOR is beaten by integer/fixed-point coding on stock data in one study, but no citable number. Daily-bar stock data per the citation title is not confirmed; check ref [1] before relying on it.

## P6. IoTDB encoding comparison (Xiao et al., PVLDB 15(10), 2022). Int vs float under Gorilla
URL: https://www.vldb.org/pvldb/vol15/p2148-song.pdf [URL seen in search results] (cached: CACHE-D/iotdb-encoding/p2148-song.pdf).
Hardware: "2.1 GHz Intel(R) Xeon(R) CPU and 128GB RAM" (Sec. 7, printed p.2155, PDF p.8). Model, threads, storage: not stated. Apache IoTDB, version: not stated on the pages read. 1088 real series (Table 7), "28 combinations of 7 encoding schemes and 4 compression schemes on 4 different data types" (Sec. 7.1).
| Finding | Quote | Location |
|---|---|---|
| XOR codec better on integers than on floats | "GORILLA performs better on INT32 and INT64 than FLOAT and DOUBLE since positions of leading and trailing 0 are more similar." | Sec. 7.1.1, printed p.2155 (PDF p.8) |
| Second-order delta | "TS_2DIFF encoding achieves good (low) compression ratio, with or without compression." | Sec. 7.1.1, p.2155 |
| Scope | "While there is no winner in all the data features, TS_2DIFF performs well in a number of cases." | Sec. 1, printed p.2149 (PDF p.2) |
Numbers: only box plots (Fig. 9); no values extracted. Applicability: direct qualitative evidence for the int-vs-float question. IoT data, not ticks. Applies to the value column, not timestamps; the paper's time-column treatment was not separately tabulated in what I read.

## P7. DeXOR (Lv et al., PVLDB 19(5): 849-861, 2026)
URL: https://www.vldb.org/pvldb/vol19/p849-li.pdf [URL seen in search results] (cached: CACHE-D/dexor/p849-li.pdf). Table 2 confirmed on a 220 dpi render of PDF p.9.
Hardware: "Ubuntu server (Intel Xeon Gold 6348, 512 GB RAM)" (Sec. 6, PDF p.9). "All SLC schemes are Java-based and Docker-containerized". Clock, storage, threads: not stated.
Metric: ACB = average compression bits; "Compression ratio equals ACB/64". Speed in MB/s of input (assumed from Table 2 caption; input definition not stated further).
Data: datasets "from previous studies [33, 35, 49]"; "SUSA, SUK, SDE" presumably the Stocks sets, kept in original order. Block setting: not stated.

STOCK ROWS, Table 2, printed p.857 (PDF p.9):
| Algo | SUSA ACB | SUK ACB | SDE ACB | Com. MB/s S/K/D | Decom. MB/s S/K/D |
|---|---|---|---|---|---|
| Gorilla | 43.85 | 35.65 | 46.18 | 18.19/30.80/17.69 | 62.94/48.26/44.95 |
| Chimp | 49.52 | 43.31 | 52.18 | 35.63/34.60/26.57 | 46.08/21.46/58.53 |
| Elf | 24.78 | 24.25 | 27.10 | 36.20/18.45/22.77 | 28.57/57.27/23.17 |
| Elf+ | 21.86 | 22.82 | 25.29 | 39.74/31.35/26.23 | 53.84/58.62/51.62 |
| Camel | 11.45 | 10.18 | 13.58 | 9.61/10.44/4.21 | 32.68/83.70/35.83 |
| DeXOR | 9.71 | 11.59 | 12.22 | 49.63/37.71/19.60 | 84.58/87.13/91.15 |
Applicability: direct for streaming float coding on stock prices. Speeds are Java in Docker and far below ALP/Gorilla numbers elsewhere, so ranking only.
Cautions:
- Chimp is WORSE than Gorilla here on all three stock sets (e.g. 49.52 vs 43.85), the reverse of Chimp's own Table 3 (34.67 vs 36.43). Contradiction between studies, unexplained.
- Table 4 (PDF p.11), "geomean", gives ALP AVG. Comp. Bits 52.97, Comp. 2.07 MB/s, Decomp. 38.73 MB/s, vs Chimp128 31.37, SElf* 19.55, Elf* 17.55, DeXOR 17.82. That ALP figure is irreconcilable with ALP's own all-dataset 21.7 b/v (and the reproducibility report's 21.7). Treat as suspect; it is a Java reimplementation context and a geomean over a different dataset mix, but the gap is not explained.
- DeXOR says Camel "employs lossy truncation" on low-dp sets where Camel beats it (DPT, SUK, EVC); Table 2 caption: "Camel [49] works losslessly only in low-dp datasets."

## P8. BtrBlocks / Pseudodecimal Encoding (Kuschewski et al., SIGMOD 2023)
URL: https://www.cs.cit.tum.de/fileadmin/w00cfj/dis/papers/btrblocks.pdf [URL checked: HTTP 200, application/pdf] (cached: CACHE-D/btrblocks/btrblocks.pdf). Read in text layer, PDF p.10 for Table 3.
Hardware: c5n.18xlarge, "Intel Xen [sic] Platinum 8000 series (Skylake-SP) CPU with 36×3.5 GHz cores (72 threads)" (Sec. 6). Threads for Table 3: not stated.
| Finding | Quote | Location |
|---|---|---|
| Decimal-origin prices are the named failure case for binary encodings | "A common example is storing monetary prices such as $3.25 or $0.99 as floating-point numbers." | Sec. 4.1 |
| Why FOR+BP fails on raw doubles | "standard techniques such as FOR+Bit-packing are not effective" | Sec. 4.1 |
| PDE vs XOR on price columns | "it often outperforms other schemes on columns with less precision, like the abundant pricing data columns" | Sec. 6.5 |
| Fixed-precision is the better representation | "double-precision floating-point numbers are frequently used where fixed-precision numbers would be sufficient (and indeed better suited)" | Sec. 4.1 |

Table 3, "Compression Ratios of Pseudodecimal Encoding (PD), other double schemes (Public BI, large double columns)", ratio = raw/compressed (higher better), PDE cascaded into FastBP128:
| Column | FPC | Gorilla | Chimp | Chimp128 | PDE |
|---|---|---|---|---|---|
| CommonGov./10 | 1.2 | 1.1 | 1.5 | 1.9 | 1.8 |
| CommonGov./26 | 15.1 | 48.0 | 28.0 | 6.9 | 75.0 |
| CommonGov./30 | 6.4 | 7.0 | 7.6 | 5.0 | 7.8 |
| CommonGov./31 | 9.3 | 14.3 | 13.3 | 5.6 | 23.4 |
| CommonGov./40 | 14.3 | 38.0 | 25.0 | 6.7 | 54.6 |
| Arade/4 | .95 | 1.1 | 1.2 | 1.6 | 1.9 |
| NYC/29 | 1.5 | 2.1 | 2.5 | 1.7 | 1.0 |
| CMSProvider/1 | 1.5 | 1.7 | 1.8 | 2.4 | 1.6 |
| CMSProvider/9 | 2.7 | 2.3 | 3.4 | 2.4 | 6.6 |
| CMSProvider/25 | .98 | .98 | 1.1 | 1.2 | 1.0 |
| Medicare/1 | 1.2 | 1.4 | 1.5 | 2.0 | 1.5 |
| Medicare/9 | 2.6 | 2.3 | 3.4 | 2.3 | 6.3 |
Applicability: direct on the decimal-origin question (monetary columns), but BI tables, not time-ordered ticks. Note contrast with ALP Table 4 stocks, where PDE (25-26 b/v) trails Chimp128 (12-17 b/v) and Elf: PDE's per-value exponent costs on stock prices.

## P11. BOS, Bit-packing with Outlier Separation (ICDE 2025, DOI 10.1109/ICDE65448.2025.00107). THE CLOSEST SAME-DATA XOR vs INTEGER RESULT ON STOCK PRICES
URL: https://sxsong.github.io/doc/25ICDE-BOS.pdf (cached: CACHE-D/tsc-bos-icde25/25ICDE-BOS.pdf). Fig. 10(a) is a printed table inside a figure, printed p.1384, PDF p.10; confirmed on a render of that page, not only the text layer.
Hardware: "Apple M1 Pro chip, featuring 8 CPU cores and 14 GPU cores... 16GB of unified memory" (Sec. VIII-A). Threads: not stated. Software: implemented in Apache IoTDB and TsFile (Sec. VII), version not stated.
Counted: ratio = "uncompressedSize/compressedSize" (Sec. VIII), higher is better. Base width of the uncompressed value: not stated. Time: figure axis "ns/point".
Float handling (all integer codecs on float data): "Algorithms designed for integers, such as RLE, SPRINTZ and TS2DIFF, first convert float into integer by scaling 10^p, where p is the precision". So the float rows are a true test of "scale to integer, then integer-code" against XOR on the same values.
Data: Nifty-Stocks (NS): Float, 295,193,088 values, Table III, ref [26] https://www.kaggle.com/datasets/tadakasuryateja/nifty-50-stocks. Tick vs bar granularity: not stated.

Fig. 10(a), Nifty-Stocks column, ratio (higher is better):
| Method | NS ratio | Method | NS ratio |
|---|---|---|---|
| GORILLA | 1.19 | RLE+BP | 1.56 |
| CHIMP | 1.39 | RLE+BOS-V / -B | 1.74 |
| Elf | 1.37 | RLE+BOS-M | 1.72 |
| BUFF | 1.56 | SPRINTZ+BP | 1.81 |
| TS2DIFF+BP | 1.82 | SPRINTZ+BOS-V / -B | 2.07 (best) |
| TS2DIFF+BOS-V / -B | 2.04 | SPRINTZ+BOS-M | 2.00 |
| TS2DIFF+BOS-M | 2.03 | | |

Other rows of the same figure (Gorilla vs TS2DIFF+BP):
| Set | Type | Gorilla | TS2DIFF+BP |
|---|---|---|---|
| EE, MT, VC, CS | integer | 1.67, 2.23, 1.94, 1.71 | 2.05, 2.38, 2.64, 2.44 |
| TC | integer | 6.39 | 4.00 (Gorilla wins) |
| TT | integer | 3.14 | 4.08 |
| YE, GM, UE, CV, TF | float | 1.20, 1.58, 0.96, 1.53, 2.02 | 2.15, 1.69, 1.96, 2.41, 2.76 |

Fig. 10(c), Nifty-Stocks, ns/point, compress / decompress: Gorilla 58/2; Chimp 52/3; Elf 65/45; BUFF 148/3; RLE+BP 16/12; SPRINTZ+BP 12/11; SPRINTZ+BOS-V 992/43; SPRINTZ+BOS-B 303/23; SPRINTZ+BOS-M 33/18; TS2DIFF+BP 11/12; TS2DIFF+BOS-V compress 927 (text layer).
CAUTION: Gorilla decompress of 2 ns/point is faster than bit-packing's 12 here and is out of line with every other paper's decode figures. Do not use it without reproduction.
Applicability: DIRECT evidence for the key question, with limits. Same stock data, same harness, scaled integer plus delta plus bit-packing beats Gorilla 1.82x to 1.19x (53% larger ratio, derived) and beats Chimp and Elf on ratio. Limits: Nifty-50 prices, not US; granularity unknown; p is a per-dataset decimal precision, not a declared 1e-4 fixed point; the TC row shows Gorilla can win on an integer set; the paper's reason was not established in what I read. "BOS has replaced Bit-packing in IoTDB" was only seen in a search summary, so it is not claimed here.

## P14. BTrDB (Andersen & Culler, FAST '16). HIGH-RESOLUTION TIMESTAMP DELTA-DELTA
URL: https://www.usenix.org/conference/fast16/technical-sessions/presentation/andersen (cached: CACHE-D/btrdb/fast16-andersen.pdf).
Quotes, Sec. 5.4, PDF p.7: "nanosecond timestamps produce very large deltas"; delta-delta "encodes each delta as the difference from the mean of a window of previous delta values"; "This method is lossless only if used with integers"; then Huffman with a fixed tree.
Number, Sec. 6.3, PDF p.10: 2.757 TB stored, "5.514 bytes per reading" against 16 raw bytes, "compression ratio of 2.9x". All-in, including statistics overhead, so not a pure timestamp figure.
Data: uPMU phasor data at 120 Hz, timestamps accurate to 100 ns (Sec. 6.3); nanosecond units (PDF p.4). Hardware: EC2 nodes. Threads: not stated.
Applicability: context for (d). Regular 120 Hz sampling with ns-unit timestamps. Not irregular. Confirms that a windowed-mean predictor was chosen because plain deltas are large at ns resolution, and that the predictor is integer-only.

## P15. TSXor (Bruno, Nardini, Pibiri, Trani, Venturini; SPIRE 2021)
DOI: https://doi.org/10.1007/978-3-030-86692-1_18 (Crossref: Lecture Notes in Computer Science, 2021) (cached: CACHE-D/tsxor/tsxor.pdf). The SPIRE venue is not printed on the cached file. Table 4, PDF p.5.
Hardware: Intel i7-7700 3.60GHz, 64GB, Ubuntu 18.04, C++. Threads: not stated.
| Data | Ratio TSXor / FPC / Gorilla | Decomp MB/s | Comp MB/s |
|---|---|---|---|
| Oxford-Man (Realized Library, 143,397 x 19, 79.85% distinct) | 1.30x / 1.06x / 1.28x | 604 / 222 / 574 | 15 / 170 / 630 |
| AMPds2 | 6.39x / 1.10x / 2.03x | 1174 / 411 / 666 | 67 / 339 / 704 |
Applicability: context. Oxford-Man is realized-volatility measures derived from finance data, NOT prices. It shows Gorilla near 1.3x on noisy finance-derived doubles.

## P16. Sprintz (Blalock, Madden, Guttag; IMWUT 2(3) Art. 93, Sept 2018)
URL: https://arxiv.org/abs/1808.02515 (cached: CACHE-D/sprintz/1808.02515.pdf). Hardware on PDF p.12: 2013 MacBook Pro, 2.6GHz i7-4960HQ, single thread, best of ten runs.
Quotes: "can decompress at up to 3GB/s in a single thread"; "over 200MB/s on the highest-ratio setting and 600MB/s on the fastest". Fire stage: "encode at up to 5GB/s and decode at up to 6GB/s". memcpy reference 7.5 GB/s.
Data: UCR archive, 8- and 16-bit integers. Gorilla not benchmarked. No stock data. Bits per value: not extracted as a stock figure because none exists.
Applicability: context. Integer predictive coding at small widths. ITCH prices are 32-bit, so these rates do not carry over.

## P17. FastLanes (Afroozeh & Boncz, PVLDB 16(9), 2023)
URL: https://ir.cwi.nl/pub/33289/33289.pdf, HTTP 200 (cached: CACHE-D/fastlanes/33289.pdf). PDF p.4: "70 values per cycle = 140 billion values per second on one 2GHz core" (3-bit values, AVX512). Decode "can reach 70 tuples/cycle (T=8, W=1)" (Fig. 8). L1-resident vector decoded 30M times, turbo disabled. DELTA figure (Fig. 10) has no printed numbers.
Applicability: context. Upper bound for SIMD bit-unpacking on tiny widths; not a tick decode rate.

## P18. Beyond Compression (Hishida et al., PVLDB 18(11): 4396-4409, 2025, doi 10.14778/3749646.3749701)
URL: https://www.vldb.org/pvldb/vol18/p4396-hishida.pdf (cached: CACHE-D/beyond-compression/p4396-hishida.pdf). Result statement PDF p.9: "The top methods in terms of compression ratio are Gzip, Zstd, ALP, and Elf, in that order. ALP also achieves the highest compression and decompression throughput." Hardware PDF p.7: Xeon Gold 6216/6242, 192GB, single thread, AVX2.
Stocks-DE/UK/USA are in its data set but box plots give no per-stock numbers.
Applicability: independent peer-reviewed confirmation of the ordering, no stock row.

## P19. Patas (EDBT 2024 demo)
URL: https://openproceedings.org/2024/conf/edbt/paper-248.pdf (cached: CACHE-D/tsc-patas-edbt/paper-248.pdf). Quote: "applies zero-padding to the meaningful bits of each resulting XOR-ed value, to form 1-byte aligned representations". Part of DuckDB as of 0.6.0. No numbers in this paper. Patas numbers: ALP Tables 4 and 5 (P4).

---

# NOT PEER-REVIEWED: PREPRINTS AND A THESIS

These may later have been published; only what the cached file shows is claimed.

## NP1. VIREL (Zhang et al., arXiv:2607.22433v3, 26 Oct 2026). PREPRINT, the most uniform harness found
URL: https://arxiv.org/pdf/2607.22433 (cached: CACHE-D/virel/2607.22433.pdf). Table I confirmed on a 230 dpi render of PDF p.9.
Hardware: "pinned to one core of a 64-core AMD EPYC 9554 (Zen 4, 256 GB memory, Ubuntu 22.04, Linux 6.8) with GCC 12.3, -O3" (Sec. Evaluation/Protocol, PDF p.8). Clock: not stated. Falcon runs on NVIDIA RTX PRO 6000 GPU with CUDA 12.8. Pcodec 1.0.4 via Python binding, "so we report its ratio only". xz 5.2.5, zstd 1.5.7.
Baselines: "ALP, ELF*, ELF, Chimp, Gorilla, and DeXOR run from the authors' C++ code in one harness". Threads: one. Data in memory. Median of five runs.
Metric: "COMPRESSION RATIO AND ONE-THREAD THROUGHPUT (HIGHER IS BETTER)". Ratio = raw/compressed (multiplier). 1,024-value INDEPENDENT pages (so each page restarts: unfavourable to long-history methods). MB/s of raw input.

STOCK ROWS, Table I, PDF p.9. Bits/value derived as 64/ratio.
| Codec | SUK x | USA x | SDE x | Derived b/v (USA) | Enc MB/s S/U/D | Dec MB/s S/U/D |
|---|---|---|---|---|---|---|
| VIREL-Exact UPPER | 12.93 | 13.82 | 12.79 | ~4.6 | 130/128/97 | 2,372/2,535/2,418 |
| VIREL-Exact FAST | 12.42 | 13.36 | 12.29 | ~4.8 | 818/831/712 | 6,539/7,009/6,792 |
| Falcon (GPU) | 8.99 | 12.25 | 8.86 | ~5.2 | 46/45/43 | 187/193/185 |
| Pcodec | 10.13 | 10.81 | 10.02 | ~5.9 | n/a | n/a |
| ELF* | 6.52 | 6.93 | 5.22 | ~9.2 | 137/148/140 | 590/554/483 |
| ELF | 5.44 | 5.70 | 4.53 | ~11.2 | 151/164/157 | 615/559/525 |
| ALP | 5.91 | 7.06 | 5.53 | ~9.1 | 123/119/118 | 4,594/4,605/4,591 |
| DeXOR | 5.51 | 6.57 | 5.22 | ~9.7 | 87/96/83 | 326/353/299 |
| Chimp | 1.93 | 1.56 | 1.49 | ~41 | 516/498/509 | 537/544/537 |
| Gorilla | 1.75 | 1.48 | 1.39 | ~43 | 617/626/640 | 809/766/781 |
Applicability: direct for ranking codecs on these stock doubles in C++ on a modern core; best single table in this file for encode/decode. Not peer-reviewed. Consistency check: its ALP USA (~9.1 b/v) is close to ALP's own 7.9 b/v and its DeXOR USA (~9.7) matches DeXOR's own 9.71, which further isolates DeXOR Table 4's ALP 52.97 as an outlier.
Domain note, quote: "Stocks-UK quotes in ticks of 0.1 and 0.05" (Sec. II/III, PDF p.3 area). Directly relevant: ITCH prices are multiples of 100 in 1e-4 units.

## NP2. Matt, Huang, Maag, "Lossless Compression of Time Series Data: A Comparative Study" (arXiv:2510.07015, 2025). PREPRINT per the file; venue not stated
URL: https://arxiv.org/abs/2510.07015 (cached: CACHE-D/lossless-ts-comparison/2510.07015.pdf). Hardware: "Intel Xeon Silver 4208 CPU (2.10 GHz) and 96 GB of RAM" (Sec. IV-A).
Why here: its whole frame is integer-valued series: "Time series datasets are typically distributed in floating point format although in most cases, this is not how the data was originally represented" (Sec. IV-C).
Quote on Gorilla: "Due to the generally lower compressibility of floats, it is not competitive on integer data." (Sec. II, item "Gorilla"). They therefore EXCLUDE Gorilla; so there is no measured Gorilla number on integers in this paper; the statement is an asserted reason, not a result.
Applicability: direct but qualitative for the int-vs-XOR question. No finance data.

## NP3. Falcon (arXiv:2511.04140v2, 11 Nov 2025). PREPRINT; PVLDB template with placeholder "XXX", publication not confirmed
URL: https://arxiv.org/abs/2511.04140 (cached: CACHE-D/tsc-falcon/2511.04140.pdf). Table 3 PDF p.9; Tables 4 and 5 PDF p.10.
Hardware: NVIDIA RTX 5080, i7-14700KF, 32GB DDR4, Ubuntu 24.04.2, CUDA 12.9. Threads: not stated per table.
Data: Stocks-price (SP), 11,321,684 values, "Stock price in 3 countries", INFORE. Ratio = compressed/original, lower is better. Throughput GB/s.
| Method | Ratio | Comp GB/s | Decomp GB/s |
|---|---|---|---|
| GPU Falcon | 0.096 | 8.23 | 8.81 |
| GPU ALP | 0.161 | 2.89 | 1.41 |
| CPU ALP | 0.161 | 0.70 | 4.04 |
| CPU Elf | 0.174 | 0.13 | 0.19 |
| CPU Elf* | 0.157 | 0.07 | 0.16 |
Other SP ratios, Table 3: ndzip 0.988, Lz4 0.424, Bitcomp 0.899, GDeflate 0.635, Snappy 0.403, GPU Elf* 0.144.
Applicability: context. GPU work is out of scope for tick-store; useful only for the CPU ALP row on stock doubles (4.04 GB/s decode on one modern desktop CPU).

## NP4. Pcodec (Loncaric, Jeppesen, Zinberg; arXiv:2502.06112v2, 25 May 2025). PREPRINT, NOT PEER-REVIEWED (no venue stated on the file)
URL: https://arxiv.org/abs/2502.06112 (cached: CACHE-D/pcodec-2025/paper.pdf).
Hardware: "bare-metal 96-core Intel Xeon Platinum 8488C CPU", 1 thread and 48 threads (Sec. 3.2.1). Large datasets limited to 2 million rows. Datasets: Air Quality, Housing, Payments, r/place, Taxi, Twitter. No stock data. Codec version not stated.
| Finding | Value / quote | Location |
|---|---|---|
| Multiplier modes | IntMult "ms-precise timestamps stored as µs"; FloatMult "prices that are multiples of 0.01" | Table 1, Sec. 2.1 |
| Consecutive delta order | "takes consecutive differences o times", 1 ≤ o ≤ 7 | Sec. 2.2 |
| Ratio | "29% to 94% higher compression ratio than other codecs" | Sec. 3.2.2 |
| Decode | ">1GiB/s per thread": Table 3 single-thread 1,432 to 5,450 MiB/s across six data sets | Table 3 |
Applicability: design evidence that a detected multiplier (m=100 on ITCH prices) belongs in the integer path; the paper's own example is order-of-magnitude-aligned timestamps. Ratio numbers are context only (no tick data). VIREL Table I gives Pcodec on stock rows: 10.13/10.81/10.02x.

## NP5. TritanDB (Siow et al., arXiv:1801.07947v1, Jan 2018; the ACM-style header "1:10" implies a journal submission, venue not confirmed on the file). PREPRINT as cached
URL: https://arxiv.org/pdf/1801.07947 (cached: CACHE-D/tritandb/1801.07947.pdf). Text layer read; PDF p.9 (Table 4), p.11 (Table 5), p.12 (Table 6).
Hardware: "4 × 3.2GHz CPU, 8 GB memory and average disk data rate of 146.2 MB/s" (microbenchmark setup, PDF p.16). Threads: not stated. Software: TritanDB, version not stated.
Data: SRBench weather (647k rows, seconds, regular), Shelburne agriculture (12.4M rows, "Timestamps are recorded up to nanosecond precision", 10s median interval), GreenTaxi (4.4M rows, seconds, "unevenly-spaced").

TIMESTAMPS, Table 5, "Compressed size of timestamps and values in datasets with varying methods", printed p.1:11. Unit MB. Bits/ts derived as MB*8e6/rows, assuming MB=1e6 bytes (not stated).
| Dataset | Precision | δΔ (DoD) | Delta-RLE-LEB128 | Delta-RLE-Rice | Delta raw | Derived DoD bits/ts |
|---|---|---|---|---|---|---|
| SRBench | s | 0.6 | 0.5 | 0.4 | 5.2 | ~7.4 |
| Shelburne | ms | 8.0 | 18.3 | 13.6 | 99.5 | ~5.2 |
| Shelburne | ns | 35.9 | 56.2 | 44.1 | 99.5 | ~23.2 |
| GreenTaxi | s, uneven | 4.0 | 6.9 | 1.5 | 35.5 | ~7.3 |
Quotes: "Delta-of-delta compression, δ∆, performs well on high precision, milli and nanosecond timestamps" (Sec. 4.2.1, p.1:10-11). "For high precision timestamps (e.g. in nanoseconds), where deltas themselves are large however, delta-of-delta compression that stores the difference between deltas can often be more effective."
Format: ns variant: first delta "44 bits for nanoseconds precision" in a 4-hour block; buckets '10' 7 bits, '110' 24 bits, '1110' 32 bits, '1111' 64 bits (Sec. 4.2.1).
Applicability: the only same-data ms-vs-ns DoD measurement found. Shelburne is REGULAR (10 s median) with jitter, δMAD 0, δIQR 293k; ITCH is irregular, so this is a lower bound on the problem, not a tick figure. Moving from ms to ns on the same timestamps multiplied DoD size by 4.5x (35.9/8.0, derived). DoD beat delta+Rice only where delta jitter was dominant; on the uneven dense seconds data (GreenTaxi) Rice beat DoD 2.7x.

VALUES, same Table 5 (MB): Shelburne Gorilla 440.8, FPC 419.3, DoD 426.6, raw 597.4; GreenTaxi 342.1 / 317.1 / 318.8 / 710.9; SRBench 8.2 / 23.8 / 21.9 / 33.9.
Times, Table 6 (100 attempts; s): compress Shelburne Gorilla 30.68, FPC 42.02, DoD 40.91; decompress 3.80 / 4.57 / 5.42.

## NP6. Elf+, "Erasing-based lossless compression method for streaming floating-point time series" (arXiv:2306.16053v1, 28 Jun 2023). PREPRINT as cached
URL: https://arxiv.org/abs/2306.16053 (cached: CACHE-D/elfplus/). Table 3, PDF p.16.
Hardware: i5-11400 2.60GHz, 16GB. Threads: not stated. Counted: ratio = compressed/original (lower is better), block of 1,000 values.
| Set | Elf+ ratio | Compress, µs, average per 1,000-value block |
|---|---|---|
| Stocks-UK | 0.19 | 40 |
| Stocks-USA | 0.18 | 39 |
| Stocks-DE | 0.23 | 43 |
Applicability: context. Doubles, XOR family with decimal erasing.

## NP7. SElf* / Elf*, "Adaptive Encoding Strategies for Erasing-Based Lossless Floating-Point Compression" (arXiv:2308.11915v2, 14 Sep 2023). PREPRINT as cached
URL: https://arxiv.org/abs/2308.11915 (cached: CACHE-D/selfstar/). Table IV, PDF p.10. Hardware as NP6, JDK 1.8. Threads: not stated.
Counted: ratio = compressed/original, lower is better (Sec. VI settings). Times are "average measurements of one block", 1,000 consecutive records, up to 100 blocks. Confirmed on a render of PDF p.10.
| Method | Ratio SDE / SUK / SUSA | Compress µs per block SDE / SUK / SUSA | Decompress µs per block SDE / SUK / SUSA |
|---|---|---|---|
| Gorilla | .717 / .577 / .678 | 19 / 19 / 19 | 20 / 18 / 20 |
| Chimp | .670 / .519 / .640 | 25 / 27 / 25 | 24 / 22 / 23 |
| Chimp128 | .271 / .286 / .230 | 27 / 30 / 26 | 19 / 20 / 18 |
| Elf | .262 / .219 / .240 | 58 / 54 / 57 | 43 / 35 / 44 |
| Elf+ | .229 / .193 / .184 | 43 / 46 / 39 | 33 / 30 / 32 |
| SElf* | .197 / .170 / .159 | 51 / 50 / 46 | 33 / 28 / 29 |
| Elf* | .191 / .153 / .144 | 69 / 69 / 61 | 33 / 28 / 29 |
| Xz | .192 / .161 / .171 | 1277 / 1190 / 1238 | 152 / 139 / 137 |
| Zstd | .265 / .221 / .240 | 188 / 160 / 161 | 30 / 31 / 30 |
| Snappy | .345 / .316 / .323 | 104 / 82 / 82 | 25 / 25 / 25 |
Derived: Gorilla decode 20 µs per 1,000 values is 20 ns/value on the Elf harness, ten times the 2 ns/point in P11 Fig. 10(c).
Applicability: context. Best XOR-family stock ratio found; still doubles.

## NP8. Kuffo Rivero MSc thesis (2023). THESIS. ONLY PER-STOCK SPEED ROWS FOR ALP FOUND
URL: https://homepages.cwi.nl/~boncz/msc/2023-KuffoRivero.pdf (cached: CACHE-D/kuffo-thesis/). Table 5.4, printed p.91, PDF p.105. Text layer read. Hardware: Intel Ice Lake, AVX512, model 8375C, 3.5 GHz (Table 5.1, PDF p.101); "CPU turbo-scaling features were disabled when available" (PDF p.107, printed p.93). Memory and storage: not stated in the table. Threads: not stated for Table 5.4.
Unit: tuples per cycle, compression then decompression. Higher is better.
| Set | ALP | Chimp | Chimp128 | Elf | Gorilla | PDE | Patas | Zstd |
|---|---|---|---|---|---|---|---|---|
| Stocks-DE comp | 0.507 | 0.030 | 0.051 | 0.009 | 0.029 | 0.002 | 0.059 | 0.017 |
| Stocks-DE decomp | 1.771 | 0.024 | 0.051 | 0.009 | 0.032 | 0.424 | 0.175 | 0.054 |
| Stocks-UK comp | 0.505 | 0.037 | 0.044 | 0.010 | 0.048 | 0.002 | 0.058 | 0.016 |
| Stocks-UK decomp | 1.786 | 0.044 | 0.045 | 0.015 | 0.057 | 0.466 | 0.170 | 0.046 |
| Stocks-USA comp | 0.505 | 0.043 | 0.061 | 0.012 | 0.045 | 0.003 | 0.058 | 0.020 |
| Stocks-USA decomp | 1.803 | 0.030 | 0.061 | 0.011 | 0.045 | 0.416 | 0.169 | 0.052 |
Applicability: strong context. Stock doubles decode at about 1.8 tuples/cycle under ALP against 0.045 for Gorilla on Stocks-USA (40x, derived). Same work as P4 but per-dataset.

---

# VENDOR / PROJECT PUBLISHED

Not peer-reviewed. Hardware, threads and data are usually not stated; each entry says so. None is a target.

## V1. Prometheus
- "Prometheus stores an average of only 1-2 bytes per sample." https://prometheus.io/docs/prometheus/latest/storage/ (cached: CACHE-D/tsdoc-prom/storage.html). Bytes per sample. Hardware, data, version: not stated.
- Timestamp buckets widened for millisecond input: "Gorilla has a max resolution of seconds, Prometheus milliseconds."; buckets of 14, 17, 20, 64 bits; comment says the alternative would "needlessly jump to large bit sizes". https://raw.githubusercontent.com/prometheus/prometheus/main/tsdb/chunkenc/xor.go, lines ~186-208 (cached: CACHE-D/tsdoc-prom-xor/xor.go.txt).
- Chunk format doc: varbit_xor 1-77 bits, varbit_ts 1-68 bits (cached: CACHE-D/tsdoc-prom-chunks/chunks.md).
Applicability: direct on design only: a production system had to retune DoD buckets when resolution rose by 10^3. ITCH rises by a further 10^6 over milliseconds.

## V2. ClickHouse
https://clickhouse.com/docs/reference/statements/create/table/codec (cached: CACHE-D/tsdoc-ch-codec/codec.html). No numeric benchmark in the docs.
- DoubleDelta: "Optimal compression rates are achieved for monotonic sequences with a constant stride, such as time series data."; "Uses 1 extra bit for 32-bit deltas: 5-bit prefixes instead of 4-bit prefixes."
- Gorilla: "the slower the values of the series changes, the better the compression rate".
- GCD: "columns with values that change (increase or decrease) in multiples of the GCD". Relevant because ITCH prices are usually multiples of 100.
- Source comment, CompressionCodecDoubleDelta.cpp (cached: CACHE-D/tsdoc-ch-dd): "best used against monotonic integer sequences with constant (or almost constant) stride"; prefixes 10/110/1110/11110/11111 with 7/9/12/32/64 bits.
Applicability: design context. The vendor's own stated optimum (constant stride) is not ITCH's shape.

## V3. TimescaleDB / Tiger Data
https://www.tigerdata.com/blog/time-series-compression-algorithms-explained, published 2020-04-22 per page metadata (cached: CACHE-D/tsdoc-tiger/page.html). "data collection happens at regular intervals"; "This compresses a full timestamp (8 bytes = 64 bits) down to just a single bit (64x compression)."
Applicability: illustrative best case, stated for regular intervals. No measured data. Says nothing on irregular timestamps.

## V4. DuckDB
- 0.6.0 announcement, https://duckdb.org/2022/11/14/announcing-duckdb-060.html (cached: CACHE-D/tsdoc-duck-060/page.html): city temperature doubles, uncompressed 25.4 MB, Chimp 9.7 MB, Patas 10.2 MB; Patas "almost matching uncompressed data in read speed". Hardware and thread count: not stated.
- PR #9635, https://github.com/duckdb/duckdb/pull/9635 (cached: CACHE-D/tsc-duckdb-pr9635/pr.json; opened 2023-11-10, merged 2024-01-22, author lkuffo): 41 data sets; "ALP achieves in average a x4.3 compression ratio. Compared to x2.1 and x2.4 of Patas and Chimp respectively."; SUM x1.4 slower than uncompressed, Patas x3.7, Chimp x7.7; "x2-4 times faster than Patas" at decompression. Hardware: not stated in what was read. No stock row.
- Lightweight compression post, 2022-10-28 (cached: CACHE-D/tsdoc-duck-light/page.html): whole-database sizes, for example Taxi 15.3 GB in 0.2.8 to 4.8 GB on dev. Not float-specific.
Applicability: context. Shows an engine shipping both XOR variants and then ALP.

## V5. VictoriaMetrics (Valialkin, 2019-05-20)
Via Wayback copy of faun.pub (cached: CACHE-D/tsdoc-vm-wayback/page.html). "Gorilla gives 3x-8x compression for typical time series data"; floats "may be converted into integer series by multiplying by 10^N"; "0.4 bytes per data point"; "10x times better than 4 bytes per data point for the same data in Prometheus". Data: node_exporter. Includes a general-purpose stage after the integer step, so the win is not attributable to the integer step alone. Hardware: not stated.
Applicability: vendor same-data claim for float-to-integer beating Gorilla-family coding, monitoring data.

## V6. M3 (Uber)
https://m3db.io/docs/architecture/m3db/engine/ (cached: CACHE-D/m3-engine/engine.html): "compression ratio of 1.45 bytes/datapoint with Uber's production workloads. This was a 40% improvement over standard TSZ, which only gave us a compression ratio of 2.42 bytes/datapoint". Source m3tsz.go (cached: CACHE-D/tsdoc-m3-int/m3tsz.go.txt): `DefaultIntOptimizationEnabled = true`, `maxMult = uint8(6)`, `convertToIntFloat` tries multipliers up to 10^6.
Applicability: a production system converts floats to scaled integers before coding. The doc does not attribute the 40% solely to this step. Hardware: not stated.

## V7. Tick-data blog (WEAK)
Parth Khare / AlphaBots, dev.to, "Posted on May 10", year not shown, https://dev.to/parth_khare_84da5090de191/how-we-compressed-635-gb-of-financial-tick-data-to-55-gb-56dm (cached: CACHE-D/tsc-devto-tick/post.html). Delta plus bit-packing plus ZSTD. Nifty historical ~15M rows, 63.5 GB to 5.5 GB, "91.6% smaller" than Parquet zstd; EQY US ALL BBO 8.8M rows, 118.92 MB to 30.09 MB, "74.7% smaller". Hardware and versions: not stated.
Applicability: the only item found on US equity BBO. Unverifiable, and ~15M rows for 63.5 GB is implausible per row. Do not use as a target.

## V8. Beringei (see G7 under P1)
"a single machine is able to compress more than 1.5 million datapoints/second". The only Gorilla-implementation throughput from Facebook's own side.

---

# CONTRADICTIONS BETWEEN SOURCES (none resolved, all left visible)

1. Gorilla paper: Fig. 5 (59.06/28.30/12.64, 39.6 b) vs its prose (51/30/19, 36.9 b). See P1.
2. Chimp vs Gorilla on stocks: Chimp's Table 3 has Chimp ahead (34.67 vs 36.43, Stocks-USA); DeXOR has Chimp behind (49.52 vs 43.85); VIREL has Chimp ahead on all three sets by a small margin. See P2, P7, NP1. Unexplained.
3. Gorilla on Stocks-USA: 36.43 b/v (Chimp), 37.7 (ALP), ~44 (Elf, up to 100 blocks), ~43 derived (VIREL, 1,024-value pages). Differences follow sample and block size; do not rank across studies.
4. Stocks value counts: ALP 43.6M / 59.3M / 282.1M vs Chimp 45.4M / 115.1M / 374.4M (DE/UK/USA). Not explained.
5. Stocks-UK decimals: 1 in Chimp, 2 in ALP. VIREL states ticks of 0.1 and 0.05.
6. DeXOR Table 4 gives ALP 52.97 as an aberrant value. See P7.
7. Gorilla decode 2 ns/point (P11) vs 20 ns/value (NP7, derived). See P11.

---

# NOT FOUND

| Item | Where I looked | Result |
|---|---|---|
| Any published DoD or price-compression number on ITCH | Digest queries plus a "Nasdaq TotalView ITCH columnar" search | None. tsc-tickfold README says "Status: early development", no results |
| Bits per timestamp on truly irregular ns exchange timestamps | Gorilla, Prometheus, ClickHouse, Tiger, BTrDB, TritanDB, DeXOR, searches | None. Closest: TritanDB Shelburne ns (regular with jitter) |
| Peer-reviewed XOR vs integer delta on tick-level exchange prices | BOS, BUFF, IoTDB, BtrBlocks, Matt et al. | None at known tick level. BOS Nifty-Stocks has unknown granularity; BUFF Stock is unlabelled and has no printed bar values |
| Whether INFORE Stocks-* as used by Chimp/ALP are tick level | Chimp, ALP, Zenodo record 3886895 (`datacite.json`, record page) | Field list not described. "exchange price data" is all Chimp says. The record is "Financial data set used in INFORE project", issued 2020-06-09 |
| Gorilla per-core encode/decode rate | All 12 pages of the paper | Not stated. Only Beringei README |
| Camel primary PDF (DOI 10.1145/3698802) | dl.acm.org returned HTML, cached as tsc-camel/3698802.pdf | Not opened. No Camel number recorded |
| Kangaroo | rmarcus listing only | Primary not opened |
| Altinity ClickHouse codec numbers | curl returned a Cloudflare block (tsc-altinity). WebFetch gave a summary only | Not confirmed; recorded as no evidence |
| ClickHouse first-party codec benchmark numbers | ClickHouse docs and source comment | No numbers |
| Tiger statement on irregular timestamps | tsdoc-tiger page | None |
| DuckDB version where ALP became default | PR #9635, DuckDB pages | Version not found. "default" seen only via a WebFetch summary of duckdb.org/library/alp, not cached |
| LeCo (SIGMOD 2024) | Search hit https://people.iiis.tsinghua.edu.cn/~huanchen/publications/leco-sigmod24.pdf | Not opened |
| VLDB Journal 2024 IoTDB encoding paper | Search hit | Not opened |
| IoTDB box-plot values, BUFF Fig. 8 values | Rendered figures | No printed values; ordering only |
| FCBench stock rows | Cached paper | No stock data; only gas price |
| AMMMO | Cached paper | No timestamp or Gorilla content used |
| "BOS has replaced Bit-packing in IoTDB" | Search summary | Not confirmed in primary text, so not claimed |
| Beyond Compression per-stock numbers | Paper text | Box plots only |
| Peer-review status of Elf+ and SElf* | arXiv copies. Crossref query rate-limited (HTTP 429) | Listed as preprint as cached; the journal version may exist |
