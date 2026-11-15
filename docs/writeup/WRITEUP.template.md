# tick-store: one day of Nasdaq ITCH in a columnar store

On one trading day of Nasdaq TotalView-ITCH 5.0, {{ss_records}} messages in {{ss_source_gb}} GB, tick-store
keeps a store {{ss_ratio}} times smaller than the feed and smaller than Parquet files or the
ArcticDB and QuestDB time-series databases, and gives every message back bit-exact. Keeping each
stock's records together makes queries about one stock faster than in the other systems' default
setups, though not than ArcticDB with a table per stock, and short windows across every stock slow,
because each must decode a whole segment's positions to put records back in file order. Gorilla,
the codec Facebook published for time series, {{verdict:gorilla_bits}} to plain integer codecs in
decode time and in size on all but one near-empty column, and the plainest miss is the single-threaded writer, which on the older test machine ingests below
Nasdaq's peak published message rate.

## What was measured, and against what

**Data and correctness.** The reference day is 30 December 2019, from Nasdaq's public sample files;
a second day, 30 October 2019, serves only the round trip and the bit-flip check. The round trip
re-encodes every stored record, compares it with the source byte for byte, and requires the
SHA-256 of the rebuilt stream to equal the source's: {{verdict:g1}} on the reference day on both
machines below, and on the second day on the laptop. A flipped bit in a data block and in the
footer is caught with block checksums on and off ({{verdict:g2}}), and every parser and decoder is
fuzzed for {{code:fuzz_required_minutes}} minutes under AddressSanitizer and UndefinedBehaviorSanitizer ({{verdict:g3}}). The last runs found two faults, both in the test
harness (near the end).

**Hosts.** Every pinned figure comes from a 2018 embedded 4-core Zen 1, an AMD Ryzen V1605B with
4 MB of L3 and 15 GB of memory, running Linux: "the Zen 1" below. Its AVX2 splits each 256-bit
operation in two. Each benchmark is pinned to one core and every thread of the comparison to two; the
first core's hyperthread sibling is not kept idle, and a benchmark is refused if the sibling was
over {{code:sibling_busy_limit_pct}}% busy. Context figures come from an Apple M4 Max laptop, "the M4": unpinned, no hardware counters.

**Clock.** The Zen 1 ran with frequency boost on and no fixed clock, and the comparison recorded no
clock while it ran, so systems timed hours apart may have run at different clocks. The Zen 1 is
shared with other work and its settings were not mine to change, so no run had boost off, and what it
costs is not measured. The spread it could hide in is a coefficient of variation of
{{cdL_cv_min}}% to {{cdL_cv_max}}% in the benchmarks, and (slowest − fastest) / median of
{{cmpL_ts_warm_spread_min}}% to {{cmpL_ts_warm_spread_max}}% warm and {{cmpL_ts_cold_spread_min}}%
to {{cmpL_ts_cold_spread_max}}% cold in tick-store's queries; each write ran {{cmpL_ts_write_reps}}
time(s). Cycle counts move less with the clock, but a memory wait costs more cycles at a higher one.

**The targets.** Nothing published measures a columnar store of ITCH, so each speed and size target
is a figure published for something near it, in a paper, a vendor benchmark or a mailing-list post,
almost all on other data and most on newer processors; the rest ask to beat the other three systems
on the same machine. [BENCHMARKS.md](BENCHMARKS.md) gives every source. I chose to keep each
target as published, so a gap is the distance to that figure, not a defect in itself.

**The comparison.** Parquet (a columnar file format, written by pyarrow), ArcticDB (Man Group's
DataFrame database for time series) and QuestDB (a time-series SQL server) each ran its default
setup, fixed in advance, plus any other setup that won a query on a
five-minute slice. The queries are one second of every stock at the open (Q1), one hour of one
stock (Q2), its per-minute aggregate (Q3), one millisecond of every stock (Q4) and the day's sum of
one shares column (Q5), each run {{cmpL_warm_runs}} times warm, proven to read nothing from storage, and {{cmpL_cold_runs}} times
cold, proven evicted. Writes are timed in total and engine-only, leaving out reading and converting
the input. An unflushed write returns once data is in the page cache, a flushed one after a proven
flush to the device.

Every figure is filled in by `scripts/render_writeup.py` from the results file and field that the [figure
list]({{path:figures}}) names for it, and a paragraph's link is to the file most of its figures
come from; the script does not check that the field it reads is the one the sentence describes.

## Data layout

**The file, in bytes.** A store is one container file: a {{code:container_header_bytes}}-byte header (magic, version, header
size), the blocks' payloads back to back, a footer, and a {{code:container_trailer_bytes}}-byte trailer holding the footer's
offset and CRC32C ([container.hpp](include/tick_store/store/container.hpp)). The footer opens with
{{code:footer_counts_bytes}} bytes of counts, then gives each block a {{code:block_entry_bytes}}-byte entry: payload offset and metadata offset ({{code:block_offset_field_bytes}}
bytes each), payload size and metadata size ({{code:block_size_field_bytes}} each), the payload's CRC32C ({{code:block_crc_bytes}}), the codec id ({{code:block_codec_bytes}}) and
{{code:block_reserved_bytes}} reserved bytes. Each named section has a {{code:section_entry_bytes}}-byte entry, and a metadata area gives each block {{code:block_metadata_bytes}}
bytes: segment, column, piece and value count. Each segment's first block adds a summary: first
record, first, last, lowest and highest timestamp, record counts, and a bitmap of the stocks
present, up to {{code:locate_bitmap_max_bytes}} bytes. Opening a file reads its two ends, and a time or stock filter reads the
footer alone.

**Segments and stored order.** Records are cut into segments of 2^{{code:segment_records_log2}} messages, {{ss_segments}} on the day, and a
segment decodes from its own blocks alone ([docs/store.md](docs/store.md)). Inside a segment,
records are sorted by stock locate, the day's 16-bit number for each stock, and kept in file order
within each stock, so one stock's records are one run of each of the {{code:column_count}} columns and a one-stock
query reads one range per column. The `locate` column holds each run's positions in file order,
which is how file order comes back; a window across every stock must decode it whole. Columns are
cut into pieces of {{code:piece_values}} values, each one container block, the unit read, checked and, where zstd
wraps it, inflated. A piece holds {{code:piece_codec_blocks}} codec blocks of {{code:column_block_values}} values, each opening with an {{code:block_frame_bytes}}-byte
frame (value count, payload bytes) and carrying its own first value, so it decodes alone.

**Where the bytes go.** Of the store's {{ss_stored_bytes}} bytes, timestamps take {{ss_ts_mb}} MB,
{{ss_ts_share}}% of the column payloads; then come the locate column ({{ss_locate_mb}} MB),
new-order references ({{ss_creation_ref_mb}} MB), references to existing orders
({{ss_ref_distance_mb}} MB) and message types ({{ss_type_mb}} MB)
([store sizes]({{path:ss}})).

**References as distances within the stock.** An execute, cancel, delete or replace names an order
by a 64-bit reference. The best codec measured needs {{ce_referenced_ref_bits}} bits for it in file
order, {{ce_ref_distance_min_bits}} to {{ce_ref_distance_max_bits}} as a distance back in the day's
orders, and {{ce_ref_locate_distance_min_bits}} to {{ce_ref_locate_distance_max_bits}} as a
distance back in the stock's own list, so I chose the last
([codec evaluation]({{path:ce}})). Distance 0 means "the next import", a full
reference to an order created before the segment, which keeps segments independent: the day holds
{{ss_imports}} imports, and distances cost {{ss_ref_distance_bits}} bits as built. A per-segment
book checkpoint was costed far larger and dropped; that costing is in no results file.

**Prices as integers, per stock.** ITCH prices are integers in units of 10^-4 dollars. The store
groups add prices by stock and side and codes them with PFOR delta (patched frame of reference on
the deltas: one bit width per vector, values too wide kept in a patch list), trying each
block at a few scales from its own divisors and keeping the smallest. The best codec needs
{{ce_add_price_best_bits}} bits per price in file order, {{ce_add_price_pl_best_bits}} per stock and
{{ce_add_price_pls_best_bits}} per stock and side; as built, prices cost {{ss_add_price_bits}}. The
per-stock figures leave out the sequence that restores file order, at least
{{ce_perm_add_locate_side_bits}} bits per add by its order-0 entropy, which the store pays once, in
the `locate` column, at {{ss_locate_bits}} bits per record.

**Timestamps: delta, not delta-of-delta.** ITCH timestamps do not tick at regular intervals, the
case delta-of-delta is built for: the ideal code for a delta in file order is
{{ce_ts_ideal_delta_bits}} bits, for a delta-of-delta {{ce_ts_ideal_dod_bits}}. PFOR delta reaches
{{ce_ts_best_bits}}, patching saving {{ce_ts_patch_saving_bits}} bits. Per stock, each delta spans
the other stocks' messages in between, and PFOR patches {{ce_tspl_pfor_exception_pct}}% of values.
The store keeps timestamps per stock as residuals from a `time_base` column, the lowest time of
each {{code:time_base_records}} records in file order; with the bases they miss their target (Missed targets).

**Segment size, the one layout choice measured on the full day.** The comparison ran 2^{{cmpL_seg1_log2}}-record
segments beside the default 2^{{code:segment_records_log2}} ([RESULTS.md](RESULTS.md#comparison) has both). Larger segments
make each stock's run four times longer, so Q2 touches {{cmpL_ts_q2_segments}} segments instead of
{{cmpL_seg1_q2_segments}} and the store is smaller (2^{{cmpL_seg1_log2}} takes {{cmpL_seg1_apparent_bytes}} bytes);
at 2^{{cmpL_seg1_log2}} Q2 slows to {{cmpL_seg1_q2_warm_med_ms}} ms warm. But a window across every stock decodes
the segment's whole `locate` column: {{cmpL_ts_q4_values}} values for Q4's {{cmpL_q4_rows}} rows at
2^{{code:segment_records_log2}}, {{cmpL_seg1_q4_values}} at 2^{{cmpL_seg1_log2}}. So Q4 takes {{cmpL_ts_q4_warm_med_ms}} ms warm at 2^{{code:segment_records_log2}} vs
{{cmpL_seg1_q4_warm_med_ms}} at 2^{{cmpL_seg1_log2}}, and Q1 {{cmpL_ts_q1_warm_med_ms}} vs
{{cmpL_seg1_q1_warm_med_ms}}. I chose 2^{{code:segment_records_log2}} by a scoring rule committed before the layout experiments
(points for figures within 5% of the best, among stores under three quarters of Parquet's size): it
won Q2, Q3 and Q5 and gave up Q1 and Q4. Only these two sizes were timed under the measurement
wrapper.

## Codecs, and whether Gorilla lost

Gorilla was built as published, delta-of-delta for timestamps and XOR of successive doubles for
values, and is exact on the whole day. The one column where it is smaller is `mwcb_level`, with
{{ce_mwcb_values}} values a day: {{ce_mwcb_gorilla_bytes}} bytes vs {{ce_mwcb_best_bytes}}
([codec evaluation]({{path:ce}})).

| Column and order | Gorilla, bits per value | Gorilla over the best integer codec |
|---|---|---|
| Timestamps, file order | {{ce_ts_gorilla_bits}} | {{ce_ts_gorilla_over_best}}x |
| Timestamps, per stock* | {{ce_tspl_gorilla_bits}} | {{ce_tspl_gorilla_over_best}}x |
| Add prices, file order, dollar doubles | {{ce_price_xor_dollars_bits}} | {{cr5_add_file_dollars}}x |
| Add prices, file order, integer-valued doubles | {{ce_price_xor_integer_bits}} | {{cr5_add_file_best}}x |
| Add prices, per stock*, integer-valued doubles | {{ce_price_pl_xor_integer_bits}} | {{cr5_add_pl_best}}x |

\* Leaves out the cost of the sequence that restores file order.

On prices the double is what costs Gorilla: a price with cents has no exact binary form, so its
mantissa is full and successive prices XOR to many significant bits, and an integer tick count
stored as a double removes that. The price-ratio target is {{verdict:cr5_over_dollars}} over XOR on
dollar doubles on all six price cases, and {{verdict:cr5_over_best_file}} over the best Gorilla
variant in file order.

In decode time Gorilla takes {{cdM_ds6_ts}} times as long as the store's codecs on timestamps on the M4 and
{{cdM_ds6_price}} times on prices, and on the Zen 1 {{cdL_ds7_ts}} and {{cdL_ds7_price}} times
([M4 column decodes]({{path:cdM}})); the M4's target is {{verdict:ds6_ts}} on timestamps and
{{verdict:ds6_price}} on prices.

**What the writer chooses.** Each column has a fixed codec family: PFOR (plain, delta, head delta,
scaled), a dictionary, run-length, raw bytes, and zstd over a piece where smaller
({{ss_add_price_zstd_blocks}} of {{ss_add_price_blocks}} price pieces). On the eight hottest columns
I chose ranked PFOR, dictionary codes ranked by frequency and packed with PFOR, which decodes faster
than a Huffman dictionary for a few more bytes. The store as built misses three size targets that a codec meets
in evaluation; whether it pays for the stock order, the block size or the codec choice has not been
separated.

## Decode: kernels, SIMD, and what it cost

**The vector layout.** A full PFOR vector is {{code:vector_values}} values at one bit width in {{code:lanes}} lanes of 64-bit
words, value i in lane i mod {{code:lanes}}, so a row of {{code:lanes}} consecutive values unpacks from {{code:lanes}} consecutive words
with one shift ({{code:neon_registers_per_row}} NEON or {{code:avx2_registers_per_row}} AVX2 registers) and no transpose. At width w the packed values take
{{code:vector_bytes_per_width}} × w bytes, after a {{code:pfor_vector_header_bytes}}-byte header (mode, width, base length) and the base, and a {{code:pfor_exception_count_bytes}}-byte exception
count follows ([pfor.hpp](include/tick_store/codec/pfor.hpp)). Deltas stay in value order, so the
inverse delta is a prefix sum across each row's lanes; FastLanes, a layout for vectorised integer
decoding from the Dutch institute CWI, transposes values to make it an add down each lane.

**Kernel sets.** Scalar, NEON and AVX2 sets give identical values. The best set the processor runs
is chosen once, from CPUID on x86-64, and `TICK_STORE_SIMD` can cap it. The AVX2 set avoids PEXT,
PDEP and gathers, which are slow on Zen 1. What the store's reader calls:

| Kernel | Scalar | NEON | AVX2 |
|---|---|---|---|
| `unpack_vector`, a full interleaved vector | store | store | store |
| `unpack_linear`, tails and patch lists | store | store | store |
| `add_base`, frame of reference | store | store | store |
| `delta_pass`, second pass of a two-pass delta | store | store | store, for tails and patched vectors |
| `unpack_delta`, unpack and delta fused | bench, tests | bench, tests | store, for unpatched delta vectors |
| `unpack_delta_narrow`, `prefix_sum`, `svb_decode` | bench, tests | bench, tests | bench, tests |
| `delta_of_delta_decode` | never: the writer writes no delta-of-delta | | |

**What SIMD bought.** On the M4, NEON over scalar, medians over widths: linear unpack
{{skM_linear_speedup}}x, two-pass delta {{skM_two_pass_speedup}}x, fused delta
{{skM_fused_speedup}}x (not called on NEON), interleaved unpack {{skM_unpack_speedup}}x (clang had
already vectorised the scalar loop), frame-of-reference add {{skM_for_add_speedup}}x, and Stream
VByte, a byte-aligned codec the store never writes, {{skM_svb_speedup}}x ([M4 kernels]({{path:skM}})). On the Zen 1, AVX2
over scalar: interleaved unpack {{skL_unpack_speedup}}x, two-pass delta {{skL_two_pass_speedup}}x,
fused {{skL_fused_speedup}}x. A delta-of-delta reconstruct target is {{verdict:ds8}} at
{{skM_dod_gbps}} GB/s, by a kernel the store never calls.
<!-- CUT-OR-CAUSE (may never exist: Zen 1 kernel file): replace the Zen 1 sentence with "There is
no Zen 1 kernel figure: every complete kernel run there was refused by the sibling rule." -->

On queries, the same Zen 1 binary at AVX2 vs capped to scalar changes the warm engine median by
{{d3_simd_q1_pct}}%, {{d3_simd_q2_pct}}%, {{d3_simd_q3_pct}}%, {{d3_simd_q4_pct}}% and
{{d3_simd_q5_pct}}% on Q1 to Q5 ([counters run]({{path:d3}})). Two parts of the path have no vector
form: Q5 sums share columns held as ranked dictionary codes, and the time-base add (each time the
larger of its base and the previous time, plus a residual) is a max-plus recurrence.
<!-- CUT-OR-CAUSE (D3): replace the first sentence with "Its effect on the Zen 1 queries was not
measured." -->

**What SIMD cost.** {{simd_src_lines}} lines of kernels and dispatch, {{simd_src_share}}% of the
{{src_include_lines}} in `src/` and `include/`; a {{simd_bench_lines}}-line kernel benchmark and
{{simd_test_lines}} lines of kernel tests; fuzz targets that compare AVX2 with scalar; and
{{ds4_attempts_refused}} complete Zen 1 kernel runs refused by the sibling rule.

**The store's price column.** Per piece, the reader inflates any zstd frame, allocates, decodes
PFOR with scale and patches, range-checks every value and appends. On the Zen 1 those steps take
{{d4_zstd_pct}}%, {{d4_pfor_unpack_pct}}% (unpack), {{d4_patches_pct}}% (patches),
{{d4_scale_range_pct}}% (scale and range check) and {{d4_alloc_copy_pct}}% (allocation and copy) of
the decode's user cycles ([price-decode profile]({{path:d4}})).
<!-- CUT-OR-CAUSE (D4): replace the last sentence with "The steps' shares were not separated." -->

## Branch prediction and instruction counts

Gorilla's decode is a chain of data-dependent branches: each value's control bits decide how many
bits to read next. On the Zen 1, counting user mode in the timed passes only, Gorilla held in L1
runs {{cdL_gorilla_l1_instr}} instructions per price at {{cdL_gorilla_l1_ipc}} per cycle, so
instruction count binds it, with {{cdL_gorilla_l1_bm}} mispredicts per value on top; the cycles a
mispredict costs were not measured, so none of the time is put on them. On the whole column it takes
{{cdL_gorilla_xor_cycles}} cycles, {{cdL_gorilla_xor_instr}} instructions and
{{cdL_gorilla_xor_bm}} mispredicts per price, the store's price column {{cdL_store_price_cycles}},
{{cdL_store_price_instr}} and {{cdL_store_price_bm}} at {{cdL_store_price_ipc}} IPC. Over prices
and timestamps, the store's codecs take {{ctr_bm_ratio_min}} to {{ctr_bm_ratio_max}} times fewer
mispredicts than Gorilla and {{ctr_instr_ratio_min}} to {{ctr_instr_ratio_max}} times fewer
instructions. PFOR delta on timestamps, at {{cdL_pfor_delta_ipc}} IPC, is not instruction-bound, and
with no memory counters collected nothing here says what binds it
([Zen 1 column decodes]({{path:cdL}})).

Parsing ITCH costs {{icL_parse_cycles}} cycles, {{icL_parse_instr}} instructions and
{{icL_parse_bm}} mispredicts per message, and parsing then re-encoding {{icL_codec_cycles}},
{{icL_codec_instr}} and {{icL_codec_bm}}, so encoding adds {{icL_encode_added_bm}} mispredicts
([ITCH codec benchmark]({{path:icL}})). The whole-day write runs at {{d3_write_ipc}} IPC with
{{d3_write_bm_per_msg}} mispredicts per message and {{d3_write_sys_pct}}% of its CPU time in the
kernel; whole query processes run at {{d3_query_ipc_min}} to {{d3_query_ipc_max}} IPC.
<!-- CUT-OR-CAUSE (D3): replace the last sentence with "The write's and the queries' own counters
were not collected." -->

## Memory

The writer holds one segment's messages, {{code:column_value_bytes}} bytes per column value, then sorts and codes them, so
its memory is bounded by the segment, not the day: a peak footprint of
{{rtM_ingest_footprint_mb}} MB on the M4 at 2^{{rtM_segment_log2}}, and at 2^{{cmpM_seg1_log2}} a peak resident set, which also
counts the mapped source, {{cmpM_seg_rss_diff_mb}} MB lower. The Zen 1's query worker peaks at
{{cmpL_tick_query_vmhwm_mb}} MB resident, mapped store pages included ([Zen 1
comparison]({{path:cmpL}})).
QuestDB's day store is larger than the Zen 1's memory, so warm queries run one at a time, and a
timed query during which a page was swapped in is kept and marked: for tick-store,
{{cmpL_ts_swapin_list}}.

## Writes and queries against three systems

**Size.** tick-store's flushed store takes {{cmpL_ts_B_alloc_gb}} GB on disk vs the smallest setup
of each other system: Parquet {{cmpL_parquet_min_B_alloc_gb}} GB, ArcticDB
{{cmpL_arctic_min_B_alloc_gb}} GB, QuestDB {{cmpL_questdb_min_B_alloc_gb}} GB
([Zen 1 comparison]({{path:cmpL}})).

**Writes.** tick-store parses the ITCH file inside its clock; the others
read columns exported from the same day and spend {{cmpL_others_read_share_min}}% to
{{cmpL_others_read_share_max}}% of their unflushed total doing so. Unflushed, in seconds total and
engine-only: tick-store {{cmpL_ts_A_total_s}} and {{cmpL_ts_A_engine_s}}, Parquet
{{cmpL_parquet_A_total_s}} and {{cmpL_parquet_A_engine_s}}, ArcticDB {{cmpL_arctic_A_total_s}} and
{{cmpL_arctic_A_engine_s}}, QuestDB {{cmpL_questdb_A_total_s}} and {{cmpL_questdb_A_engine_s}}.
QuestDB's flushed write is {{verdict:questdb_B_like_for_like}}: its flush proof on the day
{{verdict:questdb_B_proof}}. [RESULTS.md](RESULTS.md#comparison) has the flushed writes.

**Why the writer is slow.** It is single-threaded, and parsing is a small part of it: alone,
parsing runs at {{icL_parse_mps}} M messages a second on the slice in memory, about {{wr_parse_s}}
s of the engine-only write. Of the write's user cycles, PFOR encoding, a search over
widths and scales, takes {{d1_pfor_encode_pct}}%, dictionary encoding {{d1_dict_encode_pct}}%, book
state {{d1_book_state_pct}}%, segment assembly {{d1_segment_assembly_pct}}%, sorting
{{d1_sort_pct}}%, parsing {{d1_parse_pct}}% and zstd {{d1_zstd_pct}}% ([writer
profile]({{path:d1}})).
<!-- CUT-OR-CAUSE (D1): replace the last sentence with "Where the rest goes was not split; the
PFOR encoder codes each block at every candidate scale, which is the likeliest cost, but that is a
reading of the code, not a measurement." -->

**Queries.** Among the four default setups, tick-store has the lowest median and slowest run on Q2
and Q3, warm and cold ({{verdict:ql4_linux_q2q3}}), and on Q5 cold ({{verdict:ql4_linux_q5_cold}});
the next fastest takes {{cmpL_q2_warm_best_ms}} ms on Q2 warm and {{cmpL_q3_warm_best_ms}} ms on Q3.
ArcticDB set up with one table per stock is faster still, {{cmpL_arcps_q2_warm_med_ms}} and
{{cmpL_arcps_q3_warm_med_ms}} ms warm, but takes {{cmpL_arcps_q1_warm_med_s}} s on Q1. Across every
stock, {{cmpL_q1_warm_best_system}} answers Q1 in {{cmpL_q1_warm_best_ms}} ms warm and
{{cmpL_q4_warm_best_system}} Q4 in {{cmpL_q4_warm_best_ms}} ms (tick-store's are under Segment
size); on Q5 warm tick-store is {{verdict:ql4_linux_q5_warm}} ([RESULTS.md](RESULTS.md#comparison)).

## Tried and not kept

- **Gorilla, delta-of-delta, and other reference codings** (stateless, one creation list for the
  day, day-wide distances): each cost more than what the store uses.
- **SIMD where it could not help**: 32-bit lanes gained nothing.
- **Layout experiments**: a locate column in file order halved Q4 but cost too many bytes, and
  1,024-value blocks slowed Q5; these were iteration runs, so no figure is quoted.
- **Not built, each a step on a missed target**: a stored per-stock minute aggregate (Q3), a sum
  over ranked dictionary counts that unpacks nothing (Q5), checkpoints inside a run so a window can
  start near itself (Q1, Q4), and a second writer thread.

## Missed targets

On the Zen 1 unless the M4 is named, units as the bar's; "x" is how many times off the bar.

| Asked | Bar (source) | Measured | Gap | Why |
|---|---|---|---|---|
| Timestamps, store as built | {{bar:cr3}} bits (KX case study, NYSE quotes) | {{ss_ts_bits}}; per stock* {{ce_tspl_best_bits}} | {{gap_cr3_store}}; {{gap_cr3_pl}} bits | Bases add bits; per-stock deltas patched |
| Add prices, file order | {{bar:cr4}} bits (ALP paper, stock doubles) | in Data layout | {{gap_cr4_file}} bits | Stocks interleave; per stock {{verdict:cr4_pl}} |
| Integer vs best Gorilla, file order | {{bar:cr5}}x smaller (2025 paper, Indian stocks) | {{cr5_best_file_missed_min}}x to {{cr5_best_file_missed_max}}x | to {{gap_cr5_worst}} | Integer-valued doubles help Gorilla |
| References, store as built | {{bar:cr6}} bits (KX case study) | new orders {{ss_creation_ref_bits}}; imports {{ss_import_ref_bits}} | {{gap_cr6_creation}}; {{gap_cr6_import}} bits | Codec alone {{ce_creation_ref_best_bits}}; imports are full references |
| Shares | {{bar:cr7}} bits (KX case study) | executions, trades, imbalances {{ce_shares_missed_min_bits}} to {{ce_shares_missed_max_bits}}; store {{ss_add_shares_bits}}, {{ss_reduce_shares_bits}} | {{gap_cr7_min}} to {{gap_cr7_max}} bits | Coded without book state |
| Gorilla decode, M4 | {{bar:ds1}} ns (mailing-list post, smooth synthetic values) | {{cdM_ds1_ns}} ns | {{gap_ds1_x}}x | Real prices: {{cdM_gorilla_xor_bits}} bits per value |
| Gorilla decode in L1 | {{bar:ds2}} cycles (ALP paper, Intel Ice Lake) | {{cdL_gorilla_l1_cycles}} | {{gap_ds2_x}}x | Instruction count |
| Delta decode, M4 | {{bar:ds3}} G values/s (FastLanes, Apple M1) | {{skM_two_pass_gvps}} G | {{gap_ds3_pct}}% | {{skM_two_pass_widths_reaching}} of {{skM_two_pass_widths}} widths reach it; cross-lane sum not costed |
| Delta decode | {{bar:ds4}} cycles (FastLanes, Zen 3 server) | {{skL_fused_cycles}} | {{gap_ds4_x}}x | Not traced |
| Price-column decode | {{bar:ds5}} MB/s (ALP, one AMD EPYC 9554 core) | {{cdL_ds5_mbps}}; M4 {{cdM_ds5_mbps}} | {{gap_ds5_linux_x}}x; {{gap_ds5_mac_pct}}% | Whole column path |
| Store vs Gorilla decode, prices, M4 | {{bar:ds6}}x faster (third-party benchmark, Apple M2 Max) | in Codecs | {{gap_ds6_price}}x | Whole column path; steps under Decode |
| One-thread ingest | {{bar:wt1}} M records/s (QuestDB vendor figure, 32 workers) | {{cmpL_ts_A_rate_m}} M; M4 {{cmpM_ts_A_rate_m}} M | {{gap_wt1_linux_x}}x; {{gap_wt1_mac_x}}x | One thread; parsing {{wr_parse_pct}}% |
| Ingest above Nasdaq's peak rate | {{bar:wt2}} M messages/s (Nasdaq bandwidth report) | unflushed as above; flushed {{cmpL_ts_B_rate_m}} M | {{gap_wt2_linux_x}}x unflushed | One thread; M4 {{verdict:wt2_mac}} |
| Writes no slower than each system | same machine | in Writes | unflushed {{verdict:wt3_linux}}; flushed {{verdict:wt4_linux}} | Parsing and coding inside its clock |
| Q2 warm median | {{bar:ql1}} ms (DuckDB on a small Parquet store, preprint) | {{cmpL_ts_q2_warm_med_ms}}; M4 {{cmpM_ts_q2_warm_med_ms}} | {{gap_ql1_linux_x}}x; {{gap_ql1_mac_ms}} ms | {{q2_ns_per_row}} ns a row; dictionary decoding {{d2_q2_dict_pct}}% |
| Q3 warm median | {{bar:ql2}} ms (QuestDB vendor benchmark) | {{cmpL_ts_q3_warm_med_ms}}; M4 {{cmpM_ts_q3_warm_med_ms}} | {{gap_ql2_linux_x}}x; {{gap_ql2_mac_x}}x | Decodes an hour of adds; no stored aggregate |
| Q4 cold mean | {{bar:ql3}} ms (TimescaleDB, order-book preprint) | {{cmpL_ts_q4_cold_mean_ms}}; M4 {{cmpM_ts_q4_cold_mean_ms}} | {{gap_ql3_linux_x}}x; {{gap_ql3_mac_x}}x | Whole-segment positions; {{cmpL_ts_q4_cold_device_mb}} MB read cold |
| Lowest of four, Q1, Q4, Q5 warm | same machine | in Queries | missed | Q1, Q4 as above; Q5 unpacks {{cmpL_ts_q5_values}} values, {{q5_ns_per_value}} ns each |

\* Leaves out the cost of the sequence that restores file order.
<!-- CUT-OR-CAUSE (D2): if D2 does not exist, the Q2 row's why becomes "{{q2_ns_per_row}} ns a
row; shares not measured". CUT-OR-CAUSE (may never exist: Zen 1 kernel file): if the Zen 1 delta
target stays not judged, drop its row and add to the not-judged line "the Zen 1 delta decode, since
no kernel run passed the sibling rule". -->

Not judged: the store's speed-up over Gorilla on the Zen 1 and delta-of-delta decode with bit
parsing, which have no published figure, and the comparison targets on the M4, since the
four-system day ran on the Zen 1 only ([RESULTS.md](RESULTS.md#targets)).

## Two faults in the test harness

Every figure here was measured at commit `{{measured_commit}}`, except the fuzz result, judged at
`{{fuzz_commit}}`, which differs from it only in the fuzz harness.

**The fuzz oracle's work per input was unbounded.** It re-encodes every decoded value and compares,
and a {{fuzz_stopped_input_bytes}}-byte input can declare a million values. At the measured commit six of the seven fuzz
targets on the Zen 1 ran clean for the full run; the seventh stopped in its corpus replay, at {{fuzz_stopped_s}} s on
a generated seed, over a {{code:fuzz_input_timeout_s}} s per-input limit. Now every input is still decoded in full
under the sanitizers, the re-encode comparisons run in full up to {{fuzz_oracle_budget_values}}
values and on a prefix past it, and named maximal inputs keep the full check in a unit test. On a
larger input the fuzzer therefore never compares the values past the prefix. At the second commit
{{g3_mac_clean}} of {{g3_mac_targets}} targets ran clean on the M4 and {{g3_linux_clean}} of {{g3_linux_targets}} on the Zen 1, the shortest run
{{g3_shortest_s}} s ([fuzz results]({{path:fuzz}})).

**The bit-flip check moved its footer flip.** The check flips a bit at the middle byte of a data
block and of the footer. On 30 October 2019 the footer's middle byte is the last byte of a block
entry's CRC32C, which nothing reads with checksums off, so the flip moves one byte on, to the
entry's codec id. The script that plans the flips lacked that rule at the measured commit, and the
second day's check failed on it once; the rule is restored and the day checked again. The first
day's flips are unchanged.

## How the results were checked

`scripts/check.sh` runs warnings-as-errors builds, unit tests, sanitizers, clang-tidy, the fuzz
targets, coverage and the results validators, and each check is shown failing: a prover applies
each of {{proofs_listed}} faults in a throwaway clone and requires the named step to fail, while a
harmless control passes; the last full run proved {{proofs_proven}} of {{proofs_recorded}}
([prover record]({{path:proofs}})). The comparison's measures, voiding causes and spread
thresholds were fixed before any measured run.

The final review recorded {{review_rows}} findings, each fixed with a test seen failing first or
closed with its reason ([docs/REVIEW.md](docs/REVIEW.md)). A second pass found defects in the fixes:
the write verdicts had used the total clock alone, and judged on both clocks as promised, the
unflushed write went from beating one system to beating none.

## Limits

Performance comes from one pinned day, on one machine that also runs a backup job reading new
stores. Sizes on the M4 are file-system allocation, so sizes come from the Zen 1. Beyond boost, not
measured and not estimated: the cycles a mispredict costs and any memory-level counter.
