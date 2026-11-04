# ITCH store, version 2

The store holds one ITCH 5.0 day as columns inside the container of `docs/format.md`. It gives back
every frame bit-exact, and the source's tail, the bytes after its last whole frame. The code is
`include/tick_store/store/itch_store.hpp` (layout and shared parts), `itch_segment.hpp` (one segment
between records and columns), `itch_store_writer.hpp`, `itch_store_reader.hpp` and, for queries,
`itch_query.hpp`.
`tests/unit/golden/itch-store-v2.tsc` is the 26-record test stream and its tail at 8 records a
segment; the writer must produce it byte for byte and the reader must read it back.

All integers in the store's own fields are little-endian, as in the container.

## Segments

A segment is N consecutive records of the source, N fixed per file, 1 to 2^24, by default 2^20.
The last segment holds the remainder. A segment decodes from its own blocks and the `symbols`
section alone: no state crosses a segment boundary, so reading segment k reads segment k's blocks
and nothing else. N = 2^20 was chosen on the measured day; `NOTES.md` has the sweep.

Each segment is one container block per column that has values, in column order, the `type` block
first. A segment has no blocks of its own beyond those, and no checkpoint: the one thing it would
need from earlier segments, the orders its messages name that were created before it, it carries
as its import list (below).

## Sections

| Name | Bytes | Content |
|---|---|---|
| `store` | 24 | u32 version (2), u32 N, u64 record count, u64 segment count, which must be the record count over N rounded up |
| `symbols` | 4 + 10 n | u32 n, then n entries of u16 locate and the 8-byte stock, locates strictly ascending: the first stock any message carried for each locate |
| `tail` | any | the source's bytes after its last whole frame |

The file has exactly these three sections. The reader refuses any version but 2; version 1, which
kept the locate column in file order and had no seek index or plain flag, is no longer read.

## Block metadata

Every block's metadata starts with 12 bytes: u32 segment, u16 column id, u16 reserved (0), u32
value count (at least 1). A `type` block's metadata goes on with the segment's summary, which is
what a time index or a symbol filter reads without touching a payload:

| Offset | Size | Field |
|---|---|---|
| 12 | 8 | first record, the index in the file of the segment's first record |
| 20 | 8 | first timestamp |
| 28 | 8 | last timestamp |
| 36 | 8 | lowest timestamp |
| 44 | 8 | highest timestamp |
| 52 | 4 | records with a type (all but raw records) |
| 56 | 1 | plain: 1 if the column form keeps every record of the segment as a hot or rare row, else 0 |
| 57 | 2 | locate bitmap size b, at most 8,192 |
| 59 | b | bitmap: bit (l mod 8) of byte l / 8 is set if locate l appears; no trailing zero byte |

A segment is plain when it has no record the decoder returned raw and every hot record's values fit
the column form's signed columns with a printable stock and MPID (`column_form_plain()` in
`columns.hpp`, the test `tick_export` applies). A query then takes every row's timestamp from its
own record. The flag is checked against each record when the segment is decoded whole.

Raw records carry no timestamp or locate, so the times and bitmap cover the typed records, and are
0 and empty when there are none. The reader checks each summary's own rules from the footer at open
(times within 48 bits, lowest and highest bounding first and last, typed records at most the
records), the directory (segments in order, each with a `type` block, first records contiguous,
every segment N records but the last, columns ascending, each block under a codec its column
takes, the segment count), and, when it decodes a segment, the summary against the columns.

## Columns

Every record has a type symbol: its ITCH type byte, or 0 for a record the decoder returned raw (an
unknown type, or a known type at the wrong length). Every typed record has a locate, a timestamp and
a tracking number. The rest go by type:

| Id | Column | Values from | Codec | Order | zstd |
|---|---|---|---|---|---|
| 0 | type | every record | dictionary | file | |
| 1 | locate | typed records | PFOR frame of reference | locate runs | yes |
| 2 | timestamp | typed records | PFOR delta | file | |
| 3 | tracking | typed records | dictionary | by type | |
| 4 | creation_ref | A, F, U's new reference | PFOR head delta | file | yes |
| 5 | ref_distance | E, C, X, D, U's original reference | dictionary | by type | |
| 6 | import_ref | each distance 0 | PFOR, delta or head delta, the smaller | file | |
| 7 | add_side | A, F | dictionary | file | |
| 8 | add_price | A, F | PFOR delta, adaptive scale | by locate and side | yes |
| 9 | add_shares | A, F | dictionary | by locate | |
| 10 | mpid | F | dictionary | file | |
| 11 | reduce_shares | E, C, X | dictionary | by type | |
| 12 | match | E, C, P, Q, B | PFOR delta | file | |
| 13 | printable | C | run length | file | |
| 14 | exec_price | C | PFOR, adaptive scale | file | |
| 15 | replace_shares | U | dictionary | file | |
| 16 | replace_price | U | PFOR, adaptive scale | file | |
| 17 | trade_ref | P | PFOR delta | file | |
| 18 | trade_side | P | dictionary | file | |
| 19 | trade_shares | P | dictionary | file | |
| 20 | trade_price | P | PFOR, adaptive scale | file | |
| 21 | cross_shares | Q | PFOR | file | |
| 22 | cross_price | Q | PFOR, adaptive scale | file | |
| 23 | cross_type | Q | dictionary | file | |
| 24 | imbalance_shares | I: paired, then imbalance | PFOR | file | |
| 25 | imbalance_price | I: far, near, reference | PFOR delta, adaptive scale | file | |
| 26 | imbalance_flags | I: direction, cross type and variation in 3 bytes | dictionary | file | yes |
| 27 | mwcb_level | V: the three levels | PFOR | file | |
| 28 | rare_bytes | S, R, H, Y, L, W, K, J, h, N, O: the payload after the 11-byte header | raw, a byte a value | file | yes |
| 29 | raw_length | raw records | PFOR | file | |
| 30 | raw_bytes | raw records' whole payloads | raw, a byte a value | file | yes |
| 31 | stock_exception_record | A, F, P, Q, I whose stock differs from the symbol | PFOR delta | file | |
| 32 | stock_exception_value | those stocks, 8 bytes each | raw, a byte a value | file | |
| 33 | locate_run | each locate the segment has | PFOR delta | per run | |
| 34 | locate_run_records | each locate's typed records | dictionary | per run | |
| 35 | locate_run_adds | each locate's A and F records | dictionary | per run | |
| 36 | seek | the seek index (below) | PFOR delta | seek | |

A column's payload is its values coded in blocks of 4,096 back to back. A column marked zstd is
wrapped in the `zstd` codec where that is smaller (`docs/format.md`); the reader accepts either.
Every value is checked against its field's width on read.

Grouped columns hold their values sorted, stably, by a key the reader gets from columns it decodes
first: `by type` by the type symbol, `by locate and side` by (locate << 8 | side), `by locate` by
the locate. The reader puts them back in file order. The counts each column must hold follow from
the type column, except the import list's (the number of distance 0s), the raw bytes' (the sum of
the raw lengths) and the stock exceptions', which may be any number up to the records. The run
directory's columns hold one value per locate in the summary's bitmap, and `seek` holds 36 + 27 G
values for G groups.

### Locate runs

The `locate` column holds, for each locate the segment has in ascending order, the indices among
the segment's typed records of that locate's records: a run's first index, then each later one less
the one before less 1. So a locate's records are one contiguous run of the column, and a symbol
filter reads that run alone. The run directory holds one value per run, in the same order, in each
of its three columns: the locate (`locate_run`), its typed records (`locate_run_records`, so a run's
start is the sum of the runs before it) and its A and F records (`locate_run_adds`, so its first add
in `add_shares` and its first in `add_price`'s side groups follow the same way). The runs are
exactly the bitmap's locates. On read the ids must strictly ascend, every run must have a record,
the runs must hold exactly the typed records, and every index must be below that count and named
once, so the runs are a partition; the adds are checked against the type column.

### Seek index

Each segment's `seek` column is what a query needs to start at a group of 4,096 records instead of
the segment's first record, and to skip what cannot match. A group is 4,096 consecutive records of
the segment (exactly one codec block of `type`), the last one shorter. The values, all u64:

| Values | Content |
|---|---|
| 36 | maxima: the largest value of every column but `seek`, in column order, 0 for a column with no values |
| G | for each of the G groups, the lowest timestamp of its typed records, 0 if none |
| G | the highest, 0 if none |
| G | the references its records name at distance 0, which each take an import |
| 24 G | for each of the 24 symbols 0, A, B, C, D, E, F, H, I, J, K, L, N, O, P, Q, R, S, U, V, W, X, Y, h in turn, its records in each group |

From the counts, a group's start in every column follows by prefix sums: the typed records before
it, the values of each type-driven column, the imports and the creations. When the index is
parsed, the counts must sum to each group's records, the times must be within 48 bits and ordered,
the imports within the group's naming records, and the maxima within each column's range. When the
segment is decoded whole, every value is checked against the columns. The index is 2 MB on
12302019, and `NOTES.md` has its bytes and the locate column's on both days.

### Order references

A, F and U's new reference are creations. E, C, X, D and U's original reference name an order,
stored as a distance back in that locate's list of known orders: the orders created in this
segment on that locate and the imports it has introduced, in the order they were appended. A
distance of 0 introduces the next import, the next value of `import_ref`, which is appended to the
locate's list. So an order created in an earlier segment, or never created, or created under
another locate, costs one import the first time a segment names it on a locate, and a short
distance after that. The writer keeps one entry per reference with the locate that last created or
named it, so a reference named under alternating locates is imported each time.

### Stocks

The stocks of A, F, P, Q and I are derived from the `symbols` section. Where one differs from its
locate's symbol, the record's index in the segment goes to `stock_exception_record` and its 8 bytes
to `stock_exception_value`. An exception equal to the symbol, or naming a record of another type, is
refused. The other types with a stock keep it in their `rare_bytes`.

## Reading

`ItchStoreReader::open` reads the container's two ends, the three sections and the footer's block
metadata, nothing else. `SegmentReader::seek(k)` reads segment k's blocks, checks each block's CRC
unless the reader was opened with `Checksums::skip`, decodes the columns, checks everything that
ties them together, and gives the records back in order. `ItchStoreRecordReader` walks the segments
for the oracle, so `tick_roundtrip verify` reads the store through the whole path.

Errors name what refused and where: the segment, the block, the column, the file byte, the record
and the value's index in its column, whichever apply, with the expected and actual values.

### Queries

`ItchQuery` (`itch_query.hpp`) answers Q1 to Q5 of `BENCHMARKS.md` with the comparison harness's
semantics, so each answer equals what `compare/queries.py` computes from `tick_export`'s columns:

- A row is a record as the column form holds it: its number in the file, its timestamp in
  nanoseconds from midnight, type byte, locate, order reference (U's original), side, shares, price
  and stock, each 0 or empty where the form does not use it. A record the form calls raw keeps its
  type byte and takes the timestamp of the row before it, 0 for the file's first, with locate 0.
- A window is half-open on that timestamp. The harness finds it by a binary search over the
  timestamps, which equals a filter only because a day's timestamps never go down; the query
  filters, and the full-day oracle in `NOTES.md` shows the two agree on 12302019.
- The minute aggregate counts A and F rows of the locate in the window per minute, with their
  shares summed and the least and greatest price. The share sum is over every row: add, reduce,
  replace and trade shares. The busiest locate is the most rows in a window, locate 0 excluded, the
  lowest on a tie.

How a segment is answered, cheapest first: the footer's summary skips a segment whose times or
locates cannot match; the seek index skips every group whose time zone map cannot, and gives each
needed column's value index at the first group that can; a locate filter reads that locate's run of
the locate column and its directory entry; and only the columns the query needs are read, a codec
block at a time. A segment that is not plain is answered exactly instead: decoded whole, each record
re-encoded and mapped by `row_values()`, the function `tick_export` uses. Every block read is
CRC-checked unless the reader skips checksums, and every value is checked against its field and the
seek index's maximum for its column; a file the whole decode accepts gives the same answer on either
path. Each answer reports the segments, blocks, bytes and values it decoded.

`tools/tick_query` serves the queries as JSON lines on stdin and stdout for the harness; its usage
is in `include/tick_store/harness/query_cli.hpp`.

### Worst-case work for one segment

Decoding a random segment k reads its blocks only: at most 37, their CRCs, then at most N type
values, N locates, timestamps and tracking numbers, at most N values in each typed column but
`imbalance_price` and `mwcb_level` (3 per record) and `imbalance_shares` (2 per record), at most
37 `rare_bytes` per record (the largest rare type, O, is 48 bytes), and the raw records' own bytes,
at most 65,535 per record, then one directory value per locate and 36 + 27 G seek values. zstd
inflates at most 32 bytes a value plus 1,024 per column, and exactly the byte count for byte
columns. File order comes back by a counting sort for the columns grouped by type, O(N), and a sort
of the adds by key, O(A log A) for A adds. Each reference is one list lookup. Nothing outside segment k is read but the `symbols` section, parsed once at open.

## Writing

`ItchStoreWriter` takes one message at a time and writes each segment as it fills, so its private
memory is bounded by one segment, never by the day. For a segment of N records it holds 8 bytes
for each value (at most 19 per typed record, an I with a stock exception, plus one per rare or raw
byte), the grouping keys (at most 9 bytes per record), and a table of the segment's references,
16 bytes a slot with the slots the smallest power of two at least twice the distinct references
(at most 2N), plus 8 bytes a reference; then, while a column is coded, that column's chunk. The
tables of 65,536 locates are fixed: 256 KiB of list lengths, an 8 KiB bitmap, and the symbol
table's 256 KiB index with 10 bytes per symbol. The container keeps 12 bytes of metadata per block
and each segment summary for the footer. `NOTES.md` has the footprint measured on the full days.
The durability class,
`page_cache` or `device_flush`, is a parameter of `ItchStoreWriter::create` and of `tick_roundtrip
ingest`, never a default. The writer refuses a timestamp over 48 bits and a raw message that is
empty, over 65,535 bytes, or one `decode` would never return raw, changing nothing.
