#!/usr/bin/env python3
"""Runs the comparison and writes its results file.

    TICK_STORE_MEASURE=/path/to/measure.sh python3 -I compare/run.py

Each system runs as one process directly under the measurement wrapper, one at a time. The
wrapper is never named here: it comes from TICK_STORE_MEASURE. Without it there is no lock, so
the file is marked iteration-only, goes under build/, and scripts/check_compare.py refuses
it. On Linux each system runs as

    <wrapper> taskset -c 4 python3 -I compare/run_system.py ...

and run_system.py adds cpu6 as the run's second CPU. Output goes to files, so nothing depends
on a terminal or an SSH connection. The results file appears only when every system has
finished and check_compare.py has passed it, so a file that exists is complete.

Exit codes: 0 done, 75 the wrapper says retry later (finished systems are kept, and run it
again to go on), 2 usage or setup, anything else what failed.
"""

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import run_bench  # noqa: E402

from compare.layout import PINS_PATH, SLICE_NAME, Layout  # noqa: E402
from compare.queries import CLIPPING_STATEMENT  # noqa: E402
from compare.schema import read_manifest  # noqa: E402
from compare.systems import SYSTEM_NAMES  # noqa: E402

CHECK_COMPARE = REPO_ROOT / "scripts" / "check_compare.py"
RUN_SYSTEM = REPO_ROOT / "compare" / "run_system.py"
DEFAULT_RESULTS_DIR = REPO_ROOT / "results" / "compare"
DEFAULT_ITERATION_DIR = REPO_ROOT / "build" / "compare-iteration"
LINUX_CORE = run_bench.LINUX_CORE
LABEL = ("These numbers come from a five-minute slice and prove that the harness works. They "
         "are not claims about a full day, and no target changes because of them.")
MACOS_STATEMENT = ("Unpinned. No hardware counters. macOS has no thread pinning, and no "
                   "unprivileged hardware counters. Each system gets the same thread budget.")
LINUX_STATEMENT = (f"Each system ran under taskset -c {LINUX_CORE}, and the run then widened its "
                   "own mask to cpu4 and cpu6. Every thread of every system shares those two "
                   "CPUs, client and server alike. QuestDB's JVM has more threads than CPUs.")
LINUX_HEAP = ("QuestDB ran with -Xmx1g for this slice. The heap needed for a full day is "
              "unchecked.")
MACOS_HEAP = "QuestDB ran with -Xmx1g, as on Linux, so that the two runs match."
WARM_METHOD = ("Every store file read through tick_coldcache warm, then residency read at 100% "
               "of pages by mincore before and after all repetitions, with the server running.")
COLD_METHOD = {
    "macOS": ("Service stopped. A fresh read-only MAP_SHARED mapping and msync(MS_INVALIDATE) "
              "per file, repeated until mincore reads 0 on every file. A fresh process then "
              "opens the store, with the server started again where there is one, and runs the "
              "query once. File system metadata stays warm. iostat's machine-wide disk0 delta "
              "must reach the bytes that became resident: a shortfall is conclusive, and an "
              "excess is not."),
    "Linux": ("Service stopped. fsync, then posix_fadvise(POSIX_FADV_DONTNEED) per file, repeated "
              "until mincore reads 0 on every file. A fresh process then opens the store, with "
              "the server started again where there is one, and runs the query once. File "
              "system metadata stays warm. The block device's sectors read must reach the bytes "
              "that became resident: a shortfall is conclusive, and an excess is not."),
}
COLD_INCLUDES = ("Each cold number is the query alone. The server's start and the worker's "
                 "imports fall before the clock, and pages they touched count as resident and "
                 "as device reads.")
BASELINE = ("The slice's source bytes: every ITCH frame with its two-byte length prefix, which "
            "is what BENCHMARKS.md's stored bytes must be able to rebuild.")


def compiler_facts(layout):
    """From the bench build's own cache, with no paths in what is kept."""
    cache = {}
    for line in (layout.tools.parent / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([A-Z_]+):\w+=(.*)$", line)
        if match:
            cache[match[1]] = match[2]
    version = run_bench.run_text([cache["CMAKE_CXX_COMPILER"], "--version"]).splitlines()[0]
    return {"compiler": version, "build_type": cache.get("CMAKE_BUILD_TYPE", ""),
            "compile_flags": cache.get("CMAKE_CXX_FLAGS_RELEASE", "")}


def require_clean_tree(measure):
    """A reported number comes from a commit, and an exported tree has no working changes."""
    if not measure or (REPO_ROOT / "COMMIT").exists():
        return
    status = run_bench.run_text(["git", "-C", str(REPO_ROOT), "status", "--porcelain"])
    if status:
        raise run_bench.Failure(run_bench.EXIT_USAGE, "the tree is dirty: commit first")


def run_one(system, args, layout, measure, parts):
    """Runs a system unless its part is already there. Returns its load lines."""
    part = parts / f"{system}.json"
    lines_path = parts / f"{system}.load"
    if part.exists() and lines_path.exists():
        return json.loads(lines_path.read_text(encoding="utf-8"))
    command = [measure] if measure else []
    if platform.system() == "Linux" and measure:
        command += ["taskset", "-c", str(LINUX_CORE)]
    command += [sys.executable, "-I", str(RUN_SYSTEM), "--system", system, "--part", str(part),
                "--slice", args.slice, *layout.as_arguments()]
    for option in ("write_repetitions", "warm_repetitions", "cold_repetitions", "variants"):
        value = getattr(args, option)
        if value is not None:
            command += [f"--{option.replace('_', '-')}", str(value)]
    logs = layout.work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    stdout_path = logs / f"{system}.stdout"
    stderr_path = logs / f"{system}.stderr"
    with open(stdout_path, "wb") as stdout, open(stderr_path, "wb") as stderr:
        code = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                              check=False).returncode
    text = stderr_path.read_text(encoding="utf-8", errors="replace")
    if code == run_bench.EXIT_RETRY_LATER and measure:
        raise run_bench.Failure(run_bench.EXIT_RETRY_LATER,
                                "retry later: the measurement wrapper did not get the machine")
    if code != 0:
        raise run_bench.Failure(code, f"{system}: exited {code}, see {stderr_path.name}")
    lines = run_bench.clean_load_lines(text) if measure else []
    lines_path.write_text(json.dumps(lines), encoding="utf-8")
    return lines


def build_document(host, args, layout, pin, commit, measure, parts, load_lines):
    linux = host == "Linux"
    systems = {system: json.loads((parts / f"{system}.json").read_text(encoding="utf-8"))
               for system in args.systems}
    truths = [part.pop("truth") for part in systems.values()]
    query_sets = [part.pop("queries") for part in systems.values()]
    if any(item != truths[0] for item in truths) or any(item != query_sets[0]
                                                        for item in query_sets):
        raise run_bench.Failure(1, "the systems did not run the same queries")
    runs = [{"load_lines": lines} for lines in load_lines]
    header = {
        "schema": 1,
        "host": host,
        "measurement": "reported" if measure else "iteration-only",
        "statement": LINUX_STATEMENT if linux else MACOS_STATEMENT,
        "cpu_model": run_bench.cpu_model(host),
        "kernel": f"{platform.system()} {platform.release()}",
        **compiler_facts(layout),
        "commit": commit,
        "core": LINUX_CORE if linux else "not-available",
        "second_thread": "cpu6, used by every system" if linux else "not-available",
        "governor": run_bench.governor_of(host, runs),
        "turbo": run_bench.linux_turbo() if linux else "not-available",
        "pinning": "pinned" if linux else "not-supported",
        "load_lines": [line for lines in load_lines for line in lines],
    }
    manifest = read_manifest(layout.export_dir(args.slice))
    return {
        "header": header,
        "label": LABEL,
        "heap": LINUX_HEAP if linux else MACOS_HEAP,
        "input": {"slice": args.slice, "sha256": pin["sha256"], "messages": pin["message_count"],
                  "bytes": pin["size"], "ratio_baseline": BASELINE, "clipping": CLIPPING_STATEMENT,
                  "export": manifest},
        "method": {"warm": WARM_METHOD, "cold": COLD_METHOD[host], "cold_includes": COLD_INCLUDES,
                   "one_system_at_a_time": True,
                   "input_cache": "the export's column files are warm before every write",
                   "p99": "nearest rank, so a small n gives a value that really occurred"},
        "queries": {"parameters": query_sets[0], "truth": truths[0],
                    "truth_method": "numpy over the exported columns, sharing no code with "
                                    "any system"},
        "systems": systems,
    }


def publish(document, output, measure):
    """Writes the file whole, and only after the validator has passed it."""
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_name(output.name + ".pending")
    pending.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
    if measure:
        checked = subprocess.run(
            [sys.executable, "-I", str(CHECK_COMPARE), "--pending-as", output.name, str(pending)],
            check=False, capture_output=True, text=True)
        if checked.returncode != 0:
            print(checked.stderr, end="", file=sys.stderr)
            raise run_bench.Failure(1, f"the results failed validation: {pending.name} kept")
    os.replace(pending, output)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--slice", default=SLICE_NAME)
    parser.add_argument("--systems", nargs="+", default=list(SYSTEM_NAMES),
                        choices=SYSTEM_NAMES)
    parser.add_argument("--results-dir", type=Path, default=DEFAULT_RESULTS_DIR)
    parser.add_argument("--iteration-dir", type=Path, default=DEFAULT_ITERATION_DIR)
    parser.add_argument("--variants", help="comma-separated, for iteration")
    for name in ("write-repetitions", "warm-repetitions", "cold-repetitions"):
        parser.add_argument(f"--{name}", type=int)
    for name in ("data", "vendor", "work", "tools"):
        parser.add_argument(f"--{name}", type=Path)
    return parser.parse_args(argv)


def run(args):
    measure = os.environ.get(run_bench.MEASURE_ENV)
    host = run_bench.host_label()
    pins = json.loads(PINS_PATH.read_text(encoding="utf-8"))
    pin = pins["slices"].get(args.slice)
    if pin is None:
        raise run_bench.Failure(run_bench.EXIT_USAGE, f"no pin for slice {args.slice}")
    require_clean_tree(measure)
    commit = run_bench.current_commit()
    layout = Layout.from_arguments(args)
    parts = layout.work / "parts" / commit[:12]
    parts.mkdir(parents=True, exist_ok=True)
    load_lines = [run_one(system, args, layout, measure, parts) for system in args.systems]
    document = build_document(host, args, layout, pin, commit, measure, parts, load_lines)
    directory = args.results_dir if measure else args.iteration_dir
    output = directory / f"{host}-{args.slice}-{commit[:12]}.json"
    publish(document, output, measure)
    print(output if measure else f"{run_bench.MEASURE_ENV} is not set, iteration-only: {output}")


def main(argv=None):
    try:
        run(parse_arguments(argv))
    except run_bench.Failure as failure:
        print(f"run: {failure}", file=sys.stderr)
        return failure.code
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, KeyError) as error:
        print(f"run: {error}", file=sys.stderr)
        return run_bench.EXIT_USAGE
    return 0


if __name__ == "__main__":
    sys.exit(main())
