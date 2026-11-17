#!/usr/bin/env python3
"""Flip every bit of every byte of two small ITCH stores, one at a time on a fresh copy, and
record which flips `verify` and `verify --no-checksums` let pass. Makes all-bit-sweep.json:

    python3 -I tests/scripts/oracle_fixtures/all_bit_sweep.py build/dev/tools/tick_roundtrip \\
        tests/scripts/oracle_fixtures/all-bit-sweep.json

The stores: scripts/check_oracle.py's own, four records at two a segment, and a bigger one,
79 records at 16 a segment, made by bigger_stream() below. Every (byte, bit) the file does not
list exited 1 in that mode; a flip that exits otherwise than 0 or 1 stops the sweep. About
three minutes at eight threads on the Mac. tests/scripts/test_check_oracle.py's AllBits holds
the counts the pages quote to the file. Standard library only.
"""

import concurrent.futures
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_oracle  # noqa: E402

BIGGER_SEGMENT_RECORDS = 16


def bigger_stream():
    """A system event, three stock directories, 60 adds over three locates with a delete after
    every fourth, and a two-byte cut tail."""
    frame, header = check_oracle.frame, check_oracle.header
    records = [frame(header(b"S", 0, 0, 1_000) + b"O")]
    for locate in (7, 8, 9):
        records.append(frame(header(b"H", locate, 1, 2_000 + locate)
                             + f"SYM{locate}    ".encode()[:8] + b"T" + b"\0" + b"    "))
    reference = 1
    for i in range(60):
        locate = 7 + i % 3
        timestamp = 10_000 + i * 977
        records.append(frame(header(b"A", locate, 2, timestamp) + struct.pack(">Q", reference)
                             + b"BS"[i % 2:i % 2 + 1] + struct.pack(">I", 100 + i)
                             + f"SYM{locate}    ".encode()[:8]
                             + struct.pack(">I", 1_234_500 + 100 * i)))
        if i % 4 == 3:
            records.append(frame(header(b"D", locate, 3, timestamp + 5)
                                 + struct.pack(">Q", reference)))
        reference += 1
    return records, b"\x00\x01"


STORES = (("oracle", check_oracle.source_stream, check_oracle.SEGMENT_RECORDS,
           "scripts/check_oracle.py's store: its source_stream(), two records a segment"),
          ("bigger", bigger_stream, BIGGER_SEGMENT_RECORDS,
           "bigger_stream() in tests/scripts/oracle_fixtures/all_bit_sweep.py, 16 records a "
           "segment"))


def source_of(stream):
    records, tail = stream()
    return b"".join(records) + tail


def sweep(tool, work, name, stream, segment_records, what):
    source_bytes = source_of(stream)
    source, store = work / f"{name}.itch", work / f"{name}.tsc"
    source.write_bytes(source_bytes)
    pin = hashlib.sha256(source_bytes).hexdigest()
    subprocess.run([tool, "ingest", str(source), str(store), "--durability", "page_cache",
                    "--segment-records", str(segment_records)], check=True,
                   capture_output=True)
    layout = json.loads(subprocess.run([tool, "layout", str(store), "--blocks"], check=True,
                                       capture_output=True, text=True).stdout)
    original = store.read_bytes()

    def one(at):
        copy = work / f"{name}-{at}.tsc"
        passed = {"crcs_on": [], "crcs_off": []}
        for bit in range(8):
            data = bytearray(original)
            data[at] ^= 1 << bit
            copy.write_bytes(data)
            for mode, extra in (("crcs_on", ()), ("crcs_off", ("--no-checksums",))):
                code = subprocess.run([tool, "verify", str(source), str(copy), "--expect-sha256",
                                       pin, *extra], capture_output=True).returncode
                if code not in (0, 1):
                    raise SystemExit(f"{name}: verify exited {code} at byte {at}, bit {bit}")
                if code == 0:
                    passed[mode].append([at, bit])
        copy.unlink()
        return passed

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        found = list(pool.map(one, range(len(original))))
    return {"name": name, "stream": what, "source_sha256": pin, "layout": layout,
            "passed": {mode: [pair for each in found for pair in each[mode]]
                       for mode in ("crcs_on", "crcs_off")}}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    tool, out = argv[0], Path(argv[1])
    scratch = REPO_ROOT / "build"
    scratch.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="all-bit-sweep-", dir=scratch))
    try:
        stores = [sweep(tool, work, *store) for store in STORES]
    finally:
        shutil.rmtree(work)
    out.write_text(json.dumps({
        "what": "every bit of every byte of each store flipped in turn on a fresh copy, and "
                "verify run with checksums and with --no-checksums; `passed` lists the (byte, "
                "bit) pairs that exited 0, and every other pair exited 1",
        "stores": stores}) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
