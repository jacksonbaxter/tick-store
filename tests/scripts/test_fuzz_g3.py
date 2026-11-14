#!/usr/bin/env python3
"""Tests for scripts/fuzz_g3.py. Run with:

    python3 -I tests/scripts/test_fuzz_g3.py

The launcher runs on a stand-in fuzzer, a short script that answers -runs=0, -merge=1 and
-max_total_time the way libFuzzer does, with its folders, seeds and launch file under the repo's
gitignored build/ directory, and its commit read from a small git repository made there. A
finding in the replay or the merge must stop the target before its timed run, and what the
launcher writes must be what fuzz_results.py and check_fuzz.py read.
"""

import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "fuzz_g3.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_fuzz_g3"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_fuzz  # noqa: E402
import fuzz_results  # noqa: E402

# libFuzzer's lines, as the probe of a real target printed them (docs/NOTES.md, G-3).
STAND_IN = r'''#!/usr/bin/env python3
import os, sys, time
from pathlib import Path
flags = [word for word in sys.argv[1:] if word.startswith("-")]
folders = [Path(word) for word in sys.argv[1:] if not word.startswith("-")]
fault = os.environ.get("STAND_IN_FAULT", "")
print("ASAN_OPTIONS seen: " + os.environ.get("ASAN_OPTIONS", ""))
def files(dirs):
    return sorted(path for folder in dirs for path in folder.iterdir())
if "-runs=0" in flags:
    count = len(files(folders))
    print(f"INFO: seed corpus: files: {count} min: 1b max: 1b total: {count}b rss: 30Mb")
    print(f"#{count + 1}\tINITED cov: 5 ft: 5 corp: 2/2b exec/s: 0 rss: 30Mb")
    if fault == "replay":
        print("==1==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x1")
        sys.exit(1)
    print(f"#{count + 1}\tDONE   cov: 5 ft: 5 corp: 2/2b lim: 4 exec/s: 0 rss: 30Mb")
    print(f"Done {count + 1} runs in 0 second(s)")
    print(f"stat::number_of_executed_units: {count + 1}")
elif "-merge=1" in flags:
    out, inputs = folders[0], files(folders[1:])
    print(f"MERGE-OUTER: {len(inputs)} files, 0 in the initial corpus, 0 processed earlier")
    print("MERGE-OUTER: attempt 1")
    attempts = 1
    if fault == "merge":
        print("MERGE-OUTER: attempt 2")
        attempts = 2
    for index, path in enumerate(inputs[:2]):
        (out / f"merged-{index}").write_bytes(path.read_bytes())
    print(f"MERGE-OUTER: successful in {attempts} attempt(s)")
    print("MERGE-OUTER: 2 new files with 5 new features added; 5 new coverage edges")
else:
    budget = int(next(word for word in flags if word.startswith("-max_total_time=")).split("=")[1])
    start = time.monotonic()
    count = len(files(folders))
    print(f"INFO: seed corpus: files: {count} min: 1b max: 1b total: {count}b rss: 30Mb")
    time.sleep(0.3)
    print(f"#{count + 1}\tINITED cov: 5 ft: 5 corp: 2/2b exec/s: 0 rss: 30Mb", flush=True)
    (folders[0] / "new-input").write_bytes(b"new")
    print("#4\tNEW    cov: 6 ft: 6 corp: 3/3b lim: 4 exec/s: 0 rss: 30Mb L: 1/1 MS: 1 Byte-")
    time.sleep(max(0, budget - (time.monotonic() - start)))
    print("#9\tDONE   cov: 6 ft: 6 corp: 3/3b lim: 4 exec/s: 9 rss: 31Mb")
    print(f"Done 9 runs in {budget} second(s)")
    print("stat::number_of_executed_units: 9")
    print("stat::average_exec_per_sec:     9")
    print("stat::peak_rss_mb:              31")
'''

LAUNCH = """flags -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1
asan_options Darwin detect_leaks=1:leak_check_at_exit=0:abort_on_error=0:exitcode=77
asan_options Linux detect_leaks=1:abort_on_error=0:exitcode=77
target fuzz_stand_in stand_in stand_in 64
"""


def git(repo, *arguments):
    return subprocess.run(["git", "-C", str(repo), "-c", "user.name=test", "-c",
                           "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
                           *arguments], capture_output=True, text=True, check=True).stdout.strip()


class LauncherTest(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.dir)
        self.repo = self.dir / "repo"
        (self.repo / "src").mkdir(parents=True)
        (self.repo / "src" / "code.cpp").write_text("int f();\n")
        git(self.repo, "init", "-q")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "code")
        self.commit = git(self.repo, "rev-parse", "HEAD")
        self.build = self.dir / "build"
        binary = self.build / "fuzz" / "fuzz_stand_in"
        binary.parent.mkdir(parents=True)
        binary.write_text(STAND_IN)
        binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
        seeds = self.build / "fuzz-seeds" / "stand_in"
        seeds.mkdir(parents=True)
        for index in range(3):
            (seeds / f"seed-{index}").write_bytes(bytes([index]))
        self.corpus = self.dir / "corpus"
        (self.corpus / "stand_in").mkdir(parents=True)
        (self.corpus / "stand_in" / "saved-0").write_bytes(b"saved")
        self.launch = self.dir / "launch.txt"
        self.launch.write_text(LAUNCH)
        self.logs = self.dir / "logs"

    def arguments(self, targets=("fuzz_stand_in",)):
        return ["--logs", str(self.logs), "--seconds", "1", "--build", str(self.build),
                "--corpus", str(self.corpus), "--work", str(self.dir / "work"), "--artifacts",
                str(self.dir / "artifacts"), "--launch", str(self.launch), "--repo",
                str(self.repo), *targets]

    def launch_run(self, fault="", targets=("fuzz_stand_in",)):
        env = dict(os.environ, STAND_IN_FAULT=fault)
        return subprocess.run([sys.executable, "-I", str(SCRIPT), *self.arguments(targets)],
                              capture_output=True, text=True, env=env, check=False)

    def phases(self):
        return json.loads((self.logs / "fuzz_stand_in.phases.json").read_text())

    def run_record(self):
        facts = {"cpu_model": "m", "compiler": "c", "compile_flags": "f",
                 "default_kernel_level": "neon"}
        return fuzz_results.run_of("macOS", self.logs, "fuzz_stand_in", facts, [str(self.dir)])

    def test_a_clean_target_replays_merges_then_runs_timed_on_the_merged_set(self):
        done = self.launch_run()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        phases = self.phases()
        self.assertEqual(phases["commit"], self.commit)
        self.assertNotIn("failed_in", phases)
        self.assertIn(" -runs=0 -max_len=64 -rss_limit_mb=2048", phases["replay"]["command"])
        self.assertIn(" -merge=1 -max_len=64 ", phases["merge"]["command"])
        timed = phases["timed"]["command"].split()
        self.assertEqual(timed[-1], str(self.dir / "work" / "fuzz_stand_in"))
        self.assertIn("-max_total_time=1", timed)
        self.assertGreaterEqual(phases["timed"]["seconds_to_inited"], 0.3)
        meta = (self.logs / "fuzz_stand_in.meta").read_text()
        system = "Darwin" if platform.system() == "Darwin" else "Linux"
        options = next(line.split()[2] for line in LAUNCH.splitlines()
                       if line.startswith(f"asan_options {system} "))
        self.assertIn(f"ASAN_OPTIONS={options} UBSAN_OPTIONS=print_stacktrace=1\n", meta)
        self.assertIn(f"ASAN_OPTIONS seen: {options}",
                      (self.logs / "fuzz_stand_in.replay.log").read_text())
        # Only the timed run's new input joins the saved corpus, not the merged seed copies.
        self.assertEqual(sorted(path.name for path in (self.corpus / "stand_in").iterdir()),
                         ["new-input", "saved-0"])
        run = self.run_record()
        self.assertTrue(run["clean"], run["findings"])
        self.assertEqual((run["replay"]["inputs"], run["replay"]["runs"]), (4, 5))
        self.assertEqual((run["merge"]["inputs_kept"], run["merge"]["attempts"]), (2, 1))
        # What check_fuzz.py would still refuse of it is the length alone: 1 s, not 1,800.
        problems = check_fuzz.check_phases("run", run)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(f"under the {check_fuzz.REQUIRED_SECONDS} s", problems[0])

    def test_a_finding_in_the_replay_stops_the_target_before_its_timed_run(self):
        done = self.launch_run("replay")
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(self.phases()["failed_in"], "replay")
        self.assertNotIn("merge", self.phases())
        self.assertNotIn("timed", self.phases())
        run = self.run_record()
        self.assertFalse(run["clean"])
        self.assertEqual(run["findings"][0]["kind"], "crash")

    def test_a_merge_libfuzzer_had_to_restart_fails_the_target(self):
        done = self.launch_run("merge")
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(self.phases()["failed_in"], "merge")
        self.assertNotIn("timed", self.phases())
        run = self.run_record()
        self.assertFalse(run["clean"])
        self.assertEqual(run["merge"]["attempts"], 2)

    def test_a_checkout_whose_fuzzed_code_differs_from_head_is_refused(self):
        (self.repo / "src" / "code.cpp").write_text("int g();\n")
        done = self.launch_run()
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("the fuzzed code differs from HEAD", done.stderr)
        self.assertFalse(self.logs.exists())

    # Review RG-9: a target whose worker raises, or that never reaches the results, is failed,
    # and no earlier launch's files can stand for this one's.

    def test_a_target_whose_worker_raises_is_failed_and_the_next_target_still_runs(self):
        # A target whose saved corpus folder is a file: making it raises in the launcher.
        self.launch.write_text(LAUNCH + "target fuzz_broken broken stand_in 64\n")
        shutil.copy2(self.build / "fuzz" / "fuzz_stand_in", self.build / "fuzz" / "fuzz_broken")
        (self.corpus / "broken").write_text("not a folder")
        done = self.launch_run(targets=("fuzz_broken", "fuzz_stand_in"))
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn("fuzz_broken: FAILED, the launcher raised FileExistsError", done.stdout)
        self.assertIn("1 clean, failed: ['fuzz_broken']", done.stdout)
        self.assertFalse((self.logs / "fuzz_broken.meta").exists())
        # The same worker goes on to the next target.
        self.assertIn("timed", self.phases())

    def test_a_listed_target_missing_from_the_results_counts_as_failed(self):
        fuzz_g3 = __import__("fuzz_g3")

        def dies(*_):
            raise SystemExit("a worker that ends before it records its target")

        with mock.patch.object(fuzz_g3, "run_target", dies), \
                mock.patch.object(fuzz_g3.time, "sleep"):
            code = fuzz_g3.main(self.arguments())
        self.assertEqual(code, 1)

    def test_an_earlier_launchs_files_in_the_logs_folder_are_refused(self):
        self.logs.mkdir()
        for suffix in (".meta", ".phases.json"):
            (self.logs / f"fuzz_stand_in{suffix}").write_text("an earlier launch\n")
        done = self.launch_run()
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("fuzz_stand_in.meta", done.stderr)
        self.assertIn("fuzz_stand_in.phases.json", done.stderr)
        self.assertEqual((self.logs / "fuzz_stand_in.meta").read_text(), "an earlier launch\n")
        self.assertFalse((self.logs / "fuzz_stand_in.replay.log").exists())

    def test_the_repos_launch_file_has_every_target(self):
        launch = __import__("fuzz_g3").read_launch()
        self.assertEqual(sorted(launch["targets"]), check_fuzz.targets_in(check_fuzz.DEFAULT_CMAKE))
        for required in check_fuzz.REQUIRED_FLAGS:
            self.assertIn(required, launch["flags"])
        self.assertEqual(sorted(launch["asan_options"]), ["Darwin", "Linux"])


if __name__ == "__main__":
    os.environ.pop("PYTHONPATH", None)
    unittest.main()
