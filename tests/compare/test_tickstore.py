#!/usr/bin/env python3
"""compare/systems/tickstore.py, with no tool run: each rule it reads settings, flushes and answers
by, shown failing on one injected fault. Needs the repo's .venv for pyarrow.

    .venv/bin/python -I tests/compare/test_tickstore.py

tests/compare/test_real_systems.py runs the same module on a real store.
"""

import io
import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

try:
    import pyarrow  # noqa: F401
except ImportError:
    raise unittest.SkipTest("needs the .venv: compare/setup.py makes it") from None

from compare.durability import CLASS_A, CLASS_B  # noqa: E402
from compare.queries import (ADD_ORDER_TYPES, ROW_COLUMNS, Query, aggregate_digest,  # noqa: E402
                             digest_result)
from compare.schema import TradingDay  # noqa: E402
from compare.settings import NOT_PROVEN, PROVEN, SettingsMismatch, require_all_match  # noqa: E402
from compare.systems import SYSTEM_NAMES, system_class  # noqa: E402
from compare.systems import tickstore  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402
from compare.systems.tickstore import (COLUMN_CODECS, EngineAnswer, TickStoreSystem,  # noqa: E402
                                       ToolFailure, answer_key, codec_settings,
                                       day_label, expected_flushes, expected_kernel_level,
                                       kernel_sets_that_ran)

DAY = TradingDay(2019, 12, 30)


def system(durability=CLASS_B):
    layout = SimpleNamespace(tool=lambda name: Path("/nonexistent") / name)
    return TickStoreSystem(RunContext(layout, Path("/nonexistent/root"), "v3-seg4Mi", durability,
                                      DAY, source=Path("/nonexistent/slice.itch")))


class FakeWorker:
    """Lines the worker would print, read one at a time."""

    def __init__(self, lines):
        self.stdout = io.StringIO("".join(json.dumps(line) + "\n" for line in lines))
        self.stdin = io.StringIO()
        self.stderr = io.StringIO()

    def wait(self, timeout=None):
        return 0

    def poll(self):
        return None


class Registered(unittest.TestCase):
    def test_tickstore_is_a_system_of_the_harness(self):
        self.assertIn("tickstore", SYSTEM_NAMES)
        self.assertIs(system_class("tickstore"), TickStoreSystem)
        self.assertIn(TickStoreSystem.headline_variant, TickStoreSystem.variants)


class Codecs(unittest.TestCase):
    def blocks(self):
        return [{"column": column, "codec": codec} for column, (codec, _) in COLUMN_CODECS.items()]

    def test_every_column_on_its_listed_codec_passes(self):
        settings, found = codec_settings(self.blocks() + [{"column": "locate", "codec": "zstd"}])
        self.assertEqual(len(require_all_match(settings)), len(COLUMN_CODECS))
        self.assertEqual(found["locate"], ["pfor", "zstd"])

    def test_a_column_on_another_codec_fails(self):
        settings, _ = codec_settings(self.blocks() + [{"column": "timestamp", "codec": "zstd"}])
        with self.assertRaisesRegex(SettingsMismatch, "codec ids, timestamp"):
            require_all_match(settings)

    def test_a_column_docs_do_not_list_fails(self):
        with self.assertRaisesRegex(SettingsMismatch, "a column docs/store.md has not"):
            codec_settings([{"column": "extra", "codec": "raw"}])

    def test_the_table_covers_the_39_columns_of_docs_store_md(self):
        self.assertEqual(len(COLUMN_CODECS), 39)
        text = (REPO_ROOT / "docs" / "store.md").read_text(encoding="utf-8")
        for column in COLUMN_CODECS:
            self.assertIn(f"| {column} |", text)


class Flushes(unittest.TestCase):
    def test_class_a_makes_none_and_class_b_the_file_then_its_directory(self):
        self.assertEqual(expected_flushes(CLASS_A), [])
        self.assertEqual([item["call"] for item in expected_flushes(CLASS_B, darwin=True)],
                         ["F_FULLFSYNC", "F_FULLFSYNC"])
        linux = expected_flushes(CLASS_B, darwin=False)
        self.assertEqual([(item["call"], item["of"]) for item in linux],
                         [("fdatasync", "file"), ("fsync", "directory")])

    def test_class_b_is_proven_by_the_calls_and_no_dirty_page(self):
        flush = {"engine_flushes": expected_flushes(CLASS_B), "data_dirty_pages": 0}
        self.assertEqual(system().durability_verdict(flush, False)["status"], PROVEN)

    def test_class_b_with_a_flush_missing_is_not_proven(self):
        flush = {"engine_flushes": expected_flushes(CLASS_B)[:1], "data_dirty_pages": 0}
        self.assertEqual(system().durability_verdict(flush, True)["status"], NOT_PROVEN)

    def test_class_b_with_a_failed_flush_is_not_proven(self):
        calls = [dict(item) for item in expected_flushes(CLASS_B)]
        calls[0]["result"] = -1
        flush = {"engine_flushes": calls, "data_dirty_pages": 0}
        self.assertEqual(system().durability_verdict(flush, True)["status"], NOT_PROVEN)

    def test_class_b_with_dirty_pages_is_not_proven(self):
        flush = {"engine_flushes": expected_flushes(CLASS_B), "data_dirty_pages": 3}
        self.assertEqual(system().durability_verdict(flush, True)["status"], NOT_PROVEN)

    def test_class_a_with_a_flush_is_not_proven(self):
        flush = {"engine_flushes": expected_flushes(CLASS_B), "data_dirty_pages": 0}
        self.assertEqual(system(CLASS_A).durability_verdict(flush, True)["status"], NOT_PROVEN)
        self.assertEqual(system(CLASS_A).durability_verdict(
            {"engine_flushes": [], "data_dirty_pages": 0}, False)["status"], PROVEN)

    def test_the_facts_state_the_primitive_and_granularity(self):
        for durability in (CLASS_A, CLASS_B):
            facts = system(durability).durability_facts()
            self.assertTrue(facts["sync_primitive"] and facts["flush_granularity"])
            self.assertIs(facts["like_for_like"], True)


class Kernels(unittest.TestCase):
    def test_the_level_comes_from_the_processor(self):
        self.assertIn(expected_kernel_level(), ("scalar", "neon", "avx2"))
        self.assertEqual(system().worker_environment(),
                         {"TICK_STORE_SIMD": expected_kernel_level()})

    def test_the_sets_that_ran_are_those_counted(self):
        self.assertEqual(kernel_sets_that_ran({"scalar": 0, "neon": 4, "avx2": 0}), ["neon"])
        self.assertEqual(kernel_sets_that_ran({"scalar": 1, "neon": 4, "avx2": 0}),
                         ["neon", "scalar"])


class Answers(unittest.TestCase):
    def answer(self, first, full):
        owner = system()
        owner.worker = FakeWorker([full])
        return EngineAnswer(owner, first)

    def test_rows_take_the_workers_digest(self):
        query = Query("Q1", "rows", 0, 1)
        answer = self.answer({"name": "", "answered": True, "rows": 2},
                             {"name": "", "rows": 2, "sha256": "ab", "engine_ns": 5000})
        self.assertEqual(digest_result(query, answer), {"rows": 2, "sha256": "ab"})
        self.assertEqual(answer.engine_seconds, 5e-6)

    def test_an_aggregate_is_digested_here_too_and_must_agree(self):
        query = Query("Q3", "minute_aggregate", 0, 1, 3)
        rows = [[1, 2, 3, 4, 5]]
        good = aggregate_digest(rows)
        answer = self.answer({"name": "", "answered": True},
                             {"name": "", "answer": rows, "engine_ns": 1, **good})
        self.assertEqual(digest_result(query, answer), good)
        bad = self.answer({"name": "", "answered": True},
                          {"name": "", "answer": rows, "engine_ns": 1, "rows": 1,
                           "sha256": "0" * 64})
        with self.assertRaisesRegex(ToolFailure, "digested its answer"):
            digest_result(query, bad)

    def test_an_error_line_after_the_ack_fails(self):
        answer = self.answer({"name": "", "answered": True}, {"name": "", "error": "broken"})
        with self.assertRaisesRegex(ToolFailure, "broken"):
            answer.finish()

    def test_queries_outside_what_the_worker_digests_are_refused(self):
        owner = system()
        with self.assertRaises(ValueError):
            owner.query_rows(0, 1, -1, ROW_COLUMNS[:-1])
        with self.assertRaises(ValueError):
            owner.query_minute_aggregate(0, 1, 3, (ord("A"),))
        with self.assertRaises(ValueError):
            owner.query_column_sum("price")
        self.assertEqual(ADD_ORDER_TYPES, (ord("A"), ord("F")))

    def test_an_ack_returns_before_the_digest_is_read(self):
        owner = system()
        owner.worker = FakeWorker([{"name": "", "answered": True, "rows": 1, "engine_ns": 7},
                                   {"name": "", "rows": 1, "sha256": "cd", "engine_ns": 7,
                                    "bytes_decoded": 9, "values_decoded": 3, "segments": 1,
                                    "exact_segments": 0, "blocks": 2, "digest_ns": 4,
                                    "kernel_runs": {"neon": 1}}])
        answer = owner.query_rows(5, 6, -1, ROW_COLUMNS)
        self.assertIsNone(answer.full)
        sent = json.loads(owner.worker.stdin.getvalue())
        self.assertIs(sent["ack"], True)
        facts = owner.scan_facts(Query("Q1", "rows", 5, 6))
        self.assertEqual((facts["bytes_decoded"], facts["values_decoded"]), (9, 3))

    def test_a_column_sum_is_one_key_whatever_the_query_holds(self):
        self.assertEqual(answer_key("column_sum", 5, 6, 7), ("column_sum", 0, 0, -1))
        self.assertEqual(answer_key("rows", 5, 6, 7), ("rows", 5, 6, 7))


class SettingsAfterIngest(unittest.TestCase):
    """The run's read-back with the tools replaced by what they would say: every setting must be
    the one requested, or the run stops."""

    def system(self, durability=CLASS_B, ready=None, report=None, runs=None, blocks=None):
        owner = system(durability)
        level = owner.kernel_level
        owner.expected_records = 100
        owner.ingest_report = {"durability": owner.mode,
                               "flushes": expected_flushes(durability), **(report or {})}
        ready_line = {"ready": True, "store_version": 3, "segment_records": 1 << 22,
                      "records": 100, "path": "indexed", "kernel_level": level, **(ready or {})}
        kernel_runs = runs if runs is not None else {level: 5}

        def start_worker(check=True):
            owner.ready = ready_line

        def ask(request):
            return SimpleNamespace(finish=lambda: {"kernel_runs": kernel_runs})

        owner._start_worker = start_worker
        owner._ask = ask
        owner.close = lambda: None
        owner.layout_blocks = lambda: blocks if blocks is not None else [
            {"column": column, "codec": codec} for column, (codec, _) in COLUMN_CODECS.items()]
        return owner

    def test_everything_as_requested_passes(self):
        names = [item["name"] for item in self.system().settings_after_ingest()]
        for name in ("store version", "segment records", "records", "durability mode",
                     "flushes made", "query path", "kernel level", "kernel sets that ran"):
            self.assertIn(name, names)
        self.assertEqual(sum(name.startswith("codec ids, ") for name in names), 39)

    def test_a_segment_size_mismatch_stops_the_run(self):
        with self.assertRaisesRegex(SettingsMismatch,
                                    "segment records: requested 4194304, in force 65536"):
            self.system(ready={"segment_records": 65536}).settings_after_ingest()

    def test_a_store_version_mismatch_stops_the_run(self):
        with self.assertRaisesRegex(SettingsMismatch, "store version: requested 3, in force 2"):
            self.system(ready={"store_version": 2}).settings_after_ingest()

    def test_page_cache_in_class_b_stops_the_run(self):
        with self.assertRaisesRegex(SettingsMismatch, "durability mode: requested "
                                                      "'device_flush', in force 'page_cache'"):
            self.system(report={"durability": "page_cache", "flushes": []}).settings_after_ingest()

    def test_a_missing_flush_stops_the_run(self):
        with self.assertRaisesRegex(SettingsMismatch, "flushes made"):
            self.system(report={"flushes": expected_flushes(CLASS_B)[:1]}).settings_after_ingest()

    def test_a_flush_in_class_a_stops_the_run(self):
        with self.assertRaisesRegex(SettingsMismatch, "flushes made"):
            self.system(CLASS_A, report={"flushes": expected_flushes(CLASS_B)}
                        ).settings_after_ingest()

    def test_another_kernel_level_stops_the_run(self):
        with self.assertRaisesRegex(SettingsMismatch, "kernel level: requested"):
            self.system(ready={"kernel_level": "not_a_level"}).settings_after_ingest()

    def test_another_kernel_set_running_stops_the_run(self):
        level = expected_kernel_level()
        other = "scalar" if level != "scalar" else "neon"
        with self.assertRaisesRegex(SettingsMismatch, "kernel sets that ran"):
            self.system(runs={level: 5, other: 1}).settings_after_ingest()

    def test_a_column_on_another_codec_stops_the_run(self):
        blocks = [{"column": column, "codec": codec} for column, (codec, _) in
                  COLUMN_CODECS.items()] + [{"column": "type", "codec": "pfor"}]
        with self.assertRaisesRegex(SettingsMismatch, "codec ids, type"):
            self.system(blocks=blocks).settings_after_ingest()

    def test_fewer_records_than_written_stops_the_run(self):
        with self.assertRaisesRegex(SettingsMismatch, "records: requested 100, in force 99"):
            self.system(ready={"records": 99}).settings_after_ingest()


class Ingest(unittest.TestCase):
    def test_the_command_names_the_mode_and_the_segment_size(self):
        owner = system()
        command = owner.ingest_command("in.itch", "out.tsc", "device_flush")
        self.assertEqual(command[1:5], ["ingest", "in.itch", "out.tsc", "--durability"])
        self.assertIn("device_flush", command)
        self.assertEqual(command[command.index("--segment-records") + 1],
                         str(tickstore.SEGMENT_RECORDS))
        self.assertNotIn("--first-records", command)
        probe = owner.ingest_command("in.itch", "probe.tsc", "page_cache", first_records=65536)
        self.assertEqual(probe[-2:], ["--first-records", "65536"])

    def test_the_input_statement_says_itch_parsing_is_in_the_time(self):
        self.assertIn("ITCH parsing", tickstore.INPUT_STATEMENT)
        self.assertIn("Nothing is subtracted", tickstore.INPUT_STATEMENT)
        self.assertEqual(system().requested()["input"], tickstore.INPUT_STATEMENT)

    def test_the_day_label_is_the_tools_form(self):
        self.assertEqual(day_label(DAY), "12302019")

    def test_no_source_is_refused(self):
        layout = SimpleNamespace(tool=lambda name: Path(name))
        owner = TickStoreSystem(RunContext(layout, Path("root"), "v3-seg4Mi", CLASS_A, DAY))
        with self.assertRaisesRegex(ToolFailure, "gave none"):
            owner._source()


if __name__ == "__main__":
    unittest.main(verbosity=1)
