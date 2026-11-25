#!/usr/bin/env python3
"""Tests for scripts/gate_results.py. Run with:

    python3 -I tests/scripts/test_gate_results.py

Each test writes the raw records of a small gate on both hosts, two named seeds run twice each,
in the layout the runs' own runner leaves them, under the repo's gitignored build/ folder, and
assembles the file from them: the figures are the records', the time an execution is the gate's,
a run passes as the gate passed it, and records that disagree with each other, or with the
tree's list and bar, make no file. tests/scripts/test_check_gate.py uses these records too.
"""

import json
import shutil
import statistics
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRATCH_ROOT = REPO_ROOT / "build" / "test_gate_results"
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_results  # noqa: E402
import gate_results  # noqa: E402

COMMIT = "8616263b7561bdb39c642c90fde45985fcaf81fc"
SEEDS = (("fuzz_run_length", "run_length/bound-small-blocks"),
         ("fuzz_gorilla_xor", "gorilla_xor/bound-costliest"))
LAUNCH = "flags -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1\n"
PER_SEED = 2
# (file_ms, units, load, processes started) of each run, by seed and run, on each host.
FIGURES = {
    "macOS": {(1, 1): (1225, 2, 4.16, None), (1, 2): (1210, 2, 4.10, None),
              (2, 1): (377, 1, 3.99, None), (2, 2): (370, 1, 3.99, None)},
    "Linux": {(1, 1): (9337, 2, 2.40, 258), (1, 2): (8192, 2, 2.34, 130),
              (2, 1): (3160, 1, 1.20, 53), (2, 2): (2955, 1, 1.18, 55)},
}
STEP = {"macOS": ("M19-bound-seeds", "mac"), "Linux": ("L20-bound-seeds", "spin")}
HOME = {"macOS": "/Users/someone/run", "Linux": "/home/someone/run"}
COLUMNS = "\t".join(gate_results.TABLE_COLUMNS)


def seeds_text(seeds=SEEDS):
    return "".join(f"{target} {seed}\n" for target, seed in seeds)


def stat(jiffies, processes):
    """A copy of the kernel's counters: eight cpus, each its busy and idle jiffies."""
    lines = ["cpu  1 0 1 1 0 0 0 0 0 0"]
    for cpu in range(8):
        busy, idle = jiffies[cpu]
        lines.append(f"cpu{cpu} {busy} 0 0 {idle} 0 0 0 0 0 0")
    lines += ["ctxt 1", f"processes {processes}", "procs_running 1"]
    return "\n".join(lines) + "\n"


def write_host(root, host, seeds_file, figures=None, edit=None):
    """One host's run folder as the runner leaves it. `edit(name, text)` may change any file
    before it is written, to make the records disagree."""
    figures = figures or FIGURES[host]
    step, state = STEP[host]
    folder = root / host
    raw = folder / "raw" / step
    raw.mkdir(parents=True)
    digest = gate_results.hashlib.sha256(Path(seeds_file).read_bytes()).hexdigest()
    files = {}
    status = [f"{host.lower()}-runner mode=real commit={COMMIT} start=2026-11-17T14:00:00Z",
              f"== {step} start 2026-11-17T15:00:00Z",
              f"   [{step}] {HOME[host]}/src/fuzz/bound_seeds.txt sha256 {digest}, "
              f"{len(SEEDS)} seeds, each run {PER_SEED} times; bar 5000 ms an execution (half of "
              "launch.txt's -timeout=10), the worst run judged"]
    table = [COLUMNS]
    listed = []
    second = 0
    for index, (target, seed) in enumerate(SEEDS, 1):
        listed.append(f"{target}\t{HOME[host]}/build/fuzz/fuzz-seeds/{seed}\t16384")
        for run in range(1, PER_SEED + 1):
            file_ms, units, load, started = figures[(index, run)]
            stem = f"seed-{index}-{state}-{run}"
            start = f"2026-11-17T15:{10 + second // 60:02d}:{second % 60:02d}Z"
            second += 9
            end = f"2026-11-17T15:{10 + second // 60:02d}:{second % 60:02d}Z"
            second += 1
            wall = file_ms + 200
            status += [f"   [{step}] {stem} start {start}",
                       f"     command: heavy.sh {HOME[host]}/build/fuzz/fuzz/{target} -runs=1",
                       f"   [{step}] {stem} end {end} exit 0"]
            files[f"{stem}.log"] = (
                "INFO: Seed: 1\n"
                f"Running: {HOME[host]}/build/fuzz/fuzz-seeds/{seed}\n"
                f"Executed {HOME[host]}/build/fuzz/fuzz-seeds/{seed} in {file_ms} ms\n"
                f"stat::number_of_executed_units: {units}\nstat::peak_rss_mb:              378\n")
            tag = target + ("-avx2" if host == "Linux" else "")
            if host == "Linux":
                before = [(100, 1000)] * 8
                after = [(101, 1099), (101, 1099), (102, 1098), (101, 1099), (101, 1099),
                         (101, 1099), (199, 1001), (200, 1000)]
                files[f"{stem}.stat0"] = stat(before, 1000)
                files[f"{stem}.stat1"] = stat(after, 1000 + started)
                files[f"{stem}.wall"] = f"{wall}\n"
                files[f"{stem}.beside"] = gate_results.BUSY_BOTH_ENDS + "\n"
                shares = "cpu7 100.0% busy, cpu2 2.0%; "
                beside, sibling, third = gate_results.BUSY_BOTH_ENDS, "100.0", "2.0"
            else:
                shares, beside, sibling, third = "", "-", "-", "-"
            status.append(f"   [{step}] seed {index} {target} {seed} run {run} of {PER_SEED}: "
                          f"libFuzzer {file_ms} ms, units {units}, wall {wall} ms, exit 0, ok; "
                          f"{shares}load {load:.2f}" + (f"; {beside}" if host == "Linux" else ""))
            table.append("\t".join([tag, seed, state, str(run), str(file_ms), str(wall), "0",
                                    sibling, f"{load:.2f}", "10", "ok", str(units), beside, third,
                                    f"raw/{step}/{stem}.log"]))
    verdicts = ["target\tseed\truns\tunits\tworst_ms_per_execution\tmedian_ms_per_execution\t"
                "limit_ms\tverdict"]
    for index, (target, seed) in enumerate(SEEDS, 1):
        times = [figures[(index, run)][0] / figures[(index, run)][1]
                 for run in range(1, PER_SEED + 1)]
        units = figures[(index, 1)][1]
        verdicts.append(f"{target}\t{seed}\t{PER_SEED}\t{units}\t{max(times):.1f}\t"
                        f"{statistics.median(times):.1f}\t5000\tok")
    status.append(f"== {step} end 2026-11-17T15:20:00Z exit 0 ok")
    files["list.txt"] = "\n".join(listed) + "\n"
    files["seedtime.tsv"] = "\n".join(table) + "\n"
    files["gate.tsv"] = "\n".join(verdicts) + "\n"
    files["../../status.txt"] = "\n".join(status) + "\n"
    for name, text in files.items():
        if edit is not None:
            text = edit(name, text)
        (raw / name).write_text(text, encoding="utf-8")
    return folder


class Records:
    """A gate's records on both hosts, with the tree's list and launch file."""

    def __init__(self, root, edit=None, host_edited="Linux", figures=None, seeds=SEEDS):
        self.root = root
        self.seeds = root / "bound_seeds.txt"
        self.seeds.write_text(seeds_text(seeds), encoding="utf-8")
        self.launch = root / "launch.txt"
        self.launch.write_text(LAUNCH, encoding="utf-8")
        self.hosts = []
        for host in ("macOS", "Linux"):
            self.hosts.append((host, write_host(
                root, host, self.seeds, figures if host == host_edited else None,
                edit if host == host_edited else None)))

    def assemble(self, commit=COMMIT):
        return gate_results.assemble(commit, "2026-11-25", self.hosts, self.seeds, self.launch)


def assembled(root):
    """The document the small gate's records make."""
    return Records(root).assemble()


class Scratch(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.dir)


class AssembleTest(Scratch):
    def test_the_records_make_the_file(self):
        document = assembled(self.dir)
        self.assertEqual(document["header"]["schema"], gate_results.SCHEMA)
        self.assertEqual(document["header"]["bar_ms_per_execution"], 5000)
        self.assertEqual(document["header"]["runs_per_seed"], PER_SEED)
        self.assertEqual(len(document["runs"]), 2 * len(SEEDS) * PER_SEED)
        worst = document["summary"]["Linux"]["worst"]
        self.assertEqual((worst["seed"], worst["runs"], worst["ms_per_execution"],
                          worst["file_ms"], worst["seed_median_ms_per_execution"]),
                         ("run_length/bound-small-blocks", [1], 4668.5, 9337, 4382.25))
        self.assertTrue(document["summary"]["macOS"]["all_pass"])

    def test_each_run_is_timed_as_the_gate_timed_it(self):
        runs = {(run["host"], run["seed"], run["run"]): run for run in assembled(self.dir)["runs"]}
        one = runs[("Linux", "run_length/bound-small-blocks", 1)]
        self.assertEqual((one["file_ms"], one["executed_units"], one["ms_per_execution"]),
                         (9337, 2, 4668.5))
        gorilla = runs[("macOS", "gorilla_xor/bound-costliest", 2)]
        self.assertEqual(gorilla["ms_per_execution"], 370.0)
        self.assertEqual((one["start"], one["end"]), ("2026-11-17T15:10:00Z",
                                                      "2026-11-17T15:10:09Z"))

    def test_linux_runs_keep_their_counters_and_the_mac_runs_none(self):
        runs = assembled(self.dir)["runs"]
        linux = next(run for run in runs if run["host"] == "Linux")
        self.assertEqual(linux["cpu_busy_percent"]["cpu7"], 100.0)
        self.assertEqual(linux["cpu_busy_percent"]["cpu2"], 2.0)
        self.assertEqual(linux["processes_started"], 258)
        self.assertTrue(linux["sibling_busy_at_start"] and linux["sibling_busy_at_end"])
        mac = next(run for run in runs if run["host"] == "macOS")
        self.assertNotIn("cpu_busy_percent", mac)

    def test_ties_for_the_worst_are_all_named(self):
        figures = dict(FIGURES["macOS"])
        figures[(1, 2)] = figures[(1, 1)]
        document = Records(self.dir, figures=figures, host_edited="macOS").assemble()
        self.assertEqual(document["summary"]["macOS"]["worst"]["runs"], [1, 2])

    def test_the_file_names_no_machine_and_says_how_it_was_made(self):
        document = assembled(self.dir)
        self.assertEqual(check_results.check_leaks(document), [])
        self.assertNotIn("someone", json.dumps(document))
        said = document["header"]["assembled"]
        for words in ("Assembled on 2026-11-25", "a script written after the runs",
                      "the runs of 2026-11-17"):
            self.assertIn(words, said)

    def test_a_run_over_the_bar_does_not_pass(self):
        figures = dict(FIGURES["Linux"])
        figures[(1, 2)] = (10002, 2, 2.34, 130)

        def over(name, text):
            return text.replace("5001.0\t4834.8\t5000\tok", "5001.0\t4834.8\t5000\tover") \
                if name == "gate.tsv" else text
        document = Records(self.dir, edit=over, figures=figures).assemble()
        run = next(run for run in document["runs"] if run["host"] == "Linux" and
                   run["seed"] == "run_length/bound-small-blocks" and run["run"] == 2)
        self.assertFalse(run["pass"])
        self.assertFalse(document["summary"]["Linux"]["all_pass"])


class RefusedTest(Scratch):
    def assert_refused(self, records, words, commit=COMMIT):
        with self.assertRaises(gate_results.Refused) as caught:
            records.assemble(commit)
        self.assertIn(words, str(caught.exception))

    def test_another_commit_is_refused(self):
        self.assert_refused(Records(self.dir), "the run's commit is", "f" * 40)

    def test_a_list_not_the_trees_is_refused(self):
        records = Records(self.dir)
        records.seeds.write_text(seeds_text() + "\n", encoding="utf-8")
        self.assert_refused(records, "not the tree's fuzz/bound_seeds.txt")

    def test_seeds_in_another_order_are_refused(self):
        records = Records(self.dir)
        records.seeds.write_text(seeds_text(SEEDS[::-1]), encoding="utf-8")
        self.assert_refused(records, "not fuzz/bound_seeds.txt's seeds in order")

    def test_a_bar_not_half_the_timeout_is_refused(self):
        records = Records(self.dir)
        records.launch.write_text(LAUNCH.replace("-timeout=10", "-timeout=12"), encoding="utf-8")
        self.assert_refused(records, "is not half of fuzz/launch.txt's -timeout=12")

    def test_a_runners_table_that_disagrees_with_a_log_is_refused(self):
        def edit(name, text):
            return text.replace("\t9337\t", "\t9300\t") if name == "seedtime.tsv" else text
        self.assert_refused(Records(self.dir, edit=edit), "the runner's table gives libfuzzer_ms")

    def test_a_step_log_that_disagrees_with_a_log_is_refused(self):
        def edit(name, text):
            return text.replace("in 9337 ms", "in 9300 ms") if name.endswith(".log") else text
        self.assert_refused(Records(self.dir, edit=edit), "not its libFuzzer output's")

    def test_counters_that_disagree_with_the_step_log_are_refused(self):
        def edit(name, text):
            return text.replace("cpu7 200 0 0 1000", "cpu7 150 0 0 1050") \
                if name == "seed-1-spin-1.stat1" else text
        self.assert_refused(Records(self.dir, edit=edit), "the step log's shares")

    def test_a_gate_verdict_its_runs_do_not_give_is_refused(self):
        def edit(name, text):
            return text.replace("4668.5\t4382.2\t", "4600.0\t4382.2\t") \
                if name == "gate.tsv" else text
        self.assert_refused(Records(self.dir, edit=edit), "the gate's verdict of")

    def test_a_missing_run_is_refused(self):
        def edit(name, text):
            return "\n".join(line for line in text.splitlines()
                             if "seed-2-spin-2 end" not in line) + "\n" \
                if name == "../../status.txt" else text
        self.assert_refused(Records(self.dir, edit=edit), "gorilla_xor/bound-costliest run 2")


if __name__ == "__main__":
    unittest.main()
