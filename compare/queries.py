"""Q1 to Q5 from BENCHMARKS.md, one parameter set for every system, and their ground truth.

The ground truth comes from the exported columns with numpy, and shares no code with any
system. A system's answer must equal it, in row count and in a hash of the rows or of the
aggregate, or the run fails. That is the proof behind "same queries".

The slice is five minutes long, so hour-long windows are clipped to it and Q5 scans the
slice, not a day. Every result file says so. Nothing here supports a claim about a whole day.
"""

import hashlib
import json
from dataclasses import asdict, dataclass

import numpy as np

from compare.schema import NANOS_PER_SECOND, plain_array

NANOS_PER_MINUTE = 60 * NANOS_PER_SECOND
# Columns a row query returns. A real reader of the order flow wants these.
ROW_COLUMNS = ("seq", "ts", "msg_type", "locate", "order_ref", "side", "shares", "price", "stock")
ADD_ORDER_TYPES = (ord("A"), ord("F"))
OPEN_NS = (9 * 3600 + 30 * 60) * NANOS_PER_SECOND
HOUR_NS = 3600 * NANOS_PER_SECOND


class QueryMismatch(Exception):
    """A system's answer differs from the ground truth."""


@dataclass(frozen=True)
class Query:
    """One query, in nanoseconds from midnight. `kind` chooses the system's method."""

    name: str
    kind: str
    start_ns: int
    end_ns: int
    locate: int = -1
    text: str = ""


def busiest_locate(columns, start_ns, end_ns):
    """The locate with most rows in the window, the lowest on a tie. 0 is not a symbol."""
    ts = columns.fixed["ts_ns"]
    first, last = np.searchsorted(ts, [start_ns, end_ns])
    locates = np.asarray(columns.fixed["locate"][first:last])
    counts = np.bincount(locates)
    counts[0] = 0
    return int(np.argmax(counts))


def build_queries(columns, window_end_ns):
    """The one parameter set. `window_end_ns` is the slice's window end, from the pin."""
    clipped_end = min(OPEN_NS + HOUR_NS, window_end_ns)
    locate = busiest_locate(columns, OPEN_NS, clipped_end)
    return (
        Query("Q1", "rows", OPEN_NS, OPEN_NS + NANOS_PER_SECOND,
              text="One second of all symbols at the open, [09:30:00, 09:30:01)."),
        Query("Q2", "rows", OPEN_NS, clipped_end, locate,
              "One hour of one symbol, clipped to the slice: every record of the busiest locate "
              "in [09:30:00, 09:34:30). The one-hour window would run to 10:30:00."),
        Query("Q3", "minute_aggregate", OPEN_NS, clipped_end, locate,
              "Per-minute aggregate of one symbol over one hour, clipped to the slice: for the "
              "add orders of that locate, per minute, count, sum of shares, least and greatest "
              "price, over [09:30:00, 09:34:30)."),
        Query("Q4", "rows", OPEN_NS, OPEN_NS + 1_000_000,
              text="One millisecond of all symbols, [09:30:00.000, 09:30:00.001)."),
        Query("Q5", "column_sum", 0, 0,
              text="Full scan of one column, the sum of shares, clipped to the slice: all "
                   "6,125,294 rows and not a day's 268,744,780."),
    )


def query_to_json(query):
    return asdict(query)


def query_from_json(item):
    return Query(**item)


CLIPPING_STATEMENT = (
    "The slice holds every S and R of 12302019 plus the five minutes [09:29:30, 09:34:30). Q2 "
    "and Q3 are defined over one hour and are clipped to [09:30:00, 09:34:30). Q5 is defined "
    "over a full day and is clipped to the slice. No claim about a full-day query is made from "
    "this file.")


def _hash_rows(rows):
    """A digest of rows, given as (column name, int64 array or list of str) pairs in order."""
    digest = hashlib.sha256()
    for name, values in rows:
        digest.update(name.encode("ascii"))
        if isinstance(values, np.ndarray):
            digest.update(values.astype("<i8").tobytes())
        else:
            for value in values:
                digest.update(value.encode("ascii"))
                digest.update(b"\0")
    return digest.hexdigest()


def rows_digest(table):
    """Row count and digest of a returned Arrow table, sorted by `seq` first, so that a system
    may give rows of the same nanosecond in any order."""
    import pyarrow as pa
    import pyarrow.compute as pc

    table = table.select(list(ROW_COLUMNS))
    table = table.take(pc.sort_indices(table.column("seq")))
    rows = []
    for name in ROW_COLUMNS:
        column = plain_array(table.column(name), name)
        if name == "stock":
            rows.append((name, column.to_pylist()))
        elif pa.types.is_timestamp(column.type):
            rows.append((name, column.cast(pa.int64()).to_numpy(zero_copy_only=False)))
        else:
            rows.append((name, column.to_numpy(zero_copy_only=False).astype(np.int64)))
    return {"rows": table.num_rows, "sha256": _hash_rows(rows)}


def aggregate_digest(rows):
    """Row count and digest of canonical aggregate rows, a list of lists of ints."""
    text = json.dumps(rows, separators=(",", ":"))
    return {"rows": len(rows), "sha256": hashlib.sha256(text.encode("ascii")).hexdigest()}


def _window_rows(columns, query):
    ts = columns.fixed["ts_ns"]
    first, last = (int(i) for i in np.searchsorted(ts, [query.start_ns, query.end_ns]))
    if query.locate < 0:
        return np.arange(first, last)
    mask = np.asarray(columns.fixed["locate"][first:last]) == query.locate
    return first + np.flatnonzero(mask)


def _truth_rows(columns, query, day):
    picked = _window_rows(columns, query)
    pairs = []
    for name in ROW_COLUMNS:
        if name == "ts":
            values = np.asarray(columns.fixed["ts_ns"][picked]) + day.midnight_ns
            pairs.append(("ts", values.astype(np.int64)))
        elif name == "stock":
            offsets = columns.offsets["stock"]
            data = columns.text_bytes["stock"]
            pairs.append(("stock", [bytes(data[int(offsets[i]):int(offsets[i + 1])]).decode("ascii")
                                    for i in picked]))
        else:
            pairs.append((name, np.asarray(columns.fixed[name][picked]).astype(np.int64)))
    return {"rows": len(picked), "sha256": _hash_rows(pairs)}


def _truth_minutes(columns, query, day):
    picked = _window_rows(columns, query)
    types = np.asarray(columns.fixed["msg_type"][picked])
    picked = picked[np.isin(types, ADD_ORDER_TYPES)]
    ts = np.asarray(columns.fixed["ts_ns"][picked])
    shares = np.asarray(columns.fixed["shares"][picked]).astype(np.int64)
    price = np.asarray(columns.fixed["price"][picked]).astype(np.int64)
    minutes = ts // NANOS_PER_MINUTE
    rows = []
    for minute in np.unique(minutes):
        here = minutes == minute
        rows.append([int(minute) * NANOS_PER_MINUTE + day.midnight_ns, int(here.sum()),
                     int(shares[here].sum()), int(price[here].min()), int(price[here].max())])
    return rows


def ground_truth(columns, query, day):
    """Computed from the exported columns alone."""
    if query.kind == "rows":
        return _truth_rows(columns, query, day)
    if query.kind == "minute_aggregate":
        return aggregate_digest(_truth_minutes(columns, query, day))
    if query.kind == "column_sum":
        # Summed in place: a copy to int64 would be 2 GB on a full day.
        return aggregate_digest([[columns.records,
                                  int(columns.fixed["shares"].sum(dtype=np.int64))]])
    raise ValueError(query.kind)


def run_query(system, query):
    """Runs one query on a system. The result is whatever the system returned, not yet in any
    common form, so a caller can time this call alone."""
    if query.kind == "rows":
        return system.query_rows(query.start_ns, query.end_ns, query.locate, ROW_COLUMNS)
    if query.kind == "minute_aggregate":
        return system.query_minute_aggregate(query.start_ns, query.end_ns, query.locate,
                                             ADD_ORDER_TYPES)
    if query.kind == "column_sum":
        return system.query_column_sum("shares")
    raise ValueError(query.kind)


def digest_result(query, result):
    """The common form of a system's answer. Each system returns rows as an Arrow table, and
    aggregates as a list of lists of ints."""
    return rows_digest(result) if query.kind == "rows" else aggregate_digest(result)


def require_truth(query, digest, truth, system_name):
    if digest != truth:
        raise QueryMismatch(f"{system_name} {query.name}: {digest} against ground truth {truth}")
