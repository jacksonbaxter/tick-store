#!/usr/bin/env python3
"""Check what git tracks. Exits 1 and names every problem if a file breaks a rule.

    python3 -I scripts/check_hygiene.py [--root DIR]

This repository is public, so three rules hold for every tracked file: no absolute home
path, no market data, and nothing over 1 MB except the kinds of result matched below, each a
JSON object under a limit of its own. Only tracked
files count, since the data and the build tree stay on disk by design. The patterns are
generic. They catch the shape of a leak, and name nobody.

A tree exported with git archive, as a sync to the Linux host makes, has no .git and carries a
COMMIT file instead. Every file in it was tracked, so there the check reads every file but
COMMIT and those its .gitignore rules exclude, which were made after the export.

Standard library only. tests/scripts/test_check_hygiene.py shows each rule failing on an
injected fault.
"""

import argparse
import fnmatch
import json
import re
import subprocess
import sys
import tempfile
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
# re-measure keeps its allowance and a new kind of large file has to be argued for here. Each
# kind has a limit of its own, and a file of an allowed kind must be a JSON object at any size,
# so no pattern lets a data file or an unbounded record through.
LARGE_RESULTS = {
    "results/compare/*-12302019-open-*.json": 2_000_000,
    # The SIMD kernel benchmark, 1,141,409 bytes on Linux and 929,369 on macOS at ede0176b10bb:
    # 314 result rows, one a case and kernel, about 3 KB each and 939 KB in all, each with its
    # timer calibration, warm-up, verification, throughput statistics over ten repetitions,
    # latency percentiles and, on Linux, its perf counters; and the header's 628 load lines,
    # 83 KB, and 628 frequency snapshots, measure.sh's start and end for each run. The
    # validator reads every row, and the rules keep each run's load lines with its figures.
    # It holds no market data: every value is a timing, a count, a rate, a frequency, a
    # setting or the commit, and each case's input is generated from the seed it records.
    "results/bench/*-simd_kernel-*.json": 2_000_000,
    # The four-system comparison's full day, 1,102,687 bytes on Linux at ede0176b10bb, where
    # the protocol 1 run it replaced was 811,417: four systems in 11 variants and 22 write
    # classes, each of five queries timed warm over 30 repetitions and cold over 7. The bytes
    # are the cold runs' eviction proofs, 239 KB, one for each cold run, which the validator
    # counts against n; the warm proofs, 62 KB; the per-repetition timings the medians and
    # tails are read from, warm and cold, 43 KB, and the Python floor's, 31 KB; each write's
    # detail, 107 KB, with QuestDB's 66 write-ahead-log states a class, 82 KB, showing its
    # rows applied before the clock stopped, and where its input was when the clock started;
    # and each class's settings and stored settings, 127 KB, the setup the comparison fixed.
    # Protocol 2 added the per-repetition timings, more cold proofs and the input record.
    # It holds no rows or values of the market data: only timings, counts, sizes, settings
    # and hashes. Each query's answer is its row count and SHA-256, a query's parameters a
    # time window and a locate number, and every list of numbers a timing or a count.
    "results/compare/*-12302019-full-*.json": 2_000_000,
}


def tracked_files(root):
    listing = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                             capture_output=True).stdout
    return [name for name in listing.decode().split("\0") if name]


def is_work_tree_top(root):
    done = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=False)
    return done.returncode == 0 and Path(done.stdout.strip()).resolve() == Path(root).resolve()


def exported_files(root):
    """Every file of an exported tree but COMMIT and what its .gitignore files exclude. git
    lists them against an empty repository of its own, so the rules are git's, and only the
    tree's own .gitignore files count, not this machine's global excludes."""
    with tempfile.TemporaryDirectory() as scratch:
        empty = Path(scratch) / "empty"
        subprocess.run(["git", "init", "-q", str(empty)], check=True, capture_output=True)
        listing = subprocess.run(
            ["git", f"--git-dir={empty / '.git'}", f"--work-tree={root}", "ls-files", "-z",
             "--others", "--exclude-per-directory=.gitignore"],
            cwd=root, check=True, capture_output=True).stdout
    return [name for name in listing.decode().split("\0") if name and name != "COMMIT"]


def files_to_check(root):
    """The files and what they are, or None if root is neither a checkout nor an export."""
    if is_work_tree_top(root):
        return tracked_files(root), "tracked files"
    commit_file = Path(root) / "COMMIT"
    if commit_file.is_file():
        commit = commit_file.read_text(encoding="utf-8").strip()[:12]
        return exported_files(root), f"files of the tree exported at {commit}"
    return None


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
    kinds = [pattern for pattern in LARGE_RESULTS if fnmatch.fnmatchcase(name, pattern)]
    if not kinds:
        if len(content) > MAX_BYTES:
            found.append(f"{name}: {len(content)} bytes is over the {MAX_BYTES} byte limit")
        return found
    limit = min(LARGE_RESULTS[pattern] for pattern in kinds)
    if len(content) > limit:
        found.append(f"{name}: {len(content)} bytes is over the {limit} byte limit of its kind, "
                     f"{kinds[0]}")
    if not is_json_object(content):
        found.append(f"{name}: a file of a kind allowed to be large, {kinds[0]}, must be a "
                     "JSON object, and this is not")
    return found


def is_json_object(content):
    try:
        return isinstance(json.loads(content), dict)
    except (ValueError, RecursionError):
        return False


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check what git tracks.")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)

    found = files_to_check(args.root)
    if found is None:
        print("neither the top of a git work tree nor an exported tree with a COMMIT file, so "
              "nothing was checked", file=sys.stderr)
        return 1
    names, kind = found
    if not names:
        print(f"no {kind}, so nothing was checked", file=sys.stderr)
        return 1
    problems = [problem for name in names for problem in problems_in(args.root, name)]
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"{len(names)} {kind} clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
