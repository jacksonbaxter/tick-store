# Comparison harness

Stores one pinned ITCH slice in Parquet (through pyarrow), ArcticDB on LMDB and QuestDB, and
proves what each run claims. For every variant of every system, and for durability classes A
and B, it:

1. proves the settings in force match the settings requested, and stops if they do not,
2. makes a small commit in a store of its own and reads the dirty pages it left,
3. writes the slice three times, and sizes the last store at rest,
4. reads that store back whole and gives it to the same G-1 oracle as the stub store, which
   must rebuild the source bit for bit.

Then, on the class B store, it runs Q1 to Q5 from `BENCHMARKS.md`, warm and cold, and every
answer must equal a ground truth computed with numpy from the exported columns, sharing no code
with any system (`compare/queries.py`). A failed proof or a wrong answer stops the run. Python
never parses ITCH: `tools/tick_export` writes the column files, and `tick_roundtrip
verify-columns` judges what comes back.

One system runs at a time, each as one process. `compare/run.py` writes
`results/compare/<host>-<slice>-<commit>.json` only when every system has finished and
`scripts/check_compare.py` has passed the file, so a file that exists is complete.

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
| `compare/fullday.py` | One system through a whole day, as an iteration-only capacity check |
| `compare/guard.py` | Runs a command in its own session, and stops it before it can hurt the host |
| `compare/systems/` | One file per system, behind `compare/systems/base.py` |
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

Exit 75 means the wrapper did not get the machine: finished systems are kept, so run it again to
go on. Exit 2 is usage or setup, and anything else is what failed. For iteration, `--systems`,
`--variants` and `--write-repetitions`, `--warm-repetitions` and `--cold-repetitions` narrow a run.

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

The primitive beside every class B number:

| System | Linux | macOS |
|---|---|---|
| Parquet, by the harness | `fsync` | `F_FULLFSYNC` |
| ArcticDB, by LMDB | `fdatasync` | `F_FULLFSYNC` |
| QuestDB | `msync(MS_SYNC)` and plain `fsync` | `msync(MS_SYNC)` and plain `fsync` |

The harness's own primitive is what it calls. LMDB's and QuestDB's are stated from what is known of
them, and are not traced in a run.

**How a class is proven.** Each run makes a small commit of 65,536 rows in a store of its own, and
reads each file's dirty pages straight after the commit returns. The last full write is read the
same way. `compare/settings.py` gives the verdict:

- Class A is proven only if pages are still dirty, since that shows no flush ran.
- Class B is proven only if no data page is dirty, and class A on the same system and host saw
  dirty pages through the same detector, so that a 0 means something. Files a commit leaves dirty by
  design are listed by name in `residual_dirty_files`: `lock.mdb` for LMDB, and for QuestDB the
  partition's column files, the symbol maps and `_meta`. Everything else must be clean.
- Parquet is the exception, because the harness makes the flush itself. macOS starts writing a file
  out when it closes, so a closed Parquet file reads 0 dirty with or without a flush, and dirty pages
  are no proof there. The verdict is the count of flushes made: 2 that succeeded and 0 dirty pages
  under B, and none under A.

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
give the stored form.

**ArcticDB**'s settings come from the environment before it loads. The harness reads them back
through `lib.options()`, the library config in storage, and the engine's own `get_config_int`. After
ingest, the stored index gives the largest segment, the map size comes from the meta page of
`data.mdb`, and the first stored segment's bytes are decoded by a plain LZ4 reader and must equal
the engine's first timestamps. The LZ4 acceleration cannot be read: the segment header has no
Python binding in 6.28.2.

**Parquet**'s thread counts come from `pa.cpu_count()` and `pa.io_thread_count()`. After ingest, the
footer gives each column chunk's compression, dictionary, statistics, page index and delta
encoding, the format version, the sort order and each row group's rows, and the first page header
in the file gives the data page version. The compression level cannot be read, since the footer
has no level, so a result records it as requested and says so.

## Warm and cold proof

Neither kind of number can reach a results file without its proof: `compare/coldcache.py` raises
instead, and `scripts/check_compare.py` checks the proofs again.

**Warm.** `tick_coldcache warm` reads every store file through, with the server running. `mincore`
on a fresh mapping must then read 100% of pages resident, both before and after all 30 repetitions
of each query.

**Cold.** Seven repetitions per query:

1. The service stops, and every file is flushed.
2. `tick_coldcache evict` removes every file from the cache. On macOS that is `msync(MS_INVALIDATE)`
   on a fresh read-only `MAP_SHARED` mapping. On Linux it is `fsync`, then
   `posix_fadvise(POSIX_FADV_DONTNEED)` over the whole file. Either way it repeats, up to 16
   passes, until `mincore` reads 0 on every file.
3. The device counter is read, the service starts again where there is one, and a fresh process
   opens the store and runs the query once.
4. The pages now resident can only have come from the device, so the device must have moved at
   least that many bytes. A shortfall is conclusive and fails the run. Only files the engine merely
   reads count: LMDB's lock file, which it writes as it opens, does not.

The Linux counter is the block device's sectors read. The macOS counter is `iostat`'s total for
`disk0`, which is machine-wide, counts reads and writes together, and has two decimals of a MiB, so
it can only ever show a shortfall. An excess proves nothing on either host. File system metadata
stays warm, because neither method reaches it.

Each cold number is the query alone, in a fresh process. The server's start and the worker's
imports fall before the clock, and pages they touched count as resident and as device reads.
`pq.read_table` and `Table.group_by` import `pyarrow.dataset` and `pyarrow.acero` on their first
call, so `ParquetSystem.open` imports both first, and `tests/compare/test_system_worker.py` fails
if any module loads inside the timed call. The clock does hold what an engine sets up on its first
call in a process: for Parquet on macOS, about 1 ms over a second call on warm pages.

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
- QuestDB starts through `bin/questdb.sh` with `JVM_PREPEND`, not from `java` directly, and writes a
  GC log per server start. It is otherwise the same server: the prepended options are the ones
  `QuestDBServer.command` passes.
- The guard samples every 2 s. Under 3 GiB of `MemAvailable`, under 50 GiB of free disk, or over
  10 MiB/s of swap traffic for three samples, it sends the session SIGTERM, then SIGKILL after 90 s
  or under 2 GiB. It exits 70, and its report says why. Each exit status names its signal.
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

## What the numbers are

Slice numbers prove that the harness works. They are not claims about a full day, and no target
changes because of them. The slice is five minutes long, so Q2 and Q3 are clipped from an hour to
`[09:30:00, 09:34:30)`, and Q5 scans 6,125,294 rows and not a day's 268,744,780. Every results file
says so. What the runs found is in "Comparison harness" in `NOTES.md`.
