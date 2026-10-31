#!/usr/bin/env python3
"""The full-day path on a tiny table: streamed ingest, the host guard, and the walls that keep a
capacity run out of the results. Needs the repo's .venv.

    .venv/bin/python -I tests/compare/test_stream_guard.py
"""

import json
import signal
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
from compare.guard import (GIB, SWAP_STRIKES, HostSample, Limits, breach, parse_meminfo,  # noqa: E402
                           parse_status, parse_swap_pages, session_of, wait_status_fields)
from compare.layout import Layout  # noqa: E402
from compare.schema import ColumnSet, arrow_table, table_chunks  # noqa: E402
from compare.systems import system_class  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402

CHECK_COMPARE = REPO_ROOT / "scripts" / "check_compare.py"
LIMITS = Limits()
HEALTHY = HostSample(8 * GIB, GIB, 0, 200 * GIB)


class Breach(unittest.TestCase):
    def test_a_healthy_host_goes_on(self):
        self.assertEqual(breach(HEALTHY, 0, 0, LIMITS), (None, 0))

    def test_memory_under_the_limit_fires_at_once(self):
        low = HostSample(3 * GIB - 1, GIB, 0, 200 * GIB)
        self.assertIn("MemAvailable", breach(low, 0, 0, LIMITS)[0])

    def test_memory_at_the_limit_goes_on(self):
        self.assertIsNone(breach(HostSample(3 * GIB, GIB, 0, 200 * GIB), 0, 0, LIMITS)[0])

    def test_a_full_disk_fires(self):
        self.assertIn("disk", breach(HostSample(8 * GIB, GIB, 0, GIB), 0, 0, LIMITS)[0])

    def test_swap_fires_only_when_it_keeps_on(self):
        fast = LIMITS.max_swap_bytes_per_second + 1
        strikes = 0
        for _ in range(SWAP_STRIKES - 1):
            reason, strikes = breach(HEALTHY, fast, strikes, LIMITS)
            self.assertIsNone(reason)
        self.assertEqual(breach(HEALTHY, 0, strikes, LIMITS), (None, 0))
        self.assertIn("swap", breach(HEALTHY, fast, strikes, LIMITS)[0])


class ProcFiles(unittest.TestCase):
    def test_meminfo(self):
        fields = parse_meminfo("MemTotal:  10 kB\nMemAvailable:  4 kB\nHugePages_Total:  0\n")
        self.assertEqual(fields["MemAvailable"], 4096)

    def test_swap_counts_both_ways(self):
        self.assertEqual(parse_swap_pages("pswpin 3\npswpout 4\nnr_free_pages 9\n"), 7)

    def test_status_fields(self):
        status = parse_status("Name:\tjava\nVmHWM:\t  100 kB\nRssFile:\t  7 kB\nThreads:\t9\n")
        self.assertEqual(status, {"VmHWM": 102400, "RssFile": 7168})

    def test_a_comm_with_spaces_and_a_bracket_does_not_move_the_session(self):
        self.assertEqual(session_of("12 (a b) c) S 1 12 77 0 -1 4194304"), 77)

    def test_a_wait_status_names_its_signal(self):
        self.assertEqual(wait_status_fields(-signal.SIGILL), {"code": None, "signal": "SIGILL"})
        self.assertEqual(wait_status_fields(3), {"code": 3, "signal": None})


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

    def system(self, name):
        kind = system_class(name)
        layout = Layout(self.base, self.base, self.base, self.base)
        context = RunContext(layout, self.base / name, kind.headline_variant, CLASS_B, DAY,
                             full_day=True)
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

    def test_a_day_maps_more_than_a_slice_and_the_chunk_fits_the_stored_unit(self):
        system = self.system("arcticdb")
        self.assertGreater(system.map_size_bytes, 2_000_000_000)
        self.assertEqual(system.stream_chunk_rows() % system.tuning["rows_per_segment"], 0)
        parquet = self.system("parquet")
        self.assertEqual(parquet.stream_chunk_rows() % parquet.tuning["row_group_rows"], 0)


if __name__ == "__main__":
    unittest.main()
