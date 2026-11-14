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
from types import SimpleNamespace
from unittest import mock

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

import store_results  # noqa: E402

from compare import run as compare_run  # noqa: E402
from compare import variant_rule  # noqa: E402
from compare.durability import CLASS_B  # noqa: E402
from compare.layout import Layout  # noqa: E402
from compare.queries import OPEN_NS, Query, ground_truth, query_to_json  # noqa: E402
from compare.schema import NANOS_PER_SECOND, ColumnSet  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402
from compare.systems.parquet_system import ParquetSystem  # noqa: E402
from compare.systems.questdb_system import (QuestDBServer, ServerFailure,  # noqa: E402
                                             jvm_properties, library_names, runtime_reading)

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
# A stand-in for tools/tick_coldcache, which a worker's write and cold steps call around their
# clocks: every file has 1 page, none of it resident or dirty.
# Warmed files and the write's input, the export under in/, read resident: a write's input
# must be whole when its clock starts (review RF-11). The store never is.
FAKE_COLDCACHE = """
import json, sys
resident = 1 if sys.argv[1] == "warm" or all("/in/" in arg + "/" for arg in sys.argv[2:]) else 0
print(json.dumps({"pages": 1, "resident_pages": resident, "page_size": 4096, "dirty_pages": 0}))
"""
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
        tool = cls.base / "tick_coldcache"
        tool.write_text(f"#!{sys.executable} -I\n{FAKE_COLDCACHE}", encoding="utf-8")
        tool.chmod(0o755)
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

    def test_a_cold_answer_records_the_store_resident_before_its_clock(self):
        """Review F-1: Parquet's whole open is inside each call, and its clock starts with the
        query, after the worker's open, which touches no store file: what was resident then is
        recorded (review RF-8)."""
        self.cold(QUERIES[0])
        answer = json.loads((self.base / "answer.json").read_text(encoding="utf-8"))
        self.assertIs(answer["open_inside_clock"], True)
        self.assertEqual(answer["resident_bytes_before_clock"], 0)
        self.assertEqual(answer["resident_bytes_before_open"], 0)
        self.assertEqual(answer["seconds"], answer["query_seconds"])
        self.assertIsNone(answer["open_seconds"])


class ColdClockStart(unittest.TestCase):
    """The resident bytes a cold proof keeps are read where the figure's clock starts: before
    the open when the open is added to the figure, before the query when it is not (RF-8)."""

    def answer(self, reported_open):
        from compare import worker

        pages = iter([3])  # Resident just before the query: the open read the store.
        system = SimpleNamespace(name="parquet", open_inside_cold_clock=True,
                                 read_paths=lambda: [],
                                 reported_open_seconds=lambda: reported_open)
        request = {"query": "q", "truth": {}, "store_before_open": {"resident_pages": 0,
                                                                    "page_size": 4096}}
        with (mock.patch.object(worker, "query_from_json", return_value="q"),
              mock.patch.object(worker.coldcache, "residency",
                                lambda layout, paths: {"resident_pages": next(pages),
                                                       "page_size": 4096}),
              mock.patch.object(worker, "timed_query", return_value=(0.01, None)),
              mock.patch.object(worker, "digest_result", return_value={}),
              mock.patch.object(worker, "require_truth")):
            return worker.step_cold_query(system, request, None)

    def test_an_open_outside_the_figure_is_read_before_the_query(self):
        answer = self.answer(reported_open=None)
        self.assertEqual(answer["resident_bytes_before_clock"], 3 * 4096)
        self.assertEqual(answer["seconds"], answer["query_seconds"])

    def test_an_open_added_to_the_figure_is_read_before_the_open(self):
        answer = self.answer(reported_open=0.002)
        self.assertEqual(answer["resident_bytes_before_clock"], 0)
        self.assertEqual(answer["seconds"], 0.012)


class FullDayWarm(unittest.TestCase):
    """Warm on a whole day: the device's own counter across the repetitions, query by query,
    with every attempt kept."""

    def run_warm(self, process_reads, machine_wide=None):
        from compare import coldcache, worker

        reads = iter(process_reads)
        wide = iter(machine_wide or [4096] * len(process_reads))
        self.processes_seen = []

        class Counter:
            def __init__(self, path):
                pass

            def delta(self):
                return {"bytes": next(wide), "counter": "block device stat", "slack": 0}

        test = self

        class Reads:
            def __init__(self, processes):
                test.processes_seen.append(processes)

            def delta(self):
                count = next(reads)
                return {"bytes": count, "by_process": {"worker": count, "waited children": 0},
                        "counter": "read_bytes per process"}

        system = SimpleNamespace(name="fake", query_rows=lambda *args: [[1, 2]],
                                 query_column_sum=lambda column: [[1, 2]],
                                 scan_facts=lambda query: {"status": "unavailable"},
                                 query_processes=lambda: {"tick_query": 7},
                                 reported_open_seconds=lambda: None)
        query = Query("Q5", "column_sum", 0, 0)
        request = {"queries": [query_to_json(query)], "repetitions": 4, "paths": ["store"],
                   "truth": {"Q5": {"rows": 1, "sha256": "x"}},
                   "service_processes": {"questdb": 9}}
        residency = {"pages": 8, "resident_pages": 8, "page_size": 4096}
        with (mock.patch.object(coldcache, "DeviceCounter", Counter),
              mock.patch.object(coldcache, "ProcessReads", Reads),
              mock.patch.object(coldcache, "residency", return_value=residency),
              mock.patch.object(worker, "digest_result", return_value={"rows": 1, "sha256": "x"}),
              mock.patch.object(worker, "python_floor", return_value=[0.0])):
            return worker.warm_full_day(system, request, None)

    def test_processes_that_read_nothing_prove_the_first_attempt(self):
        answer = self.run_warm([0])
        proof = answer["proofs"]["Q5"]
        self.assertEqual((proof["attempts"], proof["discarded"]), (1, []))
        self.assertEqual(proof["accepted"]["device_bytes_read"], 0)
        self.assertEqual(proof["accepted"]["machine_wide_device_bytes_read"], 4096)
        self.assertEqual(self.processes_seen, [{"questdb": 9, "tick_query": 7}])
        self.assertEqual(proof["accepted"]["warm_up_runs"], 1)
        self.assertEqual(len(answer["queries"]["Q5"]["seconds"]), 4)

    def test_an_attempt_that_read_the_device_is_kept_and_run_again(self):
        proof = self.run_warm([4096, 0])["proofs"]["Q5"]
        self.assertEqual(proof["attempts"], 2)
        self.assertEqual(proof["discarded"][0]["device_bytes_read"], 4096)

    def test_three_attempts_that_read_the_device_fail_the_run(self):
        from compare.coldcache import ProofFailed

        with self.assertRaisesRegex(ProofFailed, "each of 3 attempts"):
            self.run_warm([4096, 1, 8192])

    def test_a_quiet_device_does_not_pass_a_process_that_read(self):
        from compare.coldcache import ProofFailed

        with self.assertRaisesRegex(ProofFailed, "each of 3 attempts"):
            self.run_warm([4096, 4096, 4096], machine_wide=[0, 0, 0])

    def test_each_attempt_records_memfree_either_side(self):
        accepted = self.run_warm([0])["proofs"]["Q5"]["accepted"]
        self.assertIn("mem_free_bytes_before", accepted)
        self.assertIn("mem_free_bytes_after", accepted)


class ResidencyWarmOnADay(unittest.TestCase):
    """tick-store's day off Linux keeps the slice's warm proof: every store file resident before
    and after, never the per-process reads of /proc."""

    def test_a_day_asking_for_residency_never_reads_proc(self):
        from compare import coldcache, worker

        system = SimpleNamespace(name="fake", query_rows=lambda *args: [[1, 2]],
                                 query_column_sum=lambda column: [[1, 2]],
                                 scan_facts=lambda query: {"status": "unavailable"},
                                 reported_open_seconds=lambda: None)
        query = Query("Q5", "column_sum", 0, 0)
        request = {"queries": [query_to_json(query)], "repetitions": 3, "paths": ["store"],
                   "truth": {"Q5": {"rows": 1, "sha256": "x"}}, "full_day": True,
                   "warm_proof": "residency"}
        residency = {"pages": 8, "resident_pages": 8, "page_size": 4096}

        def refuse(*args):
            raise AssertionError("the per-process proof ran")

        with (mock.patch.object(coldcache, "ProcessReads", refuse),
              mock.patch.object(coldcache, "warm", return_value=residency) as warmed,
              mock.patch.object(coldcache, "residency", return_value=residency),
              mock.patch.object(worker, "digest_result", return_value={"rows": 1, "sha256": "x"}),
              mock.patch.object(worker, "python_floor", return_value=[0.0])):
            answer = worker.step_warm_queries(system, request, None)
        warmed.assert_called_once()
        self.assertEqual(answer["proof"]["resident_before"], 8)
        self.assertEqual(len(answer["queries"]["Q5"]["seconds"]), 3)

    def test_the_slice_runs_untimed_answer_checked_warm_ups_first(self):
        """Review F-3: a first call that pays the engine's set-up is not a timed repetition."""
        import time

        from compare import coldcache, worker

        calls = []

        def column_sum(column):
            calls.append(time.perf_counter())
            if len(calls) == 1:
                time.sleep(0.05)
            return [[1, 2]]

        system = SimpleNamespace(name="fake", query_column_sum=column_sum,
                                 scan_facts=lambda query: {"status": "unavailable"},
                                 reported_open_seconds=lambda: None)
        query = Query("Q5", "column_sum", 0, 0)
        request = {"queries": [query_to_json(query)], "repetitions": 5, "paths": ["store"],
                   "truth": {"Q5": {"rows": 1, "sha256": "x"}}}
        residency = {"pages": 8, "resident_pages": 8, "page_size": 4096}
        digests = []

        def digest(query, result):
            digests.append(result)
            return {"rows": 1, "sha256": "x"}

        with (mock.patch.object(coldcache, "warm", return_value=residency),
              mock.patch.object(coldcache, "residency", return_value=residency),
              mock.patch.object(worker, "digest_result", digest),
              mock.patch.object(worker, "python_floor", return_value=[0.0])):
            answer = worker.step_warm_queries(system, request, None)
        seconds = answer["queries"]["Q5"]["seconds"]
        self.assertEqual(len(seconds), 5)
        self.assertLess(max(seconds), 0.04)
        self.assertEqual(len(calls), 5 + worker.WARM_UP_RUNS)
        self.assertEqual(len(digests), 5 + worker.WARM_UP_RUNS)
        self.assertEqual(answer["proof"]["warm_up_runs"], worker.WARM_UP_RUNS)
        self.assertEqual(answer["queries"]["Q5"]["warm_up_runs"], worker.WARM_UP_RUNS)

    def test_the_proof_states_the_untimed_runs_that_ran(self):
        # Review RF-10: the proof's warm_up_runs was the constant, whatever ran.
        from compare import coldcache, worker

        real = worker.timed_repetitions

        def none_untimed(system, query, truth, repetitions):
            if repetitions == worker.WARM_UP_RUNS:
                return [], [], None
            return real(system, query, truth, repetitions)

        system = SimpleNamespace(name="fake", query_column_sum=lambda column: [[1, 2]],
                                 scan_facts=lambda query: {}, reported_open_seconds=lambda: None)
        query = Query("Q5", "column_sum", 0, 0)
        request = {"queries": [query_to_json(query)], "repetitions": 2, "paths": ["store"],
                   "truth": {"Q5": {"rows": 1, "sha256": "x"}}}
        residency = {"pages": 8, "resident_pages": 8, "page_size": 4096}
        with (mock.patch.object(coldcache, "warm", return_value=residency),
              mock.patch.object(coldcache, "residency", return_value=residency),
              mock.patch.object(worker, "timed_repetitions", none_untimed),
              mock.patch.object(worker, "digest_result",
                                return_value={"rows": 1, "sha256": "x"}),
              mock.patch.object(worker, "python_floor", return_value=[0.0])):
            answer = worker.step_warm_queries(system, request, None)
        self.assertEqual(answer["queries"]["Q5"]["warm_up_runs"], 0)
        self.assertEqual(answer["proof"]["warm_up_runs"], 0)

    def test_a_linux_day_keeps_the_per_process_proof(self):
        from compare import run_system

        with mock.patch.object(run_system.platform, "system", return_value="Linux"):
            self.assertEqual(run_system.warm_proof_kind(True), run_system.PROCESS_READS)
            self.assertEqual(run_system.warm_proof_kind(False), run_system.RESIDENCY)
        with mock.patch.object(run_system.platform, "system", return_value="Darwin"):
            self.assertEqual(run_system.warm_proof_kind(True), run_system.RESIDENCY)


class PageCacheBeforeWarm(unittest.TestCase):
    """Before a day's warm queries the source, the export and the store leave the page cache,
    each proven at 0 resident, the store flushed first."""

    def runner(self):
        from compare import run_system

        runner = run_system.Runner.__new__(run_system.Runner)
        runner.layout = None
        runner.source = Path("day.itch")
        runner.columns_dir = Path("export/day-full")
        return runner

    def test_inputs_then_store_are_evicted_and_the_proof_kept(self):
        from compare import coldcache, run_system

        reports = [{"pages": 100, "resident_pages": 0, "passes": 1, "proven": True,
                    "method": "fadvise"},
                   {"pages": 7, "resident_pages": 0, "passes": 2, "proven": True,
                    "method": "fadvise"}]
        with (mock.patch.object(coldcache, "evict", side_effect=reports) as evict,
              mock.patch.object(run_system, "sync_tree") as sync,
              mock.patch.object(coldcache, "meminfo_bytes",
                                side_effect=[1 << 30, 2 << 30, 8 << 30, 9 << 30])):
            found = self.runner().clear_page_cache(Path("store"), [Path("store/a")])
        sync.assert_called_once_with(Path("store"))
        self.assertEqual(evict.call_args_list,
                         [mock.call(None, [Path("day.itch"), Path("export/day-full")]),
                          mock.call(None, [Path("store/a")])])
        self.assertEqual((found["inputs"]["pages"], found["store_pages"],
                          found["resident_after"]), (100, 7, 0))
        self.assertEqual((found["inputs"]["mem_free_bytes_before"],
                          found["inputs"]["mem_free_bytes_after"]), (2 << 30, 8 << 30))
        self.assertEqual((found["mem_free_bytes_before"], found["mem_free_bytes_after"]),
                         (1 << 30, 9 << 30))

    def test_a_read_back_on_a_day_evicts_the_inputs_first(self):
        from compare import coldcache

        runner = self.runner()
        runner.full_day = True
        runner.windows = [(0, 1)]
        runner.args = SimpleNamespace(system="parquet")
        runner.layout = SimpleNamespace(work=Path("work"))
        order = []
        report = {"pages": 100, "resident_pages": 0, "passes": 1, "proven": True,
                  "method": "fadvise"}

        def evict(layout, paths):
            order.append("evict")
            return report

        def worker(system, step, **fields):
            order.append(step)
            return {"read_seconds": 1.0, "peak_rss_bytes": 1, "stored_settings": [],
                    "windows": 1, "rows": 5, "write_columns_seconds": 0.5, "method": "windows"}

        runner.worker = worker
        runner.verify = lambda directory: {"exit_code": 0, "verdict": "pass"}
        with (mock.patch.object(coldcache, "evict", side_effect=evict),
              mock.patch.object(coldcache, "meminfo_bytes", return_value=1 << 30),
              mock.patch("shutil.rmtree")):
            result = runner.read_back(SimpleNamespace())
        self.assertEqual(order, ["evict", "read_back"])
        self.assertEqual(result["streamed"]["inputs_evicted_before"]["pages"], 100)

    def test_an_eviction_not_proven_stops_the_run(self):
        from compare import coldcache, run_system

        with (mock.patch.object(coldcache, "evict",
                                side_effect=coldcache.ProofFailed("eviction not proven")),
              mock.patch.object(run_system, "sync_tree"),
              self.assertRaises(coldcache.ProofFailed)):
            self.runner().clear_page_cache(Path("store"), [Path("store/a")])


class ColdRuns(unittest.TestCase):
    """A cold repetition after which the store gained pages, with nothing of ours running, is
    not counted: something else was reading the store, so it may not have been cold."""

    def runs(self, refills, repetitions=3):
        from compare import run_system

        runner = run_system.Runner.__new__(run_system.Runner)
        runner.args = SimpleNamespace(system="questdb", cold_repetitions=repetitions)
        queue = list(refills)
        seen = []

        def repetition(system, root, paths, query, truth):
            seen.append(query.name)
            answer = {"seconds": 0.5 + len(seen), "engine_seconds": None, "open_seconds": None,
                      "query_seconds": 0.5 + len(seen),
                      "swap_timed": {"pages_in": 0, "pages_out": 0}}
            return answer, {"resident_after_eviction": 0}, queue.pop(0) if queue else 0

        runner.cold_repetition = repetition
        query = Query("Q2", "rows", OPEN_NS, OPEN_NS + NANOS_PER_SECOND, 7)
        found = runner.cold_runs("plain", None, Path("store"), [Path("store/a")], [query],
                                 {"Q2": {}})
        return found, seen

    def test_a_quiet_store_counts_every_repetition(self):
        (cold, _, _, proofs, again, _), seen = self.runs([0, 0, 0])
        self.assertEqual((len(cold["Q2"]), len(proofs["Q2"]), again["Q2"], len(seen)),
                         (3, 3, [], 3))

    def test_a_refilled_store_runs_the_repetition_again(self):
        (cold, _, swaps, proofs, again, _), seen = self.runs([0, 13_643, 0, 0])
        self.assertEqual(len(seen), 4)
        self.assertEqual(again["Q2"], [13_643])
        self.assertEqual(cold["Q2"], [1.5, 3.5, 4.5])
        self.assertEqual((len(proofs["Q2"]), len(swaps["Q2"])), (3, 3))

    def test_a_store_that_keeps_refilling_stops_the_run(self):
        from compare import run_system

        with self.assertRaisesRegex(run_system.StepFailed, "kept reading"):
            self.runs([1] * (run_system.COLD_RERUNS + 1))


class Assembly(unittest.TestCase):
    """run_system.py's part as it is put together from the workers' answers: the slice's
    warm-up seconds, tick-store's cold parts and the warm worker's open (review RF-9)."""

    class Phase:
        counts = {"pages_in": 0, "pages_out": 0}

        def start(self):
            return self

        def stop(self):
            return self

    WARM = {"queries": {"Q1": {"seconds": [1e-5, 2e-5], "digest": {"rows": 1},
                               "swap_timed": {"pages_in": 0, "pages_out": 0},
                               "warm_up_runs": 1, "warm_up_seconds": [5e-5],
                               "engine_seconds": [0.9e-5, 1.9e-5]}},
            "proof": {"pages": 8, "resident_before": 8, "resident_after": 8, "warm_up_runs": 1},
            "scan_facts": {"Q1": {}}, "floor_seconds": [1e-6, 1e-6], "peak_rss_bytes": 1,
            "open": {"seconds": 0.01, "reported_open_seconds": 0.002}}
    PARTS = {"Q1": {"open_seconds_by_repetition": [0.002, 0.003],
                    "query_seconds_by_repetition": [0.01, 0.02]}}

    def runner(self):
        from compare import run_system

        runner = run_system.Runner.__new__(run_system.Runner)
        runner.args = SimpleNamespace(system="tickstore", warm_repetitions=2,
                                      cold_repetitions=2, variants="v3-seg4Mi")
        runner.full_day = False
        runner.warm_proof = run_system.RESIDENCY
        runner.stores = Path("stores")
        runner.note = lambda text: None
        system = SimpleNamespace(data_paths=lambda: [Path("store")], start_service=lambda: None,
                                 stop_service=lambda: None, service_processes=lambda: {})
        runner.system_class = lambda context: system
        runner.context = lambda root, variant, durability: None
        runner.worker = lambda system, step, **fields: json.loads(json.dumps(self.WARM))
        swap_timed = {"pages_in": 0, "pages_out": 0}
        runner.cold_runs = lambda *args: (
            {"Q1": [0.012, 0.023]}, {"Q1": [0.008, 0.015]}, {"Q1": [swap_timed, swap_timed]},
            {"Q1": [{}, {}]}, {"Q1": []}, json.loads(json.dumps(self.PARTS)))
        runner.after_queries = lambda system: None
        return runner

    def test_a_query_keeps_its_warm_up_and_its_cold_parts(self):
        from compare import run_system

        runner = self.runner()
        with mock.patch.object(run_system.swap, "Phase", self.Phase):
            results, *_ = runner.run_queries("v3-seg4Mi", CLASS_B,
                                             [Query("Q1", "rows", 0, 1)], {})
        self.assertEqual(results["Q1"]["warm"]["warm_up_seconds"], [5e-5])
        parts = results["Q1"]["cold"]["parts"]
        self.assertEqual(parts["open_seconds_by_repetition"], [0.002, 0.003])
        self.assertEqual(parts["query_seconds_by_repetition"], [0.01, 0.02])
        self.assertEqual(runner.warm_open, self.WARM["open"])

    def test_a_cold_query_with_no_reported_open_has_no_parts(self):
        from compare import run_system

        runner = self.runner()
        self.PARTS = {"Q1": {"open_seconds_by_repetition": [None, None],
                             "query_seconds_by_repetition": [0.01, 0.02]}}
        with mock.patch.object(run_system.swap, "Phase", self.Phase):
            results, *_ = runner.run_queries("v3-seg4Mi", CLASS_B,
                                             [Query("Q1", "rows", 0, 1)], {})
        self.assertNotIn("parts", results["Q1"]["cold"])

    def test_each_variant_keeps_its_warm_workers_open(self):
        from compare import run_system

        runner = self.runner()
        runner.pin = {"message_count": 9}
        runner.memory_seen = []
        runner.sample_memory = lambda: None
        runner.verify_slice = lambda: None
        runner.columns_dir = Path("export")
        runner.window_end_ns = lambda: 1
        runner.system_class = SimpleNamespace(variants={"v3-seg4Mi": {}}, versions=lambda: {},
                                              versions_read=lambda facts: {},
                                              headline_variant="v3-seg4Mi")
        swap_counts = {"pages_in": 0, "pages_out": 0}

        def run_class(variant, durability, control):
            return {"swap": {"write": swap_counts, "read_back": swap_counts}, "facts": {}}, {
                "data_dirty_pages": 1}

        def run_queries(variant, durability, queries, truth):
            runner.warm_open = {"seconds": 0.01, "variant": variant}
            return {}, {}, 1, None, {"warm_queries": swap_counts, "cold_queries": swap_counts}

        runner.run_class = run_class
        runner.run_queries = run_queries
        columns = SimpleNamespace(manifest={"tail_bytes": 0}, records=9)
        with (mock.patch.object(run_system, "ColumnSet", return_value=columns),
              mock.patch.object(run_system, "build_queries", return_value=[]),
              mock.patch.object(run_system.shutil, "rmtree")):
            out = runner.run()
        self.assertEqual(out["variants"]["v3-seg4Mi"]["warm_worker_open"],
                         {"seconds": 0.01, "variant": "v3-seg4Mi"})


class WriteCountsItsServer(unittest.TestCase):
    """run_class hands the write's worker the processes of the system's service, so a macOS
    write counts the server's device reads beside the worker's (review RV-5)."""

    class Stop(Exception):
        pass

    def test_the_ingest_request_names_the_service_processes(self):
        from compare import run_system

        runner = run_system.Runner.__new__(run_system.Runner)
        runner.note = lambda text: None
        runner.stores = Path("stores")
        runner.columns_dir = Path("export")
        runner.write_repetitions = 1
        runner.flush_probe = lambda variant, durability: {}
        system = SimpleNamespace(start_service=lambda: None, service_settings=lambda: [],
                                 stop_service=lambda: {},
                                 service_processes=lambda: {"questdb": 42})
        runner.fresh = lambda root, variant, durability: system
        requests = []

        def worker(system, step, **fields):
            requests.append((step, fields))
            raise self.Stop

        runner.worker = worker
        with mock.patch.object(run_system.swap, "Phase", Assembly.Phase), \
                self.assertRaises(self.Stop):
            runner.run_class("plain", CLASS_B, None)
        self.assertEqual([step for step, _ in requests], ["ingest"])
        self.assertEqual(requests[0][1].get("service_processes"), {"questdb": 42})


class SecondSibling(unittest.TestCase):
    """run.py's run_one keeps cpu6's sibling reading beside cpu4's, and a part without it is
    run again (review RF-9)."""

    class Meter:
        def __init__(self, busy):
            self.busy = busy

        def snapshot_command(self, snapshot):
            return []

        def busy_percent(self, snapshot):
            return self.busy

    def run_one(self, parts, ran):
        args = compare_run.parse_arguments(["--slice", "s"])
        layout = SimpleNamespace(work=parts, as_arguments=lambda: [])

        def system(*args, **kwargs):
            # run_system.py writes the part.
            ran.append(1)
            (parts / "parquet.json").write_text("{}", encoding="utf-8")
            return SimpleNamespace(returncode=0)

        with (mock.patch.object(compare_run.run_bench, "host_label", return_value="Linux"),
              mock.patch.object(compare_run.subprocess, "run", system)):
            compare_run.run_one("parquet", args, layout, None, parts, self.Meter(0.5), None,
                                self.Meter(1.5))

    def test_the_second_siblings_reading_is_kept(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            parts, ran = Path(scratch), []
            self.run_one(parts, ran)
            self.assertEqual(json.loads((parts / "parquet.sibling-second").read_text()), 1.5)
            self.assertEqual(json.loads((parts / "parquet.sibling").read_text()), 0.5)
            self.run_one(parts, ran)
            self.assertEqual(len(ran), 1)  # Whole, so kept.
            (parts / "parquet.sibling-second").unlink()
            self.run_one(parts, ran)
            self.assertEqual(len(ran), 2)  # No second reading, so run again.


class GuardedRun(unittest.TestCase):
    """run.py's run_one refuses a system the guard stopped, even when the command exited 0, and
    keeps nothing that would let a resumed run reuse its part (review F-9)."""

    def test_a_run_the_guard_stopped_is_a_failure_and_leaves_no_load_lines(self):
        args = compare_run.parse_arguments(["--day", DAY_LABEL])

        def system(command, **kwargs):
            parts = Path(command[command.index("--part") + 1]).parent
            (parts / "parquet.json").write_text("{}", encoding="utf-8")
            report = command[command.index("--report") + 1]
            Path(report).write_text(json.dumps({"guard_fired": True,
                                                "guard_reason": "MemAvailable under 3.00 GiB"}),
                                    encoding="utf-8")
            return SimpleNamespace(returncode=0)

        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            parts = Path(scratch)
            layout = SimpleNamespace(work=parts, as_arguments=lambda: [])
            with (mock.patch.object(compare_run.run_bench, "host_label", return_value="Linux"),
                  mock.patch.object(compare_run.subprocess, "run", system),
                  self.assertRaisesRegex(compare_run.run_bench.Failure,
                                         "parquet: the guard stopped it: MemAvailable")):
                compare_run.run_one("parquet", args, layout, None, parts, None)
            self.assertFalse((parts / "parquet.load").exists())
            self.assertFalse((parts / "parquet.arguments.json").exists())


class QuestDBVersion(unittest.TestCase):
    """QuestDB's version comes from the running server's `SELECT build`, not from a literal
    (review F-12)."""

    BUILD = ("Build Information: QuestDB {}, JDK 25.0.2, Commit Hash "
             "e11c837f8a169b533540bd6fa57ef3d520b68ad9")

    def test_the_version_is_the_one_each_running_server_gave(self):
        from compare.systems.questdb_system import QuestDBSystem

        facts = [{"server_build": self.BUILD.format("10.2.0")}] * 2
        self.assertEqual(QuestDBSystem.versions_read(facts), {"questdb_server": "10.2.0"})
        self.assertNotIn("questdb_server", QuestDBSystem.versions())

    def test_two_readings_or_none_are_refused(self):
        from compare.systems.questdb_system import QuestDBSystem

        for facts in ([{"server_build": self.BUILD.format("10.1.0")},
                       {"server_build": self.BUILD.format("10.2.0")}],
                      [{"server_build": "QuestDB"}], []):
            with self.subTest(facts=facts), self.assertRaises(ServerFailure):
                QuestDBSystem.versions_read(facts)

    def test_a_part_carries_the_version_its_classes_read(self):
        from compare import run_system

        runner = run_system.Runner.__new__(run_system.Runner)
        variants = {"plain": {"classes": {
            "A": {"facts": {"server_build": self.BUILD.format("10.2.0")}},
            "B": {"facts": {"server_build": self.BUILD.format("10.2.0")}}}}}
        from compare.systems.questdb_system import QuestDBSystem
        runner.system_class = QuestDBSystem
        self.assertEqual(runner.versions(variants)["questdb_server"], "10.2.0")

    class WentOn(Exception):
        pass

    def test_an_unexpected_build_fails_the_first_class_when_its_facts_are_read(self):
        # Review RP-6: the version is parsed when the running server's facts are first read,
        # so a build string it cannot parse stops the first class there, not the whole part
        # after every variant and class has run.
        from compare import run_system
        from compare.systems.questdb_system import QuestDBSystem

        runner = run_system.Runner.__new__(run_system.Runner)
        runner.note = lambda text: None
        runner.stores = Path("stores")
        runner.columns_dir = Path("export")
        runner.write_repetitions = 1
        runner.full_day = False
        runner.flush_probe = lambda variant, durability: {}
        runner.system_class = QuestDBSystem
        stopped = []
        system = SimpleNamespace(start_service=lambda: None, service_settings=lambda: [],
                                 stop_service=lambda: stopped.append(True) or {},
                                 service_processes=lambda: {"questdb": 42},
                                 settle=lambda: {}, requested=lambda: {},
                                 service_facts=lambda: {"server_build": "Build Information: "
                                                                        "an unknown build"})
        runner.fresh = lambda root, variant, durability: system
        runner.worker = lambda system, step, **fields: {"timings": {}, "peak_rss_bytes": 1,
                                                        "settings": [], "flush": {}}

        def went_on(root):
            raise self.WentOn

        raised = None
        with mock.patch.object(run_system.swap, "Phase", Assembly.Phase), \
                mock.patch.object(run_system, "sync_tree", went_on):
            try:
                runner.run_class("plain", CLASS_B, None)
            except (ServerFailure, self.WentOn) as error:
                raised = error
        self.assertIsInstance(raised, ServerFailure)
        self.assertIn("no QuestDB version in the server's build", str(raised))
        self.assertEqual(stopped, [True])


class RuntimeReading(unittest.TestCase):
    """The reading on both hosts' output, whichever host runs this."""

    LOG = ("old line\nProperty settings:\n    java.vendor = Old\n\nserver lines\n"
           "Property settings:\n    java.class.path = \n    java.library.path = /a\n        /b\n"
           "    java.runtime.version = 25.0.5+8-LTS\n    java.vendor = Eclipse Adoptium\n"
           "    java.vendor.version = Temurin-25.0.5+8\n\nserver lines\n")
    MAPS = ("7f00-7f01 r-xp 00000000 08:01 12 /opt/q/bin/libjemalloc.so\n"
            "7f02-7f03 r-xp 00000000 08:01 13 /opt/q/lib/server/libjvm.so\n"
            "7f04-7f05 rw-p 00000000 00:00 0 \n7f06-7f07 rw-p 00000000 00:00 0 [heap]\n")
    LSOF = "p123\nfcwd\nn/some/dir\nftxt\nn/jdk/lib/server/libjvm.dylib\nn/jdk/bin/java\n"

    def test_the_last_start_is_the_one_read(self):
        properties = jvm_properties(self.LOG)
        self.assertEqual(properties["java.vendor"], "Eclipse Adoptium")
        self.assertEqual(properties["java.runtime.version"], "25.0.5+8-LTS")

    def test_a_log_with_no_properties_is_refused(self):
        with self.assertRaisesRegex(ServerFailure, "no properties"):
            jvm_properties("server lines\n")

    def test_linux_maps_show_jemalloc(self):
        reading = runtime_reading(jvm_properties(self.LOG),
                                  library_names(self.MAPS, darwin=False))
        self.assertIs(reading["jemalloc_preloaded"], True)

    def test_macos_lsof_shows_none(self):
        reading = runtime_reading(jvm_properties(self.LOG), library_names(self.LSOF, darwin=True))
        self.assertIs(reading["jemalloc_preloaded"], False)
        self.assertEqual(reading["jvm_vendor_version"], "Temurin-25.0.5+8")

    def test_an_empty_reading_is_not_no_jemalloc(self):
        with self.assertRaisesRegex(ServerFailure, "could not be read"):
            runtime_reading(jvm_properties(self.LOG), set())

    def test_the_header_takes_the_one_reading_of_every_class(self):
        def systems(*readings):
            classes = {str(index): {"facts": {"runtime": reading}}
                       for index, reading in enumerate(readings)}
            return {"questdb": {"variants": {"v": {"classes": classes}}}}

        self.assertEqual(compare_run.questdb_runtime(systems({"a": 1}, {"a": 1})), {"a": 1})
        self.assertEqual(compare_run.questdb_runtime({}), "not-run")
        with self.assertRaisesRegex(compare_run.run_bench.Failure, "one runtime"):
            compare_run.questdb_runtime(systems({"a": 1}, {"a": 2}))


class TailFigure(unittest.TestCase):
    """The nearest-rank 99th percentile of fewer than 100 runs is the slowest run, and every
    latency block says so, with its n and the rank used (review F-4)."""

    def test_the_slowest_of_7_is_named_the_slowest(self):
        from compare import run_system

        block = run_system.latency([1e-3] * 6 + [1.0])
        self.assertEqual((block["n"], block["tail_rank"], block["tail_is_slowest"]), (7, 7, True))
        self.assertEqual(block["tail"], "slowest of 7")
        self.assertEqual(block["tail_us"], block["max_us"])
        self.assertEqual(block["p99_us"], block["tail_us"])

    def test_the_slowest_of_30_is_named_the_slowest(self):
        from compare import run_system

        block = run_system.latency([i / 1000 for i in range(30)])
        self.assertEqual((block["tail_rank"], block["tail"]), (30, "slowest of 30"))

    def test_from_100_runs_the_tail_is_not_the_slowest(self):
        from compare import run_system

        block = run_system.latency([i / 1000 for i in range(200)])
        self.assertEqual((block["tail_rank"], block["tail_is_slowest"]), (198, False))
        self.assertEqual(block["tail"], "99th percentile, nearest rank: run 198 of 200")
        self.assertAlmostEqual(block["tail_us"], 197_000.0)

    def test_what_the_producer_writes_passes_the_validator(self):
        from compare import run_system

        for count in (2, 7, 30, 150):
            block = run_system.latency([i / 1000 + 0.001 for i in range(count)])
            self.assertEqual(check_compare.check_tail("x", block), [])

    def test_each_block_keeps_its_repetitions_in_the_order_they_ran(self):
        from compare import run_system

        seconds = [0.3, 0.1, 0.2]
        self.assertEqual(run_system.latency(seconds)["seconds_by_repetition"], seconds)

    def test_a_write_keeps_each_repetitions_times(self):
        from compare import run_system

        repetitions = [{"timings": {"seconds": total, "engine_seconds": engine,
                                    "python_seconds": total - engine, "python_note": "n"},
                        "peak_rss_bytes": 1, "service": {}}
                       for total, engine in ((3.0, 2.5), (2.0, 1.5), (4.0, 3.0))]
        write = run_system.summarise_writes(repetitions, {"message_count": 10, "size": 20}, {})
        self.assertEqual([item["engine_seconds"] for item in write["by_repetition"]],
                         [2.5, 1.5, 3.0])
        self.assertEqual(write["seconds"], [3.0, 2.0, 4.0])
        self.assertEqual(check_compare.check_write_repetitions("w", write), [])

    def test_the_method_and_the_rule_name_the_slowest_run(self):
        self.assertIn("slowest of 30", compare_run.TAIL_STATEMENT)
        self.assertIn("slowest of 7", compare_run.TAIL_STATEMENT)
        self.assertIn("slowest", variant_rule.RULE)
        self.assertEqual(variant_rule.FIELDS, ("median_us", "p99_us"))


class SharedRepetitions(unittest.TestCase):
    """The runner runs the counts the validator requires, from one module (review F-5)."""

    def test_the_runner_and_the_validator_count_from_one_place(self):
        from compare import repetitions, run_system

        args = run_system.parse_arguments(["--system", "parquet", "--part", "p"])
        self.assertEqual((args.warm_repetitions, args.cold_repetitions),
                         (repetitions.WARM, repetitions.COLD))
        self.assertEqual((run_system.WRITE_REPETITIONS, run_system.FULL_DAY_WRITE_REPETITIONS),
                         (repetitions.WRITE, repetitions.FULL_DAY_WRITE))
        self.assertIs(check_compare.repetitions, repetitions)


class ColdOpen(unittest.TestCase):
    """Review F-1: a cold figure holds the store's open where the system opens its store
    outside its query call, as tick-store does; and every cold answer records the store's
    resident bytes just before its clock."""

    def step(self, inside, reported):
        from compare import coldcache, worker

        readings = iter([{"pages": 9, "resident_pages": 2, "page_size": 4096}])

        system = SimpleNamespace(
            name="fake", open_inside_cold_clock=inside, read_paths=lambda: [Path("store")],
            reported_open_seconds=lambda: reported,
            query_column_sum=lambda column: [[1, 2]])
        query = Query("Q5", "column_sum", 0, 0)
        request = {"query": query_to_json(query), "truth": {"rows": 1, "sha256": "x"},
                   "store_before_open": {"pages": 9, "resident_pages": 0, "page_size": 4096}}
        with (mock.patch.object(coldcache, "residency", lambda layout, paths: next(readings)),
              mock.patch.object(worker, "timed_query", return_value=(0.5, [[1, 2]])),
              mock.patch.object(worker, "digest_result", return_value={"rows": 1, "sha256": "x"})):
            return worker.step_cold_query(system, request, None)

    def test_an_open_outside_the_query_call_is_added_to_the_figure(self):
        answer = self.step(True, 0.25)
        self.assertEqual((answer["seconds"], answer["open_seconds"], answer["query_seconds"]),
                         (0.75, 0.25, 0.5))
        self.assertEqual(answer["resident_bytes_before_clock"], 0)
        self.assertEqual(answer["resident_bytes_before_query"], 2 * 4096)

    def test_a_system_opened_before_its_clock_counts_what_its_open_read(self):
        answer = self.step(False, None)
        self.assertEqual((answer["seconds"], answer["open_seconds"]), (0.5, None))
        self.assertEqual(answer["resident_bytes_before_clock"], 2 * 4096)
        self.assertIs(answer["open_inside_clock"], False)

    def test_the_proof_keeps_it(self):
        from compare import coldcache, run_system

        runner = run_system.Runner.__new__(run_system.Runner)
        runner.layout = None
        answer = {"resident_bytes_before_clock": 0, "resident_bytes_before_open": 0,
                  "resident_bytes_before_query": 4096, "open_inside_clock": True}
        runner.worker = lambda system, step, **fields: answer
        system = SimpleNamespace(start_service=lambda: None, stop_service=lambda: None,
                                 read_paths=lambda: [Path("store")])
        evicted = {"pages": 9, "resident_pages": 0, "passes": 1, "method": "m"}
        after = {"pages": 9, "resident_pages": 3, "page_size": 4096}

        class Counter:
            def __init__(self, path):
                pass

            def delta(self):
                return {"bytes": 1 << 20, "counter": "c", "slack": 0}

        with (mock.patch.object(run_system, "sync_tree"),
              mock.patch.object(coldcache, "evict", return_value=evicted),
              mock.patch.object(coldcache, "residency", return_value=after),
              mock.patch.object(coldcache, "DeviceCounter", Counter),
              mock.patch.object(coldcache, "refilled_pages", return_value=0)):
            _, proof, _ = runner.cold_repetition(system, Path("store"), [Path("store")],
                                                 Query("Q5", "column_sum", 0, 0), {})
        self.assertEqual((proof["resident_bytes_before_clock"], proof["open_inside_clock"]),
                         (0, True))
        self.assertEqual(proof["resident_bytes_before_query"], 4096)

    def test_each_system_says_whether_its_whole_open_is_inside_its_clock(self):
        from compare.systems import system_class

        self.assertEqual({name: system_class(name).open_inside_cold_clock
                          for name in ("parquet", "arcticdb", "questdb", "tickstore")},
                         {"parquet": True, "arcticdb": False, "questdb": False,
                          "tickstore": True})
        self.assertEqual(check_compare.OPEN_INSIDE_COLD_CLOCK,
                         {"parquet": True, "arcticdb": False, "questdb": False,
                          "tickstore": True})


class WriteRecord(unittest.TestCase):
    """Every write records its total and engine-only seconds, how much of its input was
    resident when its clock started, and the device bytes read while it ran (review F-2)."""

    def run_step(self, full_day=False, name="fake", resident=None):
        from compare import coldcache, worker

        # Whole on the slice, as every input must be there; part of a day's export may be out.
        resident = (7 if full_day else 10) if resident is None else resident

        order = []

        class Counter:
            def __init__(self, path):
                order.append(("counter", str(path)))

            def delta(self):
                order.append("delta")
                return {"bytes": 8192, "counter": "sectors read", "slack": 0}

        def residency(layout, paths):
            order.append(("residency", [str(path) for path in paths]))
            return {"pages": 10, "resident_pages": resident, "page_size": 4096}

        def ingest(*args):
            order.append("ingest")
            return {"seconds": 3.0, "engine_seconds": 2.0, "python_seconds": 0.5,
                    "python_note": "n"}

        system = SimpleNamespace(
            name=name, context=SimpleNamespace(day=DAY), settings_in_force=lambda: [],
            settings_after_ingest=lambda: [], input_paths=lambda columns: [Path("export")],
            ready_input=lambda columns: order.append("ready") or {"warms": 1},
            ingest=ingest, ingest_day=ingest)
        columns = SimpleNamespace(directory=Path("export"), records=9)
        # Linux's counter, the block device's: macOS has its own (review RF-5), below.
        with (mock.patch.object(worker, "ColumnSet", return_value=columns),
              mock.patch.object(worker, "arrow_table", return_value=SimpleNamespace(num_rows=9)),
              mock.patch.object(coldcache, "residency", residency),
              mock.patch.object(coldcache.platform, "system", return_value="Linux"),
              mock.patch.object(coldcache, "DeviceCounter", Counter)):
            try:
                answer = worker.step_ingest(system, {"columns": "export", "full_day": full_day},
                                            None)
            except coldcache.ProofFailed as failure:
                return failure, order
        return answer["timings"], order

    def test_each_write_records_both_times_its_input_and_its_device_reads(self):
        for full_day in (False, True):
            timings, order = self.run_step(full_day)
            self.assertEqual(timings["engine_only_seconds"], 2.5)
            self.assertEqual(timings["input_at_clock_start"]["pages"], 10)
            self.assertEqual(timings["input_at_clock_start"]["resident_pages"],
                             7 if full_day else 10)
            self.assertEqual(timings["input_at_clock_start"]["made_ready"], {"warms": 1})
            self.assertEqual(timings["device_read"]["bytes"], 8192)

    def test_an_input_not_whole_where_it_must_be_stops_the_write_before_its_clock(self):
        # Review RF-11: refused in the worker, not hours later at publish.
        from compare import coldcache

        for full_day, name in ((False, "fake"), (False, "tickstore"), (True, "tickstore")):
            with self.subTest(full_day=full_day, name=name):
                failure, order = self.run_step(full_day, name, resident=7)
                self.assertIsInstance(failure, coldcache.ProofFailed)
                self.assertIn(f"{name}: 7 of its input's 10 pages were resident", str(failure))
                self.assertNotIn("ingest", order)

    def test_a_days_export_may_be_partly_out_of_the_cache(self):
        # Review F-2: the others read a day's export as the cache holds it.
        timings, _ = self.run_step(True, "fake", resident=7)
        self.assertEqual(timings["input_at_clock_start"]["resident_pages"], 7)

    def test_an_input_on_no_block_device_of_its_own_is_named(self):
        # Review RF-11: tmpfs, overlayfs or a btrfs subvolume have major 0 and no stat file.
        import os

        from compare import coldcache

        real_stat = os.stat

        def stat(path, *args, **kwargs):
            if str(path).startswith("input/"):
                return SimpleNamespace(st_dev=os.makedev(0, 77))
            return real_stat(path, *args, **kwargs)

        with (mock.patch.object(coldcache.os, "stat", stat),
              self.assertRaises(coldcache.ProofFailed) as caught):
            coldcache._block_device_stat(Path("input/12302019.NASDAQ_ITCH50"))
        self.assertIn("input/12302019.NASDAQ_ITCH50 is on device 0:77, which has no "
                      "/sys/dev/block/0:77/stat", str(caught.exception))

    def test_the_input_is_made_ready_and_read_before_the_clock_and_the_device_after(self):
        _, order = self.run_step()
        self.assertEqual(order, ["ready", ("residency", ["export"]), ("counter", "export"),
                                 "ingest", "delta"])

    def test_a_macos_write_counts_the_reads_of_the_processes_that_did_it(self):
        # Review RF-5: iostat's disk0 total counts the write's own output too, so it is no count
        # of reads. macOS gives each process's own: the worker's, a server's and a child's.
        import os

        from compare import coldcache, worker

        counts = {os.getpid(): 1000, 42: 5000}

        def disk_bytes_read(pid):
            return counts[pid]

        def ingest(*args):
            counts[os.getpid()] += 4096
            counts[42] += 8192
            return {"seconds": 3.0, "engine_seconds": 2.0, "python_seconds": 0.5,
                    "python_note": "n"}

        system = SimpleNamespace(
            name="fake", context=SimpleNamespace(day=DAY), settings_in_force=lambda: [],
            settings_after_ingest=lambda: [], input_paths=lambda columns: [Path("export")],
            ready_input=lambda columns: {}, ingest=ingest, ingest_day=ingest)
        columns = SimpleNamespace(directory=Path("export"), records=9)
        with (mock.patch.object(worker, "ColumnSet", return_value=columns),
              mock.patch.object(worker, "arrow_table", return_value=SimpleNamespace(num_rows=9)),
              mock.patch.object(coldcache, "residency",
                                lambda layout, paths: {"pages": 1, "resident_pages": 1}),
              mock.patch.object(coldcache.platform, "system", return_value="Darwin"),
              mock.patch.object(coldcache, "disk_bytes_read", disk_bytes_read, create=True)):
            answer = worker.step_ingest(system, {"columns": "export",
                                                 "service_processes": {"questdb": 42}}, None)
        read = answer["timings"]["device_read"]
        self.assertIn("ri_diskio_bytesread", read["counter"])
        self.assertEqual(read["bytes"], 12288)
        self.assertEqual(read["by_process"],
                         {"worker": 4096, "questdb": 8192, "waited children": 0})
        self.assertEqual(read["slack"], 0)

    def test_the_write_block_keeps_them_for_each_repetition(self):
        from compare import run_system

        repetitions = [{"timings": {"seconds": total, "engine_seconds": total - 1,
                                    "python_seconds": 0.5, "engine_only_seconds": total - 0.5,
                                    "python_note": "n",
                                    "input_at_clock_start": {"pages": 10, "resident_pages": 10},
                                    "device_read": {"bytes": 4096, "counter": "c"}},
                        "peak_rss_bytes": 1, "service": {}} for total in (3.0, 2.0, 4.0)]
        write = run_system.summarise_writes(repetitions, {"message_count": 10, "size": 20},
                                            {"seconds": "s", "engine_only_seconds": "e"})
        self.assertEqual(write["engine_only_seconds_median"], 2.5)
        self.assertEqual(write["by_repetition"][2]["input_resident_pages_at_start"], 10)
        self.assertEqual(write["by_repetition"][0]["device_bytes_read"], 4096)
        self.assertEqual(write["times"], {"seconds": "s", "engine_only_seconds": "e"})
        self.assertEqual(check_compare.check_write_record("w", write, "parquet"), [])

    def test_a_macos_write_keeps_each_repetitions_reads_by_process(self):
        # Review RV-7: the reads of each process that did the write, repetition by repetition,
        # as the validator holds them, and the counter by its exact statement.
        from compare import coldcache, run_system

        counted = [{"worker": 4096, "waited children": 8192}, {"worker": 0,
                                                               "waited children": 4096}]
        repetitions = [{"timings": {"seconds": 2.0, "engine_seconds": 1.0, "python_seconds": 0.5,
                                    "engine_only_seconds": 1.5, "python_note": "n",
                                    "input_at_clock_start": {"pages": 10, "resident_pages": 10},
                                    "device_read": {"bytes": sum(by.values()), "by_process": by,
                                                    "counter": coldcache.ProcessDiskReads.COUNTER}},
                        "peak_rss_bytes": 1, "service": {}} for by in counted]
        write = run_system.summarise_writes(repetitions, {"message_count": 10, "size": 20},
                                            {"seconds": "s", "engine_only_seconds": "e"})
        self.assertEqual([item["device_read_by_process"] for item in write["by_repetition"]],
                         counted)
        self.assertEqual(check_compare.check_write_record("w", write, "parquet", host="macOS"),
                         [])
        self.assertEqual(check_compare.MACOS_WRITE_COUNTER, coldcache.ProcessDiskReads.COUNTER)

    def test_every_system_says_what_its_two_times_hold(self):
        from compare.systems import SYSTEM_NAMES, system_class

        for name in SYSTEM_NAMES:
            times = system_class(name).write_times(False)
            self.assertEqual(sorted(times), ["engine_only_seconds", "seconds"], name)
            self.assertEqual(sorted(system_class(name).write_times(True)),
                             ["engine_only_seconds", "seconds"], name)

    def test_the_flush_report_is_read_before_anything_else_runs(self):
        # Review F-11: tick-store's settings_after_ingest runs its tools for seconds, time in
        # which writeback cleans pages, so the dirty pages are read first, for every system.
        from compare import coldcache, worker

        order = []

        def ingest(*args):
            order.append("ingest")
            return {"seconds": 3.0, "engine_seconds": 2.0, "python_seconds": 0.5,
                    "python_note": "n"}

        def dirty_by_file(layout, paths):
            order.append("dirty")
            return {"store": 0}

        class Counter:
            def __init__(self, path):
                pass

            def delta(self):
                order.append("delta")
                return {"bytes": 0, "counter": "c"}

        system = SimpleNamespace(
            name="fake", context=SimpleNamespace(day=DAY), settings_in_force=lambda: [],
            settings_after_ingest=lambda: order.append("settings_after_ingest") or [],
            input_paths=lambda columns: [Path("export")], ready_input=lambda columns: {},
            ingest=ingest, ingest_day=ingest, dirty_paths=lambda: [Path("store")],
            data_paths=lambda: [Path("store")], flush_exempt=(), flush_facts=lambda: {})
        columns = SimpleNamespace(directory=Path("export"), records=9)
        with (mock.patch.object(worker, "ColumnSet", return_value=columns),
              mock.patch.object(worker, "arrow_table", return_value=SimpleNamespace(num_rows=9)),
              mock.patch.object(coldcache, "residency",
                                lambda layout, paths: {"pages": 1, "resident_pages": 1}),
              mock.patch.object(coldcache.platform, "system", return_value="Linux"),
              mock.patch.object(coldcache, "DeviceCounter", Counter),
              mock.patch.object(coldcache, "dirty_by_file", dirty_by_file)):
            answer = worker.step_ingest(system, {"columns": "export", "flush_report": True},
                                        None)
        self.assertEqual(order, ["ingest", "delta", "dirty", "settings_after_ingest"])
        self.assertEqual(answer["flush"]["data_dirty_pages"], 0)


class SliceExportResident(unittest.TestCase):
    """On the slice every system's write reads the export, which is read through and proven
    wholly resident before the write's clock, as tick-store's source is (review F-10)."""

    def ready(self, full_day, resident=4):
        from compare import coldcache
        from compare.systems.base import System

        calls = []

        def warm(layout, paths):
            calls.append([str(path) for path in paths])
            return {"pages": 4, "resident_pages": resident}

        owner = SimpleNamespace(context=SimpleNamespace(layout="layout", full_day=full_day))
        columns = SimpleNamespace(directory=Path("export"))
        with mock.patch.object(coldcache, "warm", warm):
            return System.ready_input(owner, columns), calls

    def test_the_slice_export_is_read_through_and_recorded(self):
        record, calls = self.ready(False)
        self.assertEqual(calls, [["export"]])
        self.assertEqual((record["pages"], record["resident_pages"], record["warms"]), (4, 4, 1))

    def test_a_slice_export_left_short_stops_the_write(self):
        from compare import coldcache

        with self.assertRaisesRegex(coldcache.ProofFailed, "3 of its 4 pages"):
            self.ready(False, resident=3)

    def test_a_day_export_is_taken_as_the_cache_holds_it(self):
        record, calls = self.ready(True)
        self.assertEqual(calls, [])
        self.assertIn("nothing", record["what"])


class Statements(unittest.TestCase):
    """Every sentence compare/run.py writes must be safe to publish."""

    def test_no_statement_trips_the_leak_check(self):
        for text in (compare_run.COLD_INCLUDES, compare_run.LINUX_HEAP, compare_run.MACOS_HEAP,
                     runtime_reading(jvm_properties(RuntimeReading.LOG), {"libjvm.so"})):
            self.assertEqual(check_compare.check_results.check_leaks({"text": text}), [])


class SiblingCommand(unittest.TestCase):
    """The reading goes inside the pin, so the wrapper still sees `taskset -c 4` first."""

    class Meter:
        @staticmethod
        def snapshot_command(snapshot):
            return ["sh", snapshot]

    def test_linux_under_the_wrapper(self):
        self.assertEqual(compare_run.wrapped_command("wrapper", self.Meter, "stat", ["python"]),
                         ["wrapper", "taskset", "-c", "4", "sh", "stat", "python"])

    def test_linux_with_no_wrapper_is_unpinned_as_before_and_still_read(self):
        self.assertEqual(compare_run.wrapped_command(None, self.Meter, "stat", ["python"]),
                         ["sh", "stat", "python"])

    def test_macos_has_neither(self):
        self.assertEqual(compare_run.wrapped_command("wrapper", None, "stat", ["python"]),
                         ["wrapper", "python"])
        self.assertEqual(compare_run.sibling_block(None, None, ["parquet"]), "not-available")

    def test_each_system_is_a_case(self):
        class Meter:
            @staticmethod
            def block(cases):
                return cases

        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            (Path(scratch) / "parquet.sibling").write_text("0.5", encoding="utf-8")
            self.assertEqual(compare_run.sibling_block(Meter, Path(scratch), ["parquet"]),
                             {"parquet": 0.5})


class Document(unittest.TestCase):
    """compare/run.py's header and heap, built from parts as a real run leaves them."""

    RUNTIME = {"jvm_vendor": "v", "jvm_version": "25", "jvm_read_from": "the JVM",
               "jemalloc_preloaded": True, "jemalloc_read_from": "the process"}

    GUARD = {"processes": {"11 python3": {"VmHWM": 5}, "12 java": {"VmHWM": 9},
                           "13 python3": {"VmHWM": 7}},
             "session_rss_bytes_peak": 20, "mem_available_lowest_bytes": 3,
             "mem_available_start_bytes": 8, "swap_in_pages_during_run": 0,
             "swap_out_pages_during_run": 5, "swap_in_bytes_per_second_peak": 0.0,
             "swap_out_bytes_per_second_peak": 4096.0, "swap_total_bytes": 1 << 30,
             "limits": {"max_swap_in_bytes_per_second": 10 << 20},
             "free_disk_lowest_bytes": 100, "guard_fired": False,
             "exit": {"code": 0, "signal": None}}

    def build(self, host, meter, parts, day=None, second=None):
        systems = ("parquet", "questdb")
        for name in systems:
            classes = {"A": {"facts": {"runtime": self.RUNTIME}}} if name == "questdb" else {}
            part = {"truth": {}, "queries": [], "variants": {"v": {"classes": classes}}}
            if day:
                part["full_day"] = {"clipped": False}
                (parts / f"{name}.guard.json").write_text(json.dumps(self.GUARD),
                                                          encoding="utf-8")
            (parts / f"{name}.json").write_text(json.dumps(part), encoding="utf-8")
            (parts / f"{name}.sibling").write_text("0.5", encoding="utf-8")
            (parts / f"{name}.sibling-second").write_text("0.25", encoding="utf-8")
        args = SimpleNamespace(systems=systems, slice="s", day=day, full_day_smoke=False,
                               tickstore_alone=False)
        pin = {"sha256": "ab", "message_count": 1, "size": 1}
        layout = SimpleNamespace(export_dir=lambda name: name)
        run_bench = compare_run.run_bench
        with (mock.patch.object(compare_run, "compiler_facts", return_value={}),
              mock.patch.object(compare_run, "read_manifest", return_value={}),
              mock.patch.object(run_bench, "cpu_model", return_value="cpu"),
              mock.patch.object(run_bench, "linux_turbo", return_value="enabled")):
            return compare_run.build_document(host, args, layout, pin, "c" * 40, "wrapper", parts,
                                              [[], []], meter, second_meter=second)

    def test_each_host(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            parts = Path(scratch)
            topology = parts / "cpu" / "cpu4" / "topology"
            topology.mkdir(parents=True)
            (topology / "thread_siblings_list").write_text("4-5\n", encoding="ascii")
            meter = compare_run.run_bench.SiblingMeter(parts / "cpu", parts / "stat")
            second_topology = parts / "cpu" / "cpu6" / "topology"
            second_topology.mkdir(parents=True)
            (second_topology / "thread_siblings_list").write_text("6-7\n", encoding="ascii")
            second = compare_run.run_bench.SiblingMeter(parts / "cpu", parts / "stat",
                                                        core=compare_run.SECOND_CPU)
            linux = self.build("Linux", meter, parts, second=second)
            macos = self.build("macOS", None, parts)
        for document in (linux, macos):
            self.assertEqual(document["header"]["questdb_runtime"], self.RUNTIME)
            self.assertEqual(check_compare.check_runtime(document["header"]), [])
            self.assertEqual(document["method"]["cold_includes"], compare_run.COLD_INCLUDES)
        self.assertEqual(linux["heap"], compare_run.LINUX_HEAP)
        self.assertEqual(macos["heap"], compare_run.MACOS_HEAP)
        self.assertEqual(linux["header"]["sibling_busy_percent"]["cases"],
                         {"parquet": 0.5, "questdb": 0.5})
        self.assertEqual(check_compare.check_results.check_sibling(
            linux["header"], ["parquet", "questdb"]), [])
        self.assertEqual(macos["header"]["sibling_busy_percent"], "not-available")
        block = linux["header"]["second_sibling_busy_percent"]
        self.assertEqual((block["core"], block["sibling"], block["cases"]),
                         (6, 7, {"parquet": 0.25, "questdb": 0.25}))
        self.assertEqual(check_compare.check_results.check_sibling(
            linux["header"], ["parquet", "questdb"], check_compare.SECOND_SIBLING,
            compare_run.SECOND_CPU), [])
        self.assertEqual(macos["header"]["second_sibling_busy_percent"], "not-available")

    def test_a_full_day_states_its_day_and_each_system_its_peak_memory(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            parts = Path(scratch)
            topology = parts / "cpu" / "cpu4" / "topology"
            topology.mkdir(parents=True)
            (topology / "thread_siblings_list").write_text("4-5\n", encoding="ascii")
            meter = compare_run.run_bench.SiblingMeter(parts / "cpu", parts / "stat")
            document = self.build("Linux", meter, parts, day="12302019")
        self.assertEqual(document["label"], compare_run.FULL_DAY_LABEL)
        self.assertEqual(document["input"]["day"], "12302019")
        self.assertNotIn("slice", document["input"])
        self.assertEqual(document["queries"]["full_day"], {"clipped": False})
        memory = document["systems"]["questdb"]["peak_memory"]
        self.assertEqual(memory["vmhwm_bytes_by_process"], {"java": 9, "python3": 7})
        self.assertEqual(memory["mem_available_lowest_bytes"], 3)


class TickstoreOnlyDocument(unittest.TestCase):
    """tick-store alone on a day off Linux, labelled so, with its peak memory from time -l."""

    TIME = ("     1800.25 real      1500.00 user       200.00 sys\n"
            "  12884901888  maximum resident set size\n"
            "   2147483648  peak memory footprint\n")

    def build(self, parts):
        part = {"truth": {}, "queries": [], "full_day": {"clipped": False},
                "headline_variant": "v3-seg4Mi", "variants": {"v3-seg4Mi": {}, "v3-seg1Mi": {}}}
        (parts / "tickstore.json").write_text(json.dumps(part), encoding="utf-8")
        (parts / "tickstore.time.json").write_text(
            json.dumps(store_results.parse_time(self.TIME)), encoding="utf-8")
        args = SimpleNamespace(systems=["tickstore"], slice="s", day="12302019",
                               full_day_smoke=False, tickstore_alone=True)
        pin = {"sha256": "ab", "message_count": 1, "size": 1}
        layout = SimpleNamespace(export_dir=lambda name: name)
        run_bench = compare_run.run_bench
        with (mock.patch.object(compare_run, "compiler_facts", return_value={}),
              mock.patch.object(compare_run, "read_manifest", return_value={}),
              mock.patch.object(run_bench, "cpu_model", return_value="cpu")):
            return compare_run.build_document("macOS", args, layout, pin, "c" * 40, "wrapper",
                                              parts, [[]], None, variant_rule.tickstore_only())

    def test_the_document_says_tickstore_alone_and_passes_its_rules(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            document = self.build(Path(scratch))
        self.assertEqual(document["scope"], variant_rule.TICKSTORE_ONLY)
        self.assertEqual(document["label"], compare_run.TICKSTORE_ONLY_LABEL)
        self.assertEqual(document["heap"], compare_run.TICKSTORE_ONLY_HEAP)
        self.assertEqual(document["header"]["questdb_runtime"], "not-run")
        self.assertEqual(document["method"]["warm"], compare_run.TICKSTORE_ONLY_WARM)
        self.assertNotIn("guard", document["method"])
        self.assertEqual(document["input"]["systems_run"], ["tickstore"])
        memory = document["systems"]["tickstore"]["peak_memory"]
        self.assertEqual(memory["maximum_resident_set_size_bytes"], 12884901888)
        self.assertEqual(memory["peak_memory_footprint_bytes"], 2147483648)
        self.assertEqual(check_compare.check_time_memory("tickstore", memory), [])
        self.assertEqual(check_compare.check_tickstore_only(document, Path("x.json")), [])

    def test_tickstore_alone_is_asked_for_and_never_a_side_effect(self):
        # Review F-5: no longer the host and two flags, on either host.
        args = compare_run.parse_arguments(["--day", "12302019", "--systems", "tickstore"])
        self.assertFalse(compare_run.is_tickstore_only(args))
        args = compare_run.parse_arguments(["--day", "12302019", "--tickstore-alone"])
        self.assertTrue(compare_run.is_tickstore_only(args))

    def test_no_statement_trips_the_leak_check(self):
        for text in (compare_run.TICKSTORE_ONLY_LABEL, compare_run.TICKSTORE_ONLY_HEAP,
                     compare_run.TICKSTORE_ONLY_WARM, compare_run.TICKSTORE_ONLY_MEMORY,
                     compare_run.TICKSTORE_ONLY_INPUT_CACHE, variant_rule.tickstore_only()):
            self.assertEqual(check_compare.check_results.check_leaks({"text": text}), [])


class FakeDay:
    """compare/run.py's run() on a day, every system's run faked as its part file, and the
    document it would publish kept: the harness's own fakes, no system runs (review F-5)."""

    GUARD = Document.GUARD
    TIME = TickstoreOnlyDocument.TIME

    class Meter:
        def __init__(self, core=4):
            self.core = core

        def block(self, cases):
            return {"core": self.core, "sibling": self.core + 1, "cases": cases}

    def __init__(self, case, scratch, host, measure="wrapper", turbo="enabled", stop_at=None):
        self.case, self.scratch, self.host, self.measure = case, Path(scratch), host, measure
        self.ran = []
        self.published = []
        # What cpufreq boost reads, and every reading and system run in the order they came.
        self.turbo, self.events = turbo, []
        # A system whose run the wrapper sends back with exit 75, as the unattended runner sees.
        self.stop_at = stop_at

    def read_turbo(self, *_):
        self.events.append(("turbo", self.turbo))
        return self.turbo

    def run_one(self, system, args, layout, measure, parts, meter, variants=None,
                second_meter=None):
        self.events.append(("run", system))
        if system == self.stop_at:
            raise compare_run.run_bench.Failure(compare_run.run_bench.EXIT_RETRY_LATER,
                                                "retry later")
        self.ran.append((system, variants))
        part = {"truth": {}, "queries": [], "full_day": {"clipped": False},
                "headline_variant": "v3-seg4Mi",
                "variants": {name: {} for name in variants or ["v"]}}
        (parts / f"{system}.json").write_text(json.dumps(part), encoding="utf-8")
        if self.host == "Linux":
            (parts / f"{system}.guard.json").write_text(json.dumps(self.GUARD), encoding="utf-8")
            for suffix in ("sibling", "sibling-second"):
                (parts / f"{system}.{suffix}").write_text("0.5", encoding="utf-8")
        else:
            (parts / f"{system}.time.json").write_text(
                json.dumps(store_results.parse_time(self.TIME)), encoding="utf-8")
        return []

    def run(self, argv):
        run_bench = compare_run.run_bench
        layout = SimpleNamespace(work=self.scratch, export_dir=lambda name: name)
        environment = {run_bench.MEASURE_ENV: self.measure} if self.measure else {}
        with (mock.patch.dict(compare_run.os.environ, environment, clear=False),
              mock.patch.object(run_bench, "host_label", return_value=self.host),
              mock.patch.object(run_bench, "current_commit", return_value="c" * 40),
              mock.patch.object(run_bench, "SiblingMeter", self.Meter),
              mock.patch.object(run_bench, "cpu_model", return_value="cpu"),
              mock.patch.object(run_bench, "linux_turbo", self.read_turbo),
              mock.patch.object(run_bench, "governor_of", return_value="performance"),
              mock.patch.object(compare_run, "require_clean_tree"),
              mock.patch.object(compare_run.Layout, "from_arguments", return_value=layout),
              mock.patch.object(compare_run, "compiler_facts", return_value={}),
              mock.patch.object(compare_run, "read_manifest", return_value={}),
              mock.patch.object(compare_run, "run_one", self.run_one),
              mock.patch.object(compare_run, "publish",
                                lambda document, output, measure:
                                self.published.append((document, output)))):
            if not self.measure:
                compare_run.os.environ.pop(run_bench.MEASURE_ENV, None)
            return compare_run.main(argv)


class MeasuredRun(unittest.TestCase):
    """A measured run runs the protocol whole: it narrows nothing (review F-5)."""

    def test_a_measured_run_refuses_to_narrow_anything(self):
        for argv in (["--warm-repetitions", "5"], ["--cold-repetitions", "3"],
                     ["--write-repetitions", "1"], ["--variants", "plain"],
                     ["--systems", "parquet", "questdb"],
                     ["--day", "12302019", "--systems", "tickstore", "--variants",
                      "v3-seg4Mi,v3-seg1Mi"]):
            with self.subTest(argv=argv), tempfile.TemporaryDirectory(
                    dir=REPO_ROOT / "build") as scratch:
                day = FakeDay(self, scratch, "Linux")
                self.assertEqual(day.run(argv), compare_run.run_bench.EXIT_USAGE)
                self.assertEqual((day.ran, day.published), ([], []))

    def test_an_iteration_run_may_narrow(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            day = FakeDay(self, scratch, "Linux", measure=None)
            day.run(["--warm-repetitions", "5", "--systems", "parquet", "--variants", "v"])
        self.assertEqual(day.ran, [("parquet", ["v"])])


class TurboReadings(unittest.TestCase):
    """cpufreq boost is read before the first system's part and after the last, and stated only
    if the two agree, as run_bench.py does for a benchmark (review E-10's leftover). The first
    reading is kept with the work folder, so a run started again after the wrapper's exit 75
    keeps it."""

    SYSTEMS = ["parquet", "tickstore"]
    ARGV = ["--systems", *SYSTEMS]

    def test_turbo_is_read_before_the_first_system_and_after_the_last(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            day = FakeDay(self, scratch, "Linux", measure=None)
            self.assertEqual(day.run(self.ARGV), 0)
        self.assertEqual(day.events, [("turbo", "enabled"),
                                      *[("run", system) for system in self.SYSTEMS],
                                      ("turbo", "enabled")])
        (document, _), = day.published
        header = document["header"]
        self.assertEqual(header["turbo"], "enabled")
        self.assertEqual(header["turbo_readings"], {"before_first_system": "enabled",
                                                    "after_last_system": "enabled"})
        self.assertEqual(check_compare.check_turbo(header, check_compare.PROTOCOL_2), [])

    def test_a_restarted_run_keeps_its_first_turbo_reading(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            first = FakeDay(self, scratch, "Linux", measure=None, stop_at="tickstore")
            self.assertEqual(first.run(self.ARGV), compare_run.run_bench.EXIT_RETRY_LATER)
            again = FakeDay(self, scratch, "Linux", measure=None, turbo="disabled")
            self.assertEqual(again.run(self.ARGV), 0)
        self.assertEqual(first.events[0], ("turbo", "enabled"))
        # Started again, it reads boost only after the last system: the first reading stands.
        self.assertEqual(again.events, [*[("run", system) for system in self.SYSTEMS],
                                        ("turbo", "disabled")])
        (document, _), = again.published
        header = document["header"]
        self.assertEqual(header["turbo_readings"], {"before_first_system": "enabled",
                                                    "after_last_system": "disabled"})
        self.assertTrue(header["turbo"].startswith("unavailable: "), header["turbo"])
        self.assertIn("header.turbo_readings: cpufreq boost read 'enabled' before the first "
                      "system and 'disabled' after the last",
                      check_compare.check_turbo(header, check_compare.PROTOCOL_2))

    def test_a_run_with_other_arguments_reads_turbo_again(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            first = FakeDay(self, scratch, "Linux", measure=None)
            self.assertEqual(first.run([*self.ARGV, "--warm-repetitions", "5"]), 0)
            again = FakeDay(self, scratch, "Linux", measure=None, turbo="disabled")
            self.assertEqual(again.run(self.ARGV), 0)
        self.assertEqual(again.events[0], ("turbo", "disabled"))
        (document, _), = again.published
        self.assertEqual(document["header"]["turbo"], "disabled")

    def test_macos_reads_no_turbo(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            day = FakeDay(self, scratch, "macOS", measure=None)
            self.assertEqual(day.run(self.ARGV), 0)
        self.assertNotIn("turbo", [event for event, _ in day.events])
        (document, _), = day.published
        self.assertEqual((document["header"]["turbo"], document["header"]["turbo_readings"]),
                         ("not-available", "not-available"))
        self.assertEqual(check_compare.check_turbo(document["header"],
                                                   check_compare.PROTOCOL_2), [])


class TickstoreAloneDay(unittest.TestCase):
    """tick-store alone on a day, asked for by --tickstore-alone, on either host: its own kind
    of document, named and validated as such, and exit 0 (review F-5)."""

    def run_day(self, host, argv=("--day", "12302019", "--tickstore-alone")):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            day = FakeDay(self, scratch, host)
            code = day.run(list(argv))
        return code, day

    def test_each_host_ends_with_a_tickstore_alone_document(self):
        for host in ("Linux", "macOS"):
            with self.subTest(host=host):
                code, day = self.run_day(host)
                self.assertEqual(code, 0)
                self.assertEqual(day.ran, [("tickstore", list(variant_rule.TICKSTORE_VARIANTS))])
                (document, output), = day.published
                self.assertEqual(output.name, f"{host}-12302019-full-tickstore-cccccccccccc.json")
                self.assertEqual(document["scope"], variant_rule.TICKSTORE_ONLY)
                self.assertEqual(document["input"]["systems_run"], ["tickstore"])
                self.assertEqual(check_compare.check_tickstore_only(document, output), [])
                memory = document["systems"]["tickstore"]["peak_memory"]
                if host == "Linux":
                    # The guard ran, as on the four-system day, and warm is proven per process.
                    self.assertEqual(check_compare.check_peak_memory("tickstore", memory), [])
                    self.assertEqual(document["method"]["warm"], compare_run.FULL_DAY_WARM)
                    self.assertIn("guard", document["method"])
                else:
                    self.assertEqual(check_compare.check_time_memory("tickstore", memory), [])
                    self.assertEqual(document["method"]["warm"], compare_run.TICKSTORE_ONLY_WARM)
                self.assertEqual(check_compare.check_results.check_leaks(document), [])

    def test_a_day_off_linux_must_ask_for_tickstore_alone(self):
        code, day = self.run_day("macOS", ("--day", "12302019", "--systems", "tickstore"))
        self.assertEqual(code, compare_run.run_bench.EXIT_USAGE)
        self.assertEqual(day.ran, [])

    def test_tickstore_alone_is_a_day_of_tickstore_and_every_variant(self):
        for argv in (["--tickstore-alone"], ["--day", "12302019", "--tickstore-alone",
                                             "--systems", "parquet"],
                     ["--day", "12302019", "--tickstore-alone", "--variants", "v3-seg4Mi"],
                     ["--day", "12302019", "--tickstore-alone", "--variant-source", "x.json"]):
            with self.subTest(argv=argv):
                code, day = self.run_day("Linux", argv)
                self.assertEqual(code, compare_run.run_bench.EXIT_USAGE)
                self.assertEqual(day.ran, [])


class ResumedRun(unittest.TestCase):
    """A part is reused only if it was made with the same arguments (review F-5)."""

    def resume(self, made_with, now):
        args = compare_run.parse_arguments(["--slice", "s", *now])
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            parts = Path(scratch)
            first = compare_run.parse_arguments(["--slice", "s", *made_with])
            (parts / "parquet.json").write_text("{}", encoding="utf-8")
            (parts / "parquet.load").write_text("[]", encoding="utf-8")
            (parts / "parquet.arguments.json").write_text(
                json.dumps(compare_run.part_arguments(first, None, None)), encoding="utf-8")
            layout = SimpleNamespace(work=parts, as_arguments=lambda: [])
            with (mock.patch.object(compare_run.run_bench, "host_label", return_value="macOS"),
                  mock.patch.object(compare_run.subprocess, "run",
                                    return_value=SimpleNamespace(returncode=0)) as ran):
                compare_run.run_one("parquet", args, layout, None, parts, None)
            return ran.called

    def test_a_part_made_with_other_repetitions_runs_again(self):
        self.assertTrue(self.resume(["--warm-repetitions", "5"], []))
        self.assertTrue(self.resume([], ["--cold-repetitions", "3"]))

    def test_a_part_made_with_the_same_arguments_is_kept(self):
        self.assertFalse(self.resume(["--warm-repetitions", "5"], ["--warm-repetitions", "5"]))

    def test_a_part_made_from_another_slice_runs_again(self):
        # Review RV-10: the parts directory names the commit and the day, not the slice.
        self.assertTrue(self.resume([], ["--slice", "t"]))


class ArcticDBSettings(unittest.TestCase):
    """ArcticDB's settings as the engine reads them back, on fakes of its library and config."""

    def system(self, durability, config):
        from compare.systems.arcticdb_system import ArcticDBSystem

        system = ArcticDBSystem(RunContext(layout=None, root=Path("unused"),
                                           variant="rows100k-cols127", durability=durability,
                                           day=None))
        options = SimpleNamespace(rows_per_segment=100_000, columns_per_segment=127,
                                  dynamic_schema=False, dedup=False, encoding_version=0)
        write_options = SimpleNamespace(segment_row_size=100_000, column_group_size=127)
        config_of = SimpleNamespace(lib_desc=SimpleNamespace(
            version=SimpleNamespace(write_options=write_options)))
        system.library = SimpleNamespace(options=lambda: options,
                                         _nvs=SimpleNamespace(lib_cfg=lambda: config_of))
        engine = SimpleNamespace(get_config_int=lambda key: config.get(key))
        return system, mock.patch.dict(sys.modules, {"arcticdb_ext": engine})

    def config(self, flags):
        return {"LMDBStorage.ExtraFlags": flags, "VersionStore.NumCPUThreads": 2,
                "VersionStore.NumIOThreads": 2}

    def test_class_b_flags_the_engine_does_not_have_are_not_read_as_0(self):
        # Review F-7: the engine's None was read as 0, class B's request, so the check
        # passed on the harness's own assumption.
        from compare.settings import SettingsMismatch

        system, engine = self.system(CLASS_B, self.config(None))
        with engine, self.assertRaises(SettingsMismatch) as caught:
            system.settings_in_force()
        self.assertIn("LMDBStorage.ExtraFlags: requested 0, in force None", str(caught.exception))

    def test_class_b_flags_set_to_0_are_read_as_0(self):
        # The real engine reads an explicit 0 as 0 and an unset key as None (f7-probe.log).
        system, engine = self.system(CLASS_B, self.config(0))
        with engine:
            checked = system.settings_in_force()
        self.assertIn({"name": "LMDBStorage.ExtraFlags", "requested": 0, "in_force": 0,
                       "read_from": "ArcticDB's config, as the engine sees it"}, checked)


class ArcticDBStoredRows(unittest.TestCase):
    """The one-symbol store's row count against the rows read back (review F-8)."""

    def stored(self, described, read):
        import pandas

        from compare.systems.arcticdb_system import ArcticDBSystem

        system = ArcticDBSystem(RunContext(layout=None, root=Path("unused"),
                                           variant="rows100k-cols127", durability=CLASS_B,
                                           day=None))
        index = pandas.DataFrame({"start_row": [0], "end_row": [described], "start_col": [0],
                                  "end_col": [1]})
        tool = SimpleNamespace(find_keys=lambda kind: ["key"])
        system.library = SimpleNamespace(
            get_description=lambda symbol: SimpleNamespace(row_count=described),
            _nvs=SimpleNamespace(read_index=lambda symbol: index, library_tool=lambda: tool),
            admin_tools=lambda: SimpleNamespace(get_sizes=lambda: {}))
        system.rows_read = read
        engine = SimpleNamespace(storage=SimpleNamespace(KeyType=SimpleNamespace(TABLE_DATA=1)))
        with (mock.patch.dict(sys.modules, {"arcticdb_ext": engine}),
              mock.patch.object(system, "_segment_codec", return_value={}),
              mock.patch.object(system, "_lmdb_map_size", return_value=system.map_size_bytes)):
            return system.stored_settings()

    def test_a_description_short_of_the_rows_read_back(self):
        from compare.settings import SettingsMismatch

        with self.assertRaises(SettingsMismatch) as caught:
            self.stored(described=5, read=4)
        self.assertIn("rows in the description: requested 4, in force 5", str(caught.exception))

    def test_the_rows_read_back_are_the_description_s(self):
        self.assertEqual(self.stored(described=5, read=5)["checks_passed"], 4)

    def test_with_nothing_read_back_the_rows_are_not_a_check(self):
        # compare/fullday.py's capacity check asks for the stored settings without a read-back.
        self.assertEqual(self.stored(described=5, read=None)["checks_passed"], 3)


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
