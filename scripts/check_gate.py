#!/usr/bin/env python3
"""Validate the results file of G-3's bound-seed gate, and the disturbance entries placed in it.

    python3 -I scripts/check_gate.py [--root DIR] [results/fuzz-gate-<commit>.json ...]

With no file named, every results/fuzz-gate-*.json under the root. scripts/gate_results.py
writes the file from the runs' raw records; scripts/render_results.py runs this check before it
renders the gate's block in fuzz/README.md, so a page cannot show a file this refuses. The check
is here, not in scripts/check_fuzz.py, because check_ties.py ties the fuzz file to its
validators' code: a change there would untie the fuzz file's runs from the tree.

A file is refused if:
- its shape is not the assembler's: its schema, its header's fields, each run's fields, or a
  name that is not fuzz-gate-<the first 12 of its commit>.json;
- a run's field is not what the assembler writes there (RUN_FORMS, LINUX_FORMS): a count that
  is not a whole number of at least 0 or 1, a time an execution or a load that is not a finite
  number of at least 0, a start or end that is not a real UTC time to the second, or an end
  before its start, a busy share outside 0 to 100 or not to one decimal, or a target that is
  not its seed's own in fuzz/bound_seeds.txt;
- it holds a machine's path, user or host name (check_results.check_leaks);
- its bar is not half of the tree's fuzz/launch.txt -timeout, in ms an execution, or its
  header's words are not the assembler's: its check, its statement for its runs a seed, its
  account of its assembly with the days of its runs, its bar's text, the hosts' facts, and
  its list, fuzz/bound_seeds.txt, by its SHA-256 as the tree holds it;
- a seed of the tree's fuzz/bound_seeds.txt has no runs on a host, or a host ran a seed the list
  does not hold;
- it gives other than the gate's runs a seed, REQUIRED_RUNS, or a seed's runs on a host are
  not numbered 1 to its runs a seed;
- a run's time an execution is not its libFuzzer time over its executed units;
- a run over the bar, or one that did not exit 0 with no finding, or on Linux one whose sibling
  was not kept busy at its start and its end, is marked pass, or a run that meets all of that is
  marked fail;
- its summary is not what its runs give: each host's seeds, runs, seeds passed, whether all
  passed, and its worst run, with every run tied for it and its seed's median.

And, tying the two records together: a disturbance entry whose phase is "gate run" must name,
as "<seed> run <K> of <N>", a run the gate file of its commit holds on the Linux host, whose
records are the only ones the disturbance files keep, with N its runs a seed, and its span must
overlap that run's start and end. A "gate run" entry with no gate file of its commit is refused.

Exit 0 when every file passes, 1 when one is refused, 2 on a usage error.
Standard library only.
"""

import argparse
import datetime
import hashlib
import json
import math
import re
import statistics
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_results  # noqa: E402

SCHEMA = "tick-store-fuzz-gate-1"
HOSTS = ("macOS", "Linux")
# The gate's runs a seed: fuzz/README.md's gate paragraph gives five runs of each named seed.
REQUIRED_RUNS = 5
HEADER_KEYS = {"schema", "check", "commit", "statement", "assembled", "seeds_file",
               "seeds_file_sha256", "bar_ms_per_execution", "bar", "runs_per_seed", "hosts"}
RUN_KEYS = {"host", "seed", "target", "run", "start", "end", "exit_code", "findings",
            "executed_units", "file_ms", "ms_per_execution", "wall_ms", "load1", "pass"}
LINUX_KEYS = {"cpu_busy_percent", "processes_started", "sibling_busy_at_start",
              "sibling_busy_at_end"}
UTC = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ")
CASE = re.compile(r"^(\S+) run (\d+) of (\d+)$")
# The header's words, as scripts/gate_results.py writes them.
SEEDS_FILE = "fuzz/bound_seeds.txt"
CHECK = "G-3's bound-seed gate"
BAR = "half of fuzz/launch.txt's -timeout={timeout}, in ms an execution"
STATEMENT = ("Before G-3's fuzzing runs, each named seed of fuzz/bound_seeds.txt run alone through "
             "its fuzz target, built by the fuzz preset at this commit, with fuzz/launch.txt's "
             "flags and -max_len and -runs=1, {per_seed} times on each host. A run's time an "
             "execution is libFuzzer's time for the whole run of the input, its Executed line, "
             "over its count of executed units, 2 where the leak check reran the input; a run "
             "passes if it exited 0 with no finding and within the bar, and on Linux with its "
             "sibling kept busy at its start and its end; the gate passes if every run of every "
             "seed does.")
ASSEMBLED = re.compile(
    r"Assembled on (\d{4}-\d\d-\d\d) by scripts/gate_results\.py, a script written after the "
    r"runs, from the raw records of the runs of (.+): each host's step log and each run's "
    r"libFuzzer output, and on Linux each run's per-CPU counter copies, wall time and record of "
    r"its sibling's spinning process\. The runner that ran the gate and wrote those records is "
    r"not in the repository\. Nothing was timed again\.")
HOST_FACTS = {
    "macOS": {
        "placement": ("each run alone under heavy.sh, unpinned, with fuzz/launch.txt's macOS "
                      "leak options and the build's own kernel set"),
        "kept": ("the step log's start and end of each run, UTC to the second, its exit code, and "
                 "its line of figures with the 1-minute load read just before it began; "
                 "libFuzzer's output. No per-CPU record")},
    "Linux": {
        "placement": ("each run alone under heavy.sh, pinned to cpu6 with taskset, at "
                      "TICK_STORE_SIMD=avx2, with fuzz/launch.txt's Linux leak options, and cpu7, "
                      "cpu6's SMT sibling, kept busy for the whole run by a spinning process "
                      "started a second before it"),
        "kept": ("as on macOS, and the kernel's per-CPU counters copied before and after the "
                 "run, which give each cpu's busy share over it and the processes started "
                 "during it; its wall time; and whether the spinning process was running at its "
                 "start and at its end")}}
DISTURBANCES = "disturbances-*.json"
EXIT_OK, EXIT_INVALID, EXIT_USAGE = 0, 1, 2


def bound_seeds(path):
    """{seed name: its fuzz target} of fuzz/bound_seeds.txt, in its order, the target named as
    the assembler names it; None where a line is not "target seed"."""
    seeds = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        words = [word for word in re.split(r"[,\s]+", line.strip()) if word]
        if words and not words[0].startswith("#"):
            target = words[0] if words[0].startswith("fuzz_") else f"fuzz_{words[0]}"
            seeds[words[-1]] = target if len(words) == 2 else None
    return seeds


def timeout_of(launch):
    """fuzz/launch.txt's -timeout, in seconds, from its flags row, or None."""
    for line in Path(launch).read_text(encoding="utf-8").splitlines():
        words = line.split()
        if words and words[0] == "flags":
            found = [word for word in words[1:] if re.fullmatch(r"-timeout=\d+", word)]
            if len(found) == 1:
                return int(found[0].split("=", 1)[1])
    return None


def is_count(value, least=0):
    return isinstance(value, int) and not isinstance(value, bool) and value >= least


def is_real(value, least=0.0, most=math.inf):
    """A float, as the assembler writes a time an execution, a load or a share: finite and
    from least to most."""
    return isinstance(value, float) and math.isfinite(value) and least <= value <= most


def is_utc(value):
    """A real UTC time to the second, as the step log gives each run's start and end."""
    if not isinstance(value, str) or not UTC.fullmatch(value):
        return False
    try:
        datetime.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError:
        return False
    return True


def is_shares(value):
    """The assembler's busy shares: each cpu the counters name, in their order, cpu2 and cpu7
    among them, each to one decimal from 0 to 100. Of names cpuN with no leading zero, the
    shorter comes first and names of one length sort as their numbers do."""
    return isinstance(value, dict) and \
        all(re.fullmatch(r"cpu(0|[1-9]\d*)", cpu) for cpu in value) and \
        list(value) == sorted(value, key=lambda cpu: (len(cpu), cpu)) and \
        {"cpu2", "cpu7"} <= set(value) and \
        all(is_real(share, 0.0, 100.0) and share == float(f"{share:.1f}")
            for share in value.values())


# What the assembler writes in each run's field, and what a refusal calls it.
WHOLE, PAST_ONE = "a whole number of at least 0", "a whole number of at least 1"
REAL, TIME = "a finite number of at least 0", "a real UTC time to the second"
TRUTH = "true or false"
RUN_FORMS = (
    ("seed", lambda value: isinstance(value, str), "a seed's name"),
    ("target", lambda value: isinstance(value, str), "a fuzz target's name"),
    ("run", lambda value: is_count(value, 1), PAST_ONE),
    ("start", is_utc, TIME),
    ("end", is_utc, TIME),
    ("exit_code", is_count, WHOLE),
    ("findings", is_count, WHOLE),
    ("executed_units", lambda value: is_count(value, 1), PAST_ONE),
    ("file_ms", is_count, WHOLE),
    ("ms_per_execution", is_real, REAL),
    ("wall_ms", is_count, WHOLE),
    ("load1", is_real, REAL),
    ("pass", lambda value: isinstance(value, bool), TRUTH))
LINUX_FORMS = (
    ("cpu_busy_percent", is_shares, "each cpu's busy share, cpu2 and cpu7 among them, in the "
                                    "cpus' order, to one decimal from 0 to 100"),
    ("processes_started", is_count, WHOLE),
    ("sibling_busy_at_start", lambda value: isinstance(value, bool), TRUTH),
    ("sibling_busy_at_end", lambda value: isinstance(value, bool), TRUTH))


def shape_problems(document, name):
    """The file's shape: its schema, header, runs' fields and name."""
    if not isinstance(document, dict) or set(document) != {"header", "runs", "summary"}:
        return ["not a gate file: it holds header, runs and summary and nothing else"]
    header, runs = document["header"], document["runs"]
    if not isinstance(header, dict) or header.get("schema") != SCHEMA:
        return [f"its schema is not {SCHEMA!r}"]
    if set(header) != HEADER_KEYS:
        return [f"its header gives {sorted(header)}, not {sorted(HEADER_KEYS)}"]
    commit = header["commit"]
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit) or \
            name != f"fuzz-gate-{commit[:12]}.json":
        return [f"its commit {commit!r} is not a full commit its name gives"]
    if not is_count(header["bar_ms_per_execution"], 1) or not is_count(header["runs_per_seed"]):
        return ["its bar and its runs a seed are not whole numbers"]
    if not isinstance(runs, list) or not runs:
        return ["it holds no runs"]
    problems = []
    for index, run in enumerate(runs, 1):
        wanted = RUN_KEYS | (LINUX_KEYS if isinstance(run, dict) and
                             run.get("host") == "Linux" else set())
        if not isinstance(run, dict) or run.get("host") not in HOSTS or set(run) != wanted:
            problems.append(f"run {index} is not a run of {list(HOSTS)} with the fields "
                            f"{sorted(wanted)}")
            continue
        forms = RUN_FORMS + (LINUX_FORMS if run["host"] == "Linux" else ())
        wrong = [f"run {index}: its {key} {run[key]!r} is not {what}"
                 for key, form, what in forms if not form(run[key])]
        problems += wrong
        if not wrong and run["end"] < run["start"]:
            problems.append(f"run {index}: it ends at {run['end']}, before it starts, "
                            f"{run['start']}")
    return problems


def run_problems(runs, bar):
    """Each run's time an execution and its pass, from its own fields."""
    problems = []
    for run in runs:
        where = f"{run['host']}: {run['seed']} run {run['run']}"
        if run["ms_per_execution"] != run["file_ms"] / run["executed_units"]:
            problems.append(f"{where}: {run['ms_per_execution']!r} ms an execution is not its "
                            f"libFuzzer {run['file_ms']} ms over {run['executed_units']} units")
            continue
        over = run["ms_per_execution"] > bar
        if over and run["pass"]:
            problems.append(f"{where}: {run['ms_per_execution']} ms an execution is over the "
                            f"bar, {bar}, and the run is marked pass")
            continue
        busy = run["host"] != "Linux" or (run["sibling_busy_at_start"] is True and
                                          run["sibling_busy_at_end"] is True)
        passed = run["exit_code"] == 0 and run["findings"] == 0 and not over and busy
        if run["pass"] is not passed:
            problems.append(f"{where}: marked {'pass' if run['pass'] else 'fail'}, and its exit "
                            "code, findings, time and sibling give the other")
    return problems


def header_problems(header, runs, digest, timeout):
    """The header's words against the assembler's and the tree's list, by its SHA-256."""
    problems = []
    if header["check"] != CHECK:
        problems.append(f"its check {header['check']!r} is not {CHECK!r}")
    if header["seeds_file"] != SEEDS_FILE:
        problems.append(f"its seeds_file {header['seeds_file']!r} is not {SEEDS_FILE!r}")
    if header["seeds_file_sha256"] != digest:
        problems.append(f"its seeds_file_sha256 {header['seeds_file_sha256']!r} is not the "
                        f"tree's {SEEDS_FILE}'s, {digest}")
    if header["bar"] != BAR.format(timeout=timeout):
        problems.append(f"its bar {header['bar']!r} is not "
                        f"{BAR.format(timeout=timeout)!r}")
    if header["statement"] != STATEMENT.format(per_seed=header["runs_per_seed"]):
        problems.append(f"its statement is not the assembler's for "
                        f"{header['runs_per_seed']} runs a seed")
    days = ", ".join(sorted({run["start"][:10] for run in runs}))
    found = ASSEMBLED.fullmatch(header["assembled"]) if isinstance(header["assembled"], str) \
        else None
    day, of = (found[1], found[2]) if found else (None, None)
    if not found:
        problems.append(f"its assembled {header['assembled']!r} is not the assembler's account "
                        "of its assembly")
    elif of != days:
        problems.append(f"its assembled gives the runs of {of}, and its runs are of {days}")
    elif not is_utc(f"{day}T00:00:00Z"):
        problems.append(f"its assembled gives {day} as the day it was assembled, which is no day")
    if header["hosts"] != HOST_FACTS:
        problems.append(f"its hosts are not the assembler's facts of {' and '.join(HOSTS)}")
    elif list(header["hosts"]) != list(HOSTS):
        problems.append(f"its hosts are not in the order {', '.join(HOSTS)}")
    return problems


def coverage_problems(runs, seeds, per_seed):
    """Every seed of the list on every host, each with its runs numbered 1 to per_seed."""
    problems = []
    if per_seed != REQUIRED_RUNS:
        problems.append(f"it gives {per_seed} runs a seed, and the gate requires "
                        f"{REQUIRED_RUNS}")
    for host in HOSTS:
        mine = [run for run in runs if run["host"] == host]
        held = list(dict.fromkeys(run["seed"] for run in mine))
        for seed in seeds:
            if seed not in held:
                problems.append(f"{host}: no run of {seed}, a seed of fuzz/bound_seeds.txt")
        for seed in held:
            if seed not in seeds:
                problems.append(f"{host}: {seed} is not a seed of fuzz/bound_seeds.txt")
            numbers = sorted(run["run"] for run in mine if run["seed"] == seed)
            if numbers != list(range(1, per_seed + 1)):
                problems.append(f"{host}: {seed} has runs {numbers}, not 1 to {per_seed}")
        for run in mine:
            if run["seed"] in seeds and run["target"] != seeds[run["seed"]]:
                problems.append(f"{host}: {run['seed']} run {run['run']}'s target "
                                f"{run['target']!r} is not its own, {seeds[run['seed']]!r}")
    return problems


def summary_of(runs, per_seed):
    """What the summary must say, from the rows alone."""
    summary = {}
    for host in HOSTS:
        mine = [run for run in runs if run["host"] == host]
        if not mine:
            continue
        seeds = list(dict.fromkeys(run["seed"] for run in mine))
        passed = sum(1 for seed in seeds
                     if [run["pass"] for run in mine if run["seed"] == seed] == [True] * per_seed)
        worst = max(run["ms_per_execution"] for run in mine)
        first = next(run for run in mine if run["ms_per_execution"] == worst)
        same = [run for run in mine if run["seed"] == first["seed"]]
        summary[host] = {
            "seeds": len(seeds), "runs": len(mine), "seeds_passed": passed,
            "all_pass": passed == len(seeds),
            "worst": {"seed": first["seed"], "target": first["target"],
                      "runs": sorted(run["run"] for run in same
                                     if run["ms_per_execution"] == worst),
                      "ms_per_execution": worst, "file_ms": first["file_ms"],
                      "executed_units": first["executed_units"],
                      "seed_median_ms_per_execution": statistics.median(
                          run["ms_per_execution"] for run in same)}}
    return summary


def check_document(document, name, seeds, timeout, digest):
    """Every problem of one gate file, against the tree's list of seeds, the list's SHA-256 and
    the tree's -timeout."""
    problems = shape_problems(document, name)
    if problems:
        return problems
    problems += [f"it holds {leak}" for leak in check_results.check_leaks(document)]
    header, runs = document["header"], document["runs"]
    bar, per_seed = header["bar_ms_per_execution"], header["runs_per_seed"]
    if timeout is None or bar != timeout * 1000 // 2:
        problems.append(f"its bar, {bar} ms an execution, is not half of fuzz/launch.txt's "
                        f"-timeout={timeout}")
    problems += header_problems(header, runs, digest, timeout)
    problems += coverage_problems(runs, seeds, per_seed)
    problems += run_problems(runs, bar)
    if not problems and document["summary"] != summary_of(runs, per_seed):
        problems.append("its summary is not what its runs give")
    return problems


def gate_run_of(entry, document):
    """The Linux run of a gate file a "gate run" entry names, or why it names none."""
    found = CASE.match(str(entry.get("case")))
    if not found:
        return None, f"its case {entry.get('case')!r} is not \"<seed> run <K> of <N>\""
    seed, number, of = found[1], int(found[2]), int(found[3])
    if of != document["header"]["runs_per_seed"]:
        return None, f"it gives {of} runs a seed, and the gate file " \
            f"{document['header']['runs_per_seed']}"
    runs = [run for run in document["runs"] if run["host"] == "Linux" and
            run["seed"] == seed and run["run"] == number]
    if len(runs) != 1:
        return None, f"the gate file holds no Linux run {number} of {seed}"
    return runs[0], None


def entry_problems(root, documents):
    """Each "gate run" entry of the root's disturbance files, against the gate file of its
    commit: the run it names must be there, and its span must overlap that run's."""
    problems = []
    for path in sorted((Path(root) / "results").glob(DISTURBANCES)):
        record = json.loads(path.read_text(encoding="utf-8"))
        commit = str(record.get("commit"))
        for index, entry in enumerate(record.get("entries") or [], 1):
            if not isinstance(entry, dict) or entry.get("phase") != "gate run":
                continue
            where = f"{path.name} entry {index}"
            document = documents.get(commit[:12])
            if document is None:
                problems.append(f"{where}: a gate run entry, and there is no valid "
                                f"fuzz-gate-{commit[:12]}.json")
                continue
            run, why = gate_run_of(entry, document)
            if run is None:
                problems.append(f"{where}: {why}")
            elif not (str(entry.get("start")) <= run["end"] and
                      str(entry.get("end")) >= run["start"]):
                problems.append(f"{where}: its span, {entry.get('start')} to {entry.get('end')}, "
                                f"does not overlap {entry['case']}, {run['start']} to "
                                f"{run['end']}")
    return problems


def check(root, paths=None):
    """(the problems of each gate file by name, the entries' problems, the valid files by the
    first 12 of their commit)."""
    root = Path(root)
    paths = sorted((root / "results").glob("fuzz-gate-*.json")) if paths is None else paths
    found, documents = {}, {}
    if paths:
        seeds = bound_seeds(root / SEEDS_FILE)
        digest = hashlib.sha256((root / SEEDS_FILE).read_bytes()).hexdigest()
        timeout = timeout_of(root / "fuzz" / "launch.txt")
    for path in paths:
        try:
            document = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            found[Path(path).name] = [str(error)]
            continue
        problems = check_document(document, Path(path).name, seeds, timeout, digest)
        found[Path(path).name] = problems
        if not problems:
            documents[document["header"]["commit"][:12]] = document
    return found, entry_problems(root, documents), documents


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate the bound-seed gate's results file.")
    parser.add_argument("files", nargs="*", type=Path)
    parser.add_argument("--root", type=Path, default=REPO_ROOT,
                        help="the tree whose fuzz/ list, launch file and disturbance files to "
                             "check against")
    args = parser.parse_args(argv)
    try:
        found, entries, documents = check(args.root, args.files or None)
    except (OSError, json.JSONDecodeError) as error:
        print(f"check_gate: {error}", file=sys.stderr)
        return EXIT_USAGE
    failed = bool(entries)
    for name, problems in found.items():
        for problem in problems:
            print(f"{name}: {problem}", file=sys.stderr)
        failed = failed or bool(problems)
        if not problems:
            document = next(document for document in documents.values()
                            if f"fuzz-gate-{document['header']['commit'][:12]}.json" == name)
            print(f"{name}: ok, {len(document['runs'])} runs on "
                  f"{len(document['summary'])} hosts")
    for problem in entries:
        print(problem, file=sys.stderr)
    return EXIT_INVALID if failed else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
