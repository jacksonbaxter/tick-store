#!/usr/bin/env python3
"""Gates G-1 and G-2 end to end on a small stream, through the real tool.

    python3 -I scripts/check_oracle.py path/to/tick_roundtrip

Writes a few ITCH messages and a cut tail under the gitignored build/ folder, stores them with
`tick_roundtrip ingest`, and requires `verify` to pass against the pin (G-1). Then it flips one
stored bit, requires `verify` to exit 1 and name that bit (G-2), and flips it back. No data
file is needed, so this runs everywhere. Exits 1 if either gate fails, naming the location the
oracle reported.

Standard library only.
"""

import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
STORE_HEADER_SIZE = 40
EXIT_PASS = 0
EXIT_MISMATCH = 1
FLIP_BIT = 3
# Bytes into the third record: its prefix, then type, locate, tracking and timestamp.
FLIP_RECORD = 2
FLIP_OFFSET_IN_RECORD = 9


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


def check(tool, folder):
    records, tail = source_stream()
    source_bytes = b"".join(records) + tail
    source = folder / "source.itch"
    store = folder / "source.stub"
    source.write_bytes(source_bytes)
    pin = hashlib.sha256(source_bytes).hexdigest()

    if run(tool, "ingest", str(source), str(store)).returncode != EXIT_PASS:
        return "ingest failed"

    verify = ("verify", str(source), str(store), "--expect-sha256", pin)
    done = run(tool, *verify)
    if done.returncode != EXIT_PASS:
        return f"G-1 failed: verify exited {done.returncode} on an untouched store"
    report = json.loads(done.stdout)
    if report.get("records") != len(records) or report.get("verdict") != "pass":
        return f"G-1 failed: verify reported {report.get('records')} records, " \
               f"{report.get('verdict')}"

    source_offset = sum(map(len, records[:FLIP_RECORD])) + FLIP_OFFSET_IN_RECORD
    flip(store, STORE_HEADER_SIZE + source_offset, FLIP_BIT)
    done = run(tool, *verify)
    flip(store, STORE_HEADER_SIZE + source_offset, FLIP_BIT)
    if done.returncode != EXIT_MISMATCH:
        return f"G-2 failed: verify exited {done.returncode} on a flipped bit"
    first = json.loads(done.stdout).get("first_difference") or {}
    expected = {"kind": "bytes", "record_index": FLIP_RECORD,
                "source_byte_offset": source_offset, "bit": FLIP_BIT}
    wrong = {key: first.get(key) for key, value in expected.items() if first.get(key) != value}
    if wrong:
        return f"G-2 failed: verify named the wrong place: {wrong}, expected {expected}"
    return None


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    scratch = REPO_ROOT / "build"
    scratch.mkdir(exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="check-oracle-", dir=scratch))
    try:
        failure = check(argv[0], folder)
    finally:
        shutil.rmtree(folder)
    if failure:
        print(failure, file=sys.stderr)
        return 1
    print("G-1 and G-2 hold on the small stream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
