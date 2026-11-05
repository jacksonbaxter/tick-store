"""The one table every system stores, and the column files it travels in.

tools/tick_export writes the files and include/tick_store/store/columns.hpp defines them.
This reads and writes the same form, so Python never parses ITCH. compare/README.md
explains why the table looks as it does.
"""

import re
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

import numpy as np

NANOS_PER_SECOND = 1_000_000_000
MANIFEST_NAME = "manifest.txt"
TIMESTAMP_COLUMN = "ts"

# Name, numpy type and Arrow type name, in file order. `ts_ns` is nanoseconds from
# midnight in the files. Each system stores it as `ts`, a real time on the trading day.
FIXED_COLUMNS = (
    ("seq", "<i8", "int64"),
    ("ts_ns", "<i8", "int64"),
    ("msg_type", "<i2", "int16"),
    ("locate", "<i4", "int32"),
    ("tracking", "<i4", "int32"),
    ("order_ref", "<i8", "int64"),
    ("new_ref", "<i8", "int64"),
    ("side", "i1", "int8"),
    ("shares", "<i4", "int32"),
    ("price", "<i4", "int32"),
    ("match", "<i8", "int64"),
    ("printable", "i1", "int8"),
)
TEXT_COLUMNS = ("stock", "mpid", "rest")
TABLE_COLUMNS = ("seq", "ts", "msg_type", "locate", "tracking", "order_ref", "new_ref", "side",
                 "shares", "price", "match", "printable", "stock", "mpid", "rest")

MANIFEST_PATTERN = re.compile(
    r"tick-columns 1\nrecords (\d+)\ntail_bytes (\d+)\ntext_bytes stock (\d+)\n"
    r"text_bytes mpid (\d+)\ntext_bytes rest (\d+)\n\Z")


class ColumnsError(Exception):
    """A column directory that cannot be used, or a table that cannot become one."""


@dataclass(frozen=True)
class TradingDay:
    """The date records are placed on. ITCH gives nanoseconds from an unstated midnight."""

    year: int
    month: int
    day: int

    @classmethod
    def from_pin(cls, day_label):
        """From a pin label such as 12302019."""
        return cls(int(day_label[4:8]), int(day_label[0:2]), int(day_label[2:4]))

    @property
    def midnight_ns(self):
        moment = datetime(self.year, self.month, self.day, tzinfo=UTC)
        return int(moment.timestamp()) * NANOS_PER_SECOND

    @property
    def iso(self):
        return f"{self.year:04d}-{self.month:02d}-{self.day:02d}"


def read_manifest(directory):
    text = (Path(directory) / MANIFEST_NAME).read_text(encoding="ascii")
    match = MANIFEST_PATTERN.fullmatch(text)
    if match is None:
        raise ColumnsError("the manifest is not in the exact form")
    records, tail, stock, mpid, rest = (int(group) for group in match.groups())
    return {"records": records, "tail_bytes": tail,
            "text_bytes": {"stock": stock, "mpid": mpid, "rest": rest}}


def format_manifest(records, tail_bytes, text_bytes):
    return (f"tick-columns 1\nrecords {records}\ntail_bytes {tail_bytes}\n"
            f"text_bytes stock {text_bytes['stock']}\ntext_bytes mpid {text_bytes['mpid']}\n"
            f"text_bytes rest {text_bytes['rest']}\n")


class ColumnSet:
    """A column directory, mapped read-only. Nothing is copied until a caller asks."""

    def __init__(self, directory):
        self.directory = Path(directory)
        self.manifest = read_manifest(directory)
        self.records = self.manifest["records"]
        self.fixed = {}
        for name, numpy_type, _ in FIXED_COLUMNS:
            self.fixed[name] = self._map(f"{name}.bin", numpy_type, self.records)
        self.offsets = {}
        self.text_bytes = {}
        for name in TEXT_COLUMNS:
            self.offsets[name] = self._map(f"{name}.off", "<u8", self.records + 1)
            self.text_bytes[name] = self._map(f"{name}.dat", "u1",
                                              self.manifest["text_bytes"][name])

    def _map(self, file_name, numpy_type, count):
        path = self.directory / file_name
        size = np.dtype(numpy_type).itemsize * count
        if path.stat().st_size != size:
            raise ColumnsError(f"{file_name}: {path.stat().st_size} bytes, expected {size}")
        if count == 0:
            return np.empty(0, dtype=numpy_type)
        return np.memmap(path, dtype=numpy_type, mode="r")

    def timestamps_ns(self, day):
        return self.fixed["ts_ns"] + day.midnight_ns

    def _read(self, file_name, numpy_type, first, count):
        """`count` items from item `first`, read into memory of their own, not mapped."""
        return np.fromfile(self.directory / file_name, dtype=numpy_type, count=count,
                           offset=first * np.dtype(numpy_type).itemsize)


def arrow_table(columns, day):
    """The whole table as Arrow, over the mapped files for everything but `ts`."""
    import pyarrow as pa

    arrays = {}
    for name, _, arrow_type in FIXED_COLUMNS:
        if name == "ts_ns":
            continue
        arrays[name] = pa.array(columns.fixed[name], type=getattr(pa, arrow_type)())
    arrays["ts"] = pa.array(columns.timestamps_ns(day).astype("datetime64[ns]"),
                            type=pa.timestamp("ns"))
    for name in TEXT_COLUMNS:
        arrays[name] = pa.Array.from_buffers(
            pa.large_string(), columns.records,
            [None, pa.py_buffer(columns.offsets[name]), pa.py_buffer(columns.text_bytes[name])])
    return pa.table({name: arrays[name] for name in TABLE_COLUMNS})


def table_chunks(columns, day, chunk_rows):
    """The same table as `arrow_table`, in order, `chunk_rows` rows at a time.

    Each chunk is read into memory of its own, so a chunk freed is gone and no mapping of a
    day's files stays resident. This is the only way to load a whole day: `arrow_table` makes
    a day-sized `ts`, and the systems' whole-table paths build more on top of it."""
    import pyarrow as pa

    for first in range(0, columns.records, chunk_rows):
        count = min(chunk_rows, columns.records - first)
        arrays = {}
        for name, numpy_type, arrow_type in FIXED_COLUMNS:
            values = columns._read(f"{name}.bin", numpy_type, first, count)
            if name == "ts_ns":
                arrays["ts"] = pa.array((values + day.midnight_ns).astype("datetime64[ns]"),
                                        type=pa.timestamp("ns"))
            else:
                arrays[name] = pa.array(values, type=getattr(pa, arrow_type)())
        for name in TEXT_COLUMNS:
            offsets = columns._read(f"{name}.off", "<u8", first, count + 1)
            start = int(offsets[0])
            data = columns._read(f"{name}.dat", "u1", start, int(offsets[-1]) - start)
            arrays[name] = pa.Array.from_buffers(
                pa.large_string(), count,
                [None, pa.py_buffer((offsets - start).astype("<i8")), pa.py_buffer(data)])
        yield pa.table({name: arrays[name] for name in TABLE_COLUMNS})


def plain_array(column, name):
    """One column of a returned table as a single array, undictionaried, with no nulls."""
    import pyarrow as pa

    array = column.combine_chunks() if column.num_chunks else pa.array([], column.type)
    if isinstance(array, pa.ChunkedArray):
        array = pa.concat_arrays(array.chunks)
    if pa.types.is_dictionary(array.type):
        array = array.dictionary_decode()
    if array.null_count != 0:
        raise ColumnsError(f"{name}: {array.null_count} nulls came back, and none went in")
    return array


def write_columns(table, directory, day):
    """Writes a table a system returned as a column directory, and fails on any value that
    does not fit its column. The rows must already be in `seq` order: the reader checks."""
    with ColumnWriter(directory, day) as writer:
        writer.append(table)


class ColumnWriter:
    """A column directory written one table at a time, in order, so that a whole day read back
    never has to be one table. Every value must fit its column, as in `write_columns`, and the
    manifest is written only by `finish`, so a directory left by a failure is never whole."""

    def __init__(self, directory, day):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.midnight = day.midnight_ns
        self.records = 0
        self.text_bytes = dict.fromkeys(TEXT_COLUMNS, 0)
        self.files = {}
        for name, _, _ in FIXED_COLUMNS:
            self.files[f"{name}.bin"] = open(self.directory / f"{name}.bin", "wb")
        for name in TEXT_COLUMNS:
            self.files[f"{name}.off"] = open(self.directory / f"{name}.off", "wb")
            self.files[f"{name}.dat"] = open(self.directory / f"{name}.dat", "wb")
        for name in TEXT_COLUMNS:
            np.zeros(1, dtype="<u8").tofile(self.files[f"{name}.off"])

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if kind is None:
            self.finish()
        else:
            self.close()

    def close(self):
        for handle in self.files.values():
            handle.close()

    def append(self, table):
        import pyarrow as pa

        missing = set(TABLE_COLUMNS) - set(table.column_names)
        if missing:
            raise ColumnsError(f"columns missing from the table: {sorted(missing)}")
        records = table.num_rows
        for name, numpy_type, _ in FIXED_COLUMNS:
            source = "ts" if name == "ts_ns" else name
            array = plain_array(table.column(source), source)
            if pa.types.is_timestamp(array.type):
                if array.type.unit != "ns":
                    raise ColumnsError(f"{source}: {array.type.unit} precision came back")
                values = array.cast(pa.int64()).to_numpy(zero_copy_only=False) - self.midnight
            else:
                values = array.to_numpy(zero_copy_only=False)
            narrow = values.astype(numpy_type)
            if not np.array_equal(narrow.astype(np.int64), values.astype(np.int64)):
                raise ColumnsError(f"{name}: a value does not fit {numpy_type}")
            narrow.tofile(self.files[f"{name}.bin"])

        for name in TEXT_COLUMNS:
            array = plain_array(table.column(name), name).cast(pa.large_string())
            _, offsets_buffer, data_buffer = array.buffers()
            offsets = np.frombuffer(offsets_buffer, dtype="<i8", count=records + 1,
                                    offset=array.offset * 8)
            first = int(offsets[0])
            base = self.text_bytes[name]
            (offsets[1:] - first + base).astype("<u8").tofile(self.files[f"{name}.off"])
            end = int(offsets[-1])
            if data_buffer is not None:
                self.files[f"{name}.dat"].write(memoryview(data_buffer)[first:end])
            self.text_bytes[name] = base + end - first
        self.records += records

    def finish(self):
        """Closes every file, then writes the tail and the manifest that make it whole."""
        self.close()
        (self.directory / "tail.bin").write_bytes(b"")
        (self.directory / MANIFEST_NAME).write_text(
            format_manifest(self.records, 0, self.text_bytes), encoding="ascii")


def read_windows(columns, rows_per_window):
    """Half-open time windows over the whole day, each holding about `rows_per_window` rows,
    in order. Rows of one nanosecond never straddle two windows, since a window is a range of
    times, and the files' times never decrease in file order, so window after window, each sorted
    by seq, is the table in file order. A system reads a whole day back one window at a time."""
    ts = columns.fixed["ts_ns"]
    bounds = [0]
    for first in range(rows_per_window, columns.records, rows_per_window):
        value = int(ts[first])
        if value > bounds[-1]:
            bounds.append(value)
    last = int(ts[columns.records - 1]) + 1 if columns.records else 1
    bounds.append(max(last, bounds[-1] + 1))
    return list(zip(bounds[:-1], bounds[1:], strict=True))
