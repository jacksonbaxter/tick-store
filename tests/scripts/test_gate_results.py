#!/usr/bin/env python3
"""Tests for scripts/gate_results.py. Run with:

    python3 -I tests/scripts/test_gate_results.py

Each test writes the raw records of a small gate on both hosts, two named seeds run five times each,
in the layout the runs' own runner leaves them, under the repo's gitignored build/ folder, and
assembles the file from them: the figures are the records', the time an execution is the gate's,
a run passes as the gate passed it, and records that disagree with each other, or with the
tree's list and bar, make no file. tests/scripts/test_check_gate.py uses these records too.
"""

import contextlib
import io
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
PER_SEED = 5
# (file_ms, units, load, processes started) of each run, by seed and run, on each host.
FIGURES = {
    "macOS": {(1, 1): (1225, 2, 4.16, None), (1, 2): (1210, 2, 4.10, None),
              (1, 3): (1220, 2, 4.10, None), (1, 4): (1215, 2, 4.05, None),
              (1, 5): (1212, 2, 4.05, None),
              (2, 1): (377, 1, 3.99, None), (2, 2): (370, 1, 3.99, None),
              (2, 3): (372, 1, 3.98, None), (2, 4): (373, 1, 3.98, None),
              (2, 5): (371, 1, 3.97, None)},
    "Linux": {(1, 1): (9337, 2, 2.40, 258), (1, 2): (8192, 2, 2.34, 130),
              (1, 3): (7816, 2, 2.29, 53), (1, 4): (7816, 2, 2.24, 53),
              (1, 5): (7801, 2, 2.20, 60),
              (2, 1): (3160, 1, 1.20, 53), (2, 2): (2955, 1, 1.18, 55),
              (2, 3): (2950, 1, 1.10, 53), (2, 4): (2952, 1, 1.10, 53),
              (2, 5): (2960, 1, 1.10, 53)},
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


def write_host(root, host, seeds_file, figures=None, edit=None, per_seed=PER_SEED):
    """One host's run folder as the runner leaves it, each seed run per_seed times.
    `edit(name, text)` may change any file before it is written, to make the records
    disagree."""
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
              f"{len(SEEDS)} seeds, each run {per_seed} times; bar 5000 ms an execution (half of "
              "launch.txt's -timeout=10), the worst run judged"]
    table = [COLUMNS]
    listed = []
    second = 0
    for index, (target, seed) in enumerate(SEEDS, 1):
        listed.append(f"{target}\t{HOME[host]}/build/fuzz/fuzz-seeds/{seed}\t16384")
        for run in range(1, per_seed + 1):
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
            status.append(f"   [{step}] seed {index} {target} {seed} run {run} of {per_seed}: "
                          f"libFuzzer {file_ms} ms, units {units}, wall {wall} ms, exit 0, ok; "
                          f"{shares}load {load:.2f}" + (f"; {beside}" if host == "Linux" else ""))
            table.append("\t".join([tag, seed, state, str(run), str(file_ms), str(wall), "0",
                                    sibling, f"{load:.2f}", "10", "ok", str(units), beside, third,
                                    f"raw/{step}/{stem}.log"]))
    verdicts = ["target\tseed\truns\tunits\tworst_ms_per_execution\tmedian_ms_per_execution\t"
                "limit_ms\tverdict"]
    for index, (target, seed) in enumerate(SEEDS, 1):
        times = [figures[(index, run)][0] / figures[(index, run)][1]
                 for run in range(1, per_seed + 1)]
        units = figures[(index, 1)][1]
        verdicts.append(f"{target}\t{seed}\t{per_seed}\t{units}\t{max(times):.1f}\t"
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

    def __init__(self, root, edit=None, host_edited="Linux", figures=None, seeds=SEEDS,
                 list_text=None, per_seed=PER_SEED):
        """`list_text`, if given, is the tree's list as written; `per_seed` is the edited
        host's runs a seed."""
        self.root = root
        self.seeds = root / "bound_seeds.txt"
        self.seeds.write_text(seeds_text(seeds) if list_text is None else list_text,
                              encoding="utf-8")
        self.launch = root / "launch.txt"
        self.launch.write_text(LAUNCH, encoding="utf-8")
        self.hosts = []
        for host in ("macOS", "Linux"):
            edited = host == host_edited
            self.hosts.append((host, write_host(
                root, host, self.seeds, figures if edited else None, edit if edited else None,
                per_seed if edited else PER_SEED)))

    def assemble(self, commit=COMMIT):
        return gate_results.assemble(commit, "2026-11-25", self.hosts, self.seeds, self.launch)


def assembled(root):
    """The document the small gate's records make."""
    return Records(root).assemble()


def replaced(wanted, old, new, count=1):
    """An edit that replaces old with new in the file named `wanted` alone, and fails if old is
    not there to replace."""
    def edit(file_name, text):
        if file_name != wanted:
            return text
        if old not in text:
            raise AssertionError(f"{old!r} not in {file_name}")
        return text.replace(old, new, count)
    return edit


def edits(*changes):
    """Several edits made one after another."""
    def edit(name, text):
        for change in changes:
            text = change(name, text)
        return text
    return edit


STATUS = "../../status.txt"
# Linux seed 1's run 2, as the records write it.
RUN_2_FIGURES = "run 2 of 5: libFuzzer 8192 ms, units 2, wall 8392 ms, exit 0, ok;"
RUN_2_ROW = "\t2\t8192\t8392\t0\t100.0\t2.34\t10\tok\t2\t"
SEED_1_VERDICT = "4668.5\t3908.0\t5000\tok"
ON_BOTH = gate_results.BUSY_BOTH_ENDS


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
                         ("run_length/bound-small-blocks", [1], 4668.5, 9337, 3908.0))
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

    def linux_run(self, document, seed, number):
        return next(run for run in document["runs"] if run["host"] == "Linux" and
                    run["seed"] == seed and run["run"] == number)

    def test_a_comment_in_the_list_is_no_seed(self):
        document = Records(self.dir, list_text="# the gate's seeds\n" + seeds_text()).assemble()
        self.assertEqual(document["summary"]["Linux"]["seeds"], len(SEEDS))

    def test_a_target_named_without_its_prefix_is_its_fuzz_target(self):
        listed = "".join(f"{target.removeprefix('fuzz_')} {seed}\n" for target, seed in SEEDS)
        document = Records(self.dir, list_text=listed).assemble()
        self.assertEqual({run["target"] for run in document["runs"]},
                         {target for target, _ in SEEDS})

    def test_a_timeout_outside_the_flags_row_is_not_the_bar(self):
        records = Records(self.dir)
        records.launch.write_text("note -timeout=12\n" + LAUNCH, encoding="utf-8")
        self.assertEqual(records.assemble()["header"]["bar_ms_per_execution"], 5000)

    def test_iowait_counts_as_idle(self):
        edit = replaced("seed-1-spin-1.stat1", "cpu2 102 0 0 1098 0 ", "cpu2 102 0 0 1048 50 ")
        run = self.linux_run(Records(self.dir, edit=edit).assemble(),
                             "run_length/bound-small-blocks", 1)
        self.assertEqual(run["cpu_busy_percent"]["cpu2"], 2.0)

    def test_a_run_with_a_finding_does_not_pass(self):
        edit = edits(
            lambda name, text: text + "==ERROR: AddressSanitizer: heap-use-after-free\n"
            if name == "seed-1-spin-2.log" else text,
            replaced(STATUS, RUN_2_FIGURES, RUN_2_FIGURES.replace(", ok;", ", finding;")),
            replaced("seedtime.tsv", RUN_2_ROW, RUN_2_ROW.replace("\tok\t", "\tfinding\t")),
            replaced("gate.tsv", SEED_1_VERDICT, SEED_1_VERDICT.replace("ok", "fail")))
        document = Records(self.dir, edit=edit).assemble()
        run = self.linux_run(document, "run_length/bound-small-blocks", 2)
        self.assertEqual((run["findings"], run["pass"]), (1, False))
        self.assertFalse(document["summary"]["Linux"]["all_pass"])

    def test_a_run_that_exited_otherwise_does_not_pass(self):
        edit = edits(
            replaced(STATUS, "seed-1-spin-2 end 2026-11-17T15:10:19Z exit 0",
                     "seed-1-spin-2 end 2026-11-17T15:10:19Z exit 1"),
            replaced(STATUS, RUN_2_FIGURES, RUN_2_FIGURES.replace("exit 0, ok;", "exit 1, crash;")),
            replaced("seedtime.tsv", RUN_2_ROW,
                     RUN_2_ROW.replace("\t0\t100.0", "\t1\t100.0").replace("\tok\t", "\tcrash\t")),
            replaced("gate.tsv", SEED_1_VERDICT, SEED_1_VERDICT.replace("ok", "fail")))
        run = self.linux_run(Records(self.dir, edit=edit).assemble(),
                             "run_length/bound-small-blocks", 2)
        self.assertEqual((run["exit_code"], run["findings"], run["pass"]), (1, 0, False))

    def test_a_run_that_crashed_keeps_its_findings(self):
        edit = edits(
            lambda name, text: text + "==ERROR: AddressSanitizer: heap-use-after-free\n"
            if name == "seed-1-spin-2.log" else text,
            replaced(STATUS, "seed-1-spin-2 end 2026-11-17T15:10:19Z exit 0",
                     "seed-1-spin-2 end 2026-11-17T15:10:19Z exit 1"),
            replaced(STATUS, RUN_2_FIGURES, RUN_2_FIGURES.replace("exit 0, ok;", "exit 1, crash;")),
            replaced("seedtime.tsv", RUN_2_ROW,
                     RUN_2_ROW.replace("\t0\t100.0", "\t1\t100.0").replace("\tok\t", "\tcrash\t")),
            replaced("gate.tsv", SEED_1_VERDICT, SEED_1_VERDICT.replace("ok", "fail")))
        run = self.linux_run(Records(self.dir, edit=edit).assemble(),
                             "run_length/bound-small-blocks", 2)
        self.assertEqual((run["exit_code"], run["findings"], run["pass"]), (1, 1, False))

    def test_a_linux_run_whose_sibling_stopped_does_not_pass(self):
        stopped = ON_BOTH.replace("after: spinner running", "after: spinner gone")
        edit = edits(
            replaced("seed-1-spin-2.beside", ON_BOTH, stopped),
            replaced(STATUS, f"load 2.34; {ON_BOTH}", f"load 2.34; {stopped}"),
            replaced("seedtime.tsv", f"\t2\t{ON_BOTH}\t2.0\traw/L20-bound-seeds/seed-1-spin-2.log",
                     f"\t2\t{stopped}\t2.0\traw/L20-bound-seeds/seed-1-spin-2.log"),
            replaced("gate.tsv", SEED_1_VERDICT, SEED_1_VERDICT.replace("ok", "fail")))
        run = self.linux_run(Records(self.dir, edit=edit).assemble(),
                             "run_length/bound-small-blocks", 2)
        self.assertEqual((run["sibling_busy_at_start"], run["sibling_busy_at_end"], run["pass"]),
                         (True, False, False))

    def test_a_start_line_of_another_runs_label_is_not_the_runs(self):
        edit = lambda name, text: text + (  # noqa: E731
            "   [L20-bound-seeds] seed-1-alone-1 start 2026-11-17T16:00:00Z\n"
            if name == STATUS else "")
        run = self.linux_run(Records(self.dir, edit=edit).assemble(),
                             "run_length/bound-small-blocks", 1)
        self.assertEqual(run["start"], "2026-11-17T15:10:00Z")

    def test_a_start_line_of_another_step_is_not_the_runs(self):
        edit = lambda name, text: text + (  # noqa: E731
            "   [L19-other-bound-seeds] seed-1-spin-1 start 2026-11-17T16:00:00Z\n"
            if name == STATUS else "")
        run = self.linux_run(Records(self.dir, edit=edit).assemble(),
                             "run_length/bound-small-blocks", 1)
        self.assertEqual(run["start"], "2026-11-17T15:10:00Z")

    def test_a_line_of_figures_of_another_step_is_not_the_runs(self):
        edit = lambda name, text: text + (  # noqa: E731
            "   [L19-other-bound-seeds] seed 1 fuzz_run_length run_length/bound-small-blocks "
            "run 1 of 5: libFuzzer 1 ms, units 2, wall 2 ms, exit 0, ok; load 0.00\n"
            if name == STATUS else "")
        run = self.linux_run(Records(self.dir, edit=edit).assemble(),
                             "run_length/bound-small-blocks", 1)
        self.assertEqual(run["file_ms"], 9337)

    def test_a_run_over_the_bar_does_not_pass(self):
        figures = dict(FIGURES["Linux"])
        figures[(1, 2)] = (10002, 2, 2.34, 130)

        def over(name, text):
            return text.replace("5001.0\t3908.0\t5000\tok", "5001.0\t3908.0\t5000\tover") \
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
            return text.replace("4668.5\t3908.0\t", "4600.0\t3908.0\t") \
                if name == "gate.tsv" else text
        self.assert_refused(Records(self.dir, edit=edit), "the gate's verdict of")

    def test_a_list_line_that_is_not_target_and_seed_is_refused(self):
        records = Records(self.dir)
        records.seeds.write_text(seeds_text() + "fuzz_x extra x/y\n", encoding="utf-8")
        self.assert_refused(records, 'not "target seed"')

    def test_a_launch_file_with_no_timeout_is_refused(self):
        records = Records(self.dir)
        records.launch.write_text("flags -rss_limit_mb=2048\n", encoding="utf-8")
        self.assert_refused(records, "has no flags row with one -timeout=N")

    def test_a_launch_file_with_two_timeouts_is_refused(self):
        records = Records(self.dir)
        records.launch.write_text(LAUNCH.replace("-timeout=10", "-timeout=10 -timeout=12"),
                                  encoding="utf-8")
        self.assert_refused(records, "has no flags row with one -timeout=N")

    def test_counters_without_a_processes_line_are_refused(self):
        edit = replaced("seed-1-spin-1.stat1", "processes 1258\n", "")
        self.assert_refused(Records(self.dir, edit=edit), "no per-cpu lines or no processes line")

    def test_counters_naming_other_cpus_are_refused(self):
        edit = replaced("seed-1-spin-1.stat1", "cpu5 101 0 0 1099 0 0 0 0 0 0\n", "")
        self.assert_refused(Records(self.dir, edit=edit), "name other cpus")

    def test_counters_over_which_no_time_passed_are_refused(self):
        edit = replaced("seed-1-spin-1.stat1", "cpu3 101 0 0 1099 ", "cpu3 100 0 0 1000 ")
        self.assert_refused(Records(self.dir, edit=edit), "no time passed on cpu3")

    def test_a_log_with_two_executed_lines_is_refused(self):
        def edit(name, text):
            if name != "seed-1-spin-1.log":
                return text
            executed = next(line for line in text.splitlines() if line.startswith("Executed"))
            return text + executed + "\n"
        self.assert_refused(Records(self.dir, edit=edit), "not one Executed line for")

    def test_a_log_that_executed_another_input_is_refused(self):
        edit = replaced("seed-1-spin-1.log", "fuzz-seeds/run_length/bound-small-blocks in",
                        "fuzz-seeds/run_length/bound-other in")
        self.assert_refused(Records(self.dir, edit=edit), "not one Executed line for")

    def test_a_log_with_two_unit_counts_is_refused(self):
        edit = replaced("seed-1-spin-1.log", "stat::number_of_executed_units: 2\n",
                        "stat::number_of_executed_units: 2\nstat::number_of_executed_units: 2\n")
        self.assert_refused(Records(self.dir, edit=edit), "not one unit count of at least 1")

    def test_a_log_with_no_executed_unit_is_refused(self):
        edit = replaced("seed-1-spin-1.log", "stat::number_of_executed_units: 2",
                        "stat::number_of_executed_units: 0")
        self.assert_refused(Records(self.dir, edit=edit), "not one unit count of at least 1")

    def test_a_runners_outcome_its_records_do_not_give_is_refused(self):
        edit = lambda name, text: text + (  # noqa: E731
            "==ERROR: AddressSanitizer: heap-use-after-free\n" if name == "seed-1-spin-2.log"
            else "")
        self.assert_refused(Records(self.dir, edit=edit),
                            "the runner's outcome 'ok' is not what its records give")

    def test_a_runners_table_with_a_run_too_many_is_refused(self):
        def edit(name, text):
            if name != "seedtime.tsv":
                return text
            last = text.splitlines()[-1]
            return text + last.replace("\t5\t", "\t6\t").replace("-5.log", "-6.log") + "\n"
        self.assert_refused(Records(self.dir, edit=edit),
                            "the runner's table holds 11 runs, the records 10")

    def test_a_runners_table_of_other_columns_is_refused(self):
        edit = replaced("seedtime.tsv", "\tload1\t", "\tload\t")
        self.assert_refused(Records(self.dir, edit=edit), "seedtime.tsv: not the columns")

    def test_a_gate_table_without_a_seed_is_refused(self):
        def edit(name, text):
            return "".join(line for line in text.splitlines(keepends=True)
                           if "bound-costliest" not in line) if name == "gate.tsv" else text
        self.assert_refused(Records(self.dir, edit=edit),
                            "the gate's verdict of gorilla_xor/bound-costliest")

    def test_a_gate_median_its_runs_do_not_give_is_refused(self):
        edit = replaced("gate.tsv", "4668.5\t3908.0\t", "4668.5\t3900.0\t")
        self.assert_refused(Records(self.dir, edit=edit), "the gate's verdict of")

    def test_a_gate_limit_not_the_bar_is_refused(self):
        edit = replaced("gate.tsv", SEED_1_VERDICT, SEED_1_VERDICT.replace("5000", "6000"))
        self.assert_refused(Records(self.dir, edit=edit), "the gate's verdict of")

    def test_a_gate_failing_a_seed_whose_runs_passed_is_refused(self):
        edit = replaced("gate.tsv", SEED_1_VERDICT, SEED_1_VERDICT.replace("ok", "fail"))
        self.assert_refused(Records(self.dir, edit=edit), "the gate's verdict of")

    def test_a_list_of_another_length_is_refused(self):
        edit = replaced(STATUS, f", {len(SEEDS)} seeds, each run", f", {len(SEEDS) + 1} seeds, "
                        "each run")
        self.assert_refused(Records(self.dir, edit=edit), "the gate ran a list whose SHA-256 is")

    def test_a_bar_from_another_timeout_is_refused(self):
        edit = replaced(STATUS, "-timeout=10), the worst", "-timeout=12), the worst")
        self.assert_refused(Records(self.dir, edit=edit),
                            "the gate's bar, 5000 ms, is not half of fuzz/launch.txt's -timeout=10")

    def test_a_bar_not_half_its_timeout_is_refused(self):
        edit = edits(replaced(STATUS, "bar 5000 ms an execution", "bar 6000 ms an execution"),
                     replaced("gate.tsv", "\t5000\t", "\t6000\t", count=-1))
        self.assert_refused(Records(self.dir, edit=edit),
                            "the gate's bar, 6000 ms, is not half of fuzz/launch.txt's -timeout=10")

    def test_hosts_that_ran_each_seed_a_different_number_of_times_are_refused(self):
        self.assert_refused(Records(self.dir, per_seed=4),
                            "the hosts ran each seed a different number of times")

    def test_two_gate_folders_are_refused(self):
        records = Records(self.dir)
        (dict(records.hosts)["Linux"] / "raw" / "L21-bound-seeds").mkdir()
        self.assert_refused(records, "not one raw/*-bound-seeds folder")

    def test_a_step_log_with_two_bar_lines_is_refused(self):
        def edit(name, text):
            if name != STATUS:
                return text
            bar = next(line for line in text.splitlines() if " sha256 " in line)
            return text + bar + "\n"
        self.assert_refused(Records(self.dir, edit=edit),
                            "the step log holds not one line of the gate's list and bar")

    def test_a_run_with_no_start_is_refused(self):
        def edit(name, text):
            return "".join(line for line in text.splitlines(keepends=True)
                           if "seed-1-spin-3 start" not in line) if name == STATUS else text
        self.assert_refused(Records(self.dir, edit=edit), "run_length/bound-small-blocks run 3: "
                            "no start, end, line of figures and table row")

    def test_a_run_with_no_line_of_figures_is_refused(self):
        def edit(name, text):
            return "".join(line for line in text.splitlines(keepends=True)
                           if "run_length/bound-small-blocks run 3 of 5:" not in line) \
                if name == STATUS else text
        self.assert_refused(Records(self.dir, edit=edit), "run_length/bound-small-blocks run 3: "
                            "no start, end, line of figures and table row")

    def test_a_run_with_no_table_row_is_refused(self):
        def edit(name, text):
            return "".join(line for line in text.splitlines(keepends=True)
                           if "seed-1-spin-3.log" not in line) if name == "seedtime.tsv" else text
        self.assert_refused(Records(self.dir, edit=edit), "run_length/bound-small-blocks run 3: "
                            "no start, end, line of figures and table row")

    def test_a_step_log_naming_another_seed_is_refused(self):
        edit = replaced(STATUS, "gorilla_xor/bound-costliest run 3 of 5:",
                        "gorilla_xor/bound-other run 3 of 5:")
        self.assert_refused(Records(self.dir, edit=edit),
                            "the runner's table or step log names another seed")

    def test_a_runners_table_naming_another_seed_is_refused(self):
        def edit(name, text):
            if name != "seedtime.tsv":
                return text
            return "".join(line.replace("gorilla_xor/bound-costliest", "gorilla_xor/bound-other")
                           if "seed-2-spin-3.log" in line else line
                           for line in text.splitlines(keepends=True))
        self.assert_refused(Records(self.dir, edit=edit),
                            "the runner's table or step log names another seed")

    def test_a_wall_time_not_the_step_logs_is_refused(self):
        edit = replaced("seed-1-spin-1.wall", "9537\n", "9600\n")
        self.assert_refused(Records(self.dir, edit=edit),
                            "the step log's shares, busy process or wall time")

    def test_a_missing_run_is_refused(self):
        def edit(name, text):
            return "\n".join(line for line in text.splitlines()
                             if "seed-2-spin-5 end" not in line) + "\n" \
                if name == "../../status.txt" else text
        self.assert_refused(Records(self.dir, edit=edit), "gorilla_xor/bound-costliest run 5")


class MainTest(Scratch):
    """The command line: the file written on records that agree, exit 1 and no file on records
    that do not, and a usage error, exit 2, on arguments it cannot take."""

    def main(self, records, *changes, out=None):
        out = out or self.dir / "out.json"
        arguments = {"--commit": COMMIT, "--assembled-on": "2026-11-25", "--out": str(out),
                     "--seeds": str(records.seeds), "--launch": str(records.launch)}
        hosts = [f"{host}={folder}" for host, folder in records.hosts]
        for key, value in changes:
            if key == "--host":
                hosts = value
            else:
                arguments[key] = value
        argv = [item for pair in arguments.items() for item in pair]
        for host in hosts:
            argv += ["--host", host]
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()) as said:
            code = gate_results.main(argv)
        return code, said.getvalue()

    def usage_error(self, *changes):
        with self.assertRaises(SystemExit) as caught:
            self.main(Records(self.dir), *changes)
        self.assertEqual(caught.exception.code, 2)

    def test_records_that_agree_make_the_file(self):
        records = Records(self.dir)
        self.assertEqual(self.main(records)[0], 0)
        self.assertEqual((self.dir / "out.json").read_text(encoding="utf-8"),
                         json.dumps(records.assemble(), indent=1) + "\n")

    def test_records_that_disagree_make_no_file(self):
        records = Records(self.dir, edit=replaced("gate.tsv", SEED_1_VERDICT,
                                                  SEED_1_VERDICT.replace("ok", "fail")))
        code, said = self.main(records)
        self.assertEqual(code, 1)
        self.assertIn("gate_results: Linux: the gate's verdict of", said)
        self.assertFalse((self.dir / "out.json").exists())

    def test_a_host_folder_that_is_not_there_makes_no_file(self):
        records = Records(self.dir)
        code, said = self.main(records, ("--host", [f"macOS={self.dir / 'macOS'}",
                                                    f"Linux={self.dir / 'nowhere'}"]))
        self.assertEqual(code, 1)
        self.assertFalse((self.dir / "out.json").exists())

    def test_a_commit_that_is_not_a_full_hash_is_a_usage_error(self):
        self.usage_error(("--commit", COMMIT[:12]))

    def test_a_day_that_is_not_a_date_is_a_usage_error(self):
        self.usage_error(("--assembled-on", "25-11-2026"))

    def test_a_host_of_another_name_is_a_usage_error(self):
        self.usage_error(("--host", [f"macOS={self.dir / 'macOS'}",
                                     f"Windows={self.dir / 'Linux'}"]))

    def test_a_host_with_no_folder_is_a_usage_error(self):
        self.usage_error(("--host", [f"macOS={self.dir / 'macOS'}", "Linux="]))

    def test_a_host_given_twice_is_a_usage_error(self):
        self.usage_error(("--host", [f"Linux={self.dir / 'Linux'}",
                                     f"Linux={self.dir / 'Linux'}"]))


if __name__ == "__main__":
    unittest.main()
