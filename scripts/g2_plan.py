#!/usr/bin/env python3
"""Plan G-2's two bit flips from a store's block listing.

    build/bench/tools/tick_roundtrip layout <store> --blocks > layout-blocks.json
    python3 -I scripts/g2_plan.py layout-blocks.json g2.json

The rule is the committed files' (results/roundtrip-*.json, `seed`): the middle segment's
first timestamp block, its middle byte, bit 1; and the footer's middle byte, bit 1. The middle
segment is the listing's segment count halved, rounded down; the footer runs from the
container's footer offset to the trailer. g2.json is what scripts/store_results.py reads
as the flips, and the one line printed, "<data offset> <footer offset>", is the two byte
offsets `tick_roundtrip flip-bit` is given.

It refuses, exit 1, layout output without its block list, a middle segment without exactly
one first timestamp block, and a block or footer that is not inside the file where the
container says.

Standard library only.
"""

import json
import sys
from pathlib import Path

SEED = ("the middle segment's timestamp block, its middle byte, bit 1; the footer's middle "
        "byte, bit 1")
BIT = 1


class Refused(Exception):
    pass


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
    return {"seed": SEED,
            "data": {"block": block["block"], "segment": block["segment"],
                     "column": "timestamp", "block_payload_offset": start,
                     "block_payload_bytes": size, "store_byte_offset": start + size // 2,
                     "bit": BIT},
            "footer": {"footer_offset": footer_offset, "footer_bytes": footer_bytes,
                       "store_byte_offset": footer_offset + footer_bytes // 2, "bit": BIT}}


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
