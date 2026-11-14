#!/usr/bin/env python3
"""Validate the G-3 fuzzing results file. Exits 1 and names every problem if it has one.

    python3 -I scripts/check_fuzz.py results/fuzz-g3-<commit>.json

G-3 in BENCHMARKS.md: every libFuzzer target runs clean for 30 minutes under heavy.sh. A file
that claims it has to hold that run for every target fuzz/CMakeLists.txt defines, on macOS, and
on Linux for every target whose decode reaches the AVX2 kernels or that verifies CRC32C, which
runs the SSE4.2 kernel there, since the Mac can run neither.
This refuses a file that leaves a target out, a clean run shorter than 1,800 s, a run whose own
final lines disagree with its numbers, a run looser than the targets' memory caps, and summary
counts that are not the runs'. A run is scripts/fuzz_g3.py's three parts: the replay of every
saved corpus input and seed, which must have run them all clean; their merge, in one attempt,
since a second means an input crashed it; and the timed run on the merged set, at least
1,800 s by its own clock, split where its first new input ran. A run that was not clean may stop early, but must say what it
found; RESULTS.md's Targets block then reads it as missed, never as met. One stopped by its replay
or merge carries that part's command, as the launcher writes it, held to the caps but to no time
budget, since the timed part never ran.

It also refuses anything not safe to publish, with check_results.py's rules.

A file holds for the code it was made at and no later: it is refused once anything a fuzz binary
is built from or launched with, TIED_PATHS, differs from the file's commit, committed or not.
That needs the commit in git's history. Where there is none, an exported tree or a clone too
shallow to hold it, the file is not judged: it exits EXIT_UNJUDGED and says why, which
render_results.py passes on and scripts/check.sh reports as a skip, a failure under --no-skips.
One file made before this rule is let through it until the freeze replaces it: PRE_TIE_FILE,
known by the SHA-256 of its bytes under PRE_TIE_PINS in scripts/data_pins.json, never by
anything the file says of itself. No other file may be pinned there. It is also the one file
that may keep PRE_TIE_SCHEMA, from before the launcher, whose runs record no replay or merge.

Standard library only. tests/scripts/test_check_fuzz.py shows each rule failing on an injected
fault.
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_results  # noqa: E402

DEFAULT_CMAKE = REPO_ROOT / "fuzz" / "CMakeLists.txt"
DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"

SCHEMA = "tick-store-fuzz-2"
# The pinned file's, from before scripts/fuzz_g3.py: each run replayed its folders inside its
# timed part, and records nothing of it.
PRE_TIE_SCHEMA = "tick-store-fuzz-1"
EXIT_OK = 0
EXIT_INVALID = 1
EXIT_USAGE = 2
# The file is valid, but git cannot say whether the fuzzed code changed after its commit.
EXIT_UNJUDGED = 3

# What a fuzz binary is built from or launched with: the sources and headers, the targets, their
# oracles and seed generator, the CMake files and presets, fuzz/launch.txt's flags and folders,
# and the G-3 launcher. fuzz/'s prose is not. A file is refused once any of these differs from
# its commit. scripts/check.sh is not here: its fuzz steps read fuzz/launch.txt.
TIED_PATHS = ("src", "include", "fuzz", "cmake", "CMakeLists.txt", "CMakePresets.json",
              "scripts/fuzz_g3.py")
NOT_TIED = (":(exclude)fuzz/*.md",)
# G-3's file from ea1dde4, made before the tie, and the key of the pins file that holds its
# SHA-256. docs/TASKS.md removes both when the freeze's file is committed.
PRE_TIE_FILE = "fuzz-g3-ea1dde4c02e4.json"
PRE_TIE_PINS = "fuzz_g3_file_before_the_tie"

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
REPLAY_FIELDS = ("command", "inputs", "runs", "seconds", "exit_code", "clean", "findings",
                 "done_line")
MERGE_FIELDS = ("command", "inputs_kept", "attempts", "seconds", "exit_code", "clean",
                "findings")
TIMED_FIELDS = ("seconds_before_first_new_input", "seconds_after_first_new_input")
# How far the launcher's clock and the meta file's whole-second epochs may differ.
CLOCK_SLACK_SECONDS = 2
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
    if header.get("schema") not in (SCHEMA, PRE_TIE_SCHEMA):
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


def check_caps(where, command):
    words = str(command).split()
    return [f"{where}.command: no {flag}, so it is not the targets' run"
            for flag in REQUIRED_FLAGS if flag not in words]


def check_command(where, command):
    problems = check_caps(where, command)
    words = str(command).split()
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


def max_len_of(command):
    found = [word for word in str(command).split() if word.startswith("-max_len=")]
    return found[0] if len(found) == 1 else None


def check_part_command(where, command, flag, timed_command):
    problems = []
    words = str(command).split()
    if flag not in words:
        problems.append(f"{where}.command: no {flag}")
    for required in REQUIRED_FLAGS:
        if required not in words:
            problems.append(f"{where}.command: no {required}, so it is not the targets' run")
    if any(word.startswith("-max_total_time=") for word in words):
        problems.append(f"{where}.command: a time budget, so it may not run every input")
    if max_len_of(command) is None or max_len_of(command) != max_len_of(timed_command):
        problems.append(f"{where}.command: its -max_len is not the timed run's")
    return problems


def check_phases(where, run):
    """A clean run's replay ran every input clean, its merge took one attempt, and its timed
    part, before and after its first new input, is the run's own time and at least G-3's."""
    problems = []
    replay, merge, timed = run.get("replay"), run.get("merge"), run.get("timed")
    for name, part, fields in (("replay", replay, REPLAY_FIELDS), ("merge", merge, MERGE_FIELDS),
                               ("timed", timed, TIMED_FIELDS)):
        if not isinstance(part, dict) or any(field not in part for field in fields):
            problems.append(f"{where}.{name}: missing, or without {', '.join(fields)}")
    if problems:
        return problems
    for name, part, flag in (("replay", replay, "-runs=0"), ("merge", merge, "-merge=1")):
        if part["exit_code"] != 0 or part["clean"] is not True or part["findings"]:
            problems.append(f"{where}.{name}: not clean, so the timed run cannot be")
        if not is_number(part["seconds"]) or part["seconds"] < 0:
            problems.append(f"{where}.{name}.seconds: not a time")
        problems += check_part_command(f"{where}.{name}", part["command"], flag, run["command"])
    done = DONE.match(str(replay["done_line"]))
    if not isinstance(replay["inputs"], int) or replay["inputs"] < 1 or done is None or \
            int(done[1]) != replay["runs"] or replay["runs"] < replay["inputs"]:
        problems.append(f"{where}.replay: its Done line does not run each of its "
                        f"{replay['inputs']} inputs")
    if merge["attempts"] != 1:
        problems.append(f"{where}.merge.attempts: {merge['attempts']}; libFuzzer starts its "
                        "merge again only past an input that crashed it")
    if not isinstance(merge["inputs_kept"], int) or merge["inputs_kept"] < 1 or \
            isinstance(replay["inputs"], int) and merge["inputs_kept"] > replay["inputs"]:
        problems.append(f"{where}.merge.inputs_kept: {merge['inputs_kept']} is not a set of the "
                        "replayed inputs")
    before, after = timed["seconds_before_first_new_input"], timed["seconds_after_first_new_input"]
    if not is_number(before) or not is_number(after) or before < 0 or after < 0:
        problems.append(f"{where}.timed: not two times")
        return problems
    if before + after < REQUIRED_SECONDS:
        problems.append(f"{where}.timed: {before} s before its first new input and {after} s "
                        f"after, {before + after:.1f} s, under the {REQUIRED_SECONDS} s G-3 asks "
                        "for")
    if is_number(run["wall_seconds"]) and \
            abs(before + after - run["wall_seconds"]) > CLOCK_SLACK_SECONDS:
        problems.append(f"{where}.timed: {before + after:.1f} s is not the run's "
                        f"{run['wall_seconds']} s")
    return problems


def failed_part(run):
    """The replay or merge that stopped a run that was not clean, if the run carries that part's
    command as its own: the launcher writes it so and never starts the timed part. That command
    has no time budget, so the run is held to the caps alone, and read as missed (review RG-1)."""
    if run.get("clean") is not False or "timed" in run:
        return None
    words = str(run.get("command")).split()
    for name in ("replay", "merge"):
        part = run.get(name)
        if not isinstance(part, dict) or part.get("clean") is True:
            continue
        part_words = str(part.get("command")).split()
        if part_words and words[-len(part_words):] == part_words:
            return name
    return None


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
        if failed_part(run) is None:
            problems += check_command(where, run["command"])
        else:
            problems += check_caps(where, run["command"])
        if run["clean"] is True:
            problems += check_clean(where, run)
            if header.get("schema") == SCHEMA:
                problems += check_phases(where, run)
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


def git(repo, *arguments):
    return subprocess.run(["git", "-C", str(repo), *arguments], capture_output=True, text=True,
                          check=False)


def history_problem(repo, commit):
    """Why git here cannot compare the tree with the commit, or None if it can."""
    top = git(repo, "rev-parse", "--show-toplevel")
    if top.returncode != 0 or Path(top.stdout.strip()).resolve() != Path(repo).resolve():
        return (f"{Path(repo).name} is not a git checkout, as an exported tree is not, so there "
                "is no history to compare with")
    if git(repo, "cat-file", "-e", f"{commit}^{{commit}}").returncode != 0:
        return (f"{commit[:12]} is not in this checkout's history, as in a shallow clone; fetch "
                "it whole")
    return None


def tied_changes(repo, commit):
    """Every tied file that differs from the commit in the tree, committed or not, or new."""
    changed = git(repo, "diff", "--name-only", commit, "--", *TIED_PATHS, *NOT_TIED)
    new = git(repo, "ls-files", "--others", "--exclude-standard", "--", *TIED_PATHS, *NOT_TIED)
    for done in (changed, new):
        if done.returncode != 0:
            raise OSError(f"git: {done.stderr.strip()}")
    return sorted(set(changed.stdout.split()) | set(new.stdout.split()))


def pinned_before_the_tie(path, pins):
    """(problems, exempt): whether this is the one file made before the tie, by its bytes."""
    pinned = pins.get(PRE_TIE_PINS) or {}
    problems = [f"{PRE_TIE_PINS} in the pins file: {name} is pinned, and only {PRE_TIE_FILE} "
                "may be" for name in sorted(pinned) if name != PRE_TIE_FILE]
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    exempt = path.name == PRE_TIE_FILE and pinned.get(PRE_TIE_FILE) == digest
    return problems, exempt


def check_tie(path, commit, repo, pins):
    """(problems, unjudged, note) for a file whose content is valid."""
    problems, exempt = pinned_before_the_tie(path, pins)
    schema = json.loads(path.read_text(encoding="utf-8"))["header"]["schema"]
    if schema == PRE_TIE_SCHEMA and not exempt:
        problems.append(f"header.schema: {PRE_TIE_SCHEMA} records no replay or merge, and only "
                        f"{PRE_TIE_FILE}, by its pin, may keep it; a file made now is {SCHEMA}")
    reason = history_problem(repo, commit)
    if exempt:
        counted = ("" if reason else
                   f"; {len(tied_changes(repo, commit))} tied files changed since {commit[:12]}")
        return problems, None, f"made before the tie and pinned, so not tied{counted}"
    if reason:
        return problems, (f"could not judge whether the fuzzed code changed after "
                          f"{commit[:12]}: {reason}"), None
    changed = tied_changes(repo, commit)
    if changed:
        shown = ", ".join(changed[:8]) + (f" and {len(changed) - 8} more" if len(changed) > 8
                                          else "")
        problems.append(f"the fuzzed code changed after {commit[:12]}, the file's commit: "
                        f"{shown}. Run G-3 again at this code")
    return problems, None, f"tied: nothing fuzzed changed since {commit[:12]}"


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate the G-3 fuzzing results file.")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--cmake", type=Path, default=DEFAULT_CMAKE,
                        help="the fuzz/CMakeLists.txt whose targets the file must cover")
    parser.add_argument("--repo", type=Path, default=REPO_ROOT,
                        help="the checkout whose code the file must still be the code of")
    parser.add_argument("--pins", type=Path, default=DEFAULT_PINS)
    args = parser.parse_args(argv)
    try:
        targets = targets_in(args.cmake)
        pins = json.loads(args.pins.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"check_fuzz: cannot read {args.cmake.name} or {args.pins.name}: {error}",
              file=sys.stderr)
        return EXIT_USAGE
    if not targets:
        print(f"check_fuzz: {args.cmake.name} defines no fuzz targets", file=sys.stderr)
        return EXIT_USAGE
    failed = unjudged = False
    for path in args.files:
        problems = check_file(path, targets)
        reason = note = None
        if not problems:
            commit = json.loads(path.read_text(encoding="utf-8"))["header"]["commit"]
            try:
                problems, reason, note = check_tie(path, commit, args.repo, pins)
            except OSError as error:
                problems = [f"cannot compare with {commit[:12]}: {error}"]
        if problems:
            failed = True
            for problem in problems:
                print(f"{path.name}: {problem}", file=sys.stderr)
        elif reason:
            unjudged = True
            print(f"{path.name}: {reason}", file=sys.stderr)
        else:
            print(f"{path.name}: ok, {note}")
    if failed:
        return EXIT_INVALID
    return EXIT_UNJUDGED if unjudged else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
