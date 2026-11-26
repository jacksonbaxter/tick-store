#!/usr/bin/env python3
"""Assemble the results file of G-3's bound-seed gate from each host's raw records of it.

    python3 -I scripts/gate_results.py --commit SHA --assembled-on YYYY-MM-DD \\
        --out results/fuzz-gate-<commit>.json --host macOS=DIR --host Linux=DIR

Before its 30-minute fuzzing runs, G-3's run on each host timed every named seed of
fuzz/bound_seeds.txt alone through its fuzz target, five times, against a bar of half
fuzz/launch.txt's -timeout an execution (fuzz/README.md, "Bounds"). An untracked runner on each
host ran that gate and wrote its records; this script was written afterwards, on 2026-11-25,
and turns them into a results file. It times nothing. DIR is one host's run folder as that
runner left it:

    status.txt                     the runner's step log: the run's commit on its first line;
                                   for the gate's step, its list's SHA-256, its seed count, its
                                   runs a seed and its bar; each run's start and end, UTC, with
                                   its exit code, and its line of figures, with the 1-minute load
                                   read just before it began
    raw/<step>-bound-seeds/        the gate's own folder:
      list.txt                     each named seed with its target, in the list's order
      seed-N-<state>-E.log         libFuzzer's output for run E of the list's seed N
      seedtime.tsv, gate.tsv       the runner's table of runs and the gate's verdicts, read
                                   only to refuse records that disagree with them
      and on Linux, for each run:
      seed-N-spin-E.stat0, .stat1  the kernel's per-CPU counters, copied before and after it
      seed-N-spin-E.wall           its wall time in ms
      seed-N-spin-E.beside         whether the process keeping cpu7, cpu6's SMT sibling, busy
                                   was running at its start and at its end

Each run's time an execution is the gate's own: libFuzzer's one "Executed <input> in N ms",
which covers every execution of the input, the leak check's rerun included, over its
stat::number_of_executed_units, 2 where the leak check reran the input and 1 for the Gorilla
seeds; libFuzzer prints no time for one execution alone. A run passes as the gate passed it:
exit 0, no finding in its log, one Executed line and a unit count of at least 1, its time an
execution within the bar, and on Linux the sibling's busy process running at both ends. Each
cpu's busy share is the gate's, everything but idle and iowait between the two copies, to one
decimal; the processes started during a run are the counters' `processes` between them. The
gate held the shares, the load and the processes to no rule, and they are recorded, not judged.

The commit must be the runs' own, the list the tree's fuzz/bound_seeds.txt by its SHA-256, and
the bar half the tree's fuzz/launch.txt -timeout: G-3's fuzz kind ties fuzz/ to the commit, so
the tree's are the run's. A record that disagrees with the runner's own table or the gate's
verdicts stops the assembly. Nothing of a machine's paths or names goes into the file. The file
is validated by scripts/check_gate.py, which render_results.py runs.

Standard library only.
"""

import argparse
import hashlib
import json
import re
import statistics
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

SCHEMA = "tick-store-fuzz-gate-1"
HOSTS = ("macOS", "Linux")
# The run labels the runner gives each host's runs, seed-N-<state>-E.
STATES = {"macOS": "mac", "Linux": "spin"}
SPIN_CPU, THIRD_CPU = 7, 2
BUSY_BOTH_ENDS = "before: spinner running; after: spinner running"

COMMIT_LINE = re.compile(r"\bcommit=([0-9a-f]{40})\b")
BAR_LINE = re.compile(r"\] \S+bound_seeds\.txt sha256 ([0-9a-f]{64}), (\d+) seeds, each run (\d+) "
                      r"times; bar (\d+) ms an execution \(half of launch\.txt's -timeout=(\d+)\)")
EDGE_LINE = re.compile(r"\[(\S+-bound-seeds)\] seed-(\d+)-(\w+)-(\d+) (start|end) "
                       r"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)(?: exit (\d+))?$")
FIGURES_LINE = re.compile(r"\[\S+-bound-seeds\] seed (\d+) (\S+) (\S+) run (\d+) of (\d+): "
                          r"libFuzzer (\S+) ms, units (\S+), wall (\S+) ms, exit (\d+), "
                          r"([^;]*);(?: cpu(\d+) (\S+)% busy, cpu(\d+) (\S+)%;)? load (\S+)"
                          r"(?:; (.*))?$")
EXECUTED = re.compile(r"^Executed (.+) in (\d+) ms$")
UNITS = re.compile(r"^stat::number_of_executed_units:\s*(\d+)\s*$")
# What the gate's runner counted as a finding in a run's log.
FINDINGS = ("==ERROR:", "ERROR: libFuzzer", "runtime error:", "SUMMARY: ", "Test unit written to",
            "ALARM: working on the last Unit", "==WARNING: libFuzzer")
TABLE_COLUMNS = ("target", "seed", "state", "exec", "libfuzzer_ms", "wall_ms", "exit",
                 "sibling_busy_pct", "load1", "timeout_s", "outcome", "units", "beside",
                 "cpu2_busy_pct", "log")


class Refused(Exception):
    """Records that cannot make the file, with why."""


def bound_seeds(path):
    """(target, seed) for each line of fuzz/bound_seeds.txt, in its order."""
    found = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        words = [word for word in re.split(r"[,\s]+", line.strip()) if word]
        if words and not words[0].startswith("#"):
            if len(words) != 2:
                raise Refused(f"{Path(path).name}: not \"target seed\": {line!r}")
            target = words[0] if words[0].startswith("fuzz_") else f"fuzz_{words[0]}"
            found.append((target, words[1]))
    return found


def timeout_of(launch):
    """fuzz/launch.txt's -timeout, in seconds, from its flags row."""
    for line in Path(launch).read_text(encoding="utf-8").splitlines():
        words = line.split()
        if words and words[0] == "flags":
            found = [word for word in words[1:] if re.fullmatch(r"-timeout=\d+", word)]
            if len(found) == 1:
                return int(found[0].split("=", 1)[1])
    raise Refused(f"{Path(launch).name} has no flags row with one -timeout=N")


def counters(path):
    """{cpu name: (busy, total)} and the processes count of a copy of the kernel's counters."""
    cpus, processes = {}, None
    for line in Path(path).read_text(encoding="ascii").splitlines():
        words = line.split()
        if words and re.fullmatch(r"cpu\d+", words[0]):
            values = [int(value) for value in words[1:]]
            idle = values[3] + (values[4] if len(values) > 4 else 0)
            cpus[words[0]] = (sum(values) - idle, sum(values))
        elif words and words[0] == "processes":
            processes = int(words[1])
    if not cpus or processes is None:
        raise Refused(f"{Path(path).name} holds no per-cpu lines or no processes line")
    return cpus, processes


def busy_shares(before, after):
    """Each cpu's busy share between two copies, to one decimal, as the gate gave them."""
    first, started0 = counters(before)
    second, started1 = counters(after)
    if sorted(first) != sorted(second):
        raise Refused(f"{Path(before).name} and {Path(after).name} name other cpus")
    shares = {}
    for cpu in sorted(first, key=lambda name: int(name[3:])):
        busy = second[cpu][0] - first[cpu][0]
        total = second[cpu][1] - first[cpu][1]
        if total <= 0:
            raise Refused(f"{Path(before).name}: no time passed on {cpu}")
        shares[cpu] = float(f"{100.0 * busy / total:.1f}")
    return shares, started1 - started0


def read_log(path, seed):
    """(file_ms, executed_units, the lines that report a finding) from one run's libFuzzer
    output."""
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    executed = [match for match in map(EXECUTED.match, lines) if match]
    units = [match for match in map(UNITS.match, lines) if match]
    findings = [line for line in lines if any(mark in line for mark in FINDINGS)]
    if len(executed) != 1 or not executed[0][1].endswith(f"/{seed}"):
        raise Refused(f"{Path(path).name}: not one Executed line for {seed}")
    if len(units) != 1 or int(units[0][1]) < 1:
        raise Refused(f"{Path(path).name}: not one unit count of at least 1")
    return int(executed[0][2]), int(units[0][1]), findings


def read_table(path, columns):
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if not lines or tuple(lines[0].split("\t")) != columns:
        raise Refused(f"{Path(path).name}: not the columns {columns}")
    return [dict(zip(columns, line.split("\t"))) for line in lines[1:] if line.strip()]


def number(text):
    return float(text) if "." in text else int(text)


def host_runs(host, folder, seeds, commit):
    """Every run of the gate on one host, in the list's order, and the step's bar line."""
    folder = Path(folder)
    status = (folder / "status.txt").read_text(encoding="utf-8").splitlines()
    first = COMMIT_LINE.search(status[0]) if status else None
    if first is None or first[1] != commit:
        raise Refused(f"{host}: the run's commit is {first[1] if first else 'not given'}, not "
                      f"{commit}")
    steps = sorted(folder.glob("raw/*-bound-seeds"))
    if len(steps) != 1:
        raise Refused(f"{host}: not one raw/*-bound-seeds folder in the run's folder")
    step = steps[0]
    bars = [match for line in status if f"[{step.name}]" in line
            for match in [BAR_LINE.search(line)] if match]
    if len(bars) != 1:
        raise Refused(f"{host}: the step log holds not one line of the gate's list and bar")
    bar = bars[0]
    listed = [line.split("\t") for line in
              (step / "list.txt").read_text(encoding="utf-8").splitlines() if line.strip()]
    names = [(target, seed_path.split("/fuzz-seeds/", 1)[-1]) for target, seed_path, _ in listed]
    if names != seeds:
        raise Refused(f"{host}: the gate's list is not fuzz/bound_seeds.txt's seeds in order")
    edges, figures = {}, {}
    state = STATES[host]
    for line in status:
        match = EDGE_LINE.search(line)
        if match and match[1] == step.name and match[3] == state:
            key = (int(match[2]), int(match[4]))
            edges.setdefault(key, {})[match[5]] = (match[6], match[7])
        match = FIGURES_LINE.search(line)
        if match and f"[{step.name}]" in line:
            figures[(int(match[1]), int(match[4]))] = match
    table = read_table(step / "seedtime.tsv", TABLE_COLUMNS)
    rows = {}
    for row in table:
        stem = Path(row["log"]).name.removesuffix(".log")
        parts = stem.split("-")
        rows[(int(parts[1]), int(parts[3]))] = row
    per_seed, limit = int(bar[3]), bar_ms(bar)
    runs = []
    for index, (target, seed) in enumerate(seeds, 1):
        for run in range(1, per_seed + 1):
            key, where = (index, run), f"{host}: {seed} run {run}"
            stem = step / f"seed-{index}-{state}-{run}"
            edge, figure, row = edges.get(key, {}), figures.get(key), rows.get(key)
            if "start" not in edge or "end" not in edge or figure is None or row is None:
                raise Refused(f"{where}: no start, end, line of figures and table row")
            if (row["target"].removesuffix("-avx2"), row["seed"], figure[2], figure[3]) != (
                    target, seed, target, seed):
                raise Refused(f"{where}: the runner's table or step log names another seed")
            file_ms, units, findings = read_log(stem.with_suffix(".log"), seed)
            exit_code = int(edge["end"][1])
            record = {"seed": seed, "target": target, "run": run, "start": edge["start"][0],
                      "end": edge["end"][0], "exit_code": exit_code, "findings": len(findings),
                      "executed_units": units, "file_ms": file_ms,
                      "ms_per_execution": file_ms / units, "wall_ms": int(figure[8]),
                      "load1": float(figure[15])}
            told = {"libfuzzer_ms": str(file_ms), "units": str(units), "exit": str(exit_code),
                    "wall_ms": figure[8], "load1": figure[15], "beside": "-",
                    "sibling_busy_pct": "-", "cpu2_busy_pct": "-"}
            busy_both_ends = True
            if host == "Linux":
                shares, started = busy_shares(stem.with_suffix(".stat0"),
                                              stem.with_suffix(".stat1"))
                beside = stem.with_suffix(".beside").read_text(encoding="utf-8").strip()
                record["wall_ms"] = int(stem.with_suffix(".wall").read_text(encoding="utf-8"))
                record["cpu_busy_percent"] = shares
                record["processes_started"] = started
                record["sibling_busy_at_start"] = beside.startswith("before: spinner running;")
                record["sibling_busy_at_end"] = beside.endswith("; after: spinner running")
                busy_both_ends = beside == BUSY_BOTH_ENDS
                told.update({"beside": beside,
                             "sibling_busy_pct": f"{shares[f'cpu{SPIN_CPU}']:.1f}",
                             "cpu2_busy_pct": f"{shares[f'cpu{THIRD_CPU}']:.1f}"})
                if (figure[11], figure[12], figure[13], figure[14], figure[16]) != (
                        str(SPIN_CPU), told["sibling_busy_pct"], str(THIRD_CPU),
                        told["cpu2_busy_pct"], beside) or record["wall_ms"] != int(figure[8]):
                    raise Refused(f"{where}: the step log's shares, busy process or wall time "
                                  "are not the run's own records'")
            if (figure[6], figure[7], figure[9]) != (told["libfuzzer_ms"], told["units"],
                                                       told["exit"]):
                raise Refused(f"{where}: the step log's figures are not its libFuzzer output's")
            for column, value in told.items():
                if row[column] != value:
                    raise Refused(f"{where}: the runner's table gives {column} {row[column]!r}, "
                                  f"the records {value!r}")
            clean = exit_code == 0 and not findings
            if (row["outcome"] == "ok", figure[10] == "ok") != (clean, clean):
                raise Refused(f"{where}: the runner's outcome {row['outcome']!r} is not what its "
                              "records give")
            record["pass"] = clean and busy_both_ends and file_ms <= limit * units
            runs.append(record)
    if len(table) != len(runs):
        raise Refused(f"{host}: the runner's table holds {len(table)} runs, the records "
                      f"{len(runs)}")
    check_gate_table(host, step / "gate.tsv", runs, bar_ms(bar))
    return runs, bar


def bar_ms(bar):
    return int(bar[4])


def check_gate_table(host, path, runs, limit):
    """The gate's own verdict of each seed, its worst and median, must be the runs'."""
    columns = ("target", "seed", "runs", "units", "worst_ms_per_execution",
               "median_ms_per_execution", "limit_ms", "verdict")
    verdicts = {row["seed"]: row for row in read_table(path, columns)}
    for seed in dict.fromkeys(run["seed"] for run in runs):
        times = [run["ms_per_execution"] for run in runs if run["seed"] == seed]
        row = verdicts.get(seed)
        if row is None or row["worst_ms_per_execution"] != f"{max(times):.1f}" or \
                row["median_ms_per_execution"] != f"{statistics.median(times):.1f}" or \
                row["limit_ms"] != str(limit) or (row["verdict"] == "ok") != all(
                    run["pass"] for run in runs if run["seed"] == seed):
            raise Refused(f"{host}: the gate's verdict of {seed} is not what its runs give")


def summary_of(runs, per_seed):
    """Per host: its seeds and runs, how many seeds passed, whether all did, and its worst run,
    every run that took that time, with its seed's median."""
    summary = {}
    for host in HOSTS:
        mine = [run for run in runs if run["host"] == host]
        if not mine:
            continue
        seeds = list(dict.fromkeys(run["seed"] for run in mine))
        passed = [seed for seed in seeds if all(run["pass"] for run in mine if run["seed"] == seed)
                  and sum(run["seed"] == seed for run in mine) == per_seed]
        worst = max(mine, key=lambda run: run["ms_per_execution"])
        tied = [run["run"] for run in mine if run["seed"] == worst["seed"]
                and run["ms_per_execution"] == worst["ms_per_execution"]]
        summary[host] = {
            "seeds": len(seeds), "runs": len(mine), "seeds_passed": len(passed),
            "all_pass": len(passed) == len(seeds),
            "worst": {"seed": worst["seed"], "target": worst["target"], "runs": tied,
                      "ms_per_execution": worst["ms_per_execution"],
                      "file_ms": worst["file_ms"], "executed_units": worst["executed_units"],
                      "seed_median_ms_per_execution": statistics.median(
                          run["ms_per_execution"] for run in mine
                          if run["seed"] == worst["seed"])}}
    return summary


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


def assemble(commit, assembled_on, hosts, seeds_file, launch):
    seeds = bound_seeds(seeds_file)
    timeout = timeout_of(launch)
    digest = hashlib.sha256(Path(seeds_file).read_bytes()).hexdigest()
    runs, per_seed = [], None
    for host, folder in sorted(hosts, key=lambda item: HOSTS.index(item[0])):
        found, bar = host_runs(host, folder, seeds, commit)
        if bar[1] != digest or int(bar[2]) != len(seeds):
            raise Refused(f"{host}: the gate ran a list whose SHA-256 is {bar[1]}, not the tree's "
                          f"fuzz/bound_seeds.txt, {digest}")
        if (int(bar[5]), bar_ms(bar)) != (timeout, timeout * 1000 // 2):
            raise Refused(f"{host}: the gate's bar, {bar[4]} ms, is not half of fuzz/launch.txt's "
                          f"-timeout={timeout}")
        if per_seed not in (None, int(bar[3])):
            raise Refused("the hosts ran each seed a different number of times")
        per_seed = int(bar[3])
        runs += [{"host": host, **run} for run in found]
    dates = sorted({run["start"][:10] for run in runs})
    header = {
        "schema": SCHEMA, "check": "G-3's bound-seed gate", "commit": commit,
        "statement": ("Before G-3's fuzzing runs, each named seed of fuzz/bound_seeds.txt run "
                      f"alone through its fuzz target, built by the fuzz preset at this commit, "
                      f"with fuzz/launch.txt's flags and -max_len and -runs=1, {per_seed} times "
                      "on each host. A run's time an execution is libFuzzer's time for the whole "
                      "run of the input, its Executed line, over its count of executed units, 2 "
                      "where the leak check reran the input; a run passes if it exited 0 with no "
                      "finding and within the bar, and on Linux with its sibling kept busy at "
                      "its start and its end; the gate passes if every run of every seed does."),
        "assembled": (f"Assembled on {assembled_on} by scripts/gate_results.py, a script written "
                      f"after the runs, from the raw records of the runs of {', '.join(dates)}: "
                      "each host's step log and each run's libFuzzer output, and on Linux each "
                      "run's per-CPU counter copies, wall time and record of its sibling's "
                      "spinning process. The script that ran the gate and wrote those records is "
                      "not in the repository. Nothing was timed again."),
        "seeds_file": "fuzz/bound_seeds.txt", "seeds_file_sha256": digest,
        "bar_ms_per_execution": timeout * 1000 // 2,
        "bar": f"half of fuzz/launch.txt's -timeout={timeout}, in ms an execution",
        "runs_per_seed": per_seed,
        "hosts": {host: HOST_FACTS[host] for host, _ in sorted(
            hosts, key=lambda item: HOSTS.index(item[0]))}}
    return {"header": header, "runs": runs, "summary": summary_of(runs, per_seed)}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Assemble the bound-seed gate's results file.")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--assembled-on", required=True, help="the day it runs, YYYY-MM-DD")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--host", action="append", required=True, help="HOST=DIR")
    parser.add_argument("--seeds", type=Path, default=REPO_ROOT / "fuzz" / "bound_seeds.txt")
    parser.add_argument("--launch", type=Path, default=REPO_ROOT / "fuzz" / "launch.txt")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        parser.error("--commit: a full commit hash")
    if not re.fullmatch(r"\d{4}-\d\d-\d\d", args.assembled_on):
        parser.error("--assembled-on: YYYY-MM-DD")
    hosts = []
    for item in args.host:
        name, _, folder = item.partition("=")
        if name not in HOSTS or not folder or name in dict(hosts):
            parser.error(f"--host {item}: HOST=DIR, HOST one of {list(HOSTS)}, once each")
        hosts.append((name, Path(folder)))
    try:
        document = assemble(args.commit, args.assembled_on, hosts, args.seeds, args.launch)
    except (Refused, OSError, ValueError, IndexError) as error:
        print(f"gate_results: {error}", file=sys.stderr)
        return 1
    args.out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
