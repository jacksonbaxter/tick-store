# tick-store: one day of Nasdaq ITCH in a columnar store

On a trading day of Nasdaq TotalView-ITCH 5.0, {{ss_records}} messages in {{ss_source_gb}} GB,
tick-store keeps a store {{ss_ratio}} times smaller than the feed, takes the least disk of the
systems compared ({{verdict:cr2}}), and gives every message back bit-exact ({{verdict:g1}}).
Keeping each stock's records together makes an hour of a stock and its per-minute aggregate
quicker to query than in the other systems' fixed setups ({{verdict:ql4_linux_q2q3}}), while
windows across every stock are answered quicker by ArcticDB or QuestDB.
Gorilla, the codec Facebook published for time series, {{verdict:gorilla_bits}} to plain integer
codecs in size on every column but the near-empty `mwcb_level`, and the writer, on a single
thread, ingests below Nasdaq's peak published message rate on the older test machine
({{verdict:wt2_linux_a}} on the unflushed write).

## What was measured, and against what

**Data and correctness.** The reference day is 30 December 2019, from Nasdaq's public sample files;
a second day, 30 October 2019, serves only the round trip and the bit-flip check. The round trip
re-encodes every stored record, compares it with the source byte for byte, and requires the
SHA-256 of the rebuilt stream to equal the source's: {{verdict:g1}} on the reference day on both
machines, and on the second day on the laptop. A flipped bit in a data block and in the footer is
caught with block checksums on and off ({{verdict:g2}}), and every parser and decoder is fuzzed for
{{code:fuzz_required_minutes}} minutes under AddressSanitizer and UndefinedBehaviorSanitizer
({{verdict:g3}}).

**Hosts.** Every pinned figure comes from a Linux host whose processor is {{cap_cpu}}, made in
2018, with {{cap_mem_gib}} GiB of memory: "the Zen 1". Each benchmark is pinned to a core and every thread of the
comparison to a pair of them; the first core's hyperthread sibling is not kept idle, and a
benchmark is refused if the sibling was over {{code:sibling_busy_limit_pct}}% busy. Context
figures come from an Apple M4 Max laptop, "the M4": unpinned, no hardware counters.

**Clock.** The Zen 1 ran with frequency boost on and no fixed clock, and the comparison recorded no
clock while it ran, so systems timed hours apart may have run at different clocks. The Zen 1 is
shared with other work and its settings were not mine to change, so no run had boost off, and what
it costs is not measured. The spread it could hide in is a coefficient of variation of
{{cdL_cv_min}}% to {{cdL_cv_max}}% in the Zen 1's benchmarks, and a spread, the range of a query's runs over their median, of
{{cmpL_ts_warm_spread_min}}% to {{cmpL_ts_warm_spread_max}}% warm and
{{cmpL_ts_cold_spread_min}}% to {{cmpL_ts_cold_spread_max}}% cold in tick-store's queries; each
system's day writes have a repetition count of {{cmpL_write_reps}}.

**The targets.** Nothing published measures a columnar store of ITCH, so each speed and size target
is a figure published for something near it, on other data and mostly on newer processors; the rest compare tick-store with the other systems on
the same machine. [BENCHMARKS.md](BENCHMARKS.md) gives every source. I chose to keep each target as
published, so a gap is the distance to that figure, not a defect in itself.

**The comparison.** Parquet (a columnar file format, written by pyarrow), ArcticDB (Man Group's
DataFrame database for time series) and QuestDB (a time-series SQL server) each ran the setup
the comparison fixed for it in advance, plus the other setups a selection run on a slice of the day kept. The queries
are a second of every stock at the open (Q1), an hour of a stock (Q2), its per-minute aggregate
(Q3), a millisecond of every stock (Q4) and the day's sum of a shares column (Q5), each run
{{cmpL_warm_runs}} times warm, proven to read nothing from storage, and {{cmpL_cold_runs}} times
cold, proven evicted. An unflushed write returns when data is in the page cache, a flushed write
after a proven flush to the device.

**The inputs are not alike.** tick-store reads the day's ITCH file, made resident in the page cache
before its clock starts, on the slice and on the day. The other systems read the columns
exported from the same day, {{cmpL_others_input_pages}} pages ({{cmpL_others_input_gb}} GB), as
the cache holds them when the write starts: on the day, {{cmpL_others_resident_A_min}} to
{{cmpL_others_resident_A_max}} of those pages were resident at the start of an unflushed write and
{{cmpL_others_resident_B}} at the start of a flushed write,
and each write read {{cmpL_others_device_gb_min}} to {{cmpL_others_device_gb_max}} GB from the
device inside its clock ([Zen 1 comparison]({{path:cmpL}})). On the slice every input is resident.
A write is timed in total and engine-only, the total minus the harness's reading of the input and
its conversion to the form the system's call takes; the engine-only clock still holds what the
engine reads from the device, so neither clock removes the difference: ArcticDB's
engine-only flushed write took {{cmpL_arctic_B_engine_s}} s on this run and
{{rp_arctic_B_engine_s}} s on the run it replaced ({{pin:compare-replaced}}).

Every figure is filled in by `scripts/render_writeup.py` from the results file and field the
[figure list]({{path:figures}}) names for it; nothing checks that the field is what the sentence
describes.
Every figure here was measured at commit `{{measured_commit}}`{{commit_exceptions}}.

## Data layout

**The file, in bytes.** A store is a container file: a {{code:container_header_bytes}}-byte
header, the blocks' payloads back to back, a footer, and a {{code:container_trailer_bytes}}-byte
trailer holding the footer's offset and CRC32C
([container.hpp](include/tick_store/store/container.hpp)). The footer gives each block a
{{code:block_entry_bytes}}-byte entry, offsets, sizes, CRC32C and codec id, and each segment's
first block adds a summary of its times and a bitmap of the stocks present, up to
{{code:locate_bitmap_max_bytes}} bytes. Opening a file reads its ends, and a time or stock filter
reads the footer alone.

**Segments and stored order.** Records are cut into segments of {{seg_records}} messages,
{{ss_segments}} on the day, and a segment decodes from its own blocks alone
([docs/store.md](docs/store.md)). Inside a segment, records are sorted by stock locate, the day's
16-bit number for each stock, and kept in file order within each stock, so a stock's records are a
run of each of the {{code:column_count}} columns and a single-stock query reads a range per column.
The `locate` column holds each run's positions in file order, which is how file order comes back;
a window across every stock must decode it whole. Columns are cut into pieces of
{{code:piece_values}} values, each a container block, the unit read, checked and, where zstd wraps
it, inflated. A piece holds {{code:piece_codec_blocks}} codec blocks of
{{code:column_block_values}} values, each opening with an {{code:block_frame_bytes}}-byte frame and
carrying its own first value, so it decodes alone.

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
([codec evaluation]({{path:ce}})). Distance 0 means "the next import", a full reference to an
order created before the segment, which keeps segments independent: the day holds
{{ss_imports}} imports, and distances cost {{ss_ref_distance_bits}} as built.

**Prices as integers, per stock.** ITCH prices are integers in units of 10^-4 dollars. The store
groups add prices by stock and side and codes them with PFOR delta (patched frame of reference on
the deltas: a bit width per vector, values too wide kept in a patch list), trying each block at a
few scales from its own divisors and choosing among them by size. The best codec needs
{{ce_add_price_best_bits}} per price in file order, {{ce_add_price_pl_best_bits}} per stock and
{{ce_add_price_pls_best_bits}} per stock and side; as built, prices cost {{ss_add_price_bits}}. The
per-stock figures leave out the sequence that restores file order, whose order-0 entropy is
{{ce_perm_add_locate_side_bits}} bits per add, a floor on its cost, which the store pays in the
`locate` column, at {{ss_locate_bits}} bits per record.

**Timestamps: delta, not delta-of-delta.** ITCH timestamps do not tick at regular intervals, the
case delta-of-delta is built for: the ideal code for a delta in file order is
{{ce_ts_ideal_delta_bits}} bits, for a delta-of-delta {{ce_ts_ideal_dod_bits}}. PFOR delta reaches
{{ce_ts_best_bits}}, patching saving {{ce_ts_patch_saving_bits}} bits. Per stock, each delta spans
the other stocks' messages in between, and PFOR patches {{ce_tspl_pfor_exception_pct}}% of values.
The store keeps timestamps per stock as residuals from a `time_base` column, the earliest time of
each {{code:time_base_records}} records in file order.

**Segment size, the layout choice measured on the full day.** The comparison ran
{{seg1_records}}-record segments beside the default {{seg_records}}
([RESULTS.md](RESULTS.md#comparison) has both). At the default, each stock's run is
{{seg_run_ratio}} times as long, Q2 touches {{cmpL_ts_q2_segments}} segments instead of
{{cmpL_seg1_q2_segments}}, and the store takes {{ss_stored_bytes}} bytes against
{{cmpL_seg1_apparent_bytes}}; at {{seg1_records}} Q2 takes {{cmpL_seg1_q2_warm_med_ms}} ms warm
against {{cmpL_ts_q2_warm_med_ms}}. A window across every stock decodes the segment's
whole `locate` column: {{cmpL_ts_q4_values}} values for Q4's {{cmpL_q4_rows}} rows at the default,
{{cmpL_seg1_q4_values}} at {{seg1_records}}, and Q4 takes {{cmpL_ts_q4_warm_med_ms}} ms warm
against {{cmpL_seg1_q4_warm_med_ms}}, Q1 {{cmpL_ts_q1_warm_med_ms}} against
{{cmpL_seg1_q1_warm_med_ms}}. I chose the default by a scoring rule committed before the layout
experiments.

## Codecs, and whether Gorilla lost

Gorilla was built as published, delta-of-delta for timestamps and XOR of successive doubles for
values, and is exact on the whole day ({{verdict:g4}}). On `mwcb_level`, with {{ce_mwcb_values}}
values a day, it takes {{ce_mwcb_gorilla_bytes}} bytes against {{ce_mwcb_best_bytes}} for the
integer codecs ([codec evaluation]({{path:ce}})).

| Column and order | Gorilla, bits per value | Gorilla over the best integer codec |
|---|---|---|
| Timestamps, file order | {{ce_ts_gorilla_bits}} | {{ce_ts_gorilla_over_best}}x |
| Timestamps, per stock* | {{ce_tspl_gorilla_bits}} | {{ce_tspl_gorilla_over_best}}x |
| Add prices, file order, dollar doubles | {{ce_price_xor_dollars_bits}} | {{cr5_add_file_dollars}} |
| Add prices, file order, integer-valued doubles | {{ce_price_xor_integer_bits}} | {{cr5_add_file_best}} |
| Add prices, per stock*, integer-valued doubles | {{ce_price_pl_xor_integer_bits}} | {{cr5_add_pl_best}} |

\* Leaves out the cost of the sequence that restores file order.

A price with cents has no finite binary form, so as a double its mantissa is full and successive
prices XOR to many significant bits, and an integer tick count stored as a double removes that:
Gorilla takes {{ce_price_xor_dollars_bits}} bits per add price on dollar doubles and
{{ce_price_xor_integer_bits}} on integer-valued ones. The price-ratio target is
{{verdict:cr5_over_dollars}} over XOR on dollar doubles on every price case, and
{{verdict:cr5_over_best_file}} over the best Gorilla variant in file order.

In decode time Gorilla takes {{cdM_ds6_ts}} as long as the store's codecs on timestamps on the
M4 and {{cdM_ds6_price}} on prices ([M4 column decodes]({{path:cdM}})), against a bar of
{{bar:ds6}} on each: {{verdict:ds6_ts}} on timestamps and {{verdict:ds6_price}} on prices. On the
Zen 1 the same ratios are {{cdL_ds7_ts}} and {{cdL_ds7_price}}.

**What the writer chooses.** Each column has a fixed codec family: PFOR (plain, delta, head delta,
scaled), a dictionary, run-length, raw bytes, and zstd over a piece where it saves bytes
({{ss_add_price_zstd_blocks}} of {{ss_add_price_blocks}} price pieces). On some columns I chose
ranked PFOR, dictionary codes ranked by frequency and packed with PFOR.

## Decode: kernels, SIMD, and what it cost

**The vector layout.** A full PFOR vector is {{code:vector_values}} values at a bit width in
{{code:lanes}} lanes of {{code:lane_word_bits}}-bit words, value i in lane i mod {{code:lanes}}, so
a row of {{code:lanes}} consecutive values unpacks from {{code:lanes}} consecutive words with a
shift ({{code:neon_registers_per_row}} NEON or {{code:avx2_registers_per_row}} AVX2 registers) and
no transpose. At width w the packed values take {{code:vector_bytes_per_width}} × w bytes, after a
{{code:pfor_vector_header_bytes}}-byte header and the base, and a
{{code:pfor_exception_count_bytes}}-byte exception count follows
([pfor.cpp](src/codec/pfor.cpp)). Deltas stay in value order, so the inverse delta is a prefix sum
across each row's lanes; FastLanes, a layout for vectorised integer decoding from the Dutch
institute CWI, transposes values to make it an add down each lane.

**Kernel sets.** Scalar, NEON and AVX2 sets give identical values, and the widest set the
processor runs is chosen at start-up, from CPUID on x86-64, which `TICK_STORE_SIMD` can cap. The
store's reader calls each set's full-vector unpack, tail unpack and base add, and the second pass of
the two-pass delta; on AVX2 it calls the fused unpack and delta for unpatched delta vectors. The
narrow delta, prefix-sum, Stream VByte and delta-of-delta kernels serve benchmarks and tests only.

**What SIMD bought.** On the M4, NEON over scalar, medians over widths: linear unpack
{{skM_linear_speedup}}x, two-pass delta {{skM_two_pass_speedup}}x, fused delta
{{skM_fused_speedup}}x (not called on NEON), interleaved unpack {{skM_unpack_speedup}}x,
frame-of-reference add {{skM_for_add_speedup}}x, and Stream VByte, a byte-aligned codec the store
never writes, {{skM_svb_speedup}}x ([M4 kernels]({{path:skM}})). On the Zen 1, AVX2 over scalar:
interleaved unpack {{skL_unpack_speedup}}x, two-pass delta {{skL_two_pass_speedup}}x, fused
{{skL_fused_speedup}}x. A delta-of-delta reconstruct target is {{verdict:ds8}} at {{skM_dod_gbps}},
by a kernel the store never calls.

On queries, the same Zen 1 binary at AVX2 against capped to scalar changes the warm engine median
by {{d3_simd_q1_pct}}%, {{d3_simd_q2_pct}}%, {{d3_simd_q3_pct}}%, {{d3_simd_q4_pct}}% and
{{d3_simd_q5_pct}}% on Q1 to Q5, and the whole-day write runs at {{d3_write_ipc}} IPC with
{{d3_write_bm_per_msg}} mispredicts per message and {{d3_write_sys_pct}}% of its CPU time in the
kernel ([counters run]({{path:d3}}): {{note:diag-counters}}).

**What SIMD cost.** {{simd_src_lines}} lines of kernels and dispatch, {{simd_src_share}}% of the
{{src_include_lines}} in `src/` and `include/`; a {{simd_bench_lines}}-line kernel benchmark and
{{simd_test_lines}} lines of kernel tests; and fuzz targets that compare AVX2 with scalar.

**The store's price column.** Per piece, the reader inflates any zstd frame, allocates, decodes
PFOR with scale and patches, range-checks every value and appends. On the Zen 1 those steps take
{{d4_zstd_pct}}%, {{d4_pfor_unpack_pct}}% (unpack), {{d4_patches_pct}}% (patches),
{{d4_scale_range_pct}}% (scale and range check) and {{d4_alloc_copy_pct}}% (allocation and copy) of
the decode's user cycles ([price-decode profile]({{path:d4}}): {{note:diag-price}}).

## Branch prediction and instruction counts

On the Zen 1, counting user mode in the timed passes only, Gorilla held in L1
runs {{cdL_gorilla_l1_instr}} instructions per price at {{cdL_gorilla_l1_ipc}} per cycle, with
{{cdL_gorilla_l1_bm}} mispredicts per value; the cycles a mispredict costs were not measured, so
none of the time is put on them. On the whole column it takes {{cdL_gorilla_xor_cycles}} cycles,
{{cdL_gorilla_xor_instr}} instructions and {{cdL_gorilla_xor_bm}} mispredicts per price, the
store's price column {{cdL_store_price_cycles}}, {{cdL_store_price_instr}} and
{{cdL_store_price_bm}} at {{cdL_store_price_ipc}} IPC. Over prices and timestamps, Gorilla's
mispredicts are {{ctr_bm_ratio_min}} to {{ctr_bm_ratio_max}} times the store's codecs' and its
instructions {{ctr_instr_ratio_min}} to {{ctr_instr_ratio_max}} times. PFOR delta on timestamps
runs at {{cdL_pfor_delta_ipc}} IPC, and with no memory counters collected nothing here says what
limits it ([Zen 1 column decodes]({{path:cdL}})).

## Memory

The writer holds a segment's messages, {{code:column_value_bytes}} bytes per column value, then
sorts and codes them: a peak footprint of {{rtM_ingest_footprint_mb}} MB on the M4 at segments of
{{rtM_segment_records}} records, and at {{cmpM_seg1_records}} a peak resident set, which also counts
the mapped source, {{cmpM_seg_rss_diff_mb}} MB smaller. The Zen 1's query worker peaks at
{{cmpL_tick_query_vmhwm_mb}} MB resident, mapped store pages included ([Zen 1
comparison]({{path:cmpL}})). QuestDB's day store, {{cmpL_questdb_B_alloc_gb}} GB, is larger than
the Zen 1's {{cap_mem_gib}} GiB of memory, so warm queries run singly, and a timed query during which a page was
swapped in is kept and marked: for tick-store, {{cmpL_ts_swapin_list}}.

## Writes and queries against three systems

**Size.** tick-store's flushed store takes {{cmpL_ts_B_alloc_gb}} GB on disk against the smallest
setup of each other system: Parquet {{cmpL_parquet_min_B_alloc_gb}} GB, ArcticDB
{{cmpL_arctic_min_B_alloc_gb}} GB, QuestDB {{cmpL_questdb_min_B_alloc_gb}} GB
([Zen 1 comparison]({{path:cmpL}})).

**Writes.** tick-store parses the ITCH file inside its clock; the others read the exported
columns, not resident as tick-store's input is, and spend {{cmpL_others_read_share_min}}% to
{{cmpL_others_read_share_max}}% of their unflushed total reading and converting them. Unflushed,
in seconds total and engine-only: tick-store {{cmpL_ts_A_total_s}} and {{cmpL_ts_A_engine_s}},
Parquet {{cmpL_parquet_A_total_s}} and {{cmpL_parquet_A_engine_s}}, ArcticDB
{{cmpL_arctic_A_total_s}} and {{cmpL_arctic_A_engine_s}}, QuestDB {{cmpL_questdb_A_total_s}} and
{{cmpL_questdb_A_engine_s}}. QuestDB's flushed write is {{verdict:questdb_B_like_for_like}}: its
flush proof on the day reads {{verdict:questdb_B_proof}}. The write verdicts follow the targets table.

**Where the writer's time goes.** Alone, parsing runs at {{icL_parse_mps}} M messages a second on
the slice in memory, about {{wr_parse_s}} s of the engine-only write. Of the write's user cycles,
PFOR encoding, a search over widths and scales, takes {{d1_pfor_encode_pct}}%, dictionary encoding
{{d1_dict_encode_pct}}%, book state {{d1_book_state_pct}}%, segment assembly
{{d1_segment_assembly_pct}}%, sorting {{d1_sort_pct}}%, parsing {{d1_parse_pct}}% and zstd
{{d1_zstd_pct}}% ([writer profile]({{path:d1}}): {{note:diag-writer}}).

**Queries.** Among the fixed setups, tick-store has the lowest median and p99 on Q2 and Q3,
warm and cold ({{verdict:ql4_linux_q2q3}}), and on Q5 cold ({{verdict:ql4_linux_q5_cold}});
the next fastest takes {{cmpL_q2_warm_best_ms}} ms on Q2 warm, against tick-store's
{{cmpL_ts_q2_warm_med_ms}}, and {{cmpL_q3_warm_best_ms}} ms on Q3, against
{{cmpL_ts_q3_warm_med_ms}}. ArcticDB set up with a table per stock takes
{{cmpL_arcps_q2_warm_med_ms}} and {{cmpL_arcps_q3_warm_med_ms}} ms warm on them, and
{{cmpL_arcps_q1_warm_med_s}} s on Q1. Across every stock, {{cmpL_q1_warm_best_system}} answers Q1
in {{cmpL_q1_warm_best_ms}} ms warm and {{cmpL_q4_warm_best_system}} Q4 in
{{cmpL_q4_warm_best_ms}} ms (tick-store's are under Segment size); on Q5 warm tick-store's
{{cmpL_ts_q5_warm_med_ms}} ms against the fastest other's {{cmpL_q5_warm_best_ms}} ms leaves it
{{verdict:ql4_linux_q5_warm}} ([RESULTS.md](RESULTS.md#comparison)).

## Tried and not kept

Gorilla, delta-of-delta and other reference codings (stateless, a creation list for the day,
day-wide distances) were evaluated and not kept (see Codecs). Not built: a stored per-stock minute aggregate (Q3), a sum over ranked dictionary
counts that unpacks nothing (Q5), checkpoints inside a run so a window can start near itself (Q1,
Q4), and a second writer thread.

## Missed targets

On the Zen 1 unless the M4 is named, units as the bar's; "x" is how many times off the bar.

| Asked | Bar (source) | Measured | Gap | Verdict | Why |
|---|---|---|---|---|---|
| Timestamps, store as built | {{bar:cr3}} (KX case study, NYSE quotes) | {{ss_ts_bits}}; per stock* {{ce_tspl_best_bits}} | {{gap_cr3_store}}; {{gap_cr3_pl}} bits | {{verdict:cr3}} | Bases add bits; per-stock deltas patched |
| Add prices, file order | {{bar:cr4}} (ALP paper, stock doubles) | {{ce_add_price_best_bits}} | {{gap_cr4_file}} bits | {{verdict:cr4}} | Stocks interleave; per stock {{verdict:cr4_pl}} |
| Gorilla's bits over the integer codec's, file order | {{bar:cr5}} (2025 paper, Indian stocks) | {{cr5_best_file_missed_min}}x to {{cr5_best_file_missed_max}}x | to {{gap_cr5_worst}} | {{verdict:cr5}} | Integer-valued doubles help Gorilla |
| References, store as built | {{bar:cr6}} (KX case study) | new orders {{ss_creation_ref_bits}}; imports {{ss_import_ref_bits}} | {{gap_cr6_creation}}; {{gap_cr6_import}} bits | {{verdict:cr6}} | Codec alone {{ce_creation_ref_best_bits}}; imports are full references |
| Shares | {{bar:cr7}} (KX case study) | executions, trades, imbalances {{ce_shares_missed_min_bits}} to {{ce_shares_missed_max_bits}}; store {{ss_add_shares_bits}}, {{ss_reduce_shares_bits}} | {{gap_cr7_min}} to {{gap_cr7_max}} bits | {{verdict:cr7}} | Not traced |
| Gorilla decode, M4 | {{bar:ds1}} (mailing-list post, smooth synthetic values) | {{cdM_ds1_ns}} | {{gap_ds1_x}}x | {{verdict:ds1}} | Not traced |
| Gorilla decode in L1 | {{bar:ds2}} (ALP paper, Intel Ice Lake) | {{cdL_gorilla_l1_cycles}} | {{gap_ds2_x}}x | {{verdict:ds2}} | {{cdL_gorilla_l1_instr}} instructions a value |
| Delta decode, M4 | {{bar:ds3}} (FastLanes, Apple M1) | {{skM_two_pass_gvps}} | {{gap_ds3_pct}}% | {{verdict:ds3}} | {{skM_two_pass_widths_reaching}} of {{skM_two_pass_widths}} widths reach it |
| Delta decode | {{bar:ds4}} (FastLanes, Zen 3 server) | {{skL_fused_cycles}} | {{gap_ds4_x}}x | {{verdict:ds4}} | Not traced |
| Price-column decode | {{bar:ds5}} (ALP, an AMD EPYC 9554 core) | {{cdL_ds5_mbps}}; M4 {{cdM_ds5_mbps}} | {{gap_ds5_linux_x}}x; {{gap_ds5_mac_pct}}% | {{verdict:ds5_linux}}; M4 {{verdict:ds5_mac}} | Whole column path, steps under Decode |
| Store against Gorilla decode, prices, M4 | {{bar:ds6}} (third-party benchmark, Apple M2 Max) | {{cdM_ds6_price}} | {{gap_ds6_price}}x | {{verdict:ds6}} | Whole column path, steps under Decode |
| Single-thread ingest | {{bar:wt1}} (QuestDB vendor figure, 32 workers) | {{cmpL_ts_A_rate_m}}; M4 {{cmpM_ts_A_rate_m}} | {{gap_wt1_linux_x}}x; {{gap_wt1_mac_x}}x | {{verdict:wt1_linux}}; M4 {{verdict:wt1_mac}} | Parsing {{wr_parse_pct}}% of it |
| Ingest above Nasdaq's peak rate | {{bar:wt2}} (Nasdaq bandwidth report) | flushed {{cmpL_ts_B_rate_m}}; unflushed under Single-thread ingest | {{gap_wt2_linux_x}}x unflushed | {{verdict:wt2_linux}} ({{disturbance:ts_B_write}}) | Not traced |
| Q2 warm median | {{bar:ql1}} (DuckDB on a small Parquet store, preprint) | {{cmpL_ts_q2_warm_med_ms}}; M4 {{cmpM_ts_q2_warm_med_ms}} | {{gap_ql1_linux_x}}x; {{gap_ql1_mac_ms}} ms | {{verdict:ql1_linux}}; M4 {{verdict:ql1_mac}} | {{q2_ns_per_row}} ns a row |
| Q3 warm median | {{bar:ql2}} (QuestDB vendor benchmark) | {{cmpL_ts_q3_warm_med_ms}}; M4 {{cmpM_ts_q3_warm_med_ms}} | {{gap_ql2_linux_x}}x; {{gap_ql2_mac_x}}x | {{verdict:ql2_linux}}; M4 {{verdict:ql2_mac}} | Not traced |
| Q4 cold mean | {{bar:ql3}} (TimescaleDB, order-book preprint) | {{cmpL_ts_q4_cold_mean_ms}}; M4 {{cmpM_ts_q4_cold_mean_ms}} | {{gap_ql3_linux_x}}x; {{gap_ql3_mac_x}}x | {{verdict:ql3_linux}}; M4 {{verdict:ql3_mac}} | Whole-segment positions; {{cmpL_ts_q4_cold_device_mb}} MB read cold |
| Lowest median and p99 of the fixed setups, each query | same machine | Q1, Q4 under Segment size; Q5 warm under Queries | | {{verdict:ql4_linux}} | Q5 unpacks {{cmpL_ts_q5_values}} values, {{q5_ns_per_value}} ns each |

\* Leaves out the cost of the sequence that restores file order.

On the unflushed write, the target of writing no slower than each other system's fixed setup on
both clocks, with tick-store's input resident and theirs read from the device inside their clocks,
is {{verdict:wt3_linux}}. On the flushed write it is {{verdict:wt4_linux}}: against ArcticDB {{changed:wt4_arcticdb}}, and nothing on record explains
ArcticDB's change (the replaced run's figures come from {{pin:compare-replaced}}). tick-store's flushed write,
{{cmpL_ts_B_total_s}} s here and {{rp_ts_B_total_s}} s on the run replaced, {{disturbance:ts_B_write}};
QuestDB's, {{cmpL_questdb_B_total_s}} s here and {{rp_questdb_B_total_s}} s on the run replaced,
{{disturbance:questdb_B_write}}.

## Not judged

With no published figure: the store's speed-up over Gorilla on the Zen 1
({{verdict:ds7_linux}}) and delta-of-delta decode with bit parsing ({{verdict:ds9_mac}} on the M4,
{{verdict:ds9_linux}} on the Zen 1); and, with no comparison day of every system on the M4, its comparison targets
({{verdict:wt3_mac}}, {{verdict:ql4_mac}}) ([RESULTS.md](RESULTS.md#targets)).

## Commits and the fuzz result

**The fuzz oracle's work.** Each fuzz target decodes its input and re-encodes the decoded values to
compare. At the measured commit, the fuzz run stopped on the harness's per-input time limit,
{{code:fuzz_input_timeout_s}} s, when a target's oracle was given a maximal input, so that run is no
result. The oracle's work is now bounded in tiers: every input is still decoded
in full under the sanitizers, and fuzzing compares the kernel sets' values on blocks of up to
{{code:fuzz_kernel_compare_values}} values and {{code:fuzz_past_bound_values}} values of any block
past that, while the store writes blocks of {{code:column_block_values}} values. Each host's fuzz run is
judged at the second commit: {{g3_mac_clean}} of {{g3_mac_targets}} targets ran clean on the M4 and
{{g3_linux_clean}} of {{g3_linux_targets}} on the Zen 1, the shortest run {{g3_shortest_s}} s
([fuzz results]({{path:fuzz}})).

## How the results were checked

`scripts/check.sh` runs warnings-as-errors builds, unit tests, sanitizers, clang-tidy, the fuzz
targets, coverage and the results validators. A prover applies a fault to a throwaway clone and
requires the named step to fail; it lists {{proofs_listed}} faults and a harmless control. Its
record ([prover record]({{path:proofs}})) is of a run over {{proofs_covered}} of the listed
faults, {{proofs_proven}} of them proven, and {{proofs_control}}. The comparison's measures,
voiding causes and spread thresholds were fixed before any measured run. The final review recorded
{{review_rows}} findings, each fixed with a test seen failing first or closed with its reason
([docs/REVIEW.md](docs/REVIEW.md)).

## Limits

Performance comes from a pinned day on a single host, the Zen 1, whose {{cap_mem_gib}} GiB of memory
is smaller than the other systems' {{cmpL_others_input_gb}} GB of input, and with a repetition count of {{cmpL_write_reps}} for
each day write; the host also runs a backup job, and logins fell inside timed phases
({{disturbance:summary}}). A fair protocol would give every system its input resident before its
clock, or none, and repeat every write. Sizes on the M4 are file-system allocation, so sizes come
from the Zen 1. Beyond boost, not measured and not estimated: the cycles a mispredict costs and any
memory-level counter.
