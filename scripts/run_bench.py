#!/usr/bin/env python3
"""Run a benchmark binary and write its results file.

    TICK_STORE_MEASURE=/path/to/measure.sh \\
        python3 -I scripts/run_bench.py build/bench/bench/itch_codec_bench

A binary says what it reads with --input-kind: "slice", a pinned file of real messages,
whose cases are messages per second; "seeded", input it makes from a fixed seed per case,
as simd_kernel_bench does, whose cases are values per second; or "column", columns it
builds from the pinned slice, as column_decode_bench does, whose cases are values per
second of real data.

Each case in the binary runs as its own process directly under the measurement wrapper,
because the wrapper signals only its direct child, and so that `perf stat` around that
process counts one workload. The wrapper is never named here: it comes from
TICK_STORE_MEASURE. Without that variable there is no lock, so the file is marked
iteration-only, goes under build/, and check_results.py will refuse it.

On Linux the case runs as

    <wrapper> taskset -c 4 sh -c <copy /proc/stat, then exec> \\
        perf stat -x, -e cycles:u,instructions:u,branch-misses:u ...

with events held off by perf's control FIFOs until the binary opens them around the
timed passes. The copy of /proc/stat is taken once the wrapper has the lock, and read
against /proc/stat when it exits, to record how busy the core's SMT sibling was: nothing
keeps it idle. stdout and stderr go to files on the host, so nothing depends on a
terminal or an SSH connection staying up. The results file appears only when the whole
run has finished and check_results.py has passed it, so a file that exists is complete.

Exit codes: 0 done, 75 the wrapper says retry later (nothing was measured), 2 usage or
setup, anything else the code the wrapper or benchmark exited with.
"""

import argparse
import json
import os
import platform
import re
import resource
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CHECK_RESULTS = REPO_ROOT / "scripts" / "check_results.py"
DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"
DEFAULT_RESULTS_DIR = REPO_ROOT / "results" / "bench"
DEFAULT_ITERATION_DIR = REPO_ROOT / "build" / "bench-iteration"
DEFAULT_WORK_DIR = REPO_ROOT / "build" / "bench-raw"
DEFAULT_SLICE = "12302019-open"

MEASURE_ENV = "TICK_STORE_MEASURE"
EXIT_RETRY_LATER = 75
EXIT_USAGE = 2

LINUX_CORE = 4
LINUX_SECOND_THREAD = "cpu6 reserved, unused: every case is single-threaded"
PERF_EVENTS = {
    "cycles:u": "cycles",
    "instructions:u": "instructions",
    "branch-misses:u": "branch_misses",
}
COUNTER_UNITS = {
    "cycles": "core cycles, user mode",
    "instructions": "instructions, user mode",
    "branch_misses": "mispredicted branches, user mode",
}
COUNTER_SCOPE = "timed throughput passes only, gated by perf's control FIFOs"
# docs/TASKS.md: confirm what cache-misses counts on Zen 1 before quoting it.
CACHE_MISSES_UNAVAILABLE = {
    "status": "unavailable",
    "reason": "not collected until what the event counts on Zen 1 is confirmed",
}
MACOS_STATEMENT = ("Unpinned. No hardware counters. macOS has no thread pinning, and no "
                   "unprivileged hardware counters.")
LINUX_STATEMENT = (f"Pinned with taskset -c {LINUX_CORE} and verified by sched_getcpu. Counters "
                   "come from perf stat user-mode events. perf cycles and timer ticks are "
                   "different units.")

CPU_SYSFS = Path("/sys/devices/system/cpu")
PROC_STAT = Path("/proc/stat")
SIBLING_FILES = ("thread_siblings_list", "core_cpus_list")
# user nice system idle iowait irq softirq steal. Guest time is already inside user and nice.
JIFFY_FIELDS = 8
IDLE_FIELDS = (3, 4)
# The copy runs in the command's place and execs it, so the wrapper's child is the same process.
SNAPSHOT_SCRIPT = 'cat "$0" > "$1" && shift && exec "$@"'
SIBLING_METHOD = ("Busy jiffies over total jiffies from the kernel's proc stat file, read once "
                  "the measurement lock is held and again when the wrapper exits. Busy is "
                  "everything but idle and iowait, and a wait for the lock is not in the window.")

LOAD_LINE = re.compile(r"^measure\.sh: (?:start|end) load1=")
HOST_TOKEN = re.compile(r" host=\S+")
GOVERNOR = re.compile(r" governor=(\S+)")
FREQUENCY = re.compile(r" freq_khz=\S*?cpu\d+:(\d+)")


class Failure(Exception):
    """A setup or result problem. The run produced no results file."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def host_label():
    system = platform.system()
    if system == "Darwin":
        return "macOS"
    if system == "Linux":
        return "Linux"
    raise Failure(EXIT_USAGE, f"no host label for {system}")


def run_text(command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout.strip()


def current_commit():
    """HEAD of this tree. An exported tree has no .git and carries a COMMIT file."""
    exported = REPO_ROOT / "COMMIT"
    if exported.exists():
        return exported.read_text(encoding="utf-8").strip()
    return run_text(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"])


def cpu_model(host):
    if host == "macOS":
        return run_text(["sysctl", "-n", "machdep.cpu.brand_string"])
    for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip()
    raise Failure(EXIT_USAGE, "/proc/cpuinfo has no model name")


def linux_turbo(sysfs=CPU_SYSFS):
    """enabled or disabled, from cpufreq's boost switch, or unavailable with the reason. A
    virtual machine, such as a hosted CI runner, has no switch, and check_results.py refuses
    a Linux file without a measured state: only the pinned host publishes Linux results."""
    for path in (sysfs / f"cpu{LINUX_CORE}" / "cpufreq" / "boost", sysfs / "cpufreq" / "boost"):
        try:
            state = path.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        return {"1": "enabled", "0": "disabled"}.get(
            state, f"unavailable: cpufreq boost reads {state!r}, which is neither 0 nor 1")
    return "unavailable: no cpufreq boost switch, as on a virtual machine"


def parse_cpu_list(text):
    """A kernel CPU list, such as 4-5 or 4,5."""
    cpus = []
    for part in text.strip().split(","):
        low, _, high = part.partition("-")
        cpus += range(int(low), int(high or low) + 1)
    return cpus


def linux_sibling(core, sysfs):
    """The core's one other hardware thread, and the topology file that named it."""
    for name in SIBLING_FILES:
        try:
            text = (sysfs / f"cpu{core}" / "topology" / name).read_text(encoding="ascii")
        except OSError:
            continue
        if not text.strip():
            continue
        others = [cpu for cpu in parse_cpu_list(text) if cpu != core]
        if len(others) != 1:
            raise Failure(EXIT_USAGE, f"cpu{core} has {len(others)} siblings in {name}, not 1")
        return others[0], name
    raise Failure(EXIT_USAGE, f"no topology file names cpu{core}'s sibling")


def cpu_jiffies(stat_text, cpu):
    """(busy, total) jiffies of one CPU, from the text of /proc/stat."""
    for line in stat_text.splitlines():
        fields = line.split()
        if fields and fields[0] == f"cpu{cpu}":
            values = [int(value) for value in fields[1:JIFFY_FIELDS + 1]]
            idle = sum(values[index] for index in IDLE_FIELDS)
            return sum(values) - idle, sum(values)
    raise Failure(1, f"/proc/stat has no cpu{cpu}")


class SiblingMeter:
    """How busy the pinned core's SMT sibling was across a case. Nothing keeps it idle. `core`
    is the pinned core, LINUX_CORE unless a run pins a second CPU and meters its sibling too."""

    def __init__(self, sysfs=CPU_SYSFS, stat=PROC_STAT, core=LINUX_CORE):
        self.core = core
        self.sibling, self.topology = linux_sibling(core, sysfs)
        self.stat = stat

    def snapshot_command(self, snapshot):
        """A prefix for a command, so that the first reading falls inside the lock."""
        return ["sh", "-c", SNAPSHOT_SCRIPT, str(self.stat), str(snapshot)]

    def busy_percent(self, snapshot):
        """Since the snapshot, up to now."""
        try:
            first = snapshot.read_text(encoding="ascii")
        except OSError as error:
            raise Failure(1, f"no reading of cpu{self.sibling} before the case: {error}") from None
        busy_before, total_before = cpu_jiffies(first, self.sibling)
        busy, total = cpu_jiffies(self.stat.read_text(encoding="ascii"), self.sibling)
        if total <= total_before:
            raise Failure(1, f"no jiffies passed on cpu{self.sibling} across the case")
        return 100.0 * (busy - busy_before) / (total - total_before)

    def statement(self):
        return f"cpu{self.sibling} idle as stated, not enforced: see sibling_busy_percent"

    def block(self, cases):
        return {"core": self.core, "sibling": self.sibling, "topology": self.topology,
                "method": SIBLING_METHOD, "cases": cases}


def clean_load_lines(stderr_text):
    """measure.sh's load lines, without the hostname it prints in them."""
    return [HOST_TOKEN.sub("", line) for line in stderr_text.splitlines()
            if LOAD_LINE.match(line)]


def peak_rss_bytes():
    """The largest process run so far. ru_maxrss is bytes on macOS and KiB on Linux."""
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return peak if platform.system() == "Darwin" else peak * 1024


def parse_perf(path):
    """Counters from `perf stat -x, -o`, keyed by our field names."""
    counters = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split(",")
        if line.startswith("#") or len(fields) < 5 or fields[2] not in PERF_EVENTS:
            continue
        name = PERF_EVENTS[fields[2]]
        if fields[0].isdigit():
            counters[name] = {"value": int(fields[0]), "unit": COUNTER_UNITS[name],
                              "coverage_percent": float(fields[4] or 0.0)}
        else:
            counters[name] = {"status": "unavailable", "reason": f"perf said {fields[0]}"}
    for name in PERF_EVENTS.values():
        if name not in counters:
            counters[name] = {"status": "unavailable", "reason": "perf did not report it"}
    return counters


def macos_counters():
    reason = "macOS has no unprivileged hardware counters"
    fields = ("cycles", "instructions", "branch_misses", "cache_misses")
    return {field: {"status": "unavailable", "reason": reason} for field in fields}


# What a case counts, by the binary's input kind.
ITEMS = {"slice": "messages", "seeded": "values", "column": "values"}
# The kinds that read the pinned slice, and so get its path and pin.
READS_SLICE = ("slice", "column")


def input_kind_of(binary):
    """What the binary reads, by its own answer to --input-kind."""
    kind = run_text([str(binary), "--input-kind"])
    if kind not in ITEMS:
        raise Failure(EXIT_USAGE, f"{binary.name} says its input is {kind!r}, not one of "
                                  f"{sorted(ITEMS)}")
    return kind


def throughput_of(google_benchmark, case, items="messages"):
    """Median, mean and spread across repetitions, from Google Benchmark's own aggregates."""
    aggregates = {}
    repetitions = 0
    for row in google_benchmark.get("benchmarks", []):
        if not row.get("name", "").startswith(f"{case}/throughput"):
            continue
        if row.get("run_type") == "aggregate":
            aggregates[row["aggregate_name"]] = row
        else:
            repetitions += 1
    missing = {"median", "mean", "stddev", "cv"} - aggregates.keys()
    if missing or repetitions == 0:
        raise Failure(1, f"{case}: Google Benchmark gave no aggregates for {sorted(missing)}")
    if google_benchmark["context"].get("library_build_type") != "release":
        raise Failure(1, "Google Benchmark itself was not built for release")
    time_unit = aggregates["median"].get("time_unit")
    if time_unit != "ms":
        raise Failure(1, f"{case}: expected milliseconds, got {time_unit}")
    return {
        "repetitions": repetitions,
        "pass_ms_median": aggregates["median"]["real_time"],
        f"{items}_per_second": {
            "median": aggregates["median"]["items_per_second"],
            "mean": aggregates["mean"]["items_per_second"],
            "stddev": aggregates["stddev"]["items_per_second"],
            "cv": aggregates["cv"]["items_per_second"],
        },
        "bytes_per_second_median": aggregates["median"]["bytes_per_second"],
    }


def counters_of(host, case_files, report, items="messages"):
    if host == "macOS":
        return macos_counters()
    counters = parse_perf(case_files["perf"])
    if counters["cycles"].get("value") == 0:
        raise Failure(1, "perf counted nothing, so the gate never opened")
    gated = report["gated"][items if items == "messages" else "items"]
    per_item = {}
    for name in PERF_EVENTS.values():
        if "value" in counters[name]:
            per_item[name] = counters[name]["value"] / gated
    counters["cache_misses"] = dict(CACHE_MISSES_UNAVAILABLE)
    counters["scope"] = COUNTER_SCOPE
    counters[f"gated_{items}"] = gated
    counters["per_message" if items == "messages" else "per_value"] = per_item
    return counters


def ratio_of(case, report, kind="slice"):
    if kind == "column":
        verification = report["verification"]
        values = verification["values"]
        stored = verification["encoded_bytes"]
        return {"value": 8 * values / stored, "source_bytes": 8 * values,
                "output_bytes": stored, "bits_per_value": 8 * stored / values,
                "note": "the column's values as 8-byte words over its encoded bytes"}
    if kind == "seeded":
        return {"status": "not-applicable", "reason": "a decode kernel, so nothing is stored"}
    if case.endswith("_encode"):
        bytes_out = report["verification"]["bytes"]
        return {"value": 1.0, "source_bytes": bytes_out, "output_bytes": bytes_out,
                "note": "the wire format re-encoded, with no compression yet"}
    return {"status": "not-applicable", "reason": "decode only, so nothing is stored"}


def wrapped_command(host, measure, case_files, binary_args, meter=None):
    command = []
    if measure:
        command.append(measure)
    if host == "Linux":
        command += ["taskset", "-c", str(LINUX_CORE),
                    *meter.snapshot_command(case_files["stat"]), "perf", "stat", "-x,",
                    "-e", ",".join(PERF_EVENTS), "--delay=-1",
                    f"--control=fifo:{case_files['control']},{case_files['ack']}",
                    "-o", str(case_files["perf"]), "--"]
    return command + binary_args


def run_case(host, measure, binary, case, slice_path, pin, work_dir, meter=None, kind="slice"):
    # A case name may hold slashes, as pfor_unpack/w=13/neon does.
    stem = case.replace("/", "_")
    files = {name: work_dir / f"{stem}.{name}" for name in
             ("stdout", "stderr", "report", "gbench", "perf", "control", "ack", "stat")}
    for path in files.values():
        path.unlink(missing_ok=True)

    arguments = [str(binary)]
    if kind in READS_SLICE:
        arguments += [f"--slice={slice_path}", f"--expect-sha256={pin['sha256']}",
                      f"--expect-messages={pin['message_count']}"]
    arguments += [f"--case={case}", f"--report={files['report']}",
                  f"--benchmark_out={files['gbench']}", "--benchmark_out_format=json",
                  "--benchmark_color=false"]
    if host == "Linux":
        os.mkfifo(files["control"], 0o600)
        os.mkfifo(files["ack"], 0o600)
        arguments += [f"--core={LINUX_CORE}", f"--perf-control={files['control']}",
                      f"--perf-ack={files['ack']}"]

    command = wrapped_command(host, measure, files, arguments, meter)
    with open(files["stdout"], "wb") as stdout, open(files["stderr"], "wb") as stderr:
        code = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stdout,
                              stderr=stderr, check=False).returncode
    stderr_text = files["stderr"].read_text(encoding="utf-8", errors="replace")
    if code == EXIT_RETRY_LATER and measure:
        raise Failure(EXIT_RETRY_LATER,
                      "retry later: the measurement wrapper did not get the machine "
                      f"({len(stderr_text.splitlines())} lines in {files['stderr']})")
    if code != 0:
        raise Failure(code, f"{case}: exited {code}, see {files['stderr']}")

    sibling_busy = meter.busy_percent(files["stat"]) if meter else None
    report = json.loads(files["report"].read_text(encoding="utf-8"))
    google_benchmark = json.loads(files["gbench"].read_text(encoding="utf-8"))
    items = ITEMS[kind]
    return {
        "case": case,
        "kind": kind,
        "sibling_busy_percent": sibling_busy,
        "report": report,
        "throughput": throughput_of(google_benchmark, case, items),
        "counters": counters_of(host, files, report, items),
        "load_lines": clean_load_lines(stderr_text) if measure else [],
    }


def frequency_block(host, runs):
    if host == "macOS":
        return "not-available"
    block = {"snapshot_khz": [], "effective_mhz": {},
             "note": ("snapshot_khz is scaling_cur_freq at each load line. effective_mhz is "
                      "user-mode cycles over the wall time of the timed passes.")}
    for run in runs:
        for line in run["load_lines"]:
            match = FREQUENCY.search(line)
            if match:
                block["snapshot_khz"].append(int(match[1]))
        cycles = run["counters"].get("cycles", {}).get("value")
        wall_ns = run["report"]["gated"]["wall_ns"]
        if cycles and wall_ns:
            block["effective_mhz"][run["case"]] = cycles / wall_ns * 1000.0
    return block


def governor_of(host, runs):
    if host == "macOS":
        return "not-available"
    seen = set()
    for run in runs:
        for line in run["load_lines"]:
            match = GOVERNOR.search(line)
            if match:
                seen.add(match[1])
    # measure.sh sets the governor only while it holds the lock, so a file read from
    # sysfs before or after the run would give the wrong answer.
    return ",".join(sorted(seen)) or "unavailable"


def result_row(host, run):
    report = run["report"]
    return {
        "case": run["case"],
        "host": host,
        "isa": report["build"]["isa"],
        "timer": report["timer"],
        "timer_end_ticks_per_second": report["timer_end"]["ticks_per_second"],
        "warmup": report["warmup"],
        "verification": report["verification"],
        "throughput": run["throughput"],
        "counters": run["counters"],
        "ratio": ratio_of(run["case"], report, run.get("kind", "slice")),
        "latency": report["latency"],
    }


def build_document(host, benchmark, slice_name, pin, commit, measure, runs, meter=None,
                   sysfs=CPU_SYSFS):
    first = runs[0]["report"]
    build = first["build"]
    linux = host == "Linux"
    header = {
        "schema": 1,
        "host": host,
        "measurement": "reported" if measure else "iteration-only",
        "statement": LINUX_STATEMENT if linux else MACOS_STATEMENT,
        "cpu_model": cpu_model(host),
        "kernel": f"{platform.system()} {platform.release()}",
        "compiler": build["compiler"],
        "compile_flags": build["compile_flags"],
        "build_type": build["build_type"],
        "sanitizers": build["sanitizers"],
        # Each row carries the kernel set it ran. A file of several sets lists them all.
        "isa": "; ".join(dict.fromkeys(run["report"]["build"]["isa"] for run in runs)),
        "commit": commit,
        # How the build established its commit, git or an export's COMMIT file, since a tree
        # not compared with git is not known to be clean (review RA-5).
        "commit_source": build["commit_source"],
        "core": LINUX_CORE if linux else "not-available",
        "sibling": meter.statement() if linux else "not-available",
        "sibling_busy_percent": (meter.block({run["case"]: run["sibling_busy_percent"]
                                              for run in runs}) if linux else "not-available"),
        "second_thread": LINUX_SECOND_THREAD if linux else "not-available",
        "governor": governor_of(host, runs),
        "frequency": frequency_block(host, runs),
        "turbo": linux_turbo(sysfs) if linux else "not-available",
        "pinning": first["pinning"]["status"],
        "counters": "perf stat, :u events" if linux else "unavailable",
        "timer": first["timer"],
        "load_lines": [line for run in runs for line in run["load_lines"]],
        "peak_rss_bytes": peak_rss_bytes(),
    }
    kind = runs[0].get("kind", "slice")
    if kind == "seeded":
        input_block = {
            "kind": "seeded",
            "items": "values",
            "seed": "each case makes its own input from a fixed seed, in its verification block",
        }
    elif kind == "column":
        input_block = {
            "kind": "column",
            "items": "values",
            "slice": slice_name,
            "sha256": pin["sha256"],
            "messages": pin["message_count"],
            "bytes": pin["size"],
            "seed": "the input is a pinned file, and its columns are built from it whole",
        }
    else:
        input_block = {
            "kind": "slice",
            "slice": slice_name,
            "sha256": pin["sha256"],
            "messages": pin["message_count"],
            "bytes": pin["size"],
            "seed": "the input is a pinned file. Only batch boundaries are seeded, per case",
        }
    return {
        "header": header,
        "benchmark": benchmark,
        "input": input_block,
        "results": [result_row(host, run) for run in runs],
    }


def check_builds_match(runs, commit, measure):
    """A reported number has to come from the commit it cites."""
    if not measure:
        return
    for run in runs:
        build = run["report"]["build"]
        if build["commit"] != commit:
            raise Failure(EXIT_USAGE, f"the binary was built at {build['commit']}, and this "
                                      f"tree is at {commit}: rebuild it")
        if build["tree_dirty"]:
            raise Failure(EXIT_USAGE, "the binary was built from a dirty tree: commit and "
                                      "rebuild it")


def publish(document, output, pins_path, measure):
    """Writes the file whole, and only after the validator has passed it."""
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_name(output.name + ".pending")
    pending.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    if measure:
        checked = subprocess.run(
            [sys.executable, "-I", str(CHECK_RESULTS), "--pins", str(pins_path), str(pending)],
            check=False, capture_output=True, text=True)
        if checked.returncode != 0:
            print(checked.stderr, end="", file=sys.stderr)
            pending.unlink()
            raise Failure(1, "the results failed validation, so no file was written")
    os.replace(pending, output)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description="Run a benchmark and write its results file.")
    parser.add_argument("binary", type=Path)
    parser.add_argument("--slice", default=DEFAULT_SLICE, help="key under slices in the pins")
    parser.add_argument("--slice-path", type=Path, help="where the slice is, if not in data/")
    parser.add_argument("--pins", type=Path, default=DEFAULT_PINS)
    parser.add_argument("--results-dir", type=Path, default=DEFAULT_RESULTS_DIR)
    parser.add_argument("--iteration-dir", type=Path, default=DEFAULT_ITERATION_DIR)
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK_DIR,
                        help="where raw outputs go, under the build tree")
    parser.add_argument("--sysfs-cpu", type=Path, default=CPU_SYSFS,
                        help="where the kernel's CPU topology and boost switch are")
    parser.add_argument("--proc-stat", type=Path, default=PROC_STAT,
                        help="where the kernel's CPU times are")
    return parser.parse_args(argv)


def run(args):
    measure = os.environ.get(MEASURE_ENV)
    host = host_label()
    binary = args.binary.resolve()
    kind = input_kind_of(binary)
    pins = json.loads(args.pins.read_text(encoding="utf-8"))
    pin = pins["slices"].get(args.slice)
    if pin is None and kind in READS_SLICE:
        raise Failure(EXIT_USAGE, f"no pin for slice {args.slice}")
    slice_path = (args.slice_path or REPO_ROOT / pin["path"]) if kind in READS_SLICE else None
    benchmark = binary.name.removesuffix("_bench")
    commit = current_commit()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    meter = SiblingMeter(args.sysfs_cpu, args.proc_stat) if host == "Linux" else None

    cases = run_text([str(binary), "--list-cases"]).split()
    runs = [run_case(host, measure, binary, case, slice_path, pin, args.work_dir, meter, kind)
            for case in cases]
    check_builds_match(runs, commit, measure)

    document = build_document(host, benchmark, args.slice, pin, commit, measure, runs, meter,
                              args.sysfs_cpu)
    directory = args.results_dir if measure else args.iteration_dir
    output = directory / f"{host}-{benchmark}-{commit[:12]}.json"
    publish(document, output, args.pins, measure)
    if not measure:
        print(f"{MEASURE_ENV} is not set, so this is iteration-only: {output}")
    else:
        print(output)


def main(argv=None):
    try:
        run(parse_arguments(argv))
    except Failure as failure:
        print(f"run_bench: {failure}", file=sys.stderr)
        return failure.code
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, KeyError) as error:
        print(f"run_bench: {error}", file=sys.stderr)
        return EXIT_USAGE
    return 0


if __name__ == "__main__":
    sys.exit(main())
