#!/usr/bin/env python3
"""Tests for scripts/check_oracle.py's footer sweep, without the tool. Run with:

    python3 -I tests/scripts/test_check_oracle.py

oracle_fixtures/footer-sweep.json is a real sweep: the oracle's store, made by
`python3 -I scripts/check_oracle.py build/dev/tools/tick_roundtrip --sweep-out PATH` at the
freeze commit's code, with `verify --no-checksums`'s verdict on bit 1 of each of its 1,464 footer
bytes. The oracle step makes and judges the same sweep with the built tool on every check.sh run.
"""

import importlib.util
import json
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_oracle.py"
FIXTURE = Path(__file__).resolve().parent / "oracle_fixtures" / "footer-sweep.json"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_oracle"

SPEC = importlib.util.spec_from_file_location("check_oracle", SCRIPT)
check_oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_oracle)
g2_plan = check_oracle.g2_plan


def fixture():
    document = json.loads(FIXTURE.read_text(encoding="utf-8"))
    return document["layout"], document["swept"]


class FooterSweep(unittest.TestCase):
    def setUp(self):
        self.layout, self.swept = fixture()
        container = self.layout["container"]
        self.footer_offset = container["footer_offset"]
        self.footer_bytes = (self.layout["file_bytes"] - container["trailer_bytes"]
                             - self.footer_offset)

    def test_the_fixture_is_the_oracles_store(self):
        records, _ = check_oracle.source_stream()
        self.assertEqual((self.layout["records"], self.layout["segment_records"]),
                         (len(records), check_oracle.SEGMENT_RECORDS))
        self.assertEqual(self.footer_bytes, 1464)
        self.assertEqual(len(self.swept), self.footer_bytes)

    def test_every_byte_the_rule_reads_was_caught_and_every_byte_it_skips_was_not(self):
        self.assertEqual(check_oracle.judge_footer_sweep(self.layout, self.swept), [])
        # What passed is the CRC32C of each of the 26 block entries and 3 section entries.
        passed = [at for at, caught in self.swept if not caught]
        self.assertEqual(len(passed), 4 * (self.layout["blocks"] + 3))
        self.assertTrue(all(g2_plan.unread(self.layout, at) for at in passed))

    def test_from_every_footer_byte_the_rule_flips_one_verify_caught(self):
        caught = dict(self.swept)
        for at in range(self.footer_bytes):
            flipped, _ = g2_plan.flip_from(self.layout, at, self.footer_bytes)
            self.assertTrue(caught[flipped], f"from footer byte {at}, byte {flipped}")

    def test_a_byte_the_rule_reads_that_verify_passed_is_named(self):
        swept = [[at, caught and at != 700] for at, caught in self.swept]
        problems = check_oracle.judge_footer_sweep(self.layout, swept)
        self.assertIn("footer byte 700, which the rule may flip, passed verify --no-checksums "
                      "flipped", problems)

    def test_a_byte_the_rule_skips_that_verify_caught_is_named(self):
        swept = [[at, caught or at == 48] for at, caught in self.swept]
        problems = check_oracle.judge_footer_sweep(self.layout, swept)
        self.assertEqual(problems, ["footer byte 48, the first byte of block 0's CRC32C, which "
                                    "the rule passes over, was caught"])

    def test_a_sweep_that_missed_a_byte_is_refused(self):
        swept = [entry for entry in self.swept if entry[0] != 900]
        self.assertEqual(check_oracle.judge_footer_sweep(self.layout, swept),
                         ["the sweep flipped 1463 bytes, not each of the footer's 1464 once "
                          "in order"])

    def sweep_stand_in(self, exit_for):
        """Sweeps a file of the fixture's size with a verify that finds the one flipped byte."""
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, folder)
        store = folder / "store.tsc"
        original = random.Random(1).randbytes(self.layout["file_bytes"])
        store.write_bytes(original)

        def verify():
            now = store.read_bytes()
            changed = [i for i in range(len(now)) if now[i] != original[i]]
            self.assertEqual(len(changed), 1)
            self.assertEqual(now[changed[0]] ^ original[changed[0]], 1 << g2_plan.BIT)
            return exit_for(changed[0] - self.footer_offset)

        swept = check_oracle.sweep_footer(store, self.footer_offset, self.footer_bytes, verify)
        self.assertEqual(store.read_bytes(), original)
        return swept

    def test_the_sweep_flips_every_footer_byte_once_and_puts_it_back(self):
        caught = dict(self.swept)
        swept = self.sweep_stand_in(lambda at: 1 if caught[at] else 0)
        self.assertEqual(check_oracle.judge_footer_sweep(self.layout, swept), [])

    def test_an_exit_that_is_neither_pass_nor_mismatch_stops_the_sweep(self):
        with self.assertRaisesRegex(check_oracle.SweepFailed,
                                    "exited 2 with footer byte 3 flipped"):
            self.sweep_stand_in(lambda at: 2 if at == 3 else 1)

    def test_a_missing_tool_fails_and_says_so(self):
        done = subprocess.run([sys.executable, "-I", str(SCRIPT),
                               str(REPO_ROOT / "build" / "no-such-tool")],
                              capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 1)
        self.assertIn("no tick_roundtrip at", done.stderr)


if __name__ == "__main__":
    unittest.main()
