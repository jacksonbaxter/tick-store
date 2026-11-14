#!/usr/bin/env python3
"""Plan G-2's two bit flips from a store's block listing.

    build/bench/tools/tick_roundtrip layout <store> --blocks > layout-blocks.json
    python3 -I scripts/g2_plan.py layout-blocks.json g2.json

The rule is the committed files' (results/roundtrip-*.json, `seed`): the middle segment's
first timestamp block, its middle byte, bit 1; and in the footer, bit 1 of the first byte at
or after its middle byte that the reader reads with checksums off. With checksums on the
footer's CRC32C covers every byte of it; with them off, the reader checks or decodes every
footer byte but the CRC32C of each block entry and of each section entry, which only a
checked read compares (docs/NOTES.md, G-2's footer flip at the freeze, has the table). A flip
in one of those cannot be caught and leaves the data intact, so the flip moves on past it, and
the seed says what the middle byte fell in and how far the flip moved, in the words of
results/roundtrip-10302019.json, whose footer flip was moved one byte on by hand. Where the
middle byte is read, the seed and the flip are the middle byte's, as on 12302019. The middle
segment is the listing's segment count halved, rounded down; the footer runs from the
container's footer offset to the trailer. g2.json is what scripts/store_results.py reads as
the flips, and the one line printed, "<data offset> <footer offset>", is the two byte offsets
`tick_roundtrip flip-bit` is given. scripts/check_oracle.py flips every footer byte of a small
store and holds the tool's verdicts to unread() and flip_from().

It refuses, exit 1, layout output without its block list, a middle segment without exactly
one first timestamp block, a block or footer that is not inside the file where the container
says, a footer whose parts are not the sizes the rule reads (a fixed part of 24 bytes, 32 bytes
of entry a block and 64 a section, as include/tick_store/store/container.hpp has them) or do
not add up to it, and a footer with no byte read at or after its middle.

Standard library only.
"""

import json
import sys
from pathlib import Path

SEED = ("the middle segment's timestamp block, its middle byte, bit 1; the footer's middle "
        "byte, bit 1")
BIT = 1

# The container's footer (include/tick_store/store/container.hpp): three 8-byte counts, then
# one entry a block, then one a section, then the blocks' metadata. A block entry's CRC32C is
# followed by its codec id, a section entry's by its name's length. With checksums off nothing
# reads either CRC32C (src/store/container.cpp, block_with and section_at).
FOOTER_FIXED_BYTES = 24     # kFooterFixedSize
BLOCK_ENTRY_BYTES = 32      # kBlockEntrySize
BLOCK_CRC = 24              # kBlockCrc
BLOCK_CODEC = 28            # kBlockCodec
SECTION_ENTRY_BYTES = 64    # kSectionEntrySize
SECTION_CRC = 16            # kSectionCrc
CRC_BYTES = 4
MOVED = {1: "one byte", 2: "two bytes", 3: "three bytes", 4: "four bytes"}
WHICH_BYTE = ("first", "second", "third", "last")


class Refused(Exception):
    pass


def unread(layout, at):
    """What byte `at` of the footer is, if nothing reads it with checksums off: the kind of
    entry, the entry's index and the byte of its CRC32C. None for a byte the reader reads."""
    container = layout["container"]
    block_entries, section_entries = (container["block_entry_bytes"],
                                      container["section_entry_bytes"])
    entry_at = at - FOOTER_FIXED_BYTES
    if 0 <= entry_at < block_entries:
        index, byte = divmod(entry_at, BLOCK_ENTRY_BYTES)
        if BLOCK_CRC <= byte < BLOCK_CRC + CRC_BYTES:
            return "block", index, byte - BLOCK_CRC
    entry_at -= block_entries
    if 0 <= entry_at < section_entries:
        index, byte = divmod(entry_at, SECTION_ENTRY_BYTES)
        if SECTION_CRC <= byte < SECTION_CRC + CRC_BYTES:
            return "section", index, byte - SECTION_CRC
    return None


def flip_from(layout, at, footer_bytes):
    """The first byte at or after byte `at` of the footer that the reader reads with checksums
    off, and the seed that names it."""
    fell_in = unread(layout, at)
    flipped = at
    while flipped < footer_bytes and unread(layout, flipped) is not None:
        flipped += 1
    if flipped >= footer_bytes:
        raise Refused(f"no byte of the footer at or after its byte {at} is read with "
                      "checksums off")
    if fell_in is None:
        return flipped, SEED
    kind, index, byte = fell_in
    return flipped, (f"{SEED}, moved {MOVED[flipped - at]} on because it fell in a {kind} "
                     f"entry's CRC32C, the {WHICH_BYTE[byte]} byte of {kind} {index}'s")


def footer_flip(layout, footer_offset, footer_bytes):
    """The footer's flip, as a byte of the footer, and the seed that names it."""
    container = layout["container"]
    fixed, block_entries = container["footer_fixed_bytes"], container["block_entry_bytes"]
    section_entries = container["section_entry_bytes"]
    if fixed != FOOTER_FIXED_BYTES:
        raise Refused(f"the footer's fixed part is {fixed} bytes, not the "
                      f"{FOOTER_FIXED_BYTES} this rule reads")
    if block_entries != layout["blocks"] * BLOCK_ENTRY_BYTES:
        raise Refused(f"the footer's block entries are {block_entries} bytes for "
                      f"{layout['blocks']} blocks, not {BLOCK_ENTRY_BYTES} a block")
    if section_entries % SECTION_ENTRY_BYTES:
        raise Refused(f"the footer's section entries are {section_entries} bytes, not "
                      f"{SECTION_ENTRY_BYTES} a section")
    parts = fixed + block_entries + section_entries + container["block_metadata_bytes"]
    if parts != footer_bytes:
        raise Refused(f"the footer's parts add to {parts} bytes, and it has {footer_bytes}")
    return flip_from(layout, footer_bytes // 2, footer_bytes)


def plan(layout):
    if layout.get("command") != "layout" or "block_list" not in layout:
        raise Refused("not the output of tick_roundtrip layout --blocks: no block_list")
    container = layout["container"]
    footer_offset = container["footer_offset"]
    footer_bytes = layout["file_bytes"] - container["trailer_bytes"] - footer_offset
    if footer_bytes <= 0:
        raise Refused(f"the footer at {footer_offset} has {footer_bytes} bytes before the "
                      f"trailer of a {layout['file_bytes']}-byte file")
    middle = layout["segments"] // 2
    found = [block for block in layout["block_list"] if block["segment"] == middle
             and block["column"] == "timestamp" and block["piece"] == 0]
    if len(found) != 1:
        raise Refused(f"segment {middle} has {len(found)} first timestamp blocks, not one")
    block = found[0]
    start, size = block["payload_offset"], block["payload_bytes"]
    if size <= 0 or start < container.get("header_bytes", 0) or start + size > footer_offset:
        raise Refused(f"block {block['block']}, {size} bytes at {start}, is not before the "
                      f"footer at {footer_offset}")
    flipped, seed = footer_flip(layout, footer_offset, footer_bytes)
    return {"seed": seed,
            "data": {"block": block["block"], "segment": block["segment"],
                     "column": "timestamp", "block_payload_offset": start,
                     "block_payload_bytes": size, "store_byte_offset": start + size // 2,
                     "bit": BIT},
            "footer": {"footer_offset": footer_offset, "footer_bytes": footer_bytes,
                       "store_byte_offset": footer_offset + flipped, "bit": BIT}}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print("usage: g2_plan.py LAYOUT_BLOCKS_JSON OUT_JSON", file=sys.stderr)
        return 2
    try:
        flips = plan(json.loads(Path(argv[0]).read_text(encoding="utf-8")))
    except (Refused, OSError, ValueError, KeyError, TypeError) as error:
        print(f"g2_plan: {type(error).__name__ + ': ' if isinstance(error, KeyError) else ''}"
              f"{error}", file=sys.stderr)
        return 1
    Path(argv[1]).write_text(json.dumps(flips, indent=2) + "\n", encoding="utf-8")
    print(flips["data"]["store_byte_offset"], flips["footer"]["store_byte_offset"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
