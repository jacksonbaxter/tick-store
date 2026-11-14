#!/usr/bin/env python3
"""Run G-3's 30-minute fuzz runs on this host and write the raw logs fuzz_results.py reads.

    python3 -I scripts/fuzz_g3.py --logs DIR [--parallel N | --cpus 2,6,7] [--level avx2]
        [--seconds 1800] [--build build/fuzz] [--corpus build/fuzz/corpus]
        [--work build/fuzz/g3] [--artifacts build/fuzz/artifacts] TARGET...

Each target runs in three parts, one after another, every one with fuzz/launch.txt's flags,
ASan options and -max_len, as scripts/check.sh's fuzz steps take them:

  1. The replay. Every saved corpus input and every seed is run once, with -runs=0, through the
     target and its oracle, as the start of a run does, leak check included. A finding, a
     nonzero exit or no Done line fails the target, and it stops there.
  2. The merge. -merge=1 of the same folders into an empty one keeps the inputs that hold all
     the coverage and features the replay reached, smallest first. libFuzzer restarts its merge
     child past an input that crashes it, so more than one attempt fails the target too.
  3. The timed run. -max_total_time=SECONDS on the merged folder alone, which loads in a
     fraction of the replay's time; its new inputs go to that folder and are then copied to the
     saved corpus. Its INITED line ends the load: the first input libFuzzer makes by mutation
     runs right after it, so the seconds before and after that line are the run's time before
     and after its first new input.

Each target writes to DIR, as <tag>, the target's name with -LEVEL after it if --level is given:
<tag>.replay.log, <tag>.merge.log, the timed run's <tag>.log, <tag>.meta in the form
fuzz_results.py reads (the commit, the start and end epochs, the environment, the command, the
exit code, and on Linux the fuzzer's VmHWM read every 5 s), on macOS <tag>.time from BSD
`time -l`, and <tag>.phases.json with each part's command, exit code and clock. If the replay
or the merge fails, <tag>.log and <tag>.meta are that part's, so the run is recorded as not
clean with its finding.

A target whose part raises in the launcher, or that a worker never records, is failed, and the
launcher exits 1. A logs folder that already holds a listed target's files from an earlier
launch is refused, so they are never read as this launch's.

--cpus pins each target to one of the CPUs given with taskset, as many at once as CPUs;
--parallel runs that many at once, unpinned. A checkout whose fuzzed code (check_fuzz.py's
TIED_PATHS) differs from its HEAD is refused: the file would name a commit it was not made at.

Standard library only. tests/scripts/test_fuzz_g3.py runs it on a stand-in fuzzer.
"""

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_fuzz  # noqa: E402
import fuzz_results  # noqa: E402

LAUNCH = REPO_ROOT / "fuzz" / "launch.txt"
UBSAN_OPTIONS = "print_stacktrace=1"
INITED = re.compile(r"^#\d+\s+INITED ")
EXIT_REFUSED = 2
# What a target's run leaves in the logs folder, which fuzz_results.py reads as the run's.
LOG_SUFFIXES = (".replay.log", ".merge.log", ".log", ".meta", ".time", ".phases.json")


def read_launch(path=LAUNCH):
    """fuzz/launch.txt as {"flags": [...], "asan_options": {system: options},
    "targets": {name: (corpus, seeds, max_len)}}."""
    launch = {"flags": None, "asan_options": {}, "targets": {}}
    for line in path.read_text(encoding="utf-8").splitlines():
        words = line.split()
        if not words or words[0].startswith("#"):
            continue
        if words[0] == "flags" and launch["flags"] is None:
            launch["flags"] = words[1:]
        elif words[0] == "asan_options" and len(words) == 3:
            launch["asan_options"].setdefault(words[1], words[2])
        elif words[0] == "target" and len(words) == 5:
            launch["targets"].setdefault(words[1], (words[2], words[3], int(words[4])))
    return launch


def commit_here(repo):
    """HEAD in a checkout, or the COMMIT file of a tree a sync exported."""
    done = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=False)
    top = subprocess.run(["git", "-C", str(repo), "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True, check=False)
    if done.returncode == 0 and Path(top.stdout.strip()).resolve() == Path(repo).resolve():
        changed = check_fuzz.tied_changes(repo, done.stdout.strip())
        if changed:
            raise SystemExit("fuzz_g3: the fuzzed code differs from HEAD, so the runs would "
                             f"name a commit they were not made at: {' '.join(changed)}")
        return done.stdout.strip()
    commit = Path(repo) / "COMMIT"
    if commit.exists():
        return commit.read_text(encoding="utf-8").strip()
    raise SystemExit("fuzz_g3: neither a git checkout nor an exported tree with a COMMIT file")


def stamp():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def run_part(command, environment, log_path, time_path=None, watch_inited=False):
    """Run one part with its output in log_path. Returns its clock: start and end epochs, the
    seconds from start to its INITED line if asked, its exit code, and on Linux its VmHWM."""
    if time_path is not None:
        command = ["/usr/bin/time", "-l", "-o", str(time_path), *command]
    started = time.time()
    clock = time.monotonic()
    inited = None
    peak = [0]
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, errors="replace")
        status = Path(f"/proc/{process.pid}/status")

        def poll():
            while process.poll() is None:
                try:
                    for line in status.read_text().splitlines():
                        if line.startswith("VmHWM:"):
                            peak[0] = max(peak[0], int(line.split()[1]))
                except (OSError, ValueError):
                    pass
                time.sleep(5)

        poller = threading.Thread(target=poll, daemon=True)
        if status.parent.exists():
            poller.start()
        for line in process.stdout:
            if watch_inited and inited is None and INITED.match(line):
                inited = time.monotonic() - clock
            log.write(line)
        code = process.wait()
    return {"start_epoch": started, "end_epoch": time.time(),
            "seconds": round(time.monotonic() - clock, 1),
            "seconds_to_inited": None if inited is None else round(inited, 1),
            "exit_code": code, "vmhwm_kb": peak[0] or None}


def merge_attempts(log_path):
    return log_path.read_text(encoding="utf-8", errors="replace").count("MERGE-OUTER: attempt ")


def tag_of(target, level):
    return f"{target}-{level}" if level else target


def left_over(logs, targets, level):
    """Files a target's run writes that are in the logs folder already, from an earlier launch,
    which fuzz_results.py would read as this one's (review RG-9)."""
    return sorted(path.name for target in targets for suffix in LOG_SUFFIXES
                  if (path := logs / f"{tag_of(target, level)}{suffix}").exists())


def run_target(target, args, launch, commit, cpu):
    corpus_name, seeds_name, max_len = launch["targets"][target]
    system = platform.system()
    asan = launch["asan_options"].get(system if system == "Darwin" else "Linux")
    environment = dict(os.environ, UBSAN_OPTIONS=UBSAN_OPTIONS, ASAN_OPTIONS=asan)
    if args.level:
        environment["TICK_STORE_SIMD"] = args.level
    tag = tag_of(target, args.level)
    logs = args.logs
    binary = args.build / "fuzz" / target
    seeds = args.build / "fuzz-seeds" / seeds_name
    corpus = args.corpus / corpus_name
    merged = args.work / tag
    corpus.mkdir(parents=True, exist_ok=True)
    args.artifacts.mkdir(parents=True, exist_ok=True)
    if merged.exists():
        shutil.rmtree(merged)
    merged.mkdir(parents=True)
    pin = ["taskset", "-c", str(cpu)] if cpu is not None else []
    common = [f"-max_len={max_len}", *launch["flags"],
              f"-artifact_prefix={args.artifacts}/"]
    replay = [*pin, str(binary), "-runs=0", *common, str(corpus), str(seeds)]
    merge = [*pin, str(binary), "-merge=1", *common, str(merged), str(corpus), str(seeds)]
    timed = [*pin, str(binary), f"-max_total_time={args.seconds}", *common, str(merged)]
    environment_line = (f"ASAN_OPTIONS={asan} UBSAN_OPTIONS={UBSAN_OPTIONS}"
                        + (f" TICK_STORE_SIMD={args.level}" if args.level else ""))
    phases = {"tag": tag, "commit": commit}
    parts = (("replay", replay, logs / f"{tag}.replay.log"),
             ("merge", merge, logs / f"{tag}.merge.log"))
    last = None
    for name, command, log_path in parts:
        clock = run_part(command, environment, log_path)
        phases[name] = {"command": " ".join(command), **clock}
        findings = fuzz_results.read_log(log_path)["findings"]
        failed = clock["exit_code"] != 0 or findings or (
            name == "replay" and not fuzz_results.read_log(log_path)["done"]) or (
            name == "merge" and merge_attempts(log_path) != 1)
        if failed:
            phases["failed_in"] = name
            shutil.copyfile(log_path, logs / f"{tag}.log")
            last = (command, clock)
            break
    else:
        before = {path.name for path in merged.iterdir()}
        time_path = logs / f"{tag}.time" if system == "Darwin" else None
        clock = run_part(timed, environment, logs / f"{tag}.log", time_path, watch_inited=True)
        phases["timed"] = {"command": " ".join(timed), **clock}
        new = [path for path in merged.iterdir() if path.name not in before]
        for path in new:
            if not (corpus / path.name).exists():
                shutil.copyfile(path, corpus / path.name)
        phases["timed"]["new_inputs_saved"] = len(new)
        last = (timed, clock)
    command, clock = last
    hwm = f" vmhwm_kb_polled={clock['vmhwm_kb']}" if clock["vmhwm_kb"] else ""
    (logs / f"{tag}.meta").write_text(
        f"commit={commit}\n"
        f"start={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(clock['start_epoch']))} "
        f"epoch={int(clock['start_epoch'])}\n"
        f"{environment_line}\n"
        f"command={' '.join(command)}\n"
        f"end={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(clock['end_epoch']))} "
        f"epoch={int(clock['end_epoch'])} exit={clock['exit_code']}{hwm}\n", encoding="utf-8")
    (logs / f"{tag}.phases.json").write_text(json.dumps(phases, indent=1) + "\n",
                                             encoding="utf-8")
    outcome = phases.get("failed_in")
    print(f"{stamp()} {tag}: " + (f"FAILED in the {outcome}" if outcome else
                                  f"replay {phases['replay']['seconds']} s, merge "
                                  f"{phases['merge']['seconds']} s, timed "
                                  f"{phases['timed']['seconds']} s, exit "
                                  f"{phases['timed']['exit_code']}"), flush=True)
    return outcome is None and clock["exit_code"] == 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run G-3's fuzz runs on this host.")
    parser.add_argument("targets", nargs="+")
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=check_fuzz.REQUIRED_SECONDS)
    parser.add_argument("--level", help="TICK_STORE_SIMD for every run, as avx2 on Linux")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--parallel", type=int, default=1)
    group.add_argument("--cpus", help="pin each run to one of these CPUs, as many at once")
    parser.add_argument("--build", type=Path, default=Path("build/fuzz"))
    parser.add_argument("--corpus", type=Path, default=Path("build/fuzz/corpus"))
    parser.add_argument("--work", type=Path, default=Path("build/fuzz/g3"))
    parser.add_argument("--artifacts", type=Path, default=Path("build/fuzz/artifacts"))
    parser.add_argument("--launch", type=Path, default=LAUNCH)
    parser.add_argument("--repo", type=Path, default=REPO_ROOT,
                        help="the tree whose commit the runs are at")
    args = parser.parse_args(argv)
    launch = read_launch(args.launch)
    unknown = [name for name in args.targets if name not in launch["targets"]]
    if unknown or launch["flags"] is None:
        print(f"fuzz_g3: {args.launch.name} has no flags row or no row for {unknown}",
              file=sys.stderr)
        return EXIT_REFUSED
    for name in args.targets:
        if not (args.build / "fuzz" / name).exists():
            print(f"fuzz_g3: {args.build / 'fuzz' / name} is not built", file=sys.stderr)
            return EXIT_REFUSED
    commit = commit_here(args.repo)
    stale = left_over(args.logs, args.targets, args.level)
    if stale:
        print(f"fuzz_g3: {args.logs} already holds an earlier launch's files for these targets, "
              f"which would be read as this launch's: {' '.join(stale)}. Give a new --logs",
              file=sys.stderr)
        return EXIT_REFUSED
    args.logs.mkdir(parents=True, exist_ok=True)
    slots = [int(cpu) for cpu in args.cpus.split(",")] if args.cpus else [None] * args.parallel
    queue = list(args.targets)
    results = {}
    lock = threading.Lock()

    def worker(cpu):
        while True:
            with lock:
                if not queue:
                    return
                name = queue.pop(0)
            try:
                results[name] = run_target(name, args, launch, commit, cpu)
            except Exception as error:  # noqa: BLE001 - any failure fails its target alone
                results[name] = False
                print(f"{stamp()} {tag_of(name, args.level)}: FAILED, the launcher raised "
                      f"{type(error).__name__}: {error}", flush=True)

    print(f"{stamp()} G-3 at {commit[:12]}: {len(queue)} targets, {len(slots)} at once",
          flush=True)
    workers = [threading.Thread(target=worker, args=(cpu,)) for cpu in slots]
    for thread in workers:
        thread.start()
        time.sleep(2)
    for thread in workers:
        thread.join()
    # A listed target with no result, whose worker ended before recording it, failed too.
    failed = sorted(name for name in args.targets if not results.get(name, False))
    print(f"{stamp()} done: {len(args.targets) - len(failed)} clean, failed: "
          f"{failed or 'none'}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
