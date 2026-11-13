# ITCH store, version 3

The store holds one ITCH 5.0 day as columns inside the container of `docs/format.md`. It gives back
every frame bit-exact, and the source's tail, the bytes after its last whole frame. The code is
`include/tick_store/store/itch_store.hpp` (layout and shared parts), `itch_segment.hpp` (one segment
between records and columns), `itch_store_writer.hpp`, `itch_store_reader.hpp` and, for queries,
`itch_query.hpp`.
`tests/unit/golden/itch-store-v3.tsc` is the 26-record test stream and its tail at 8 records a
segment; the writer must produce it byte for byte and the reader must read it back.
`itch-store-v3-07b020e.tsc` is the same stream as the writer made it at `07b020e`, before the hot
dictionary columns were held to ranked PFOR (below): the same version, every byte meaning the same,
so the reader must read it back too.

All integers in the store's own fields are little-endian, as in the container.

## Segments

A segment is N consecutive records of the source, N fixed per file, 1 to 2^24, by default 2^22.
The last segment holds the remainder. A segment decodes from its own blocks and the `symbols`
section alone: no state crosses a segment boundary, so reading segment k reads segment k's blocks
and nothing else. N = 2^22 was chosen on the measured day by the layout experiments in
`docs/NOTES.md`, which have the sweep.

A segment keeps its records in **stored order**: the typed records by ascending locate, each
locate's in file order, then the raw records in file order. So one locate's records are one run of
every column, and the `locate` column (below) holds each typed record's index in the segment, which
gives file order back. Every column but the run directory, the seek index and the time bases
holds its values in stored order, or grouped from it as listed.

Each column of a segment is one or more container blocks, its **pieces**, in column order, the
`type` column first. A piece holds 16,384 values, four codec blocks, the last piece what is left;
a byte column (`raw_bytes` in the table) is always one piece. A piece is the unit that is read,
CRC-checked and, for a zstd column, inflated, so a query that needs a few thousand values of a
column reads a piece of it, not the whole column. A segment has no blocks of its own beyond its
columns' pieces, and no checkpoint: the one thing it would need from earlier segments, the orders
its messages name that were created before it, it carries as its import list (below).

## Sections

| Name | Bytes | Content |
|---|---|---|
| `store` | 24 | u32 version (3), u32 N, u64 record count, u64 segment count, which must be the record count over N rounded up |
| `symbols` | 4 + 10 n | u32 n, then n entries of u16 locate and the 8-byte stock, locates strictly ascending: the first stock any message carried for each locate |
| `tail` | any | the source's bytes after its last whole frame |

The file has exactly these three sections. The reader refuses any version but 3. Version 1, which
kept the locate column in file order and had no seek index or plain flag, and version 2, which
kept every segment in file order with each column one block, are no longer read.

## Block metadata

Every block's metadata starts with 12 bytes: u32 segment, u16 column id, u16 piece (its index
among the column's pieces, from 0), u32 value count (at least 1, and 16,384 for every piece of an
integer column but the last). The first piece of a `type` column goes on with the segment's
summary, which is what a time index or a symbol filter reads without touching a payload:

| Offset | Size | Field |
|---|---|---|
| 12 | 8 | first record, the index in the file of the segment's first record |
| 20 | 8 | first timestamp: the time of the typed record first in the file |
| 28 | 8 | last timestamp: the time of the typed record last in the file |
| 36 | 8 | lowest timestamp |
| 44 | 8 | highest timestamp |
| 52 | 4 | records, at least the piece's values and at most 2^24 |
| 56 | 4 | records with a type (all but raw records) |
| 60 | 1 | plain: 1 if the column form keeps every record of the segment as a hot or rare row, else 0 |
| 61 | 2 | locate bitmap size b, at most 8,192 |
| 63 | b | bitmap: bit (l mod 8) of byte l / 8 is set if locate l appears; no trailing zero byte |

A segment is plain when it has no record the decoder returned raw and every hot record's values fit
the column form's signed columns with a printable stock and MPID (`column_form_plain()` in
`columns.hpp`, the test `tick_export` applies). A query then takes every row's timestamp from its
own record. The flag is checked against each record when the segment is decoded whole.

Raw records carry no timestamp or locate, so the times and bitmap cover the typed records, and are
0 and empty when there are none. The reader checks each summary's own rules from the footer at open
(times within 48 bits, lowest and highest bounding first and last, typed records at most the
records), the directory (segments in order, each with a `type` block, first records contiguous,
every segment N records but the last, columns ascending, each column's pieces numbered from 0 with
every piece but the last full and a byte column one piece, the `type` column's pieces holding the
summary's records, each block under a codec its column takes, the segment count), and, when it
decodes a segment, the summary against the columns.

## Columns

Every record has a type symbol: its ITCH type byte, or 0 for a record the decoder returned raw (an
unknown type, or a known type at the wrong length). Every typed record has a locate, a timestamp and
a tracking number. The rest go by type:

| Id | Column | Values from | Codec | Order | zstd |
|---|---|---|---|---|---|
| 0 | type | every record | dictionary | stored | |
| 1 | locate | typed records | PFOR frame of reference | locate runs | yes |
| 2 | timestamp | typed records | PFOR | time residuals | |
| 3 | tracking | typed records | dictionary | by type | |
| 4 | creation_ref | A, F, U's new reference | PFOR head delta | stored | yes |
| 5 | ref_distance | E, C, X, D, U's original reference | dictionary | by type | |
| 6 | import_ref | each distance 0 | PFOR, delta or head delta, the smaller | stored | |
| 7 | add_side | A, F | dictionary | stored | |
| 8 | add_price | A, F | PFOR delta, adaptive scale | by locate and side | yes |
| 9 | add_shares | A, F | dictionary | by locate | |
| 10 | mpid | F | dictionary | stored | |
| 11 | reduce_shares | E, C, X | dictionary | by type | |
| 12 | match | E, C, P, Q, B | PFOR delta | stored | |
| 13 | printable | C | run length | stored | |
| 14 | exec_price | C | PFOR, adaptive scale | stored | |
| 15 | replace_shares | U | dictionary | stored | |
| 16 | replace_price | U | PFOR, adaptive scale | stored | |
| 17 | trade_ref | P | PFOR delta | stored | |
| 18 | trade_side | P | dictionary | stored | |
| 19 | trade_shares | P | dictionary | stored | |
| 20 | trade_price | P | PFOR, adaptive scale | stored | |
| 21 | cross_shares | Q | PFOR | stored | |
| 22 | cross_price | Q | PFOR, adaptive scale | stored | |
| 23 | cross_type | Q | dictionary | stored | |
| 24 | imbalance_shares | I: paired, then imbalance | PFOR | stored | |
| 25 | imbalance_price | I: far, near, reference | PFOR delta, adaptive scale | stored | |
| 26 | imbalance_flags | I: direction, cross type and variation in 3 bytes | dictionary | stored | yes |
| 27 | mwcb_level | V: the three levels | PFOR | stored | |
| 28 | rare_bytes | S, R, H, Y, L, W, K, J, h, N, O: the payload after the 11-byte header | raw, a byte a value | stored | yes |
| 29 | raw_length | raw records | PFOR | stored | |
| 30 | raw_bytes | raw records' whole payloads | raw, a byte a value | stored | yes |
| 31 | stock_exception_record | A, F, P, Q, I whose stock differs from the symbol | PFOR delta | stored | |
| 32 | stock_exception_value | those stocks, 8 bytes each | raw, a byte a value | stored | |
| 33 | time_base | each 64 records of the segment in file order | PFOR delta | time chunks | |
| 34 | locate_run | each locate the segment has | PFOR delta | per run | |
| 35 | locate_run_start | each locate's first stored record | PFOR delta | per run | |
| 36 | locate_run_add_start | each locate's first A or F among the adds | PFOR delta | per run | |
| 37 | locate_run_import_start | each locate's first import | PFOR delta | per run | |
| 38 | seek | the seek index (below) | PFOR delta | seek | |

A piece's payload is its values coded in blocks of 4,096 back to back. The dictionary codec has
three modes (`codec/dictionary.hpp`); the writer holds the eight hot dictionary columns, `type`,
`ref_distance`, `add_side`, `add_shares`, `reduce_shares`, `replace_shares`, `trade_side` and
`trade_shares`, to ranked PFOR, which decodes faster than Huffman for a few more bytes, and lets
the others take the smallest mode (each column's `dict_modes` in `itch_store.hpp`). The reader takes
every mode in every dictionary column. A piece of a column marked
zstd is wrapped in the `zstd` codec where that is smaller (`docs/format.md`); the reader accepts
either, piece by piece. Every value is checked against its field's width on read.

Grouped columns hold their values sorted, stably, by a key the reader gets from columns it decodes
first: `by type` by the type symbol, `by locate and side` by (locate << 8 | side), `by locate` by
the locate. The reader puts them back in stored order. In stored order a locate's adds are already
together, so `by locate` changes nothing and `by locate and side` sorts within each locate's adds.
The counts each column must hold follow from the type column, except the import list's (the
number of distance 0s), the raw bytes' (the sum of the raw lengths) and the stock exceptions',
which may be any number up to the records. `time_base` holds one value for each 64 records,
rounded up, none if no record is typed; the run directory's columns hold one value per locate in
the summary's bitmap, and `seek` holds 38 + 29 G values for G groups.

### Locate runs

The `locate` column holds, for each locate the segment has in ascending order, the indices in the
segment (its file order) of that locate's records, ascending: a run's first index, then each later
one less the one before less 1. Since the typed records are stored by locate, the run of the
`locate` column and the locate's stored records are the same range, and the indices give each
record's place in the file. The run directory holds one value per run, in the same order, in each
of its four columns: the locate (`locate_run`), the run's first stored record
(`locate_run_start`), its first A or F among the segment's adds (`locate_run_add_start`, which is
also its first in `add_shares` and the start of its side groups in `add_price`), and its first
import (`locate_run_import_start`). A run ends where the next starts, the last at the totals. So a
symbol filter reads one directory entry, then one range of every column. The runs are exactly the
bitmap's locates. On read the ids must strictly ascend; the starts must begin at 0 and strictly
ascend below the typed records, so every run has a record; the add and import starts must begin
at 0, never fall and stay within the totals, and each run's adds and imports are checked against
its records; and every index must be below the records and named once, so the runs place the typed
records at distinct indices, the raw records taking the rest in file order.

### Time bases

A locate's times in file order rise slowly, but each delta spans every other locate's records
between two of its own, so they code larger than the file's own deltas. The `time_base` column
gives them back most of that: for each 64 records of the segment in file order, the lowest time of
its typed records (the value before where it has none, 0 for a first with none). The `timestamp`
column holds each typed record's time less the larger of its base, the value of the 64 records its
index falls in, and the time before it in its run (none for a run's first record), modulo 2^64. A
time below that, which a day's file never has but a file may, wraps and comes back the same way;
the reader refuses a time past 48 bits and a base the times do not give. Reading a run's times
needs its indices, which the `locate` column gives, and the bases, 16,384 values a segment.

### Seek index

Each segment's `seek` column is what a query needs to start at a group of 4,096 records instead of
the segment's first record, and to skip what cannot match. A group is 4,096 consecutive records of
the segment in stored order (exactly one codec block of `type`), the last one shorter; a file group
is 4,096 consecutive records in file order. The values, all u64:

| Values | Content |
|---|---|
| 38 | maxima: the largest value of every column but `seek`, as stored, in column order, 0 for a column with no values |
| G | for each of the G groups, the lowest timestamp of its typed records, 0 if none |
| G | the highest, 0 if none |
| G | the references its records name at distance 0, which each take an import |
| 24 G | for each of the 24 symbols 0, A, B, C, D, E, F, H, I, J, K, L, N, O, P, Q, R, S, U, V, W, X, Y, h in turn, its records in each group |
| G | for each of the G file groups, the lowest timestamp of its typed records, 0 if none |
| G | the highest, 0 if none |

From the counts, a group's start in every column follows by prefix sums: the typed records before
it, the values of each type-driven column, the imports and the creations; a record's within its
group follows from the type column over the part of the group before it. A locate filter prunes
its run by the stored groups' times, and a query over every locate finds the records it can need
by the file groups'. When the index is parsed, the counts must sum to each group's records, the
times must be within 48 bits and ordered, the imports within the group's naming records, and the
maxima within each column's range. When the segment is decoded whole, every value is checked
against the columns. `docs/NOTES.md` has the index's bytes and the locate column's on both days.

### Order references

A, F and U's new reference are creations. E, C, X, D and U's original reference name an order,
stored as a distance back in that locate's list of known orders: the orders created in this
segment on that locate and the imports it has introduced, in the order they were appended, which
is stored order, and so the locate's own file order. A distance of 0 introduces the next import,
the next value of `import_ref`, which is appended to the locate's list. So an order created in an
earlier segment, or never created, or created under another locate, costs one import the first
time a segment names it on a locate, and a short distance after that, and a locate's list is built
from its own run alone. The writer keeps one entry per reference with the locate that last created
or named it, reading the records in stored order, so a reference is imported once by each locate
that names it after another locate had it.

### Stocks

The stocks of A, F, P, Q and I are derived from the `symbols` section. Where one differs from its
locate's symbol, the record's index in stored order goes to `stock_exception_record` and its 8 bytes
to `stock_exception_value`. An exception equal to the symbol, or naming a record of another type, is
refused. The other types with a stock keep it in their `rare_bytes`.

## Reading

`ItchStoreReader::open` reads the container's two ends, the three sections and the footer's block
metadata, nothing else. `SegmentReader::seek(k)` reads segment k's blocks, checks each block's CRC
unless the reader was opened with `Checksums::skip`, decodes the columns, checks everything that
ties them together, decodes the records in stored order, and gives them back in file order. `ItchStoreRecordReader` walks the segments
for the oracle, so `tick_roundtrip verify` reads the store through the whole path.

Errors name what refused and where: the segment, the block, the column, the file byte, the record
and the value's index in its column, whichever apply, with the expected and actual values where
the check compared values. A container's or a codec's refusal is given in its own words, in
parentheses, with its own values and offset, and the store's error adds only the place. A flipped
bit in a payload reads `container: block, segment 32, column timestamp, block 56537 (bad checksum:
block at offset 878190723: expected 2290514605, have 1031937585)`: the stored CRC, then the one
computed.

### Queries

`ItchQuery` (`itch_query.hpp`) answers Q1 to Q5 of `BENCHMARKS.md` with the comparison harness's
semantics, so each answer equals what `compare/queries.py` computes from `tick_export`'s columns:

- A row is a record as the column form holds it: its number in the file, its timestamp in
  nanoseconds from midnight, type byte, locate, order reference (U's original), side, shares, price
  and stock, each 0 or empty where the form does not use it. A record the form calls raw keeps its
  type byte and takes the timestamp of the row before it, 0 for the file's first, with locate 0.
- A window is half-open on that timestamp. The harness finds it by a binary search over the
  timestamps, which equals a filter only because a day's timestamps never go down; the query
  filters, and the full-day oracle in `docs/NOTES.md` shows the two agree on 12302019.
- The minute aggregate counts A and F rows of the locate in the window per minute, with their
  shares summed and the least and greatest price. The share sum is over every row: add, reduce,
  replace and trade shares. The busiest locate is the most rows in a window, locate 0 excluded, the
  lowest on a tie.

How a segment is answered, cheapest first: the footer's summary skips a segment whose times or
locates cannot match. A locate filter takes the locate's run from the bitmap's rank and its
directory entry, each column's first index for the run from the seek counts at the run's group
plus the types before it in the group, prunes the run's end by the stored groups' times, and reads
that one range of each column it needs, resolving references from the run's own list. A query over
every locate finds the file groups whose times can match, decodes the run directory and the
`locate` column, takes from each run the records whose indices fall there, reads each such run
from its start to the last of them (its references need the run's list from its start), and puts
the rows back in file order by their indices. Only the columns a query needs are read, a codec
block at a time, each piece read, CRC-checked and inflated the first time a value in it is wanted.
A segment that is not plain is answered exactly instead: decoded whole, each record re-encoded and
mapped by `row_values()`, the function `tick_export` uses. Every block read is CRC-checked unless
the reader skips checksums, every value is checked against its field and every time against 48
bits, and a run's limits against the totals and the records before it; what a partial read cannot
check, the whole decode does, and a file the whole decode accepts gives the same answer on either
path. Each answer reports the segments, blocks, bytes and values it decoded.

`tools/tick_query` serves the queries as JSON lines on stdin and stdout for the harness; its usage
is in `include/tick_store/harness/query_cli.hpp`.

### Worst-case work for one segment

Decoding a random segment k reads its blocks only: at most 39 columns of at most N / 16,384
pieces each, rounded up, their CRCs, then at most N type
values, N locates, timestamps and tracking numbers, at most N values in each typed column but
`imbalance_price` and `mwcb_level` (3 per record) and `imbalance_shares` (2 per record), at most
37 `rare_bytes` per record (the largest rare type, O, is 48 bytes), and the raw records' own bytes,
at most 65,535 per record, then N / 64 time bases, four directory values per locate and 38 + 29 G
seek values. zstd inflates at most 32 bytes a value plus 1,024 per piece, and exactly the byte
count for byte columns. Stored order comes back by a counting sort for the columns grouped by
type, O(N), and a sort of the adds by key, O(A log A) for A adds; file order by each record's
index, O(N). Each reference is one list lookup. Nothing outside segment k is read but the `symbols` section, parsed once at open.

Two codec costs are not linear in the values, and a reader of files it did not write should
budget for them, since the reader takes every dictionary mode in every dictionary column. A
dictionary block in mode 2, move-to-front, turns each rank r into an index by moving r 16-bit
list entries, and r may be up to K - 1 = 2,047 (`kDictMtfMaxEntries` is 2,048), so it costs up
to 2,047 moves, 4,094 bytes, per value, not O(1). A valid block of 2^20 values all of rank 2,047
over a dictionary of 0 to 2,047 is 7,342 bytes and asks for 2^20 x 2,047 x 2 = 4.29 GB of
moves. A block in modes 0 or 1 checks its K entries for a repeat by a hash set that sorts a copy
when entries crowd one slot, O(K log K) at worst, K at most the block's count. The column
decoder reads each block's count before decoding it and refuses one that would pass the piece's
count, so neither cost reaches past the values a piece declares: at most 16,384 values a piece,
which in mode 2 is at most 67 MB of moves. For an untrusted file, then, assume a dictionary column
costs up to 2,047 16-bit moves a value and every other column O(1) a value, on top of what this
section lists (B-1, B-3 and B-4 in `docs/REVIEW.md`).

## Writing

`ItchStoreWriter` takes one message at a time and writes each segment as it fills, so its private
memory is bounded by one segment, never by the day. For a segment of N records it holds the
segment's messages as decoded, with a copy of each raw record's bytes, until the segment is full,
then sorts them into stored order by locate (a counting sort) and builds the columns. It holds 8
bytes for each value (at most 19 per typed record, an I with a stock exception, plus one per rare or raw
byte), the grouping keys (at most 9 bytes per record), and a table of the segment's references,
16 bytes a slot with the slots the smallest power of two at least twice the distinct references
(at most 2N), plus 8 bytes a reference; then, while a column is coded, that column's chunk. The
tables of 65,536 locates are fixed: 256 KiB of list lengths, an 8 KiB bitmap, and the symbol
table's 256 KiB index with 10 bytes per symbol. The container keeps 12 bytes of metadata per block
and each segment summary for the footer. `docs/NOTES.md` has the footprint measured on the full days.
The durability class,
`page_cache` or `device_flush`, is a parameter of `ItchStoreWriter::create` and of `tick_roundtrip
ingest`, never a default. The writer refuses a timestamp over 48 bits and a raw message that is
empty, over 65,535 bytes, or one `decode` would never return raw, changing nothing.
