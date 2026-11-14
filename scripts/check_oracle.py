#!/usr/bin/env python3
"""Gates G-1 and G-2 end to end on a small stream, through the real tool.

    python3 -I scripts/check_oracle.py path/to/tick_roundtrip

Writes a few ITCH messages and a cut tail under the gitignored build/ folder, stores them in the
ITCH store with `tick_roundtrip ingest` at two records a segment, and requires `verify` to pass
against the pin (G-1). Then G-2, each flip undone before the next:

  - a bit in a data block (segment 1's timestamps): with CRCs on, `verify` exits 1 naming a
    block CRC failure at that block's index and file offset; with --no-checksums, it exits 1
    naming the record that differs or the segment and column the decoder refused;
  - a bit in the footer: `verify` exits 1 naming the footer's CRC;
  - bit 1 of every byte of the footer in turn, with --no-checksums, each put back before the
    next: `verify` must exit 1 on every byte scripts/g2_plan.py's rule may flip and 0 on every
    byte it passes over, the CRC32C of each block and section entry, and from every byte of the
    footer the byte the rule moves to must be one `verify` caught. A sweep that did not flip
    each footer byte exactly once is refused. On this store's 1,464-byte footer it takes about
    5 s on the Mac.

Then `verify` passes again on the restored file. No data file is needed, so this runs
everywhere. Exits 1 if either gate fails, naming what the tool reported, and if the tool is
not there: check.sh's oracle step builds the dev preset first, in CI's unit job on both hosts.

    python3 -I scripts/check_oracle.py path/to/tick_roundtrip --sweep-out PATH

also writes the store's layout and each footer byte's verdict to PATH, as
tests/scripts/oracle_fixtures/footer-sweep.json was made.

Standard library only.
"""

import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
EXIT_PASS = 0
EXIT_MISMATCH = 1
SEGMENT_RECORDS = 2
FLIP_BIT = 3
# Segment 1 is the add and the delete.
FLIP_SEGMENT = 1
FLIP_COLUMN = "timestamp"

_SPEC = importlib.util.spec_from_file_location("g2_plan", REPO_ROOT / "scripts" / "g2_plan.py")
g2_plan = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(g2_plan)


def frame(payload):
    return struct.pack(">H", len(payload)) + payload


def header(type_code, locate, tracking, timestamp_ns):
    return type_code + struct.pack(">HH", locate, tracking) + timestamp_ns.to_bytes(6, "big")


def source_stream():
    """Hand-built from the specification's layouts, so that no encoder vouches for itself."""
    records = [
        frame(header(b"S", 0, 0, 1_000) + b"O"),
        frame(header(b"H", 7, 1, 2_000) + b"ACME    " + b"T" + b"\0" + b"    "),
        frame(header(b"A", 7, 2, 3_000) + struct.pack(">Q", 42) + b"B" +
              struct.pack(">I", 100) + b"ACME    " + struct.pack(">I", 1_234_500)),
        frame(header(b"D", 7, 3, 4_000) + struct.pack(">Q", 42)),
    ]
    tail = b"\x00"
    return records, tail


def run(tool, *arguments):
    done = subprocess.run([tool, *arguments], capture_output=True, text=True)
    sys.stdout.write(done.stdout)
    sys.stdout.write(done.stderr)
    return done


def flip(path, offset, bit):
    with path.open("r+b") as file:
        file.seek(offset)
        byte = file.read(1)[0]
        file.seek(offset)
        file.write(bytes([byte ^ (1 << bit)]))


def layout(tool, store):
    done = run(tool, "layout", str(store), "--blocks")
    if done.returncode != EXIT_PASS:
        return None
    return json.loads(done.stdout)


class SweepFailed(Exception):
    pass


def sweep_footer(store, footer_offset, footer_bytes, verify):
    """Flips bit g2_plan.BIT of each footer byte in turn, runs `verify` and puts the byte back.
    Returns [footer byte, True if verify exited 1] in the order flipped."""
    swept = []
    for at in range(footer_bytes):
        flip(store, footer_offset + at, g2_plan.BIT)
        try:
            code = verify()
        finally:
            flip(store, footer_offset + at, g2_plan.BIT)
        if code not in (EXIT_PASS, EXIT_MISMATCH):
            raise SweepFailed(f"verify --no-checksums exited {code} with footer byte {at} "
                              "flipped")
        swept.append([at, code == EXIT_MISMATCH])
    return swept


def judge_footer_sweep(layout, swept):
    """What the sweep shows against g2_plan's rule, as a list of problems."""
    container = layout["container"]
    footer_bytes = layout["file_bytes"] - container["trailer_bytes"] - container["footer_offset"]
    if [at for at, _ in swept] != list(range(footer_bytes)):
        return [f"the sweep flipped {len(swept)} bytes, not each of the footer's "
                f"{footer_bytes} once in order"]
    caught = [was_caught for _, was_caught in swept]
    problems = []
    for at in range(footer_bytes):
        skipped = g2_plan.unread(layout, at)
        if skipped is None and not caught[at]:
            problems.append(f"footer byte {at}, which the rule may flip, passed verify "
                            "--no-checksums flipped")
        elif skipped is not None and caught[at]:
            kind, index, byte = skipped
            problems.append(f"footer byte {at}, the {g2_plan.WHICH_BYTE[byte]} byte of {kind} "
                            f"{index}'s CRC32C, which the rule passes over, was caught")
    for at in range(footer_bytes):
        flipped, _ = g2_plan.flip_from(layout, at, footer_bytes)
        if not caught[flipped]:
            problems.append(f"from footer byte {at} the rule flips byte {flipped}, which "
                            "passed verify --no-checksums flipped")
    return problems


def check(tool, folder, sweep_out=None):
    records, tail = source_stream()
    source_bytes = b"".join(records) + tail
    source = folder / "source.itch"
    store = folder / "source.tsc"
    source.write_bytes(source_bytes)
    pin = hashlib.sha256(source_bytes).hexdigest()

    ingest = ("ingest", str(source), str(store), "--durability", "page_cache",
              "--segment-records", str(SEGMENT_RECORDS))
    if run(tool, *ingest).returncode != EXIT_PASS:
        return "ingest failed"

    verify = ("verify", str(source), str(store), "--expect-sha256", pin)
    unchecked = verify + ("--no-checksums",)
    done = run(tool, *verify)
    if done.returncode != EXIT_PASS:
        return f"G-1 failed: verify exited {done.returncode} on an untouched store"
    report = json.loads(done.stdout)
    if (report.get("records") != len(records) or report.get("verdict") != "pass"
            or report.get("store_format") != "itch-store-v3"):
        return f"G-1 failed: verify reported {report.get('records')} records, " \
               f"{report.get('verdict')}, format {report.get('store_format')}"

    shape = layout(tool, store)
    if shape is None:
        return "layout failed on an untouched store"
    blocks = [b for b in shape["block_list"]
              if b["segment"] == FLIP_SEGMENT and b["column"] == FLIP_COLUMN]
    if len(blocks) != 1:
        return f"no {FLIP_COLUMN} block in segment {FLIP_SEGMENT}"
    block = blocks[0]
    offset = block["payload_offset"] + block["payload_bytes"] - 1

    flip(store, offset, FLIP_BIT)
    checked_run = run(tool, *verify)
    unchecked_run = run(tool, *unchecked)
    flip(store, offset, FLIP_BIT)
    if checked_run.returncode != EXIT_MISMATCH:
        return f"G-2 failed: verify exited {checked_run.returncode} on a flipped data bit"
    error = json.loads(checked_run.stdout).get("store_error") or ""
    wanted = [f"block {block['block']}", "bad checksum: block",
              f"at offset {block['payload_offset']}"]
    missing = [part for part in wanted if part not in error]
    if missing:
        return f"G-2 failed: the CRC failure does not name {missing}: {error!r}"
    if unchecked_run.returncode != EXIT_MISMATCH:
        return f"G-2 failed: verify --no-checksums exited {unchecked_run.returncode} " \
               "on a flipped data bit"
    report = json.loads(unchecked_run.stdout)
    first = report.get("first_difference") or {}
    error = report.get("store_error") or ""
    names_record = first.get("kind") == "bytes" and first.get("record_index") is not None
    names_field = f"segment {FLIP_SEGMENT}, column {FLIP_COLUMN}" in error
    if not (names_record or names_field):
        return f"G-2 failed: with CRCs off, verify named neither a record nor the column: " \
               f"{first}, {error!r}"

    footer = shape["container"]["footer_offset"]
    flip(store, footer, FLIP_BIT)
    done = run(tool, *verify)
    flip(store, footer, FLIP_BIT)
    if done.returncode != EXIT_MISMATCH:
        return f"G-2 failed: verify exited {done.returncode} on a flipped footer bit"
    error = json.loads(done.stdout).get("store_error") or ""
    if "bad checksum: footer" not in error:
        return f"G-2 failed: the footer flip was not a footer CRC failure: {error!r}"

    footer_bytes = shape["file_bytes"] - shape["container"]["trailer_bytes"] - footer
    try:
        swept = sweep_footer(store, footer, footer_bytes, lambda: subprocess.run(
            [tool, *unchecked], capture_output=True, check=False).returncode)
    except SweepFailed as failed:
        return f"G-2 failed: {failed}"
    if sweep_out:
        Path(sweep_out).write_text(json.dumps({"layout": shape, "swept": swept}) + "\n",
                                   encoding="utf-8")
    problems = judge_footer_sweep(shape, swept)
    if problems:
        return (f"G-2 failed: the footer sweep disagrees with the rule in {len(problems)} "
                f"places, first: {problems[0]}")
    print(f"footer sweep: {footer_bytes} bytes, {sum(caught for _, caught in swept)} caught, "
          "the rest the CRC32C of each block and section entry")

    done = run(tool, *verify)
    if done.returncode != EXIT_PASS:
        return f"G-2 failed: verify exited {done.returncode} on the restored store"
    return None


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    sweep_out = None
    if len(argv) == 3 and argv[1] == "--sweep-out":
        argv, sweep_out = argv[:1], argv[2]
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    if not (Path(argv[0]).is_file() and os.access(argv[0], os.X_OK)):
        print(f"no tick_roundtrip at {argv[0]}: build the dev preset first", file=sys.stderr)
        return 1
    scratch = REPO_ROOT / "build"
    scratch.mkdir(exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="check-oracle-", dir=scratch))
    try:
        failure = check(argv[0], folder, sweep_out)
    finally:
        shutil.rmtree(folder)
    if failure:
        print(failure, file=sys.stderr)
        return 1
    print("G-1 and G-2 hold on the small stream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
