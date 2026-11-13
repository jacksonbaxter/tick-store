#!/usr/bin/env python3
"""The harness's failures, on the real systems. No mock. Needs compare/setup.py to have run,
and skips visibly if it has not.

    .venv/bin/python -I tests/compare/test_real_systems.py

The first test is the reason compare/settings.py exists: a QuestDB conf holding
`cairo.commit.mode=not_a_mode` while `sync` was requested. QuestDB starts anyway, echoes the
value and runs nosync. The harness must refuse it. TickStoreReal does the same for tick-store:
a store or a tool not in the state requested, its segment size, durability or kernel set, must
be refused from what the store and the tools say, never from the harness's own config.
"""

import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from compare.layout import SLICE_NAME, Layout

LAYOUT = Layout.default()
SETUP_DONE = (LAYOUT.java().exists() and LAYOUT.probe_classes().exists())


def build_scratch():
    (REPO_ROOT / "build").mkdir(exist_ok=True)
    return tempfile.TemporaryDirectory(dir=REPO_ROOT / "build")


@unittest.skipUnless(SETUP_DONE, "QuestDB is not set up: run compare/setup.py")
class BadCommitMode(unittest.TestCase):
    def setUp(self):
        from compare.durability import CLASS_B
        from compare.schema import TradingDay
        from compare.systems.base import RunContext
        from compare.systems.questdb_system import QuestDBSystem

        self.scratch = build_scratch()
        self.addCleanup(self.scratch.cleanup)
        LAYOUT.scratch.mkdir(parents=True, exist_ok=True)
        root = Path(self.scratch.name) / "root"
        context = RunContext(LAYOUT, root, "plain", CLASS_B, TradingDay(2019, 12, 30))
        self.system = QuestDBSystem(context)
        self.system.prepare()

    def corrupt_conf(self, value):
        path = self.system.server.conf_path
        path.write_text(path.read_text(encoding="ascii").replace(
            "cairo.commit.mode=sync", f"cairo.commit.mode={value}"), encoding="ascii")

    def test_the_harness_fails_on_not_a_mode_while_sync_is_requested(self):
        from compare.settings import SettingsMismatch
        from compare.systems.questdb_system import PORTS, commit_mode_echo, port_in_use

        self.corrupt_conf("not_a_mode")
        try:
            self.system.start_service()
            # QuestDB's silence, shown: it started, and the echo is the bad text.
            echo = commit_mode_echo()
            self.assertEqual((echo["value"], echo["value_source"]), ("not_a_mode", "conf"))
            # What it really runs, from its own parser.
            self.assertEqual(self.system.server.parsed_commit_mode(), "nosync")
            with self.assertRaisesRegex(SettingsMismatch, "not_a_mode"):
                self.system.service_settings()
        finally:
            self.system.stop_service()
        self.assertFalse(any(port_in_use(port) for port in PORTS))

    def test_a_mode_of_nosync_in_a_class_b_conf_fails(self):
        from compare.settings import SettingsMismatch

        self.corrupt_conf("nosync")
        try:
            self.system.start_service()
            with self.assertRaisesRegex(SettingsMismatch, "requested 'sync', in force 'nosync'"):
                self.system.service_settings()
        finally:
            self.system.stop_service()

    def test_a_correct_conf_passes_on_the_same_path(self):
        try:
            self.system.start_service()
            names = [item["name"] for item in self.system.service_settings()]
        finally:
            self.system.stop_service()
        self.assertIn("cairo.commit.mode (parsed)", names)

    def test_the_runtime_is_read_from_the_running_server(self):
        try:
            self.system.start_service()
            runtime = self.system.server.runtime()
        finally:
            self.system.stop_service()
        linux = platform.system() == "Linux"
        self.assertEqual(runtime["jemalloc_preloaded"], linux)
        self.assertIn("GraalVM" if linux else "Temurin", runtime["jvm_vendor_version"])
        self.assertTrue(runtime["jvm_version"].startswith("25."), runtime)


@unittest.skipUnless((LAYOUT.export_dir(SLICE_NAME) / "manifest.txt").exists() and
                     LAYOUT.tool("tick_roundtrip").exists(),
                     "the slice is not exported, or the bench tools are not built")
class ReadBackRefused(unittest.TestCase):
    """A read-back that lost one value must fail the real oracle."""

    def verify(self, directory):
        pin_source = LAYOUT.data / "slices" / f"{SLICE_NAME}.itch"
        return subprocess.run([str(LAYOUT.tool("tick_roundtrip")), "verify-columns",
                               str(pin_source), str(directory)], check=False, capture_output=True,
                              text=True).returncode

    def test_a_changed_value_is_caught_and_the_untouched_copy_passes(self):
        with build_scratch() as scratch:
            directory = Path(scratch) / "columns"
            shutil.copytree(LAYOUT.export_dir(SLICE_NAME), directory)
            self.assertEqual(self.verify(directory), 0)
            path = directory / "price.bin"
            data = bytearray(path.read_bytes())
            # A hot row's price, in the middle of the window.
            data[4 * 3_000_000] ^= 1
            path.write_bytes(data)
            self.assertNotEqual(self.verify(directory), 0)


SLICE_READY = ((LAYOUT.export_dir(SLICE_NAME) / "manifest.txt").exists() and
               (LAYOUT.data / "slices" / f"{SLICE_NAME}.itch").exists() and
               all(LAYOUT.tool(name).exists()
                   for name in ("tick_roundtrip", "tick_query", "tick_export")))
SMALL_STORE_RECORDS = 200_000


@unittest.skipUnless(SLICE_READY, "the slice is not exported, or the bench tools are not built")
class TickStoreReal(unittest.TestCase):
    """tick-store's settings, read back from a real store and real tools. A setting that differs
    from the one requested must stop the run, as QuestDB's not_a_mode does."""

    def setUp(self):
        self.scratch = build_scratch()
        self.addCleanup(self.scratch.cleanup)
        LAYOUT.scratch.mkdir(parents=True, exist_ok=True)
        self.source = LAYOUT.data / "slices" / f"{SLICE_NAME}.itch"

    def system(self, durability, name=None):
        from compare.schema import TradingDay
        from compare.systems.base import RunContext
        from compare.systems.tickstore import TickStoreSystem

        root = Path(self.scratch.name) / f"root-{name or durability}"
        root.mkdir()
        context = RunContext(LAYOUT, root, "v3-seg4Mi", durability, TradingDay(2019, 12, 30),
                             source=self.source)
        return TickStoreSystem(context)

    def write(self, system, mode=None, segment_records=None):
        """A small store, as the run's write makes it, with one setting changed if asked."""
        system.ingest_report = system._run_ingest(system.ingest_command(
            self.source, system.store_path, mode or system.mode,
            first_records=SMALL_STORE_RECORDS, segment_records=segment_records))
        system.expected_records = SMALL_STORE_RECORDS
        return system

    def columns_view(self):
        """The first records of the export, as compare/queries.py's ground truth reads them."""
        from types import SimpleNamespace

        from compare.schema import ColumnSet

        columns = ColumnSet(LAYOUT.export_dir(SLICE_NAME))
        count = SMALL_STORE_RECORDS
        return SimpleNamespace(
            records=count, fixed={name: values[:count] for name, values in columns.fixed.items()},
            offsets={name: values[:count + 1] for name, values in columns.offsets.items()},
            text_bytes=columns.text_bytes)

    def test_the_settings_in_force_are_read_back_and_queries_match_the_truth(self):
        from compare.durability import CLASS_B
        from compare.queries import ADD_ORDER_TYPES, Query, digest_result, ground_truth, run_query
        from compare.schema import TradingDay

        system = self.write(self.system(CLASS_B))
        names = {item["name"]: item["in_force"] for item in system.settings_after_ingest()}
        self.assertEqual(names["store version"], 3)
        self.assertEqual(names["segment records"], 1 << 22)
        self.assertEqual(names["durability mode"], "device_flush")
        self.assertEqual(len(names["flushes made"]), 2)
        self.assertEqual(names["kernel sets that ran"], [names["kernel level"]])
        self.assertTrue(all(value for name, value in names.items()
                            if name.startswith("codec ids, ")))

        columns = self.columns_view()
        ts = columns.fixed["ts_ns"]
        start = int(ts[SMALL_STORE_RECORDS // 2])
        import numpy as np

        adds = np.isin(np.asarray(columns.fixed["msg_type"]), ADD_ORDER_TYPES)
        locate = int(np.argmax(np.bincount(np.asarray(columns.fixed["locate"])[adds])))
        queries = (Query("rows", "rows", start, start + 10_000_000),
                   Query("one", "rows", 0, start + 10_000_000, locate),
                   Query("minutes", "minute_aggregate", 0, int(ts[-1]) + 1, locate),
                   Query("sum", "column_sum", 0, 0))
        day = TradingDay(2019, 12, 30)
        system.open()
        try:
            for query in queries:
                answer = digest_result(query, run_query(system, query))
                self.assertEqual(answer, ground_truth(columns, query, day), query.name)
                self.assertGreater(answer["rows"], 0, query.name)
        finally:
            system.close()

    def test_the_open_reads_the_store_so_a_cold_figure_holds_it(self):
        """Review F-1: with the store evicted, tick_query's open alone leaves its footer's pages
        resident, so the open is part of the cold figure, and nothing is resident before it."""
        from compare import coldcache
        from compare.durability import CLASS_B

        system = self.write(self.system(CLASS_B))
        coldcache.evict(LAYOUT, [system.store_path])
        before = coldcache.residency(LAYOUT, [system.store_path])["resident_pages"]
        system.open()
        try:
            opened = coldcache.residency(LAYOUT, [system.store_path])["resident_pages"]
            reported = system.reported_open_seconds()
        finally:
            system.close()
        self.assertEqual(before, 0)
        self.assertGreater(opened, 0)
        self.assertTrue(system.open_inside_cold_clock)
        self.assertGreater(reported, 0)

    @unittest.skipUnless(platform.system() == "Darwin", "macOS's own count of a write's reads")
    def test_a_macos_write_reads_nothing_from_a_resident_source_and_reads_an_evicted_one(self):
        # Review RF-5: the device bytes read for the processes that did the write, the worker
        # and tick_roundtrip, through the harness's own write counter.
        from compare import coldcache
        from compare.durability import CLASS_A

        # The tool's own pages count too when they are not cached, 1,310,720 bytes once it was
        # evicted, so it runs once first, as a run's flush probe runs it before every write.
        self.write(self.system(CLASS_A, "first"))
        found = {}
        for state in ("resident", "evicted"):
            system = self.system(CLASS_A, f"{CLASS_A}-{state}")
            (coldcache.warm if state == "resident" else coldcache.evict)(LAYOUT, [self.source])
            reads = coldcache.write_counter(self.source, {})
            self.write(system)
            found[state] = reads.delta()
        self.assertIn("ri_diskio_bytesread", found["resident"]["counter"])
        self.assertEqual(found["resident"]["bytes"], 0, found["resident"])
        self.assertGreater(found["evicted"]["by_process"]["waited children"], 0, found["evicted"])

    def test_a_store_of_another_segment_size_is_refused(self):
        from compare.durability import CLASS_A
        from compare.settings import SettingsMismatch

        system = self.write(self.system(CLASS_A), segment_records=1 << 16)
        with self.assertRaisesRegex(SettingsMismatch,
                                    "segment records: requested 4194304, in force 65536"):
            system.settings_after_ingest()
        # A worker on that store refuses it too, before any query.
        with self.assertRaisesRegex(SettingsMismatch, "segment records"):
            system.open()
        system.close()

    def test_a_class_b_store_written_in_page_cache_is_refused(self):
        from compare.durability import CLASS_B
        from compare.settings import SettingsMismatch

        system = self.write(self.system(CLASS_B), mode="page_cache")
        with self.assertRaises(SettingsMismatch) as caught:
            system.settings_after_ingest()
        self.assertIn("durability mode: requested 'device_flush', in force 'page_cache'",
                      str(caught.exception))
        self.assertIn("flushes made", str(caught.exception))

    def test_a_kernel_cap_below_the_one_requested_is_refused(self):
        import os

        from compare.durability import CLASS_A
        from compare.settings import SettingsMismatch

        system = self.write(self.system(CLASS_A))
        self.assertNotEqual(system.kernel_level, "scalar", "this host has a vector set")
        os.environ["TICK_STORE_SIMD"] = "scalar"
        try:
            with self.assertRaises(SettingsMismatch) as caught:
                system.settings_after_ingest()
        finally:
            del os.environ["TICK_STORE_SIMD"]
        self.assertIn(f"kernel level: requested '{system.kernel_level}', in force 'scalar'",
                      str(caught.exception))
        self.assertIn("kernel sets that ran", str(caught.exception))

    def test_not_a_level_and_not_a_mode_stop_the_tools(self):
        """tick-store's own not_a_mode: neither tool runs on in some default."""
        import os

        from compare.durability import CLASS_A
        from compare.systems.tickstore import ToolFailure

        system = self.system(CLASS_A)
        with self.assertRaisesRegex(ToolFailure, "exited 2"):
            self.write(system, mode="not_a_mode")
        self.assertFalse(system.store_path.exists())
        self.write(system)
        os.environ["TICK_STORE_SIMD"] = "not_a_level"
        try:
            with self.assertRaisesRegex(ToolFailure, "not a kernel level"):
                system.open()
        finally:
            del os.environ["TICK_STORE_SIMD"]
        self.assertIsNone(system.worker)

    def test_the_export_from_the_store_passes_the_oracle_on_the_slice(self):
        from compare.durability import CLASS_A
        from compare.schema import write_columns

        system = self.system(CLASS_A)
        system.ingest_report = system._run_ingest(system.ingest_command(
            self.source, system.store_path, system.mode))
        system.open()
        try:
            table = system.read_all()
            destination = Path(self.scratch.name) / "readback"
            write_columns(table, destination, system.context.day)
            stored = system.stored_settings()
        finally:
            system.close()
        verdict = subprocess.run(
            [str(LAYOUT.tool("tick_roundtrip")), "verify-columns", str(self.source),
             str(destination)], check=False, capture_output=True, text=True)
        self.assertEqual(verdict.returncode, 0, verdict.stderr)
        self.assertEqual(stored["g1"]["verdict"], "pass")
        self.assertGreater(stored["checks_passed"], 40)


if __name__ == "__main__":
    unittest.main(verbosity=1)
