#!/usr/bin/env python3
"""Tests for scripts/store_results.py. Run with:

    python3 -I tests/scripts/test_store_results.py

The script turns one day's raw outputs into the committed results, so these feed it a small
hand-made set and require the sizes to add up, the CR-1 and CR-2 verdicts, and a refusal of
every run that does not show what the gates require.
"""

import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("store_results",
                                              REPO_ROOT / "scripts" / "store_results.py")
results = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(results)

PIN = "ab" * 32
TIME = """        12.50 real        11.00 user         1.00 sys
          1000000  maximum resident set size
           500000  peak memory footprint
"""


def verify(verdict="pass", store_error=None, first=None, stored=PIN):
    return {"tool": "tick_roundtrip", "command": "verify", "verdict": verdict, "records": 10,
            "stored_sha256": stored, "expected_sha256": PIN, "store_error": store_error,
            "first_difference": first}


def layout(file_bytes=1000):
    return {"store_format": "itch-store-v3", "records": 10, "segment_records": 4, "segments": 3,
            "file_bytes": file_bytes,
            "container": {"header_bytes": 64, "trailer_bytes": 24, "footer_fixed_bytes": 24,
                          "block_entry_bytes": 100, "section_entry_bytes": 50,
                          "block_metadata_bytes": 30},
            "sections": {"store": 24, "symbols": 14, "tail": 0},
            "columns": [
                {"column": "type", "payload_bytes": 600, "metadata_bytes": 20, "blocks": 3,
                 "zstd_blocks": 0, "values": 10},
                {"column": "import_ref", "payload_bytes": 70, "metadata_bytes": 10, "blocks": 1,
                 "zstd_blocks": 0, "values": 2},
            ]}


def capacity(parquet=3000):
    return {"day": "12302019", "systems": {
        name: {"variant": "v", "stored_bytes": size, "outcome": "finished"}
        for name, size in (("parquet", parquet), ("arcticdb", 6000), ("questdb", 20000))}}


class StoreResults(unittest.TestCase):
    def setUp(self):
        scratch = REPO_ROOT / "build"
        scratch.mkdir(exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(prefix="test-store-results-", dir=scratch))
        self.write("ingest.json", {"tool": "tick_roundtrip", "command": "ingest", "bytes": 2500,
                                   "stored_file_bytes": 1000, "durability": "page_cache",
                                   "segment_records": 4})
        self.write("layout.json", layout())
        self.write("verify.json", verify())
        self.write("g2.json", {"seed": 1, "data": {"block": 5, "block_payload_offset": 300,
                                                   "column": "timestamp",
                                                   "store_byte_offset": 310, "bit": 1},
                               "footer": {"store_byte_offset": 900, "bit": 1}})
        self.write("g2-data-crc.json", verify(
            "mismatch", "container: block, segment 1, column timestamp, block 5: expected 0, "
                        "have 0 (bad checksum: block at offset 300: expected 1, have 2)", stored=None))
        self.write("g2-data-nocrc.json", verify("mismatch", first={"kind": "bytes"}, stored="cd"))
        self.write("g2-footer-crc.json", verify(
            "mismatch", "container: container (bad checksum: footer at offset 900)", stored=None))
        self.write("g2-footer-nocrc.json", verify("mismatch", "bad footer", stored=None))
        self.write("g2-restored.json", verify())
        for stem in ("ingest", "verify", "g2-data-crc", "g2-data-nocrc", "g2-footer-crc",
                     "g2-footer-nocrc", "g2-restored"):
            (self.dir / f"{stem}.time").write_text(TIME)
        for stem in ("g2-data-crc", "g2-data-nocrc", "g2-footer-crc", "g2-footer-nocrc"):
            (self.dir / f"{stem}.err").write_text("store error\n")
        # What each run exited with, as the runner records it: verify exits 1 on a mismatch.
        for stem, code in (("ingest", 0), ("verify", 0), ("g2-data-crc", 1),
                           ("g2-data-nocrc", 1), ("g2-footer-crc", 1), ("g2-footer-nocrc", 1),
                           ("g2-restored", 0)):
            (self.dir / f"{stem}.exit").write_text(f"{code}\n")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def write(self, name, document):
        (self.dir / name).write_text(json.dumps(document))

    def assemble(self, day="12302019", cap=None):
        return results.assemble(self.dir, day, "abc1234", "macOS",
                                capacity() if cap is None else cap)

    def test_query_index_bytes_come_from_the_seek_and_run_directory_columns(self):
        document = layout()
        document["columns"][0]["payload_bytes"] = 540
        document["columns"] += [
            {"column": "locate", "payload_bytes": 20, "metadata_bytes": 0, "blocks": 3,
             "zstd_blocks": 0, "values": 10},
            {"column": "seek", "payload_bytes": 30, "metadata_bytes": 0, "blocks": 3,
             "zstd_blocks": 0, "values": 40},
            {"column": "locate_run", "payload_bytes": 10, "metadata_bytes": 0, "blocks": 3,
             "zstd_blocks": 0, "values": 4},
        ]
        self.write("layout.json", document)
        _, size = self.assemble()
        index = size["query_index"]
        self.assertEqual(index["index_bytes"], 40)
        self.assertEqual(sorted(index["columns"]), ["locate_run", "seek"])
        self.assertAlmostEqual(index["fraction_of_file"], 0.04)
        self.assertEqual(index["locate_column"]["payload_bytes"], 20)

    def test_sizes_ratio_and_verdicts(self):
        roundtrip, size = self.assemble()
        self.assertEqual(size["stored_bytes"], 1000)
        self.assertAlmostEqual(size["ratio"], 2.5)
        self.assertEqual(size["parts"]["footer_total"], 204)
        self.assertEqual(size["segment_state"]["import_ref_payload_bytes"], 70)
        self.assertEqual(size["segment_state"]["imports"], 2)
        self.assertEqual(size["cr1"]["verdict"], "pass")
        self.assertEqual(size["cr2"]["verdict"], "pass")
        self.assertTrue(size["cr2"]["systems"]["parquet"]["smaller"])
        self.assertEqual(roundtrip["g1"]["verdict"], "pass")
        self.assertEqual(roundtrip["g1"]["ingest"]["real_seconds"], 12.5)
        self.assertEqual(roundtrip["g1"]["verify"]["peak_memory_footprint_bytes"], 500000)
        self.assertNotIn("tool", roundtrip["g1"]["verify"])
        self.assertEqual(roundtrip["g2"]["restored"]["verdict"], "pass")
        self.assertEqual(roundtrip["g2"]["data_crc"]["stderr"], "store error")

    def test_cr1_fails_below_the_floor_and_cr2_when_a_system_is_smaller(self):
        self.write("ingest.json", {"bytes": 2000, "stored_file_bytes": 1000,
                                   "durability": "page_cache", "segment_records": 4})
        _, size = self.assemble(cap=capacity(parquet=999))
        self.assertEqual(size["cr1"]["verdict"], "fail")
        self.assertEqual(size["cr2"]["verdict"], "fail")

    def test_the_second_day_has_no_cr1_floor_and_no_comparison(self):
        _, size = self.assemble(day="10302019")
        self.assertEqual(size["cr1"]["verdict"], "not a CR-1 day")
        self.assertAlmostEqual(size["cr1"]["gz_ratio"], 9_035_016_843 / 3_872_931_242)
        self.assertEqual(size["cr2"]["verdict"], "no comparison")

    def test_refusals(self):
        cases = {
            "verify.json": verify("mismatch"),
            "g2-restored.json": verify("mismatch"),
            "g2-data-nocrc.json": verify("pass"),
            "g2-footer-crc.json": verify("mismatch", "bad footer"),
        }
        for name, document in cases.items():
            with self.subTest(name=name):
                original = (self.dir / name).read_text()
                self.write(name, document)
                with self.assertRaises(results.Refused):
                    self.assemble()
                (self.dir / name).write_text(original)
        # A CRC failure naming another block.
        self.write("g2-data-crc.json", verify(
            "mismatch", "block 6: (bad checksum: block at offset 300)", stored=None))
        with self.assertRaises(results.Refused):
            self.assemble()

    def test_a_layout_that_does_not_add_up_is_refused(self):
        self.write("layout.json", layout(file_bytes=1001))
        self.write("ingest.json", {"bytes": 2500, "stored_file_bytes": 1001,
                                   "durability": "page_cache", "segment_records": 4})
        with self.assertRaises(results.Refused):
            self.assemble()

    def test_g1_alone_and_gnu_time(self):
        (self.dir / "verify.time").write_text(
            "\tElapsed (wall clock) time (h:mm:ss or m:ss): 1:02.50\n"
            "\tMaximum resident set size (kbytes): 2000\n")
        self.write("verify.json", {**verify(), "store_format": "itch-store-v3"})
        roundtrip = results.assemble_g1(self.dir, "12302019", "abc1234", "Linux")
        self.assertEqual(roundtrip["g1"]["verify"]["real_seconds"], 62.5)
        self.assertEqual(roundtrip["g1"]["verify"]["time_peak_rss_bytes"], 2_048_000)
        self.assertNotIn("g2", roundtrip)
        for stem in ("ingest", "verify"):
            (self.dir / f"{stem}.time").unlink()
        untimed = results.assemble_g1(self.dir, "12302019", "abc1234", "Linux")
        self.assertNotIn("real_seconds", untimed["g1"]["verify"])
        self.write("verify.json", verify("mismatch"))
        with self.assertRaises(results.Refused):
            results.assemble_g1(self.dir, "12302019", "abc1234", "Linux")

    def test_a_store_from_the_other_host_must_verify_here(self):
        self.write("verify.json", {**verify(), "store_format": "itch-store-v3"})
        self.write("cross-verify.json", verify())
        (self.dir / "cross-verify.time").write_text(TIME)
        (self.dir / "cross-verify.exit").write_text("0\n")
        (self.dir / "cross-host.txt").write_text("macOS\n")
        (self.dir / "store.sha256").write_text("ee\n")
        (self.dir / "cross-store.sha256").write_text("ee\n")
        roundtrip = results.assemble_g1(self.dir, "12302019", "abc1234", "Linux", "pinned")
        self.assertEqual(roundtrip["placement"], "pinned")
        self.assertEqual(roundtrip["cross_host"]["written_on"], "macOS")
        self.assertTrue(roundtrip["cross_host"]["same_store_bytes"])
        self.assertEqual(roundtrip["cross_host"]["verify"]["real_seconds"], 12.5)
        (self.dir / "cross-store.sha256").write_text("ff\n")
        crossed = results.assemble_g1(self.dir, "12302019", "abc1234", "Linux")
        self.assertFalse(crossed["cross_host"]["same_store_bytes"])
        self.assertNotIn("placement", crossed)
        self.write("cross-verify.json", verify("mismatch", stored="cd"))
        with self.assertRaises(results.Refused):
            results.assemble_g1(self.dir, "12302019", "abc1234", "Linux")
        self.write("cross-verify.json", verify())
        (self.dir / "cross-host.txt").unlink()
        with self.assertRaises(results.Refused):
            results.assemble_g1(self.dir, "12302019", "abc1234", "Linux")

    def test_a_missing_file_is_refused(self):
        (self.dir / "g2-restored.time").unlink()
        with self.assertRaises(results.Refused):
            self.assemble()

    def test_each_exit_code_is_the_one_its_run_exited_with(self):
        roundtrip, _ = self.assemble()
        self.assertEqual(roundtrip["g1"]["ingest"]["exit_code"], 0)
        self.assertEqual(roundtrip["g1"]["verify"]["exit_code"], 0)
        for name in ("data_crc", "data_no_checksums", "footer_crc", "footer_no_checksums"):
            self.assertEqual(roundtrip["g2"][name]["exit_code"], 1, name)
        self.assertEqual(roundtrip["g2"]["restored"]["exit_code"], 0)
        self.assertEqual(roundtrip["g2"]["exit_code_corrupt"], 1)
        self.assertEqual(roundtrip["g2"]["exit_code_restored"], 0)

    def test_a_run_that_exited_with_the_wrong_code_is_refused(self):
        # verify exits 0 on a pass, 1 on a mismatch and 2 on a usage or I/O error: a corrupt
        # store's mismatch printed by a run that exited 2 or 0 is not G-2's exit 1.
        for stem, code in (("g2-data-crc", 2), ("g2-footer-nocrc", 0), ("g2-restored", 1),
                           ("verify", 2), ("ingest", 1)):
            with self.subTest(stem=stem, code=code):
                original = (self.dir / f"{stem}.exit").read_text()
                (self.dir / f"{stem}.exit").write_text(f"{code}\n")
                with self.assertRaises(results.Refused):
                    self.assemble()
                (self.dir / f"{stem}.exit").write_text(original)

    def test_a_run_with_no_exit_code_is_refused(self):
        for text in (None, "", "one\n"):
            with self.subTest(text=text):
                path = self.dir / "g2-footer-crc.exit"
                if text is None:
                    path.unlink()
                else:
                    path.write_text(text)
                with self.assertRaises(results.Refused):
                    self.assemble()
                path.write_text("1\n")


if __name__ == "__main__":
    unittest.main()
