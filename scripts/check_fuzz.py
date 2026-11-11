#!/usr/bin/env python3
"""Validate the G-3 fuzzing results file. Exits 1 and names every problem if it has one.

    python3 -I scripts/check_fuzz.py results/fuzz-g3-<commit>.json

G-3 in BENCHMARKS.md: every libFuzzer target runs clean for 30 minutes under heavy.sh. A file
that claims it has to hold that run for every target fuzz/CMakeLists.txt defines, on macOS, and
on Linux for every target whose decode reaches the AVX2 kernels or that verifies CRC32C, which
runs the SSE4.2 kernel there, since the Mac can run neither.
This refuses a file that leaves a target out, a clean run shorter than 1,800 s, a run whose own
final lines disagree with its numbers, a run looser than the targets' memory caps, and summary
counts that are not the runs'. A run that was not clean may stop early, but must say what it
found; the README's Targets block then reads it as missed, never as met.

It also refuses anything not safe to publish, with check_results.py's rules.

Standard library only. tests/scripts/test_check_fuzz.py shows each rule failing on an injected
fault.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_results  # noqa: E402

DEFAULT_CMAKE = REPO_ROOT / "fuzz" / "CMakeLists.txt"

SCHEMA = "tick-store-fuzz-1"
EXIT_OK = 0
EXIT_INVALID = 1
EXIT_USAGE = 2

# G-3's 30 minutes. The file states it too, and may not state less.
REQUIRED_SECONDS = 1800
# The caps fuzz/README.md gives every target, as scripts/check.sh runs them.
REQUIRED_FLAGS = ("-rss_limit_mb=2048", "-malloc_limit_mb=256", "-timeout=10",
                  "-print_final_stats=1")
RSS_LIMIT_MB = 2048

# The targets whose decode path dispatches to the SIMD kernels (codec/simd.hpp), and so on
# x86-64 to the AVX2 set: PFOR and Stream VByte call active_kernels() directly; dictionary and
# run-length nest PFOR blocks; the ITCH store's and the query's column chunks are those codecs.
# Gorilla, the container, the column directory, the stub store and the ITCH parsers use none.
# A new target that reaches the kernels must be added here, with its reason.
AVX2_PATH = ("fuzz_dictionary", "fuzz_itch_query", "fuzz_itch_store", "fuzz_pfor",
             "fuzz_run_length", "fuzz_stream_vbyte")
# The targets that verify CRC32C on their input through store::crc32c(), which on x86-64 runs
# the SSE4.2 kernel (src/store/crc32c_sse42.cpp) and on the Mac the ARMv8 one: the container
# parser, and the ITCH store and the query, whose files are containers. Replaying the seeds
# with -print_coverage=1 on Linux lists crc32c_sse42 as covered for these three, and not for
# the column directory or the stub store, which carry no CRC; nor do the codecs or the ITCH
# parsers call it. A new target that verifies a CRC must be added here.
CRC32C_PATH = ("fuzz_container", "fuzz_itch_query", "fuzz_itch_store")
# What Linux must run: the union of the two.
LINUX_REQUIRED = tuple(sorted(set(AVX2_PATH) | set(CRC32C_PATH)))

HOSTS = {"macOS": {"kernel": "Darwin ", "levels": ("neon",)},
         "Linux": {"kernel": "Linux ", "levels": ("avx2", "scalar")}}
# The level each host's required runs take: the Mac's own SIMD set, and AVX2 on Linux.
REQUIRED_LEVEL = {"macOS": "neon", "Linux": "avx2"}

HEADER_FIELDS = ("schema", "gate", "commit", "statement", "required_seconds", "targets_file",
                 "targets", "avx2_path_targets", "crc32c_path_targets", "hosts")
HOST_FIELDS = ("cpu_model", "kernel", "compiler", "compile_flags", "sanitizer_options",
               "concurrency", "peak_memory")
RUN_FIELDS = ("target", "host", "commit", "kernel_level", "seconds", "wall_seconds", "runs",
              "exec_per_second", "coverage", "corpus", "peak_rss_mb", "clean", "exit_code",
              "findings", "final_status_line", "done_line", "final_stats", "command")
FINDING_KINDS = ("crash", "timeout", "oom", "leak", "other")
SUMMARY_FIELDS = ("targets_required", "targets_clean", "shortest_seconds")

FUZZER = re.compile(r"^\s*tick_store_add_fuzzer\(\s*(\w+)\s*\)", re.MULTILINE)
DONE = re.compile(r"^Done (\d+) runs in (\d+) second\(s\)$")
STATUS = re.compile(r"^#(\d+)\s+DONE\s+cov: (\d+) ft: (\d+) corp: (\d+)/\S+ .*rss: \d+Mb$")


def is_number(value):
    return check_results.is_number(value)


def targets_in(cmake):
    """Every target fuzz/CMakeLists.txt adds, sorted."""
    return sorted(FUZZER.findall(cmake.read_text(encoding="utf-8")))


def check_header(header, top_keys, path, targets):
    problems = []
    if not top_keys or top_keys[0] != "header":
        problems.append("the file does not start with its header")
    for field in HEADER_FIELDS:
        if field not in header:
            problems.append(f"header: missing field '{field}'")
    if header.get("schema") != SCHEMA:
        problems.append(f"header.schema: expected {SCHEMA}")
    if header.get("gate") != "G-3":
        problems.append("header.gate: this file judges G-3")
    commit = str(header.get("commit", ""))
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        problems.append("header.commit: not a full commit hash")
    elif path.name != f"fuzz-g3-{commit[:12]}.json":
        problems.append(f"file name {path.name} is not fuzz-g3-{commit[:12]}.json")
    if header.get("required_seconds") != REQUIRED_SECONDS:
        problems.append(f"header.required_seconds: G-3 is {REQUIRED_SECONDS} s, not "
                        f"{header.get('required_seconds')}")
    if header.get("targets") != targets:
        problems.append(f"header.targets: {header.get('targets')} is not the list "
                        f"fuzz/CMakeLists.txt defines, {targets}")
    if header.get("avx2_path_targets") != list(AVX2_PATH):
        problems.append(f"header.avx2_path_targets: {header.get('avx2_path_targets')} is not "
                        f"{list(AVX2_PATH)}")
    if header.get("crc32c_path_targets") != list(CRC32C_PATH):
        problems.append(f"header.crc32c_path_targets: {header.get('crc32c_path_targets')} is "
                        f"not {list(CRC32C_PATH)}")
    for name in AVX2_PATH:
        if name not in targets:
            problems.append(f"{name} reaches the AVX2 kernels and fuzz/CMakeLists.txt has no "
                            "such target")
    for name in CRC32C_PATH:
        if name not in targets:
            problems.append(f"{name} verifies CRC32C and fuzz/CMakeLists.txt has no such target")
    hosts = header.get("hosts")
    if not isinstance(hosts, dict) or sorted(hosts) != sorted(HOSTS):
        problems.append(f"header.hosts: needs exactly {sorted(HOSTS)}")
        return problems
    for host, facts in hosts.items():
        if not isinstance(facts, dict):
            problems.append(f"header.hosts.{host}: not an object")
            continue
        for field in HOST_FIELDS:
            if not facts.get(field):
                problems.append(f"header.hosts.{host}: missing field '{field}'")
        if not str(facts.get("kernel", "")).startswith(HOSTS[host]["kernel"]):
            problems.append(f"header.hosts.{host}.kernel: not from {host}")
    return problems


def check_command(where, command):
    problems = []
    words = str(command).split()
    for flag in REQUIRED_FLAGS:
        if flag not in words:
            problems.append(f"{where}.command: no {flag}, so it is not the targets' run")
    budget = [word for word in words if word.startswith("-max_total_time=")]
    if len(budget) != 1 or not budget[0].split("=", 1)[1].isdigit() or \
            int(budget[0].split("=", 1)[1]) < REQUIRED_SECONDS:
        problems.append(f"{where}.command: -max_total_time is not at least {REQUIRED_SECONDS}")
    return problems


def check_clean(where, run):
    """A clean run's numbers must be the ones its own final lines print."""
    problems = []
    if run["exit_code"] != 0:
        problems.append(f"{where}: clean, but the fuzzer exited {run['exit_code']}")
    if run["findings"]:
        problems.append(f"{where}: clean, with findings")
    for field in ("seconds", "wall_seconds"):
        if not is_number(run[field]) or run[field] < REQUIRED_SECONDS:
            problems.append(f"{where}.{field}: {run[field]} s is under the {REQUIRED_SECONDS} s "
                            "G-3 asks for")
    if not is_number(run["runs"]) or run["runs"] < 1:
        problems.append(f"{where}.runs: a clean run executed inputs")
    done = DONE.match(str(run["done_line"]))
    if done is None:
        problems.append(f"{where}.done_line: not libFuzzer's 'Done N runs in S second(s)'")
    elif (int(done[1]), int(done[2])) != (run["runs"], run["seconds"]):
        problems.append(f"{where}.done_line: says {done[1]} runs in {done[2]} s, the run "
                        f"{run['runs']} in {run['seconds']} s")
    status = STATUS.match(str(run["final_status_line"]))
    coverage, corpus = run["coverage"], run["corpus"]
    if status is None:
        problems.append(f"{where}.final_status_line: not libFuzzer's final DONE line")
    elif not isinstance(coverage, dict) or not isinstance(corpus, dict) or (
            int(status[1]), int(status[2]), int(status[3]), int(status[4])) != (
                run["runs"], coverage.get("cov"), coverage.get("ft"), corpus.get("units")):
        problems.append(f"{where}.final_status_line: its runs, coverage or corpus are not the "
                        "run's")
    stats = run["final_stats"]
    if not isinstance(stats, dict) or stats.get("number_of_executed_units") != run["runs"] or \
            stats.get("peak_rss_mb") != run["peak_rss_mb"] or \
            stats.get("average_exec_per_sec") != run["exec_per_second"]:
        problems.append(f"{where}.final_stats: not the run's runs, exec/s and peak RSS")
    if not is_number(run["peak_rss_mb"]) or run["peak_rss_mb"] > RSS_LIMIT_MB:
        problems.append(f"{where}.peak_rss_mb: over the {RSS_LIMIT_MB} MB cap")
    return problems


def check_runs(document, header):
    problems = []
    runs = document.get("runs")
    if not isinstance(runs, list) or not runs:
        return ["runs: no runs"], []
    seen = set()
    good = []
    for index, run in enumerate(runs):
        where = f"runs[{index}]"
        if not isinstance(run, dict):
            problems.append(f"{where}: not an object")
            continue
        missing = [field for field in RUN_FIELDS if field not in run]
        if missing:
            problems.append(f"{where}: missing {', '.join(missing)}")
            continue
        host = run["host"]
        if host not in HOSTS:
            problems.append(f"{where}.host: '{host}' is not one of {sorted(HOSTS)}")
            continue
        if run["target"] not in header.get("targets", []):
            problems.append(f"{where}.target: {run['target']} is not a target of "
                            "fuzz/CMakeLists.txt")
        if run["commit"] != header.get("commit"):
            problems.append(f"{where}.commit: not the header's commit")
        if run["kernel_level"] not in HOSTS[host]["levels"]:
            problems.append(f"{where}.kernel_level: {run['kernel_level']} on {host}")
        key = (run["target"], host, run["kernel_level"])
        if key in seen:
            problems.append(f"{where}: a second run of {run['target']} on {host} at "
                            f"{run['kernel_level']}")
        seen.add(key)
        problems += check_command(where, run["command"])
        if run["clean"] is True:
            problems += check_clean(where, run)
        elif run["clean"] is False:
            findings = run["findings"]
            if not isinstance(findings, list) or not findings or not all(
                    isinstance(item, dict) and item.get("kind") in FINDING_KINDS
                    and item.get("detail") for item in findings):
                problems.append(f"{where}.findings: a run that was not clean names each finding, "
                                f"its kind one of {FINDING_KINDS} and its detail")
        else:
            problems.append(f"{where}.clean: true or false")
        good.append(run)
    return problems, good


def required_of(header):
    return {"macOS": list(header.get("targets", [])), "Linux": list(LINUX_REQUIRED)}


def summary_of(header, runs):
    """Per host: the targets it must run, how many ran clean for the full time at the
    required level, and the shortest of those required runs."""
    summary = {}
    for host, required in required_of(header).items():
        mine = {run["target"]: run for run in runs
                if run["host"] == host and run["kernel_level"] == REQUIRED_LEVEL[host]}
        present = [mine[name] for name in required if name in mine]
        clean = [run for run in present
                 if run["clean"] is True and is_number(run["seconds"])
                 and run["seconds"] >= REQUIRED_SECONDS]
        seconds = [run["seconds"] for run in present if is_number(run["seconds"])]
        summary[host] = {"targets_required": len(required), "targets_clean": len(clean),
                         "shortest_seconds": min(seconds) if seconds else None}
    return summary


def check_coverage_of_targets(header, runs):
    """Every required target has its run on each host: a missing one is refused, not missed."""
    problems = []
    for host, required in required_of(header).items():
        have = {run["target"] for run in runs
                if run["host"] == host and run["kernel_level"] == REQUIRED_LEVEL[host]}
        for name in required:
            if name not in have:
                problems.append(f"no run of {name} on {host} at {REQUIRED_LEVEL[host]}: G-3 "
                                "needs every target")
    return problems


def check_document(document, top_keys, path, targets):
    header = document.get("header")
    if not isinstance(header, dict):
        return ["the file does not start with its header"]
    problems = check_header(header, top_keys, path, targets)
    run_problems, runs = check_runs(document, header)
    problems += run_problems
    problems += check_coverage_of_targets(header, runs)
    if not run_problems:
        if document.get("summary") != summary_of(header, runs):
            problems.append(f"summary: {document.get('summary')} is not what the runs give, "
                            f"{summary_of(header, runs)}")
    problems += check_results.check_leaks(document)
    return problems


def check_file(path, targets):
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
    return check_document(document, top_keys, path, targets)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate the G-3 fuzzing results file.")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--cmake", type=Path, default=DEFAULT_CMAKE,
                        help="the fuzz/CMakeLists.txt whose targets the file must cover")
    args = parser.parse_args(argv)
    try:
        targets = targets_in(args.cmake)
    except OSError as error:
        print(f"check_fuzz: cannot read {args.cmake.name}: {error}", file=sys.stderr)
        return EXIT_USAGE
    if not targets:
        print(f"check_fuzz: {args.cmake.name} defines no fuzz targets", file=sys.stderr)
        return EXIT_USAGE
    failed = False
    for path in args.files:
        problems = check_file(path, targets)
        if problems:
            failed = True
            for problem in problems:
                print(f"{path.name}: {problem}", file=sys.stderr)
        else:
            print(f"{path.name}: ok")
    return EXIT_INVALID if failed else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
