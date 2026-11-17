#!/usr/bin/env python3
"""Tests for scripts/check_roundtrip.py. Run with:

    python3 -I tests/scripts/test_check_roundtrip.py

G-2 is judged on a round-trip file's exit_code_corrupt and exit_code_restored. These require
each to be borne out by the exit codes its runs recorded, of every file: the committed files
that predated them, taken by the SHA-256 of their bytes, were replaced at the freeze.
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

    def test_the_commit_a_file_names_exempts_nothing(self):
        # Review RG-5: the committed files were let through by the commit they name, which any
        # file can name.
        old = document()
        old["commit"] = "1669ae0"
        for name in check_roundtrip.G2_CORRUPT + ("restored",):
            del old["g2"][name]["exit_code"]
        del old["g1"]["verify"]["exit_code"]
        self.assertIn("g2.data_crc: no exit_code observed", check_roundtrip.check(old))

    def test_the_committed_files_pass_in_full(self):
        # Every committed file carries each run's exit code, so none needs a pin.
        paths = sorted((REPO_ROOT / "results").glob("roundtrip-*.json"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(path=path.name):
                self.assertEqual(check_roundtrip.check_file(path), [])

    def test_the_pins_file_holds_no_round_trip_pin(self):
        pins = json.loads((REPO_ROOT / "scripts" / "data_pins.json").read_text(encoding="utf-8"))
        self.assertNotIn("roundtrip_files_before_exit_codes", pins)

    def scratch(self):
        root = REPO_ROOT / "build"
        root.mkdir(exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="test-check-roundtrip-", dir=root))
        self.addCleanup(shutil.rmtree, folder)
        return folder

    def test_a_pinned_name_with_other_bytes_is_checked_in_full(self):
        # The reviewer's case: the committed file, its commit kept, G-2's data block verdict
        # turned to a pass, under the pinned name. The freeze's file records every exit code,
        # so checked in full it is refused for the edit itself.
        edited = json.loads((REPO_ROOT / "results" / "roundtrip-12302019.json").read_text())
        edited["g2"]["data_crc"]["verdict"] = "pass"
        path = self.scratch() / "roundtrip-12302019.json"
        path.write_text(json.dumps(edited, indent=1), encoding="utf-8")
        ran = subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(ran.returncode, 1, ran.stderr)
        self.assertIn("roundtrip-12302019.json: g2.data_crc: exit_code 1 with the verdict 'pass', "
                      "which verify exits 0 on", ran.stderr)

    def test_the_old_form_is_refused_under_a_committed_name(self):
        # The form the assembler wrote before the exit codes were kept, G-2's summaries alone,
        # under each name the files of that form had: no name takes a file past its runs.
        old = document()
        for name in check_roundtrip.G2_CORRUPT + ("restored",):
            del old["g2"][name]["exit_code"]
        del old["g1"]["ingest"]["exit_code"]
        del old["g1"]["verify"]["exit_code"]
        folder = self.scratch()
        for name in ("roundtrip-10302019.json", "roundtrip-12302019-linux.json",
                     "roundtrip-12302019.json"):
            with self.subTest(name=name):
                path = folder / name
                path.write_text(json.dumps(old), encoding="utf-8")
                ran = subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)],
                                     capture_output=True, text=True, check=False)
                self.assertEqual(ran.returncode, 1, ran.stderr)
                self.assertIn(f"{name}: g2.data_crc: no exit_code observed", ran.stderr)

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
