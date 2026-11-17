#!/usr/bin/env python3
"""Tests for scripts/check_oracle.py's footer sweep, without the tool. Run with:

    python3 -I tests/scripts/test_check_oracle.py

oracle_fixtures/footer-sweep.json is a real sweep: the oracle's store, made by
`python3 -I scripts/check_oracle.py build/dev/tools/tick_roundtrip --sweep-out PATH` at the
freeze commit's code, with `verify --no-checksums`'s verdict on bit 1 of each of its 1,464 footer
bytes. The oracle step makes and judges the same sweep with the built tool on every check.sh run.

oracle_fixtures/all-bit-sweep.json is every bit of every byte of the oracle's store and of a
bigger one, with checksums and without, made by oracle_fixtures/all_bit_sweep.py with the dev
build's tool at the freeze commit's code: AllBits holds the counts the pages quote to it. And
LiveRefusals reaches each of check()'s refusals with a stand-in for the tool, in this process.
"""

import contextlib
import hashlib
import importlib.util
import io
import json
import random
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

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


ALL_BITS = Path(__file__).resolve().parent / "oracle_fixtures" / "all-bit-sweep.json"
_MAKER = importlib.util.spec_from_file_location(
    "all_bit_sweep", Path(__file__).resolve().parent / "oracle_fixtures" / "all_bit_sweep.py")
all_bit_sweep = importlib.util.module_from_spec(_MAKER)
_MAKER.loader.exec_module(all_bit_sweep)
# What README.md, BENCHMARKS.md and docs/NOTES.md say of G-2 with checksums off, by store: the
# (byte, bit) pairs of the block payloads, and how many of them passed.
PAYLOAD_PAIRS = {"oracle": 5_920, "bigger": 22_592}
PAYLOAD_PASSED = {"oracle": 57, "bigger": 74}
ENTRIES = {"oracle": 29, "bigger": 80}


class AllBits(unittest.TestCase):
    """Review TD-13: every bit of every byte of two small stores, from all-bit-sweep.json. With
    checksums on every flip is refused; with them off a flip passes only where the format's
    structure does not catch it: the CRC32C of each block and section entry, the trailer's
    footer CRC32C, and a measured share of the block payloads' bits."""

    def setUp(self):
        self.stores = {store["name"]: store
                       for store in json.loads(ALL_BITS.read_text(encoding="utf-8"))["stores"]}

    def regions(self, layout):
        container = layout["container"]
        footer = container["footer_offset"]
        trailer = layout["file_bytes"] - container["trailer_bytes"]
        payload = {at for block in layout["block_list"]
                   for at in range(block["payload_offset"],
                                   block["payload_offset"] + block["payload_bytes"])}
        return footer, trailer, payload

    def test_the_stores_are_the_streams_named(self):
        for name, stream, segment_records, _ in all_bit_sweep.STORES:
            with self.subTest(store=name):
                store = self.stores[name]
                self.assertEqual(store["source_sha256"], hashlib.sha256(
                    all_bit_sweep.source_of(stream)).hexdigest())
                self.assertEqual(store["layout"]["segment_records"], segment_records)
        self.assertEqual(self.stores["oracle"]["layout"], fixture()[0])
        self.assertEqual((self.stores["bigger"]["layout"]["records"],
                          self.stores["bigger"]["layout"]["segments"],
                          self.stores["bigger"]["layout"]["file_bytes"]), (79, 5, 6793))

    def test_with_checksums_every_flip_is_refused(self):
        for name, store in self.stores.items():
            with self.subTest(store=name):
                self.assertEqual(store["passed"]["crcs_on"], [])

    def test_without_checksums_a_flip_passes_only_where_the_structure_does_not_catch_it(self):
        for name, store in self.stores.items():
            with self.subTest(store=name):
                layout = store["layout"]
                footer, trailer, payload = self.regions(layout)
                passed = {tuple(pair) for pair in store["passed"]["crcs_off"]}
                crcs = {(footer + at, bit) for at in range(trailer - footer)
                        if g2_plan.unread(layout, at) for bit in range(8)}
                self.assertEqual({pair for pair in passed if footer <= pair[0] < trailer}, crcs)
                self.assertEqual(len(crcs), ENTRIES[name] * 4 * 8)
                # The trailer's CRC32C of the footer, bytes 8 to 11 (container.hpp's
                # kTrailerFooterCrc), and nothing else of it.
                self.assertEqual({pair for pair in passed if pair[0] >= trailer},
                                 {(trailer + 8 + at, bit) for at in range(4) for bit in range(8)})
                # Nothing of the header or the sections.
                self.assertEqual({pair for pair in passed if pair[0] < footer} - {
                    pair for pair in passed if pair[0] in payload}, set())
                self.assertEqual(8 * len(payload), PAYLOAD_PAIRS[name])
                self.assertEqual(len([pair for pair in passed if pair[0] in payload]),
                                 PAYLOAD_PASSED[name])


class StandInTool:
    """tick_roundtrip as check() calls it, in this process: `ingest` writes a store of the
    fixture's size, `layout` gives the fixture's layout, and `verify` finds the one flipped byte
    and answers as the real tool does, a CRC failure naming the block, the record that differs,
    the footer's CRC, and with checksums off the fixture's verdict on each footer byte. `wrong`
    names one answer to give wrongly, so each of check()'s refusals is reached."""

    def __init__(self, layout, swept, wrong=None):
        self.layout, self.caught, self.wrong = layout, dict(swept), wrong
        self.original = None
        self.untouched_verifies = 0

    def answer(self, code, report=None):
        return types.SimpleNamespace(returncode=code, stdout=json.dumps(report or {}),
                                     stderr="")

    def run(self, command, **_):
        name, arguments = command[1], command[2:]
        if name == "ingest":
            if self.wrong == "ingest":
                return self.answer(2)
            data = random.Random(1).randbytes(self.layout["file_bytes"])
            Path(arguments[1]).write_bytes(data)
            self.original = data
            return self.answer(0)
        if name == "layout":
            return self.answer(2) if self.wrong == "layout" else self.answer(0, self.layout)
        return self.verify(Path(arguments[1]).read_bytes(), "--no-checksums" in arguments)

    def verify(self, now, unchecked):
        changed = [i for i in range(len(now)) if now[i] != self.original[i]]
        if not changed:
            self.untouched_verifies += 1
            first = self.untouched_verifies == 1
            if (self.wrong == "g1" and first) or (self.wrong == "restored" and not first):
                return self.answer(1, {"verdict": "mismatch"})
            records = 3 if self.wrong == "g1-report" else 4
            return self.answer(0, {"records": records, "verdict": "pass",
                                   "store_format": "itch-store-v3"})
        at = changed[0]
        footer = self.layout["container"]["footer_offset"]
        if at >= footer:
            if not unchecked:
                if self.wrong == "footer-crc-passes":
                    return self.answer(0)
                error = ("bad block metadata" if self.wrong == "footer-not-crc"
                         else "bad checksum: footer")
                return self.answer(1, {"store_error": error})
            byte = at - footer
            if self.wrong == "sweep-exit" and byte == 3:
                return self.answer(2)
            caught = self.caught[byte] or (self.wrong == "sweep-disagrees" and byte == 48)
            return self.answer(1 if caught else 0)
        block = next(b for b in self.layout["block_list"]
                     if b["payload_offset"] <= at < b["payload_offset"] + b["payload_bytes"])
        if not unchecked:
            if self.wrong == "data-crc-passes":
                return self.answer(0)
            where = "" if self.wrong == "data-crc-unnamed" else f"block {block['block']}, "
            return self.answer(1, {"store_error": f"bad checksum: {where}at offset "
                                                  f"{block['payload_offset']}"})
        if self.wrong == "data-unchecked-passes":
            return self.answer(0)
        if self.wrong == "data-unchecked-unnamed":
            return self.answer(1, {"first_difference": {}})
        return self.answer(1, {"first_difference": {"kind": "bytes", "record_index": 2}})


class LiveRefusals(unittest.TestCase):
    """Review TD-14: each refusal of check(), the oracle step's live G-1 and G-2, reached with a
    stand-in for the tool, so that a refusal removed or inverted fails here and not only on a
    built tool."""

    def check(self, wrong=None, sweep_out=None):
        layout, swept = fixture()
        tool = StandInTool(layout, swept, wrong)
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, folder)
        out = io.StringIO()
        with mock.patch.object(check_oracle, "subprocess",
                               types.SimpleNamespace(run=tool.run)), \
                contextlib.redirect_stdout(out):
            failure = check_oracle.check("tick_roundtrip", folder, sweep_out)
        return failure, out.getvalue()

    def test_the_stand_in_passes_both_gates(self):
        failure, out = self.check()
        self.assertIsNone(failure)
        self.assertIn("footer sweep: 1464 bytes, 1348 caught", out)

    def test_each_wrong_answer_is_refused_with_its_reason(self):
        for wrong, message in (
                ("ingest", "ingest failed"),
                ("g1", "G-1 failed: verify exited 1 on an untouched store"),
                ("g1-report", "G-1 failed: verify reported 3 records, pass, format "
                              "itch-store-v3"),
                ("layout", "layout failed on an untouched store"),
                ("data-crc-passes", "G-2 failed: verify exited 0 on a flipped data bit"),
                ("data-crc-unnamed", "G-2 failed: the CRC failure does not name ['block 13', 'bad "
                                     "checksum: block']: 'bad checksum: at offset 354'"),
                ("data-unchecked-passes", "G-2 failed: verify --no-checksums exited 0 on a "
                                          "flipped data bit"),
                ("data-unchecked-unnamed", "G-2 failed: with CRCs off, verify named neither a "
                                           "record nor the column: {}, ''"),
                ("footer-crc-passes", "G-2 failed: verify exited 0 on a flipped footer bit"),
                ("footer-not-crc", "G-2 failed: the footer flip was not a footer CRC failure: "
                                "'bad block metadata'"),
                ("sweep-exit", "G-2 failed: verify --no-checksums exited 2 with footer byte 3 "
                               "flipped"),
                ("sweep-disagrees", "G-2 failed: the footer sweep disagrees with the rule in 1 "
                                    "places, first: footer byte 48, the first byte of block "
                                    "0's CRC32C, which the rule passes over, was caught"),
                ("restored", "G-2 failed: verify exited 1 on the restored store")):
            with self.subTest(wrong=wrong):
                failure, _ = self.check(wrong)
                self.assertEqual(failure, message)

    def test_the_sweep_is_written_where_asked(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, folder)
        failure, _ = self.check(sweep_out=folder / "sweep.json")
        self.assertIsNone(failure)
        layout, swept = fixture()
        self.assertEqual(json.loads((folder / "sweep.json").read_text(encoding="utf-8")),
                         {"layout": layout, "swept": swept})


if __name__ == "__main__":
    unittest.main()
