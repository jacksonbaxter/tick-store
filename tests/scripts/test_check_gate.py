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
