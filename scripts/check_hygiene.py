#!/usr/bin/env python3
"""Check what git tracks. Exits 1 and names every problem if a file breaks a rule.

    python3 -I scripts/check_hygiene.py [--root DIR]

This repository is public, so three rules hold for every tracked file: no absolute home
path, no market data, and nothing over 1 MB except the results matched below. Only tracked
files count, since the data and the build tree stay on disk by design. The patterns are
generic. They catch the shape of a leak, and name nobody.

Standard library only. tests/scripts/test_check_hygiene.py shows each rule failing on an
injected fault.
"""

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 1_000_000

# Two fixtures use these names as the fault they inject, and no real account has them.
PLACEHOLDER_USERS = {b"someone", b"x"}
HOME_PATH = re.compile(rb"/(?:Users|home)/([A-Za-z0-9._-]+)")

# Market data, stores and archives. A fixture small enough to review stays in tests/.
DATA_SUFFIXES = (".gz", ".zip", ".zst", ".tar", ".itch", ".bin", ".parquet", ".arrow",
                 ".feather", ".lmdb", ".mdb", ".stub", ".dat", ".csv.gz", "_ITCH50",
                 ".sparseimage", ".dmg")
DATA_FOLDERS = ("data/",)

# Tracked results that may pass the size limit. Each kind is named by a pattern, so a
# re-measure keeps its allowance and a new kind of large file has to be argued for here.
LARGE_RESULTS = ("results/compare/*-12302019-open-*.json",)


def tracked_files(root):
    listing = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                             capture_output=True).stdout
    return [name for name in listing.decode().split("\0") if name]


def problems_in(root, name):
    path = Path(root) / name
    if not path.is_file():
        return []
    found = []
    content = path.read_bytes()
    for match in HOME_PATH.finditer(content):
        if match.group(1).rstrip(b".-_") not in PLACEHOLDER_USERS:
            line = content.count(b"\n", 0, match.start()) + 1
            found.append(f"{name}:{line}: absolute home path")
    if name.startswith(DATA_FOLDERS) or name.endswith(DATA_SUFFIXES):
        found.append(f"{name}: data file is tracked")
    if len(content) > MAX_BYTES and not any(fnmatch.fnmatchcase(name, pattern)
                                            for pattern in LARGE_RESULTS):
        found.append(f"{name}: {len(content)} bytes is over the {MAX_BYTES} byte limit")
    return found


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check what git tracks.")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)

    names = tracked_files(args.root)
    if not names:
        print("no tracked files, so nothing was checked", file=sys.stderr)
        return 1
    problems = [problem for name in names for problem in problems_in(args.root, name)]
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"{len(names)} tracked files clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
