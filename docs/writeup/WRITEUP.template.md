# tick-store: one day of Nasdaq ITCH in a columnar store

tick-store keeps a trading day of Nasdaq TotalView-ITCH 5.0, {{ss_records}} messages in
{{ss_source_gb}} GB, in a store {{ss_ratio}} times smaller than the feed, {{cmpL_ts_B_alloc_gb}} GB
on disk against {{glance_disk_other}} GB for {{glance_disk_other_system}}, and rebuilds the feed from it.
On the Zen 1, a Linux machine from 2018, it answers an hour of a stock in {{cmpL_ts_q2_warm_med_ms}}
warm against {{cmpL_q2_warm_best_ms}} ms {{glance_q2_other_flag}} for the fastest other system in its fixed
setup, though ArcticDB with a table per stock takes {{cmpL_arcps_q2_warm_med_ms}}
ms, while a millisecond across every stock takes it {{gap_ql4_q4_x}} times as long as
{{cmpL_q4_warm_best_system}} and the whole-day sum of a column {{gap_ql4_q5_x}} times as long as
{{cmpL_q5_warm_best_system}} {{glance_q5_other_flag}}.
A single writer thread there ingests {{cmpL_ts_A_msg_rate_m}} before a flush, {{gap_wt2_linux_x}} times
short of Nasdaq's published peak message rate.

Gorilla, the codec Facebook published for time series, {{verdict:gorilla_bits}} to plain integer
codecs in size on every column but the near-empty `mwcb_level`, at {{ce_ts_gorilla_over_best}}x
their bits on timestamps and {{cr5_add_file_dollars}} on prices as dollar doubles.

## At a glance

The Zen 1, each system in its fixed setup; queries are warm medians, and the other system is
the system first on the row.

| | tick-store | The other system first on the row | tick-store over it |
|---|---|---|---|
| Disk, GB | {{cmpL_ts_B_alloc_gb}} | {{glance_disk_other}}, {{glance_disk_other_system}} | {{glance_disk_x}} |
| Day write with flush, s | {{cmpL_ts_B_total_s}} {{glance_write_ts_flag}} | {{glance_write_other}}, {{glance_write_other_system}} {{glance_write_other_flag}} | inputs not alike |
| A second of every stock (Q1), ms | {{cmpL_ts_q1_warm_med_ms}} {{glance_q1_ts_flag}} | {{cmpL_q1_warm_best_ms}}, {{cmpL_q1_warm_best_system}} {{glance_q1_other_flag}} | {{gap_ql4_q1_x}} |
| An hour of a stock (Q2), ms | {{cmpL_ts_q2_warm_med_ms}} {{glance_q2_ts_flag}} | {{cmpL_q2_warm_best_ms}}, {{cmpL_q2_warm_best_system}} {{glance_q2_other_flag}} | {{glance_q2_x}} |
| Its per-minute aggregate (Q3), ms | {{cmpL_ts_q3_warm_med_ms}} {{glance_q3_ts_flag}} | {{cmpL_q3_warm_best_ms}}, {{cmpL_q3_warm_best_system}} {{glance_q3_other_flag}} | {{glance_q3_x}} |
| A millisecond of every stock (Q4), ms | {{cmpL_ts_q4_warm_med_ms}} {{glance_q4_ts_flag}} | {{cmpL_q4_warm_best_ms}}, {{cmpL_q4_warm_best_system}} {{glance_q4_other_flag}} | {{gap_ql4_q4_x}} |
| The day's sum of a shares column (Q5), ms | {{cmpL_ts_q5_warm_med_ms}} {{glance_q5_ts_flag}} | {{cmpL_q5_warm_best_ms}}, {{cmpL_q5_warm_best_system}} {{glance_q5_other_flag}} | {{gap_ql4_q5_x}} |

† In this table and the opening, timed while the host's logs record a login or a backup job, or
while the host was swapping memory; the [targets table](RESULTS.md#targets) names each. The write row is not like-for-like:
tick-store's input was in memory before its clock, and the others read theirs from disk inside it.

## How it was measured

**Data.** The reference day is 30 December 2019, from Nasdaq's public sample files; a second day,
30 October 2019, serves the round trip and the bit-flip check. The round trip compares the
SHA-256 of the rebuilt stream with the source's.

**Hosts.** Every pinned figure comes from the Zen 1, whose processor is {{cap_cpu}}, with
{{cap_mem_gib}} GiB of memory. Each benchmark is pinned to a core, and
refused if the core's hyperthread sibling was over {{code:sibling_busy_limit_pct}}% busy. Context
figures come from an Apple M4 Max laptop, "the M4": unpinned, no hardware counters.

**Clock and repetition.** The Zen 1 ran with frequency boost on and no fixed clock, so systems
timed hours apart may have run at different clocks; what that costs every time-based figure was
not measured.

**The comparison.** Parquet, ArcticDB and QuestDB each ran the setup the comparison fixed for it in
advance, plus the setups a selection run on a slice kept. Each query ran {{cmpL_warm_runs}} times
warm, proven to read nothing from storage, and {{cmpL_cold_runs}} times cold, proven evicted. A
write without a flush returns when the data is in the page cache; a flushed write, after a proven
flush to the device.

Every figure here, read from the results files the [figure list]({{path:figures}}) names, was
measured at commit `{{measured_commit}}`{{commit_exceptions}}.

## Data layout

**The file, in bytes.** A store is a container file: a {{code:container_header_bytes}}-byte
header, the blocks' payloads, a footer, and a {{code:container_trailer_bytes}}-byte trailer ([container.hpp](include/tick_store/store/container.hpp)).
The footer gives each block a {{code:block_entry_bytes}}-byte entry, and each segment's first block
a summary of its times and a bitmap of its stocks, up to {{code:locate_bitmap_max_bytes}} bytes, so
a time or stock filter reads the footer alone.

**Segments and stored order.** Records are cut into segments of {{seg_records}} messages,
{{ss_segments}} on the day ([docs/store.md](docs/store.md)). Inside a segment, records are sorted by
stock locate, the day's 16-bit number for each stock, and kept in file order within each stock, so
a single-stock query reads a range of each of the {{code:column_count}} columns. The `locate`
column, despite its name, holds each record's position in the segment's file order: the
permutation that puts the sorted runs back in file order, which a window across every stock must
decode whole. Columns are cut into pieces of {{code:piece_values}} values, the unit read and
checked, each holding {{code:piece_codec_blocks}} codec blocks of {{code:column_block_values}}
values behind an {{code:block_frame_bytes}}-byte frame.

**Where the bytes go.** Of the store's {{ss_stored_bytes}} bytes, timestamps take {{ss_ts_mb}} MB,
{{ss_ts_share}}% of the column payloads; then come the permutation in the `locate` column
({{ss_locate_mb}} MB), new-order references ({{ss_creation_ref_mb}} MB), references to existing
orders ({{ss_ref_distance_mb}} MB) and message types ({{ss_type_mb}} MB)
([store sizes]({{path:ss}})).

**References as distances within the stock.** An execute, cancel, delete or replace names an order
by a 64-bit reference. The best codec measured needs {{ce_referenced_ref_bits}} bits for it in file
order, {{ce_ref_distance_min_bits}} to {{ce_ref_distance_max_bits}} as a distance back in the day's
orders, and {{ce_ref_locate_distance_min_bits}} to {{ce_ref_locate_distance_max_bits}}, by message
type, as a distance back in the stock's own list, so I chose the last ([codec evaluation]({{path:ce}})).
As built, with block metadata, those distances cost {{ss_ref_distance_bits}}. A distance of 0
is an import: the full reference of an order created before the segment, since no state crosses a
segment; the day holds {{ss_imports}}, at {{ss_import_ref_bits}} each. A new order's own reference
costs {{ce_creation_ref_best_bits}} bits by the best codec and {{ss_creation_ref_bits}} as built.

**Prices as integers, per stock.** ITCH prices are integers in units of 10^-4 dollars. The store
groups add prices by stock and side and codes them with PFOR delta (patched frame of reference on
the deltas), trying each block at a few scales. The best codec needs {{ce_add_price_best_bits}} per
price in file order, {{ce_add_price_pl_best_bits}} per stock and {{ce_add_price_pls_best_bits}}
per stock and side; as built, prices cost {{ss_add_price_bits}}. Per-stock figures here leave out
the permutation, whose order-0 entropy is {{ce_perm_add_locate_side_bits}} bits per add; the store
pays {{ss_locate_bits}} bits per record for it.

**Timestamps: delta, not delta-of-delta.** ITCH timestamps do not tick at regular intervals: the
ideal code for a delta in file order is {{ce_ts_ideal_delta_bits}} bits, for a delta-of-delta
{{ce_ts_ideal_dod_bits}}. PFOR delta reaches {{ce_ts_best_bits}}, its patches saving
{{ce_ts_patch_saving_bits}} bits against the same codec unpatched. I keep timestamps per stock as
residuals from a `time_base` column, the earliest time of each {{code:time_base_records}} records
in file order, where they cost {{ss_ts_bits}}.

**Sorting by stock, in bytes.** Stock order costs timestamps the difference between
{{ce_ts_best_bits}} in file order and {{ss_ts_bits}} as built, and costs the permutation's
{{ss_locate_mb}} MB; it buys prices at {{ss_add_price_bits}} against {{ce_add_price_best_bits}}
and references at {{ss_ref_distance_bits}} against {{ce_referenced_ref_bits}}. The committed files
hold no whole store in file order, so the net was not measured.

**Segment size.** The comparison ran {{seg1_records}}-record segments beside the default
{{seg_records}}. At the default each stock's run is {{seg_run_ratio}} times as long: Q2 touches
{{cmpL_ts_q2_segments}} segments instead of {{cmpL_seg1_q2_segments}} and takes
{{cmpL_ts_q2_warm_med_ms}} warm against {{cmpL_seg1_q2_warm_med_ms}}, and the store takes
{{ss_stored_bytes}} bytes against {{cmpL_seg1_apparent_bytes}}. But a window across every stock
decodes its segment's whole permutation, {{cmpL_ts_q4_values}} values for Q4's {{cmpL_q4_rows}}
rows at the default against {{cmpL_seg1_q4_values}}, so Q4 takes {{cmpL_ts_q4_warm_med_ms}} ms
against {{cmpL_seg1_q4_warm_med_ms}} and Q1 {{cmpL_ts_q1_warm_med_ms}} against
{{cmpL_seg1_q1_warm_med_ms}}. The default was picked by a rule I wrote before any layout was timed:
each layout small enough on disk scores a point on each of Q1 to Q5, warm and cold, on the M4,
where it is within a few percent of the quickest, and the most points win.

## Codecs, and whether Gorilla lost

Gorilla was built as published, delta-of-delta for timestamps and XOR of successive doubles for
values. On `mwcb_level`, with {{ce_mwcb_values}} values a day, it takes
{{ce_mwcb_gorilla_bytes}} bytes against {{ce_mwcb_best_bytes}} for the integer codecs
([codec evaluation]({{path:ce}})).

| Column and order | Gorilla, bits per value | Gorilla over the best integer codec |
|---|---|---|
| Timestamps, file order | {{ce_ts_gorilla_bits}} | {{ce_ts_gorilla_over_best}}x |
| Timestamps, per stock* | {{ce_tspl_gorilla_bits}} | {{ce_tspl_gorilla_over_best}}x |
| Add prices, file order, dollar doubles | {{ce_price_xor_dollars_bits}} | {{cr5_add_file_dollars}} |
| Add prices, file order, integer-valued doubles | {{ce_price_xor_integer_bits}} | {{cr5_add_file_best}} |
| Add prices, per stock*, integer-valued doubles | {{ce_price_pl_xor_integer_bits}} | {{cr5_add_pl_best}} |

\* Without the permutation's cost.

A price with cents has no finite binary form, so successive dollar doubles XOR to many
significant bits; an integer tick count stored as a double removes that. In decode time Gorilla
takes {{cdM_ds6_ts}} as long as the store's codecs on timestamps on the M4 and {{cdM_ds6_price}} on
prices, and {{cdL_ds7_ts}} and {{cdL_ds7_price}} on the Zen 1; why the timestamp ratio differs
between the hosts was not measured.

**What the writer chooses.** Each column has a fixed codec family: PFOR (plain, delta, head delta,
scaled), a dictionary, run-length, raw bytes, or ranked PFOR, and zstd over a piece where it saves
bytes. zstd still shrinks {{ss_add_price_zstd_blocks}} of {{ss_add_price_blocks}} PFOR-coded price
pieces, so the PFOR blocks leave redundancy across values that a block-local code does not reach.

## Decode: kernels, SIMD, and what it cost

**The vector layout.** A full PFOR vector is {{code:vector_values}} values at a bit width in
{{code:lanes}} lanes of {{code:lane_word_bits}}-bit words, value i in lane i mod {{code:lanes}}, so
a row of {{code:lanes}} values unpacks from {{code:lanes}} consecutive words with a shift. At width
w the packed values take {{code:vector_bytes_per_width}} × w bytes, after a
{{code:pfor_vector_header_bytes}}-byte header and the base, then a
{{code:pfor_exception_count_bytes}}-byte exception count ([pfor.cpp](src/codec/pfor.cpp)). Deltas
stay in value order, so the inverse delta is a prefix sum across each row's lanes.

**Which kernels the store calls.** On every instruction set the decoder calls the interleaved and
linear unpacks, the frame-of-reference add and the two-pass delta. On AVX2 it also fuses unpack,
base and prefix sum for a full delta vector with no exceptions; on NEON and scalar that fused
kernel is benchmarked and never called. The narrow fused unpack, Stream VByte and the
delta-of-delta reconstruct are benchmark-only as well.

**What SIMD bought.** On the M4, NEON over scalar, medians over widths: linear unpack
{{skM_linear_speedup}}x, two-pass delta {{skM_two_pass_speedup}}x, fused delta
{{skM_fused_speedup}}x and interleaved unpack {{skM_unpack_speedup}}x
([M4 kernels]({{path:skM}})). On the Zen 1, AVX2 over scalar: interleaved unpack
{{skL_unpack_speedup}}x, two-pass delta {{skL_two_pass_speedup}}x, fused {{skL_fused_speedup}}x.

**What SIMD changes end to end.** The same Zen 1 binary at AVX2, against itself capped to scalar,
changes the warm engine median by {{d3_simd_q1_pct}}%, {{d3_simd_q2_pct}}%, {{d3_simd_q3_pct}}%,
{{d3_simd_q4_pct}}% and {{d3_simd_q5_pct}}% on Q1 to Q5, a negative change being time saved. A
query's own runs range {{cmpL_ts_warm_spread_min}}% to {{cmpL_ts_warm_spread_max}}% of their
median, so on whole queries SIMD's effect is inside or near the run-to-run spread
([counters run]({{path:d3}}): {{note:diag-counters}}).

**What SIMD cost.** {{simd_src_lines}} lines of kernels and dispatch, {{simd_src_share}}% of the
{{src_include_lines}} in `src/` and `include/`, a {{simd_bench_lines}}-line benchmark and
{{simd_test_lines}} lines of tests.

**The store's price column.** Per piece, the reader inflates any zstd frame, allocates, decodes
PFOR with scale and patches, range-checks every value and appends. On the Zen 1 those steps take
{{d4_zstd_pct}}%, {{d4_pfor_unpack_pct}}% (unpack), {{d4_patches_pct}}% (patches),
{{d4_scale_range_pct}}% (scale and range check) and {{d4_alloc_copy_pct}}% (allocation and copy,
as the profile's memory and container groups count them; copies resolved only to the C library,
{{d4_libc_pct}}%, are not counted) of
the decode's user cycles ([price-decode profile]({{path:d4}}): {{note:diag-price}}).

## Branch prediction and instruction counts

On the Zen 1, in user mode, Gorilla takes on the whole price column {{cdL_gorilla_xor_cycles}} cycles, {{cdL_gorilla_xor_instr}}
instructions and {{cdL_gorilla_xor_bm}} mispredicts per price, the store's price column
{{cdL_store_price_cycles}}, {{cdL_store_price_instr}} and {{cdL_store_price_bm}} at
{{cdL_store_price_ipc}} IPC. Over prices and timestamps, Gorilla's mispredicts are
{{ctr_bm_ratio_min}} to {{ctr_bm_ratio_max}} times the store's codecs' and its instructions
{{ctr_instr_ratio_min}} to {{ctr_instr_ratio_max}} times. PFOR delta on timestamps runs at
{{cdL_pfor_delta_ipc}} IPC; with no memory counters, nothing here says what limits it
([Zen 1 column decodes]({{path:cdL}})).

## Memory

The writer holds a segment's messages, {{code:column_value_bytes}} bytes per column value, then
sorts and codes them. On the M4 its peak memory footprint, the memory it owns, is
{{rtM_ingest_footprint_mb}} MB at segments of {{rtM_segment_records}} records; its peak resident
set, which also counts the mapped source file, is {{cmpM_seg_rss_diff_mb}} MB smaller at
{{cmpM_seg1_records}}. tick-store's query process (`tick_query`) peaks at {{cmpL_tick_query_vmhwm_mb}} MB resident,
mapped store pages included. QuestDB's day store, {{cmpL_questdb_B_alloc_gb}} GB, is larger than
the Zen 1's {{cap_mem_gib}} GiB of memory, so warm queries run a single system at a time.

## Writes and queries against three systems

**Writes.** Before a flush, in seconds total and engine-only: tick-store {{cmpL_ts_A_total_s}}
and {{cmpL_ts_A_engine_s}}, Parquet {{cmpL_parquet_A_total_s}} and {{cmpL_parquet_A_engine_s}},
ArcticDB {{cmpL_arctic_A_total_s}} and {{cmpL_arctic_A_engine_s}}, QuestDB
{{cmpL_questdb_A_total_s}} and {{cmpL_questdb_A_engine_s}}. Engine-only is the total minus the
harness's reading and converting of the input, {{cmpL_others_read_share_min}}% to
{{cmpL_others_read_share_max}}% of the others' totals; tick-store parses its ITCH file inside the
engine. The inputs did not start alike; the targets sections give the bytes read. QuestDB's flushed write is {{verdict:questdb_B_like_for_like}}: its flush proof on the
day did not pass. With a flush, tick-store's write took {{cmpL_ts_B_total_s}} s and without a flush
{{cmpL_ts_A_total_s}} s, a single run each, so a difference of that size is not resolved here.

**Where the writer's time goes.** tick-store's engine-only time is its writer's own work:
{{d3_write_cycles_per_msg}} cycles and {{d3_write_instr_per_msg}} instructions a message at
{{d3_write_ipc}} IPC ({{note:diag-counters}}). Of its user cycles, PFOR encoding takes
{{d1_pfor_encode_pct}}%, assembling segments {{d1_segment_assembly_pct}}%, the book state, each
open order's stock and price kept to code the messages that name it, {{d1_book_state_pct}}%,
dictionary encoding {{d1_dict_encode_pct}}%, sorting {{d1_sort_pct}}%, parsing {{d1_parse_pct}}%
and zstd {{d1_zstd_pct}}% ([writer profile]({{path:d1}}): {{note:diag-writer}}). The others' engine-only times may still hold reading their input from disk, so
the clocks are not alike and the gap is not all tick-store's writer.

**Queries.** On Q2 tick-store takes {{cmpL_ts_q2_warm_med_ms}} warm against
{{cmpL_q2_warm_best_ms}} for the next system in its fixed setup, and on Q3
{{cmpL_ts_q3_warm_med_ms}} against {{cmpL_q3_warm_best_ms}} ms; ArcticDB with a table per stock takes
{{cmpL_arcps_q2_warm_med_ms}} and {{cmpL_arcps_q3_warm_med_ms}} ms on them, and
{{cmpL_arcps_q1_warm_med_s}} s on Q1. Q5, the day's sum of a shares column, decodes {{cmpL_ts_q5_values}} values at
{{q5_ns_per_value}} ns each; of its sampled cycles, dictionary decoding takes {{d2_q5_dict_pct}}%,
the C library {{d2_q5_libc_pct}}% and the unpack kernels {{d2_q5_unpack_pct}}%
([query profile]({{path:d2}}): {{note:diag-query}}).

## Tried and not kept, and not done

I tried and did not keep Gorilla for timestamps and prices; delta-of-delta timestamps; references
in file order and as distances in the day's orders; {{seg1_records}}-record segments; the fused delta kernel on NEON; and Stream VByte. Not built: a stored
per-stock minute aggregate (Q3), a sum over dictionary counts that would skip Q5's decode, a time
index inside a run that would let Q1 and Q4 skip the permutation, and a second writer thread.

A store is a single file holding a single day, written from that day's ITCH file under a temporary name
and published by rename, so a crash leaves the old file or none ([format.md](docs/format.md)). It
takes no appends, does not capture a live feed, and holds no second day.

## Targets that held

On the day the store gives every message back bit-exact ({{verdict:g1}}), a flipped bit is caught
in a data block and in the footer ({{verdict:g2}}), Gorilla is built and is exact on the whole day
({{verdict:g4}}), and the store takes the least disk of the systems compared ({{verdict:cr2}}).

## Missed targets

On the Zen 1 unless the M4 is named, units as the bar's; "x" is how many times off the bar.
[BENCHMARKS.md](BENCHMARKS.md) classes nearly every bar as context, measured on other hardware or another
workload: they show what good looks like and prove nothing about this store. Nasdaq's peak rate is
a direct bar, and the rows against the other systems are relative.

| Asked | Bar (source) | Measured | Gap | Verdict | Why |
|---|---|---|---|---|---|
| Timestamps, store as built | {{bar:cr3}} (KX case study, NYSE quotes) | {{ss_ts_bits}}; per stock, codec alone, {{ce_tspl_best_bits}} | {{gap_cr3_store}}; {{gap_cr3_pl}} bits | {{verdict:cr3}} | Not measured |
| Add prices, file order | {{bar:cr4}} (ALP paper, stock doubles) | {{ce_add_price_best_bits}} | {{gap_cr4_file}} bits | {{verdict:cr4}} | Per stock {{verdict:cr4_pl}} |
| Gorilla's bits over the integer codec's, file order | {{bar:cr5}} (2025 paper, Indian stocks) | {{cr5_best_file_missed_min}}x to {{cr5_best_file_missed_max}}x | {{gap_cr5_least}} to {{gap_cr5_worst}} | {{verdict:cr5}} | Not measured |
| References, store as built | {{bar:cr6}} (KX case study) | new orders {{ss_creation_ref_bits}}; imports {{ss_import_ref_bits}} | {{gap_cr6_creation}}; {{gap_cr6_import}} bits | {{verdict:cr6}} | Imports are full references |
| Shares | {{bar:cr7}} (KX case study) | executions, trades, imbalances {{ce_shares_missed_min_bits}} to {{ce_shares_missed_max_bits}}; store, adds {{ss_add_shares_bits}}, reductions {{ss_reduce_shares_bits}} | {{gap_cr7_min}} to {{gap_cr7_max}} bits | {{verdict:cr7}} | Not measured |
| Gorilla decode, M4 | {{bar:ds1}} (mailing-list post, smooth synthetic values) | {{cdM_ds1_ns}} | {{gap_ds1_x}}x | {{verdict:ds1}} | Not measured |
| Gorilla decode in L1 | {{bar:ds2}} (ALP paper, Intel Ice Lake) | {{cdL_gorilla_l1_cycles}} | {{gap_ds2_x}}x | {{verdict:ds2}} | {{cdL_gorilla_l1_instr}} instructions a value |
| Delta decode, M4 | {{bar:ds3}} (FastLanes, Apple M1) | {{skM_two_pass_gvps}} | {{gap_ds3_pct}}% | {{verdict:ds3}} | {{skM_two_pass_widths_reaching}} of {{skM_two_pass_widths}} widths reach it |
| Delta decode | {{bar:ds4}} (FastLanes, Zen 3 server) | {{skL_fused_cycles}} | {{gap_ds4_x}}x | {{verdict:ds4}} | Not measured |
| Price-column decode | {{bar:ds5}} (ALP, an AMD EPYC 9554 core) | {{cdL_ds5_mbps}}; M4 {{cdM_ds5_mbps}} | {{gap_ds5_linux_x}}x; {{gap_ds5_mac_pct}}% | {{verdict:ds5_linux}}; M4 {{verdict:ds5_mac}} | Steps under Decode |
| Store against Gorilla decode, prices, M4 | {{bar:ds6}} (third-party benchmark, Apple M2 Max) | {{cdM_ds6_price}} | {{gap_ds6_price}}x | {{verdict:ds6}} | Steps under Decode |
| Single-thread ingest | {{bar:wt1}} (QuestDB vendor figure, 32 workers) | {{cmpL_ts_A_rate_m}}; M4 {{cmpM_ts_A_rate_m}} | {{gap_wt1_linux_x}}x; {{gap_wt1_mac_x}}x | {{verdict:wt1_linux}}; M4 {{verdict:wt1_mac}} | A single thread; writer under Writes |
| Ingest above Nasdaq's peak rate | {{bar:wt2}} (Nasdaq bandwidth report) | unflushed {{cmpL_ts_A_msg_rate_m}}; flushed {{cmpL_ts_B_rate_m}} | {{gap_wt2_linux_x}}x; {{gap_wt2_B_x}}x | {{verdict:wt2_linux}} | Writer under Writes |
| Q2 warm median | {{bar:ql1}} (DuckDB on a small Parquet store, preprint) | {{cmpL_ts_q2_warm_med_ms}}; M4 {{cmpM_ts_q2_warm_med_ms}} | {{gap_ql1_linux_x}}x; {{gap_ql1_mac_ms}} ms | {{verdict:ql1_linux}}; M4 {{verdict:ql1_mac}} | {{q2_ns_per_row}} ns a row |
| Q3 warm median | {{bar:ql2}} (QuestDB vendor benchmark) | {{cmpL_ts_q3_warm_med_ms}}; M4 {{cmpM_ts_q3_warm_med_ms}} | {{gap_ql2_linux_x}}x; {{gap_ql2_mac_x}}x | {{verdict:ql2_linux}}; M4 {{verdict:ql2_mac}} | No stored aggregate |
| Q4 cold mean | {{bar:ql3}} (TimescaleDB, order-book preprint) | {{cmpL_ts_q4_cold_mean_ms}}; M4 {{cmpM_ts_q4_cold_mean_ms}} | {{gap_ql3_linux_x}}x; {{gap_ql3_mac_x}}x | {{verdict:ql3_linux}}; M4 {{verdict:ql3_mac}} | {{cmpL_ts_q4_cold_device_mb}} MB read cold |
| Lowest median and p99 of the fixed setups, each query | same machine | warm medians in the table at a glance | {{gap_ql4_q1_x}}x on Q1; {{gap_ql4_q4_x}}x on Q4; {{gap_ql4_q5_x}}x on Q5 | {{verdict:ql4_linux}} | Q1 to Q5 under Queries |

The writes against each other system, on both clocks: without a flush, {{verdict:wt3_linux}}.

With a flush, {{verdict:wt4_linux}}.
Against QuestDB it is not like-for-like: QuestDB read {{cmpL_questdb_B_device_b}} B from disk inside
its clock, tick-store {{cmpL_ts_B_device_b}} B, its input in memory before its clock.
The [targets table](RESULTS.md#targets) gives both runs'
figures; the cause of ArcticDB's move between them was not measured and is not known.

## Not judged

| No published figure, or no comparison day on the M4 | Verdict |
|---|---|
| The store's decode speed-up over Gorilla, Zen 1 | {{verdict:ds7_linux}} |
| Delta-of-delta decode with bit parsing, M4 | {{verdict:ds9_mac}} |
| The same, Zen 1 | {{verdict:ds9_linux}} |
| Writes against the other systems, M4 | {{verdict:wt3_mac}} |
| Queries against the other systems, M4 | {{verdict:ql4_mac}} |

## Fuzzing and checks

Every parser and decoder ran {{code:fuzz_required_minutes}} minutes of fuzzing under
AddressSanitizer and UndefinedBehaviorSanitizer at the second commit: {{g3_mac_clean}} of
{{g3_mac_targets}} targets ran clean on the M4 and {{g3_linux_clean}} of {{g3_linux_targets}} on the
Zen 1, the shortest run {{g3_shortest_s}} s ([fuzz results]({{path:fuzz}})). The checks and the faults
that prove them are in the README; the prover's
record covers {{proofs_covered}} of its {{proofs_listed}} listed faults.

## Limits

Performance comes from a pinned day on a single host, the Zen 1, whose {{cap_mem_gib}} GiB of memory
is smaller than the other systems' {{cmpL_others_input_gb}} GB of input, with each day write run
{{cmpL_write_reps}} time. A fair protocol would give every system its input resident before its
clock, or none, and repeat every write. The disturbance records hold {{disturbance:timed}} entries
inside the campaign's timed phases and {{disturbance:timed_fuzz}} inside the fuzz run's, an event
entered for each phase it fell in, from the host's logs and another project's status file. The host's own scheduled jobs ran through every timed window
on the Zen 1 and were not stopped: a repository backup every 15 minutes, a health collector and
2 small watchers every 5, 2 sync jobs every 10, a file backup about hourly, and daily maintenance;
only the runs the logs place inside a phase are listed. The Zen 1's day was measured
again: the earlier run, "the replaced run", is kept, and its figure stands beside this run's
wherever a mark names it. In the targets sections a verdict marked "swap-in" rests on figures timed
while the host was swapping memory, which may flatter tick-store where they are another system's
and understate it where they are its own, and a verdict marked with a login or a backup run on a phase that held it.
Sizes on the M4 are file-system allocation, so sizes come from the Zen 1. Not measured and not
estimated: the cycles a mispredict costs and any memory-level counter.
