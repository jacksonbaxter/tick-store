"""ArcticDB on LMDB. One library per store, one symbol for the whole table.

ArcticDB's settings come from the environment before it loads, so worker_environment sets
them and settings_in_force reads them back through the engine's own config. LMDB syncs each
transaction by default, which is class B. Class A sets MDB_NOSYNC | MDB_NOMETASYNC, which
ArcticDB's docs mark as for throwaway data.
"""

import platform
import re
import struct
import time

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
CHUNK_SEGMENTS = 40  # Segments per chunk of a streamed day.
LMDB_NO_SYNC_FLAGS = 0x10000 | 0x40000  # MDB_NOSYNC | MDB_NOMETASYNC
LMDB_META_MAGIC = 0xBEEFC0DE
LMDB_META_OFFSET = 16
INDEX_COLUMN = "ts"
DEFAULT_ENCODING_VERSION = 0  # EncodingVersion.V1. V2 is marked experimental.
AGGREGATION = {"n": ("shares", "count"), "s": ("shares", "sum"), "lo": ("price", "min"),
               "hi": ("price", "max")}


def _variant(rows, columns):
    return {"rows_per_segment": rows, "columns_per_segment": columns}


class ArcticDBSystem(System):
    name = "arcticdb"
    headline_variant = "rows100k-cols127"
    variants = {
        "rows100k-cols127": _variant(100_000, 127),
        "rows10M-cols127": _variant(10_000_000, 127),
        "rows100k-cols1": _variant(100_000, 1),
    }

    # LMDB's reader table, a mapped scratch file that LMDB rebuilds. It holds no data.
    flush_exempt = (re.compile(r"(?:^|/)lock\.mdb$"),)

    def __init__(self, context):
        super().__init__(context)
        self.library = None
        self.map_size_bytes = FULL_DAY_MAP_SIZE_BYTES if context.full_day else MAP_SIZE_BYTES
        self.map_size_text = FULL_DAY_MAP_SIZE_TEXT if context.full_day else MAP_SIZE_TEXT

    def worker_environment(self):
        flags = 0 if self.class_b else LMDB_NO_SYNC_FLAGS
        return {"ARCTICDB_LMDBStorage_ExtraFlags_int": str(flags),
                "ARCTICDB_VersionStore_NumCPUThreads_int": str(self.context.threads),
                "ARCTICDB_VersionStore_NumIOThreads_int": str(self.context.threads)}

    def requested(self):
        return {"uri": f"lmdb, on a local disk, map_size={self.map_size_text}",
                "symbol_layout": "one symbol for the whole table",
                "write": "one lib.write of a pandas frame",
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

    def ingest(self, columns, table):
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
        return self.tuning["rows_per_segment"] * CHUNK_SEGMENTS

    def ingest_stream(self, chunks):
        """The first chunk is a write, and each one after it an append, so one frame at a time
        exists. Each append makes a version, and none is pruned."""
        started = time.perf_counter()
        python_seconds = 0.0
        appends = 0
        for table in chunks:
            before = time.perf_counter()
            frame = _frame(table)
            python_seconds += time.perf_counter() - before
            if appends == 0 and not self.library.has_symbol(SYMBOL):
                self.library.write(SYMBOL, frame)
            else:
                self.library.append(SYMBOL, frame)
                appends += 1
        finished = time.perf_counter()
        return {"seconds": finished - started, "python_seconds": python_seconds,
                "engine_seconds": finished - started - python_seconds, "appends": appends}

    def probe_flush(self, table):
        self.library.write("flush-probe", _frame(table))

    def _lmdb_directory(self):
        return self.root / LIBRARY

    def dirty_paths(self):
        return [self._lmdb_directory() / "data.mdb"]

    def _read(self, **arguments):
        from arcticdb.options import OutputFormat

        return self.library.read(SYMBOL, output_format=OutputFormat.PYARROW, **arguments).data

    def read_all(self):
        table = self._read(columns=[c for c in TABLE_COLUMNS if c != INDEX_COLUMN])
        return table.sort_by("seq")

    def _range(self, start_ns, end_ns):
        """ArcticDB's date_range includes its end, and the queries' windows do not."""
        midnight = self.context.day.midnight_ns
        return (pd.Timestamp(start_ns + midnight, unit="ns"),
                pd.Timestamp(end_ns + midnight - 1, unit="ns"))

    def query_rows(self, start_ns, end_ns, locate, columns):
        from arcticdb import QueryBuilder

        builder = None
        if locate >= 0:
            builder = QueryBuilder()
            builder = builder[builder["locate"] == locate]
        wanted = [c for c in columns if c != INDEX_COLUMN]
        return self._read(date_range=self._range(start_ns, end_ns), columns=wanted,
                          query_builder=builder)

    def query_minute_aggregate(self, start_ns, end_ns, locate, msg_types):
        from arcticdb import QueryBuilder

        builder = QueryBuilder()
        builder = builder[(builder["locate"] == locate) & builder["msg_type"].isin(list(msg_types))]
        builder = builder.date_range(self._range(start_ns, end_ns))
        builder = builder.resample("1min").agg(AGGREGATION)
        table = self._read(query_builder=builder)
        rows = zip(table.column(INDEX_COLUMN).cast(pa.int64()).to_pylist(),
                   table.column("n").to_pylist(), table.column("s").to_pylist(),
                   table.column("lo").to_pylist(), table.column("hi").to_pylist(), strict=True)
        return [[int(value) for value in row] for row in rows if row[1]]

    def query_column_sum(self, column):
        values = self._read(columns=[column]).column(column)
        return [[len(values), pc.sum(values, min_count=0).as_py() or 0]]

    def scan_facts(self, query):
        """Segments that overlap the window, from the stored index. Bytes are not available."""
        index = self.library._nvs.read_index(SYMBOL)
        midnight = self.context.day.midnight_ns
        if query.kind != "column_sum":
            first = pd.Timestamp(query.start_ns + midnight, unit="ns")
            last = pd.Timestamp(query.end_ns + midnight, unit="ns")
            index = index[(index["end_index"] > first) & (index.index < last)]
        starts = index.drop_duplicates(subset=["start_row", "end_row"])
        return {"rows_decoded": int((starts["end_row"] - starts["start_row"]).sum()),
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

    def _segment_codec(self, tool, keys, index):
        """The first stored segment, decoded here with a plain LZ4 reader. Its first column
        must equal the first timestamps the engine returns, or the codec is not LZ4. The
        segment header has no Python binding in 6.28.2, so this reads the bytes after it."""
        rows = int(index["end_row"].iloc[0] - index["start_row"].iloc[0])
        first = min(keys, key=lambda key: key.start_index)
        stored = tool.read_to_segment(first).bytes
        expected = self._read(row_range=(0, rows), columns=["seq"]).column(INDEX_COLUMN)
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

    def stored_settings(self):
        import arcticdb_ext

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


def _frame(table):
    """The Arrow table as the DataFrame lib.write needs, indexed by `ts`."""
    frame = table.drop_columns(["ts"]).to_pandas(split_blocks=True, self_destruct=False)
    frame.index = pd.DatetimeIndex(table.column("ts").to_pandas(), name=INDEX_COLUMN)
    return frame


