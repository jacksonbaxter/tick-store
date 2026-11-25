#!/usr/bin/env python3
"""Tests for the bound-seed gate's block that scripts/render_results.py writes into
fuzz/README.md. Run with:

    python3 -I tests/scripts/test_render_gate.py

Each test assembles a small gate file with scripts/gate_results.py from the raw records
tests/scripts/test_gate_results.py writes, two named seeds run five times on each host, and puts
it in a tree of its own under the repo's gitignored build/ folder, beside a list of those seeds, a
launch file, a disturbance file and a fuzz/README.md with the block's markers. The block says what
each event's recorded end is, and the render refuses a page and its gate files that do not match:
the markers with no gate file or a gate file with no markers, and more than one gate file.
"""

import copy
import json
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRATCH_ROOT = REPO_ROOT / "build" / "test_render_gate"
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_results  # noqa: E402
import test_gate_results as records  # noqa: E402

C12 = records.COMMIT[:12]
FUZZ_FILE = f"fuzz-g3-{C12}.json"
BACKUP = ("the host's scheduled repository backup, from the host's journal as read after the run, "
          "not from the run's own records")
# The small gate's Linux runs: seed 1's run K from 15:10:(K-1)0 to 15:10:(K-1)9.
EVENT = {"start": "2026-11-17T15:10:12Z", "end": "2026-11-17T15:10:13Z", "precision": "second",
         "what": BACKUP, "document": FUZZ_FILE, "phase": "gate run",
         "case": "run_length/bound-small-blocks run 2 of 5", "timed": True}
PAGE = "# Fuzzing\n\nThe gate:\n\n" + "\n".join(render_results.gate_markers()) + "\n\nAfter.\n"


class Tree(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        # Seed 1's run 4, after the event's run 2, is the worst Linux run.
        figures = dict(records.FIGURES["Linux"])
        figures[(1, 4)] = (9400, 2, 2.30, 300)
        made = self.root / "made"
        made.mkdir()
        self.document = records.Records(made, figures=figures).assemble()
        for folder in ("fuzz", "results"):
            (self.root / folder).mkdir()
        (self.root / "fuzz" / "bound_seeds.txt").write_text(records.seeds_text(),
                                                             encoding="utf-8")
        (self.root / "fuzz" / "launch.txt").write_text(records.LAUNCH, encoding="utf-8")
        (self.root / "results" / FUZZ_FILE).write_text(
            json.dumps({"header": {"commit": records.COMMIT}, "runs": []}), encoding="utf-8")

    def write(self, entries, page=PAGE, gate=True):
        (self.root / "fuzz" / "README.md").write_text(page, encoding="utf-8")
        if gate:
            (self.root / "results" / f"fuzz-gate-{C12}.json").write_text(
                json.dumps(self.document), encoding="utf-8")
        (self.root / "results" / f"disturbances-{C12}.json").write_text(
            json.dumps({"schema": "tick-store disturbances 1", "commit": records.COMMIT,
                        "how": "written for the test", "entries": entries}), encoding="utf-8")

    def block(self, entries=(EVENT,)):
        self.write(list(entries))
        found = render_results.gate_block_files(self.root)
        return " ".join("\n".join(found[0][2][0][2]).split())

    def refused(self):
        with redirect_stderr(StringIO()) as said, self.assertRaises(SystemExit) as caught:
            render_results.gate_block_files(self.root)
        self.assertEqual(caught.exception.code, render_results.EXIT_USAGE)
        return said.getvalue()


class EventEndTest(Tree):
    def test_a_journal_event_is_given_by_its_last_line_in_the_journal(self):
        self.assertIn("in its run 4 of 5 (median 4,096.0), 2026-11-17 15:10:30 to 15:10:39 UTC, "
                      "which holds no recorded event. It began 17 s after a backup run's last "
                      "line in the host's journal (2026-11-17 15:10:12 to 15:10:13 UTC), in an "
                      "earlier run of the gate.", self.block())

    def test_a_computed_login_is_given_by_the_latest_it_could_have_ended(self):
        login = {**EVENT, "what": render_results.WATCHER_COMPUTED}
        self.assertIn("It began 17 s after the latest a remote login could have ended "
                      "(2026-11-17 15:10:12 to 15:10:13 UTC)", self.block([login]))

    def test_a_recorded_login_is_given_by_its_end(self):
        login = {**EVENT, "what": "a remote login by this project"}
        self.assertIn("It began 17 s after the end of a remote login", self.block([login]))


class PageTest(Tree):
    def test_the_markers_with_no_gate_file_are_refused(self):
        self.write([], gate=False)
        self.assertIn("has the gate's markers and there is no fuzz-gate-*.json", self.refused())

    def test_a_gate_file_with_no_markers_is_refused(self):
        self.write([], page="# Fuzzing\n")
        self.assertIn("has the gate's markers and there is no fuzz-gate-*.json, or the reverse",
                      self.refused())

    def test_more_than_one_gate_file_is_refused(self):
        self.write([])
        other = copy.deepcopy(self.document)
        other["header"]["commit"] = "a" * 40
        (self.root / "results" / f"fuzz-gate-{'a' * 12}.json").write_text(
            json.dumps(other), encoding="utf-8")
        self.assertIn("more than one fuzz-gate-*.json", self.refused())

    def test_one_gate_file_and_its_markers_render(self):
        self.assertIn("holds the gate's 20 runs", self.block())


if __name__ == "__main__":
    unittest.main()
