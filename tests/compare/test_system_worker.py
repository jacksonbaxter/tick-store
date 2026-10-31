#!/usr/bin/env python3
"""The worker's timed path, on a real Parquet file of six rows, in a process of its own. Needs
the repo's .venv.

    .venv/bin/python -I tests/compare/test_system_worker.py

A results file says that each cold number is the query alone, with the worker's imports before
the clock. `pq.read_table` imports `pyarrow.dataset` itself, on its first call, so that sentence
has to be tested: the child below is a fresh process, as a cold query's is, running worker.main
with one thing added, a look at sys.modules on each side of the timed call.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import pyarrow  # noqa: F401
except ImportError:
    raise unittest.SkipTest("needs the .venv: compare/setup.py makes it") from None

from test_schema import DAY, write_fixture  # noqa: E402

import check_compare  # noqa: E402

from compare import run as compare_run  # noqa: E402
from compare.durability import CLASS_B  # noqa: E402
from compare.layout import Layout  # noqa: E402
from compare.queries import OPEN_NS, Query, ground_truth, query_to_json  # noqa: E402
from compare.schema import NANOS_PER_SECOND, ColumnSet  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402
from compare.systems.parquet_system import ParquetSystem  # noqa: E402
from compare.systems.questdb_system import QuestDBServer  # noqa: E402

WATCHED_WORKER = """
import json, sys
sys.path.insert(0, sys.argv[1])
from compare import worker

seen = {}
query = worker.run_query

def watched(system, request):
    seen["dataset_before_the_clock"] = "pyarrow.dataset" in sys.modules
    before = set(sys.modules)
    result = query(system, request)
    seen["imported_inside_the_clock"] = sorted(set(sys.modules) - before)
    return result

worker.run_query = watched
code = worker.main(sys.argv[2:])
print(json.dumps(seen))
sys.exit(code)
"""
DAY_LABEL = "12302019"
VARIANT = ParquetSystem.headline_variant
QUERIES = (Query("rows", "rows", OPEN_NS, OPEN_NS + NANOS_PER_SECOND),
           Query("minutes", "minute_aggregate", OPEN_NS, OPEN_NS + 300 * NANOS_PER_SECOND),
           Query("sum", "column_sum", 0, 0))


class ColdWorker(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        cls.scratch = tempfile.TemporaryDirectory(dir=REPO_ROOT / "build")
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.base = Path(cls.scratch.name)
        (cls.base / "in").mkdir()
        write_fixture(cls.base / "in")
        cls.columns = ColumnSet(cls.base / "in")
        layout = Layout(cls.base, cls.base, cls.base, cls.base)
        cls.root = cls.base / "store"
        ParquetSystem(RunContext(layout, cls.root, VARIANT, CLASS_B, DAY)).prepare()
        cls.worker({"step": "ingest", "columns": str(cls.base / "in")})

    @classmethod
    def worker(cls, fields):
        """One worker.main in a fresh process. Returns what it saw of the timed call."""
        request = {"system": "parquet", "variant": VARIANT, "class": CLASS_B,
                   "root": str(cls.root), "day": DAY_LABEL,
                   "layout": dict.fromkeys(("data", "vendor", "work", "tools"), str(cls.base)),
                   **fields}
        request_path = cls.base / "request.json"
        request_path.write_text(json.dumps(request), encoding="utf-8")
        run = subprocess.run([sys.executable, "-I", "-c", WATCHED_WORKER, str(REPO_ROOT),
                              str(request_path), str(cls.base / "answer.json")], check=False,
                             capture_output=True, text=True)
        if run.returncode != 0:
            raise AssertionError(f"the worker exited {run.returncode}: {run.stderr}")
        return json.loads(run.stdout)

    def cold(self, query):
        truth = ground_truth(self.columns, query, DAY)
        return self.worker({"step": "cold_query", "query": query_to_json(query), "truth": truth})

    def test_the_dataset_module_is_loaded_before_the_clock(self):
        for query in QUERIES:
            with self.subTest(query=query.kind):
                self.assertTrue(self.cold(query)["dataset_before_the_clock"])

    def test_nothing_at_all_is_imported_inside_the_clock(self):
        for query in QUERIES:
            with self.subTest(query=query.kind):
                self.assertEqual(self.cold(query)["imported_inside_the_clock"], [])


class HeapStatements(unittest.TestCase):
    """What compare/run.py writes, against the validator and the real capacity file."""

    @staticmethod
    def problems(document, host):
        capacity = json.loads(check_compare.DEFAULT_CAPACITY.read_text(encoding="utf-8"))
        return check_compare.check_heap(document, host, capacity)

    def test_each_host_passes(self):
        self.assertEqual(self.problems({"heap": compare_run.LINUX_HEAP}, "Linux"), [])
        self.assertEqual(self.problems({"heap": compare_run.MACOS_HEAP}, "macOS"), [])

    def test_the_server_is_started_with_the_heap_stated(self):
        self.assertEqual(" ".join(QuestDBServer(None, "root", 2, {}).heap_options),
                         compare_run.QUESTDB_HEAP)


if __name__ == "__main__":
    unittest.main(verbosity=1)
