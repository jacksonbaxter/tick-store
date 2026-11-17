#!/usr/bin/env python3
"""Tie every results file to the code it was measured at. Exits 1 and names every file whose
measured code changed since its commit.

    python3 -I scripts/check_ties.py [--repo DIR] [--results DIR] [--pins FILE] [--table]

A figure holds for the code it was measured at and no later. Every file under results/ names
the commit it was measured at: each value of a key `commit` or `source_commit`, anywhere in the
file, is one. KINDS below says, for each kind of file, by its name, what was built and run when
its figure was taken, each path with its reason, and what was deliberately left out, with its
reason. For each file and each commit it names, this compares git's tree content:

  - its MEASURED paths, what was built and run when the figure was taken, between that commit
    and the working tree: HEAD and anything not yet committed, new files included, as
    check_fuzz.py ties G-3's file, whose tie this shares (its TIED_PATHS are the fuzz kind's);
  - its ASSEMBLED paths, what turned the run's raw output into the file afterwards, and the
    committed results it copies, between the last commit that wrote the file and the tree:
    an assembler may run after the commit the file names, as scripts/diag_summary.py, which
    did not exist at the freeze commit, does for the diagnostic runs made there;
  - the input pins of scripts/data_pins.json, its `days` and `slices`, which every run on real
    data verifies its input against, member by member, so the exemption pins beside them, which
    change nothing that runs, do not count.

It refuses, exit 1, a file whose measured or assembled paths differ, naming the file, the
commit and the paths; a file that names no commit, or a commit git cannot read; a file of no
kind in KINDS, so a new kind of results file must be given its paths here; and a commit that a
full clone does not hold, made up or rebased away.

A file that an existing pin declares is reported as pinned, with its commit and how many of
its measured paths changed since, and is not judged: each validator's own key in the pins file,
and only the names that validator lets be pinned there, each known by the SHA-256 of its bytes,
never by anything the file says of itself. This script adds no pin and no exemption: once the
validators' pins are removed, it judges those files with no change here.

Where git has no history to compare with, a tree exported with git archive as a sync to the
Linux host makes, it judges no file's code and says "not judged: no history here", and exits
EXIT_UNJUDGED, which scripts/check.sh reports as a skip, a failure under --no-skips. So does a
file whose commit a shallow clone lacks. A full clone that lacks it refuses the file.

Exit 0 if every file's measured code is unchanged or the file is pinned, 1 if any file is
refused, 2 on a usage error, 3 if nothing was refused but some file could not be judged for
want of history.

Standard library only. tests/scripts/test_check_ties.py shows each rule failing on a small git
repository of its own.
"""

import argparse
import fnmatch
import functools
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_compare  # noqa: E402
import check_fuzz  # noqa: E402
import check_roundtrip  # noqa: E402
import prove_checks  # noqa: E402

DEFAULT_RESULTS = REPO_ROOT / "results"
DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"

EXIT_OK = 0
EXIT_REFUSED = 1
EXIT_USAGE = 2
# Nothing was refused, but some file's code could not be compared for want of git history.
EXIT_UNJUDGED = check_fuzz.EXIT_UNJUDGED

COMMIT_KEYS = ("commit", "source_commit")
COMMIT = re.compile(r"[0-9a-f]{7,40}")
NO_HISTORY = "not judged: no history here"


@dataclass(frozen=True)
class Kind:
    """One kind of results file: the files, by pattern relative to results/; the paths that
    were built and run when its figure was taken, compared with the commit it names, and those
    that assembled it, compared with the last commit that wrote it, as (pathspec, reason); the
    data_pins.json members its runs read; what is excluded from the paths, and what is left out
    of them, as (path, reason); and the pin that may declare one, as (key, names)."""
    name: str
    files: tuple
    measured: tuple
    assembled: tuple = ()
    pin_members: tuple = ()
    excluded: tuple = ()
    left_out: tuple = ()
    pin: tuple = None


PINS_FILE = "scripts/data_pins.json"

# What every binary that takes a figure is built from: the bench preset's build, or the coverage
# preset's for coverage.
BUILT = (
    ("src", "the library and the harness every tool and benchmark links; "
            "src/bench_support/build_info.cpp.in is the build facts each file records"),
    ("include", "the library's headers"),
    ("cmake", "the CMake modules: warnings, flags, sanitisers, the build facts, and "
              "Dependencies.cmake, which pins Google Benchmark, zstd, HdrHistogram and "
              "GoogleTest by commit"),
    ("CMakeLists.txt", "the top-level build, which defines the library, harness and "
                       "bench-support targets"),
    ("CMakePresets.json", "the presets, which set each build's type and flags, the bench "
                          "preset's -O3 -DNDEBUG among them"),
)
TOOLS = ("tools", "the data tools a run calls: tick_roundtrip, tick_codec_eval, tick_census, "
                  "tick_export, tick_query, tick_coldcache, and tools/CMakeLists.txt; the "
                  "whole folder, since one CMakeLists.txt builds them all")
BENCH = ("bench", "the benchmark binaries and bench/CMakeLists.txt, which reads the slice's "
                  "pin at configure time")
RUN_BENCH = ("scripts/run_bench.py", "runs each benchmark case under the wrapper, reads the "
                                     "commit, compiler, load, boost state and the sibling's "
                                     "busy share, and writes the file; compare/run.py and "
                                     "diag_summary.py import its readings")
BUILD_PRESET = ("scripts/build_preset.py", "names the preset that configured the build folder "
                                           "the tools ran from")
STORE_RESULTS = ("scripts/store_results.py", "assembles the round-trip and store-size files "
                                             "from the runs' raw output and exit statuses")
CAPACITY = ("results/fullday-capacity-12302019.json", "a committed result this file copies "
                                                      "figures from: CR-2's stored bytes")
# The input pins a run on real data verifies its input against, read by bench/CMakeLists.txt,
# run_bench.py, compare/layout.py and fetch_data.py. The pins file's other keys exempt
# committed files from validator rules, and change nothing that runs.
INPUT_PINS = ("days", "slices")

VALIDATORS = ("scripts/check_results.py, check_compare.py, check_roundtrip.py, check_fuzz.py, "
              "check_oracle.py", "they judge the file as committed, at HEAD, on every run of "
              "check.sh, and make no figure; a run's own call of one only refuses to write")
RENDER = ("scripts/render_results.py, scripts/targets.json, BENCHMARKS.md", "turn the figures "
          "into the pages and verdicts, at HEAD, under render-check; they measure nothing")
PROSE = ("*.md, docs/, faults/, tests/scripts/, .github/", "prose, fault patches, the scripts' "
         "own tests and CI: nothing a measurement builds or runs")
TESTS_UNIT = ("tests/unit, tests/cmake", "the unit tests' own binary and a CMake script test: "
              "built beside the tools in a build folder, but nothing in them is linked into a "
              "tool or benchmark, and tests/unit/CMakeLists.txt sets nothing on the library's "
              "targets")
OTHER_BUILDS = ("fuzz", "built only by the fuzz preset; no tool or benchmark links it")
FETCH = ("scripts/fetch_data.py", "fetches a day and verifies it against its pin, which the input "
                                  "pins above hold; each file records its input's SHA-256")
PROSE_IN_PATHS = (("*.md", "prose inside the paths, such as compare/README.md: nothing builds "
                           "or runs it"),)

KINDS = (
    Kind("bench", ("bench/*.json",),
         measured=(*BUILT, BENCH, RUN_BENCH),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(TOOLS[:1] + ("built by the same preset, but no benchmark runs or links a "
                                "tool",), VALIDATORS, RENDER, PROSE, TESTS_UNIT, OTHER_BUILDS,
                   FETCH)),
    Kind("compare", ("compare/*.json",),
         measured=(*BUILT, TOOLS,
                   ("compare", "the whole harness: its runner, workers, guard, systems, "
                               "queries, settings, the Java commit-mode probe, and "
                               "requirements.lock, which pins every comparison system's wheel "
                               "by hash"),
                   RUN_BENCH, STORE_RESULTS, BUILD_PRESET),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(("results/fullday-capacity-12302019.json", "read by check_compare.py, the "
                    "validator, not by the harness"), BENCH[:1] + ("built by the same preset, "
                    "never run by the harness",), VALIDATORS, RENDER, PROSE, TESTS_UNIT,
                   OTHER_BUILDS, FETCH),
         pin=(check_compare.PROTOCOL_1_FILES, None)),
    Kind("capacity", ("fullday-capacity-*.json",),
         measured=(*BUILT, TOOLS,
                   ("compare", "compare/fullday.py ran each system through the whole day, on "
                               "the harness's workers, systems and wheels")),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(VALIDATORS, RENDER, PROSE, TESTS_UNIT, OTHER_BUILDS, FETCH)),
    Kind("census", ("census-*.json",),
         measured=(*BUILT, TOOLS),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(("its host, build_preset, commit and memory fields", "added beside "
                    "tick_census's own output by hand, at 0e9c9de: no tracked script writes "
                    "them"), VALIDATORS, PROSE, TESTS_UNIT, OTHER_BUILDS, FETCH)),
    Kind("codec-eval", ("codec-eval-*.json",),
         measured=(*BUILT, TOOLS),
         assembled=(("scripts/codec_eval_results.py", "writes the file from tick_codec_eval's "
                                                     "output, its time file and the build "
                                                     "folder"),
                    BUILD_PRESET, ("scripts/store_results.py", "codec_eval_results.py imports "
                                                               "its readers")),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(VALIDATORS, RENDER, PROSE, TESTS_UNIT, OTHER_BUILDS, FETCH)),
    Kind("roundtrip", ("roundtrip-*.json",),
         measured=(*BUILT, TOOLS),
         assembled=(STORE_RESULTS, BUILD_PRESET),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(("scripts/g2_plan.py", "chooses which bytes G-2 flips; the flips and their "
                    "seed are recorded in the round-trip file itself, and "
                    "tests/scripts/test_g2_plan.py reproduces both days' plans, so it is not a "
                    "measured path: 7a352ad and 1d234ee changed it after the freeze, and the "
                    "second day's rerun at the freeze commit used the changed planner"),
                   VALIDATORS, RENDER, PROSE, TESTS_UNIT, OTHER_BUILDS, FETCH),
         pin=(check_roundtrip.PINS_KEY, check_roundtrip.PINNED_NAMES)),
    Kind("store-size", ("store-size-*.json",),
         measured=(*BUILT, TOOLS),
         assembled=(STORE_RESULTS, BUILD_PRESET, CAPACITY),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(("scripts/g2_plan.py", "G-2's planner, which the store-size figures do not "
                    "read"), VALIDATORS, RENDER, PROSE, TESTS_UNIT, OTHER_BUILDS, FETCH)),
    # Its commits are the codec evaluation files' it costs, so it measures what they do.
    Kind("store-estimate", ("store-estimate.json",),
         measured=(*BUILT, TOOLS),
         assembled=(("scripts/store_estimate.py", "computes the estimate from the codec "
                                                  "evaluation files"),
                    ("results/codec-eval-*.json", "the committed results it costs, so a "
                                                  "re-measured evaluation makes it stale"),
                    CAPACITY[:1] + ("the stored bytes it judges the estimate against",)),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(VALIDATORS, RENDER, PROSE, TESTS_UNIT, OTHER_BUILDS, FETCH)),
    Kind("coverage", ("coverage.json",),
         measured=(*BUILT,
                   ("tests/unit", "the unit tests whose run is measured, and their "
                                  "CMakeLists.txt"),
                   ("fuzz", "the fuzz oracles' headers, which the unit tests include and the "
                            "gate requires, and the regression inputs they replay"),
                   ("scripts/coverage.sh", "builds the coverage preset, runs the tests, merges "
                                           "and exports the profile"),
                   ("scripts/coverage_report.py", "writes the summary and holds the gate's "
                                                  "required list")),
         excluded=PROSE_IN_PATHS,
         left_out=(("scripts/check.sh", "its coverage step calls coverage.sh into build/; the "
                    "published file comes from coverage.sh --output itself"), VALIDATORS,
                   RENDER, PROSE)),
    Kind("diag", ("diag/*.json",),
         measured=(*BUILT, TOOLS, BENCH, RUN_BENCH),
         assembled=(("scripts/diag_summary.py", "turns the run's raw perf reports into the "
                                                "file and holds the groups its shares are "
                                                "summed by; written after the freeze, so it is "
                                                "compared from the commit that wrote the file"),),
         pin_members=INPUT_PINS,
         excluded=PROSE_IN_PATHS,
         left_out=(("the freeze runner's diagnostic steps", "an untracked script on the Linux "
                    "host; facts.txt in each run's folder records its commands"), VALIDATORS,
                   RENDER, PROSE, TESTS_UNIT, OTHER_BUILDS)),
    # check_fuzz.py's own tie, shared, so the two cannot drift.
    Kind("fuzz", ("fuzz-g3-*.json",),
         measured=tuple((path, "what a fuzz binary is built from or launched with: "
                               "check_fuzz.py's TIED_PATHS") for path in check_fuzz.TIED_PATHS),
         assembled=(("scripts/fuzz_results.py", "assembles the file from the runs' logs"),
                    ("scripts/fuzz_host.py", "writes each host's facts, which the header "
                                             "holds, on that host after its launch")),
         excluded=tuple((spec.split(")", 1)[1], "check_fuzz.py's NOT_TIED: fuzz/'s prose")
                        for spec in check_fuzz.NOT_TIED),
         left_out=(("scripts/check.sh", "check_fuzz.py's reason: its fuzz steps read "
                    "fuzz/launch.txt"), VALIDATORS, RENDER, PROSE),
         pin=(check_fuzz.PRE_TIE_PINS, (check_fuzz.PRE_TIE_FILE,))),
    # The record of what else ran on the host during a campaign's timed steps: made by hand
    # from the host's logs and the project's session records, so no code makes its entries.
    Kind("disturbances", ("disturbances-*.json",),
         measured=(),
         left_out=(("the host's window log, its backup job's log and this project's session "
                    "records", "the list is made by hand from them after the campaign; nothing "
                    "in the tree writes or measures it, and render_results.py checks its shape "
                    "and that every document it places an entry in was measured at its commit"),
                   RENDER, PROSE)),
    # prove_checks.py keys its proofs by the last commit that changed anything outside
    # NOT_CODE, and each fault's fingerprint holds what of NOT_CODE made its verdict.
    Kind("proofs", ("check-proofs.json",),
         measured=((".", "every check and what it checks: prove_checks.py's own key, the last "
                         "commit that changed anything outside its NOT_CODE"),),
         excluded=tuple((spec.split(")", 1)[1], "prove_checks.py's NOT_CODE: each fault's "
                         "fingerprint holds what of it made its verdict")
                        for spec in prove_checks.NOT_CODE)),
)


def kind_of(name):
    """The kind of a file by its path relative to results/, or None."""
    for kind in KINDS:
        if any(fnmatch.fnmatchcase(name, pattern) for pattern in kind.files):
            return kind
    return None


def commits_named(document, where=""):
    """Every (where, value) of a key COMMIT_KEYS names, anywhere in the document."""
    found = []
    if isinstance(document, dict):
        for key, value in document.items():
            here = f"{where}.{key}" if where else key
            if key in COMMIT_KEYS and not isinstance(value, (dict, list)):
                found.append((here, value))
            else:
                found += commits_named(value, here)
    elif isinstance(document, list):
        for index, value in enumerate(document):
            found += commits_named(value, f"{where}[{index}]")
    return found


def git(repo, *arguments):
    return check_fuzz.git(repo, *arguments)


def is_checkout(repo):
    top = git(repo, "rev-parse", "--show-toplevel")
    return top.returncode == 0 and Path(top.stdout.strip()).resolve() == Path(repo).resolve()


@functools.cache
def resolve(repo, commit):
    """The full hash of a commit this checkout holds, or None."""
    done = git(repo, "rev-parse", "--verify", "--quiet", f"{commit}^{{commit}}")
    return done.stdout.strip() if done.returncode == 0 else None


def pathspecs(paths, excluded):
    return (*(path for path, _ in paths), *(f":(exclude){path}" for path, _ in excluded))


@functools.cache
def changed_paths(repo, base, specs):
    """Every file under specs that differs between base and the tree, committed or not, or is
    new: check_fuzz.py's tied_changes, for any paths."""
    if not specs:
        return []
    changed = git(repo, "diff", "--name-only", base, "--", *specs)
    new = git(repo, "ls-files", "--others", "--exclude-standard", "--", *specs)
    for done in (changed, new):
        if done.returncode != 0:
            raise OSError(f"git: {done.stderr.strip()}")
    return sorted(set(changed.stdout.split()) | set(new.stdout.split()))


def changed_members(repo, base, keys):
    """The input pins, by member, that differ between base and the tree."""
    if not keys:
        return []
    shown = git(repo, "show", f"{base}:{PINS_FILE}")
    try:
        old = json.loads(shown.stdout) if shown.returncode == 0 else {}
    except json.JSONDecodeError:
        old = {}
    try:
        new = json.loads((Path(repo) / PINS_FILE).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        new = {}
    return [f"{PINS_FILE} ({key})" for key in keys
            if not isinstance(old, dict) or not isinstance(new, dict)
            or old.get(key) != new.get(key) or key not in new]


def measured_changes(repo, commit, kind):
    return (changed_paths(repo, commit, pathspecs(kind.measured, kind.excluded))
            + changed_members(repo, commit, kind.pin_members))


def written_at(repo, path):
    """The last commit that wrote the file as it is now, or None if it is not committed as it
    is: a file being written now was assembled from the tree as it is."""
    try:
        name = Path(path).resolve().relative_to(Path(repo).resolve()).as_posix()
    except ValueError:
        return None
    if git(repo, "status", "--porcelain", "--", name).stdout.strip():
        return None
    done = git(repo, "log", "-1", "--format=%H", "--", name)
    return done.stdout.strip() or None


def assembled_changes(repo, path, kind):
    """(base, changed): what assembled the file that differs since the commit that wrote it."""
    if not kind.assembled:
        return None, []
    base = written_at(repo, path)
    if base is None:
        return None, []
    return base, changed_paths(repo, base, pathspecs(kind.assembled, ()))


def pinned(kind, path, data, pins):
    """The pins key that declares this file, by the SHA-256 of its bytes, or None."""
    if kind.pin is None:
        return None
    key, names = kind.pin
    entries = pins.get(key) or {}
    name = Path(path).name
    if names is not None and name not in names:
        return None
    if entries.get(name) == hashlib.sha256(data).hexdigest():
        return key
    return None


def shown(paths):
    return ", ".join(paths[:8]) + (f" and {len(paths) - 8} more" if len(paths) > 8 else "")


@dataclass
class Verdict:
    outcome: str
    lines: list


def judge(repo, path, name, pins, history):
    """One file's verdict: same, pinned, differs, no commit, no kind, refused or unjudged."""
    kind = kind_of(name)
    if kind is None:
        return Verdict("no kind", ["no kind in check_ties.py's KINDS matches it, so nothing "
                                   "says what its figures measure: give its kind its paths"])
    try:
        data = Path(path).read_bytes()
        document = json.loads(data.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return Verdict("refused", [f"cannot read it: {error}"])
    named = commits_named(document)
    if not named:
        return Verdict("no commit", [f"names no commit it was measured at, so nothing ties "
                                     f"its figures to code ({kind.name})"])
    unreadable = [f"{where} is {value!r}, not a commit" for where, value in named
                  if not isinstance(value, str) or not COMMIT.fullmatch(value)]
    if unreadable:
        return Verdict("refused", unreadable)
    commits = sorted({value for _, value in named})
    key = pinned(kind, path, data, pins)
    if key is not None:
        counts = []
        for commit in commits:
            full = resolve(repo, commit) if history else None
            counted = (f"; {len(measured_changes(repo, full, kind))} measured paths changed "
                       "since" if full else "")
            counts.append(f"{commit[:12]}{counted}")
        return Verdict("pinned", [f"pinned under {key} in the pins file: measured at "
                                  f"{', '.join(counts)}; not judged ({kind.name})"])
    if not history:
        return Verdict("unjudged", [f"{NO_HISTORY}: measured at "
                                    f"{', '.join(commit[:12] for commit in commits)}, and "
                                    f"{Path(repo).name} is not a git checkout, as an exported "
                                    f"tree is not ({kind.name})"])
    problems, unjudged, same = [], [], []
    for commit in commits:
        full = resolve(repo, commit)
        if full is None:
            missing = check_fuzz.missing_from_full_history(repo, commit)
            if missing:
                problems.append(missing)
            else:
                unjudged.append(f"not judged: {check_fuzz.history_problem(repo, commit)}")
            continue
        changed = measured_changes(repo, full, kind)
        if changed:
            problems.append(f"measured at {commit[:12]}, and what it measures changed since: "
                            f"{shown(changed)}. Measure it again at this code")
        else:
            same.append(commit[:12])
    base, changed = assembled_changes(repo, path, kind)
    if changed:
        problems.append(f"written at {base[:12]}, and what assembled it changed since: "
                        f"{shown(changed)}. Assemble it again")
    if problems:
        return Verdict("differs", [f"{problem} ({kind.name})" for problem in problems])
    if unjudged:
        return Verdict("unjudged", [f"{line} ({kind.name})" for line in unjudged])
    return Verdict("same", [f"ok, nothing it measures changed since {', '.join(same)} "
                            f"({kind.name})"])


def table():
    lines = []
    for kind in KINDS:
        lines.append(f"{kind.name}: results/{', results/'.join(kind.files)}")
        for path, why in kind.measured:
            lines.append(f"  measured   {path}: {why}")
        if kind.pin_members:
            lines.append(f"  measured   {PINS_FILE} {', '.join(kind.pin_members)}: the input "
                         "pins its runs verify their input against")
        for path, why in kind.assembled:
            lines.append(f"  assembled  {path}: {why}")
        for path, why in kind.excluded:
            lines.append(f"  excluded   {path}: {why}")
        for path, why in kind.left_out:
            lines.append(f"  left out   {path}: {why}")
        if kind.pin:
            lines.append(f"  pin        {kind.pin[0]}")
    return "\n".join(lines)


ERRORS = ("differs", "no commit", "no kind", "refused")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Tie every results file to the code it was "
                                                 "measured at.")
    parser.add_argument("--repo", type=Path, default=REPO_ROOT,
                        help="the checkout whose code the files must still be the code of")
    parser.add_argument("--results", type=Path,
                        help="the results folder, by default the repo's results/")
    parser.add_argument("--pins", type=Path, help="the pins file, by default the repo's")
    parser.add_argument("--table", action="store_true",
                        help="print the kinds and their paths, with reasons, and exit")
    args = parser.parse_args(argv)
    if args.table:
        print(table())
        return EXIT_OK
    results = args.results or args.repo / "results"
    pins_path = args.pins or args.repo / PINS_FILE
    try:
        pins = json.loads(pins_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"check_ties: cannot read the pins: {error}", file=sys.stderr)
        return EXIT_USAGE
    files = sorted(results.rglob("*.json"))
    if not files:
        print(f"check_ties: no results files in {results.name}", file=sys.stderr)
        return EXIT_USAGE
    # Each run reads the tree afresh: a caller may change it between runs.
    resolve.cache_clear()
    changed_paths.cache_clear()
    history = is_checkout(args.repo)
    counts = {}
    for path in files:
        name = path.relative_to(results).as_posix()
        try:
            verdict = judge(args.repo, path, name, pins, history)
        except OSError as error:
            verdict = Verdict("refused", [f"cannot compare: {error}"])
        counts[verdict.outcome] = counts.get(verdict.outcome, 0) + 1
        stream = sys.stdout if verdict.outcome in ("same", "pinned") else sys.stderr
        for line in verdict.lines:
            print(f"{name}: {line}", file=stream)
    order = ("same", "pinned", "differs", "no commit", "no kind", "refused", "unjudged")
    print(f"check_ties: {len(files)} files: " + ", ".join(
        f"{counts.get(outcome, 0)} {outcome}" for outcome in order))
    if any(counts.get(outcome) for outcome in ERRORS):
        return EXIT_REFUSED
    if counts.get("unjudged"):
        if not history:
            print(f"check_ties: {NO_HISTORY}: {args.repo.resolve().name} is not a git "
                  "checkout, so no file's code was compared", file=sys.stderr)
        return EXIT_UNJUDGED
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
