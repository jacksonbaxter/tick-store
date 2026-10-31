#!/usr/bin/env python3
"""Validate results files. Exits 1 and names every problem if any file has one.

    python3 -I scripts/check_results.py results/bench/*.json

A results file is a claim that a number was measured under stated conditions. This
checks that the file can back the claim: a header comes first and is complete, the
host cannot be mistaken, a measurement lock really was held, the input was verified,
no latency sits below what the timer can resolve, and on Linux the core's SMT sibling
was measured and stayed under the limit. It also checks that the file is safe to
publish, with no path, username or hostname anywhere in it.

Standard library only. tests/scripts/test_check_results.py shows each rule failing on
an injected fault.
"""

import argparse
import getpass
import json
import re
import socket
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"

SCHEMA = 1
EXIT_OK = 0
EXIT_INVALID = 1
EXIT_USAGE = 2

REPORTED = "reported"
ITERATION_ONLY = "iteration-only"
UNAVAILABLE = "unavailable"
NOT_AVAILABLE = "not-available"

HEADER_FIELDS = (
    "schema",
    "host",
    "measurement",
    "statement",
    "cpu_model",
    "kernel",
    "compiler",
    "compile_flags",
    "build_type",
    "sanitizers",
    "isa",
    "commit",
    "core",
    "sibling",
    "sibling_busy_percent",
    "second_thread",
    "governor",
    "frequency",
    "turbo",
    "pinning",
    "counters",
    "timer",
    "load_lines",
    "peak_rss_bytes",
)
TIMER_FIELDS = ("source", "calibrated_against", "ticks_per_second", "resolution_ns",
                "read_cost_ns", "min_publishable_ns")
COUNTER_FIELDS = ("cycles", "instructions", "branch_misses", "cache_misses")

# Each host's timer and architecture, which a mixed file cannot satisfy.
HOSTS = {
    "macOS": {"timer": "mach_absolute_time", "isa": "arm64", "kernel": "Darwin "},
    "Linux": {"timer": "rdtscp+lfence", "isa": "x86-64", "kernel": "Linux "},
}
LINUX_CORE = 4
# Our choice, not a measured limit: a sibling busy for more than this share of a case is no
# longer idle as a result states. Nothing keeps it idle, so a reported result has to show it.
SIBLING_BUSY_LIMIT_PERCENT = 5.0

LOAD_LINE = re.compile(
    r"^measure\.sh: (?P<kind>start|end) load1=(?P<load>[0-9.]+)(?: exit=(?P<exit>\d+))? "
    r"at=\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z"
    r"(?: cpus=(?P<cpus>\S+) governor=(?P<governor>\S+) freq_khz=(?P<freq>\S+)"
    r" perf_event_paranoid=\S+)?$"
)

# Anywhere in any string: paths, addresses and name forms that identify a machine.
LEAK_PATTERNS = (
    # Anywhere at all, because a flag can join a path with no separator: -I/home/x.
    ("a home-style path", re.compile(r"/Users/|/home/|/root/|~/")),
    ("an absolute path", re.compile(r"(?:^|[\s=:,\"'(])/[\w.+-]+/[\w.+-]")),
    ("a Windows path", re.compile(r"\b[A-Za-z]:\\")),
    ("an include flag", re.compile(r"(?:^|\s)-(?:I|isystem|L|isysroot|-sysroot)")),
    ("an IP address", re.compile(r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b")),
    ("a hostname field", re.compile(r"\bhost(?:name)?[=:]\s*\S", re.IGNORECASE)),
    ("a network name", re.compile(r"\b[\w-]+\.(?:local|lan|home|ts\.net)\b", re.IGNORECASE)),
)
LEAK_KEYS = {"host_name", "hostname", "username", "user", "executable", "cwd", "path"}


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def walk(value, where="$"):
    """Every (location, key-or-None, string-or-None) in a JSON value."""
    if isinstance(value, dict):
        for key, item in value.items():
            yield where, key, None
            yield from walk(item, f"{where}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from walk(item, f"{where}[{index}]")
    elif isinstance(value, str):
        yield where, None, value


def local_names():
    """This machine's own host and user names, which no results file may contain."""
    names = []
    try:
        names.append(socket.gethostname().split(".")[0])
    except OSError:
        pass
    try:
        names.append(getpass.getuser())
    except (OSError, KeyError, ImportError):
        pass
    return [name for name in names if len(name) >= 3]


def check_leaks(document):
    problems = []
    names = [re.compile(rf"\b{re.escape(name)}\b") for name in local_names()]
    for where, key, text in walk(document):
        if key is not None:
            if key.lower() in LEAK_KEYS:
                problems.append(f"{where}: field '{key}' identifies a machine")
            continue
        for label, pattern in LEAK_PATTERNS:
            if pattern.search(text):
                problems.append(f"{where}: {label} in '{text}'")
        if any(name.search(text) for name in names):
            problems.append(f"{where}: this machine's name in the text")
    return problems


def check_header(header, top_keys):
    problems = []
    if not top_keys or top_keys[0] != "header":
        problems.append("the file does not start with its header")
    for field in HEADER_FIELDS:
        if field not in header:
            problems.append(f"header: missing field '{field}'")
    timer = header.get("timer")
    if isinstance(timer, dict):
        for field in TIMER_FIELDS:
            if field not in timer:
                problems.append(f"header.timer: missing field '{field}'")
    elif "timer" in header:
        problems.append("header.timer: not an object")
    if header.get("schema", SCHEMA) != SCHEMA:
        problems.append(f"header.schema: expected {SCHEMA}")
    return problems


def check_build(header):
    problems = []
    if "build_type" in header and header["build_type"] != "Release":
        problems.append(f"header.build_type: {header['build_type']} is not Release")
    if "sanitizers" in header and header["sanitizers"] != "none":
        problems.append(f"header.sanitizers: {header['sanitizers']} would distort any number")
    if "compile_flags" in header and "-O3" not in str(header["compile_flags"]).split():
        problems.append("header.compile_flags: no -O3")
    if "commit" in header and not re.fullmatch(r"[0-9a-f]{40}", str(header["commit"])):
        problems.append("header.commit: not a full commit hash")
    return problems


def check_load_lines(header, case_count):
    """A measurement lock must have been held around every case."""
    if header.get("measurement") == ITERATION_ONLY:
        return ["header.measurement: iteration-only, so this is not a reported result"]
    if header.get("measurement", REPORTED) != REPORTED:
        return [f"header.measurement: unknown value {header['measurement']}"]
    lines = header.get("load_lines")
    if "load_lines" in header and not lines:
        return ["header.load_lines: no load lines, so this is iteration-only, "
                "not a reported result"]
    if not isinstance(lines, list):
        return []
    problems = []
    parsed = []
    for line in lines:
        match = LOAD_LINE.match(str(line))
        if match is None:
            problems.append(f"header.load_lines: not a measure.sh load line: '{line}'")
        else:
            parsed.append(match)
    if problems:
        return problems
    kinds = [match["kind"] for match in parsed]
    if kinds != ["start", "end"] * case_count:
        problems.append(f"header.load_lines: expected a start and an end for each of "
                        f"{case_count} cases, got {kinds}")
    for match in parsed:
        if match["kind"] == "end" and match["exit"] != "0":
            problems.append(f"header.load_lines: a run ended with exit {match['exit']}")
    return problems


def check_host(document, path):
    """The host label decides everything else, so nothing may disagree with it."""
    header = document["header"]
    host = header.get("host")
    if host not in HOSTS:
        return [f"header.host: '{host}' is not one of {sorted(HOSTS)}"]
    facts = HOSTS[host]
    problems = []
    if not path.name.startswith(f"{host}-"):
        problems.append(f"mixed hosts: file name {path.name} does not match header.host {host}")
    if not str(header.get("kernel", facts["kernel"])).startswith(facts["kernel"]):
        problems.append(f"mixed hosts: header.kernel is not from {host}")
    if not str(header.get("isa", facts["isa"])).startswith(facts["isa"]):
        problems.append(f"mixed hosts: header.isa is not from {host}")
    timer = header.get("timer")
    if isinstance(timer, dict) and timer.get("source", facts["timer"]) != facts["timer"]:
        problems.append(f"mixed hosts: header.timer.source is not from {host}")
    for index, result in enumerate(document.get("results", [])):
        if result.get("host") != host:
            problems.append(f"mixed hosts: results[{index}].host is "
                            f"{result.get('host')}, and the header says {host}")
        result_timer = result.get("timer", {})
        if result_timer.get("source", facts["timer"]) != facts["timer"]:
            problems.append(f"mixed hosts: results[{index}].timer.source is not from {host}")
    for line in header.get("load_lines") or []:
        match = LOAD_LINE.match(str(line))
        if host == "macOS" and match and match["cpus"]:
            problems.append("mixed hosts: a Linux load line in a macOS file")
    return problems


def check_macos(document):
    header = document["header"]
    problems = []
    statement = str(header.get("statement", ""))
    if "Unpinned" not in statement or "No hardware counters" not in statement:
        problems.append("header.statement: macOS results must state plainly that they are "
                        "unpinned and have no hardware counters")
    if "pinning" in header and header["pinning"] != "not-supported":
        problems.append(f"header.pinning: macOS cannot be {header['pinning']}")
    if "core" in header and header["core"] != NOT_AVAILABLE:
        problems.append(f"header.core: macOS claims core {header['core']}")
    if "counters" in header and header["counters"] != UNAVAILABLE:
        problems.append(f"header.counters: macOS claims {header['counters']}")
    if header.get("sibling_busy_percent", NOT_AVAILABLE) != NOT_AVAILABLE:
        problems.append("header.sibling_busy_percent: macOS has no pinned core to measure")
    for index, result in enumerate(document.get("results", [])):
        for field, value in result.get("counters", {}).items():
            if isinstance(value, dict) and is_number(value.get("value")):
                problems.append(f"results[{index}].counters.{field}: macOS claims a "
                                "hardware counter")
    for where, _, text in walk(document):
        if text and "proc_pid_rusage" in text:
            problems.append(f"{where}: proc_pid_rusage is not used in place of counters")
    return problems


def check_sibling(header, cases):
    """The SMT sibling, measured across every case and under the limit in each."""
    if "sibling_busy_percent" not in header:
        return []  # Named among the missing header fields.
    block = header["sibling_busy_percent"]
    if not isinstance(block, dict) or not isinstance(block.get("cases"), dict):
        return ["header.sibling_busy_percent: Linux must measure it, case by case"]
    problems = []
    sibling = block.get("sibling")
    if block.get("core") != LINUX_CORE:
        problems.append(f"header.sibling_busy_percent: core {block.get('core')} is not the "
                        f"pinned core {LINUX_CORE}")
    if not is_number(sibling) or sibling == LINUX_CORE:
        problems.append(f"header.sibling_busy_percent: {sibling} is not a sibling of the core")
    if not block.get("method") or not block.get("topology"):
        problems.append("header.sibling_busy_percent: no method, or no topology file")
    measured = block["cases"]
    for case in cases:
        if case not in measured:
            problems.append(f"header.sibling_busy_percent: no reading for {case}")
    for case, value in measured.items():
        if not is_number(value) or not 0 <= value <= 100:
            problems.append(f"header.sibling_busy_percent: {case} reads {value!r}, which is not "
                            "a percentage")
        elif value > SIBLING_BUSY_LIMIT_PERCENT:
            problems.append(f"header.sibling_busy_percent: {case} reads {value}%, over the "
                            f"{SIBLING_BUSY_LIMIT_PERCENT}% limit")
    return problems


def check_linux(header, cases):
    problems = check_sibling(header, cases)
    if "core" in header and header["core"] != LINUX_CORE:
        problems.append(f"header.core: expected {LINUX_CORE}, got {header['core']}")
    if "pinning" in header and header["pinning"] != "pinned":
        problems.append(f"header.pinning: Linux results must be pinned, got {header['pinning']}")
    if "sibling" in header and "not enforced" not in str(header["sibling"]):
        problems.append("header.sibling: the idle sibling must be recorded as stated, "
                        "not claimed")
    for field in ("governor", "turbo"):
        if field in header and header[field] in (None, "", UNAVAILABLE, NOT_AVAILABLE):
            problems.append(f"header.{field}: Linux must record it")
    if "turbo" in header and header["turbo"] not in ("enabled", "disabled", None, "",
                                                     UNAVAILABLE, NOT_AVAILABLE):
        problems.append(f"header.turbo: unknown value {header['turbo']}")
    if "frequency" in header:
        frequency = header["frequency"]
        snapshots = frequency.get("snapshot_khz") if isinstance(frequency, dict) else None
        if not snapshots or not all(is_number(value) for value in snapshots):
            problems.append("header.frequency: Linux must record observed frequency")
    return problems


def check_timer(where, timer):
    problems = []
    if timer.get("calibrated_against") != "CLOCK_MONOTONIC_RAW":
        problems.append(f"{where}: not calibrated against CLOCK_MONOTONIC_RAW")
    for field in ("ticks_per_second", "resolution_ns", "read_cost_ns", "min_publishable_ns"):
        value = timer.get(field)
        if not is_number(value) or value <= 0:
            problems.append(f"{where}.{field}: needs a measured positive value")
    return problems


def check_latency(where, latency, timer):
    """Every published latency must clear what the timer can resolve."""
    problems = []
    for field in ("unit", "batch_messages", "timer_resolution_ns", "percentiles"):
        if field not in latency:
            problems.append(f"{where}: '{field}' must sit next to the percentiles")
    if not is_number(latency.get("batch_messages")) or latency.get("batch_messages", 0) < 1:
        problems.append(f"{where}.batch_messages: needs a batch size")
    floor = max((value for value in (timer.get("min_publishable_ns"),
                                     timer.get("resolution_ns"),
                                     latency.get("timer_resolution_ns")) if is_number(value)),
                default=None)
    if floor is None:
        return problems + [f"{where}: no timer floor to judge it by"]
    values = [(f"{where}.min_ns", latency.get("min_ns")), (f"{where}.max_ns", latency.get("max_ns"))]
    for point in latency.get("percentiles", []):
        values.append((f"{where} p{point.get('percentile')}", point.get("value_ns")))
    for label, value in values:
        if not is_number(value):
            problems.append(f"{label}: not a number")
        elif value < floor:
            problems.append(f"{label}: {value} ns is below the {floor} ns the timer can resolve")
    return problems


def check_counters(where, counters):
    """B4: each counter has a value, or says it is unavailable. It is never just absent."""
    problems = []
    for field in COUNTER_FIELDS:
        value = counters.get(field)
        if not isinstance(value, dict):
            problems.append(f"{where}.{field}: missing, and not marked unavailable")
        elif is_number(value.get("value")):
            continue
        elif value.get("status") != UNAVAILABLE or not value.get("reason"):
            problems.append(f"{where}.{field}: neither a value nor marked unavailable "
                            "with a reason")
    return problems


def check_results_rows(document, pins):
    problems = []
    results = document.get("results")
    if not isinstance(results, list) or not results:
        return ["results: no results"]
    input_block = document.get("input", {})
    slices = pins.get("slices", {})
    pin = slices.get(input_block.get("slice"))
    if pin is None:
        problems.append(f"input.slice: '{input_block.get('slice')}' has no pin")
    for index, result in enumerate(results):
        where = f"results[{index}]"
        timer = result.get("timer")
        if not isinstance(timer, dict):
            problems.append(f"{where}.timer: missing")
            timer = {}
        problems += check_timer(f"{where}.timer", timer)
        latency = result.get("latency")
        if not isinstance(latency, dict):
            problems.append(f"{where}.latency: missing")
        else:
            problems += check_latency(f"{where}.latency", latency, timer)
        counters = result.get("counters")
        if not isinstance(counters, dict):
            problems.append(f"{where}.counters: missing")
        else:
            problems += check_counters(f"{where}.counters", counters)
        if "ratio" not in result:
            problems.append(f"{where}.ratio: missing")
        warmup = result.get("warmup", {})
        if not is_number(warmup.get("passes_per_repetition")) or warmup["passes_per_repetition"] < 1:
            problems.append(f"{where}.warmup: warm-up must be explicit and recorded")
        verification = result.get("verification", {})
        if pin is not None:
            for field in ("input_sha256", "reencoded_sha256"):
                if verification.get(field) != pin["sha256"]:
                    problems.append(f"{where}.verification.{field}: not the slice's pin")
            if verification.get("messages") != pin["message_count"]:
                problems.append(f"{where}.verification.messages: not the slice's count")
        throughput = result.get("throughput", {})
        if not is_number(throughput.get("messages_per_second", {}).get("median")):
            problems.append(f"{where}.throughput: no median")
        if not is_number(throughput.get("messages_per_second", {}).get("cv")):
            problems.append(f"{where}.throughput: no variability across repetitions")
    return problems


def check_document(document, top_keys, path, pins):
    header = document.get("header")
    if not isinstance(header, dict):
        return ["the file does not start with its header"]
    results = document.get("results")
    case_count = len(results) if isinstance(results, list) else 0
    problems = check_header(header, top_keys)
    problems += check_load_lines(header, case_count)
    problems += check_build(header)
    problems += check_host(document, path)
    if header.get("host") == "macOS":
        problems += check_macos(document)
    elif header.get("host") == "Linux":
        problems += check_linux(header, [result.get("case") for result in results]
                                if isinstance(results, list) else [])
    if isinstance(header.get("timer"), dict):
        problems += check_timer("header.timer", header["timer"])
    problems += check_results_rows(document, pins)
    problems += check_leaks(document)
    return problems


def check_file(path, pins):
    try:
        top_keys = []

        def remember_order(pairs):
            top_keys[:] = [key for key, _ in pairs]
            return dict(pairs)

        document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=remember_order)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"cannot read it: {error}"]
    if not isinstance(document, dict):
        return ["the file is not a JSON object"]
    return check_document(document, top_keys, path, pins)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate benchmark results files.")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--pins", type=Path, default=DEFAULT_PINS,
                        help="the data pins to compare against")
    args = parser.parse_args(argv)
    try:
        pins = json.loads(args.pins.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"check_results: cannot read the pins: {error}", file=sys.stderr)
        return EXIT_USAGE

    failed = False
    for path in args.files:
        problems = check_file(path, pins)
        if problems:
            failed = True
            for problem in problems:
                print(f"{path.name}: {problem}", file=sys.stderr)
        else:
            print(f"{path.name}: ok")
    return EXIT_INVALID if failed else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
