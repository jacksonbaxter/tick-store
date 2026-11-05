"""tick-store, the store under test, through its own tools and nothing else.

Its natural input is ITCH, so a write is tools/tick_roundtrip ingest on the slice itself, timed
from here like every other system's write, and that time holds the ITCH parsing the other systems
never do, since they load the columns tools/tick_export already parsed. Nothing subtracts it.

Queries go to one tools/tick_query worker per store, started in open(), so the store is opened
before any clock, as every other system's client is. Each query is one request line with "ack",
timed from here as a round trip: the clock stops on the line the worker sends once the answer is
in its memory, and the digest it then makes, and its full answer line, fall outside the clock, as
the other systems' digests do in Python. The worker's own engine time is recorded beside it.

The read-back goes through tick_export --store, the store's records as the harness's columns,
which tick_roundtrip verify-columns then judges as it judges every system. The byte-level G-1
check, tick_roundtrip verify of the store against the source, is stronger, and runs too.

Every setting is read back from the store or a running tool: the version and segment size from
the store section's bytes, the durability from the flushes the writer made, each block's codec
from the footer, and the kernel set from the query worker that chose it.
"""

import json
import platform
import shutil
import subprocess
import time
from pathlib import Path

from compare import coldcache
from compare.queries import ROW_COLUMNS, aggregate_digest
from compare.schema import ColumnSet, arrow_table, read_manifest
from compare.settings import NOT_PROVEN, PROVEN, Setting, SettingsMismatch, require_all_match
from compare.systems.base import System

STORE_NAME = "store.tsc"
PROBE_NAME = "probe.tsc"
STORE_FORMAT = "itch-store-v3"
STORE_VERSION = 3
SEGMENT_RECORDS = 1 << 22  # The writer's default, and the headline: see compare/README.md.
SEGMENT_RECORDS_1MI = 1 << 20
HOT_COLUMNS = ("the writer's default column specs: the eight hot dictionary columns held to "
               "ranked PFOR. Not read back: the footer names each block's codec id, not its "
               "dictionary mode")
QUERY_PATH = "indexed"
MODE_FOR_CLASS = {"A": "page_cache", "B": "device_flush"}
WORKER_START_SECONDS = 60
KERNEL_LEVELS = ("scalar", "neon", "avx2")
# The settings probe's kernel counters come from a query that reads every segment: the share
# sum, Q5's shape.
SETTINGS_PROBE = {"name": "settings", "kind": "column_sum"}
INPUT_STATEMENT = ("tick-store ingests the ITCH slice itself, so its write time holds ITCH "
                   "parsing. The other systems ingest the columns tools/tick_export wrote from "
                   "the same slice, already parsed, and their times do not. Nothing is "
                   "subtracted.")
FULL_DAY_INPUT_STATEMENT = ("tick-store ingests the day's ITCH file itself, so its write time "
                            "holds ITCH parsing. The other systems ingest the columns "
                            "tools/tick_export wrote from the same day, already parsed, and "
                            "their times do not. Nothing is subtracted.")
G1_NOTE = ("tick_roundtrip verify, the G-1 check: every record re-serialised from the store and "
           "compared with the source byte for byte, and both streams hashed. Stronger than the "
           "column oracle every system passes.")

# docs/store.md, "Columns": the codec each column's blocks name in the footer, and whether the
# writer may wrap a piece in zstd where that is smaller. Every PFOR variant is codec id pfor.
COLUMN_CODECS = {
    "type": ("dictionary", False), "locate": ("pfor", True), "timestamp": ("pfor", False),
    "tracking": ("dictionary", False), "creation_ref": ("pfor", True),
    "ref_distance": ("dictionary", False), "import_ref": ("pfor", False),
    "add_side": ("dictionary", False), "add_price": ("pfor", True),
    "add_shares": ("dictionary", False), "mpid": ("dictionary", False),
    "reduce_shares": ("dictionary", False), "match": ("pfor", False),
    "printable": ("run_length", False), "exec_price": ("pfor", False),
    "replace_shares": ("dictionary", False), "replace_price": ("pfor", False),
    "trade_ref": ("pfor", False), "trade_side": ("dictionary", False),
    "trade_shares": ("dictionary", False), "trade_price": ("pfor", False),
    "cross_shares": ("pfor", False), "cross_price": ("pfor", False),
    "cross_type": ("dictionary", False), "imbalance_shares": ("pfor", False),
    "imbalance_price": ("pfor", False), "imbalance_flags": ("dictionary", True),
    "mwcb_level": ("pfor", False), "rare_bytes": ("raw", True), "raw_length": ("pfor", False),
    "raw_bytes": ("raw", True), "stock_exception_record": ("pfor", False),
    "stock_exception_value": ("raw", False), "time_base": ("pfor", False),
    "locate_run": ("pfor", False), "locate_run_start": ("pfor", False),
    "locate_run_add_start": ("pfor", False), "locate_run_import_start": ("pfor", False),
    "seek": ("pfor", False),
}


class ToolFailure(Exception):
    """A tick-store tool exited non-zero or gave an answer that is not one."""


def allowed_codecs(column):
    codec, zstd = COLUMN_CODECS[column]
    return {codec, "zstd"} if zstd else {codec}


def expected_kernel_level():
    """The best set this processor runs, read from the processor and never from our tools."""
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        return "neon"  # NEON is baseline on arm64.
    if machine in ("x86_64", "amd64"):
        if platform.system() == "Linux":
            flags = set()
            for line in Path("/proc/cpuinfo").read_text(encoding="ascii").splitlines():
                if line.startswith("flags"):
                    flags.update(line.split(":", 1)[1].split())
                    break
            return "avx2" if "avx2" in flags else "scalar"
        text = subprocess.run(["sysctl", "-n", "machdep.cpu.leaf7_features"], check=False,
                              capture_output=True, text=True).stdout
        return "avx2" if "AVX2" in text.split() else "scalar"
    return "scalar"


def expected_flushes(durability, darwin=None):
    """What the writer must have called for a class: nothing under A, and under B the file's
    data before the rename, then its directory after, by the host's primitive."""
    if durability != "B":
        return []
    darwin = platform.system() == "Darwin" if darwin is None else darwin
    if darwin:
        return [{"call": "F_FULLFSYNC", "of": "file", "result": 0},
                {"call": "F_FULLFSYNC", "of": "directory", "result": 0}]
    return [{"call": "fdatasync", "of": "file", "result": 0},
            {"call": "fsync", "of": "directory", "result": 0}]


def flush_text(flushes):
    return [f"{item['call']} on the {item['of']}, returned {item['result']}" for item in flushes]


def day_label(day):
    return f"{day.month:02d}{day.day:02d}{day.year:04d}"


def codec_settings(blocks):
    """One setting per column: every block of it names a codec docs/store.md lists for it."""
    found = {column: set() for column in COLUMN_CODECS}
    for block in blocks:
        if block["column"] not in found:
            raise SettingsMismatch(f"the footer names a column docs/store.md has not: "
                                   f"{block['column']!r}")
        found[block["column"]].add(block["codec"])
    settings = [Setting(f"codec ids, {column}", True, codecs <= allowed_codecs(column),
                        "every block of the column in the footer, by tick_roundtrip layout "
                        "--blocks, against docs/store.md")
                for column, codecs in found.items()]
    return settings, {column: sorted(codecs) for column, codecs in found.items()}


def answer_key(kind, start_ns, end_ns, locate):
    """A column sum has no window and no locate, whatever the query object holds."""
    if kind == "column_sum":
        return (kind, 0, 0, -1)
    return (kind, start_ns, end_ns, locate)


def kernel_sets_that_ran(runs):
    return sorted(level for level in KERNEL_LEVELS if runs.get(level, 0) > 0)


class EngineAnswer:
    """One answer from the worker. The timed call returns this as soon as the worker says the
    answer is in its memory; its digest is read afterwards, outside the clock."""

    def __init__(self, system, first):
        self.system = system
        self.first = first
        self.full = None

    def finish(self):
        if self.full is None:
            self.full = self.system._read_line()
            if "error" in self.full or self.full.get("name") != self.first.get("name"):
                raise ToolFailure(f"tick_query: {self.full}")
        return self.full

    @property
    def engine_seconds(self):
        return self.finish()["engine_ns"] / 1e9

    def harness_digest(self, query):
        """The worker's digest for rows, which it made from the rows it returned. For an
        aggregate, the answer itself is digested here as well, and the two must agree."""
        full = self.finish()
        own = {"rows": full["rows"], "sha256": full["sha256"]}
        if query.kind == "rows":
            return own
        mine = aggregate_digest(full["answer"])
        if mine != own:
            raise ToolFailure(f"tick_query digested its answer as {own}, and it is {mine}")
        return mine


def _variant(segment_records):
    return {"store_version": STORE_VERSION, "segment_records": segment_records,
            "hot_columns": HOT_COLUMNS, "query_path": QUERY_PATH,
            "checksums": "verified on every read"}


class TickStoreSystem(System):
    name = "tickstore"
    # The headline is the writer's default, 2^22 records, chosen by the rule in "Layout
    # experiments" in NOTES.md before it was measured. 2^20 is the layout it replaced, faster
    # on Q1 and Q4 and slower on Q2, Q3 and Q5: see compare/README.md.
    headline_variant = "v3-seg4Mi"
    variants = {"v3-seg4Mi": _variant(SEGMENT_RECORDS),
                "v3-seg1Mi": _variant(SEGMENT_RECORDS_1MI)}

    def __init__(self, context):
        super().__init__(context)
        self.mode = MODE_FOR_CLASS[context.durability]
        self.kernel_level = expected_kernel_level()
        self.flushes = None
        self.ingest_report = None
        self.expected_records = None
        self.worker = None
        self.ready = None
        self.pending = None
        self.answers = {}
        self.export_dir = None
        self.layout_report = None
        self.codecs_found = None
        self.segment_records = self.tuning["segment_records"]

    @property
    def store_path(self):
        return self.root / STORE_NAME

    def tool(self, name):
        return str(self.context.layout.tool(name))

    def worker_environment(self):
        # The cap names the level asked for, so a build or processor without it stops the
        # worker rather than quietly running another set.
        return {"TICK_STORE_SIMD": self.kernel_level}

    def requested(self):
        return {"input": FULL_DAY_INPUT_STATEMENT if self.context.full_day else INPUT_STATEMENT,
                "store_format": STORE_FORMAT,
                "ingest": f"tools/tick_roundtrip ingest --durability {self.mode} "
                          f"--segment-records {self.segment_records}",
                "durability": self.mode, "segment_records": self.segment_records,
                "hot_columns": HOT_COLUMNS,
                "codecs": "each column's codec as docs/store.md lists it, zstd where the "
                          "writer finds it smaller",
                "query_worker": f"tools/tick_query, one process per store, opened before the "
                                f"clock, path {QUERY_PATH}, block CRCs checked",
                "kernel_level": self.kernel_level, "TICK_STORE_SIMD": self.kernel_level,
                "threads": "one: the writer and the query worker each run on one thread"}

    # Writes.

    def ingest_command(self, source, store, mode, first_records=None, segment_records=None):
        command = [self.tool("tick_roundtrip"), "ingest", str(source), str(store),
                   "--durability", mode,
                   "--segment-records", str(segment_records or self.segment_records)]
        if first_records is not None:
            command += ["--first-records", str(first_records)]
        return command

    def _run_ingest(self, command):
        run = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                             check=False)
        if run.returncode != 0:
            raise ToolFailure(f"tick_roundtrip ingest exited {run.returncode}: "
                              f"{run.stderr.strip()[:300]}")
        return json.loads(run.stdout)

    def _source(self):
        if self.context.source is None:
            raise ToolFailure("tick-store ingests the ITCH slice, and the run gave none")
        return Path(self.context.source)

    def open(self):
        if self.store_path.exists():
            self._start_worker()

    def settings_in_force(self):
        return []

    def ingest_day(self, columns):
        """A whole day is the same command on the day's file: it is tick-store's natural input,
        and no table is built."""
        return self.ingest(columns, None)

    def ingest(self, columns, table):
        source = self._source()
        warmed = coldcache.warm(self.context.layout, [source])
        if warmed["resident_pages"] != warmed["pages"]:
            raise ToolFailure("the source is not resident before the write")
        self.expected_records = columns.records
        command = self.ingest_command(source, self.store_path, self.mode)
        started = time.perf_counter()
        report = self._run_ingest(command)
        finished = time.perf_counter()
        self.ingest_report = report
        self.flushes = report["flushes"]
        seconds = finished - started
        return {"seconds": seconds, "engine_seconds": report["wall_seconds"],
                "python_seconds": seconds - report["wall_seconds"],
                "python_note": "python_seconds is the tool's start and exit, outside its own "
                               "clock. engine_seconds is tick_roundtrip's own wall time: mapping "
                               "the source, parsing ITCH, coding and writing the store, and its "
                               "commit",
                "input": FULL_DAY_INPUT_STATEMENT if self.context.full_day else INPUT_STATEMENT,
                "input_resident": {"pages": warmed["pages"],
                                   "resident_pages": warmed["resident_pages"]},
                "records": report["records"], "stored_file_bytes": report["stored_file_bytes"],
                "tool_peak_rss_bytes": report["peak_rss_bytes"], "flushes": report["flushes"],
                "clock_stops": "when tick_roundtrip exits, after the rename that publishes the "
                               "store and, under device_flush, its flushes"}

    def probe_flush(self, table):
        report = self._run_ingest(self.ingest_command(self._source(), self.root / PROBE_NAME,
                                                      self.mode, first_records=table.num_rows))
        if report["records"] != table.num_rows:
            raise ToolFailure(f"the probe stored {report['records']} records")
        self.flushes = report["flushes"]

    def flush_facts(self):
        return {"engine_flushes": self.flushes}

    def dirty_paths(self):
        return [self.root / PROBE_NAME]

    def settings_after_ingest(self):
        """Everything in force, read back from the store just written and the tools."""
        report = self.ingest_report
        self._start_worker(check=False)
        try:
            probe = self._ask(SETTINGS_PROBE)
            runs = probe.finish()["kernel_runs"]
            ready = self.ready
        finally:
            self.close()
        settings, codecs = codec_settings(self.layout_blocks())
        self.codecs_found = codecs
        return require_all_match([
            Setting("store version", STORE_VERSION, ready["store_version"],
                    "the store section's bytes, read by tools/tick_query"),
            Setting("segment records", self.segment_records, ready["segment_records"],
                    "the store section's bytes, read by tools/tick_query"),
            Setting("records", self.expected_records, ready["records"],
                    "the store's header, as tools/tick_query opened it"),
            Setting("durability mode", self.mode, report["durability"],
                    "tick_roundtrip ingest's report of the mode its writer was created in"),
            Setting("flushes made", flush_text(expected_flushes(self.context.durability)),
                    flush_text(report["flushes"]),
                    "the system calls the writer made, recorded by tick_roundtrip through its "
                    "FileOps seam"),
            Setting("query path", QUERY_PATH, ready["path"], "tools/tick_query's ready line"),
            Setting("kernel level", self.kernel_level, ready["kernel_level"],
                    "the kernel set tools/tick_query chose, with TICK_STORE_SIMD at the level "
                    "the processor's own flags give"),
            Setting("kernel sets that ran", [self.kernel_level], kernel_sets_that_ran(runs),
                    "tools/tick_query's per-thread kernel counters over one share sum"),
            *settings,
        ])

    def layout_blocks(self):
        run = subprocess.run([self.tool("tick_roundtrip"), "layout", str(self.store_path),
                              "--blocks"], capture_output=True, text=True, check=False)
        if run.returncode != 0:
            raise ToolFailure(f"tick_roundtrip layout exited {run.returncode}")
        self.layout_report = json.loads(run.stdout)
        return self.layout_report["block_list"]

    # Reads.

    def _start_worker(self, check=True):
        command = [self.tool("tick_query"), str(self.store_path), "--day",
                   day_label(self.context.day), "--path", QUERY_PATH]
        self.worker = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, bufsize=1)
        try:
            self.ready = self._read_line()
            if not self.ready.get("ready"):
                raise ToolFailure(f"tick_query is not ready: {self.ready}")
        except (ToolFailure, json.JSONDecodeError):
            worker = self.worker
            self.worker = None
            worker.kill()
            worker.wait()
            for stream in (worker.stdin, worker.stdout, worker.stderr):
                stream.close()
            raise
        if check:
            require_all_match([
                Setting("store version", STORE_VERSION, self.ready["store_version"],
                        "the store section's bytes, read by tools/tick_query"),
                Setting("segment records", self.segment_records, self.ready["segment_records"],
                        "the store section's bytes, read by tools/tick_query"),
                Setting("kernel level", self.kernel_level, self.ready["kernel_level"],
                        "the kernel set tools/tick_query chose"),
                Setting("query path", QUERY_PATH, self.ready["path"],
                        "tools/tick_query's ready line"),
            ])

    def _read_line(self):
        line = self.worker.stdout.readline()
        if not line:
            code = self.worker.wait()
            raise ToolFailure(f"tick_query exited {code}: "
                              f"{self.worker.stderr.read().strip()[:300]}")
        return json.loads(line)

    def _ask(self, request):
        """Sends one request and returns once the worker says the answer is in its memory."""
        if self.pending is not None:
            self.pending.finish()
        self.worker.stdin.write(json.dumps({**request, "ack": True}) + "\n")
        self.worker.stdin.flush()
        first = self._read_line()
        if "error" in first or not first.get("answered"):
            raise ToolFailure(f"tick_query: {first}")
        self.pending = EngineAnswer(self, first)
        self.answers[answer_key(request["kind"], request.get("start_ns", 0),
                                request.get("end_ns", 0), request.get("locate", -1))] = self.pending
        return self.pending

    def close(self):
        worker = self.worker
        if worker is not None:
            try:
                if worker.poll() is None:
                    if self.pending is not None:
                        self.pending.finish()
                    worker.stdin.write(json.dumps({"kind": "quit"}) + "\n")
                    worker.stdin.flush()
                    worker.stdout.readline()
            finally:
                self.pending = None
                self.worker = None
                worker.stdin.close()
                code = worker.wait(timeout=WORKER_START_SECONDS)
                worker.stdout.close()
                worker.stderr.close()
            if code != 0:
                raise ToolFailure(f"tick_query exited {code}")
        if self.export_dir is not None:
            shutil.rmtree(self.export_dir, ignore_errors=True)
            self.export_dir = None

    def read_all(self):
        """The store's records as the harness's columns, through tick_export --store."""
        directory = self.context.layout.work / "readback-export" / self.name
        self.export_dir = directory
        self._export(directory)
        return arrow_table(ColumnSet(directory), self.context.day)

    def read_back_to(self, destination, windows):
        """A whole day: tick_export --store writes the column files straight to `destination`,
        every record through the store's read path and the ITCH encoder, with no table between."""
        started = time.perf_counter()
        self._export(destination)
        seconds = time.perf_counter() - started
        return {"rows": read_manifest(destination)["records"], "read_seconds": seconds,
                "windows": "none: tick_export --store reads the whole store in order"}

    def _export(self, directory):
        shutil.rmtree(directory, ignore_errors=True)
        Path(directory).parent.mkdir(parents=True, exist_ok=True)
        run = subprocess.run([self.tool("tick_export"), "--store", str(self.store_path),
                              str(directory)], capture_output=True, text=True, check=False)
        if run.returncode != 0:
            raise ToolFailure(f"tick_export --store exited {run.returncode}: "
                              f"{run.stderr.strip()[:300]}")

    def query_rows(self, start_ns, end_ns, locate, columns):
        if tuple(columns) != ROW_COLUMNS:
            raise ValueError("tick_query digests ROW_COLUMNS and nothing else")
        return self._ask({"kind": "rows", "start_ns": start_ns, "end_ns": end_ns,
                          "locate": locate})

    def query_minute_aggregate(self, start_ns, end_ns, locate, msg_types):
        from compare.queries import ADD_ORDER_TYPES

        if tuple(msg_types) != ADD_ORDER_TYPES:
            raise ValueError("tick_query aggregates A and F and nothing else")
        return self._ask({"kind": "minute_aggregate", "start_ns": start_ns, "end_ns": end_ns,
                          "locate": locate})

    def query_column_sum(self, column):
        if column != "shares":
            raise ValueError("tick_query sums shares and nothing else")
        return self._ask({"kind": "column_sum"})

    def scan_facts(self, query):
        """What the engine read for the last answer to this query, from its own counters."""
        answer = self.answers.get(answer_key(query.kind, query.start_ns, query.end_ns,
                                             query.locate))
        if answer is None:
            return {"status": "unavailable", "reason": "the query did not run in this worker"}
        full = answer.finish()
        return {"rows_decoded": {"status": "unavailable",
                                 "reason": "tick-store counts values decoded, not rows"},
                "bytes_decoded": full["bytes_decoded"], "values_decoded": full["values_decoded"],
                "segments": full["segments"], "exact_segments": full["exact_segments"],
                "blocks": full["blocks"], "kernel_runs": full["kernel_runs"],
                "digest_ns": full["digest_ns"],
                "method": "tools/tick_query's own counters for the last repetition: stored "
                          "bytes and values decoded, segments and blocks read"}

    def stored_settings(self):
        """The stored form, read back: the header, every block's codec, and G-1."""
        if self.worker is None:
            self._start_worker()
        ready = self.ready
        blocks = self.layout_blocks()
        layout = self.layout_report
        settings, codecs = codec_settings(blocks)
        records = ready["records"]
        segments = -(-records // self.segment_records)
        g1 = self.verify_g1()
        checked = require_all_match([
            Setting("store version", STORE_VERSION, ready["store_version"],
                    "the store section's bytes"),
            Setting("segment records", self.segment_records, layout["segment_records"],
                    "tick_roundtrip layout, from the footer"),
            Setting("segments", segments, layout["segments"], "tick_roundtrip layout"),
            Setting("G-1 verdict", "pass", g1.get("verdict"), "tick_roundtrip verify"),
            Setting("G-1 hashes match", True, g1.get("hashes_match"), "tick_roundtrip verify"),
            *settings,
        ])
        return {"checks_passed": len(checked), "store_format": STORE_FORMAT,
                "records": records, "segments": layout["segments"],
                "blocks": layout["blocks"], "file_bytes": layout["file_bytes"],
                "codecs_found": codecs,
                "columns": [{key: column[key] for key in ("column", "payload_bytes",
                                                          "metadata_bytes", "blocks",
                                                          "zstd_blocks", "values")}
                            for column in layout["columns"]],
                "sections": layout["sections"], "container": layout["container"],
                "g1": {key: g1.get(key) for key in ("verdict", "records", "mismatched_records",
                                                    "hashes_match", "exit_code")},
                "g1_note": G1_NOTE}

    def verify_g1(self):
        run = subprocess.run([self.tool("tick_roundtrip"), "verify", str(self._source()),
                              str(self.store_path)], capture_output=True, text=True, check=False)
        try:
            report = json.loads(run.stdout)
        except json.JSONDecodeError:
            report = {"verdict": "no report"}
        report["exit_code"] = run.returncode
        return report

    # Durability.

    def durability_verdict(self, flush, control_saw_dirty):
        """The writer's flushes are recorded call by call, so the proof is the calls made.

        Dirty pages are recorded beside them. macOS starts writing a file out when it closes, so
        a closed store can read 0 dirty with or without a flush, as Parquet's does."""
        calls = flush.get("engine_flushes")
        wanted = expected_flushes(self.context.durability)
        if self.class_b:
            if calls == wanted and flush.get("data_dirty_pages") == 0:
                return {"status": PROVEN,
                        "reason": f"the writer called {', '.join(flush_text(calls))}, and 0 pages "
                                  "stayed dirty"}
            return {"status": NOT_PROVEN,
                    "reason": f"flushes made: {flush_text(calls or [])}, dirty pages "
                              f"{flush.get('data_dirty_pages')}"}
        if calls == []:
            return {"status": PROVEN, "reason": "the writer made no flush: page_cache"}
        return {"status": NOT_PROVEN, "reason": f"flushes made in class A: {flush_text(calls)}"}

    def durability_facts(self):
        if not self.class_b:
            return {"sync_primitive": "none: Durability::page_cache makes no flush",
                    "flush_granularity": "none", "like_for_like": True,
                    "note": "every byte handed to the kernel and the file renamed into place"}
        darwin = platform.system() == "Darwin"
        return {"sync_primitive": ("F_FULLFSYNC on the file, then on its directory" if darwin
                                   else "fdatasync on the file, then fsync on its directory"),
                "flush_granularity": "one flush per file, at its commit: the file's data before "
                                     "the rename that publishes it, then its directory",
                "like_for_like": True,
                "note": ("F_FULLFSYNC, as LMDB and the harness's Parquet flush use" if darwin
                         else "fdatasync and a directory fsync, as LMDB's fdatasync and the "
                              "harness's fsync reach the device")}

    def data_paths(self):
        return [self.store_path]

    @staticmethod
    def versions():
        return {"tick_store": "this repository at the header's commit, bench build",
                "store_format": STORE_FORMAT}
