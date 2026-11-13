"""Parquet through pyarrow, one file per run.

Each variant's settings are in research/evidence/column-systems.md section 7. pyarrow never
fsyncs, so class B here is the harness flushing the closed file and its directory, and that
is recorded in every result.
"""

import time

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from compare.durability import harness_sync_primitive, sync_path
from compare.schema import TABLE_COLUMNS
from compare.settings import NOT_PROVEN, PROVEN, Setting, require_all_match
from compare.systems.base import System

FILE_NAME = "table.parquet"
FLUSHES_PER_FILE = 2  # The file, then its directory.
ROW_GROUP_128K = 1 << 17
ROW_GROUP_1MI = 1 << 20
CHUNK_ROW_GROUPS = 4  # Row groups per chunk of a streamed day.
WHOLE_SLICE = None  # One row group, near the 512 MB in the sweep at this slice's size.
PARQUET_VERSION = "2.6"
DATA_PAGE_VERSION = "1.0"
DICTIONARY_COLUMNS = ("msg_type", "locate", "tracking", "side", "shares", "price", "printable",
                      "stock", "mpid")
DELTA_COLUMNS = ("seq", "ts", "order_ref", "new_ref", "match")
PAGE_HEADER_TYPE_FIELD = 0x15
PAGE_TYPE_TOKENS = {0x00: "1.0", 0x06: "2.0"}


# The footer's name for each codec the writer is asked for.
FOOTER_CODEC = {"zstd": "ZSTD", "snappy": "SNAPPY", "none": "UNCOMPRESSED"}


def _variant(codec, level, row_group, price_delta=False, ts_default=False):
    return {"compression": codec, "compression_level": level, "row_group_rows": row_group,
            "price_shares_delta": price_delta,
            "ts_encoding": ("the writer's default: dictionary, falling back to PLAIN past the "
                            "1 MB dictionary page limit" if ts_default
                            else "DELTA_BINARY_PACKED")}


class ParquetSystem(System):
    name = "parquet"
    headline_variant = "zstd1-rg1Mi"
    variants = {
        "zstd1-rg1Mi": _variant("zstd", 1, ROW_GROUP_1MI),
        "zstd1-rg128k": _variant("zstd", 1, ROW_GROUP_128K),
        "zstd1-rgall": _variant("zstd", 1, WHOLE_SLICE),
        "snappy-rg1Mi": _variant("snappy", None, ROW_GROUP_1MI),
        "zstd9-rg1Mi": _variant("zstd", 9, ROW_GROUP_1MI),
        "zstd1-rg1Mi-delta-price": _variant("zstd", 1, ROW_GROUP_1MI, price_delta=True),
        # Section 7 of research/evidence/column-systems.md sweeps "none" among the codecs, and
        # runs `ts` once on the writer's default against DELTA_BINARY_PACKED. See
        # compare/README.md for the documentation behind both.
        "none-rg1Mi": _variant("none", None, ROW_GROUP_1MI),
        "zstd1-rg1Mi-ts-default": _variant("zstd", 1, ROW_GROUP_1MI, ts_default=True),
    }

    @property
    def path(self):
        return self.root / FILE_NAME

    @property
    def ts_default(self):
        return self.tuning["ts_encoding"] != "DELTA_BINARY_PACKED"

    def _dictionary_columns(self):
        columns = DICTIONARY_COLUMNS
        if self.tuning["price_shares_delta"]:
            columns = tuple(c for c in columns if c not in ("shares", "price"))
        if self.ts_default:
            # pyarrow's use_dictionary defaults to True for every column.
            columns = ("ts", *columns)
        return columns

    def _delta_columns(self):
        columns = DELTA_COLUMNS
        if self.tuning["price_shares_delta"]:
            columns = (*columns, "shares", "price")
        if self.ts_default:
            columns = tuple(c for c in columns if c != "ts")
        return columns

    def _writer(self, path, schema):
        return pq.ParquetWriter(
            path, schema, version=PARQUET_VERSION, data_page_version=DATA_PAGE_VERSION,
            compression=self.tuning["compression"],
            compression_level=self.tuning["compression_level"],
            use_dictionary=list(self._dictionary_columns()),
            column_encoding=dict.fromkeys(self._delta_columns(), "DELTA_BINARY_PACKED"),
            write_statistics=True, write_page_index=False, bloom_filter_options=None,
            sorting_columns=[pq.SortingColumn(TABLE_COLUMNS.index("ts"))])

    def __init__(self, context):
        super().__init__(context)
        self.harness_flushes = 0

    def requested(self):
        return {"writer": "streaming pq.ParquetWriter", "version": PARQUET_VERSION,
                "data_page_version": DATA_PAGE_VERSION, "data_page_bytes": "1 MiB, the default",
                "compression": self.tuning["compression"],
                "compression_level": self.tuning["compression_level"],
                "row_group_rows": self.tuning["row_group_rows"] or "the whole slice",
                "dictionary": list(self._dictionary_columns()),
                "delta_binary_packed": list(self._delta_columns()),
                "plain_columns": ["rest"], "statistics": True, "page_index": False,
                "bloom_filters": False, "sorting_columns": ["ts"],
                "reader": {"call": "pq.read_table", "use_threads": True, "pre_buffer": True,
                           "memory_map": False},
                "threads": self.context.threads}

    # open() holds no file: every pq.read_table opens the file and reads and parses its footer
    # inside the timed call, so the whole open is inside every clock, cold and warm.
    open_inside_cold_clock = True

    def open(self):
        # pq.read_table and Table.group_by import these on their first call, which would put the
        # imports on the clock.
        import pyarrow.acero  # noqa: F401
        import pyarrow.dataset  # noqa: F401

        pa.set_cpu_count(self.context.threads)
        pa.set_io_thread_count(self.context.threads)

    def settings_in_force(self):
        return require_all_match([
            Setting("arrow cpu threads", self.context.threads, pa.cpu_count(), "pa.cpu_count()"),
            Setting("arrow io threads", self.context.threads, pa.io_thread_count(),
                    "pa.io_thread_count()"),
        ])

    def _flush(self, path):
        """Class B only. The primitive is recorded with the result."""
        if self.class_b:
            sync_path(path)
            sync_path(path.parent)
            self.harness_flushes += 2

    def ingest(self, columns, table):
        row_group = self.tuning["row_group_rows"] or table.num_rows
        started = time.perf_counter()
        with self._writer(self.path, table.schema) as writer:
            writer.write_table(table, row_group_size=row_group)
        written = time.perf_counter()
        self._flush(self.path)
        finished = time.perf_counter()
        return {"seconds": finished - started, "engine_seconds": written - started,
                "flush_seconds": finished - written, "python_seconds": 0.0,
                "python_note": "the Arrow table is a zero-copy view of the mapped columns, and "
                               "the engine call is the whole write"}

    @classmethod
    def write_times(cls, full_day):
        if full_day:
            seconds = ("reading each chunk of the day's export into an Arrow table, "
                       "pq.ParquetWriter writing each chunk, and under class B the harness's "
                       "flush of the file and then its directory")
            reading = "the reading of the chunks"
        else:
            seconds = ("pq.ParquetWriter writing the zero-copy Arrow view of the mapped "
                       "export, and under class B the harness's flush of the file and then its "
                       "directory")
            reading = "0 on the slice"
        return {"seconds": seconds,
                "engine_only_seconds": f"the total less python_seconds, {reading}: the writer "
                                       "and, under class B, the flush"}

    def stream_chunk_rows(self):
        return self.tuning["row_group_rows"] * CHUNK_ROW_GROUPS

    def ingest_stream(self, chunks):
        """The clock holds reading each chunk from the column files, which a day cannot keep
        warm, as well as the writer: `python_seconds` is that reading."""
        if self.tuning["row_group_rows"] is None:
            raise NotImplementedError("one row group of a whole day cannot be streamed")
        chunks = iter(chunks)
        started = time.perf_counter()
        reading = 0.0
        before = time.perf_counter()
        first = next(chunks)
        reading += time.perf_counter() - before
        with self._writer(self.path, first.schema) as writer:
            table = first
            while table is not None:
                writer.write_table(table, row_group_size=self.tuning["row_group_rows"])
                before = time.perf_counter()
                table = next(chunks, None)
                reading += time.perf_counter() - before
        written = time.perf_counter()
        self._flush(self.path)
        finished = time.perf_counter()
        return {"seconds": finished - started, "engine_seconds": written - started - reading,
                "flush_seconds": finished - written, "python_seconds": reading,
                "python_note": "python_seconds reads each chunk of the column files into an "
                               "Arrow table, inside the clock, since a day's 24 GB of columns "
                               "cannot stay warm. engine_seconds is the writer"}

    def probe_flush(self, table):
        path = self.root / "probe.parquet"
        with self._writer(path, table.schema) as writer:
            writer.write_table(table)
        self._flush(path)

    def dirty_paths(self):
        return [self.root / "probe.parquet"]

    def _read(self, columns=None, filters=None):
        return pq.read_table(self.path, columns=columns, filters=filters, use_threads=True,
                             pre_buffer=True, memory_map=False)

    def read_all(self):
        return self._read(list(TABLE_COLUMNS))

    def read_window(self, start_ns, end_ns):
        return self._read(list(TABLE_COLUMNS), self._filter(start_ns, end_ns, -1)).sort_by("seq")

    def _filter(self, start_ns, end_ns, locate):
        ts_type = pa.timestamp("ns")
        midnight = self.context.day.midnight_ns
        expression = (pc.field("ts") >= pa.scalar(start_ns + midnight, ts_type)) & (
            pc.field("ts") < pa.scalar(end_ns + midnight, ts_type))
        if locate >= 0:
            expression &= pc.field("locate") == pa.scalar(locate, pa.int32())
        return expression

    def query_rows(self, start_ns, end_ns, locate, columns):
        return self._read(list(columns), self._filter(start_ns, end_ns, locate))

    def query_minute_aggregate(self, start_ns, end_ns, locate, msg_types):
        expression = self._filter(start_ns, end_ns, locate) & pc.field("msg_type").isin(
            pa.array(msg_types, pa.int16()))
        table = self._read(["ts", "shares", "price"], expression)
        minutes = pc.floor_temporal(table.column("ts"), unit="minute")
        grouped = table.append_column("minute", minutes).group_by("minute").aggregate(
            [("shares", "count"), ("shares", "sum"), ("price", "min"), ("price", "max")])
        grouped = grouped.sort_by("minute")
        rows = zip(grouped.column("minute").cast(pa.int64()).to_pylist(),
                   grouped.column("shares_count").to_pylist(),
                   grouped.column("shares_sum").to_pylist(),
                   grouped.column("price_min").to_pylist(),
                   grouped.column("price_max").to_pylist(), strict=True)
        return [list(row) for row in rows]

    def query_column_sum(self, column):
        values = self._read([column]).column(column)
        return [[len(values), pc.sum(values, min_count=0).as_py() or 0]]

    def scan_facts(self, query):
        """Row groups that can overlap the window, from the footer, for the columns read."""
        from compare.queries import ROW_COLUMNS

        metadata = pq.ParquetFile(self.path).metadata
        wanted = {"rows": ROW_COLUMNS, "minute_aggregate": ("ts", "shares", "price", "msg_type",
                                                            "locate"),
                  "column_sum": ("shares",)}[query.kind]
        names = [metadata.schema.column(i).path for i in range(metadata.num_columns)]
        ts_column = names.index("ts")
        midnight = self.context.day.midnight_ns
        rows = 0
        decoded = 0
        for index in range(metadata.num_row_groups):
            group = metadata.row_group(index)
            stats = group.column(ts_column).statistics
            if query.kind != "column_sum":
                low = _nanos(stats.min) - midnight
                high = _nanos(stats.max) - midnight
                if high < query.start_ns or low >= query.end_ns:
                    continue
            rows += group.num_rows
            decoded += sum(group.column(i).total_uncompressed_size
                           for i, name in enumerate(names) if name in wanted)
        return {"rows_decoded": rows, "bytes_decoded": decoded,
                "method": "footer: rows and uncompressed bytes of the row groups whose ts "
                          "statistics overlap the window, for the columns read"}

    def stored_settings(self):
        parquet = pq.ParquetFile(self.path)
        metadata = parquet.metadata
        expected_groups = self.tuning["row_group_rows"] or metadata.num_rows
        codec = FOOTER_CODEC[self.tuning["compression"]]
        checks = [Setting("format version", PARQUET_VERSION, metadata.format_version, "footer")]
        rows = [metadata.row_group(i).num_rows for i in range(metadata.num_row_groups)]
        checks.append(Setting("row-group rows", True,
                              all(count <= expected_groups for count in rows[:-1]) and
                              all(count == expected_groups for count in rows[:-1]) and
                              rows[-1] <= expected_groups, "footer, every row group"))
        columns = []
        with open(self.path, "rb") as stored:
            for group_index in range(metadata.num_row_groups):
                group = metadata.row_group(group_index)
                for index in range(group.num_columns):
                    chunk = group.column(index)
                    name = chunk.path_in_schema
                    version = _data_page_version(stored, chunk.data_page_offset)
                    checks.append(Setting(f"group {group_index} {name} compression", codec,
                                          chunk.compression, "footer"))
                    checks.append(Setting(f"group {group_index} {name} data page version",
                                          DATA_PAGE_VERSION, version,
                                          "first page header in the file"))
                    checks.append(Setting(f"group {group_index} {name} dictionary",
                                          name in self._dictionary_columns(),
                                          chunk.has_dictionary_page, "footer"))
                    checks.append(Setting(f"group {group_index} {name} statistics", True,
                                          chunk.is_stats_set, "footer"))
                    checks.append(Setting(f"group {group_index} {name} page index", False,
                                          chunk.has_column_index, "footer"))
                    if name in self._delta_columns():
                        checks.append(Setting(f"group {group_index} {name} delta encoded", True,
                                              "DELTA_BINARY_PACKED" in chunk.encodings, "footer"))
                    if group_index == 0:
                        columns.append({"column": name, "compression": chunk.compression,
                                        "encodings": sorted(chunk.encodings),
                                        "dictionary": chunk.has_dictionary_page,
                                        "data_page_version": version,
                                        "compressed_bytes": chunk.total_compressed_size,
                                        "uncompressed_bytes": chunk.total_uncompressed_size})
        sorting = metadata.row_group(0).sorting_columns
        checks.append(Setting("sorted by ts", [TABLE_COLUMNS.index("ts")],
                              [item.column_index for item in sorting], "footer"))
        checked = require_all_match(checks)
        return {"checks_passed": len(checked), "created_by": metadata.created_by,
                "format_version": metadata.format_version, "row_groups": len(rows),
                "row_group_rows": sorted(set(rows)), "first_group_columns": columns,
                "compression_level": {
                    "requested": self.tuning["compression_level"],
                    "in_force": "not recorded: the Parquet footer has no level"},
                "harness_flush": ({"primitive": harness_sync_primitive(),
                                   "of": "the closed file, then its directory"}
                                  if self.class_b else None)}

    def flush_facts(self):
        return {"harness_flushes": self.harness_flushes}

    def durability_verdict(self, flush, control_saw_dirty):
        """The harness makes the flush here, so the proof is that it was made or not made.

        Dirty pages are recorded but are no proof. macOS starts writing a file out when it
        closes, so a closed Parquet file reads 0 dirty with or without a flush."""
        calls = flush.get("harness_flushes")
        if self.class_b:
            if calls == FLUSHES_PER_FILE and flush.get("data_dirty_pages") == 0:
                return {"status": PROVEN,
                        "reason": f"the harness called {harness_sync_primitive()} on the file "
                                  "and its directory, both succeeded, and 0 pages stayed dirty"}
            return {"status": NOT_PROVEN, "reason": f"{calls} flushes made, {FLUSHES_PER_FILE} "
                                                    "needed"}
        if calls == 0:
            return {"status": PROVEN, "reason": "the harness made no flush, and pyarrow has none"}
        return {"status": NOT_PROVEN, "reason": f"{calls} flushes made in class A"}

    def durability_facts(self):
        if not self.class_b:
            return {"sync_primitive": "none: pyarrow never syncs", "flush_granularity": "none",
                    "like_for_like": True, "note": "the page cache alone"}
        return {"sync_primitive": harness_sync_primitive(),
                "flush_granularity": "one flush per file, after it closes",
                "like_for_like": True,
                "note": "pyarrow never syncs, so the harness flushes the file and its directory "
                        "and that flush is timed in the write"}

    def data_paths(self):
        return [self.path]

    @staticmethod
    def versions():
        return {"pyarrow": pa.__version__}


def _nanos(value):
    """A statistics value as nanoseconds: pyarrow gives pandas or numpy-backed datetimes."""
    if hasattr(value, "value"):
        return int(value.value)
    return int(pa.scalar(value, pa.timestamp("ns")).cast(pa.int64()).as_py())


def _data_page_version(stored, offset):
    """Which data page form a column chunk starts with, from its thrift page header."""
    stored.seek(offset)
    header = stored.read(2)
    if header[0] != PAGE_HEADER_TYPE_FIELD or header[1] not in PAGE_TYPE_TOKENS:
        return f"unknown page header {header.hex()}"
    return PAGE_TYPE_TOKENS[header[1]]


