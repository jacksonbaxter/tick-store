#!/usr/bin/env python3
"""Tests for scripts/g2_plan.py. Run with:

    python3 -I tests/scripts/test_g2_plan.py

The script runs as a subprocess on stand-ins of `tick_roundtrip layout --blocks` output under
the repo's gitignored build/ directory.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "g2_plan.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_g2_plan"

SEED = ("the middle segment's timestamp block, its middle byte, bit 1; the footer's middle "
        "byte, bit 1")


def blocks(segments=5, footer_offset=90_000, trailer=24, file_bytes=100_000):
    """A store of `segments` segments, each with a type block and three timestamp pieces."""
    listing = []
    offset = 16
    for segment in range(segments):
        for column, piece in (("type", 0), ("timestamp", 0), ("timestamp", 1), ("locate", 0)):
            size = 1000 + 10 * len(listing)
            listing.append({"block": len(listing), "segment": segment, "column": column,
                            "piece": piece, "codec": "pfor", "payload_offset": offset,
                            "payload_bytes": size})
            offset += size
    return {"tool": "tick_roundtrip", "command": "layout", "store_format": "itch-store-v3",
            "segments": segments, "blocks": len(listing), "file_bytes": file_bytes,
            "container": {"header_bytes": 16, "trailer_bytes": trailer,
                          "footer_offset": footer_offset},
            "block_list": listing}


class G2Plan(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.layout = self.dir / "layout-blocks.json"
        self.out = self.dir / "g2.json"

    def run_script(self, layout):
        self.layout.write_text(json.dumps(layout), encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), str(self.layout),
                               str(self.out)], capture_output=True, text=True, check=False)

    def test_the_flips_are_the_middle_segments_first_timestamp_block_and_the_footer(self):
        layout = blocks()
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 0, done.stderr)
        # Segment 2 of 5; its first timestamp piece is block 9 of the listing.
        block = layout["block_list"][9]
        self.assertEqual((block["segment"], block["column"], block["piece"]), (2, "timestamp", 0))
        footer_bytes = 100_000 - 24 - 90_000
        plan = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertEqual(plan, {
            "seed": SEED,
            "data": {"block": 9, "segment": 2, "column": "timestamp",
                     "block_payload_offset": block["payload_offset"],
                     "block_payload_bytes": block["payload_bytes"],
                     "store_byte_offset": block["payload_offset"] + block["payload_bytes"] // 2,
                     "bit": 1},
            "footer": {"footer_offset": 90_000, "footer_bytes": footer_bytes,
                       "store_byte_offset": 90_000 + footer_bytes // 2, "bit": 1}})
        # The runner reads the two offsets flip-bit is given from the one line printed.
        self.assertEqual(done.stdout, f"{plan['data']['store_byte_offset']} "
                                      f"{plan['footer']['store_byte_offset']}\n")

    def test_the_committed_days_flips_follow_from_its_layout(self):
        # results/roundtrip-12302019.json's flips: segment 32 of 65 (store-size-12302019.json),
        # block 56537 at 878190723 for 46123 bytes; the footer at 1753215965 for 5018769 bytes,
        # then 24 of trailer, to the 1758234758 bytes of the file.
        layout = {"tool": "tick_roundtrip", "command": "layout", "segments": 65,
                  "file_bytes": 1753215965 + 5018769 + 24,
                  "container": {"header_bytes": 16, "trailer_bytes": 24,
                                "footer_offset": 1753215965},
                  "block_list": [{"block": 56536, "segment": 32, "column": "locate", "piece": 0,
                                  "payload_offset": 878100000, "payload_bytes": 90723},
                                 {"block": 56537, "segment": 32, "column": "timestamp",
                                  "piece": 0, "payload_offset": 878190723,
                                  "payload_bytes": 46123}]}
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, "878213784 1755725349\n")

    def test_layout_output_without_its_block_list_is_refused(self):
        layout = blocks()
        del layout["block_list"]
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 1)
        self.assertIn("layout --blocks", done.stderr)
        self.assertFalse(self.out.exists())

    def test_a_middle_segment_without_one_first_timestamp_block_is_refused(self):
        layout = blocks()
        layout["block_list"] = [b for b in layout["block_list"]
                                if not (b["segment"] == 2 and b["column"] == "timestamp")]
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 1)
        self.assertIn("segment 2 has 0 first timestamp blocks", done.stderr)
        self.assertFalse(self.out.exists())

    def test_a_footer_that_is_not_inside_the_file_is_refused(self):
        done = self.run_script(blocks(footer_offset=100_000))
        self.assertEqual(done.returncode, 1)
        self.assertIn("footer", done.stderr)
        self.assertFalse(self.out.exists())
        # A data block past the footer is not a data block of this file.
        done = self.run_script(blocks(footer_offset=5_000, file_bytes=10_000))
        self.assertEqual(done.returncode, 1)
        self.assertIn("not before the footer", done.stderr)

    def test_the_plan_is_what_store_results_reads(self):
        done = self.run_script(blocks())
        self.assertEqual(done.returncode, 0, done.stderr)
        plan = json.loads(self.out.read_text(encoding="utf-8"))
        # store_results.py's g2_runs: the seed, the data block's number, offset and column,
        # and the footer entry, which it writes out as the injected flips.
        self.assertEqual(sorted(plan), ["data", "footer", "seed"])
        for key in ("block", "block_payload_offset", "column"):
            self.assertIn(key, plan["data"])


if __name__ == "__main__":
    unittest.main()
