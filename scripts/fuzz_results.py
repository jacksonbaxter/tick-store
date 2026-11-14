#!/usr/bin/env python3
"""Assemble the G-3 fuzzing results file from the raw logs of each host's runs.

    python3 -I scripts/fuzz_results.py --commit SHA --out results/fuzz-g3-<commit>.json \\
        --host macOS=DIR --host Linux=DIR [--strip PREFIX ...]

Each DIR holds host.json, the host's facts (cpu_model, kernel, compiler, compile_flags,
sanitizer_options, concurrency, peak_memory, default_kernel_level), and for each run what
scripts/fuzz_g3.py wrote:

    <tag>.meta         key=value lines: commit=, start= epoch=, the environment line, command=,
                       and end= epoch= exit= (and vmhwm_kb_polled= on Linux)
    <tag>.log          the timed run's own output, with -print_final_stats=1
    <tag>.time         on macOS, what /usr/bin/time -l wrote
    <tag>.replay.log   the replay of the saved corpus and the seeds, -runs=0
    <tag>.merge.log    their merge, -merge=1, into the folder the timed run starts from
    <tag>.phases.json  each part's command, exit code and clock

The replay's inputs are libFuzzer's count of files in its folders, its runs its Done line, and
the merge's kept inputs its own count of new files; the seconds are the launcher's clock, the
timed run's split at its INITED line, after which its first new input runs.

Every value comes from those files. A run is clean only if the fuzzer exited 0, printed its final
DONE line, its Done line and its final stats, and its log holds no sanitizer report, no libFuzzer
error and no crash artifact. --strip removes a machine's own folder from the commands, which
must be relative to the repository or the project folder in the file. A run whose checkout had
moved past --commit is taken only if nothing check_fuzz.py ties the file to (TIED_PATHS)
changed in between, and keeps that checkout as checkout_at_start. The output is checked with
check_fuzz.py before it is written, and not written if it fails.

Standard library only.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_fuzz  # noqa: E402

FINDINGS = (
    ("leak", re.compile(r"ERROR: LeakSanitizer")),
    ("oom", re.compile(r"ERROR: libFuzzer: (?:out-of-memory|malloc\()")),
    ("timeout", re.compile(r"ERROR: libFuzzer: timeout")),
    ("crash", re.compile(r"ERROR: AddressSanitizer|runtime error:|ERROR: libFuzzer: deadly "
                         r"signal|ERROR: libFuzzer: fuzz target|==ERROR: UndefinedBehavior")),
    ("crash", re.compile(r"^check_\w+: ")),
    ("other", re.compile(r"Test unit written to|ERROR: libFuzzer")),
)
STAT = re.compile(r"^stat::(\w+):\s+(\d+)$")
SEED_FILES = re.compile(r"INFO: seed corpus: files: (\d+) ")
MERGED = re.compile(r"MERGE-OUTER: (\d+) new files with ")
INITED = re.compile(r"^#(\d+)\s+INITED .*exec/s: (\d+) ")
TIME_FIELDS = {"maximum resident set size": "max_rss_bytes",
               "peak memory footprint": "peak_footprint_bytes"}


class Refused(Exception):
    pass


def read_meta(path):
    meta = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("command="):
            meta["command"] = line.split("=", 1)[1]
        elif line.startswith(("ASAN_OPTIONS=", "UBSAN_OPTIONS=")):
            meta["environment"] = line
        else:
            for word in line.split():
                if "=" in word:
                    key, value = word.split("=", 1)
                    meta[key] = value
    for key in ("commit", "epoch", "exit", "command", "environment"):
        if key not in meta:
            raise Refused(f"{path.name}: no {key}")
    return meta


def epochs(path):
    found = re.findall(r"epoch=(\d+)", path.read_text(encoding="utf-8"))
    if len(found) != 2:
        raise Refused(f"{path.name}: needs a start and an end epoch, has {len(found)}")
    return int(found[0]), int(found[1])


def read_log(path):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    status = [line for line in lines if re.match(r"^#\d+\s+DONE ", line)]
    done = [line for line in lines if line.startswith("Done ")]
    stats = {match[1]: int(match[2]) for match in map(STAT.match, lines) if match}
    findings = []
    for line in lines:
        for kind, pattern in FINDINGS:
            if pattern.search(line):
                findings.append({"kind": kind, "detail": line.strip()})
                break
    inited = next((match for match in map(INITED.match, lines) if match), None)
    return {"status": status[-1] if status else "", "done": done[-1] if done else "",
            "stats": stats, "findings": findings, "inited": inited}


def read_time(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].strip() in TIME_FIELDS:
            values[TIME_FIELDS[parts[1].strip()]] = int(parts[0])
        elif line.strip().endswith("sys") and "real" in line:
            values["real_seconds"] = float(line.split()[0])
    return values


def relative(text, prefixes):
    for prefix in prefixes:
        text = text.replace(prefix.rstrip("/") + "/", "")
    return text


def flag_value(command, name):
    for word in command.split():
        if word.startswith(f"-{name}="):
            return word.split("=", 1)[1]
    return None


def run_of(host, folder, tag, facts, prefixes):
    meta = read_meta(folder / f"{tag}.meta")
    start, end = epochs(folder / f"{tag}.meta")
    log = read_log(folder / f"{tag}.log")
    command = relative(meta["command"], prefixes)
    words = command.split()
    target = Path(next(word for word in words if "fuzz_" in Path(word).name)).name
    level = meta.get("TICK_STORE_SIMD") or facts["default_kernel_level"]
    done = re.match(r"^Done (\d+) runs in (\d+) second\(s\)$", log["done"])
    status = check_fuzz.STATUS.match(log["status"])
    stats = log["stats"]
    exit_code = int(meta["exit"])
    findings = log["findings"]
    if exit_code != 0 and not findings:
        findings = [{"kind": "other", "detail": f"the fuzzer exited {exit_code}"}]
    clean = (exit_code == 0 and not findings and done is not None and status is not None
             and "number_of_executed_units" in stats and "peak_rss_mb" in stats)
    environment = " ".join(word for word in meta["environment"].split()
                           if word.split("=", 1)[0] in ("ASAN_OPTIONS", "UBSAN_OPTIONS",
                                                        "TICK_STORE_SIMD"))
    cpu = "unpinned"
    if words[:2] == ["taskset", "-c"]:
        cpu = f"cpu {words[2]}"
    run = {
        "target": target, "host": host, "commit": meta["commit"], "cpu": cpu,
        "cpu_model": facts["cpu_model"], "compiler": facts["compiler"],
        "compile_flags": facts["compile_flags"], "kernel_level": level,
        "seconds": int(done[2]) if done else None, "wall_seconds": end - start,
        "runs": int(done[1]) if done else stats.get("number_of_executed_units"),
        "exec_per_second": stats.get("average_exec_per_sec"),
        "coverage": {"cov": int(status[2]), "ft": int(status[3])} if status else None,
        "corpus": ({"units": int(status[4]),
                    "size": log["status"].split("corp: ", 1)[1].split()[0].split("/", 1)[1]}
                   if status else None),
        "peak_rss_mb": stats.get("peak_rss_mb"),
        "clean": clean, "exit_code": exit_code, "findings": findings,
        "final_status_line": re.sub(r"\s+", " ", log["status"]).strip(),
        "done_line": log["done"], "final_stats": stats,
        "command": f"{environment} {command}",
        "max_len": int(flag_value(command, "max_len") or 0),
        "started": meta.get("start"), "ended": meta.get("end"),
    }
    if log["inited"] is not None:
        runs, rate = int(log["inited"][1]), int(log["inited"][2])
        run["inited"] = {"runs": runs, "approx_seconds": round(runs / rate) if rate else None}
    run.update(phases_of(folder, tag, prefixes))
    failed_in = run.pop("failed_in", None)
    if failed_in and not run["findings"]:
        run["findings"] = [{"kind": "other", "detail": f"the {failed_in} did not pass"}]
    if failed_in:
        run["clean"] = False
    time_path = folder / f"{tag}.time"
    if time_path.exists():
        run["os_peak_memory"] = {"method": "BSD time -l", **read_time(time_path)}
    elif "vmhwm_kb_polled" in meta:
        run["os_peak_memory"] = {"method": "VmHWM read from /proc every 5 s while it ran",
                                 "vmhwm_bytes": int(meta["vmhwm_kb_polled"]) * 1024}
    return run


def phases_of(folder, tag, prefixes):
    """The replay, the merge and the timed run's split, from fuzz_g3.py's files."""
    path = folder / f"{tag}.phases.json"
    if not path.exists():
        raise Refused(f"{path.name}: missing; scripts/fuzz_g3.py writes it beside each run")
    phases = json.loads(path.read_text(encoding="utf-8"))
    record = {}
    if phases.get("failed_in"):
        record["failed_in"] = phases["failed_in"]
    for name in ("replay", "merge"):
        if name not in phases:
            continue
        log_path = folder / f"{tag}.{name}.log"
        text = log_path.read_text(encoding="utf-8", errors="replace")
        log = read_log(log_path)
        part = {"command": relative(phases[name]["command"], prefixes),
                "seconds": phases[name]["seconds"], "exit_code": phases[name]["exit_code"],
                "findings": log["findings"]}
        if name == "replay":
            files = SEED_FILES.search(text)
            done = re.match(r"^Done (\d+) runs in \d+ second\(s\)$", log["done"])
            part.update(inputs=int(files[1]) if files else None,
                        runs=int(done[1]) if done else None, done_line=log["done"])
            part["clean"] = (part["exit_code"] == 0 and not part["findings"]
                             and done is not None and files is not None)
        else:
            kept = MERGED.search(text)
            part.update(inputs_kept=int(kept[1]) if kept else None,
                        attempts=text.count("MERGE-OUTER: attempt "))
            part["clean"] = (part["exit_code"] == 0 and not part["findings"]
                             and part["attempts"] == 1 and kept is not None)
        record[name] = part
    if "timed" in phases:
        timed = phases["timed"]
        inited = timed["seconds_to_inited"]
        record["timed"] = {
            "seconds_before_first_new_input": inited,
            "seconds_after_first_new_input": (None if inited is None
                                              else round(timed["seconds"] - inited, 1)),
            "new_inputs_saved": timed.get("new_inputs_saved")}
    return record


def code_changes(first, second):
    """The files between two commits that a fuzz binary is built from or launched with, or ""
    if none: check_fuzz.py's tie, so the two scripts define it once (review RG-2)."""
    done = subprocess.run(["git", "-C", str(REPO_ROOT), "diff", "--name-only", first, second,
                           "--", *check_fuzz.TIED_PATHS, *check_fuzz.NOT_TIED],
                          capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise Refused(f"git diff {first} {second}: {done.stderr.strip()}")
    return " ".join(done.stdout.split())


def assemble(commit, hosts, prefixes, cmake):
    targets = check_fuzz.targets_in(cmake)
    header_hosts = {}
    runs = []
    for host, folder in hosts:
        facts = json.loads((folder / "host.json").read_text(encoding="utf-8"))
        header_hosts[host] = {key: value for key, value in facts.items()
                              if key != "default_kernel_level"}
        for meta in sorted(folder.glob("*.meta")):
            runs.append(run_of(host, folder, meta.stem, facts, prefixes))
    for run in runs:
        if run["commit"] != commit:
            changed = code_changes(commit, run["commit"])
            if changed:
                raise Refused(f"{run['target']} on {run['host']} ran at {run['commit']}, not "
                              f"{commit}, and these differ: {changed}")
            # The checkout moved on while the binaries built at the commit ran: nothing the tie
            # names changed in between. Both are recorded.
            run["checkout_at_start"] = run["commit"]
            run["commit"] = commit
    order = {"macOS": 0, "Linux": 1}
    runs.sort(key=lambda run: (order[run["host"]], run["target"], run["kernel_level"]))
    header = {
        "schema": check_fuzz.SCHEMA, "gate": "G-3", "commit": commit,
        "statement": ("Each libFuzzer target of fuzz/CMakeLists.txt, under the heavy-job "
                      "lock, with fuzz/launch.txt's flags and caps: first every input of the "
                      "saved corpus and every seed the real encoders write, run once through "
                      "the target's oracle, then merged into the smallest set that keeps their "
                      "coverage, then a timed run of at least "
                      f"{check_fuzz.REQUIRED_SECONDS} s that starts from that set; on Linux, "
                      "each target whose decode reaches the AVX2 kernels or that verifies "
                      "CRC32C, which runs the SSE4.2 kernel there, with the level set to avx2. "
                      "Coverage, corpus and exec/s are the timed run's, libFuzzer's own final "
                      "figures."),
        "required_seconds": check_fuzz.REQUIRED_SECONDS,
        "targets_file": "fuzz/CMakeLists.txt", "targets": targets,
        "avx2_path_targets": list(check_fuzz.AVX2_PATH),
        "crc32c_path_targets": list(check_fuzz.CRC32C_PATH), "hosts": header_hosts,
    }
    document = {"header": header, "runs": runs}
    document["summary"] = check_fuzz.summary_of(header, runs)
    return document


def main(argv=None):
    parser = argparse.ArgumentParser(description="Assemble the G-3 fuzzing results file.")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--host", action="append", required=True, help="HOST=DIR")
    parser.add_argument("--strip", action="append", default=[])
    parser.add_argument("--cmake", type=Path, default=check_fuzz.DEFAULT_CMAKE)
    args = parser.parse_args(argv)
    hosts = []
    for item in args.host:
        name, _, folder = item.partition("=")
        if name not in check_fuzz.HOSTS or not folder:
            parser.error(f"--host {item}: use HOST=DIR with HOST one of {sorted(check_fuzz.HOSTS)}")
        hosts.append((name, Path(folder)))
    try:
        document = assemble(args.commit, hosts, args.strip, args.cmake)
    except (Refused, OSError, KeyError, StopIteration, json.JSONDecodeError) as error:
        print(f"fuzz_results: {error}", file=sys.stderr)
        return 1
    problems = check_fuzz.check_document(document, list(document), args.out,
                                         check_fuzz.targets_in(args.cmake))
    if problems:
        for problem in problems:
            print(f"fuzz_results: {problem}", file=sys.stderr)
        return 1
    args.out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
