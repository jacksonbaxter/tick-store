# Comparison harness

Stores one pinned ITCH slice in Parquet (through pyarrow), ArcticDB on LMDB, QuestDB and
tick-store itself, and proves what each run claims. For every variant of every system, and for
durability classes A and B, it:

1. proves the settings in force match the settings requested, and stops if they do not,
2. makes a small commit in a store of its own and reads the dirty pages it left,
3. writes the slice three times, and sizes the last store at rest,
4. reads that store back whole and gives it to the same G-1 oracle as the stub store, which
   must rebuild the source bit for bit.

Then, on the class B store, it runs Q1 to Q5 from `BENCHMARKS.md`, warm and cold, and every
answer must equal a ground truth computed with numpy from the exported columns, sharing no code
with any system (`compare/queries.py`). A failed proof or a wrong answer stops the run. Python
never parses ITCH: `tools/tick_export` writes the column files, and `tick_roundtrip
verify-columns` judges what comes back. tick-store alone ingests the ITCH slice itself, its
natural input, so its write time holds the ITCH parsing the others never do: see "tick-store"
below.

One system runs at a time, each as one process. `compare/run.py` writes
`results/compare/<host>-<slice>-<commit>.json` only when every system has finished and
`scripts/check_compare.py` has passed the file, so a file that exists is complete. A full-day
run, Linux only, does the same on a whole pinned day and writes
`results/compare/<host>-<day>-full-<commit>.json`: see "Full-day comparison" below. On macOS a
full day is tick-store alone, which on Linux is a run of its own: see "Full day of tick-store
alone".

| Path | Job |
|---|---|
| `compare/setup.py` | Builds the environment from a clean checkout |
| `compare/run.py` | Runs every system and writes the results file |
| `compare/run_system.py` | One system through its variants and classes |
| `compare/worker.py` | One step in a process of its own, so a step's peak memory is its own and a cold query opens the engine from nothing |
| `compare/schema.py` | The table, and the column files in Python |
| `compare/queries.py` | Q1 to Q5, one parameter set, and their ground truth |
| `compare/durability.py` | The classes, and the host's strongest sync |
| `compare/settings.py` | Requested against in force, and the verdict on a class |
| `compare/coldcache.py` | Warm and cold proofs, and the size at rest |
| `compare/variant_rule.py` | Which variants a full-day run takes, from the slice file of the same host and commit |
| `compare/fullday.py` | One system through a whole day, as an iteration-only capacity check |
| `compare/guard.py` | Runs a command in its own session, and stops it before it can hurt the host |
| `compare/systems/` | One file per system, behind `compare/systems/base.py`. tick-store's is `tickstore.py` |
| `compare/java/CommitModeProbe.java` | Runs QuestDB's own configuration parser on a conf |

## Setup

```sh
python3 -I compare/setup.py [--vendor DIR] [--venv DIR] [--uv PATH]
```

It needs `uv`. It makes a virtual environment from `compare/requirements.lock`, by hash, fetches
QuestDB and a JDK from their official releases, checks each against its pin in `compare/setup.py`,
unpacks them under `vendor/` and compiles the probe. Nothing goes outside `vendor/` and `.venv/`
unless a directory is given. On Linux, QuestDB bundles its own runtime and the JDK only compiles
the probe.

A run also needs the `bench` build, and the slice and its export:

```sh
cmake --preset bench && cmake --build --preset bench
build/bench/tools/tick_export data/slices/12302019-open.itch data/export/12302019-open
```

`scripts/data_pins.json` records how the slice is cut. A run checks the slice's SHA-256 and proves
through the oracle that the export rebuilds it before anything is loaded.

## Run

A reported run needs a measurement wrapper, given by `TICK_STORE_MEASURE`: a command that takes the
machine, runs its arguments and prints load lines on stderr. Without it the file is marked
iteration-only, goes under `build/compare-iteration/`, and `scripts/check_compare.py` refuses it. A
reported run needs a clean tree, or an exported tree with a `COMMIT` file. Use the environment's
Python: each child runs under the interpreter that started `compare/run.py`.

On macOS, with everything in the checkout's own folders:

```sh
TICK_STORE_MEASURE=<wrapper> .venv/bin/python -I compare/run.py
```

On Linux, with data, downloads and build kept beside an exported tree:

```sh
TICK_STORE_MEASURE=<wrapper> <venv>/bin/python -I compare/run.py \
    --data <dir> --vendor <dir> --work <dir> --tools <dir> --results-dir <dir>
```

The Linux run adds `taskset -c 4` itself, and `compare/run_system.py` then widens the mask to cpu4
and cpu6, so every thread of a system shares those two CPUs. macOS has no pinning.

Nothing keeps cpu4's SMT sibling idle, so the header's `sibling_busy_percent` records how busy it
was, per system: the sibling is read from `thread_siblings_list`, or `core_cpus_list` failing that,
and its busy and total jiffies from `/proc/stat` once the lock is held and when the wrapper exits.
`scripts/check_compare.py` refuses a reported result where any system exceeds 5%, a limit that is
our choice and not a measured threshold, and macOS records `not-available`. cpu6's sibling, cpu7
on debian, is measured the same way from the same first reading and recorded as
`second_sibling_busy_percent`, held to the same 5% rule by the same check from protocol 2. The
files measured before it, protocol 1, record cpu5 alone.

Exit 75 means the wrapper did not get the machine: finished systems are kept, so run it again to
go on. Exit 2 is usage or setup, and anything else is what failed. For iteration, `--systems`,
`--variants` and `--write-repetitions`, `--warm-repetitions` and `--cold-repetitions` narrow a run.
A measured run, with `TICK_STORE_MEASURE` set, refuses all five: it is the whole protocol, and
`scripts/check_compare.py` requires of every reported file exactly the repetitions in
`compare/repetitions.py`, which `compare/run_system.py` runs: 30 warm and 7 cold per query, and 3
writes per class and variant on the slice or 1 on a day. tick-store alone on a day is asked for by
name, `--tickstore-alone`. A system's finished part is reused, when a run is resumed, only if it
was made with the same repetition arguments, variants, scope and measurement.

Afterwards:

```sh
.venv/bin/python -I scripts/check_compare.py results/compare/*.json
.venv/bin/python -I tests/compare/test_real_systems.py -v
```

The second is the failures on the real systems, with no mock. Its first test is the reason
`compare/settings.py` exists: see below.

## The one table

Every system stores one table with one row per record, in file order, so the ratio baseline is
identical. `include/tick_store/store/columns.hpp` defines the column files, and
`compare/schema.py` reads and writes the same form.

| Column | Arrow | QuestDB | Holds |
|---|---|---|---|
| `seq` | int64 | LONG | The record's number in the file, from 0 |
| `ts` | timestamp ns | TIMESTAMP_NS, designated | Nanoseconds from midnight, placed on the trading day. `ts_ns` in the files |
| `msg_type` | int16 | SHORT | The type byte, as a number |
| `locate` | int32 | INT | Stock locate. SYMBOL INDEX in QuestDB's `locate-index` variant |
| `tracking` | int32 | INT | Tracking number |
| `order_ref` | int64 | LONG | Order reference. For `U`, the original one |
| `new_ref` | int64 | LONG | `U`'s new order reference |
| `side` | int8 | BYTE | The buy or sell byte |
| `shares` | int32 | INT | Shares, executed shares or cancelled shares |
| `price` | int32 | INT | The wire's fixed-point price, unconverted. For `C`, the execution price |
| `match` | int64 | LONG | Match number |
| `printable` | int8 | BYTE | `C`'s printable byte |
| `stock` | large_string | SYMBOL | The 8 bytes as sent, padding included |
| `mpid` | large_string | SYMBOL | `F`'s 4-byte attribution |
| `rest` | large_string | VARCHAR | See below |

ITCH gives a timestamp from an unstated midnight. `ts` places it on the trading day at UTC
midnight, and `write_columns` subtracts that again on the way back.

### The 23 message types

A row takes exactly one of three forms. `seq`, `ts`, `msg_type`, `locate` and `tracking` are set
on every hot and rare row.

**Hot: 8 types, the order flow.** Every field is in a typed column and `rest` is empty.

| Type | Columns used, beyond the five above |
|---|---|
| `A` | `order_ref`, `side`, `shares`, `price`, `stock` |
| `F` | `order_ref`, `side`, `shares`, `price`, `stock`, `mpid` |
| `E` | `order_ref`, `shares`, `match` |
| `C` | `order_ref`, `shares`, `price`, `match`, `printable` |
| `X` | `order_ref`, `shares` |
| `D` | `order_ref` |
| `U` | `order_ref`, `new_ref`, `shares`, `price` |
| `P` | `order_ref`, `side`, `shares`, `price`, `match`, `stock` |

**Rare: the other 15 types,** `S R H Y L V W K J h Q B I N O`. `rest` is the message body after
its 11-byte header, as lowercase hex, and every other column holds 0 or is empty. The stock on an
`R`, say, is inside `rest`, and `stock` is empty on that row.

**Raw: anything the other two cannot hold exactly.** That is a payload the decoder calls raw, a
hot field outside its column's range, or a stock or MPID that is not printable ASCII. `rest` is
`raw:` and then the whole payload, type byte first, as lowercase hex. `seq` and `msg_type` stay
set, `ts` repeats the previous row's, or is 0 on the first row, so the column stays sorted, and
everything else is 0 or empty. Since `:` is not a hex digit, no rare row can begin that way.

So `rest` holds nothing on a hot row, a hex body on a rare row, and `raw:` and hex on a raw row. Hex
is lowercase only, so a digit has one spelling and a flipped case bit cannot go unseen.

The pinned slice has 0 raw rows, and 7 of the 15 rare types: `H I L Q R S Y`. A source's bytes
after its last frame go to `tail.bin`, but the table has nowhere to keep them, so a run stops on a
slice with a tail.

### Unused numeric columns hold 0, for every system

A numeric column a row's form does not use holds 0, a text column is empty, and this is the same
for every system, because every system loads the same Arrow table from the same column files.
No null is ever stored:

- The table must be the same input for every system. A system that could store a null more
  cheaply than a 0 would otherwise be given a different table, and the sizes would no longer be
  over the same records.
- A null is not the same thing across the systems. QuestDB's null sentinels are the minimum of
  each width, so the export keeps those out of reach: a hot value that does not fit its signed
  column exactly becomes a raw row. `plain_array` in `compare/schema.py` fails a read-back
  on any null, since none went in.
- The rebuild checks it. A stored value that the rebuild would ignore is still a stored value that
  came back wrong, so `ColumnsParser` refuses any non-zero value in a column the row's form does
  not use.

A 0 is therefore not a "missing" marker. `msg_type` alone says which columns a row uses, and a 0 in
a used column is a real value: every `P` in the slice has `order_ref` 0.

## Durability classes

Class A returns once data is in the OS page cache. Class B returns after a flush to the device.
`BENCHMARKS.md` defines them, and they are never compared with each other.

| System | Class A | Class B | Flush granularity under B |
|---|---|---|---|
| Parquet | Nothing: pyarrow never syncs | The harness flushes the closed file and then its directory, and that flush is timed in the write | One flush per file, after it closes |
| ArcticDB | `ARCTICDB_LMDBStorage_ExtraFlags_int` set to MDB_NOSYNC and MDB_NOMETASYNC, which ArcticDB's docs mark as for throwaway data | The same variable at 0, LMDB's default | One LMDB transaction per storage key: each data segment, index and version key commits alone |
| QuestDB | `cairo.commit.mode=nosync` | `cairo.commit.mode=sync`. `async` never counts, since it is a no-op flush on Linux | Per commit: the client commits about every 100 batches, and each WAL commit is then applied to the table |
| tick-store | `--durability page_cache`: every byte handed to the kernel and the file renamed into place, no flush | `--durability device_flush` | One flush per file, at its commit: the file's data before the rename that publishes it, then its directory |

The primitive beside every class B number:

| System | Linux | macOS |
|---|---|---|
| Parquet, by the harness | `fsync` | `F_FULLFSYNC` |
| ArcticDB, by LMDB | `fdatasync` | `F_FULLFSYNC` |
| QuestDB | `msync(MS_SYNC)` and plain `fsync` | `msync(MS_SYNC)` and plain `fsync` |
| tick-store | `fdatasync` on the file, `fsync` on its directory | `F_FULLFSYNC` on the file and on its directory |

The harness's own primitive is what it calls. tick-store's is what its writer called, recorded call
by call. LMDB's and QuestDB's are stated from what is known of them, and are not traced in a run.

**How a class is proven.** Each run makes a small commit of 65,536 rows in a store of its own, and
reads each file's dirty pages straight after the commit returns. The last full write is read the
same way, before anything else runs in its worker, for every system. Before protocol 2,
tick-store's was read only after its worker had run the tools that report its settings, time in
which writeback could clean pages (review F-11). `compare/settings.py` gives the verdict:

- Class A is proven only if pages are still dirty, since that shows no flush ran.
- Class B is proven only if no data page is dirty, and class A on the same system and host saw
  dirty pages through the same detector, so that a 0 means something. Files a commit leaves dirty by
  design are listed by name in `residual_dirty_files`: `lock.mdb` for LMDB, and for QuestDB the
  partition's column files, the symbol maps and `_meta`. Everything else must be clean.
- Parquet is the exception, because the harness makes the flush itself. macOS starts writing a file
  out when it closes, so a closed Parquet file reads 0 dirty with or without a flush, and dirty pages
  are no proof there. The verdict is the count of flushes made: 2 that succeeded and 0 dirty pages
  under B, and none under A.
- tick-store is proven the same way, for the same reason: its store is one file written and
  closed. `tick_roundtrip ingest` passes its writer a `FileOps` that makes the real system calls
  and records each flush, what it was called on and what it returned, and prints the list. Class B
  is proven only by the file's flush and then its directory's, both returning 0, by the host's
  primitive, and 0 dirty pages; class A only by no flush at all.

`NOT PROVEN` is a valid thing to record, and the validator accepts it as long as the file does not
claim the proof.

**Like-for-like** is a separate field. QuestDB's class B is never like-for-like on macOS, because
its plain `fsync` does not reach the device's cache while LMDB and the harness use `F_FULLFSYNC`.
It also loses the mark on any host where table pages stayed dirty once the rows were readable, since
under `sync` the WAL is what is flushed at commit. `scripts/check_compare.py` enforces both.

## Reading the setting in force

Every setting is compared with what was requested, and the first difference fails the run, naming
all of them. Reading an echo is not enough, because of QuestDB.

**QuestDB** accepts `cairo.commit.mode=not_a_mode` without a word, echoes it through `SHOW
PARAMETERS` and runs nosync. So the commit mode must pass four independent checks, on a running
server: the value in `server.conf` as read back must be a real mode and the one requested, `SHOW
PARAMETERS` must say it came from the conf, its echoed value must match, and
`compare/java/CommitModeProbe.java` must print the same mode, by running the same jar's own
`PropServerConfiguration` on that conf and reading `getCommitMode()`. The last is what sees through
a bad value. The worker counts and echoed conf keys come from `SHOW
PARAMETERS`, the heap and processor count from the running command, and the bind addresses from
`ss` on Linux and `lsof` on macOS. After ingest, `SHOW COLUMNS`, `tables()` and `SHOW CREATE TABLE`
give the stored form. The header's `questdb_runtime` is read the same way on both hosts: the JVM's
vendor and version from the properties it prints to its log as it starts, and jemalloc from the
libraries mapped into the running server, by `/proc` on Linux and `lsof` on macOS.

**ArcticDB**'s settings come from the environment before it loads. The harness reads them back
through `lib.options()`, the library config in storage, and the engine's own `get_config_int`. After
ingest, the stored index gives the largest segment, the map size comes from the meta page of
`data.mdb`, and the first stored segment's bytes are decoded by a plain LZ4 reader and must equal
the engine's first timestamps. The LZ4 acceleration cannot be read: the segment header has no
Python binding in 6.28.2.

**tick-store**'s settings are read from the store just written and from its tools, after the
write's clock stops. `tools/tick_query`, opened on the store, reads the version and the segment
size from the bytes of the store section, and the records from its header; it names the kernel set
it chose, with `TICK_STORE_SIMD` set to the level the processor's own flags give (`neon` on arm64,
`avx2` on an x86-64 with AVX2), and one share sum counts its kernel calls by set, which must be that
set alone. The durability mode and the flushes made come from `tick_roundtrip ingest`'s report.
`tick_roundtrip layout --blocks` gives every block's codec from the footer, and each column's must
be the one `docs/store.md` lists, or zstd where it allows it. Each query worker checks the
version, segment size, path and kernel set again as it opens. The same `not_a_mode` failure is
shown on the real tools in `tests/compare/test_real_systems.py`: a store of another segment size,
a class B store written in `page_cache`, and a kernel cap below the one asked for are refused, and
`--durability not_a_mode` and `TICK_STORE_SIMD=not_a_level` stop the tools.

**Parquet**'s thread counts come from `pa.cpu_count()` and `pa.io_thread_count()`. After ingest, the
footer gives each column chunk's compression, dictionary, statistics, page index and delta
encoding, the format version, the sort order and each row group's rows, and the first page header
in the file gives the data page version. The compression level cannot be read, since the footer
has no level, so a result records it as requested and says so.

## A write's two times

The write protocol is the same for every run: tick-store's source is read through and wholly
resident before its clock, on the slice and on the day, and the other systems read the export
inside theirs, on a day chunk by chunk and partly from the device. The input's cache state is
therefore not the same for all, so every write records two times, and from protocol 2 each
repetition keeps both:

- `seconds`, the whole clock, the headline write time;
- `engine_only_seconds`, `seconds` less `python_seconds`: the clock without reading the input
  or putting it in the form the system's call takes.

| System | `seconds` holds | `engine_only_seconds` holds |
|---|---|---|
| Parquet | `pq.ParquetWriter` writing the zero-copy Arrow view of the mapped export (on a day, reading each chunk of the export into Arrow first), and under class B the harness's flush of the file and then its directory | The writer and, under class B, the flush: on the slice the total, since `python_seconds` is 0; on a day the total less reading the chunks |
| ArcticDB | Building the pandas frame `lib.write` takes (one symbol per security: the stable sort by locate and a frame per locate; on a day, reading each chunk, and for per security spreading the day by locate into bucket files), then `lib.write`, `lib.append` or `write_batch` and `list_symbols`, each storage key an LMDB commit | ArcticDB's write, append, `write_batch` and `list_symbols` calls and LMDB's commits, without the reading, the frames or the partitioning |
| QuestDB | Dictionary-encoding the SYMBOL columns (on a day, after reading each chunk), sending the rows through the client, and waiting until the WAL is applied and every row is visible | The client's send, the server's WAL apply and the wait until every row is visible, without the reading or the dictionary encoding |
| tick-store | `tools/tick_roundtrip ingest` as a process, from its start to its exit: mapping the ITCH source, parsing it, coding and writing the store, its commit and, under class B, its flushes | `tick_roundtrip`'s own wall time: the same less the process's start and exit. The ITCH parsing stays in it |

`engine_seconds`, kept beside them, is each system's own split and is not the same thing for all:
Parquet's leaves out the class B flush. Each repetition also records `input_pages` and
`input_resident_pages_at_start`, read by `mincore` on a fresh mapping of the input (the export's
column files, or tick-store's ITCH file) once it is made ready and just before the clock, and
`device_bytes_read` over the write: on Linux the block device's sectors read, machine-wide; on
macOS the bytes the device read for the processes that did the write, each one's own count
(`proc_pid_rusage`'s `ri_diskio_bytesread`): the worker, QuestDB's server, and tick-store's
`tick_roundtrip`, read once it has exited and before it is reaped, since macOS adds no reaped
child's reads to its parent's. `iostat`'s `disk0` total, which the cold proof uses, counts the
write's own output too, so `scripts/check_compare.py` refuses it for a protocol 2 macOS write
(review RF-5). A process's own executable and libraries count when they are not cached; in a run
the flush probe has run the same tool first. Each write's `times` says what its two times hold,
as the table does.

On the slice every system's input is made wholly resident before its clock: tick-store's ITCH
slice, and the export's column files for the other three, read through by `tick_coldcache warm`,
with the write refused unless `mincore` then reads every page resident. The record of it is the
write's `input_at_clock_start.made_ready`, and `scripts/check_compare.py` requires
`input_resident_pages_at_start` to equal `input_pages` for every system on a protocol 2 slice.
The files before protocol 2 stated that the export was warm before every write and never checked
it (review F-10). On a day the export is taken as the cache holds it and only recorded. The
reading just before the clock is held to the same rule in the worker: tick-store's source on any
run, and every input on the slice, must be whole there, or the write stops before its clock
rather than when the file is validated (review RF-11). On Linux a write's device reads come from
the stat file of the block device under its input, so an input on a file system with no block
device of its own, such as tmpfs, overlayfs or a btrfs subvolume, stops the write with an error
naming it.

**A write target is met against another system only if tick-store is no slower on both times.**
`BENCHMARKS.md`'s targets are not reworded; the rule is how they are judged, by
`scripts/render_results.py`, whose tables show both columns. The files measured before protocol 2
keep the total, the medians of `engine_seconds` and `python_seconds`, and tick-store's
`input_resident`, but no engine-only seconds, residency of the others' input or device reads.

## Warm and cold proof

Neither kind of number can reach a results file without its proof: `compare/coldcache.py` raises
instead, and `scripts/check_compare.py` checks the proofs again.

**Warm.** `tick_coldcache warm` reads every store file through, with the server running. `mincore`
on a fresh mapping must then read 100% of pages resident, both before and after all 30 repetitions
of each query. Each query first runs once untimed, its answer checked, as on a full day, so that
no timed repetition is the engine's first call in the worker; the proof records `warm_up_runs`,
and each query its warm-up's seconds. The files before protocol 2 had no warm-up on the slice, so
their first repetition paid each engine's first-call set-up, and with 30 runs their tail figure,
the slowest, could be that call (review F-3).

**Cold.** Seven repetitions per query:

1. The service stops, and every file is flushed.
2. `tick_coldcache evict` removes every file from the cache. On macOS that is `msync(MS_INVALIDATE)`
   on a fresh read-only `MAP_SHARED` mapping. On Linux it is `fsync`, then
   `posix_fadvise(POSIX_FADV_DONTNEED)` over the whole file. Either way it repeats, up to 32
   passes with a wait between them that doubles from 20 ms to 1 s, until `mincore` reads 0 on
   every file. It was 16 passes 20 ms apart until a full day's QuestDB store outlasted that:
   see "Full-day comparison" in `docs/NOTES.md`. Then, 1 s later and with nothing of ours running,
   the store must still read 0. A store that keeps or regains pages is being read by something
   else: on `debian`, the host's hourly backup read a new day's store, which takes it
   minutes. The eviction then runs again, 5 s later and doubling to 60 s apart, for at most
   30 minutes, and fails the run past that. Each proof records its attempts and the seconds
   waited.
3. The device counter is read, the service starts again where there is one, and a fresh process
   opens the store and runs the query once.
4. The pages now resident can only have come from the device, so the device must have moved at
   least that many bytes. A shortfall is conclusive and fails the run. Only files the engine merely
   reads count: LMDB's lock file, which it writes as it opens, does not.
5. With the service stopped, the store must not gain a page over 1 s. If it does, something else
   was reading it, and the run may have found pages it did not read itself. That repetition is
   not counted and runs again, at most 10 times per query. Each query's cold block keeps the pages
   gained by each repetition run again, as `run_again_refilled_pages`.

The Linux counter is the block device's sectors read. The macOS counter is `iostat`'s total for
`disk0`, which is machine-wide, counts reads and writes together, and has two decimals of a MiB, so
it can only ever show a shortfall. An excess proves nothing on either host. File system metadata
stays warm, because neither method reaches it.

Each cold number is a fresh process's query, and as much of the store's open as each system
lets the clock hold. The server's start and the worker's imports fall before the clock, and pages
they touched count as resident and as device reads. `pq.read_table` and `Table.group_by` import
`pyarrow.dataset` and `pyarrow.acero` on their first call, so `ParquetSystem.open` imports both
first, and `tests/compare/test_system_worker.py` fails if any module loads inside the timed call.
The clock does hold what an engine sets up on its first call in a process: for Parquet on macOS,
about 1 ms over a second call on warm pages.

Whether the clock holds the system's whole open of its store differs, and each system's module
says so in `open_inside_cold_clock`:

| System | Whole open inside the cold clock | Why |
|---|---|---|
| Parquet | Yes | Its client holds no file: every `pq.read_table` opens the file and reads and parses its footer inside the timed call |
| tick-store | Yes | `tools/tick_query`'s open maps the store, reads and checks the footer, builds the segment directory and parses the symbols before its ready line, which reports how long that took; a cold figure is that open plus the query's round trip, and the cold block keeps both parts, repetition by repetition, in `parts` |
| ArcticDB | No | The worker's `open` opens the LMDB environment and reads the library's config, and per security lists its symbols, before the clock; `lib.read` then reads the version and index keys inside it |
| QuestDB | No | The server starts before the worker and opens the table's files as it starts and as the client connects |

Every cold proof records the store's bytes resident just before its clock started,
`resident_bytes_before_clock`, beside the bytes resident before the worker's open and just before
the query. Where the clock holds the whole open, `scripts/check_compare.py` requires it to be 0,
and it is the bytes read before the open; where it does not, it is what the open had made
resident, and it counts in the device's reads, as before. The tick-store figures before protocol
2 are the query alone, the open outside the clock: on the full day's store the footer it reads is
about 5 MB (review F-1).

## tick-store

tick-store runs through its own tools and nothing else: store version 3, the indexed query path,
CRCs checked on every read. Its headline, `v3-seg4Mi`, is the writer's default segment of 2^22
records with the eight hot dictionary columns held to ranked PFOR, the layout the rule in "Layout
experiments" in `docs/NOTES.md` chose before it was measured, and it stays fixed in
`compare/systems/tickstore.py`. Its tuning variant, `v3-seg1Mi`, is the same writer at 2^20
records, the segment size it replaced, which the layout experiments found faster on Q1 and Q4 and
slower on Q2, Q3 and Q5, so the comparison itself shows that trade. Both write the hot columns
the same way, which no result reads back: the footer names each block's codec id, not its
dictionary mode. The slice results from `25a462b` ran 2^20 as their only variant, under the
same name, before the layout experiments.

- **Ingest.** `tick_roundtrip ingest <slice> <store> --durability page_cache|device_flush
  --segment-records 4194304` (1048576 for `v3-seg1Mi`), run and timed from Python as every other
  write is, with the slice
  read through first, as the others' column files are. The clock holds the ITCH parsing, which
  the other systems never do, since they load the columns `tick_export` already parsed. Nothing is
  subtracted, and every results file says so in `input.ingest_input` and in each write's
  `detail.input`. `python_seconds` is the tool's start and exit, outside its own clock.
- **Read-back.** `tick_export --store` writes the store's records as the column files, each record
  through the store's whole read path and the ITCH encoder, and the harness then writes and judges
  them exactly as it does every other system's. G-1, `tick_roundtrip verify` of the store against
  the source byte for byte, is stronger, and runs as well: it is in `stored_settings.g1`.
- **Queries.** One `tools/tick_query` per store, started in the worker. Its open is outside the warm
  clock, as the other clients' are, and its time is recorded once per worker, in the variant's
  `warm_worker_open`; a cold figure is a fresh process's open, as the ready line reports it, plus
  the query (see "Warm and cold proof"). Not every client is opened before the clock: Parquet's
  opens the file inside every call. Each query is one request line with `"ack": true`, timed from
  Python as a round trip that stops on the worker's line saying the answer is in its memory. The worker then
  digests the answer by `compare/queries.py`'s rule, outside the clock, as the other systems'
  digests are made in Python: on the slice that digest of Q1's 527,644 rows takes longer than the
  engine. It returns no rows to Python, so a rows digest is the worker's own, and it must still
  equal the numpy ground truth; an aggregate's answer comes back whole and is digested in Python
  too. The worker's engine time is recorded beside every warm and cold round trip, as `engine`,
  and is not the number compared. Cold runs through `tick_coldcache` as for the others.
- **Threads.** The writer and the query worker each run on one thread; the others have two.

## Tuning variants and their sources

Each system runs every variant below, and the headline variant of each is fixed in its module, as
it was before these were added. The settings come from section 7 of
`research/evidence/column-systems.md`, which quotes each system's documentation.

| System | Variant | What it tests | Source |
|---|---|---|---|
| Parquet | `none-rg1Mi` | No compression codec, against snappy and zstd | Section 7 sweeps "none, snappy (pyarrow default), zstd level 1 (Arrow default), one higher level". pyarrow's `compression` lists `'NONE'` among its valid values (`python/pyarrow/parquet/core.py` lines 778 to 780, Arrow 26.0.0), and the footer reads `UNCOMPRESSED` |
| Parquet | `zstd1-rg1Mi-ts-default` | `ts` on the writer's default encoding, against `DELTA_BINARY_PACKED` | Section 7: "Time column: `column_encoding={'ts': 'DELTA_BINARY_PACKED'}`, one run vs default". The default is `use_dictionary`, "default True", and "if the dictionary size is too large, the column will fallback to ``PLAIN`` encoding" (`core.py` lines 772 to 776), past the 1 MB dictionary page limit |
| tick-store | `v3-seg1Mi` | Segments of 2^20 records, against the headline's 2^22, both with the hot columns held to ranked PFOR: faster on Q1 and Q4, slower on Q2, Q3 and Q5 in the layout experiments | "Layout experiments" in `docs/NOTES.md`, experiment 1 and the final round, scored by a rule committed before any of them was measured. tick-store's own measurements, not a documentation source, since it is ours |
| ArcticDB | `rows100k-cols127-per-security` | One symbol per security, against one symbol for the whole table | ArcticDB's [data organisation](https://docs.arcticdb.io/latest/tutorials/data_organisation/) lists "Separate Symbol for Each Security": "Every security has its own symbol with the same data shape", "Ideal for single security analysis", and "Analysis involving many securities requires reading many symbols". Section 7: "Run one symbol for all, and one symbol per security". `Library.write_batch` and `Library.read_batch` are its documented calls for many symbols (docstrings in `arcticdb/version_store/library.py`, 6.28.2) |

**ArcticDB per security** needs a query path of its own, and it is ArcticDB's documented one. The
write sorts the table by locate, stably, and gives `write_batch` one frame per locate, its rows in
file order, under the symbol `locate-NNNNN`: 8,907 symbols on the slice. It then calls
`list_symbols` once, inside the clock, because ArcticDB warns after many writes that the call
compacts its symbol list and needs write access, and a query must not be the first to pay that. A
one-security query (Q2, Q3) reads that symbol alone, with no filter. A query over every security
(Q1, Q4, Q5) is one `read_batch` over every symbol, the list of which the worker takes from
`list_symbols` when it opens, before any clock, as an application keeps its universe. The cost is at
ingest under class B: LMDB commits each storage key in its own transaction, so every symbol costs
flushes of its own. In the slice results at `25a462b`, the median write took 5.85 s in class A and
216.9 s in class B on the Mac, against 0.48 s and 0.52 s for one symbol, and 14.95 s and 133.7 s on
Linux, against 1.05 s and 1.21 s. The store took 235,167,744 bytes against 141,860,864 on the Mac,
and 208,986,112 against 141,168,640 on Linux. A one-security query gains: Q2 warm 1.8 ms against
39.3 ms on the Mac, 8.4 ms against 139.7 ms on Linux. A query over every security loses: Q1 warm
1.05 s against 7.4 ms on the Mac, 5.77 s against 27.4 ms on Linux.

## Full-day capacity

A run on a whole day answers one question: does the system fit on the host and survive? It is not a
benchmark. `compare/fullday.py` needs the day's export at `<data>/export/<day>-full`, and runs under
`compare/guard.py`, Linux only:

```sh
<venv>/bin/python -I compare/guard.py --report <file> --samples <file> --watch <dir> -- \
    <venv>/bin/python -I compare/fullday.py --system questdb [--heap 1g] [--durability B] \
    --data <dir> --vendor <dir> --work <dir> --tools <dir>
```

- Ingest streams: `table_chunks` in `compare/schema.py` reads chunks of about 4 M rows into memory
  of their own, and each system's `ingest_stream` holds one at a time. ArcticDB's first chunk is a
  write and the rest are appends, and its LMDB map is 64 GB, because the slice's 2 GB cannot hold a
  day. Every chunk is a whole number of row groups or segments, so `stored_settings` applies as is.
- One full-column scan and one window run, each against the ground truth, then `stored_settings`.
  There is no whole-store read-back, since that needs a day on the heap, and no warm or cold proof.
- tick-store and ArcticDB's per-security layout have no streamed ingest here, and are not part of
  this check.
- QuestDB starts through `bin/questdb.sh` with `JVM_PREPEND`, not from `java` directly, and writes a
  GC log per server start. It is otherwise the same server: the prepended options are the ones
  `QuestDBServer.command` passes.
- The guard samples every 2 s. It sends the session SIGTERM, then SIGKILL after 90 s or under
  2 GiB, on any of: (a) `MemAvailable` under 3 GiB; (b) swap-in, `pswpin`, over 10 MiB/s for three
  samples in a row; (c) swap exhausted, `SwapFree` under 2% of `SwapTotal`, with `MemAvailable`
  under 3 GiB plus `SwapTotal`; or free disk under 50 GiB. Swap-out alone never stops it: the
  kernel paging idle memory out while plenty is available is reclaim, not exhaustion. Its samples
  and report keep swap-in and swap-out apart. It exits 70, and its report says why. Each exit
  status names its signal. The rule and its thresholds were registered in "The guard's swap
  rule" in `docs/NOTES.md` before any run under it. Until then, the guard stopped on swap traffic,
  in and out together, over 10 MiB/s for three samples.
- Output stays under `<work>/fullday/<tag>/`, marked `iteration-only`. `compare/fullday.py` refuses
  a work folder under any `results` folder, and `scripts/check_compare.py` refuses the file.
  `results/fullday-capacity-12302019.json` is the curated summary, and sits outside `results/compare/`.

**QuestDB's heap on Linux is `-Xms1g -Xmx1g`, the Phase 3 setting.** It held for a full day in class
A and in class B, with no collection after start-up and at most 300 MiB of 982 MiB in use when ingest
ended. QuestDB's default heap was not tested, because nothing pointed to it. The evidence is in
`results/fullday-capacity-12302019.json`. Each Linux results file states its heap and cites that
file, and `scripts/check_compare.py` refuses a heap other than the one the file records as checked,
or one the running command did not hold. A macOS file states its own heap, and that this check is
Linux only.

## Full-day comparison

The headline comparison: every system on the whole of `12302019`, 268,744,780 records, on
debian, Linux only. It is the slice run on a day, with these differences, each forced by the day's
size or by the host:

```sh
TICK_STORE_MEASURE=<wrapper> <venv>/bin/python -I compare/run.py --day 12302019 \
    --variant-source <results>/Linux-12302019-open-<commit>.json \
    --data <dir> --vendor <dir> --work <dir> --tools <dir> --results-dir <results>
```

- **Input.** The day's ITCH file, checked against its pin, and its whole export at
  `<data>/export/12302019-full`, which `tools/tick_export` makes and the oracle proves rebuilds
  the day before anything is loaded. Every ratio is over the day's 8,251,407,909 source bytes.
- **Queries as defined, nothing clipped.** Q2 and Q3 run over the hour `[09:30:00, 10:30:00)`, the
  busiest locate is the busiest over that hour of the day, and Q5 sums shares over every record.
  The ground truth is the same numpy code over the full export. The parameters, the hour, the
  window the locate was chosen over and Q5's row count are in the file, and
  `scripts/check_compare.py` refuses a full-day file with any window clipped.
- **Writes stream.** No day-sized table is built: Parquet, ArcticDB and QuestDB take the export in
  chunks of their stored unit, as in the capacity check, and their clock holds reading each chunk
  from the column files, since 24 GB of columns cannot stay in this host's page cache. ArcticDB's
  chunk is whole segments, about 4 M rows, or one segment where a segment is larger: at
  `e2ad871` it was 40 segments whatever their size, so `rows10M-cols127` took the whole day as
  one frame, which passed 11.9 GB before the guard stopped it.
  tick-store's write is the same command on the day's ITCH file, read through first and resident,
  as on the slice. If one read through leaves it short, the day's export, which that write never
  reads, is evicted and proven at 0 resident, and the file is read through again, all before the
  clock; a file still short stops the run, naming its pages resident against those wanted,
  MemAvailable, MemFree and Cached, and what else under the data and work folders holds the page
  cache. `input_resident` in the write's detail records the reads and any eviction. On debian at
  `ca96b36` one read left 1.71 to 1.73 M of the file's 2.01 M pages resident, because reclaim
  kept about 5.1 GB of the export the oracle and the ground truth had just read and dropped the
  file's own pages ("The full day's source precondition" in `docs/NOTES.md`). The other systems'
  writes read the export inside their clocks and have no such precondition, so nothing is evicted
  before them. Every write records both its total and its engine-only seconds, its input's
  residency when its clock started and the device's reads while it ran: see "A write's two
  times". ArcticDB's per-security layout spreads the day by locate into bucket files of
  about one chunk each, then writes each bucket's securities whole in one `write_batch`, so each
  security is still written once, in file order; that partitioning is inside its clock, as the
  slice's sort is.
- **One write per class.** Each is a sustained run of minutes over the whole day, and three of
  each for every variant would not fit the run (below). A write's median is that one write.
- **Read-back streams.** Each store is read back one time window of about 4 Mi rows at a time,
  each window ordered by seq, into one column directory, which `tick_roundtrip verify-columns`
  judges against the day as on the slice; tick-store's goes through `tick_export --store` straight
  into the directory, and G-1 runs on its store too. A window is a range of times, so rows of one
  nanosecond never straddle two, and the export's times never decrease in file order.
  Before each read-back the day's ITCH file and export are evicted from the page cache, proven
  at 0 resident, with MemFree recorded either side: what the read-back allocates then comes from
  free memory and not from reclaim, which on this host also swaps. At `e2ad871`, with 8 Mi-row
  windows and nothing evicted, the guard stopped ArcticDB's first read-back on swap traffic of 18
  to 30 MB/s with 12.4 GB still available.
- **Class A's store goes after its read-back,** since queries run on class B alone and two stores
  of QuestDB's 23 GB beside a 24 GB read-back would crowd the disk the guard watches.
- **Warm is proven query by query.** QuestDB's store, 22.9 GB in the capacity check, is larger
  than this host's 15 GB of memory, so no store is held resident whole. Each query runs once
  untimed, its answer checked, and then the processes that answer it must cause 0 bytes of
  storage reads across all 30 repetitions: the worker, tick-store's `tick_query` or QuestDB's
  server, each by `read_bytes` in its own `/proc/<pid>/io`, which counts page faults on a
  mapping and readahead in the process that caused them, and any child the worker waited for.
  The block device's machine-wide sectors-read count is kept beside it in every attempt. An
  attempt where they read anything is kept in the result, with what was read, and the query
  runs again, up to three attempts; a third failure stops the run. Residency of every store file
  is recorded before and after. Before the warm queries the day's ITCH file, its export and
  every store file are evicted from the page cache, the store flushed first, each proven at 0
  resident, and MemFree is recorded either side, so that each query's untimed run reads what it
  needs into free memory.

  Both are what the first two full-day runs found. At `193177b` the proof was the machine-wide
  count alone, with nothing evicted: Parquet's Q2 failed all three attempts with 782,336, 274,432
  and 73,728 bytes read, its store's resident pages steady, with almost no memory free. At
  `953f3d4`, with the page cache emptied and 11.7 GB free, it failed again with 4,096, 4,096 and
  987,136 bytes, the store's resident pages again steady: reads on a shared host that the
  query did not make. A machine-wide count cannot tell them apart; the processes' own can.
- **Cold is as on the slice:** every store file evicted and proven at 0 resident, a fresh process,
  and the device's count must reach what became resident.
- **The guard.** Each system runs under `compare/guard.py`, inside the measurement wrapper and
  under the same pin, with the capacity check's limits. Its report gives each system's peak
  memory: the largest VmHWM of each process it saw, the largest sum of their RSS at one sample,
  and MemAvailable at its lowest, and the pages it saw swapped in and out.
- **Swap per phase.** Every results file, slice or day, counts the host's pages swapped in and
  out over each phase: write, read-back, warm queries and cold queries, for each variant and
  class, from `/proc/vmstat` on Linux and `vm_stat` on macOS. The counts are host-wide. Each
  query also counts them over its timed runs alone, as `swap_timed` in its warm and cold blocks.
  A query with any page swapped in while timed is flagged, the same for every system: its number
  is kept and marked, and RESULTS.md marks it `(swap-in)`. `scripts/check_compare.py` requires
  every count, and a flag that follows from it.
- Settings read-back, the heap rule, the durability proofs, the sibling meter and the load lines
  apply exactly as on the slice.

**The variant rule, written before any full-day run.** The slice runs every variant of every
system. On the day that does not fit: estimated from `results/fullday-capacity-12302019.json` and
the Linux slice timings at `25a462b`, counting both classes, one write each, both classes' read-
backs and the oracle, and the queries, every variant would take about 13.5 hours of measurement
on debian, and about 20 with three writes per class, against a guide of about 10.

| System | Variants and minutes, each with both classes |
|---|---|
| Parquet | 7 streamable variants at 34 to 41 each (write 3.1 min a class, read-back and oracle 10, queries 7), about 250. `zstd1-rgall`, one row group for the whole input, cannot stream a day |
| ArcticDB | headline 43, `rows10M-cols127` 52, `rows100k-cols1` 72 (class B writes 5.3 times the headline's on the slice), per security 98 (partition and writes 33, read-back over 8,907 symbols 28, Q1, Q4 and Q5 over every symbol 37), about 265 |
| QuestDB | `plain` 87 (writes 10.5 and 16 min from the capacity check, settle, read-back 38, cold restarts 8), `locate-index` 95, about 182 |
| tick-store | 43 each (write about 4 min a class, `tick_export --store`, the oracle and G-1 15 min a class), 86 |
| Every system | the source's SHA-256 and the export's oracle before it starts, 6 each, 24 |

So a full-day run takes, for every system alike, **its headline variant and every other variant
of that system that won a query on the Linux slice at the same commit**: among that system's
variants, both the lowest median and the lowest tail figure of one query, warm or cold, the test
the "Lowest" column and QL-4 use. At the slice's 30 warm and 7 cold repetitions the tail figure
is the slowest run, the slowest of 30 or of 7: see "What the numbers are". `compare/variant_rule.py` applies it to the slice file named by
`--variant-source`, which must be this host's slice results at this commit; the file records the
rule, the slice file and each system's winners, and `scripts/check_compare.py` applies the rule
again to that slice file and refuses a file whose variants differ. On the slice at `25a462b` the
rule gives Parquet `zstd1-rg128k` and `none-rg1Mi`, ArcticDB `rows10M-cols127` and per security,
QuestDB `locate-index`, and tick-store `v3-seg1Mi` (its only variant then), about 9.8 hours by the
table above; the run takes whatever the slice at its own commit gives.

At `e2ad871` the Linux slice gave more: Parquet
`zstd1-rg128k`, `snappy-rg1Mi`, `none-rg1Mi` and `zstd1-rg1Mi-ts-default`, ArcticDB
`rows10M-cols127` and per security, QuestDB `locate-index`, and tick-store `v3-seg1Mi`, about 11.2
hours by the table above, over the guide. The rule is kept as written and the run takes them all.
Wins between close variants move from one slice run to the next: `snappy-rg1Mi` and
`zstd1-rg1Mi-ts-default` each won by one cold measure.

At `dd5de38`, the commit of the full-day results, the Linux slice gives Parquet `zstd1-rg128k` and
`none-rg1Mi`, ArcticDB `rows10M-cols127`, `rows100k-cols1` and per security, QuestDB
`locate-index`, and tick-store `v3-seg1Mi`: about 11.2 hours by the table above, over the guide,
run as the rule gives.

At `e0b7df2`, the commit of the guard's new swap rule, and again at `4b5ade4`, the Linux slice
gave Parquet `zstd1-rg128k`, `zstd9-rg1Mi` and `none-rg1Mi`, ArcticDB `rows10M-cols127` and per
security, QuestDB `locate-index`, and tick-store `v3-seg1Mi`. Against `dd5de38`, Parquet gained
`zstd9-rg1Mi`, which won Q5 cold, and ArcticDB lost `rows100k-cols1`, which won nothing.

At `7a2e092` the list changed again: Parquet `zstd1-rg128k`, `snappy-rg1Mi` and `none-rg1Mi`,
ArcticDB `rows10M-cols127` and per security, QuestDB `locate-index`, and tick-store `v3-seg1Mi`.
`snappy-rg1Mi` won Q2 cold, and no other Parquet variant beat the rest on Q5 cold, so
`zstd9-rg1Mi` dropped out. That full day was lost to the host's backup reading QuestDB's store.

At `67ae574`, the commit of the full-day results, the list changed before the run: Parquet
`zstd1-rg128k` and `none-rg1Mi`, ArcticDB `rows100k-cols1` and per security, QuestDB
`locate-index`, and tick-store `v3-seg1Mi`. Against `7a2e092`, Parquet lost `snappy-rg1Mi`, and
ArcticDB lost `rows10M-cols127` and gained `rows100k-cols1`, as at `dd5de38`. By the table above
that is about 10.3 hours: Parquet three variants, about 112 minutes, ArcticDB 213, QuestDB 182,
tick-store 86, and 24 for the checks before each system.

At `dd5de38` the full day has not finished. The guard stopped QuestDB `plain` twice on swap
traffic: in its class A write, and on a freshly booted host in its read-back. Both times the kernel
paged out QuestDB's own idle heap, 0.65 to 0.93 GB, to keep the store's mapped pages. The free swap
filled in about 6 s, at up to 81 MB/s, with 11.6 GB or more still available. The capacity check met
none of this, because swap was already full when it started. See "Full-day comparison" in
`docs/NOTES.md`. The guard's swap rule then changed, as registered in "The guard's swap rule" in
`docs/NOTES.md` before the rerun: swap-out alone no longer stops a run, and QuestDB's heap stays
`-Xms1g -Xmx1g`.

**Reproducing tick-store's Linux figures at a later commit.** `--variant-source` wants Linux slice
results of the same commit, and the only ones committed are `67ae574`'s, so at any other commit
run tick-store alone, which runs both its variants, the headline `v3-seg4Mi` and `v3-seg1Mi`, the
pair the rule chose at `67ae574`:

```sh
TICK_STORE_MEASURE=<wrapper> <venv>/bin/python -I compare/run.py --day 12302019 \
    --tickstore-alone \
    --data <dir> --vendor <dir> --work <dir> --tools <dir> --results-dir <dir>
```

On disk first: the decompressed day at `<data>/12302019.NASDAQ_ITCH50` from
`scripts/fetch_data.py --day 12302019`, 8.25 GB, which the run checks against its pin; its export
at `<data>/export/12302019-full`, 23.9 GB, from `<tools>/tick_export <data>/12302019.NASDAQ_ITCH50
<data>/export/12302019-full`, which the oracle proves before anything is loaded; the `bench`
build's tools in `<tools>`; the environment `compare/setup.py` makes; and at least 50 GiB free
on the work folder's disk, the guard's floor. One variant took 32 minutes on debian at `4c049c7`,
the checks before it included, so the pair takes a little over an hour. The run measures
tick-store exactly as the four-system day did, with the guard and the warm proof by each
process's storage reads, and ends as the macOS day of tick-store alone does: it validates the
document as tick-store alone and writes `<results>/Linux-12302019-full-tickstore-<commit>.json`,
exit 0. Read tick-store's figures from it beside its block in
`results/compare/Linux-12302019-full-67ae574f5666.json`. Before the review's F-5 the same run
took `--systems tickstore --variants v3-seg4Mi,v3-seg1Mi`, was validated as a four-system day,
exited 1 and kept a `.pending` file; a measured run now refuses those arguments.

## Full day of tick-store alone

BENCHMARKS.md gives WT-1, WT-2 and QL-1 to QL-3 the host "Both", and the four-system full day
runs on Linux only: its warm proof and its guard read `/proc`. On macOS the same day runs with
tick-store by itself:

```sh
TICK_STORE_MEASURE=<wrapper> .venv/bin/python -I compare/run.py --day 12302019 --tickstore-alone
```

It writes `results/compare/<host>-<day>-full-tickstore-<commit>.json`. Everything is the full day
above, on tick-store's own path: the day's ITCH file and its export checked against the pin and
the oracle, one write per class through `tick_roundtrip ingest`, the store read back whole through
`tick_export --store` and judged by `tick_roundtrip verify-columns`, G-1 on the store, Q1 to Q5
as BENCHMARKS.md defines them with nothing clipped, each answer against the numpy ground truth, and
cold through `tick_coldcache` with the eviction, the quiet second and the refill check. Three
things differ, and the file states each:

- **Every variant, no rule.** With no other system there is nothing for the slice rule to rank, so
  it runs every variant tick-store has, the headline first (`compare/variant_rule.py`).
- **Warm by residency.** Before the warm queries the day's ITCH file, its export and the store are
  evicted, as on Linux; then every store file is read through and must be 100% resident before and
  after all repetitions, the slice's proof. The store, 1.76 GB, fits the Mac's 64 GiB many times
  over, so the proof that a store larger than memory forced on Linux is not needed.
- **Peak memory from `time -l`.** The guard reads `/proc`, so `compare/run.py` runs the system's
  whole run under BSD `time -l`, inside the measured command, and records its maximum resident set
  and the runner's peak memory footprint.

The same flag runs it on Linux, where nothing differs from the four-system day but the other three
systems: every variant, the warm proof by each process's storage reads and peak memory from the
guard, as above. `scripts/check_compare.py` refuses any other system in it, a variant left out, the
four-system rule stated, on macOS the per-process warm proof or peak memory without `time -l`, on
Linux the warm proof by residency or peak memory without the guard, a heap claimed for a QuestDB
that did not run, and either kind under the other's name. Its numbers rank nothing: RESULTS.md
marks its queries "not ranked: alone".

## What the numbers are

The full-day file is the one the targets are judged on. Slice numbers prove that the harness
works; they are not claims about a full day, and no target changes because of them. Every latency
carries its median, its tail figure and its mean. The tail figure, `tail_us`, is the 99th
percentile by nearest rank, so it is a value that really occurred, and below 100 runs that rank is
the last: with 30 warm and 7 cold repetitions it is the slowest of 30 and the slowest of 7, never
a 1% tail. Each block records its `n`, the rank used (`tail_rank`), whether the figure is the
slowest run (`tail_is_slowest`) and what it is (`tail`, such as "slowest of 7"). `p99_us` holds the
same figure under its old name, which the files measured before the final review's protocol use;
those files carry no `protocol` in their header and count as protocol 1, and every file since says
`"protocol": 2`. `scripts/check_compare.py` accepts no protocol only from those files' commits,
`67ae574` and `2503d1e`, listed in its `PROTOCOL_1_COMMITS`, and any stated protocol only as the
integer 2. The slice is five minutes long, so Q2 and Q3 are
clipped from an hour to `[09:30:00, 09:34:30)`, and Q5 scans 6,125,294 rows and not a day's
268,744,780. Every results file says which it is, and `scripts/render_results.py` never puts a
full-day number and a slice number in one table. What the runs found is in "Comparison harness",
"tick-store in the comparison (slice)" and "Full-day comparison" in `docs/NOTES.md`.
