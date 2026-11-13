#!/usr/bin/env python3
"""Runs the comparison and writes its results file.

    TICK_STORE_MEASURE=/path/to/measure.sh python3 -I compare/run.py

Each system runs as one process directly under the measurement wrapper, one at a time. The
wrapper is never named here: it comes from TICK_STORE_MEASURE. Without it there is no lock, so
the file is marked iteration-only, goes under build/, and scripts/check_compare.py refuses
it. On Linux each system runs as

    <wrapper> taskset -c 4 sh -c <copy /proc/stat, then exec> python3 -I compare/run_system.py ...

and run_system.py adds cpu6 as the run's second CPU. The copy is how busy the sibling of cpu4 was
across the system's run, read against /proc/stat when the wrapper exits. Output goes to files, so
nothing depends on a terminal or an SSH connection. The results file appears only when every
system has finished and check_compare.py has passed it, so a file that exists is complete.

A full-day run, Linux only, is

    TICK_STORE_MEASURE=... python3 -I compare/run.py --day 12302019 --variant-source <slice file>

with the slice results file of the same host and commit, from which compare/variant_rule.py
picks each system's variants. Each system then runs as

    <wrapper> taskset -c 4 sh -c <copy /proc/stat, then exec> python3 -I compare/guard.py
        --report <file> --samples <file> --watch <work> -- python3 -I compare/run_system.py --day

so the guard watches the host as in the capacity check, and its report gives each system's peak
memory. The results file is <host>-<day>-full-<commit>.json.

Off Linux, a full day is tick-store alone, every variant it has:

    TICK_STORE_MEASURE=... python3 -I compare/run.py --day 12302019 --systems tickstore

The four-system day needs Linux, because its warm proof reads each process's storage reads
from /proc. tick-store's day store fits a Mac's memory many times over, so its warm proof is the
slice's: every store file resident before and after the repetitions. Everything else is the
full day as on Linux. There is no guard, which reads /proc, so peak memory comes from
/usr/bin/time -l around the system's run, inside the measured command. The results file is
<host>-<day>-full-tickstore-<commit>.json.

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
import store_results  # noqa: E402

from compare import swap, variant_rule  # noqa: E402
from compare.layout import PINS_PATH, SLICE_NAME, Layout  # noqa: E402
from compare.queries import CLIPPING_STATEMENT, FULL_DAY_STATEMENT  # noqa: E402
from compare.schema import read_manifest  # noqa: E402
from compare.systems import SYSTEM_NAMES  # noqa: E402
from compare.systems.questdb_system import heap_flags  # noqa: E402
from compare.systems.tickstore import FULL_DAY_INPUT_STATEMENT, INPUT_STATEMENT  # noqa: E402

CHECK_COMPARE = REPO_ROOT / "scripts" / "check_compare.py"
RUN_SYSTEM = REPO_ROOT / "compare" / "run_system.py"
GUARD = REPO_ROOT / "compare" / "guard.py"
DEFAULT_RESULTS_DIR = REPO_ROOT / "results" / "compare"
DEFAULT_ITERATION_DIR = REPO_ROOT / "build" / "compare-iteration"
LINUX_CORE = run_bench.LINUX_CORE
LABEL = ("These numbers come from a five-minute slice and prove that the harness works. They "
         "are not claims about a full day, and no target changes because of them.")
FULL_DAY_LABEL = ("A full day: every record of the pinned day in every system, every query as "
                  "BENCHMARKS.md defines it with nothing clipped. These are the numbers the "
                  "targets are judged on.")
FULL_DAY_SUFFIX = "full"
FULL_DAY_BASELINE = ("The day's source bytes: every ITCH frame with its two-byte length prefix, "
                     "the decompressed file, which is what BENCHMARKS.md's stored bytes must be "
                     "able to rebuild.")
FULL_DAY_INPUT_CACHE = (
    "The others' writes read the day's export, 24 GB of column files, chunk by chunk inside "
    "the clock, and this host's page cache cannot hold it, so part of each read comes from the "
    "device. tick-store's write reads the day's ITCH file, 8.25 GB, which is read through "
    "first and must be resident, as on the slice.")
FULL_DAY_WARM = ("Query by query, since QuestDB's store is larger than this host's memory: one "
                 "untimed run first, answer checked, then the processes that answer it, the "
                 "worker, tick-store's query tool or QuestDB's server, must cause 0 bytes of "
                 "storage reads across all repetitions, by read_bytes in each one's "
                 "/proc/<pid>/io; the block device's machine-wide count is kept beside it. An "
                 "attempt where they read any is kept in the result and run again, up to three "
                 "times. Residency of every store file "
                 "is recorded before and after. Before the warm queries the day's ITCH file, "
                 "its export and every store file are evicted from the page cache, each proven "
                 "at 0 resident, so that reclaim does not take the queries' own pages; MemFree "
                 "is recorded either side.")
FULL_DAY_WRITES = ("One write per class and variant, each a sustained run of minutes over the "
                   "whole day, so a write's median is that one write. Three of each would not "
                   "fit the run: see compare/README.md.")
GUARD_STATEMENT = ("Each system ran under compare/guard.py: it samples MemAvailable, free swap, "
                   "swap-in, swap-out and free disk every 2 s, and stops the run on MemAvailable "
                   "under 3 GiB, swap-in over 10 MiB/s for 3 samples in a row, swap exhausted "
                   "with MemAvailable under 3 GiB plus SwapTotal, or free disk under 50 GiB. "
                   "Swap-out alone never stops it, and is recorded per phase. Peak memory is the "
                   "largest VmHWM of each process it saw, and the largest sum of their RSS at a "
                   "sample.")
TICKSTORE_ONLY_SUFFIX = "full-tickstore"
TICKSTORE_ONLY_LABEL = ("A full day of tick-store alone: every record of the pinned day, every "
                        "query as BENCHMARKS.md defines it with nothing clipped, and no other "
                        "system. The four-system full day runs on Linux only; this gives the "
                        "targets whose host is Both their figure on this host. No figure here "
                        "is compared with another system's.")
TICKSTORE_ONLY_HEAP = {"setting": "not-run", "full_day_check": "Linux only",
                       "statement": "QuestDB did not run: tick-store ran alone. The full-day "
                                    "capacity check is Linux only."}
TICKSTORE_ONLY_INPUT_CACHE = (
    "tick-store's write reads the day's ITCH file, 8.25 GB, which is read through first and "
    "must be resident, as on the slice. No other system ran, so no export is read inside a "
    "clock.")
TICKSTORE_ONLY_WARM = (
    "Before the warm queries the day's ITCH file, its export and every store file are evicted "
    "from the page cache, each proven at 0 resident, as on Linux. Then every store file is read "
    "through by tick_coldcache warm, and mincore on a fresh mapping must read 100% of its pages "
    "resident before and after all repetitions of every query: the slice's proof, which holds "
    "on the day because tick-store's day store fits this host's memory many times over. The "
    "per-process proof of the four-system day reads /proc, which this host does not have.")
TIME_TOOL = ("/usr/bin/time", "-l")
TICKSTORE_ONLY_MEMORY = (
    "BSD time -l around the system's whole run, inside the measured command: the largest "
    "resident set of compare/run_system.py and every process it waited for, which counts the "
    "clean file-backed pages of every mapped file, and run_system.py's own peak memory "
    "footprint. Each worker's and tool's own peak is in its step.")
# Results files that carry swap per phase. scripts/check_compare.py requires it from schema 2.
SCHEMA = 2
# The comparison protocol of the final review: a document without one is protocol 1, measured
# before it. scripts/check_compare.py holds a protocol 2 document to every rule added with it.
PROTOCOL = 2
TAIL_STATEMENT = (
    "Each latency block's tail figure, tail_us, is the 99th percentile by nearest rank, so it "
    "is a value that really occurred, with n and the rank used beside it. Below 100 runs the "
    "nearest rank is the last, so with 30 warm and 7 cold repetitions the tail figure is the "
    "slowest of 30 and the slowest of 7, never a 1% tail, and each block says so in "
    "tail_is_slowest and tail. p99_us holds the same figure under its old name.")
MACOS_STATEMENT = ("Unpinned. No hardware counters. macOS has no thread pinning, and no "
                   "unprivileged hardware counters. Each system gets the same thread budget.")
LINUX_STATEMENT = (f"Each system ran under taskset -c {LINUX_CORE}, and the run then widened its "
                   "own mask to cpu4 and cpu6. Every thread of every system shares those two "
                   "CPUs, client and server alike. QuestDB's JVM has more threads than CPUs.")
QUESTDB_HEAP = " ".join(heap_flags())
CAPACITY_CHECK = "results/fullday-capacity-12302019.json"
LINUX_HEAP = {"setting": QUESTDB_HEAP, "full_day_check": CAPACITY_CHECK,
              "statement": f"QuestDB ran with {QUESTDB_HEAP}, the setting {CAPACITY_CHECK} records "
                           "as checked and adequate for a full day on Linux."}
MACOS_HEAP = {"setting": QUESTDB_HEAP, "full_day_check": "Linux only",
              "statement": f"QuestDB ran with {QUESTDB_HEAP}, as on Linux, so that the two runs "
                           "match. The full-day capacity check is Linux only."}
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
COLD_INCLUDES = ("Each cold number is the query alone, in a fresh process. The server's start "
                 "and the worker's imports fall before the clock, pyarrow.dataset and "
                 "pyarrow.acero among them, and pages they touched count as resident and as "
                 "device reads. The clock does hold what an engine sets up on its first call in "
                 "a process: for Parquet on macOS, about 1 ms over a second call on warm pages.")
TICKSTORE_QUERIES = ("tick-store's queries go to one tools/tick_query worker per store, started "
                     "before the clock. Each is timed from Python as a round trip that stops on "
                     "the worker's acknowledgement that the answer is in its memory. The worker "
                     "then digests the answer by compare/queries.py's rule, outside the clock, "
                     "as the other systems' digests are made in Python, and returns no rows to "
                     "Python. Its engine time is recorded beside each round trip and is not the "
                     "number compared. The writer and the worker each run on one thread.")
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


def wrapped_command(measure, meter, snapshot, command):
    """The wrapper, the pin and the first sibling reading around a system's command."""
    prefix = [measure] if measure else []
    if meter is not None and measure:
        prefix += ["taskset", "-c", str(LINUX_CORE)]
    if meter is not None:
        prefix += meter.snapshot_command(snapshot)
    return prefix + command


def system_command(system, args, layout, part, variants, guard):
    """run_system.py's command for one system, under the guard on a full-day run."""
    command = [sys.executable, "-I", str(RUN_SYSTEM), "--system", system, "--part", str(part),
               "--slice", args.slice, *layout.as_arguments()]
    if args.day:
        command += ["--day", args.day]
    if args.full_day_smoke:
        command.append("--full-day-smoke")
    for option in ("write_repetitions", "warm_repetitions", "cold_repetitions"):
        value = getattr(args, option)
        if value is not None:
            command += [f"--{option.replace('_', '-')}", str(value)]
    if variants is not None:
        command += ["--variants", ",".join(variants)]
    if guard is None:
        return command
    return [sys.executable, "-I", str(GUARD), "--report", str(guard["report"]),
            "--samples", str(guard["samples"]), "--watch", str(layout.work), "--", *command]


def run_one(system, args, layout, measure, parts, meter, variants=None):
    """Runs a system unless its part is already there. Returns its load lines."""
    part = parts / f"{system}.json"
    lines_path = parts / f"{system}.load"
    sibling_path = parts / f"{system}.sibling"
    time_path = parts / f"{system}.time.json"
    timed = is_tickstore_only(args, run_bench.host_label())
    if part.exists() and lines_path.exists() and (meter is None or sibling_path.exists()) and \
            (not timed or time_path.exists()):
        return json.loads(lines_path.read_text(encoding="utf-8"))
    snapshot = parts / f"{system}.stat"
    guard = None
    if is_full_day(args) and run_bench.host_label() == "Linux":
        guard = {"report": parts / f"{system}.guard.json",
                 "samples": parts / f"{system}.guard.csv"}
    inner = system_command(system, args, layout, part, variants, guard)
    if timed:
        inner = [*TIME_TOOL, *inner]
    command = wrapped_command(measure, meter, snapshot, inner)
    logs = layout.work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    stdout_path = logs / f"{system}.stdout"
    stderr_path = logs / f"{system}.stderr"
    with open(stdout_path, "wb") as stdout, open(stderr_path, "wb") as stderr:
        code = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                              check=False).returncode
    text = stderr_path.read_text(encoding="utf-8", errors="replace")
    if guard is not None and guard["report"].exists():
        report = json.loads(guard["report"].read_text(encoding="utf-8"))
        if report.get("guard_fired"):
            raise run_bench.Failure(1, f"{system}: the guard stopped it: "
                                       f"{report.get('guard_reason')}")
    if code == run_bench.EXIT_RETRY_LATER and measure:
        raise run_bench.Failure(run_bench.EXIT_RETRY_LATER,
                                "retry later: the measurement wrapper did not get the machine")
    if code != 0:
        raise run_bench.Failure(code, f"{system}: exited {code}, see {stderr_path.name}")
    if meter is not None:
        sibling_path.write_text(json.dumps(meter.busy_percent(snapshot)), encoding="utf-8")
    if timed:
        figures = store_results.parse_time(text)
        if figures is None or not figures.get("peak_memory_footprint_bytes"):
            raise run_bench.Failure(1, f"{system}: /usr/bin/time -l printed no peak memory")
        time_path.write_text(json.dumps(figures), encoding="utf-8")
    lines = run_bench.clean_load_lines(text) if measure else []
    lines_path.write_text(json.dumps(lines), encoding="utf-8")
    return lines


def questdb_runtime(systems):
    """What every class read from its running server, which must be one reading."""
    if "questdb" not in systems:
        return "not-run"
    readings = [entry["facts"]["runtime"] for variant in systems["questdb"]["variants"].values()
                for entry in variant["classes"].values()]
    if any(reading != readings[0] for reading in readings):
        raise run_bench.Failure(1, "QuestDB's classes did not run on one runtime")
    return readings[0]


def sibling_block(meter, parts, systems):
    """Each system's run is one case. macOS has no pinned core to have a sibling."""
    if meter is None:
        return "not-available"
    return meter.block({system: json.loads((parts / f"{system}.sibling").read_text(
        encoding="utf-8")) for system in systems})


def is_full_day(args):
    return bool(args.day or args.full_day_smoke)


def is_tickstore_only(args, host):
    """A full day off Linux: tick-store alone. See the module's docstring."""
    return is_full_day(args) and host != "Linux"


def time_memory(figures):
    """A system's peak memory from what /usr/bin/time -l printed around its run."""
    return {"maximum_resident_set_size_bytes": figures["peak_rss_bytes"],
            "peak_memory_footprint_bytes": figures["peak_memory_footprint_bytes"],
            "real_seconds": figures["real_seconds"], "source": TICKSTORE_ONLY_MEMORY}


def peak_memory(report):
    """Each system's peak memory, from its guard report: VmHWM per process name, the largest
    over the processes that share a name, and MemAvailable at its lowest."""
    by_name = {}
    for key, fields in report["processes"].items():
        name = key.split(" ", 1)[1]
        by_name[name] = max(by_name.get(name, 0), fields.get("VmHWM", 0))
    return {"vmhwm_bytes_by_process": dict(sorted(by_name.items())),
            "vmhwm_bytes_largest": max(by_name.values(), default=0),
            "session_rss_bytes_peak": report.get("session_rss_bytes_peak"),
            "mem_available_lowest_bytes": report["mem_available_lowest_bytes"],
            "mem_available_start_bytes": report["mem_available_start_bytes"],
            "swap_in_pages_during_run": report["swap_in_pages_during_run"],
            "swap_out_pages_during_run": report["swap_out_pages_during_run"],
            "swap_in_bytes_per_second_peak": report["swap_in_bytes_per_second_peak"],
            "swap_out_bytes_per_second_peak": report["swap_out_bytes_per_second_peak"],
            "swap_total_bytes": report["swap_total_bytes"],
            "guard_limits": report["limits"],
            "free_disk_lowest_bytes": report["free_disk_lowest_bytes"],
            "guard_fired": report["guard_fired"], "exit": report["exit"],
            "source": "compare/guard.py, sampling every 2 s"}


def build_document(host, args, layout, pin, commit, measure, parts, load_lines, meter=None,
                   selection=None):
    linux = host == "Linux"
    full_day = is_full_day(args)
    alone = is_tickstore_only(args, host)
    systems = {system: json.loads((parts / f"{system}.json").read_text(encoding="utf-8"))
               for system in args.systems}
    truths = [part.pop("truth") for part in systems.values()]
    query_sets = [part.pop("queries") for part in systems.values()]
    day_facts = [part.pop("full_day", None) for part in systems.values()]
    if any(item != truths[0] for item in truths) or any(item != query_sets[0]
                                                        for item in query_sets):
        raise run_bench.Failure(1, "the systems did not run the same queries")
    if any(item != day_facts[0] for item in day_facts):
        raise run_bench.Failure(1, "the systems did not run the same full-day parameters")
    if full_day and linux:
        for system, part in systems.items():
            part["peak_memory"] = peak_memory(json.loads(
                (parts / f"{system}.guard.json").read_text(encoding="utf-8")))
    if alone:
        for system, part in systems.items():
            part["peak_memory"] = time_memory(json.loads(
                (parts / f"{system}.time.json").read_text(encoding="utf-8")))
    runs = [{"load_lines": lines} for lines in load_lines]
    header = {
        "schema": SCHEMA,
        "protocol": PROTOCOL,
        "host": host,
        "measurement": "reported" if measure else "iteration-only",
        "statement": LINUX_STATEMENT if linux else MACOS_STATEMENT,
        "cpu_model": run_bench.cpu_model(host),
        "kernel": f"{platform.system()} {platform.release()}",
        **compiler_facts(layout),
        "commit": commit,
        "core": LINUX_CORE if linux else "not-available",
        "sibling_busy_percent": sibling_block(meter, parts, args.systems),
        "second_thread": "cpu6, used by every system" if linux else "not-available",
        "governor": run_bench.governor_of(host, runs),
        "turbo": run_bench.linux_turbo() if linux else "not-available",
        "pinning": "pinned" if linux else "not-supported",
        "questdb_runtime": questdb_runtime(systems),
        "load_lines": [line for lines in load_lines for line in lines],
    }
    if args.day:
        manifest = read_manifest(layout.export_dir(f"{args.day}-full"))
        input_block = {"day": args.day, "full_day": True, "sha256": pin["sha256"],
                       "messages": pin["message_count"], "bytes": pin["size"],
                       "ratio_baseline": FULL_DAY_BASELINE, "clipping": FULL_DAY_STATEMENT,
                       "ingest_input": FULL_DAY_INPUT_STATEMENT,
                       "export": manifest}
        if alone:
            input_block["systems_run"] = list(args.systems)
    else:
        manifest = read_manifest(layout.export_dir(args.slice))
        input_block = {"slice": args.slice, "sha256": pin["sha256"],
                       "messages": pin["message_count"], "bytes": pin["size"],
                       "ratio_baseline": BASELINE, "clipping": CLIPPING_STATEMENT,
                       "ingest_input": INPUT_STATEMENT, "export": manifest}
    method = {"warm": WARM_METHOD, "cold": COLD_METHOD[host], "cold_includes": COLD_INCLUDES,
              "one_system_at_a_time": True, "tickstore_queries": TICKSTORE_QUERIES,
              "input_cache": "the export's column files are warm before every write",
              "tail": TAIL_STATEMENT,
              "swap": swap.method_block()}
    queries = {"parameters": query_sets[0], "truth": truths[0],
               "truth_method": "numpy over the exported columns, sharing no code with any system"}
    if full_day:
        method.update({"warm": FULL_DAY_WARM, "input_cache": FULL_DAY_INPUT_CACHE,
                       "writes": FULL_DAY_WRITES, "guard": GUARD_STATEMENT,
                       "read_back": "each store read back one time window at a time, each "
                                    "window ordered by seq, into one column directory that "
                                    "tick_roundtrip verify-columns judges against the day"})
        queries["full_day"] = day_facts[0]
    if alone:
        method.update({"warm": TICKSTORE_ONLY_WARM, "input_cache": TICKSTORE_ONLY_INPUT_CACHE,
                       "peak_memory": TICKSTORE_ONLY_MEMORY})
        del method["guard"]
    document = {
        "header": header,
        "label": (TICKSTORE_ONLY_LABEL if alone else FULL_DAY_LABEL) if full_day else LABEL,
    }
    if alone:
        document["scope"] = variant_rule.TICKSTORE_ONLY
    document.update({
        "heap": TICKSTORE_ONLY_HEAP if alone else LINUX_HEAP if linux else MACOS_HEAP,
        "input": input_block,
        "method": method,
        "queries": queries,
    })
    if selection is not None:
        document["variant_selection"] = selection
    document["systems"] = systems
    return document


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


def variant_selection(args, commit, host):
    """The rule's variants for each system, from the slice file of this host and commit."""
    source = json.loads(args.variant_source.read_text(encoding="utf-8"))
    header = source["header"]
    if header["host"] != host or header["commit"] != commit or "slice" not in source["input"]:
        raise run_bench.Failure(run_bench.EXIT_USAGE, "the variant source must be this host's "
                                                      "slice results at this commit")
    if not args.full_day_smoke and header.get("measurement") != "reported":
        raise run_bench.Failure(run_bench.EXIT_USAGE, "the variant source is not reported")
    chosen = variant_rule.select(source)
    missing = [system for system in args.systems if system not in chosen]
    if missing:
        raise run_bench.Failure(run_bench.EXIT_USAGE, f"the variant source lacks {missing}")
    return {"rule": variant_rule.RULE, "source": args.variant_source.name,
            "source_commit": commit, "estimate": "compare/README.md, \"Full-day comparison\"",
            "systems": {system: chosen[system] for system in args.systems}}


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--slice", default=SLICE_NAME)
    parser.add_argument("--day", help="a full-day run on this pinned day: every system on "
                                      "Linux, tick-store alone elsewhere")
    parser.add_argument("--full-day-smoke", action="store_true",
                        help="the full-day path on the slice, iteration only")
    parser.add_argument("--variant-source", type=Path,
                        help="for a full-day run: the slice results of this host and commit")
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
    full_day = is_full_day(args)
    alone = is_tickstore_only(args, host)
    if alone and (args.systems != ["tickstore"] or args.variant_source is not None or
                  args.variants is not None):
        raise run_bench.Failure(run_bench.EXIT_USAGE, "a full day off Linux is tick-store alone, "
                                                      "every variant: --systems tickstore, and "
                                                      "no --variant-source or --variants")
    if args.full_day_smoke and measure:
        raise run_bench.Failure(run_bench.EXIT_USAGE, "the smoke run is iteration only")
    if full_day and not alone and args.variant_source is None and args.variants is None:
        raise run_bench.Failure(run_bench.EXIT_USAGE, "a full-day run needs --variant-source")
    if args.day:
        if args.day not in pins["days"]:
            raise run_bench.Failure(run_bench.EXIT_USAGE, f"no pin for day {args.day}")
        day = pins["days"][args.day]
        pin = {"sha256": day["decompressed_sha256"], "size": day["decompressed_size"],
               "message_count": day["message_count"]}
    else:
        pin = pins["slices"].get(args.slice)
        if pin is None:
            raise run_bench.Failure(run_bench.EXIT_USAGE, f"no pin for slice {args.slice}")
    require_clean_tree(measure)
    commit = run_bench.current_commit()
    layout = Layout.from_arguments(args)
    tag = commit[:12] + (f"-{args.day}-full" if args.day else "-smoke" if full_day else "")
    parts = layout.work / "parts" / tag
    parts.mkdir(parents=True, exist_ok=True)
    meter = run_bench.SiblingMeter() if host == "Linux" else None
    selection = None
    chosen = {}
    if alone:
        selection = variant_rule.tickstore_only()
        chosen = {system: entry["selected"] for system, entry in selection["systems"].items()}
    elif full_day and args.variant_source is not None:
        selection = variant_selection(args, commit, host)
        chosen = {system: entry["selected"] for system, entry in selection["systems"].items()}
    elif args.variants is not None:
        chosen = {system: args.variants.split(",") for system in args.systems}
    load_lines = [run_one(system, args, layout, measure, parts, meter, chosen.get(system))
                  for system in args.systems]
    document = build_document(host, args, layout, pin, commit, measure, parts, load_lines, meter,
                              selection)
    directory = args.results_dir if measure else args.iteration_dir
    suffix = TICKSTORE_ONLY_SUFFIX if alone else FULL_DAY_SUFFIX
    name = f"{args.day}-{suffix}" if args.day else (
        f"{args.slice}-{'tickstore-' if alone else ''}smoke" if full_day else args.slice)
    output = directory / f"{host}-{name}-{commit[:12]}.json"
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
