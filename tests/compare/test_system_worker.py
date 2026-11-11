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
                                 query_processes=lambda: {"tick_query": 7})
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
                                 scan_facts=lambda query: {"status": "unavailable"})
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
            answer = {"seconds": 0.5 + len(seen), "engine_seconds": None,
                      "swap_timed": {"pages_in": 0, "pages_out": 0}}
            return answer, {"resident_after_eviction": 0}, queue.pop(0) if queue else 0

        runner.cold_repetition = repetition
        query = Query("Q2", "rows", OPEN_NS, OPEN_NS + NANOS_PER_SECOND, 7)
        found = runner.cold_runs("plain", None, Path("store"), [Path("store/a")], [query],
                                 {"Q2": {}})
        return found, seen

    def test_a_quiet_store_counts_every_repetition(self):
        (cold, _, _, proofs, again), seen = self.runs([0, 0, 0])
        self.assertEqual((len(cold["Q2"]), len(proofs["Q2"]), again["Q2"], len(seen)),
                         (3, 3, [], 3))

    def test_a_refilled_store_runs_the_repetition_again(self):
        (cold, _, swaps, proofs, again), seen = self.runs([0, 13_643, 0, 0])
        self.assertEqual(len(seen), 4)
        self.assertEqual(again["Q2"], [13_643])
        self.assertEqual(cold["Q2"], [1.5, 3.5, 4.5])
        self.assertEqual((len(proofs["Q2"]), len(swaps["Q2"])), (3, 3))

    def test_a_store_that_keeps_refilling_stops_the_run(self):
        from compare import run_system

        with self.assertRaisesRegex(run_system.StepFailed, "kept reading"):
            self.runs([1] * (run_system.COLD_RERUNS + 1))


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

    def build(self, host, meter, parts, day=None):
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
        args = SimpleNamespace(systems=systems, slice="s", day=day, full_day_smoke=False)
        pin = {"sha256": "ab", "message_count": 1, "size": 1}
        layout = SimpleNamespace(export_dir=lambda name: name)
        run_bench = compare_run.run_bench
        with (mock.patch.object(compare_run, "compiler_facts", return_value={}),
              mock.patch.object(compare_run, "read_manifest", return_value={}),
              mock.patch.object(run_bench, "cpu_model", return_value="cpu"),
              mock.patch.object(run_bench, "linux_turbo", return_value="enabled")):
            return compare_run.build_document(host, args, layout, pin, "c" * 40, "wrapper", parts,
                                              [[], []], meter)

    def test_each_host(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as scratch:
            parts = Path(scratch)
            topology = parts / "cpu" / "cpu4" / "topology"
            topology.mkdir(parents=True)
            (topology / "thread_siblings_list").write_text("4-5\n", encoding="ascii")
            meter = compare_run.run_bench.SiblingMeter(parts / "cpu", parts / "stat")
            linux = self.build("Linux", meter, parts)
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
    """A full day off Linux: tick-store alone, labelled so, with its peak memory from time -l."""

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
                               full_day_smoke=False)
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

    def test_a_full_day_off_linux_is_tickstore_alone(self):
        args = compare_run.parse_arguments(["--day", "12302019", "--systems", "parquet"])
        self.assertTrue(compare_run.is_tickstore_only(args, "macOS"))
        self.assertFalse(compare_run.is_tickstore_only(args, "Linux"))
        with self.assertRaises(compare_run.run_bench.Failure):
            compare_run.run(args)

    def test_no_statement_trips_the_leak_check(self):
        for text in (compare_run.TICKSTORE_ONLY_LABEL, compare_run.TICKSTORE_ONLY_HEAP,
                     compare_run.TICKSTORE_ONLY_WARM, compare_run.TICKSTORE_ONLY_MEMORY,
                     compare_run.TICKSTORE_ONLY_INPUT_CACHE, variant_rule.tickstore_only()):
            self.assertEqual(check_compare.check_results.check_leaks({"text": text}), [])


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
