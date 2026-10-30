# Evidence: comparison systems (Parquet / Arrow, ArcticDB, QuestDB)

Phase 1 research. Status: complete for the 2026-10-30 pass. Every value below is confirmed in primary text (cached copy or fetched this task).
Fields a source does not state are marked "not stated".

Cached copies are kept outside this repo (called CACHE/downloads below).

## Sections
1. Current released versions
2. Durability on write
3. Tuning guidance from each system's docs
4. Query mechanics for time-range queries
5. Published performance numbers (independent/peer-reviewed vs vendor)
6. Independent tick-data store comparisons
7. Fair-run settings table
8. Not found

---

## 1. Current released versions (as of 2026-10-30)

All checked live this task with `gh api repos/<repo>/releases` and PyPI JSON, and they agree with the cached copies in CACHE/downloads/dh-pypi and dh-questdb-rel.

| System | Latest stable | Published (UTC) | Source | Notes |
|---|---|---|---|---|
| Apache Arrow C++ / pyarrow | 26.0.0 | GitHub 2026-10-09T07:37:33Z; PyPI 2026-10-09T08:13:28 | https://github.com/apache/arrow/releases/tag/apache-arrow-26.0.0 ; https://pypi.org/pypi/pyarrow/json | `apache-arrow-26.0.1-rc1` was tagged 2026-10-30T18:36:55Z as a prerelease. Re-check before pinning. |
| ArcticDB | 6.28.2 | GitHub 2026-10-26T07:21:31Z; PyPI 2026-10-26T07:19:43 | https://github.com/man-group/ArcticDB/releases/tag/v6.28.2 ; https://pypi.org/pypi/arcticdb/json | `v6.28.3+man0` is flagged prerelease on GitHub and is not the PyPI latest. |
| QuestDB | 10.1.0 | 2026-10-22T11:48:11Z | https://github.com/questdb/questdb/releases/tag/10.1.0 | |

All source citations below are to these tags. Paths are repo-relative, line numbers are from the cached copy.

---

## 2. Durability on write

Every quote in this section is from primary source or docs. "Like with like" conclusions are at the end of the section (2.4).

### 2.1 Parquet: Arrow C++ / pyarrow 26.0.0. No fsync anywhere in the write path

Method: recursive grep of the whole `cpp/src/arrow`, `cpp/src/parquet`, `python/pyarrow` trees of the 26.0.0 tag (CACHE/downloads/arrow-26-tree/arrow-apache-arrow-26.0.0) for `fsync|fdatasync|F_FULLFSYNC|FlushFileBuffers|O_SYNC|O_DSYNC|msync|sync_file_range|os.sync`.
Result: the only hits are `cpp/src/arrow/io/mman.h:33` and `:147`, a Windows shim defining `msync`, which nothing in those trees calls.

| Fact | Quote or code | Location | URL |
|---|---|---|---|
| Base `Flush` is a no-op | `Status Writable::Flush() { return Status::OK(); }` | cpp/src/arrow/io/interfaces.cc:241 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/arrow/io/interfaces.cc#L241 |
| `FileOutputStream` does not override `Flush` | No `FileOutputStream::Flush` in file.cc; `Close` is `impl_->Close()` | cpp/src/arrow/io/file.cc:380 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/arrow/io/file.cc#L380 |
| Bytes go out via plain `write(2)` | `write(fd, buffer + bytes_written, static_cast<size_t>(chunksize)));` | cpp/src/arrow/util/io_util.cc:1726 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/arrow/util/io_util.cc#L1726 |
| Close is plain `close(2)` | `ret = static_cast<int>(close(fd));` | cpp/src/arrow/util/io_util.cc:1571 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/arrow/util/io_util.cc#L1571 |
| pyarrow path writes open this stream | `sink = self.file_handle = filesystem.open_output_stream(` | python/pyarrow/parquet/core.py:1119 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/python/pyarrow/parquet/core.py#L1119 |

Consequence (direct evidence for tick-store): a pyarrow or Arrow C++ Parquet write returns once bytes are in the OS page cache. The Parquet footer is written at close (`WriteFileMetaData(*file_metadata_, sink_.get());`, cpp/src/parquet/file_writer.cc:375), so a crash before close leaves a file with no footer. There is no crash-consistency or durability guarantee. Any Parquet write benchmark that wants to count durable bytes must add its own fsync on the file and on the parent directory, outside Arrow, and must report it as an added step.

### 2.2 ArcticDB 6.28.2 on LMDB: fsync on every committed key, by default

Docs at https://docs.arcticdb.io/ are unversioned, but the cached `runtime_config.md` and `faq.md` are byte-identical to the v6.28.2 tag (`docs/mkdocs/docs/`), checked this task with diff.
LMDB is pinned by the ArcticDB v6.28.2 submodule `cpp/third_party/lmdb` at commit 69087ced3cb6082f7dcfb4fc2dcaa3b68a7e2e8c (gh api contents, this task). `lmdb.h` at that commit: 0.9.35, dated "Jan 27, 2026".

| Fact | Quote or code | Location | URL |
|---|---|---|---|
| Each storage key is its own write transaction | `auto txn = ::lmdb::txn::begin(env()); // scoped abort on exception, so no partial writes` then `txn.commit();` | cpp/arcticdb/storage/lmdb/lmdb_storage.cpp:83-87, `do_write(KeySegmentPair&)` | https://github.com/man-group/ArcticDB/blob/v6.28.2/cpp/arcticdb/storage/lmdb/lmdb_storage.cpp#L80-L88 |
| Writers are serialised | `std::lock_guard<std::mutex> lock{*write_mutex_};` | same file:82 | same |
| Default environment flags are only `MDB_NOTLS` plus the extra-flags setting | `env().open(lib_dir_.generic_string().c_str(), MDB_NOTLS \| lmdb_extra_env_flags());` | same file:444 | https://github.com/man-group/ArcticDB/blob/v6.28.2/cpp/arcticdb/storage/lmdb/lmdb_storage.cpp#L444 |
| Storage-config flags default | `uint32 flags = 2; // MDB default is also 0` | cpp/proto/arcticc/pb2/lmdb_storage.proto:18 (field 2 of `Config`) | https://github.com/man-group/ArcticDB/blob/v6.28.2/cpp/proto/arcticc/pb2/lmdb_storage.proto |
| Default map size, non-Windows | `constexpr uint64_t default_map_size = 400ULL * (1ULL << 30); /* 400 GiB */` | lmdb_storage.cpp:438 | https://github.com/man-group/ArcticDB/blob/v6.28.2/cpp/arcticdb/storage/lmdb/lmdb_storage.cpp#L438 |
| Commit syncs the data file, then writes the meta page | `(rc = mdb_env_sync(env, 0)) \|\|` then `(rc = mdb_env_write_meta(txn)))` | LMDB `mdb.c`:3741-3742, `_mdb_txn_commit` | https://github.com/LMDB/lmdb/blob/69087ced3cb6082f7dcfb4fc2dcaa3b68a7e2e8c/libraries/liblmdb/mdb.c#L3741 |
| Data sync call | `if (MDB_FDATASYNC(env->me_fd))` | mdb.c:2599 | same file |
| `MDB_FDATASYNC` by platform | Linux default `fdatasync` (mdb.c:454-455); Apple `fcntl(fd, F_FULLFSYNC)` (mdb.c:132) | mdb.c | same file |
| Meta page goes through the synchronous fd | "Write to the SYNC fd unless MDB_NOSYNC/MDB_NOMETASYNC." | mdb.c:3969 | same file |
| Opt-out exists, intended for throwaway data only | "Intended for throwaway data, e.g. `MDB_NOSYNC \| MDB_NOMETASYNC` (`0x50000`, i.e. `327680`) skips the fsync on every commit." | docs `runtime_config.md`, section `LMDBStorage.ExtraFlags` (line 117 onward); default "0 (no extra flags)" | https://docs.arcticdb.io/latest/runtime_config/ ; https://github.com/man-group/ArcticDB/blob/v6.28.2/docs/mkdocs/docs/runtime_config.md#L117 |
| What the opt-out gives up | "Do not use those flags for data you need to survive a crash or power loss." | same section | same |
| Vendor claim | "It ensures atomicity, consistency, and durability but does not isolate transactions." | docs `faq.md` line 66 | https://docs.arcticdb.io/latest/faq/ |
| LMDB is not the design target | "ArcticDB achieves its highest performance and scale when configured with an object storage backed (e.g. S3)." | `tutorials_lmdb_and_in_memory.md`, "How to handle concurrent writers?" | https://docs.arcticdb.io/latest/tutorials/lmdb_and_in_memory/ |

Consequence (direct evidence for tick-store): with default settings, ArcticDB on LMDB does one `fdatasync` (Linux) plus one synchronous meta write per storage key. Keys are written one at a time under a mutex. A symbol's data is split into row-and-column segments, so one `write` costs at least one LMDB commit per segment plus index, version and reference keys. Its default write throughput therefore carries a per-segment durability cost that QuestDB's and Arrow's defaults do not. This count is read from the code; no source states a measured figure.

### 2.3 QuestDB 10.1.0: default is no flush; WAL commit and durability are different things

Docs at questdb.com are unversioned (source: github.com/questdb/documentation, `main`, fetched 2026-10-30).

| Fact | Quote or code | Location | URL |
|---|---|---|---|
| Default commit mode | `final String commitMode = getString(properties, env, key, "nosync");` | core/src/main/java/io/questdb/PropServerConfiguration.java:3079 | https://github.com/questdb/questdb/blob/10.1.0/core/src/main/java/io/questdb/PropServerConfiguration.java#L3079 |
| `nosync` | "no explicit flush (relies on OS page cache)" | docs Configuration > Cairo engine, `cairo.commit.mode` | https://questdb.com/docs/configuration/cairo-engine/#cairocommitmode |
| `async` per docs | "flush call is scheduled but returns immediately" | same | same |
| `sync` per docs | "waits for flush on appended column files to complete" | same | same |
| Docs on default durability | "By default, QuestDB relies on OS-level durability, letting the OS write dirty pages to disk." | docs Architecture > Storage engine, "Durability" | https://questdb.com/docs/architecture/storage-engine/ |
| Docs on `sync` | "This invokes `fsync()` on each commit, ensuring data survives OS crashes or power loss" | same | same |
| `async` is effectively no flush on Linux | "An async sync is a no-op: msync(MS_ASYNC) has been a no-op on Linux since 2.6.19" | core/src/main/java/io/questdb/cairo/vm/MemoryPMARImpl.java:142 (also MemoryCMARWImpl.java:337) | https://github.com/questdb/questdb/blob/10.1.0/core/src/main/java/io/questdb/cairo/vm/MemoryPMARImpl.java#L141-L148 |
| WAL commit obeys the same setting | `// flush disk before getting next txn` then `syncIfRequired();`, which is gated on `commitMode != CommitMode.NOSYNC` | core/src/main/java/io/questdb/cairo/wal/WalWriter.java:1087-1088, 2091-2103 | https://github.com/questdb/questdb/blob/10.1.0/core/src/main/java/io/questdb/cairo/wal/WalWriter.java#L1087 |
| Sequencer log obeys it too | `if (commitMode != CommitMode.NOSYNC) {` `txnMem.sync(commitMode == CommitMode.ASYNC);` | core/src/main/java/io/questdb/cairo/wal/seq/TableTransactionLogV2.java:340-344 (V1: 221-225) | https://github.com/questdb/questdb/blob/10.1.0/core/src/main/java/io/questdb/cairo/wal/seq/TableTransactionLogV2.java#L340 |
| Mechanism in `sync` mode for column memory is `msync`, not `fsync` | `ff.msync(pageAddress, getPageSize(), false);`; native `msync((void *) addr, len, async ? MS_ASYNC : MS_SYNC)`; rolled pages are msynced in `release()` | MemoryPMARImpl.java:146, 172-179; core/src/main/c/share/files.c:270-271 | https://github.com/questdb/questdb/blob/10.1.0/core/src/main/c/share/files.c#L270 |
| Native `fsync` is plain `fsync` | `return fsync((int) fd);` | files.c:275. No `F_FULLFSYNC` hit in share/files.c. OS-specific sources were not checked. | same |
| WAL is default for partitioned tables | `this.walEnabledDefault = getBoolean(properties, env, PropertyKey.CAIRO_WAL_ENABLED_DEFAULT, true);` | PropServerConfiguration.java:1091 | https://github.com/questdb/questdb/blob/10.1.0/core/src/main/java/io/questdb/PropServerConfiguration.java#L1091 |
| WAL requires partitioning | "**WAL requires partitioning.** Non-partitioned tables cannot use WAL." | docs Concepts > Write-Ahead Log, "Requirements" | https://questdb.com/docs/concepts/write-ahead-log/ |
| Commit precedes visibility | "WAL apply job merges data into table storage asynchronously" | same, "How WAL works", step 3 (step 2: "Transaction is committed and acknowledged to client") | same |
| QWP ack | "Batch accepted (written to WAL)" | docs QWP ingress, "Status codes", code 0x00 | https://questdb.com/docs/connect/wire-protocols/qwp-ingress-websocket/ |
| QWP deferred frames | "An OK on a deferred frame is not durability." | same, `FLAG_DEFER_COMMIT` | same |
| ILP over TCP has no transactions | "The TCP endpoint does not support transactions." | docs ILP overview, "HTTP transaction semantics" | https://questdb.com/docs/connect/compatibility/ilp/overview/ |

Documentation conflicts to flag:
- The WAL concept page says "Committed data is never lost — replay from log after restart". That holds for a process crash, since page-cache writes survive it. Under the default `nosync` the code above issues no flush, and the storage-engine page says OS crash or power-loss protection needs `sync`. Quote the storage-engine page and the code, not the WAL table row, when describing durability.
- The storage-engine page says `sync` "invokes `fsync()`". The column-memory path in 10.1.0 uses `msync(MS_SYNC)`. On Linux both force the dirty pages out, but the source should be cited accurately.
- Docs say `async` schedules a flush. The source comment says it is a no-op on Linux. Treat `async` as equal to `nosync` for durability on Linux.

Consequence (direct evidence for tick-store): QuestDB's published ingest numbers (section 5) are at the default unless stated otherwise, which means page-cache only. Stopping the clock on the client ack measures the WAL write, not query visibility.

### 2.4 Like-with-like for the write benchmark

Derived from 2.1 to 2.3. The classes below are this file's conclusion, not a vendor statement.

| Class | tick-store | Parquet (Arrow 26) | ArcticDB 6.28.2 LMDB | QuestDB 10.1.0 |
|---|---|---|---|---|
| A. Page cache only: returns when data is in the OS cache | no fsync | Default. Nothing to set. | Set `LMDBStorage.ExtraFlags=327680`. Docs reserve it for throwaway data. | Default `cairo.commit.mode=nosync` |
| B. Durable at commit: returns after flush to device | fsync data and directory | Not available. The harness must fsync file and directory after close, and report that as added. | Default | `cairo.commit.mode=sync` (`async` does not count) |

Rules that follow:
- Do not compare across classes. Default ArcticDB is class B, default QuestDB and default Parquet are class A.
- Report both A and B for every system, or state which single class was used.
- Report flush granularity too. ArcticDB syncs per storage key, QuestDB per commit, Parquet (harness-added) per file. These are not equal even inside class B, and QuestDB's depends on batch size.
- For QuestDB, take two timings: time to last ack, and time until WAL apply has caught up. Only the second means the rows are queryable from table storage.
- Crash consistency also differs: LMDB and WAL recover to a consistent state, while a Parquet file without its footer is unreadable.
- Platform note: LMDB uses `F_FULLFSYNC` on macOS, and QuestDB's shared native code calls plain `fsync`, so class B may not be equivalent on macOS. Run class B on Linux.

---

## 3. Tuning guidance from each system's own docs

None of the three projects publishes a tuning guide specific to ITCH ticks. Each row below is what its docs or source say. Where the docs conflict, both are quoted.

### 3.1 Parquet (Arrow 26.0.0 unless stated)

| Knob | Default | Quote or code | Location | URL |
|---|---|---|---|---|
| Row group: Parquet project guidance | n/a | "We recommend large row groups (512MB - 1GB)." | parquet.apache.org Configurations, "Row Group Size". Page last modified 2024-03-08. | https://parquet.apache.org/docs/file-format/configurations/ |
| Row group: Arrow default | 1 Mi rows | `DEFAULT_MAX_ROW_GROUP_LENGTH = 1024 * 1024`. Docstring: "the minimum of the number of rows in the Table/RecordBatch and 1024 * 1024." | cpp/src/parquet/properties.h:180; python/pyarrow/parquet/core.py:1172-1175 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/parquet/properties.h#L180 |
| Data page: Parquet project guidance | n/a | "We recommend 8KB for page sizes." | parquet.apache.org Configurations, "Data Page Size" | https://parquet.apache.org/docs/file-format/configurations/ |
| Data page: Arrow default | 1 MiB | `kDefaultDataPageSize = 1024 * 1024`; "use the default data page size of 1MByte." | properties.h:175; core.py:805-808 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/python/pyarrow/parquet/core.py#L805 |
| Rows per page | 20,000 | "If None, use the default of 20000." | core.py:929-931 | same file |
| Dictionary | on; pages limited to 1 MiB, then PLAIN | "if the dictionary size is too large, the column will fallback to ``PLAIN`` encoding." | core.py:772-776 | same file |
| Dictionary page limit | 1 MB | "Specify the dictionary page size limit per row group. If None, use the default 1MB." | core.py `dictionary_pagesize_limit` | same file |
| DELTA_BINARY_PACKED | off | `column_encoding` values include 'DELTA_BINARY_PACKED'. Supported on "INT32, INT64" per the spec. | core.py `column_encoding`; https://parquet.apache.org/docs/file-format/data-pages/encodings/ | same file |
| Dictionary and explicit encoding exclude each other per column | n/a | `"To use 'column_encoding' set 'use_dictionary' to False"`. Docs say whole-writer; code also accepts `use_dictionary=[cols]` as long as the sets are disjoint. | python/pyarrow/_parquet.pyx:2128-2144 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/python/pyarrow/_parquet.pyx#L2128 |
| BYTE_STREAM_SPLIT | off | "it should be combined with a compression codec so as to achieve size reduction." If dictionary is also on, "dictionary is preferred." | core.py:823-828 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/python/pyarrow/parquet/core.py#L823 |
| Compression default differs by API | pyarrow `snappy`; C++ `UNCOMPRESSED` | `compression='snappy'` in `write_table`; `DEFAULT_COMPRESSION_TYPE = Compression::UNCOMPRESSED` | core.py:2017-2018; properties.h:185 | as above |
| zstd level | 1 | `constexpr int kZSTDDefaultCompressionLevel = 1;` | cpp/src/arrow/util/compression_internal.h:76 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/arrow/util/compression_internal.h#L76 |
| Statistics | on | `write_statistics=True`; "Statistics are enabled by default for all columns." | core.py:2019; docs/source/cpp/parquet.rst:305 | https://arrow.apache.org/docs/cpp/parquet.html |
| Page index default differs by API | pyarrow off; C++ on | `write_page_index : bool, default False`; `DEFAULT_IS_PAGE_INDEX_ENABLED = true`; "Disable writing page index in general for all columns. Default enabled." | core.py:889; properties.h:186, 845 | as above |
| Page index stats location | n/a | "Writing statistics to the page index disables the old method of writing statistics to each data page header." | core.py:890-891 | same file |
| Bloom filter | off; `ndv=1048576`, `fpp=0.05` when requested | "Bloom filters are most effective for high-cardinality columns." | core.py:933-958 | same file |
| Sort metadata | off | "The writer does not sort the data nor does it verify that the data is sorted." | core.py `sorting_columns` | same file |
| Format version | 2.6 | `version='2.6'`, needed for nanosecond timestamps to be stored natively: "``version='2.6'`` (the default), they are written natively without loss" | core.py:760, 791-793 | same file |

No project doc gives a zstd level, a row-group size, or an encoding choice for time-ordered numeric ticks. The numbers in section 7 for those knobs must be parameters to sweep, not settled facts.

### 3.2 ArcticDB 6.28.2

| Knob | Default | Quote or code | Location | URL |
|---|---|---|---|---|
| Rows per segment | 100,000 | "`rows_per_segment: int, default 100_000`" | python/arcticdb/options.py:100 | https://github.com/man-group/ArcticDB/blob/v6.28.2/python/arcticdb/options.py#L100 |
| Columns per segment | 127 | "`columns_per_segment: int, default 127`" | options.py:129 | same |
| Library options are fixed at creation | n/a | "currently library configuration options cannot be changed once the library has been created." | docs `lib_config.md` | https://docs.arcticdb.io/latest/lib_config/ |
| Vendor tuning example | 10,000,000 rows | "a large rows_per_segment value can improve performance for dataframes with a large number of rows" | notebook ArcticDB_billion_row_challenge, cell 8 | https://github.com/man-group/ArcticDB/blob/v6.28.2/docs/mkdocs/docs/notebooks/ArcticDB_billion_row_challenge.ipynb |
| Dynamic schema changes tiling | False | "When enabled, ArcticDB will only tile across the rows of the data." | options.py, `dynamic_schema` | options.py |
| Dynamic strings | on | `resolve_defaults("dynamic_strings", proto_cfg, global_default=True, **kwargs)` | python/arcticdb/version_store/_store.py:955. Not a parameter on the public `Library.write`. | https://github.com/man-group/ArcticDB/blob/v6.28.2/python/arcticdb/version_store/_store.py#L955 |
| Codec | LZ4, acceleration 1 | `lz4ptr->set_acceleration(1);` | cpp/arcticdb/codec/default_codecs.hpp:15-20. The proto also has Zstd and TurboPfor, but no public option was found in `options.py` or `library.py` (grep `zstd|lz4`). | https://github.com/man-group/ArcticDB/blob/v6.28.2/cpp/arcticdb/codec/default_codecs.hpp |
| Encoding version | None; `DEFAULT_ENCODING_VERSION = EncodingVersion.V1` (options.py:18) | "v2 is faster, but still experimental, so use with caution." | options.py:132-134 | options.py |
| Column stats are off and experimental | off | `get_int("ColumnStats.UseForQueries", 0) == 1`; API name `create_column_stats_experimental`; "MINMAX column statistics for each row-slice" | cpp/arcticdb/pipeline/column_stats_filter.cpp:26; _store.py:1319-1328 | https://github.com/man-group/ArcticDB/blob/v6.28.2/cpp/arcticdb/pipeline/column_stats_filter.cpp#L26 |
| Index sorted | validated | "verify that the index of `data` supports date range searches and update operations." | library.py:1069-1071, `validate_index`, default True | https://github.com/man-group/ArcticDB/blob/v6.28.2/python/arcticdb/version_store/library.py#L1069 |
| Symbol layout | no single answer | "Prefer a smaller number of symbols that have a large amount of data each" | docs `tutorials/data_organisation` | https://docs.arcticdb.io/latest/tutorials/data_organisation/ |
| Security identifier as secondary index | n/a | "the high performance ArcticDB indexing is only on the primary index level." | same, "Single Symbol for all Securities" (line 171) | same |
| Small appends | n/a | "lots of small appends or updates can result in lots of small data chunks, which makes reads slower." | same, line 136; fix is `compact_data` | same |
| Thread pools | CPU count; IO 1.5x CPU | "`NumIOThreads` will still default to x1.5 `NumCPUThreads`." | docs `runtime_config.md`, line 162 | https://docs.arcticdb.io/latest/runtime_config/ |
| Categorical strings | avoid | "ArcticDB currently offers extremely limited support for categorical data." | docs `faq.md` | https://docs.arcticdb.io/latest/faq/ |

Context on the backend: docs say ArcticDB is "designed for object storage and also supports local-disk storage using LMDB" (`index.md` line 46). Testing LMDB tests a supported but secondary path, and any report should say so.

### 3.3 QuestDB 10.1.0

| Knob | Default | Quote or code | Location | URL |
|---|---|---|---|---|
| Partition sizing | ILP auto-create `DAY` | "**Target 30-80 million rows per partition**" | docs Concepts > Partitions, line 53 | https://questdb.com/docs/concepts/partitions/ |
| Partition by volume | n/a | Table: `30-500 million rows/day` gives `DAY`; `>1 billion rows/day` gives `HOUR` | same, lines 59-65; also Schema design essentials | same; https://questdb.com/docs/schema-design-essentials/ |
| Designated timestamp | none unless declared | "Without a designated timestamp, you lose most of QuestDB's performance benefits." | Schema design essentials, line 46 | https://questdb.com/docs/schema-design-essentials/ |
| Timestamp resolution | n/a | `TIMESTAMP_NS` listed for "High-frequency trading, scientific data"; "**Use `TIMESTAMP` unless you specifically need nanosecond precision.**" | docs Concepts > Designated timestamp, "Timestamp resolution" | https://questdb.com/docs/concepts/designated-timestamp/ |
| Symbol capacity: concept page | automatic | "From 9.0.0 onwards, symbol capacity is fully automatic." and "The `CAPACITY` setting is now obsolete" | docs Concepts > Symbol, lines 41-42 | https://questdb.com/docs/concepts/symbol/ |
| Symbol capacity: config page, conflict | 256 | "Getting this value significantly wrong causes performance degradation." | docs Cairo engine, `cairo.default.symbol.capacity`; code still `getInt(..., CAIRO_DEFAULT_SYMBOL_CAPACITY, 256)` at PropServerConfiguration.java:1774 | https://questdb.com/docs/configuration/cairo-engine/ |
| Symbol cache | on | "When `true`, symbol values are cached on the Java heap" | Cairo engine, `cairo.default.symbol.cache.flag` | same |
| Symbol index | none unless declared | "For columns frequently used in `WHERE` clauses, add an index" | docs Concepts > Symbol, line 78 | https://questdb.com/docs/concepts/symbol/ |
| Index types | bitmap default | Table: "Storage size": "~15 bytes/value" bitmap, "~1 byte/value" posting. Vendor claim, method not stated. | docs Concepts > Deep dive > Indexes, lines 28-34 | https://questdb.com/docs/concepts/deep-dive/indexes/ |
| Index write cost | n/a | "Each new entry in the table or materialized view will trigger an entry" (Ingestion performance) | same, line 123 | same |
| O3 lag | min 1 s, max 10 min | `cairo.o3.min.lag` "**Default**: `1 second`"; `cairo.o3.max.lag` "**Default**: `10 minutes`" | Cairo engine | https://questdb.com/docs/configuration/cairo-engine/ |
| Ordered ingest | n/a | "ILP performs best when rows arrive in chronological order by their designated timestamp." | docs ILP overview, "Data ordering" | https://questdb.com/docs/connect/compatibility/ilp/overview/ |
| Ingest transport | HTTP preferred; QWP new | "ILP over HTTP is the preferred ingestion method. ILP over TCP is available for legacy workloads." | docs Ingestion config, intro. The Connect overview recommends client libraries for "Greenfield app". | https://questdb.com/docs/configuration/ingestion/ ; https://questdb.com/docs/connect/overview/ |
| ILP flush | 75,000 rows HTTP; 600 TCP | "Defaults to `75,000` for HTTP, and `600` for TCP." | ILP overview, `auto_flush_rows` | same |
| Bulk load | COPY for large files | "The [COPY](/docs/query/sql/copy/) SQL command is the preferred way to import large CSV files into partitioned tables." | docs Import CSV | https://questdb.com/docs/connect/compatibility/import-csv/ |
| COPY constraint | n/a | "If the target table exists and is partitioned, the target table must be empty." | docs COPY, "Import modes" | https://questdb.com/docs/query/sql/copy/ |
| COPY hardware | n/a | "the best `COPY FROM` performance can be achieved only on a machine with a local, physically attached SSD." | same | same |
| Native format is not compressed by the engine | n/a | "QuestDB compresses data by converting partitions to" Parquet, "which uses `lz4_raw` compression by default" | docs Capacity planning, "Data compression" | https://questdb.com/docs/getting-started/capacity-planning/ |
| OS limits | n/a | `ulimit -n` and `vm.max_map_count` both "Recommended" `1048576` | Capacity planning, limits table (lines 327-330) | same |
| File system | n/a | Lists EXT4, XFS (`ftype=1` only), ZFS | Capacity planning, lines 66-70 | same |

Consequence for storage comparisons: QuestDB's native on-disk size and its size after `ALTER TABLE CONVERT PARTITION` to Parquet are different numbers. Report both.

---

## 4. Query mechanics for a time-range query

### 4.1 Parquet via pyarrow or Arrow Dataset (row-group pruning only)

| Fact | Quote or code | Location | URL |
|---|---|---|---|
| Row groups are pruned from min/max stats | `ARROW_ASSIGN_OR_RAISE(row_groups, parquet_fragment->FilterRowGroups(options->filter));` | cpp/src/arrow/dataset/file_parquet.cc:603, 900-913, 915-966 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/arrow/dataset/file_parquet.cc#L900 |
| pyarrow does not read the page index | "Note that the page index is not yet used on the read size by PyArrow." (sic) | python/pyarrow/parquet/core.py:895 | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/python/pyarrow/parquet/core.py#L895 |
| Nor do Arrow C++ data read APIs | "data read APIs do not currently make any use of them." (subject: Column Index, Offset Index and Bloom Filter structures) | docs/source/cpp/parquet.rst:662-663 | https://arrow.apache.org/docs/cpp/parquet.html |
| Grep confirms | No page-index or bloom-filter use in `cpp/src/parquet/arrow/reader.cc` or `cpp/src/arrow/dataset/file_parquet.cc`. `ParquetFileReader::GetPageIndexReader()` exists at cpp/src/parquet/file_reader.h:171 for manual use. | | https://github.com/apache/arrow/blob/apache-arrow-26.0.0/cpp/src/parquet/file_reader.h#L171 |
| What the format allows | "Range scans on the sort column will only need to read the exact data pages that contain relevant data." | parquet.apache.org Page Index, "Goals" | https://parquet.apache.org/docs/file-format/pageindex/ |
| Another Arrow reader does this | "uses the optional page index in the footer metadata to rule out entire Data Pages." | Arrow blog, 26 Dec 2022, "Page pruning". Rust reader, not C++. | https://arrow.apache.org/blog/2022/12/26/querying-parquet-with-millisecond-latency/ |

Consequence: with pyarrow or the Arrow Dataset API the row group is the smallest unit of I/O for a time-range query. Every matching row group is decoded for the requested columns, then filtered. So the Parquet project's 512 MB to 1 GB advice and Arrow's 1 Mi rows default imply very different time-range latency, and row-group size is the main knob. A page index written by pyarrow gives no read benefit unless the reader is another engine or hand-written C++ on `GetPageIndexReader()`. State the reader used in any Parquet range-query result.

### 4.2 ArcticDB `date_range`

| Fact | Quote or code | Location | URL |
|---|---|---|---|
| Data is tiled | "sliced by both columns and rows, in order to facilitate rapid date-range and column searching during read operations" | docs On-disk storage, Data layer (line 51) | https://docs.arcticdb.io/latest/technical/on_disk_storage/ |
| Segments are chosen from the index key's start/end columns | `container.seg().column(position_t(index::Fields::start_index));` | cpp/arcticdb/pipeline/query.cpp:57-58, `build_bitset_for_index` | https://github.com/man-group/ArcticDB/blob/v6.28.2/cpp/arcticdb/pipeline/query.cpp#L46 |
| Dynamic schema path uses binary search | `std::lower_bound(end_index_col_begin, end_index_col_end, adjusted_range_start);` | query.cpp:68-82 approx, branch `dynamic_schema && !column_groups` | same |
| Static schema path (the default) walks every index row | `for (auto i = 0u; i < container.size(); ++i) {` then `is_slice_in_index_range(...)` | query.cpp:109-118 | same |
| Result is exact | "Returns only the part of the data that falls within the given range (inclusive)." | python/arcticdb/version_store/library.py, `read`, `date_range` | https://github.com/man-group/ArcticDB/blob/v6.28.2/python/arcticdb/version_store/library.py#L2079 |
| Column subset limits I/O when `columns_per_segment` is small | "can reduce the amount of data read from storage by only reading those data segments that contain data requested by the reader." | options.py:104-105 | options.py |

Consequence: the pruning unit is a segment, so `rows_per_segment` sets range-query granularity. Index lookup cost is linear in the segment count under the default static schema. With a table narrower than 127 columns, all columns share a segment unless `columns_per_segment` is lowered, so a column subset does not reduce I/O under defaults (ClickBench's ArcticDB entry reports the same point; see 5.3).

### 4.3 QuestDB interval scan

| Fact | Quote or code | Location | URL |
|---|---|---|---|
| Method | "QuestDB uses binary search on the designated timestamp column to jump directly to relevant data." (link markup removed) | docs Concepts > Deep dive > Interval scan | https://questdb.com/docs/concepts/deep-dive/interval-scan/ |
| Steps | "Prunes partitions outside the time range", "Binary searches within relevant partitions to find exact row boundaries", "Reads only rows within those boundaries" | same, "How it looks" | same |
| Check | "Good" plan shows `Interval forward scan on: trades`. "Not optimal" plan shows `Async Filter` / `Full scan`. | same, "Verifying interval scan with EXPLAIN" | same |
| Vendor latency claim | "most queries with timestamp predicates complete in **sub-millisecond** time, regardless of total table size." | docs Concepts > Designated timestamp, "Performance impact". No hardware or data stated. | https://questdb.com/docs/concepts/designated-timestamp/ |
| Requires designated timestamp | "Tables without a designated timestamp cannot use interval scan." | Interval scan, "Edge cases" | as above |

Summary: QuestDB reaches exact row boundaries by binary search over physically sorted columns. Parquet via pyarrow reaches row-group boundaries from footer stats. ArcticDB reaches segment boundaries from an index key. tick-store's time-range benchmark should report the number of rows and bytes actually decoded alongside latency, because the three differ by design.

---

## 5. Published performance numbers

No source below measures ITCH data. No source measures all of Parquet, ArcticDB and QuestDB on tick data. **Treat every number as context unless the applicability note says otherwise.**

### 5A. Peer-reviewed

#### 5A.1 Zeng et al., "An Empirical Evaluation of Columnar Storage Formats", PVLDB 17(2), 2023

URL: https://www.vldb.org/pvldb/vol17/p148-zeng.pdf (cached: CACHE/downloads/columnar-eval-vldb23/p148-zeng.pdf)
Hardware: AWS i3.2xlarge, "8 vCPUs of Intel Xeon CPU E5-2686 v4, 61GB memory, and 1.7TB NVMe SSD" (Section 5.1, p. 154 of the volume, PDF p. 7). Clock: not stated. Software: Arrow v9.0.0 for file generation; Parquet C++ with Arrow; Rust Parquet v32 for page-level zone maps. Thread count: "using a single thread" for Figure 10. Data set: workloads from the benchmark introduced in Section 4 of the paper.
Author list includes Wes McKinney (Voltron Data, per the paper header). The venue is peer reviewed.

| Value | Quote | Location | Counted | Applicability |
|---|---|---|---|---|
| Parquet default row group 1 M rows, dictionary limit 1 MB, Snappy | "Parquet has a row group size of 1m rows and sets the dictionary page size limit to 1 MB." | Section 5.1, PDF p. 7 | Configuration, not a result | Context. Shows a peer-reviewed study used Arrow's near-default settings. |
| zstd slows Parquet scans on NVMe (no number stated in text) | "the decompression overhead of Zstd hinders scan performance." | Section 5.4 "Block Compression", PDF p. 9, Figure 10 | Scan time by storage tier, Parquet, zstd vs none | Direct guidance for tick-store's read comparison: compression choice changes the Parquet read result on NVMe. Numbers are in the figure only and are not transcribed here. |
| Zone maps limited to clustered columns | "zone maps are effective only for a smaller number of well-clustered columns" | Section 5.6, PDF p. 10 | Qualitative | Direct for design: a time-ordered timestamp column is well clustered; a symbol column in a time-ordered file is not. |
| Page-level reads were not available in Arrow C++ | "To evaluate page-level zone maps, we use the Rust implementation (v32) of Parquet" | Section 5.1, PDF p. 7 | n/a | Corroborates 4.1, for Arrow 9.0. |

#### 5A.2 Kuschewski et al., "BtrBlocks", SIGMOD 2023

URL: https://www.cs.cit.tum.de/fileadmin/w00cfj/dis/papers/btrblocks.pdf (cached: CACHE/downloads/btrblocks-sigmod23/btrblocks.pdf)
Hardware: c5n.18xlarge AWS EC2 (Section 6). Clock, cores, memory: not stated in the passages read. Storage: not stated; measurement is in memory for these figures. Software: pyarrow 9.0.0, Arrow C++ low-level API; row group size 2^17. Data set: Public BI Benchmark and TPC-H. Threads: "We parallelized decompression over both rowgroups and columns."

| Value | Quote | Location | Counted | Applicability |
|---|---|---|---|---|
| Parquet+Zstd double columns, Public BI: 2.30× | Row "Parquet+Zstd", column Double/PBI: share 50.1 %, compr. 2.30 | Table 2, PDF p. 7 | Compression ratio, uncompressed / compressed, by data type | Context. BI doubles, not prices. |
| Parquet+Zstd integer columns, Public BI: 6.97× | same row, Integer/PBI: share 16.3 %, compr. 6.97 | Table 2 | same | Context |
| Parquet+Zstd double, TPC-H: 2.87×; integer, TPC-H: 1.74× | same row | Table 2 | same | Context |
| Parquet+LZ4 double, Public BI: 2.16× | Row "Parquet+LZ4" | Table 2 | same | Context. Near-identical ratio to plain Parquet (1.99×) on doubles. |
| Row group tuned to 2^17 rows | "setting the rowgroup size in Apache Arrow to 217 because we found that to be fastest." (superscript lost in extraction) | Section 6, PDF p. 7 | Configuration | Direct evidence that row-group size is worth sweeping. |

### 5B. Independent, not peer reviewed (preprints, community benchmarks)

#### 5B.1 tse_tick (Li et al., arXiv:2608.23053v1, 24 Aug 2026): nearest public Parquet tick numbers

URL: https://arxiv.org/abs/2608.23053 (cached: CACHE/downloads/papers/2608.23053.pdf)
Hardware: "Intel Core i5-14400F (10-core/16-thread), 32 GB RAM" (Table 4 note, Section 7). Storage: not stated. Software: Python 3.11, Polars 1.42, PyArrow 24.0, DuckDB 1.5.2, tse_tick 0.15.1, "2026-07-18 run". Threads: not stated for Tables 5 and 6. Data: Nikkei NEEDS Tokyo Stock Exchange tick data; authors built the library they measure. zstd level and row-group size: not stated. Which writer (Polars or pyarrow): not stated.

| Value | Quote | Location | Counted | Applicability |
|---|---|---|---|---|
| Parquet zstd 68.86 MB, Snappy 139.85 MB, CSV 2369.27 MB | "34.4× smaller than uncompressed CSV (68.9 MB versus 2.37 GB)" | Table 5, HTICST120 (4.8 M rows, 95 columns), PDF p. 10 | File size; baseline is CSV, not binary | Context, but real tick data. Baseline is text, so the ratio cannot be compared with a binary baseline. Many of the 95 columns may be sparse (not stated). |
| Write: zstd 0.9116 s, Snappy 0.7107 s | Table 5 cell | same | Seconds to write one file; 4.8 M rows gives about 5.3 M rows/s (computed here, zstd) | Context. No durability step stated. |
| Read all: zstd 0.2887 s; 3 of 95 columns: 0.0124 s | Table 5 cells | same | Seconds, medians of 5 runs, 1 warm-up discarded | Context. Warm cache implied by warm-up discard. |
| Hour slice, single ticker, DuckDB plus Hive Parquet: 0.0169 s | "completes in 0.0169 s via DuckDB" | Table 6, PDF p. 10 | Median latency; Table 6 lists 52 rows returned. The text describes the store as "1,500,000 rows, a ticker subset of multi-day data". | Context. Reader is DuckDB, partitioned by date and ticker, so this is not pyarrow pruning. |
| Row-group stats on a time key: 7.64× and 5.65× | "a one-minute slice query accelerated 7.64× and a five-minute window 5.65×, for a storage cost of +0.52%" | Section 4.4 | Latency ratio, with vs without the time key column | Direct for design: time-window predicates need a raw sortable column for row-group stats to apply. |

#### 5B.2 ClickBench entries (repository github.com/ClickHouse/ClickBench)

Data set: `hits`, "100M rows × 105 columns" (arcticdb/README.md); `questdb_load` checks for 99,997,497 rows. Not tick data. Hardware: `c6a.4xlarge`; CPU, memory, storage are not stated in the result JSON. All entries `"tuned": "no"`. Threads: not stated. Sum of hot runs computed here from the JSON as the sum over 43 queries of the fastest non-first try.

| Entry | Load (s) | Data size (bytes) | Sum of best hot tries (s) | Date | Version | URL |
|---|---|---|---|---|---|---|
| QuestDB | 5433 | 72,882,284,560 | 25.95 | 2026-09-26 | Not stated in JSON. Install script default is `QDB_VERSION:-9.3.5`. | https://github.com/ClickHouse/ClickBench/blob/main/questdb/results/20260926/c6a.4xlarge.json |
| ArcticDB, LMDB | 715 | 17,602,805,760 | 1934.56 | 2026-08-31 | Not stated; install is unpinned `pip install arcticdb` | https://github.com/ClickHouse/ClickBench/blob/main/arcticdb/results/20260831/c6a.4xlarge.json |
| DuckDB on Parquet | 5 | 14,779,976,446 | 32.48 | 2026-05-11 | Not stated | https://github.com/ClickHouse/ClickBench/blob/main/duckdb-parquet/results/20260511/c6a.4xlarge.json |

Applicability: context only. This workload is analytical scans, not time-range selection. `load_time` for DuckDB on Parquet only registers an existing file, so it is not an ingest number, and the Parquet file was not written by Arrow. QuestDB's load used HTTP `/imp`, because COPY is reserved for `nproc >= 96` in `questdb_load`. ArcticDB's result used `columns_per_segment=1` and 1 M-row appends. The ArcticDB README gives a useful warning: "every append rewrites the symbol's index key" and the rewrites' cost "grows with the square of the number of appends" (arcticdb/README.md, lines 95-96; https://github.com/ClickHouse/ClickBench/blob/main/arcticdb/README.md). It also explains that the "default tile is 127 columns wide", so on a table of 127 columns or fewer, `columns=[...]` does not prune off disk (line 73).

### 5C. Vendor-published

#### 5C.1 QuestDB

**TSBS ingest.** URL: https://questdb.com/blog/clickhouse-vs-questdb-comparison/ , "Last updated August 28, 2026".
Hardware: "AWS EC2 r8a.8xlarge (32 vCPU, 256 GB RAM, AMD EPYC), GP3 EBS storage (20,000 IOPS, 1 GB/s throughput)". Clock: not stated. Software: "Ubuntu 22.04, ClickHouse 26.7.5.10, QuestDB 9.3.3, all with default configurations." Client and database share the instance "with no CPU pinning". Threads: 32 TSBS workers. Data: TSBS `cpu-only`, two days. Counted: rows/s as reported by the loader. Commit mode: not stated, but "default configurations" plus `"nosync"` at tag 9.3.3 (PropServerConfiguration.java:2206, checked this task) means no explicit flush.

| Hosts | QuestDB rows/s | ClickHouse rows/s | Ratio as published |
|---|---|---|---|
| 1,000 | 7.77M | 1.74M | 4.5x |
| 4,000 | 7.96M | 1.76M | 4.5x |
| 100,000 | 8.59M | 1.75M | 4.9x |
| 1,000,000 | 7.14M | 1.67M | 4.3x |

The README also lists TimescaleDB 2.29.1 at 1.08M rows/s and InfluxDB 1.12.4 at 541K at 100,000 hosts (https://github.com/questdb/questdb/blob/10.1.0/README.md, "QuestDB performance vs. other databases"). Influx figures match https://questdb.com/blog/influxdb-vs-questdb-comparison/.
Applicability: context only. Vendor benchmark, page-cache durability, 10 symbol and 10 integer columns, not ticks. The post says the "hot tier is uncompressed", so these rates carry no compression cost. The same post claims "Writes are made durable before any processing", which does not match the default `nosync` code path in 2.3.

**TSBS queries, same post and setup.** "median of 3 runs, warm cache", TSBS `--workers=1`, two days of 4,000 hosts.

| Query | QuestDB | ClickHouse |
|---|---|---|
| single-groupby-1-1-1 | 0.41 ms | 3.26 ms |
| single-groupby-5-1-12 | 1.94 ms | 4.48 ms |
| double-groupby-1 | 32.20 ms | 18.90 ms |
| high-cpu-all | 1,027.50 ms | 966.70 ms |
| lastpoint | 1.70 ms | 25.60 ms |

Applicability: context only. The closest shape to tick-store's time-range query is `single-groupby`, a filtered short range, but it aggregates and does not return rows.

**QWP vs ILP.** URL: https://questdb.com/blog/qwp-vs-ilp-ingestion-benchmark/ , "3 months ago" on the page; exact date not stated.
Hardware: r8a.8xlarge as above, "Ubuntu 24.04", "QuestDB 10.0.0". Threads: "Loader workers | 32 throughout". Data: TSBS cpu-only, "10 symbol columns and 10 integer columns per row". Commit mode: not stated.
Counted: "rows sent per second, the throughput reported by the loader". This is a client-side rate.

| How sent | 4,000 hosts | 1,000,000 hosts | Limited by |
|---|---|---|---|
| Standard TSBS loader, over network | 9.4M | ~9M | the TSBS harness |
| Prebuilt frames, over network | 19M | 19M | the network |
| Prebuilt frames, single machine | 48M | 33M | the server |
| ILP over TCP, over network | 5.3M | 5.2M | 14.7 Gbit/s link |

Co-located run at 4,000 hosts through the standard loader: ILP over TCP 11.6M, QWP 11.1M rows sent/s.

Applicability: context only. These are the highest published QuestDB ingest figures, and the highest ones exclude row building from the timed loop. The file-size note is direct: "the same 69 million rows are 24 GB of ILP text or 7.1 GB of QWP binary". Protocol choice changes results by a factor of 3.6 over a real network, so tick-store must state the protocol used.

**Egress to Arrow.** URL: https://questdb.com/blog/streaming-500-million-rows-into-apache-arrow/ , "3 months ago".
Hardware: "m8gn.8xlarge, 32 vCPU, 123 GB RAM, 100 Gbps", client "c8gn.8xlarge, 32 vCPU, 61 GB RAM", gp3 at "16,000 IOPS and 1000 MiB/s". Data: 500 M rows of a 5-column `trades` table, 27 distinct symbols. Counted: rows/s drained to Arrow; "two warmup passes, then a 10 second pause, then the mean of three measured runs". QuestDB version: 10.0 per README.

| Readers | QuestDB rows/s | ClickHouse Arrow rows/s |
|---|---|---|
| 1 | 40,139,148 | 16,068,358 |
| 8 | 218,966,796 | 93,788,141 |

Applicability: context, but a useful warning. The schema is the closest vendor schema to ticks, yet with a 16 GB server the post reports only 41 M rows/s because "every read came off EBS". Results depend on the data being in page cache. README's rounded "220M rows/sec" and "2.3 s" refer to this table.

#### 5C.2 ArcticDB

| Value | Quote | Location | URL | Hardware | Data | Threads | Counted | Version, date | Applicability |
|---|---|---|---|---|---|---|---|---|---|
| Up to 100 M rows/s, single consumer | "Process up to 100 million rows per second for a single consumer" | docs `index.md`, "ArcticDB is:" | https://docs.arcticdb.io/latest/ | not stated | not stated | not stated | "rows per second", operation not stated | docs at v6.28.2 | Context only. Marketing claim with no method. |
| 1 B rows/s, all consumers | "Process a billion rows per second across all consumers" | same | same | not stated | not stated | not stated | same | same | Context only |
| 7.01 s, 1 B-row group-by aggregate | "7.01 s ± 604 ms per loop (mean ± std. dev. of 7 runs, 1 loop each)" | Notebook ArcticDB_billion_row_challenge, cell 13 output | https://github.com/man-group/ArcticDB/blob/v6.28.2/docs/mkdocs/docs/notebooks/ArcticDB_billion_row_challenge.ipynb | not stated | 1 B synthetic rows, 10,000 cities, on `lmdb://` | `VersionStore.NumCPUThreads` 16 | Wall time for three aggregates by group, about 143 M rows/s (computed here) | Python 3.11.11; ArcticDB version not stated | Context. Aggregate, not range read. `rows_per_segment=10_000_000`, non-default. Disk type and cache state not stated. |

No vendor-published ArcticDB number was found for ingest throughput or compression ratio. See section 8.

#### 5C.3 Parquet / Arrow

No Arrow or Parquet project page publishes write throughput, compression ratio or query latency for its own writer. See section 8. The DuckDB entry below is an engine vendor writing Parquet, not Arrow.

DuckDB, "Query Engines: Gatekeepers of the Parquet File Format", 2025-01-22, https://duckdb.org/2025/01/22/parquet-encodings.html . Hardware: "a MacBook Pro with an M1 Max". Threads: not stated. Writer: "the upcoming DuckDB 1.2.0", not Arrow.

| Case | Size | Time | Quote | Counted |
|---|---|---|---|---|
| TPC-H SF1 lineitem, zstd, v1 encodings | 152 MB | ~0.58 s | "the file is ~11% smaller, and writing is ~24% faster with the encodings enabled." | File size, write time, v2 vs v1 |
| same, zstd, v2 encodings | 135 MB | ~0.44 s | same | same |
| `range(1e9::BIGINT)`, default codec, v1 / v2 | 3.7 GB / 1.3 MB | ~2.96 s / ~1.68 s | "The integer sequence 0, 1, 2, … compresses extremely well with DELTA_BINARY_PACKED." | same |
| `range / 1e9` over the same 1e9 rows, v1 / v2 | 6.3 GB / 610 MB | ~3.83 s / ~2.63 s | "It is ~90% smaller and writes ~31% faster." | same |

Applicability: context. Shows the size of the effect of `DELTA_BINARY_PACKED` and `BYTE_STREAM_SPLIT`, on data that favours them.

---

## 6. Independent tick-data store comparisons (kdb+, ClickHouse, TimescaleDB)

### 6.1 Barez, Bilokon, Xiong, "Benchmarking Specialized Databases for High-frequency Data", arXiv:2301.12561v1, Jan 2023

Status: preprint, not peer reviewed. Authors are academic; no vendor affiliation in the paper header. **The only independent study found with tick-style trades and order-book data and numbers.** No QuestDB, ArcticDB or Parquet.
URL: https://arxiv.org/pdf/2301.12561 (cached: CACHE/downloads/arxiv-2301.12561/2301.12561.pdf)
Hardware: "a personal PC with an Intel i7-1065G7 CPU and 16GB RAM. All databases save their data in SSD." (PDF p. 20). Software versions: not stated. Threads: not stated. Data: "around 20 million records in the trades table and 33 million records in the order book table", CSV total 25.14 GB, crypto exchanges, 2022 (PDF p. 19). Results: mean of 10 executions, cache cleared before each execution (PDF p. 21).

| Benchmark | kdb+ in-memory | kdb+ on-disk | InfluxDB | TimescaleDB | ClickHouse | Location | Counted |
|---|---|---|---|---|---|---|---|
| W, bulk CSV load | n/a | 33,889 ms | 324,854 ms | 53,150 ms | 765,000 ms | Table 5, PDF p. 22 | Time to load one day of trades and order book; the paper reports kdb+ at "47.35 MB/s" (PDF p. 21) |
| SE, storage efficiency | n/a | 90.20 % | 83.73 % | 94.68 % | 99.65 % | Table 5 | Size in database / size of file × 100, Equation 9, PDF p. 17. **Lower is better.** The paper's text calls this "compression ratio" |
| T-V1 | 57 ms | 81 ms | 93 ms | 469 ms | 272 ms | Table 6, PDF p. 23 | Query latency |
| O-T | 5 ms | 11 ms | 124 ms | 10 ms | 706 ms | Table 6 | Query latency |
| O-NBBO | 5 ms | 34 ms | 191 ms | 1,614 ms | 454 ms | Table 6 | Query latency |

Applicability: context only. A laptop-class machine, unstated versions, unstated tuning, 16 GB RAM against 25 GB of CSV, and benchmark W's one-day file is "generated randomly" (PDF p. 19), which limits what its compression figures say about real ticks. Its value is the benchmark design: trades, order book, as-of style queries, bulk load, storage efficiency.

### 6.2 TSM-Bench, Khelifati et al., PVLDB 16(11), 2023 (peer reviewed, not tick data)

URL: https://www.vldb.org/pvldb/vol16/p3363-khelifati.pdf (cached: CACHE/downloads/tsm-bench-pvldb23/p3363-khelifati.pdf)
Hardware: "Intel(R) Xeon(R) CPU E5-2620 v4 @ 2.10GHz (32cores), 128GB of memory, 4TB TOSHIBA Hard disk" on Ubuntu 20.04, one dedicated machine, separate client (Section 4.1). Software: QuestDB v6.2.1, ClickHouse v22.6.1, TimescaleDB v2.6.0. "The default configuration was used for each system." Data: synthetic sensor series, D-LONG 518M points, D-MULTI 17.2B points (Table 2). Threads: not stated. Counted: data points/s through each bulk-load utility; "the time to ingest the data and to make it available to queries" (Section 5.1). So this does measure query visibility.

| Dataset | System | Avg. throughput (data points/s) | Load (s) | Storage (GB) | Location |
|---|---|---|---|---|---|
| D-LONG | ClickHouse | 18,386,867 | 28 | 1.97 | Table 3 |
| D-LONG | QuestDB | 2,938,892 | 176 | 4.00 | Table 3 |
| D-LONG | TimescaleDB | 2,627,138 | 197 | 4.31 | Table 3 |
| D-MULTI | ClickHouse | 11,933,701 | 1,448 | 65.50 | Table 3 |
| D-MULTI | QuestDB | 1,920,000 | 9,000 | 132.00 | Table 3 |
| D-MULTI | TimescaleDB | 1,749,976 | 9,874 | 134.00 | Table 3 |

Applicability: context only. Peer reviewed and independent of vendors, but QuestDB 6.2.1 is about four major versions behind 10.1.0, and the disk is an HDD.

### 6.3 Vendor comparison with a kdb product

KX, "Benchmarking KDB-X vs QuestDB, ClickHouse, TimescaleDB and InfluxDB with TSBS", 14 November 2025, https://kx.com/blog/benchmarking-kdb-x-vs-questdb-clickhouse-timescaledb-and-influxdb-with-tsbs/
Hardware: "AMD EPYC 9755 (Turin), 2 sockets, 128 cores/sockets", 2.2 TB DDR5, Samsung PCIe 5.0 NVMe 3.84 TB. KDB-X restricted to "a single q process limited to 16GB of memory and four execution threads". Data: TSBS DevOps `cpu` table; three scenarios, up to 2,529,792,000 rows. Single client. Queries only. QuestDB version: not found in text extraction.

| Value | Quote | Counted | Applicability |
|---|---|---|---|
| 3.36 | "The closest was QuestDB, with an average slowdown factor of 3.36." | Geometric mean of mean-response-time ratios against KDB-X | Context only. Competitor-published; not ingest; not tick data. |

### 6.4 KX compression case study on NYSE data (vendor, tick data)

URL: https://code.kx.com/q/kb/compression/fsicasestudy/ (cached: CACHE/downloads/kdb/fsicasestudy.html)
Hardware: Intel SSD D7-P5510 3.84 TB NVMe, "Intel(R) Xeon(R) 6780E", 502 GiB DDR5; second system on NFS (section "Infrastructure"). Software: kdb+ 4.1 (2025.01.17), RHEL 9.4. Data: NYSE `quote` and `trade` tables; the page calls it "a popular financial dataset from the New York Stock Exchange"; its queries use date 2022.03.31. Threads: `set` "is single-threaded"; queries with `-s 144`. Block size 128 KB. Counted: "relative sizes after compression", % of uncompressed. Lower is better.

Values below are table cells, `quote` table unless stated. Generic block compression of uncompressed columns; no delta or XOR stage.

| Column | zstd 1 | zstd 10 | zstd 22 | lz4 5 | snappy | gzip 6 |
|---|---|---|---|---|---|---|
| Participant_Timestamp | 41.3 | 40.9 | 41.0 | 69.5 | 71.5 | 45.0 |
| Time | 31.3 | 31.3 | 33.8 | 61.1 | 62.4 | 36.3 |
| Bid_Price | 7.6 | 6.4 | 5.6 | 11.1 | 15.8 | 6.6 |
| Symbol | 0.1 | 0.0 | 0.0 | 0.4 | 4.7 | 0.2 |
| TradePrice (`trade` table) | 20.2 | 17.1 | 15.2 | 24.1 | 29.9 | 16.7 |

Applicability: context, closest public per-column compression table for US equity ticks. Good baseline for a generic-codec-only column, not an ITCH measurement. Table-level figures appear only in a chart (`compression_rel_size_total.png`), so they are not transcribed. Durability parallel: "Data persisted via set may remain in the OS buffer cache before being written to disk, risking data loss if the system crashes." KX's advice: "Hot tier should not employ compression to allow maximal ingestion rate and fastest queries."

### 6.5 Paywalled or restricted

| Item | Status | Location |
|---|---|---|
| STAC-M3 (kdb+, audited, tick analytics) | Reports are behind login. "The benchmark report is available to all STAC Observer members." The page offers registration "for no charge". No report was opened, so no STAC number is recorded here. | https://docs.stacresearch.com/KDB250929 ; cached stac-m3/KDB250929.html |
| kdb+ / KDB-X in third-party benchmarks | KX Community licence blocks publication: "You will not disclose any benchmark, test or performance information" without written authorisation. ClickBench ships no kdb results for this reason. Same applies if tick-store ever benchmarks KDB-X Community. | https://github.com/ClickHouse/ClickBench/blob/main/kdb/README.md |

Blog-grade context, not independent evidence of quality: Sheftel Financial, "Data Store Speed Comparisons", 2023-11-18, https://blog.sheftel.net/2023/11/18/data-store-speed-comparisons/ , used pyarrow 14.0.1 and arcticdb 4.1.0 on 1-minute bars through pandas. Too old and not tick-level. Its charts were not transcribed.

---

## 7. Fair-run settings per system

"Source" gives where the setting or its default is documented. **"Judgement" marks this file's reasoning for tick-store, not a vendor recommendation.** Values marked "sweep" have no documented answer for this workload. All are to be recorded in the benchmark output.

### Common to all systems (Judgement, from 2.4 and 5C)

| Setting | Value | Source |
|---|---|---|
| Durability class | Run class A and class B for every system, on Linux | Section 2.4 |
| Clock stop | After data is readable, not after the client returns | TSM-Bench Section 5.1 "to make it available to queries"; QuestDB WAL "How WAL works" |
| Cache | Report cold and warm reads separately | Barez et al. Figure 10 (PDF p. 21); QuestDB Arrow post, 16 GB run |
| Input | Same rows, same time order, same schema | QuestDB ILP "Data ordering" |
| Reporting | Rows and bytes decoded per range query, plus latency | Section 4 summary |

### Parquet (Arrow C++ / pyarrow 26.0.0)

| Setting | Value | Status | Source |
|---|---|---|---|
| Format version | `version='2.6'` | Default, keeps ns timestamps native | python/pyarrow/parquet/core.py:760, 791-793 |
| Row group | Sweep: 2^17 rows, 1 Mi rows, and a group of about 512 MB | Sweep. Main range-query knob. | Arrow default properties.h:180; BtrBlocks Section 6; parquet.apache.org Configurations |
| Data page | 1 MiB default, or 8 KB with a page-index-aware reader | Default for pyarrow runs | core.py:805; parquet.apache.org Configurations |
| Dictionary | `use_dictionary=[<low-cardinality columns>]`, including symbol | Judgement | core.py:772; _parquet.pyx:2128-2144 |
| Time column | `column_encoding={'ts': 'DELTA_BINARY_PACKED'}` | Judgement, one run vs default | core.py `column_encoding`; Parquet encodings page |
| Integer price and size | `DELTA_BINARY_PACKED` | Sweep | same |
| Float columns | `BYTE_STREAM_SPLIT` with a codec | Sweep | core.py:823-828 |
| Codec | Sweep: none, snappy (pyarrow default), zstd level 1 (Arrow default), one higher level | Sweep | core.py:778; compression_internal.h:76; Zeng Section 5.4 |
| Statistics | `write_statistics=True` | Default. Row-group pruning needs them. | core.py:783; file_parquet.cc:915-966 |
| Page index | State explicitly. `False` for pyarrow readers. `True` only with a reader that uses it. | Defaults differ by API | core.py:889, 895; properties.h:186; parquet.rst:662-663 |
| Bloom filter | Off | Readers do not use it; "useful only for point queries" | parquet.rst:662-663; Zeng Section 5.6 |
| Sort metadata | `sorting_columns` set if file is time sorted | Optional. Writer does not verify. | core.py `sorting_columns` |
| Class A | Nothing to set | Default | Section 2.1 |
| Class B | Harness calls fsync on file and parent directory after close, reported as added | Not an Arrow feature | Section 2.1 |
| Reader | `pq.read_table(filters=...)`, with `use_threads` and `pre_buffer` stated | Record | core.py:1362, 1903 |

### ArcticDB 6.28.2 on LMDB

| Setting | Value | Status | Source |
|---|---|---|---|
| URI | `lmdb:///path?map_size=<above data size>`, on local disk | Default map is 400 GiB off Windows | lmdb_storage.cpp:438; docs `index.md` line 479 |
| Instances | One Arctic instance per LMDB path per process | Required | docs `index.md` line 485 |
| `rows_per_segment` | Sweep: 100,000 (default) and 10,000,000 | Sweep. Fixed at library creation. | options.py:100; billion-row notebook; `lib_config.md` |
| `columns_per_segment` | 127 (default) and 1 | Sweep. 1 is needed for column pruning on narrow tables. | options.py:129; ClickBench arcticdb/README.md |
| `dynamic_schema` | False (default) | Default. Note its linear index walk. | options.py; query.cpp:109 |
| `dedup` | False (default) | Default | options.py:46 |
| `encoding_version` | Default | v2 is "still experimental" | options.py:132-134 |
| Strings | Dynamic (default). Not categorical. | Default | _store.py:955; docs `faq.md` |
| Index | Sorted datetime index, `validate_index=True` | Default | library.py:1069 |
| Load | One large `write` per day, or bounded appends then `compact_data`. Consider `stage` then `finalize_staged_data` for parallel load. | Judgement | docs `tutorials/data_organisation` line 136; ClickBench README lines 95-96; library.py:958-968 |
| Symbol layout | Run one symbol for all, and one symbol per security | Sweep. Docs list both. | docs `tutorials/data_organisation`, lines 145-186 |
| Filter by security | `QueryBuilder` on a data column. There is no secondary index. | Note | same, line 171 |
| Column stats | Off for the baseline. One optional run with `ColumnStats.UseForQueries=1`. | Experimental | column_stats_filter.cpp:26; _store.py:1319 |
| Threads | Set `VersionStore.NumCPUThreads` and `VersionStore.NumIOThreads` to the run's core budget | Record | docs `runtime_config.md` line 153 |
| Class A | `ARCTICDB_LMDBStorage_ExtraFlags_int=327680` | Docs mark as throwaway only | docs `runtime_config.md` lines 31-40, 117 |
| Class B | Default, `LMDBStorage.ExtraFlags=0` | Default | Section 2.2 |
| Size | `du` on the LMDB directory, and `lib.admin_tools()` totals | Record | docs `tutorials/library_sizes`; ClickBench arcticdb/data-size |

Context label required in any report: LMDB is not ArcticDB's highest-performance backend (docs, `tutorials_lmdb_and_in_memory.md`).

### QuestDB 10.1.0

| Setting | Value | Status | Source |
|---|---|---|---|
| Platform | Linux on EXT4 or XFS, local NVMe; `ulimit -n` and `vm.max_map_count` at 1048576 | Docs recommend | Capacity planning, lines 30, 66-70, 327-330 |
| Table | `CREATE TABLE ... (ts TIMESTAMP_NS, symbol SYMBOL, ...) TIMESTAMP(ts) PARTITION BY DAY WAL` | Docs. Partition size from daily row count. | Designated timestamp; Partitions lines 53-65 |
| Partition | `DAY` for 30-500 M rows/day; `HOUR` above 1 B | Docs | Partitions lines 59-65 |
| Symbol capacity | Do not set `CAPACITY` | Docs conflict, see 3.3 | Concepts > Symbol lines 41-42; `cairo.default.symbol.capacity` |
| Symbol index | Run none, and `SYMBOL INDEX`; report both | Sweep. Costs ingest and storage. | Indexes lines 21-34, 123 |
| Timestamp order | Send in order; leave O3 settings at default | Docs | ILP "Data ordering" |
| Ingest path | Report each used: ILP HTTP, QWP, and `COPY` into an empty partitioned table | Record. COPY needs a CSV in `cairo.sql.copy.root`. | Ingestion config intro; QWP docs; COPY "Import modes" |
| Throughput counter | Rows applied, not "rows sent" | Judgement | QWP post, Setup |
| Client placement | Same host as tick-store's writer, or state otherwise | Judgement | QWP post "co-located run" |
| Class A | `cairo.commit.mode=nosync` | Default | PropServerConfiguration.java:3079 |
| Class B | `cairo.commit.mode=sync`. Do not use `async`. | Docs | Storage engine "Durability"; MemoryPMARImpl.java:141-148 |
| End of ingest | `SELECT wait_wal_table('<table>');` after the last write, or `writerTxn = sequencerTxn` in `wal_tables()` | Docs | https://questdb.com/docs/query/functions/meta/#wal_tables |
| Plan check | `EXPLAIN` must show `Interval forward scan` | Docs | Interval scan |
| Storage size | Report native and after `ALTER TABLE ... CONVERT PARTITION`. Default codec `lz4_raw`. | Record | Capacity planning; Concepts > Parquet line 157 |

---

## 8. Not found

| Item | Where I looked | Result |
|---|---|---|
| Any benchmark of Parquet, ArcticDB and QuestDB on the same tick data | Previous agent's searches (digest lines 1417, 2718), this task's reading of all cached sources | None. Nearest are ClickBench (not ticks) and Barez et al. (none of the three). |
| Any published number on Nasdaq ITCH for these systems | Same | None |
| QuestDB ingest throughput under `cairo.commit.mode=sync` | Digest search at line 3222; cached QuestDB blogs and docs | None. Docs say only "at the cost of reduced write throughput". |
| Commit mode in QuestDB's blog benchmarks | clickhouse-vs-questdb, influxdb-vs-questdb, qwp-vs-ilp cached HTML | Not stated. CH and Influx posts say "default configurations". |
| QuestDB compression ratio from Parquet conversion or ZFS | concepts_parquet.md; deployment/compression-zfs.md (fetched this task) | No ratio. Only codec options. |
| ArcticDB vendor ingest throughput or compression ratio | docs (`index.md`, `faq.md`, tutorials), arcticdb.io and man.com cached pages, digest search line 1415 | None. Only "rows per second" read claims with no method. |
| Public ArcticDB option for zstd | options.py, library.py, _store.py grep `zstd\|lz4`; docs | Not found. Proto has the field; default code uses LZ4. Whether it is reachable through another path was not established. |
| Apache Arrow or Parquet project write-throughput or query-latency figures | Digest search line 2724; arrow-blog/ms-latency.html is about the Rust reader and gave no numbers for this table; docs tree | None for Arrow C++/pyarrow |
| Peer-reviewed independent kdb+ vs QuestDB on ticks | Digest searches | None. KX's own post is the only direct comparison. |
| STAC-M3 report contents | stac-m3 cached listing pages | Login required. Not opened. |
| Medium post "Picking the Fastest Database to Store Time-Series Data" | https://medium.com/@smakhnist/picking-the-fastest-database-to-store-time-series-data-411ca3651277 | HTTP 403 via WebFetch. Not read. |
| ClickBench hardware detail, versions | Result JSONs | Only instance name. Versions not stated. |
| QuestDB docs version pin | questdb.com docs | Unversioned. Docs fetched from `questdb/documentation` `main`, not tied to the 10.1.0 tag. Re-check before quoting in a report. |
| Dates of QWP and Arrow-streaming blog posts | Cached HTML | Only "3 months ago", relative to the fetch date. |
| Published table-level kdb+ compression numbers | fsicasestudy page | Only in a chart image, so not transcribed. |
