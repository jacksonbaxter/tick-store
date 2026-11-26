#!/usr/bin/env python3
"""Tests that nothing which makes or clones a repository acts on one a caller's git variables
name. Run with:

    python3 -I tests/scripts/test_git_env.py

Git exports GIT_DIR and its kin to hooks and to `git rebase --exec`. Inherited, they turn a
`git -C <scratch> init`, commit, reset or clone on the repository they name, as the script tests
did on 2026-11-25 when a rebase step ran them (docs/NOTES.md). Each test here makes a decoy
repository in its own scratch folder under build/, sets those variables in a child's environment
to that decoy and nothing else, runs what makes or clones repositories, and requires every file
under the decoy's .git, its HEAD, refs, config, index and objects, unchanged byte for byte.
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

# This file makes repositories too: the same clearing as each script test's.
for git_variable in (*subprocess.run(["git", "rev-parse", "--local-env-vars"], check=True,
                                     capture_output=True, text=True).stdout.split(),
                     "GIT_NAMESPACE"):
    os.environ.pop(git_variable, None)

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS = REPO_ROOT / "tests" / "scripts"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_git_env"
CHECK_SH = REPO_ROOT / "scripts" / "check.sh"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_hygiene  # noqa: E402
import prove_checks  # noqa: E402

# git's own list of a repository's variables, and GIT_NAMESPACE, which it leaves out.
CLEARED = frozenset((*subprocess.run(["git", "rev-parse", "--local-env-vars"], check=True,
                                     capture_output=True, text=True).stdout.split(),
                     "GIT_NAMESPACE"))

# Each script test that makes or clones a repository, by itself or through a helper it calls,
# with one test of it that does.
SCRIPT_TESTS = {
    "test_check_fuzz.py": "TieTest.test_a_file_at_unchanged_code_is_tied_and_passes",
    "test_check_hygiene.py": "Hygiene.test_a_clean_repository_passes",
    "test_check_sh.py": "CheckScript.test_a_g3_file_with_no_history_to_tie_it_skips_and_fails_"
                        "under_no_skips",
    "test_check_ties.py": "SameContent.test_a_short_commit_is_resolved",
    "test_fuzz_g3.py": "LauncherTest.test_a_clean_target_replays_merges_then_runs_timed_on_the_"
                       "merged_set",
    "test_prove_checks.py": "EveryFaultApplies.test_every_listed_mutation_changes_the_committed_"
                            "tree",
    "test_render_writeup.py": "Commits.test_an_exception_is_named_in_the_essay_s_own_sentence",
}

# prepare_tree() pointed at a small source repository: it clones, fetches and resets, then
# stops at the fetched sources the source repository lacks.
PROVER_CLONE = """
import json, os, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import prove_checks
prove_checks.REPO_ROOT, prove_checks.WORK = Path(sys.argv[2]), Path(sys.argv[3])
prove_checks.TREE = prove_checks.WORK / "tree"
prove_checks.clear_git_variables()
print(json.dumps(sorted(name for name in os.environ if name.startswith("GIT_"))))
try:
    prove_checks.prepare_tree()
except SystemExit as stop:
    print(stop)
"""


def git(repo, *arguments):
    return subprocess.run(["git", "-C", str(repo), "-c", "user.name=test", "-c",
                           "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
                           *arguments], capture_output=True, text=True, check=True).stdout.strip()


def make_repo(root, text):
    root.mkdir(parents=True)
    git(root, "init", "-q")
    (root / "file.txt").write_text(text, encoding="utf-8")
    git(root, "add", "file.txt")
    git(root, "commit", "-q", "-m", text.strip())
    return git(root, "rev-parse", "HEAD")


def said(output):
    """A child's output, indented, so that its own "Ran 1 test" and FAIL lines are not read as
    this file's by check.sh's count of tests or by a fault's expected string."""
    return "\n" + "\n".join("    " + line for line in output.splitlines())


def state(repo):
    """Every file under the repository's .git, by path, with its bytes."""
    git_dir = repo / ".git"
    return {path.relative_to(git_dir).as_posix(): path.read_bytes()
            for path in sorted(git_dir.rglob("*")) if path.is_file()}


class GitVariables(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.dir)
        self.runs = 0

    def callers(self):
        """A fresh decoy, and the environment of a caller whose git names it, for each way a
        caller may: its git directory alone, as a rebase step's, and with its work tree."""
        for work_tree in (False, True):
            self.runs += 1
            decoy = self.dir / f"decoy-{self.runs}"
            make_repo(decoy, "decoy\n")
            git_dir = decoy / ".git"
            env = dict(os.environ, GIT_DIR=str(git_dir), GIT_COMMON_DIR=str(git_dir),
                       GIT_INDEX_FILE=str(git_dir / "index"),
                       GIT_OBJECT_DIRECTORY=str(git_dir / "objects"), GIT_NAMESPACE="decoy")
            if work_tree:
                env["GIT_WORK_TREE"] = str(decoy)
            yield ("GIT_DIR and GIT_WORK_TREE" if work_tree else "GIT_DIR"), decoy, env

    def assert_unchanged(self, decoy, before, what):
        after = state(decoy)
        changed = sorted(name for name in before.keys() | after.keys()
                         if before.get(name) != after.get(name))
        if changed:
            self.fail(f"{what} changed the decoy repository the caller's git variables name: "
                      + ", ".join(changed))

    def test_each_script_test_keeps_to_its_own_repositories(self):
        for name, test in SCRIPT_TESTS.items():
            for caller, decoy, env in self.callers():
                with self.subTest(file=name, caller=caller):
                    before = state(decoy)
                    done = subprocess.run([sys.executable, "-I", str(TESTS / name), test],
                                          capture_output=True, text=True, env=env)
                    self.assert_unchanged(decoy, before, f"{name} {test}")
                    self.assertEqual(done.returncode, 0, said(done.stderr[-3000:]))

    def stand_in(self, made, seen):
        """A python3 first on PATH, which the hygiene step runs: it makes a repository of its
        own, as a script test does, and writes down the git variables it was given."""
        folder = made.parent / f"{made.name}-bin"
        folder.mkdir()
        script = folder / "python3"
        script.write_text(f"""#!/bin/sh
env | sed -n 's/^\\(GIT_[A-Z_]*\\)=.*/\\1/p' > "{seen}"
git init -q "{made}" &&
git -C "{made}" -c user.name=t -c user.email=t@t -c commit.gpgsign=false \\
    commit -q --allow-empty -m made
""")
        script.chmod(0o755)
        return folder

    def run_check_sh(self, env, logs):
        return subprocess.run([str(CHECK_SH), "--log-dir", str(logs), "--only", "hygiene"],
                              capture_output=True, text=True,
                              env=dict(env, CXX=os.environ.get("CXX", "c++")))

    def test_check_sh_clears_them_before_any_step(self):
        for caller, decoy, env in self.callers():
            with self.subTest(caller=caller):
                before = state(decoy)
                made, seen = self.dir / f"made-{self.runs}", self.dir / f"seen-{self.runs}.txt"
                folder = self.stand_in(made, seen)
                done = self.run_check_sh(dict(env, PATH=f"{folder}{os.pathsep}{env['PATH']}"),
                                         self.dir / f"logs-{self.runs}")
                self.assert_unchanged(decoy, before, "a step of check.sh")
                self.assertEqual(done.returncode, 0, said(done.stdout + done.stderr))
                self.assertEqual(git(made, "rev-list", "--count", "HEAD"), "1")
                self.assertEqual(sorted(set(seen.read_text().split()) & CLEARED), [])

    def test_check_sh_still_reads_its_own_tree(self):
        names, kind = check_hygiene.files_to_check(REPO_ROOT)
        for caller, decoy, env in self.callers():
            with self.subTest(caller=caller):
                before = state(decoy)
                logs = self.dir / f"logs-{self.runs}"
                done = self.run_check_sh(env, logs)
                self.assert_unchanged(decoy, before, "check.sh's hygiene step")
                self.assertEqual(done.returncode, 0, said(done.stdout + done.stderr))
                self.assertIn(f"{len(names)} {kind} clean", (logs / "hygiene.log").read_text())

    def test_prove_checks_clears_them_before_it_clones(self):
        source = self.dir / "source"
        commit = make_repo(source, "source\n")
        for caller, decoy, env in self.callers():
            with self.subTest(caller=caller):
                before = state(decoy)
                work = self.dir / f"work-{self.runs}"
                done = subprocess.run([sys.executable, "-I", "-c", PROVER_CLONE,
                                       str(REPO_ROOT / "scripts"), str(source), str(work)],
                                      capture_output=True, text=True, env=env)
                self.assert_unchanged(decoy, before, "prove_checks.py's clone")
                self.assertEqual(done.returncode, 0, said(done.stderr))
                left, said = done.stdout.splitlines()[:2]
                self.assertEqual(sorted(set(json.loads(left)) & CLEARED), [])
                self.assertIn("googletest-src is not fetched", said)
                self.assertEqual(git(work / "tree", "rev-parse", "HEAD"), commit)

    def test_prove_checks_clears_them_before_anything_else(self):
        with mock.patch.object(prove_checks, "clear_git_variables",
                               side_effect=RuntimeError("cleared first")):
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "cleared first"):
                    prove_checks.main(["--list"])


if __name__ == "__main__":
    unittest.main()
