#!/usr/bin/env python3
"""Tests for scripts/check_roundtrip.py. Run with:

    python3 -I tests/scripts/test_check_roundtrip.py

G-2 is judged on a round-trip file's exit_code_corrupt and exit_code_restored. These require
each to be borne out by the exit codes its runs recorded, and the committed files, which
predate them, to pass until the freeze rerun replaces them.
"""

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_roundtrip.py"
SPEC = importlib.util.spec_from_file_location("check_roundtrip", SCRIPT)
check_roundtrip = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_roundtrip)


def verify(verdict, code):
    return {"command": "verify", "verdict": verdict, "exit_code": code}


def document():
    """A round-trip file as scripts/store_results.py writes it now."""
    return {"day": "12302019", "host": "macOS", "commit": "0123456789ab",
            "g1": {"gate": "G-1", "verdict": "pass",
                   "ingest": {"command": "ingest", "exit_code": 0},
                   "verify": verify("pass", 0)},
            "g2": {"gate": "G-2", "exit_code_corrupt": 1, "exit_code_restored": 0,
                   "data_crc": verify("mismatch", 1),
                   "data_no_checksums": verify("mismatch", 1),
                   "footer_crc": verify("mismatch", 1),
                   "footer_no_checksums": verify("mismatch", 1),
                   "restored": verify("pass", 0)}}


class CheckRoundtrip(unittest.TestCase):
    def test_a_file_whose_codes_were_observed_passes(self):
        self.assertEqual(check_roundtrip.check(document()), [])

    def test_a_summary_no_run_bears_out_is_refused(self):
        # The assembler of 1669ae0 typed exit_code_corrupt 1: a run that exited 2 then still
        # read "flipped store exits 1".
        typed = document()
        typed["g2"]["footer_no_checksums"]["exit_code"] = 2
        problems = check_roundtrip.check(typed)
        self.assertIn("g2.exit_code_corrupt is 1, and g2.footer_no_checksums exited 2", problems)
        typed = document()
        typed["g2"]["exit_code_restored"] = 0
        typed["g2"]["restored"]["exit_code"] = 1
        problems = check_roundtrip.check(typed)
        self.assertIn("g2.exit_code_restored is 0, and g2.restored exited 1", problems)

    def test_a_run_with_no_exit_code_is_refused(self):
        for path in (("g2", "data_crc"), ("g2", "restored"), ("g1", "verify"), ("g1", "ingest")):
            with self.subTest(path=path):
                missing = document()
                del missing[path[0]][path[1]]["exit_code"]
                self.assertIn(f"{'.'.join(path)}: no exit_code observed",
                              check_roundtrip.check(missing))
        flag = document()
        flag["g2"]["data_crc"]["exit_code"] = True
        self.assertIn("g2.data_crc: no exit_code observed", check_roundtrip.check(flag))

    def test_an_exit_code_that_contradicts_the_verdict_is_refused(self):
        wrong = document()
        wrong["g1"]["verify"]["exit_code"] = 2
        self.assertTrue(any("g1.verify: exit_code 2 with the verdict 'pass'" in problem
                            for problem in check_roundtrip.check(wrong)))
        wrong = document()
        wrong["g1"]["ingest"]["exit_code"] = 1
        self.assertIn("g1.ingest: exit_code 1, not 0", check_roundtrip.check(wrong))
        crossed = document()
        crossed["cross_host"] = {"verify": verify("pass", 1)}
        self.assertTrue(any(problem.startswith("cross_host.verify: exit_code 1")
                            for problem in check_roundtrip.check(crossed)))

    def test_a_g1_only_file_needs_no_g2(self):
        alone = document()
        del alone["g2"]
        self.assertEqual(check_roundtrip.check(alone), [])

    def test_the_committed_files_commit_passes_until_the_freeze(self):
        old = document()
        old["commit"] = "1669ae0"
        for name in check_roundtrip.G2_CORRUPT + ("restored",):
            del old["g2"][name]["exit_code"]
        del old["g1"]["verify"]["exit_code"]
        self.assertEqual(check_roundtrip.check(old), [])
        old["commit"] = "1669ae1"
        self.assertTrue(check_roundtrip.check(old))

    def test_the_committed_files_pass(self):
        paths = sorted((REPO_ROOT / "results").glob("roundtrip-*.json"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path.name):
                problems = check_roundtrip.check(json.loads(path.read_text(encoding="utf-8")))
                self.assertEqual(problems, [])

    def test_the_command_line_exits_1_and_names_the_file(self):
        scratch = REPO_ROOT / "build"
        scratch.mkdir(exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="test-check-roundtrip-", dir=scratch))
        self.addCleanup(shutil.rmtree, folder)
        good, bad = folder / "roundtrip-good.json", folder / "roundtrip-bad.json"
        good.write_text(json.dumps(document()), encoding="utf-8")
        broken = document()
        broken["g2"]["data_crc"]["exit_code"] = 2
        bad.write_text(json.dumps(broken), encoding="utf-8")
        ran = subprocess.run([sys.executable, "-I", str(SCRIPT), str(good)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        ran = subprocess.run([sys.executable, "-I", str(SCRIPT), str(good), str(bad)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(ran.returncode, 1)
        self.assertIn("roundtrip-bad.json: g2.exit_code_corrupt is 1, and g2.data_crc exited 2",
                      ran.stderr)
        self.assertNotIn("roundtrip-good.json", ran.stderr)


if __name__ == "__main__":
    unittest.main()
