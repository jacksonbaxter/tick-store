#!/usr/bin/env python3
"""Tests for scripts/check.sh. Run with:

    python3 -I tests/scripts/test_check_sh.py

These run the script as a user would, on the steps that need no build. Each output folder is
under the repo's gitignored build/ directory. They cover the promises a broken wrapper would
hide: a failed step gives a non-zero exit and is named, the steps after it still run, and a
skip is never counted as a pass.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check.sh"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ci.yml"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_sh"


class CheckScript(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.logs = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.logs)
        self.env = dict(os.environ, CXX=os.environ.get("CXX", "c++"))

    def run_script(self, *arguments, env=None):
        """Always with this test's own log folder, so a test never touches a real run's logs."""
        return subprocess.run([str(SCRIPT), "--log-dir", str(self.logs), *arguments],
                              capture_output=True, text=True, env=env or self.env)

    def summary(self):
        lines = (self.logs / "summary.tsv").read_text().splitlines()
        return {line.split("\t")[0]: line.split("\t")[1] for line in lines}

    def test_missing_cxx_is_a_clear_error(self):
        env = {key: value for key, value in self.env.items() if key != "CXX"}
        done = self.run_script("--only", "hygiene", env=env)
        self.assertEqual(done.returncode, 2)
        self.assertIn("CXX is not set", done.stderr)

    def test_a_cxx_that_cannot_run_is_a_clear_error(self):
        done = self.run_script("--only", "hygiene", env=dict(self.env, CXX="no-such-compiler"))
        self.assertEqual(done.returncode, 2)
        self.assertIn("cannot be run", done.stderr)

    def test_missing_clang_tidy_is_a_clear_error(self):
        # Only what the script needs before it looks for clang-tidy.
        tools = self.logs / "tools"
        tools.mkdir()
        for tool in ("env", "bash", "dirname"):
            (tools / tool).symlink_to(shutil.which(tool))
        cxx = shutil.which(self.env["CXX"]) or self.env["CXX"]
        done = self.run_script("--only", "hygiene", env=dict(self.env, CXX=cxx, PATH=str(tools)))
        self.assertEqual(done.returncode, 2)
        self.assertIn("clang-tidy is not on PATH", done.stderr)

    def test_an_unknown_step_is_refused(self):
        done = self.run_script("--only", "no-such-step")
        self.assertEqual(done.returncode, 2)
        self.assertIn("unknown step", done.stderr)

    def test_a_bad_fuzz_budget_is_refused(self):
        done = self.run_script("--fuzz-seconds", "0")
        self.assertEqual(done.returncode, 2)

    def test_a_passing_step_exits_zero(self):
        done = self.run_script("--only", "render-check")
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual(self.summary(), {"render-check": "PASS"})

    def test_quick_skips_visibly_and_says_so(self):
        done = self.run_script("--quick", "--only", "asan-test,coverage,render-check")
        self.assertEqual(done.returncode, 0, done.stdout)
        status = self.summary()
        self.assertEqual(status["asan-test"], "SKIP")
        self.assertEqual(status["coverage"], "SKIP")
        self.assertRegex(done.stdout, r"QUICK RUN: .* did NOT run")
        self.assertRegex(done.stdout, r"pass 1, fail 0, blocked 0, skipped 3 \(a skip is not a pass")

    def test_no_skips_turns_a_skip_into_a_failure(self):
        # Review E-6: CI named py-compare, which skipped there for want of a .venv.
        env = dict(self.env, CHECK_VENV=str(self.logs / "no-such-venv"))
        done = self.run_script("--only", "py-compare", env=env)
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual(self.summary(), {"py-compare": "SKIP"})
        done = self.run_script("--no-skips", "--only", "py-compare", env=env)
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(self.summary(), {"py-compare": "FAIL"})
        self.assertIn("--no-skips: no .venv and no CHECK_VENV", done.stdout)

    def test_no_skips_refuses_quick(self):
        done = self.run_script("--no-skips", "--quick", "--only", "render-check")
        self.assertEqual(done.returncode, 2)
        self.assertIn("--no-skips and --quick", done.stderr)

    def test_the_fuzz_launch_lives_in_fuzz_launch_txt_alone(self):
        # G-3's tie to the code names fuzz/; check.sh, where unrelated edits are frequent,
        # names no fuzz flag itself, so the 30-minute runs and the short steps share one list.
        launch = (REPO_ROOT / "fuzz" / "launch.txt").read_text()
        rows = [line.split() for line in launch.splitlines() if line.startswith("target ")]
        cmake = (REPO_ROOT / "fuzz" / "CMakeLists.txt").read_text()
        targets = sorted(re.findall(r"^tick_store_add_fuzzer\((\w+)\)", cmake, re.MULTILINE))
        self.assertEqual(sorted(row[1] for row in rows), targets)
        self.assertTrue(all(len(row) == 5 and row[4].isdigit() for row in rows), rows)
        script = SCRIPT.read_text()
        for flag in ("-rss_limit_mb", "-malloc_limit_mb", "-timeout=", "exitcode=77",
                     "leak_check_at_exit"):
            self.assertNotIn(flag, script)
        for target in targets:
            self.assertIn(f"run_fuzzer \"$1\" {target}; }}", script)

    def workflow_runs(self):
        """The workflow's check.sh command lines."""
        return [line.strip() for line in WORKFLOW.read_text().splitlines()
                if "scripts/check.sh" in line and line.strip().startswith(("run:", "scripts/"))]

    def test_the_workflow_runs_every_step_it_names(self):
        runs = self.workflow_runs()
        self.assertTrue(runs)
        for run in runs:
            self.assertIn("--no-skips", run)
            self.assertNotIn("--quick", run)

    def test_the_workflow_makes_the_environment_py_compare_needs(self):
        text = WORKFLOW.read_text()
        step = re.search(r"- name: [^\n]*\n\s+if: matrix\.check\.name == 'repo'\n"
                         r"\s+run: \|\n((?:\s{10,}\S[^\n]*\n)+)", text)
        self.assertIsNotNone(step, "no step of the repo job makes the .venv")
        commands = step[1]
        self.assertIn("-m venv .venv", commands)
        self.assertIn("--require-hashes", commands)
        self.assertIn("-r compare/requirements.lock", commands)

    def test_every_step_is_run_by_the_workflow(self):
        listed = subprocess.run([str(SCRIPT), "--list"], capture_output=True, text=True,
                                env=self.env)
        self.assertEqual(listed.returncode, 0, listed.stderr)
        steps = listed.stdout.split()
        self.assertTrue(steps)
        named = set()
        for listed in re.findall(r'only: "([a-z0-9,-]+)"', WORKFLOW.read_text()):
            named.update(listed.split(","))
        # A build is named by the steps that need it, and check.sh adds it.
        tests = {step for step in steps if not step.endswith("-build")}
        self.assertEqual(sorted(tests - named), [])


if __name__ == "__main__":
    unittest.main()
