#!/usr/bin/env python3
"""Tests for scripts/check_gate.py. Run with:

    python3 -I tests/scripts/test_check_gate.py

Each test assembles a small gate file with scripts/gate_results.py from the raw records
tests/scripts/test_gate_results.py writes, two named seeds run five times on each host, puts it
in a tree of its own under the repo's gitignored build/ folder beside a list of those seeds, a
launch file and a disturbance file, and shows the file passing before one change makes the
check refuse it. The committed file is checked last, with the figures the pages give. The block
render_results.py writes into fuzz/README.md from the file is rendered in such a tree too.
"""

import copy
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_gate"
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_gate  # noqa: E402
import render_results  # noqa: E402
import test_gate_results as records  # noqa: E402

NAME = f"fuzz-gate-{records.COMMIT[:12]}.json"
# The small gate's Linux runs, as the records time them: seed 1's run K from 15:10:(K-1)0 to
# 15:10:(K-1)9, seed 2's run K from 15:10:50 + 10(K-1).
LOGIN = {"start": "2026-11-17T15:10:31Z", "end": "2026-11-17T15:10:32Z", "precision": "second",
         "what": "a remote login by this project",
         "document": f"fuzz-g3-{records.COMMIT[:12]}.json",
         "phase": "gate run", "case": "run_length/bound-small-blocks run 4 of 5", "timed": True}


class Tree(unittest.TestCase):
    """A tree of its own: fuzz/'s list and launch file, the gate file and a disturbance file."""

    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        made = self.root / "made"
        made.mkdir()
        self.document = records.assembled(made)
        (self.root / "fuzz").mkdir()
        (self.root / "results").mkdir()
        (self.root / "fuzz" / "bound_seeds.txt").write_text(records.seeds_text(),
                                                             encoding="utf-8")
        (self.root / "fuzz" / "launch.txt").write_text(records.LAUNCH, encoding="utf-8")
        self.entries = [dict(LOGIN)]

    def write(self, document=None, name=NAME, entries=None):
        (self.root / "results" / name).write_text(
            json.dumps(self.document if document is None else document), encoding="utf-8")
        (self.root / "results" / f"disturbances-{records.COMMIT[:12]}.json").write_text(
            json.dumps({"schema": "tick-store disturbances 1", "commit": records.COMMIT,
                        "how": "written for the test",
                        "entries": self.entries if entries is None else entries}),
            encoding="utf-8")
        (self.root / "results" / LOGIN["document"]).write_text(
            json.dumps({"header": {"commit": records.COMMIT}, "runs": []}), encoding="utf-8")

    def problems(self, document=None, name=NAME, entries=None):
        self.write(document, name, entries)
        found, entry_problems, _ = check_gate.check(self.root)
        return [problem for problems in found.values() for problem in problems] + entry_problems

    def edited(self, edit):
        """The small gate's file passing, then with one change."""
        self.assertEqual(self.problems(), [])
        document = copy.deepcopy(self.document)
        edit(document)
        return document

    def run_of(self, document, host, seed, number):
        return next(run for run in document["runs"] if (run["host"], run["seed"], run["run"]) ==
                    (host, seed, number))

    def assert_refused(self, edit, words):
        problems = self.problems(self.edited(edit))
        self.assertTrue(any(words in problem for problem in problems), problems)


class ShapeTest(Tree):
    def test_a_file_named_for_another_commit_is_refused(self):
        self.assertEqual(self.problems(), [])
        (self.root / "results" / NAME).unlink()
        self.assertTrue(any("is not a full commit its name gives" in problem for problem in
                            self.problems(name="fuzz-gate-000000000000.json")))

    def test_another_schema_is_refused(self):
        self.assert_refused(lambda document: document["header"].update(schema="other"),
                            "its schema is not")

    def test_a_run_with_a_field_missing_is_refused(self):
        self.assert_refused(lambda document: document["runs"][3].pop("load1"),
                            "run 4 is not a run of")


class FormTest(Tree):
    """Each run field holds only what the assembler writes there: what is printed on the page
    as a number is a finite number of at least 0, a time is a real UTC time."""

    LINUX_RUN = ("Linux", "run_length/bound-small-blocks", 1)

    def assert_field_refused(self, key, value, words, run=LINUX_RUN):
        def edit(document):
            self.run_of(document, *run)[key] = value
        self.assert_refused(edit, words)

    def test_a_seed_that_is_not_a_name_is_refused(self):
        self.assert_field_refused("seed", 5, "its seed 5 is not a seed's name")

    def test_a_target_that_is_not_a_name_is_refused(self):
        self.assert_field_refused("target", None, "its target None is not a fuzz target's name")

    def test_a_run_numbered_zero_is_refused(self):
        self.assert_field_refused("run", 0, "its run 0 is not a whole number of at least 1")

    def test_a_start_that_is_no_time_is_refused(self):
        self.assert_field_refused("start", "2026-11-17T25:61:99Z",
                                  "its start '2026-11-17T25:61:99Z' is not a real UTC time")

    def test_a_start_on_no_real_day_is_refused(self):
        self.assert_field_refused("start", "2026-11-31T15:10:00Z",
                                  "its start '2026-11-31T15:10:00Z' is not a real UTC time")

    def test_an_end_that_is_no_time_is_refused(self):
        self.assert_field_refused("end", "2026-11-17T15:10:60Z",
                                  "its end '2026-11-17T15:10:60Z' is not a real UTC time")

    def test_an_end_before_the_start_is_refused(self):
        self.assert_field_refused("end", "2026-11-17T15:09:59Z",
                                  "it ends at 2026-11-17T15:09:59Z, before it starts")

    def test_a_negative_exit_code_is_refused(self):
        self.assert_field_refused("exit_code", -1,
                                  "its exit_code -1 is not a whole number of at least 0")

    def test_findings_as_a_string_are_refused(self):
        self.assert_field_refused("findings", "0",
                                  "its findings '0' is not a whole number of at least 0")

    def test_no_executed_unit_is_refused(self):
        self.assert_field_refused("executed_units", 0,
                                  "its executed_units 0 is not a whole number of at least 1")

    def test_a_libfuzzer_time_that_is_not_whole_is_refused(self):
        self.assert_field_refused("file_ms", 9337.0,
                                  "its file_ms 9337.0 is not a whole number of at least 0")

    def test_a_time_an_execution_that_is_not_a_float_is_refused(self):
        def edit(document):
            run = self.run_of(document, *self.LINUX_RUN)
            run.update(file_ms=9336, ms_per_execution=4668)
        self.assert_refused(edit, "its ms_per_execution 4668 is not a finite number of at "
                                  "least 0")

    def test_a_negative_wall_time_is_refused(self):
        self.assert_field_refused("wall_ms", -1,
                                  "its wall_ms -1 is not a whole number of at least 0")

    def test_a_load_that_is_not_a_number_is_refused(self):
        self.assert_field_refused("load1", float("nan"),
                                  "its load1 nan is not a finite number of at least 0")

    def test_an_infinite_load_is_refused(self):
        self.assert_field_refused("load1", float("inf"),
                                  "its load1 inf is not a finite number of at least 0")

    def test_a_negative_load_is_refused(self):
        self.assert_field_refused("load1", -3.0,
                                  "its load1 -3.0 is not a finite number of at least 0")

    def test_a_load_as_a_string_is_refused(self):
        self.assert_field_refused("load1", "2.40",
                                  "its load1 '2.40' is not a finite number of at least 0")

    def test_a_pass_that_is_not_true_or_false_is_refused(self):
        self.assert_field_refused("pass", 1, "its pass 1 is not true or false")

    def test_a_busy_share_that_is_not_a_number_is_refused(self):
        self.assert_field_refused("cpu_busy_percent", {"cpu2": 2.0, "cpu7": "lots"},
                                  "its cpu_busy_percent {'cpu2': 2.0, 'cpu7': 'lots'} is not")

    def test_a_busy_share_over_a_hundred_is_refused(self):
        self.assert_field_refused("cpu_busy_percent", {"cpu2": 2.0, "cpu7": 100.5},
                                  "its cpu_busy_percent {'cpu2': 2.0, 'cpu7': 100.5} is not")

    def test_a_busy_share_past_one_decimal_is_refused(self):
        self.assert_field_refused("cpu_busy_percent", {"cpu2": 2.05, "cpu7": 100.0},
                                  "its cpu_busy_percent {'cpu2': 2.05, 'cpu7': 100.0} is not")

    def test_busy_shares_out_of_the_cpus_order_are_refused(self):
        self.assert_field_refused("cpu_busy_percent", {"cpu7": 100.0, "cpu2": 2.0},
                                  "its cpu_busy_percent {'cpu7': 100.0, 'cpu2': 2.0} is not")

    def test_busy_shares_without_the_sibling_are_refused(self):
        self.assert_field_refused("cpu_busy_percent", {"cpu2": 2.0, "cpu6": 100.0},
                                  "its cpu_busy_percent {'cpu2': 2.0, 'cpu6': 100.0} is not")

    def test_busy_shares_of_no_cpu_name_are_refused(self):
        self.assert_field_refused("cpu_busy_percent",
                                  {"cpu2": 2.0, "cpu7": 100.0, "disk": 1.0},
                                  "'disk': 1.0} is not each cpu's busy share")

    def test_negative_processes_started_are_refused(self):
        self.assert_field_refused("processes_started", -258,
                                  "its processes_started -258 is not a whole number of at least 0")

    def test_processes_started_as_a_string_are_refused(self):
        self.assert_field_refused("processes_started", "258",
                                  "its processes_started '258' is not a whole number of at least 0")

    def test_a_sibling_at_its_start_that_is_not_true_or_false_is_refused(self):
        self.assert_field_refused("sibling_busy_at_start", 1,
                                  "its sibling_busy_at_start 1 is not true or false")

    def test_a_sibling_at_its_end_that_is_not_true_or_false_is_refused(self):
        self.assert_field_refused("sibling_busy_at_end", "yes",
                                  "its sibling_busy_at_end 'yes' is not true or false")

    def test_a_target_not_its_seeds_own_is_refused(self):
        self.assert_field_refused("target", "fuzz_pfor",
                                  "run_length/bound-small-blocks run 1's target 'fuzz_pfor' is "
                                  "not its own, 'fuzz_run_length'")


class HeaderTest(Tree):
    """The header's words are the assembler's, and its list is the tree's by its SHA-256."""

    def header_refused(self, words, **changes):
        self.assert_refused(lambda document: document["header"].update(changes), words)

    def test_another_check_is_refused(self):
        self.header_refused("its check 'G-2' is not", check="G-2")

    def test_another_list_named_is_refused(self):
        self.header_refused("its seeds_file 'fuzz/other_seeds.txt' is not 'fuzz/bound_seeds.txt'",
                            seeds_file="fuzz/other_seeds.txt")

    def test_a_list_digest_not_the_trees_is_refused(self):
        self.header_refused("its seeds_file_sha256 '" + "0" * 64 + "' is not the tree's",
                            seeds_file_sha256="0" * 64)

    def test_a_list_the_tree_has_since_changed_is_refused(self):
        self.assertEqual(self.problems(), [])
        with (self.root / "fuzz" / "bound_seeds.txt").open("a", encoding="utf-8") as seeds:
            seeds.write("# a comment added since\n")
        self.assertTrue(any("its seeds_file_sha256" in problem for problem in self.problems()))

    def test_a_bar_in_other_words_is_refused(self):
        self.header_refused("its bar 'half the timeout' is not \"half of fuzz/launch.txt's "
                            "-timeout=10, in ms an execution\"", bar="half the timeout")

    def test_another_statement_is_refused(self):
        def edit(document):
            document["header"]["statement"] = document["header"]["statement"].replace(
                "with no finding and ", "")
        self.assert_refused(edit, "its statement is not the assembler's for 5 runs a seed")

    def test_an_assembly_in_other_words_is_refused(self):
        def edit(document):
            document["header"]["assembled"] = document["header"]["assembled"].replace(
                "Nothing was timed again.", "Each run was timed again.")
        self.assert_refused(edit, "is not the assembler's account of its assembly")

    def test_an_assembly_from_runs_of_other_days_is_refused(self):
        def edit(document):
            document["header"]["assembled"] = document["header"]["assembled"].replace(
                "the runs of 2026-11-17:", "the runs of 2026-11-16, 2026-11-17:")
        self.assert_refused(edit, "its assembled gives the runs of 2026-11-16, 2026-11-17, and "
                                  "its runs are of 2026-11-17")

    def test_an_assembly_on_no_day_is_refused(self):
        def edit(document):
            document["header"]["assembled"] = document["header"]["assembled"].replace(
                "Assembled on 2026-11-25", "Assembled on 2026-13-45")
        self.assert_refused(edit, "its assembled gives 2026-13-45 as the day it was assembled")

    def test_no_hosts_facts_are_refused(self):
        self.header_refused("its hosts are not the assembler's facts of macOS and Linux",
                            hosts={})

    def test_hosts_in_another_order_are_refused(self):
        def edit(document):
            hosts = document["header"]["hosts"]
            document["header"]["hosts"] = {"Linux": hosts["Linux"], "macOS": hosts["macOS"]}
        self.assert_refused(edit, "its hosts are not in the order macOS, Linux")


class LeakTest(Tree):
    def test_a_home_path_in_the_file_is_refused(self):
        self.assert_refused(lambda document: document["header"].update(
            assembled=document["header"]["assembled"] + " /home/someone/run"), "it holds")


class BarTest(Tree):
    def test_a_bar_not_half_the_timeout_is_refused(self):
        self.assert_refused(lambda document: document["header"].update(bar_ms_per_execution=6000),
                            "is not half of fuzz/launch.txt's -timeout=10")


class CoverageTest(Tree):
    def test_a_seed_missing_on_a_host_is_refused(self):
        def edit(document):
            document["runs"] = [run for run in document["runs"] if not (
                run["host"] == "Linux" and run["seed"] == "gorilla_xor/bound-costliest")]
        self.assert_refused(edit, "Linux: no run of gorilla_xor/bound-costliest")

    def test_a_seed_not_in_the_list_is_refused(self):
        def edit(document):
            for run in document["runs"]:
                if run["seed"] == "gorilla_xor/bound-costliest" and run["host"] == "macOS":
                    run["seed"] = "gorilla_xor/bound-other"
        self.assert_refused(edit, "gorilla_xor/bound-other is not a seed of")

    def test_fewer_runs_than_the_gate_requires_are_refused(self):
        def edit(document):
            document["runs"] = [run for run in document["runs"] if run["run"] != 5]
            document["header"]["runs_per_seed"] = 4
        self.assert_refused(edit, "it gives 4 runs a seed, and the gate requires 5")

    def test_more_runs_than_the_gate_requires_are_refused(self):
        def edit(document):
            for run in list(document["runs"]):
                if run["run"] == 5:
                    document["runs"].append({**run, "run": 6})
            document["header"]["runs_per_seed"] = 6
            document["header"]["statement"] = document["header"]["statement"].replace(
                "5 times", "6 times")
        self.assert_refused(edit, "it gives 6 runs a seed, and the gate requires 5")

    def test_a_seed_with_a_run_missing_is_refused(self):
        def edit(document):
            document["runs"].remove(self.run_of(document, "macOS",
                                                "run_length/bound-small-blocks", 3))
        self.assert_refused(edit, "macOS: run_length/bound-small-blocks has runs [1, 2, 4, 5]")


class RunTest(Tree):
    def test_a_time_that_does_not_follow_from_the_runs_fields_is_refused(self):
        def edit(document):
            self.run_of(document, "Linux", "run_length/bound-small-blocks", 1)["file_ms"] = 9000
        self.assert_refused(edit, "is not its libFuzzer 9000 ms over 2 units")

    def test_a_run_over_the_bar_marked_pass_is_refused(self):
        def edit(document):
            self.run_of(document, "Linux", "run_length/bound-small-blocks", 2).update(
                file_ms=10002, ms_per_execution=5001.0)
        self.assert_refused(edit, "is over the bar, 5000, and the run is marked pass")

    def test_a_run_with_a_finding_marked_pass_is_refused(self):
        def edit(document):
            self.run_of(document, "macOS", "gorilla_xor/bound-costliest", 4)["findings"] = 1
        self.assert_refused(edit, "marked pass, and its exit code, findings, time and sibling "
                                  "give the other")

    def test_a_linux_run_without_its_sibling_busy_marked_pass_is_refused(self):
        def edit(document):
            self.run_of(document, "Linux", "gorilla_xor/bound-costliest",
                        3)["sibling_busy_at_end"] = False
        self.assert_refused(edit, "marked pass, and its exit code")

    def test_a_clean_run_marked_fail_is_refused(self):
        def edit(document):
            self.run_of(document, "macOS", "gorilla_xor/bound-costliest", 4)["pass"] = False
        self.assert_refused(edit, "marked fail, and its exit code")


class SummaryTest(Tree):
    def test_another_worst_run_is_refused(self):
        def edit(document):
            document["summary"]["Linux"]["worst"]["ms_per_execution"] = 4000.0
        self.assert_refused(edit, "its summary is not what its runs give")

    def test_a_seed_median_not_the_rows_is_refused(self):
        def edit(document):
            document["summary"]["Linux"]["worst"]["seed_median_ms_per_execution"] = 4382.25
        self.assert_refused(edit, "its summary is not what its runs give")

    def test_a_pass_count_not_the_rows_is_refused(self):
        def edit(document):
            document["summary"]["macOS"]["seeds_passed"] = 1
        self.assert_refused(edit, "its summary is not what its runs give")


    def test_an_all_pass_of_one_for_true_is_refused(self):
        def edit(document):
            document["summary"]["macOS"]["all_pass"] = 1
        self.assert_refused(edit, "its summary is not what its runs give")

    def test_a_seed_count_as_a_float_is_refused(self):
        def edit(document):
            document["summary"]["Linux"]["seeds"] = 2.0
        self.assert_refused(edit, "its summary is not what its runs give")

    def test_a_count_of_executions_as_a_float_is_refused(self):
        def edit(document):
            document["summary"]["Linux"]["worst"]["executed_units"] = 2.0
        self.assert_refused(edit, "its summary is not what its runs give")

    def test_a_tied_run_as_a_float_is_refused(self):
        def edit(document):
            document["summary"]["Linux"]["worst"]["runs"] = [1.0]
        self.assert_refused(edit, "its summary is not what its runs give")


    def test_a_summary_missing_a_field_is_refused(self):
        def edit(document):
            del document["summary"]["macOS"]["all_pass"]
        self.assert_refused(edit, "its summary is not what its runs give")

    def test_a_worst_run_tied_with_a_run_that_is_not_is_refused(self):
        def edit(document):
            document["summary"]["Linux"]["worst"]["runs"] = [1, 2]
        self.assert_refused(edit, "its summary is not what its runs give")


class EntryTest(Tree):
    def entry_problems(self, **changes):
        self.assertEqual(self.problems(), [])
        return self.problems(entries=[{**LOGIN, **changes}])

    def test_an_entry_inside_the_run_it_names_passes(self):
        self.assertEqual(self.entry_problems(start="2026-11-17T15:10:30Z",
                                             end="2026-11-17T15:10:39Z"), [])

    def test_an_entry_naming_a_run_the_file_does_not_hold_is_refused(self):
        problems = self.entry_problems(case="run_length/bound-prefix run 4 of 5")
        self.assertTrue(any("holds no Linux run 4 of run_length/bound-prefix" in problem
                            for problem in problems), problems)

    def test_an_entry_naming_another_count_of_runs_is_refused(self):
        problems = self.entry_problems(case="run_length/bound-small-blocks run 4 of 6")
        self.assertTrue(any("it gives 6 runs a seed" in problem for problem in problems),
                        problems)

    def test_an_entry_outside_the_run_it_names_is_refused(self):
        # Inside seed 1's run 5, 15:10:40 to 15:10:49, as the backup sat in run 4 of a seed
        # and was first written down elsewhere.
        problems = self.entry_problems(start="2026-11-17T15:10:41Z", end="2026-11-17T15:10:43Z")
        self.assertTrue(any("does not overlap run_length/bound-small-blocks run 4 of 5" in problem
                            for problem in problems), problems)

    def test_an_entry_between_runs_is_refused(self):
        problems = self.entry_problems(start="2026-11-17T15:10:39Z", end="2026-11-17T15:10:39Z",
                                       case="run_length/bound-small-blocks run 5 of 5")
        self.assertTrue(any("does not overlap" in problem for problem in problems), problems)

    def test_an_entry_whose_span_is_no_time_is_refused(self):
        problems = self.entry_problems(start="", end="Z")
        self.assertTrue(any("its span '' to 'Z' is not two UTC times" in problem
                            for problem in problems), problems)

    def test_an_entry_with_no_end_is_refused(self):
        problems = self.entry_problems(end=None)
        self.assertTrue(any("to None is not two UTC times" in problem for problem in problems),
                        problems)

    def test_an_entry_that_ends_before_it_starts_is_refused(self):
        problems = self.entry_problems(start="2026-11-17T15:10:35Z", end="2026-11-17T15:10:31Z")
        self.assertTrue(any("is not two UTC times, start first" in problem
                            for problem in problems), problems)

    def test_an_entry_with_no_gate_file_is_refused(self):
        self.write()
        (self.root / "results" / NAME).unlink()
        _, problems, _ = check_gate.check(self.root)
        self.assertTrue(any("there is no valid fuzz-gate-" in problem for problem in problems),
                        problems)


class RenderTest(Tree):
    """The gate's block in fuzz/README.md, rendered from the file and the entries tied to it."""

    PAGE = "# Fuzzing\n\nThe gate:\n\n" + "\n".join(render_results.gate_markers()) + "\n\nAfter.\n"

    def block(self, entries=None, page=PAGE):
        (self.root / "fuzz" / "README.md").write_text(page, encoding="utf-8")
        self.write(entries=entries)
        found = render_results.gate_block_files(self.root)
        return "\n".join(found[0][2][0][2]) if found else None

    def test_the_worst_run_carries_the_events_in_it_in_the_marks_words(self):
        text = " ".join(self.block([{**LOGIN, "case": "run_length/bound-small-blocks run 1 of 5",
                                     "start": "2026-11-17T15:10:03Z",
                                     "end": "2026-11-17T15:10:04Z"}]).split())
        self.assertIn("The worst run was `run_length/bound-small-blocks` at 4,668.5 ms an "
                      "execution, 9,337 ms over 2 executions, in its run 1 of 5 (median 3,908.0)",
                      text)
        self.assertIn("2026-11-17 15:10:00 to 15:10:09 UTC, which held a remote login, "
                      "2026-11-17 15:10:03 to 15:10:04 UTC, which could only have slowed them.",
                      text)
        self.assertIn("The load read just before it began, 2.40, was the highest of its seed's "
                      "five runs, and 258 processes started during it, against a median of 53",
                      text)

    def test_a_worst_run_with_no_event_says_so_and_the_runs_with_one_are_listed(self):
        text = " ".join(self.block().split())
        self.assertIn("15:10:00 to 15:10:09 UTC, which holds no recorded event.", text)
        self.assertIn("The Linux runs that held a recorded event, each passed: "
                      "`run_length/bound-small-blocks` run 4 of 5, 2026-11-17 15:10:30 to "
                      "15:10:39 UTC, 3,908.0 ms an execution against its seed's worst, 4,668.5, "
                      "held a remote login, 2026-11-17 15:10:31 to 15:10:32 UTC; which could only "
                      "have slowed them.", text)
        self.assertIn("| `gorilla_xor/bound-costliest` | 372.0 (377.0) | 2,955.0 (3,160.0) |", text)

    def test_an_event_before_the_worst_run_is_given_with_its_gap(self):
        figures = dict(records.FIGURES["Linux"])
        figures[(1, 4)] = (9400, 2, 2.30, 300)
        made = self.root / "made-later"
        made.mkdir()
        self.document = records.Records(made, figures=figures).assemble()
        text = " ".join(self.block([{**LOGIN, "case": "run_length/bound-small-blocks run 2 of 5",
                                     "start": "2026-11-17T15:10:12Z",
                                     "end": "2026-11-17T15:10:13Z"}]).split())
        self.assertIn("in its run 4 of 5 (median 4,096.0), 2026-11-17 15:10:30 to 15:10:39 UTC, "
                      "which holds no recorded event. It began 17 s after the end of a remote "
                      "login (2026-11-17 15:10:12 to 15:10:13 UTC), in an earlier run of the "
                      "gate. The load read just before it began, 2.30, was the third highest of "
                      "its seed's five runs, and 300 processes started during it, against a "
                      "median of 54 over the host's 10 runs.", text)

    def test_a_refused_file_renders_no_block(self):
        self.document["summary"]["Linux"]["worst"]["ms_per_execution"] = 4000.0
        with self.assertRaises(SystemExit) as caught:
            self.block()
        self.assertEqual(caught.exception.code, render_results.EXIT_USAGE)

    def test_an_entry_the_file_does_not_hold_renders_no_block(self):
        with self.assertRaises(SystemExit):
            self.block([{**LOGIN, "case": "run_length/bound-small-blocks run 4 of 6"}])

    def test_a_page_without_the_markers_is_refused_while_the_file_exists(self):
        with self.assertRaises(SystemExit):
            self.block(page="# Fuzzing\n")

    def test_no_file_and_no_markers_is_no_block(self):
        (self.root / "fuzz" / "README.md").write_text("# Fuzzing\n", encoding="utf-8")
        self.assertEqual(render_results.gate_block_files(self.root), [])


class CommittedTest(unittest.TestCase):
    def test_the_committed_file_passes_with_the_figures_the_pages_give(self):
        found, entries, documents = check_gate.check(REPO_ROOT)
        self.assertEqual(found, {"fuzz-gate-8616263b7561.json": []})
        self.assertEqual(entries, [])
        summary = documents["8616263b7561"]["summary"]
        mac, linux = summary["macOS"]["worst"], summary["Linux"]["worst"]
        self.assertEqual((mac["ms_per_execution"], mac["file_ms"], mac["executed_units"]),
                         (682.0, 1364, 2))
        self.assertEqual((linux["seed"], linux["runs"], linux["ms_per_execution"],
                          linux["file_ms"], linux["executed_units"],
                          linux["seed_median_ms_per_execution"]),
                         ("run_length/bound-small-blocks", [1], 4668.5, 9337, 2, 3908.0))
        self.assertTrue(summary["macOS"]["all_pass"] and summary["Linux"]["all_pass"])
        self.assertEqual((summary["macOS"]["seeds"], summary["Linux"]["runs"]), (31, 155))


if __name__ == "__main__":
    unittest.main()
