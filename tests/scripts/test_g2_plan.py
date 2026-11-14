#!/usr/bin/env python3
"""Tests for scripts/g2_plan.py. Run with:

    python3 -I tests/scripts/test_g2_plan.py

The script runs as a subprocess on stand-ins of `tick_roundtrip layout --blocks` output under
the repo's gitignored build/ directory. Its footer constants are read from the module and
compared with the container's header.
"""

import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "g2_plan.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_g2_plan"
CONTAINER_HPP = REPO_ROOT / "include" / "tick_store" / "store" / "container.hpp"

SPEC = importlib.util.spec_from_file_location("g2_plan", SCRIPT)
g2_plan = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(g2_plan)

SEED = ("the middle segment's timestamp block, its middle byte, bit 1; the footer's middle "
        "byte, bit 1")


def moved_seed(moved, which, index, kind="block"):
    return (f"{SEED}, moved {moved} on because it fell in a {kind} entry's CRC32C, the {which} "
            f"byte of {kind} {index}'s")


def blocks(segments=5, footer_offset=90_000, trailer=24, file_bytes=100_000, sections=3):
    """A store of `segments` segments, each with a type block and three timestamp pieces. The
    blocks' metadata takes the footer's bytes the entries do not."""
    listing = []
    offset = 16
    for segment in range(segments):
        for column, piece in (("type", 0), ("timestamp", 0), ("timestamp", 1), ("locate", 0)):
            size = 1000 + 10 * len(listing)
            listing.append({"block": len(listing), "segment": segment, "column": column,
                            "piece": piece, "codec": "pfor", "payload_offset": offset,
                            "payload_bytes": size})
            offset += size
    footer_bytes = file_bytes - trailer - footer_offset
    entries = 24 + 32 * len(listing) + 64 * sections
    return {"tool": "tick_roundtrip", "command": "layout", "store_format": "itch-store-v3",
            "segments": segments, "blocks": len(listing), "file_bytes": file_bytes,
            "container": {"header_bytes": 16, "trailer_bytes": trailer,
                          "footer_fixed_bytes": 24, "block_entry_bytes": 32 * len(listing),
                          "section_entry_bytes": 64 * sections,
                          "block_metadata_bytes": footer_bytes - entries,
                          "footer_offset": footer_offset},
            "block_list": listing}


def middle_at(middle, segments=5, sections=3):
    """The store of blocks() with a footer whose middle byte is `middle` bytes into it."""
    layout = blocks(segments, sections=sections)
    footer_bytes = 2 * middle
    container = layout["container"]
    metadata = footer_bytes - 24 - container["block_entry_bytes"] - 64 * sections
    assert metadata >= 0, f"no footer of {footer_bytes} bytes holds the entries"
    container["block_metadata_bytes"] = metadata
    layout["file_bytes"] = container["footer_offset"] + footer_bytes + container["trailer_bytes"]
    return layout


def entry_byte(block, byte):
    """Where byte `byte` of block `block`'s entry is in the footer."""
    return 24 + 32 * block + byte


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

    def footer_flip(self, layout):
        """The plan's seed and footer flip, as a byte of the footer."""
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 0, done.stderr)
        plan = json.loads(self.out.read_text(encoding="utf-8"))
        return plan["seed"], plan["footer"]["store_byte_offset"] - plan["footer"]["footer_offset"]

    def refused(self, layout, message):
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 1)
        self.assertIn(message, done.stderr)
        self.assertFalse(self.out.exists())

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
        # then 24 of trailer, to the 1758234758 bytes of the file. The footer's parts are the
        # layout's at the freeze commit; its middle byte is byte 16 of block 78417's entry, the
        # payload size, so the flip stays there.
        layout = {"tool": "tick_roundtrip", "command": "layout", "segments": 65,
                  "blocks": 112337, "file_bytes": 1753215965 + 5018769 + 24,
                  "container": {"header_bytes": 16, "trailer_bytes": 24,
                                "footer_fixed_bytes": 24, "block_entry_bytes": 3594784,
                                "section_entry_bytes": 192, "block_metadata_bytes": 1423769,
                                "footer_offset": 1753215965},
                  "block_list": [{"block": 56536, "segment": 32, "column": "locate", "piece": 0,
                                  "payload_offset": 878100000, "payload_bytes": 90723},
                                 {"block": 56537, "segment": 32, "column": "timestamp",
                                  "piece": 0, "payload_offset": 878190723,
                                  "payload_bytes": 46123}]}
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, "878213784 1755725349\n")
        self.assertEqual(json.loads(self.out.read_text(encoding="utf-8"))["seed"], SEED)

    def test_the_second_days_footer_flip_moves_past_the_crc_it_fell_in(self):
        # results/roundtrip-10302019.json: segment 35 of 71, block 61775 at 963248207 for 46740
        # bytes; the footer at 1929866884 for 5489702 bytes. Its middle byte, 1932611735, is
        # the last byte of block 85775's CRC32C, so the flip is the codec id after it, and the
        # seed is that file's, word for word.
        layout = {"tool": "tick_roundtrip", "command": "layout", "segments": 71,
                  "blocks": 122886, "file_bytes": 1929866884 + 5489702 + 24,
                  "container": {"header_bytes": 16, "trailer_bytes": 24,
                                "footer_fixed_bytes": 24, "block_entry_bytes": 3932352,
                                "section_entry_bytes": 192, "block_metadata_bytes": 1557134,
                                "footer_offset": 1929866884},
                  "block_list": [{"block": 61775, "segment": 35, "column": "timestamp",
                                  "piece": 0, "payload_offset": 963248207,
                                  "payload_bytes": 46740}]}
        done = self.run_script(layout)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, "963271577 1932611736\n")
        plan = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertEqual(plan["seed"], SEED + ", moved one byte on because it fell in a block "
                         "entry's CRC32C, the last byte of block 85775's")
        self.assertEqual(plan["footer"], {"footer_offset": 1929866884, "footer_bytes": 5489702,
                                          "store_byte_offset": 1932611736, "bit": 1})

    def test_a_middle_byte_in_a_block_entrys_crc_moves_to_the_byte_after_it(self):
        # Bytes 24 to 27 of an entry are its CRC32C, byte 28 its codec id.
        for byte, moved, which in ((24, "four bytes", "first"), (25, "three bytes", "second"),
                                   (26, "two bytes", "third"), (27, "one byte", "last")):
            with self.subTest(byte=byte):
                seed, flipped = self.footer_flip(middle_at(entry_byte(15, byte)))
                self.assertEqual(flipped, entry_byte(15, 28))
                self.assertEqual(seed, moved_seed(moved, which, 15))

    def test_a_middle_byte_beside_a_block_entrys_crc_stays_where_it_is(self):
        # Byte 23 is the metadata size's last, byte 28 the codec id: the decoder reads both.
        for byte in (23, 28):
            with self.subTest(byte=byte):
                seed, flipped = self.footer_flip(middle_at(entry_byte(15, byte)))
                self.assertEqual((seed, flipped), (SEED, entry_byte(15, byte)))

    def test_a_middle_byte_past_the_block_entries_stays_where_it_is(self):
        layout = blocks()
        sections_at = 24 + layout["container"]["block_entry_bytes"]
        # A section entry's size, the byte before its CRC32C, its name's length and the
        # blocks' metadata.
        for at in (sections_at + 64 + 8, sections_at + 64 + 15, sections_at + 64 + 20,
                   sections_at + 3 * 64 + 40):
            with self.subTest(at=at):
                self.assertEqual(self.footer_flip(middle_at(at)), (SEED, at))

    def test_a_middle_byte_in_the_footers_fixed_part_stays_where_it_is(self):
        # A store with a timestamp block has a 32-byte entry, so its footer's middle byte is at
        # byte 28 or later; a footer of no entries has it in the counts.
        layout = {"blocks": 0, "container": {"footer_fixed_bytes": 24, "block_entry_bytes": 0,
                                             "section_entry_bytes": 0,
                                             "block_metadata_bytes": 16}}
        self.assertEqual(g2_plan.footer_flip(layout, 1000, 40), (20, SEED))

    def test_a_middle_byte_in_a_section_entrys_crc_moves_to_the_byte_after_it(self):
        # Bytes 16 to 19 of a section entry are its CRC32C, byte 20 its name's length.
        sections_at = 24 + blocks()["container"]["block_entry_bytes"]
        for byte, moved, which in ((16, "four bytes", "first"), (17, "three bytes", "second"),
                                   (18, "two bytes", "third"), (19, "one byte", "last")):
            with self.subTest(byte=byte):
                seed, flipped = self.footer_flip(middle_at(sections_at + 64 + byte))
                self.assertEqual(flipped, sections_at + 64 + 20)
                self.assertEqual(seed, moved_seed(moved, which, 1, "section"))

    def test_the_byte_moved_to_is_the_first_read_one_from_any_byte(self):
        # Every byte of the footer of blocks(): a byte in a CRC32C goes to the byte after the
        # field, every other byte stays.
        layout = blocks()
        footer_bytes = 100_000 - 24 - 90_000
        sections_at = 24 + layout["container"]["block_entry_bytes"]
        for at in range(footer_bytes):
            flipped, _ = g2_plan.flip_from(layout, at, footer_bytes)
            if 24 <= at < sections_at and 24 <= (at - 24) % 32 < 28:
                self.assertEqual(flipped, at - (at - 24) % 32 + 28, at)
            elif sections_at <= at < sections_at + 192 and 16 <= (at - sections_at) % 64 < 20:
                self.assertEqual(flipped, at - (at - sections_at) % 64 + 20, at)
            else:
                self.assertEqual(flipped, at, at)

    def test_a_footer_with_no_read_byte_left_is_refused(self):
        # Cut in a block entry's CRC32C: nothing read follows the middle byte.
        layout = blocks()
        with self.assertRaisesRegex(g2_plan.Refused, "no byte of the footer at or after its "
                                                     "byte 50 is read with checksums off"):
            g2_plan.flip_from(layout, 50, 52)

    def test_a_footer_whose_entries_are_not_the_sizes_the_rule_reads_is_refused(self):
        layout = blocks()
        layout["container"]["block_entry_bytes"] = 36 * layout["blocks"]
        layout["container"]["block_metadata_bytes"] -= 4 * layout["blocks"]
        self.refused(layout, "the footer's block entries are 720 bytes for 20 blocks, not 32 "
                             "a block")
        layout = blocks()
        layout["container"]["footer_fixed_bytes"] = 32
        layout["container"]["block_metadata_bytes"] -= 8
        self.refused(layout, "the footer's fixed part is 32 bytes, not the 24 this rule reads")
        layout = blocks()
        layout["container"]["section_entry_bytes"] += 8
        layout["container"]["block_metadata_bytes"] -= 8
        self.refused(layout, "the footer's section entries are 200 bytes, not 64 a section")

    def test_a_footer_whose_parts_do_not_add_up_to_it_is_refused(self):
        layout = blocks()
        layout["container"]["block_metadata_bytes"] += 1
        self.refused(layout, "the footer's parts add to 9977 bytes, and it has 9976")

    def test_the_footer_constants_are_the_containers(self):
        constants = dict(re.findall(r"inline constexpr std::size_t (k\w+) = (\d+);",
                                    CONTAINER_HPP.read_text(encoding="utf-8")))
        constants = {name: int(value) for name, value in constants.items()}
        self.assertEqual({"kFooterFixedSize": g2_plan.FOOTER_FIXED_BYTES,
                          "kBlockEntrySize": g2_plan.BLOCK_ENTRY_BYTES,
                          "kBlockCrc": g2_plan.BLOCK_CRC, "kBlockCodec": g2_plan.BLOCK_CODEC,
                          "kSectionEntrySize": g2_plan.SECTION_ENTRY_BYTES,
                          "kSectionCrc": g2_plan.SECTION_CRC},
                         {name: constants.get(name) for name in (
                             "kFooterFixedSize", "kBlockEntrySize", "kBlockCrc", "kBlockCodec",
                             "kSectionEntrySize", "kSectionCrc")})
        # A CRC32C is 4 bytes, and the next field starts right after it in both entries.
        self.assertEqual(constants["kBlockCodec"] - constants["kBlockCrc"], g2_plan.CRC_BYTES)
        self.assertEqual(constants["kSectionNameLength"] - constants["kSectionCrc"],
                         g2_plan.CRC_BYTES)

    def test_layout_output_without_its_block_list_is_refused(self):
        layout = blocks()
        del layout["block_list"]
        self.refused(layout, "layout --blocks")

    def test_a_middle_segment_without_one_first_timestamp_block_is_refused(self):
        layout = blocks()
        layout["block_list"] = [b for b in layout["block_list"]
                                if not (b["segment"] == 2 and b["column"] == "timestamp")]
        self.refused(layout, "segment 2 has 0 first timestamp blocks")

    def test_a_footer_that_is_not_inside_the_file_is_refused(self):
        self.refused(blocks(footer_offset=100_000), "footer")
        # A data block past the footer is not a data block of this file.
        self.refused(blocks(footer_offset=5_000, file_bytes=10_000), "not before the footer")

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
