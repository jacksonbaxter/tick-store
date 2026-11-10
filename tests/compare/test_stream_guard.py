#!/usr/bin/env python3
"""The full-day path on a tiny table: streamed ingest and the walls that keep a capacity run out
of the results. Needs the repo's .venv. The guard's own rule is tested in test_guard.py.

    .venv/bin/python -I tests/compare/test_stream_guard.py
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import pyarrow  # noqa: F401
except ImportError:
    raise unittest.SkipTest("needs the .venv: compare/setup.py makes it") from None

from test_schema import DAY, RECORDS, write_fixture  # noqa: E402

from compare.durability import CLASS_B  # noqa: E402
from compare.fullday import LABEL, MEASUREMENT, refuse_results_folder  # noqa: E402
from compare.layout import Layout  # noqa: E402
from compare.schema import ColumnSet, arrow_table, read_windows, table_chunks  # noqa: E402
from compare.systems import system_class  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402

CHECK_COMPARE = REPO_ROOT / "scripts" / "check_compare.py"


class StaysOutOfResults(unittest.TestCase):
    def test_a_work_folder_under_results_is_refused(self):
        for path in ("/x/results", "/x/results/compare", "/x/results/compare/fullday"):
            with self.assertRaises(SystemExit):
                refuse_results_folder(path)
        refuse_results_folder("/x/build/compare")

    def test_the_validator_refuses_a_report(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            path = Path(scratch) / "report.json"
            path.write_text(json.dumps({"measurement": MEASUREMENT, "label": LABEL}),
                            encoding="utf-8")
            run = subprocess.run([sys.executable, "-I", str(CHECK_COMPARE), str(path)],
                                 check=False, capture_output=True, text=True)
        self.assertEqual(run.returncode, 1, run.stderr)

    def test_the_label_says_what_it_is(self):
        self.assertEqual(MEASUREMENT, "iteration-only")
        self.assertIn("not a benchmark", LABEL)


class StreamedIngest(unittest.TestCase):
    """Chunks of 2 rows, so that an append and a short last chunk both happen."""

    def setUp(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        self.scratch = tempfile.TemporaryDirectory(dir=REPO_ROOT / "build")
        self.addCleanup(self.scratch.cleanup)
        base = Path(self.scratch.name)
        (base / "in").mkdir()
        write_fixture(base / "in")
        self.columns = ColumnSet(base / "in")
        self.base = base

    def system(self, name, variant=None):
        kind = system_class(name)
        layout = Layout(self.base, self.base, self.base, self.base)
        context = RunContext(layout, self.base / name, variant or kind.headline_variant, CLASS_B,
                             DAY, full_day=True)
        system = kind(context)
        system.prepare()
        system.open()
        self.addCleanup(system.close)
        return system

    def check(self, name):
        system = self.system(name)
        system.ingest_stream(table_chunks(self.columns, DAY, 2))
        back = system.read_all().select(arrow_table(self.columns, DAY).column_names)
        whole = arrow_table(self.columns, DAY)
        for column in whole.column_names:
            self.assertEqual(back.column(column).to_pylist(), whole.column(column).to_pylist(),
                             column)
        self.assertEqual(system.query_column_sum("shares")[0][0], RECORDS)

    def test_parquet(self):
        self.check("parquet")

    def test_arcticdb(self):
        self.check("arcticdb")

    def read_back_matches(self, system, rows_per_window):
        """A whole store back through windows is the input's column files, byte for byte."""
        out = self.base / f"{system.name}-{rows_per_window}-back"
        answer = system.read_back_to(out, read_windows(self.columns, rows_per_window))
        self.assertEqual(answer["rows"], RECORDS)
        for path in sorted((self.base / "in").iterdir()):
            self.assertEqual(path.read_bytes(), (out / path.name).read_bytes(), path.name)

    def test_parquet_reads_a_day_back_window_by_window(self):
        system = self.system("parquet")
        system.ingest_day(self.columns)
        for rows in (1, 2, RECORDS):
            self.read_back_matches(system, rows)

    def test_arcticdb_reads_a_day_back_window_by_window(self):
        system = self.system("arcticdb")
        system.ingest_day(self.columns)
        for rows in (1, 2, RECORDS):
            self.read_back_matches(system, rows)
        self.assertEqual(system.rows_read, RECORDS)

    def test_arcticdb_per_security_writes_each_security_once_from_bucket_files(self):
        system = self.system("arcticdb", "rows100k-cols127-per-security")
        system.stream_chunk_rows = lambda: 2  # Three buckets: locates 0, 7 and 9.
        timings = system.ingest_day(self.columns)
        self.assertEqual(timings["buckets"], 3)
        self.assertEqual(timings["symbols"], 3)
        self.assertFalse((self.base / "partition").exists() and
                         any((self.base / "partition").rglob("*.arrow")))
        system.close()
        system.open()
        self.assertEqual(system.symbols, ["locate-00000", "locate-00007", "locate-00009"])
        self.read_back_matches(system, 2)
        self.assertEqual(system.stored_settings()["symbols"], 3)

    def test_the_windows_cover_the_day_and_never_split_a_nanosecond(self):
        for rows in (1, 2, 4, RECORDS, RECORDS + 1):
            windows = read_windows(self.columns, rows)
            self.assertEqual(windows[0][0], 0)
            self.assertGreater(windows[-1][1], int(self.columns.fixed["ts_ns"][-1]))
            for (_, end), (start, _) in zip(windows, windows[1:]):
                self.assertEqual(end, start)

    def test_a_day_maps_more_than_a_slice_and_the_chunk_fits_the_stored_unit(self):
        system = self.system("arcticdb")
        self.assertGreater(system.map_size_bytes, 2_000_000_000)
        self.assertEqual(system.stream_chunk_rows() % system.tuning["rows_per_segment"], 0)
        parquet = self.system("parquet")
        self.assertEqual(parquet.stream_chunk_rows() % parquet.tuning["row_group_rows"], 0)

    def test_a_large_segment_streams_one_segment_at_a_time(self):
        system = self.system("arcticdb")
        self.assertEqual(system.stream_chunk_rows(), 4_000_000)
        system.tuning = {**system.tuning, "rows_per_segment": 10_000_000}
        self.assertEqual(system.stream_chunk_rows(), 10_000_000)
        system.tuning = {**system.tuning, "rows_per_segment": 3_000_000}
        self.assertEqual(system.stream_chunk_rows(), 3_000_000)


if __name__ == "__main__":
    unittest.main()
