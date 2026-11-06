"""ArcticDB on LMDB. One library per store, and either one symbol for the whole table or one
symbol per security, the two layouts ArcticDB's docs describe.

ArcticDB's settings come from the environment before it loads, so worker_environment sets
them and settings_in_force reads them back through the engine's own config. LMDB syncs each
transaction by default, which is class B. Class A sets MDB_NOSYNC | MDB_NOMETASYNC, which
ArcticDB's docs mark as for throwaway data.
"""

import platform
import re
import struct
import time

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.compute as pc

from compare.schema import TABLE_COLUMNS
from compare.settings import Setting, SettingsMismatch, require_all_match
from compare.systems.base import System

SYMBOL = "ticks"
LIBRARY = "ticks"
MAP_SIZE_BYTES = 2_000_000_000  # ArcticDB reads GB as 10^9. Read back from the meta page.
MAP_SIZE_TEXT = "2GB"
FULL_DAY_MAP_SIZE_BYTES = 64_000_000_000  # A slice's 2 GB cannot hold a day. Sparse until used.
FULL_DAY_MAP_SIZE_TEXT = "64GB"
# Rows per chunk of a streamed day, in whole segments, as near this as a segment allows. Each
# append stores segments of its own, so a chunk holds whole ones; 40 of 100k is the headline's.
STREAM_CHUNK_ROWS = 4_000_000
LMDB_NO_SYNC_FLAGS = 0x10000 | 0x40000  # MDB_NOSYNC | MDB_NOMETASYNC
LMDB_META_MAGIC = 0xBEEFC0DE
LMDB_META_OFFSET = 16
INDEX_COLUMN = "ts"
DEFAULT_ENCODING_VERSION = 0  # EncodingVersion.V1. V2 is marked experimental.
AGGREGATION = {"n": ("shares", "count"), "s": ("shares", "sum"), "lo": ("price", "min"),
               "hi": ("price", "max")}


ONE_SYMBOL = "one symbol for the whole table"
PER_SECURITY = "one symbol per security"
SECURITY_PREFIX = "locate-"
PROBE_PREFIX = "flush-probe-"


def _variant(rows, columns, layout=ONE_SYMBOL):
    return {"rows_per_segment": rows, "columns_per_segment": columns, "symbol_layout": layout}


def security_symbol(locate, prefix=SECURITY_PREFIX):
    return f"{prefix}{int(locate):05d}"


class ArcticDBSystem(System):
    name = "arcticdb"
    headline_variant = "rows100k-cols127"
    # Per security: ArcticDB's docs, "Separate Symbol for Each Security", in
    # https://docs.arcticdb.io/latest/tutorials/data_organisation/. See compare/README.md.
    variants = {
        "rows100k-cols127": _variant(100_000, 127),
        "rows10M-cols127": _variant(10_000_000, 127),
        "rows100k-cols1": _variant(100_000, 1),
        "rows100k-cols127-per-security": _variant(100_000, 127, PER_SECURITY),
    }

    # LMDB's reader table, a mapped scratch file that LMDB rebuilds. It holds no data.
    flush_exempt = (re.compile(r"(?:^|/)lock\.mdb$"),)

    def __init__(self, context):
        super().__init__(context)
        self.library = None
        self.map_size_bytes = FULL_DAY_MAP_SIZE_BYTES if context.full_day else MAP_SIZE_BYTES
        self.map_size_text = FULL_DAY_MAP_SIZE_TEXT if context.full_day else MAP_SIZE_TEXT
        self.symbols = []
        self.rows_read = None

    @property
    def per_security(self):
        return self.tuning["symbol_layout"] == PER_SECURITY

    def worker_environment(self):
        flags = 0 if self.class_b else LMDB_NO_SYNC_FLAGS
        return {"ARCTICDB_LMDBStorage_ExtraFlags_int": str(flags),
                "ARCTICDB_VersionStore_NumCPUThreads_int": str(self.context.threads),
                "ARCTICDB_VersionStore_NumIOThreads_int": str(self.context.threads)}

    def requested(self):
        return {"uri": f"lmdb, on a local disk, map_size={self.map_size_text}",
                "symbol_layout": self.tuning["symbol_layout"],
                "write": ("one lib.write_batch of a pandas frame per security, then one "
                          "lib.list_symbols, as ArcticDB asks after many writes"
                          if self.per_security else "one lib.write of a pandas frame"),
                "security_symbols": (f"{SECURITY_PREFIX} and the locate, each holding that "
                                     "locate's rows in file order" if self.per_security
                                     else "not used"),
                "rows_per_segment": self.tuning["rows_per_segment"],
                "columns_per_segment": self.tuning["columns_per_segment"],
                "dynamic_schema": False, "dedup": False, "encoding_version": "default, V1",
                "column_stats": "off", "strings": "dynamic",
                "reader": "lib.read with OutputFormat.PYARROW, so no pandas on the read path",
                **self.worker_environment()}

    def service_facts(self):
        return {"context": "LMDB is not ArcticDB's highest-performance backend, by its own docs",
                "class_a_flags": "marked in ArcticDB's docs as for throwaway data"}

    def open(self):
        import arcticdb
        from arcticdb.options import LibraryOptions

        pa.set_cpu_count(self.context.threads)
        arctic = arcticdb.Arctic(f"lmdb://{self.root}?map_size={self.map_size_text}")
        if arctic.has_library(LIBRARY):
            self.library = arctic[LIBRARY]
        else:
            self.library = arctic.create_library(LIBRARY, library_options=LibraryOptions(
                rows_per_segment=self.tuning["rows_per_segment"],
                columns_per_segment=self.tuning["columns_per_segment"]))
        self.arctic = arctic
        if self.per_security:
            # The securities a query over all of them must read. Listed before any clock, as an
            # application keeps its universe.
            self.symbols = sorted(symbol for symbol in self.library.list_symbols()
                                  if symbol.startswith(SECURITY_PREFIX))

    def close(self):
        self.library = None
        self.arctic = None

    def _write_options(self):
        return self.library._nvs.lib_cfg().lib_desc.version.write_options

    def settings_in_force(self):
        import arcticdb_ext

        options = self.library.options()
        config = arcticdb_ext.get_config_int
        write_options = self._write_options()
        flags = 0 if self.class_b else LMDB_NO_SYNC_FLAGS
        return require_all_match([
            Setting("rows_per_segment", self.tuning["rows_per_segment"],
                    options.rows_per_segment, "lib.options()"),
            Setting("columns_per_segment", self.tuning["columns_per_segment"],
                    options.columns_per_segment, "lib.options()"),
            Setting("dynamic_schema", False, options.dynamic_schema, "lib.options()"),
            Setting("dedup", False, options.dedup, "lib.options()"),
            Setting("encoding_version", DEFAULT_ENCODING_VERSION, int(options.encoding_version),
                    "lib.options()"),
            Setting("segment_row_size", self.tuning["rows_per_segment"],
                    write_options.segment_row_size, "library config in storage"),
            Setting("column_group_size", self.tuning["columns_per_segment"],
                    write_options.column_group_size, "library config in storage"),
            Setting("LMDBStorage.ExtraFlags", flags, config("LMDBStorage.ExtraFlags") or 0,
                    "ArcticDB's config, as the engine sees it"),
            Setting("VersionStore.NumCPUThreads", self.context.threads,
                    config("VersionStore.NumCPUThreads"), "ArcticDB's config"),
            Setting("VersionStore.NumIOThreads", self.context.threads,
                    config("VersionStore.NumIOThreads"), "ArcticDB's config"),
        ])

    def _security_payloads(self, table, prefix):
        """One WritePayload per locate of `table`, its rows in the table's order, from a stable
        sort by locate."""
        from arcticdb import WritePayload

        ordered = table.take(pc.sort_indices(table, sort_keys=[("locate", "ascending")]))
        locates = ordered.column("locate").to_numpy()
        breaks = np.flatnonzero(np.diff(locates)) + 1
        starts = np.concatenate([[0], breaks])
        ends = np.concatenate([breaks, [len(locates)]])
        frame = _frame(ordered)
        payloads = [WritePayload(security_symbol(locates[first], prefix), frame.iloc[first:end])
                    for first, end in zip(starts, ends, strict=True)]
        return payloads, len(payloads)

    def _write_securities(self, table, prefix):
        """One symbol per locate, each that locate's rows in file order, in one write_batch."""
        started = time.perf_counter()
        payloads, _ = self._security_payloads(table, prefix)
        built = time.perf_counter()
        results = self.library.write_batch(payloads)
        failed = [result for result in results if not hasattr(result, "version")]
        if failed:
            raise SettingsMismatch(f"write_batch refused {len(failed)} symbols: {failed[0]}")
        written = time.perf_counter()
        listed = len(self.library.list_symbols())
        finished = time.perf_counter()
        return {"seconds": finished - started, "engine_seconds": finished - built,
                "python_seconds": built - started, "symbols": len(payloads),
                "write_batch_seconds": written - built, "list_symbols_seconds": finished - written,
                "symbols_listed": listed,
                "python_note": "lib.write_batch takes pandas, so the table is sorted by locate "
                               "and made one frame, and each security's rows are a slice of it. "
                               "The list_symbols after the write is ArcticDB's own compaction of "
                               "its symbol list, which it asks for after many writes"}

    def ingest(self, columns, table):
        if self.per_security:
            return self._write_securities(table, SECURITY_PREFIX)
        started = time.perf_counter()
        frame = _frame(table)
        built = time.perf_counter()
        self.library.write(SYMBOL, frame)
        finished = time.perf_counter()
        return {"seconds": finished - started, "engine_seconds": finished - built,
                "python_seconds": built - started,
                "python_note": "lib.write takes pandas and not Arrow, so the frame is built "
                               "first, with a Python str per text value"}

    def stream_chunk_rows(self):
        """Whole segments, about STREAM_CHUNK_ROWS rows, and one segment where a segment is
        larger: `rows10M-cols127` once took the whole day as one chunk, and its frame passed
        11.9 GB before the guard stopped it on a 15 GB host."""
        segment = self.tuning["rows_per_segment"]
        return segment * max(1, STREAM_CHUNK_ROWS // segment)

    def ingest_stream(self, chunks):
        """The first chunk is a write, and each one after it an append, so one frame at a time
        exists. Each append makes a version, and none is pruned. The clock holds reading each
        chunk of the column files, which a day cannot keep warm."""
        if self.per_security:
            raise NotImplementedError("one symbol per security streams through ingest_day")
        chunks = iter(chunks)
        started = time.perf_counter()
        python_seconds = 0.0
        appends = 0
        while True:
            before = time.perf_counter()
            table = next(chunks, None)
            if table is None:
                python_seconds += time.perf_counter() - before
                break
            frame = _frame(table)
            python_seconds += time.perf_counter() - before
            if appends == 0 and not self.library.has_symbol(SYMBOL):
                self.library.write(SYMBOL, frame)
            else:
                self.library.append(SYMBOL, frame)
                appends += 1
        finished = time.perf_counter()
        return {"seconds": finished - started, "python_seconds": python_seconds,
                "engine_seconds": finished - started - python_seconds, "appends": appends,
                "python_note": "python_seconds reads each chunk of the column files and builds "
                               "the pandas frame lib.write and lib.append take, with a Python "
                               "str per text value. The first chunk is a write, the rest "
                               "appends"}

    def ingest_day(self, columns):
        if not self.per_security:
            return super().ingest_day(columns)
        return self._write_securities_day(columns)

    def _write_securities_day(self, columns):
        """One symbol per locate over a whole day, each written once, whole, in file order, as
        `_write_securities` does on the slice. A day sorted by locate does not fit in memory, so
        the rows are first spread by locate into bucket files of about one chunk each, in file
        order, and each bucket is then sorted by locate, stably, and given to one write_batch.
        Everything is inside the clock, as the slice's sort is."""
        import shutil

        import pyarrow.ipc as ipc

        from compare.schema import table_chunks

        started = time.perf_counter()
        chunk_rows = self.stream_chunk_rows()
        counts = np.bincount(np.asarray(columns.fixed["locate"]))
        bucket_of = _locate_buckets(counts, chunk_rows)
        scratch = self.context.layout.work / "partition" / self.root.name
        shutil.rmtree(scratch, ignore_errors=True)
        scratch.mkdir(parents=True)
        writers = {}
        writers_open = []
        try:
            for table in table_chunks(columns, self.context.day, chunk_rows):
                buckets = bucket_of[table.column("locate").to_numpy()]
                order = np.argsort(buckets, kind="stable")
                ordered = table.take(pa.array(order))
                sorted_buckets = buckets[order]
                breaks = np.flatnonzero(np.diff(sorted_buckets)) + 1
                for first, end in zip(np.concatenate([[0], breaks]),
                                      np.concatenate([breaks, [len(order)]]), strict=True):
                    bucket = int(sorted_buckets[first])
                    if bucket not in writers:
                        writers[bucket] = ipc.new_file(scratch / f"{bucket:05d}.arrow",
                                                       table.schema)
                        writers_open.append(writers[bucket])
                    writers[bucket].write_table(ordered.slice(first, end - first))
            while writers_open:
                writers_open.pop().close()
            partitioned = time.perf_counter()
            python_seconds = partitioned - started
            batch_seconds = 0.0
            symbols = 0
            for bucket in sorted(writers):
                before = time.perf_counter()
                path = scratch / f"{bucket:05d}.arrow"
                with pa.memory_map(str(path)) as source:
                    table = ipc.open_file(source).read_all()
                payloads, built = self._security_payloads(table, SECURITY_PREFIX)
                del table
                path.unlink()
                python_seconds += time.perf_counter() - before
                before = time.perf_counter()
                results = self.library.write_batch(payloads)
                failed = [result for result in results if not hasattr(result, "version")]
                if failed:
                    raise SettingsMismatch(f"write_batch refused {len(failed)} symbols: "
                                           f"{failed[0]}")
                batch_seconds += time.perf_counter() - before
                symbols += built
            written = time.perf_counter()
            listed = len(self.library.list_symbols())
            finished = time.perf_counter()
        finally:
            while writers_open:
                writers_open.pop().close()
            shutil.rmtree(scratch, ignore_errors=True)
        return {"seconds": finished - started, "python_seconds": python_seconds,
                "engine_seconds": finished - started - python_seconds,
                "partition_seconds": partitioned - started, "symbols": symbols,
                "buckets": len(writers), "write_batch_seconds": batch_seconds,
                "list_symbols_seconds": finished - written, "symbols_listed": listed,
                "python_note": "python_seconds spreads the day's rows by locate into bucket "
                               "files of about one chunk, reading the column files as it goes, "
                               "then sorts each bucket by locate and makes its frame. "
                               "engine_seconds is one write_batch per bucket, each security "
                               "whole in one of them, and the list_symbols after, ArcticDB's "
                               "own compaction of its symbol list"}

    def probe_flush(self, table):
        if self.per_security:
            self._write_securities(table, PROBE_PREFIX)
            return
        self.library.write("flush-probe", _frame(table))

    def _lmdb_directory(self):
        return self.root / LIBRARY

    def dirty_paths(self):
        return [self._lmdb_directory() / "data.mdb"]

    def _read(self, symbol=SYMBOL, **arguments):
        from arcticdb.options import OutputFormat

        return self.library.read(symbol, output_format=OutputFormat.PYARROW, **arguments).data

    def _read_every_security(self, **arguments):
        """Every security's symbol in one read_batch, as one table in no particular order."""
        from arcticdb import ReadRequest
        from arcticdb.options import OutputFormat

        results = self.library.read_batch(
            [ReadRequest(symbol, **arguments) for symbol in self.symbols],
            output_format=OutputFormat.PYARROW)
        failed = [result for result in results if not hasattr(result, "data")]
        if failed:
            raise SettingsMismatch(f"read_batch failed on {len(failed)} symbols: {failed[0]}")
        return pa.concat_tables([result.data for result in results])

    def read_all(self):
        wanted = [c for c in TABLE_COLUMNS if c != INDEX_COLUMN]
        if self.per_security:
            table = self._read_every_security(columns=wanted).sort_by("seq")
        else:
            table = self._read(columns=wanted).sort_by("seq")
        self.rows_read = table.num_rows
        return table

    def read_window(self, start_ns, end_ns):
        wanted = [c for c in TABLE_COLUMNS if c != INDEX_COLUMN]
        window = self._range(start_ns, end_ns)
        if self.per_security:
            table = self._read_every_security(date_range=window, columns=wanted)
        else:
            table = self._read(date_range=window, columns=wanted)
        return table.sort_by("seq")

    def read_back_to(self, destination, windows):
        """As every system, and the rows read become the count the stored checks compare."""
        answer = super().read_back_to(destination, windows)
        self.rows_read = answer["rows"]
        return answer

    def _range(self, start_ns, end_ns):
        """ArcticDB's date_range includes its end, and the queries' windows do not."""
        midnight = self.context.day.midnight_ns
        return (pd.Timestamp(start_ns + midnight, unit="ns"),
                pd.Timestamp(end_ns + midnight - 1, unit="ns"))

    def query_rows(self, start_ns, end_ns, locate, columns):
        from arcticdb import QueryBuilder

        wanted = [c for c in columns if c != INDEX_COLUMN]
        if self.per_security:
            window = self._range(start_ns, end_ns)
            if locate >= 0:
                return self._read(security_symbol(locate), date_range=window, columns=wanted)
            return self._read_every_security(date_range=window, columns=wanted)
        builder = None
        if locate >= 0:
            builder = QueryBuilder()
            builder = builder[builder["locate"] == locate]
        return self._read(date_range=self._range(start_ns, end_ns), columns=wanted,
                          query_builder=builder)

    def query_minute_aggregate(self, start_ns, end_ns, locate, msg_types):
        from arcticdb import QueryBuilder

        builder = QueryBuilder()
        if self.per_security:
            builder = builder[builder["msg_type"].isin(list(msg_types))]
        else:
            builder = builder[(builder["locate"] == locate) &
                              builder["msg_type"].isin(list(msg_types))]
        builder = builder.date_range(self._range(start_ns, end_ns))
        builder = builder.resample("1min").agg(AGGREGATION)
        symbol = security_symbol(locate) if self.per_security else SYMBOL
        table = self._read(symbol, query_builder=builder)
        rows = zip(table.column(INDEX_COLUMN).cast(pa.int64()).to_pylist(),
                   table.column("n").to_pylist(), table.column("s").to_pylist(),
                   table.column("lo").to_pylist(), table.column("hi").to_pylist(), strict=True)
        return [[int(value) for value in row] for row in rows if row[1]]

    def query_column_sum(self, column):
        if self.per_security:
            values = self._read_every_security(columns=[column]).column(column)
        else:
            values = self._read(columns=[column]).column(column)
        return [[len(values), pc.sum(values, min_count=0).as_py() or 0]]

    def _query_symbols(self, query):
        if not self.per_security:
            return [SYMBOL]
        if query.kind != "column_sum" and query.locate >= 0:
            return [security_symbol(query.locate)]
        return self.symbols

    def scan_facts(self, query):
        """Segments that overlap the window, from the stored index. Bytes are not available."""
        midnight = self.context.day.midnight_ns
        rows = 0
        for symbol in self._query_symbols(query):
            index = self.library._nvs.read_index(symbol)
            if query.kind != "column_sum":
                first = pd.Timestamp(query.start_ns + midnight, unit="ns")
                last = pd.Timestamp(query.end_ns + midnight, unit="ns")
                index = index[(index["end_index"] > first) & (index.index < last)]
            starts = index.drop_duplicates(subset=["start_row", "end_row"])
            rows += int((starts["end_row"] - starts["start_row"]).sum())
        return {"rows_decoded": rows, "symbols_read": len(self._query_symbols(query)),
                "bytes_decoded": {"status": "unavailable",
                                  "reason": "ArcticDB reports neither decoded nor read bytes"},
                "method": "the stored index: rows of the segments whose time range overlaps "
                          "the window. A query filter still decodes each of them."}

    def _lmdb_map_size(self):
        """The map size LMDB itself recorded, from the meta page of data.mdb."""
        with open(self._lmdb_directory() / "data.mdb", "rb") as stored:
            page = stored.read(LMDB_META_OFFSET + 24)
        magic, _version, _address, map_size = struct.unpack_from("<IIQQ", page, LMDB_META_OFFSET)
        if magic != LMDB_META_MAGIC:
            return f"bad meta magic {magic:#x}"
        return map_size

    def _segment_codec(self, tool, keys, index, symbol=SYMBOL):
        """The first stored segment, decoded here with a plain LZ4 reader. Its first column
        must equal the first timestamps the engine returns, or the codec is not LZ4. The
        segment header has no Python binding in 6.28.2, so this reads the bytes after it."""
        rows = int(index["end_row"].iloc[0] - index["start_row"].iloc[0])
        keys = [key for key in keys if key.id == symbol]
        first = min(keys, key=lambda key: key.start_index)
        stored = tool.read_to_segment(first).bytes
        expected = self._read(symbol, row_range=(0, rows), columns=["seq"]).column(INDEX_COLUMN)
        expected = expected.cast(pa.int64()).to_numpy().astype("<i8").tobytes()
        try:
            decoded = lz4_block_prefix(stored, len(expected))
        except (ValueError, IndexError) as error:
            raise SettingsMismatch(f"the first segment is not an LZ4 block: {error}") from error
        checks = require_all_match([Setting("first column of the first segment is LZ4", True,
                                            decoded == expected, "stored segment bytes")])
        return {"codec": "LZ4", "checks_passed": len(checks), "rows_decoded": rows,
                "stored_segment_bytes": len(stored),
                "method": "the bytes after the segment header, decoded by a plain LZ4 block "
                          "reader, equal the first timestamps as the engine returns them",
                "acceleration": "not read: the header has no Python binding in 6.28.2"}

    def _stored_securities(self):
        """Per security: every symbol's index, the rows across them, and the busiest one's first
        segment through the plain LZ4 reader."""
        import arcticdb_ext

        descriptions = self.library.get_description_batch(self.symbols)
        rows = sum(int(item.row_count) for item in descriptions)
        indices = {symbol: self.library._nvs.read_index(symbol) for symbol in self.symbols}
        rows_per = max(int((index["end_row"] - index["start_row"]).max())
                       for index in indices.values())
        columns_per = max(int((index["end_col"] - index["start_col"]).max())
                          for index in indices.values())
        busiest = max(self.symbols, key=lambda symbol: int(indices[symbol]["end_row"].max()))
        tool = self.library._nvs.library_tool()
        keys = tool.find_keys(arcticdb_ext.storage.KeyType.TABLE_DATA)
        index = indices[busiest].reset_index().sort_values(["start_row", "start_col"])
        codec = self._segment_codec(tool, keys, index, busiest)
        checked = require_all_match([
            Setting("largest segment rows <= rows_per_segment", True,
                    bool(rows_per <= self.tuning["rows_per_segment"]), "every symbol's index"),
            Setting("largest segment columns <= columns_per_segment", True,
                    bool(columns_per <= self.tuning["columns_per_segment"]),
                    "every symbol's index"),
            Setting("LMDB map size", self.map_size_bytes, self._lmdb_map_size(),
                    "meta page of data.mdb"),
            Setting("rows across the symbols", self.rows_read, rows,
                    "lib.get_description_batch against the rows read back"),
            Setting("one symbol per security", True,
                    all(symbol.startswith(SECURITY_PREFIX) for symbol in self.symbols),
                    "lib.list_symbols"),
        ])
        sizes = {key.name: {"compressed_bytes": size.bytes_compressed, "keys": size.count}
                 for key, size in self.library.admin_tools().get_sizes().items()}
        return {"checks_passed": len(checked), "symbols": len(self.symbols),
                "segments": int(sum(len(index) for index in indices.values())),
                "data_keys": len(keys), "largest_segment_rows": rows_per,
                "largest_segment_columns": columns_per,
                "lmdb_map_size": self._lmdb_map_size(), "segment_codec": codec,
                "segment_codec_symbol": busiest, "admin_tools_sizes": sizes}

    def stored_settings(self):
        import arcticdb_ext

        if self.per_security:
            return self._stored_securities()
        description = self.library.get_description(SYMBOL)
        index = self.library._nvs.read_index(SYMBOL).reset_index().sort_values(
            ["start_row", "start_col"])
        rows_per = (index["end_row"] - index["start_row"]).max()
        columns_per = (index["end_col"] - index["start_col"]).max()
        tool = self.library._nvs.library_tool()
        keys = tool.find_keys(arcticdb_ext.storage.KeyType.TABLE_DATA)
        codec = self._segment_codec(tool, keys, index)
        checked = require_all_match([
            Setting("largest segment rows <= rows_per_segment", True,
                    bool(rows_per <= self.tuning["rows_per_segment"]), "stored index"),
            Setting("largest segment columns <= columns_per_segment", True,
                    bool(columns_per <= self.tuning["columns_per_segment"]), "stored index"),
            Setting("LMDB map size", self.map_size_bytes, self._lmdb_map_size(),
                    "meta page of data.mdb"),
            Setting("rows in the description", int(description.row_count),
                    int(description.row_count), "lib.get_description"),
        ])
        sizes = {key.name: {"compressed_bytes": size.bytes_compressed, "keys": size.count}
                 for key, size in self.library.admin_tools().get_sizes().items()}
        return {"checks_passed": len(checked), "segments": len(index), "data_keys": len(keys),
                "largest_segment_rows": int(rows_per),
                "largest_segment_columns": int(columns_per),
                "lmdb_map_size": self._lmdb_map_size(), "segment_codec": codec,
                "admin_tools_sizes": sizes}

    def durability_facts(self):
        if not self.class_b:
            return {"sync_primitive": "none: MDB_NOSYNC | MDB_NOMETASYNC",
                    "flush_granularity": "none", "like_for_like": True,
                    "note": "ArcticDB's docs mark these flags as for throwaway data"}
        return {"sync_primitive": self.sync_primitive(),
                "flush_granularity": "one LMDB transaction per storage key: each data segment, "
                                     "index and version key commits alone",
                "like_for_like": True, "note": "LMDB's default"}

    def data_paths(self):
        return [self.root]

    def read_paths(self):
        return sorted(self.root.glob("*/data.mdb"))

    @staticmethod
    def versions():
        import arcticdb

        return {"arcticdb": arcticdb.__version__, "pandas": pd.__version__,
                "pyarrow": pa.__version__}

    @staticmethod
    def sync_primitive():
        return "F_FULLFSYNC" if platform.system() == "Darwin" else "fdatasync"


def lz4_block_prefix(data, wanted):
    """The first `wanted` bytes of an LZ4 block. Raises ValueError on anything else.

    A plain reader of the published block format, to tell from stored bytes alone whether a
    segment really is LZ4. It is not used on any path that is timed."""
    out = bytearray()
    position = 0
    while len(out) < wanted:
        token = data[position]
        position += 1
        literals = token >> 4
        if literals == 15:
            while True:
                extra = data[position]
                position += 1
                literals += extra
                if extra != 255:
                    break
        out += data[position:position + literals]
        position += literals
        if len(out) >= wanted:
            break
        offset = data[position] | data[position + 1] << 8
        position += 2
        if offset == 0 or offset > len(out):
            raise ValueError(f"bad match offset {offset}")
        length = (token & 15) + 4
        if token & 15 == 15:
            while True:
                extra = data[position]
                position += 1
                length += extra
                if extra != 255:
                    break
        for _ in range(length):
            out.append(out[-offset])
    return bytes(out[:wanted])


def _locate_buckets(counts, rows_per_bucket):
    """A bucket number for every locate: runs of consecutive locates holding about
    `rows_per_bucket` rows between them. A locate busier than that has a bucket to itself."""
    buckets = np.zeros(len(counts), dtype=np.int64)
    bucket = 0
    held = 0
    for locate, count in enumerate(counts):
        if held and held + count > rows_per_bucket:
            bucket += 1
            held = 0
        buckets[locate] = bucket
        held += int(count)
    return buckets


def _frame(table):
    """The Arrow table as the DataFrame lib.write needs, indexed by `ts`."""
    frame = table.drop_columns(["ts"]).to_pandas(split_blocks=True, self_destruct=False)
    frame.index = pd.DatetimeIndex(table.column("ts").to_pandas(), name=INDEX_COLUMN)
    return frame


