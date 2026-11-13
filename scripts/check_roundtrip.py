#!/usr/bin/env python3
"""Validate round-trip results files: every exit code a gate is judged on was observed.

    python3 -I scripts/check_roundtrip.py results/roundtrip-*.json

scripts/store_results.py writes each run's exit status, read from the .exit file the runner
wrote beside it, as the run's exit_code, and G-2's exit_code_corrupt and exit_code_restored from
those runs. scripts/targets.json judges G-2 on the two summaries, so this refuses a file whose
summaries are not borne out by its runs:

  - every run (G-1's ingest and verify, the store from another host's verify, each of G-2's
    four corrupt runs and its restored run) holds an integer exit_code;
  - a verify's exit_code agrees with its verdict, as the tool's contract has it: 0 on a pass,
    1 on a mismatch; an ingest's is 0;
  - G-2's exit_code_corrupt equals each corrupt run's exit_code, and exit_code_restored the
    restored run's.

Files assembled before the runs' exit codes were kept, from the commits in UNRECORDED_COMMITS,
hold the two summaries alone. They are taken as they are until the freeze rerun replaces them;
docs/TASKS.md holds the item to empty the list then. Exit 0 if every file passes, 1 if any
does not, 2 on a usage error.

Standard library only.
"""

import json
import sys
from pathlib import Path

# The committed round-trip files' commit, whose assembler typed G-2's two exit codes. The raw
# folders of those runs hold .exit files with the same codes (docs/NOTES.md, The generator's
# findings), but the files do not carry them.
UNRECORDED_COMMITS = ("1669ae0",)
VERDICT_EXIT = {"pass": 0, "mismatch": 1}
G2_CORRUPT = ("data_crc", "data_no_checksums", "footer_crc", "footer_no_checksums")


def code_of(run, where, problems):
    code = run.get("exit_code") if isinstance(run, dict) else None
    if isinstance(code, bool) or not isinstance(code, int):
        problems.append(f"{where}: no exit_code observed")
        return None
    return code


def check_verify(run, where, problems):
    code = code_of(run, where, problems)
    wanted = VERDICT_EXIT.get(run.get("verdict")) if isinstance(run, dict) else None
    if code is not None and code != wanted:
        problems.append(f"{where}: exit_code {code} with the verdict {run.get('verdict')!r}, "
                        f"which verify exits {wanted} on")
    return code


def unrecorded(document):
    commit = document.get("commit")
    return isinstance(commit, str) and any(commit.startswith(old) for old in UNRECORDED_COMMITS)


def check(document):
    """The problems with one round-trip document, or an empty list."""
    if not isinstance(document, dict):
        return ["not a JSON object"]
    if unrecorded(document):
        return []
    problems = []
    g1 = document.get("g1")
    if not isinstance(g1, dict):
        return ["no g1 block"]
    ingest = g1.get("ingest")
    code = code_of(ingest, "g1.ingest", problems)
    if code not in (None, 0):
        problems.append(f"g1.ingest: exit_code {code}, not 0")
    check_verify(g1.get("verify"), "g1.verify", problems)
    if "cross_host" in document:
        check_verify((document["cross_host"] or {}).get("verify"), "cross_host.verify", problems)
    g2 = document.get("g2")
    if g2 is None:
        return problems
    if not isinstance(g2, dict):
        return problems + ["g2 is not an object"]
    for name in G2_CORRUPT:
        code = check_verify(g2.get(name), f"g2.{name}", problems)
        if code is not None and code != g2.get("exit_code_corrupt"):
            problems.append(f"g2.exit_code_corrupt is {g2.get('exit_code_corrupt')!r}, and "
                            f"g2.{name} exited {code}")
    code = check_verify(g2.get("restored"), "g2.restored", problems)
    if code is not None and code != g2.get("exit_code_restored"):
        problems.append(f"g2.exit_code_restored is {g2.get('exit_code_restored')!r}, and "
                        f"g2.restored exited {code}")
    return problems


def main(argv=None):
    paths = sys.argv[1:] if argv is None else argv
    if not paths:
        print("usage: check_roundtrip.py FILE...", file=sys.stderr)
        return 2
    failed = False
    for name in paths:
        try:
            document = json.loads(Path(name).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"{name}: {error}", file=sys.stderr)
            failed = True
            continue
        for problem in check(document):
            print(f"{name}: {problem}", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
