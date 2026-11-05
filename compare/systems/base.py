"""The small interface every system implements.

Life cycle, in the order compare/run_system.py drives it. `start_service` and `stop_service`
run in the long-lived runner and do nothing for a library. Everything else runs in a worker
process of its own, so that a cold query opens the engine from nothing and each step's peak
memory is that step's alone.

    prepare()            fresh, empty store at `root`, for this variant and class
    start_service()      server up. Fails if its ports are not free
    open()               the client, in the worker
    settings_in_force()  everything requested against what the engine says. Raises
    ingest(columns, table)  everything, durable under the class. Returns timings
    settings_after_ingest()  what only the stored form can show. Raises
    probe_flush(table)   a small commit. dirty_paths() are read straight after it returns
    read_all()           the whole table as Arrow, ordered by seq
    query_*()            the three query shapes in compare/queries.py
    stored_settings()    settings read from the stored form
    ingest_stream(chunks)  a whole day, chunk by chunk
    ingest_day(columns)  a whole day, durable under the class, for a full-day run. Streams
    read_back_to(destination, windows)  a whole day back as column files, one window at a time
    close()
    stop_service()       server down, port free, peak memory returned
    data_paths()         every file of the store, for size and residency
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path

from compare.durability import CLASS_B
from compare.settings import durability_verdict


@dataclass(frozen=True)
class RunContext:
    layout: object
    root: Path
    variant: str
    durability: str
    day: object
    threads: int = 2
    # A whole day, for compare/fullday.py's capacity check or a full-day comparison run.
    full_day: bool = False
    # QuestDB through bin/questdb.sh, for compare/fullday.py alone: a reported run starts the
    # server from java directly, on either input, so that its runtime is read the same way.
    launcher: bool = False
    heap: str = "1g"
    gc_log: Path | None = None
    # The ITCH source itself, for a system whose natural input it is: tick-store's.
    source: Path | None = None


class System(ABC):
    """One system under one variant and one durability class."""

    name = ""
    variants = {}
    headline_variant = ""
    # Files a commit leaves dirty by design, as compiled patterns on their relative paths.
    # A result lists them by name. Everything else must be clean under class B.
    flush_exempt = ()

    def __init__(self, context):
        self.context = context
        self.root = Path(context.root)
        self.tuning = self.variants[context.variant]

    @property
    def class_b(self):
        return self.context.durability == CLASS_B

    # Runner side. A library has no service.

    def prepare(self):
        self.root.mkdir(parents=True, exist_ok=False)

    def worker_environment(self):
        """Extra environment for a worker. Read by the engine when it loads."""
        return {}

    def start_service(self):
        """Starts the server, if there is one."""

    def stop_service(self):
        """Stops the server, and returns {"peak_rss_bytes": ...} if there was one."""
        return {}

    def service_settings(self):
        """Settings the runner can read with no client: QuestDB's commit mode, say."""
        return []

    def settle(self):
        """After ingest, with the service up: finish anything left in the background."""
        return {}

    def service_facts(self):
        """Facts about the system the results should state."""
        return {}

    @abstractmethod
    def requested(self):
        """Every setting this run asks for, the defaults it relies on included."""

    # Worker side.

    @abstractmethod
    def open(self):
        """Opens the client."""

    def close(self):
        """Closes the client."""

    @abstractmethod
    def settings_in_force(self):
        """Requested against in force, as Setting.as_json dicts. Raises SettingsMismatch."""

    def settings_after_ingest(self):
        """Settings only the stored form can show, read once the write has returned and outside
        its clock. Raises SettingsMismatch."""
        return []

    @abstractmethod
    def stored_settings(self):
        """What the stored form says, after ingest. Raises SettingsMismatch."""

    @abstractmethod
    def ingest(self, columns, table):
        """Stores every row, durably for the class, and returns when it is readable."""

    def stream_chunk_rows(self):
        """Rows per chunk for ingest_stream: a multiple of the stored unit, so a chunk's end
        leaves no short row group or segment for the checks in stored_settings to refuse."""
        raise NotImplementedError

    def ingest_stream(self, chunks):
        """As ingest, from an iterator of Arrow tables, holding one chunk at a time."""
        raise NotImplementedError

    def ingest_day(self, columns):
        """A whole day, as `ingest`, streamed in chunks of the stored unit so that no day-sized
        table is ever built. A system whose natural input is not the columns overrides it."""
        from compare.schema import table_chunks

        return self.ingest_stream(table_chunks(columns, self.context.day,
                                               self.stream_chunk_rows()))

    def read_window(self, start_ns, end_ns):
        """Every row in [start, end), with the table's columns, ordered by seq."""
        raise NotImplementedError

    def read_back_to(self, destination, windows):
        """The whole table as column files, one window at a time, each window ordered by seq.
        Returns the rows written and the seconds spent reading from the engine."""
        import time

        from compare.schema import ColumnWriter

        reading = 0.0
        with ColumnWriter(destination, self.context.day) as writer:
            for start_ns, end_ns in windows:
                started = time.perf_counter()
                table = self.read_window(start_ns, end_ns)
                reading += time.perf_counter() - started
                writer.append(table)
            rows = writer.records
        return {"rows": rows, "read_seconds": reading, "windows": len(windows)}

    @abstractmethod
    def probe_flush(self, table):
        """Commits a few rows into a new table in this store, and returns once committed."""

    def flush_facts(self):
        """Anything else a worker knows about the flushes it made."""
        return {}

    def dirty_paths(self):
        """The files a commit should have made clean under class B."""
        return self.data_paths()

    @abstractmethod
    def read_all(self):
        """The whole table as Arrow with the table's columns, ordered by seq."""

    @abstractmethod
    def query_rows(self, start_ns, end_ns, locate, columns):
        """Rows in [start, end), of one locate if locate >= 0, as an Arrow table."""

    @abstractmethod
    def query_minute_aggregate(self, start_ns, end_ns, locate, msg_types):
        """Per minute: [minute ns since epoch, count, sum shares, min price, max price]."""

    @abstractmethod
    def query_column_sum(self, column):
        """[[row count, sum of column]]."""

    def scan_facts(self, query):
        """Rows and bytes the engine decoded for a query, if the system can say."""
        return {"status": "unavailable", "reason": f"{self.name} does not report it"}

    def durability_verdict(self, flush, control_saw_dirty):
        """Whether this class is proven. By default from dirty pages after a commit."""
        return durability_verdict(self.context.durability, flush, control_saw_dirty)

    def flush_findings(self, facts, flush):
        """durability_facts, corrected by what the full ingest showed."""
        return facts

    def after_queries(self):
        """Anything measured last, because it changes the store."""
        return None

    # Either side.

    @abstractmethod
    def durability_facts(self):
        """{"sync_primitive", "flush_granularity", "like_for_like", "note"} for this class."""

    @abstractmethod
    def data_paths(self):
        """Every file of the store."""

    def read_paths(self):
        """The files whose resident pages can only have come from the device. A file an engine
        writes as it opens, such as LMDB's lock file, is resident with nothing read."""
        return self.data_paths()

    @staticmethod
    @abstractmethod
    def versions():
        """Exact versions, from the running code and not from a pin."""
