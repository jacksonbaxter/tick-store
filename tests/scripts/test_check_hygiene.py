#!/usr/bin/env python3
"""Tests for scripts/check_hygiene.py. Run with:

    python3 -I tests/scripts/test_check_hygiene.py

The check runs as a subprocess on a small git repository generated under the repo's
gitignored build/ directory. Each rule gets one injected fault, and each test requires the
exit code and the message naming the file. The clean repository is shown passing first, so a
fault test cannot pass by the repository having been broken all along.
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECK = REPO_ROOT / "scripts" / "check_hygiene.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_hygiene"
# Split, so this file does not trip the check it tests.
MAC_HOME = b"/Us" b"ers/alice"
LINUX_HOME = b"/ho" b"me/alice"


class Hygiene(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        self.track("README.md", b"A clean file\n")

    def track(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        subprocess.run(["git", "-C", str(self.root), "add", "-f", name], check=True)

    def run_check(self):
        return subprocess.run([sys.executable, "-I", str(CHECK), "--root", str(self.root)],
                              capture_output=True, text=True)

    def assert_refused(self, message):
        done = self.run_check()
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn(message, done.stderr)

    def test_a_clean_repository_passes(self):
        done = self.run_check()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("1 tracked files clean", done.stdout)

    def test_no_tracked_files_is_a_failure(self):
        subprocess.run(["git", "-C", str(self.root), "rm", "-q", "--cached", "README.md"],
                       check=True)
        self.assert_refused("nothing was checked")

    def test_a_mac_home_path_is_refused_with_its_line(self):
        self.track("notes.txt", b"first line\nsee " + MAC_HOME + b"/data\n")
        self.assert_refused("notes.txt:2: absolute home path")

    def test_a_linux_home_path_is_refused(self):
        self.track("notes.txt", b"see " + LINUX_HOME + b"/data\n")
        self.assert_refused("notes.txt:1: absolute home path")

    def test_a_home_path_inside_a_binary_file_is_refused(self):
        self.track("blob", b"\0\1\2" + LINUX_HOME + b"\0")
        self.assert_refused("blob:1: absolute home path")

    def test_the_fixture_placeholders_are_allowed(self):
        self.track("test.py", b"inject('/Users/someone/x', '-I/home/x.')\n")
        self.assertEqual(self.run_check().returncode, 0)

    def test_a_home_folder_with_no_user_is_allowed(self):
        self.track("notes.txt", b"macOS keeps accounts under /Users/ and Linux under /home/\n")
        self.assertEqual(self.run_check().returncode, 0)

    def test_a_tracked_data_file_is_refused(self):
        self.track("12302019.NASDAQ_ITCH50", b"x")
        self.assert_refused("12302019.NASDAQ_ITCH50: data file is tracked")

    def test_anything_tracked_in_the_data_folder_is_refused(self):
        self.track("data/notes.txt", b"x")
        self.assert_refused("data/notes.txt: data file is tracked")

    def test_a_large_file_is_refused(self):
        self.track("big.txt", b"x" * 1_000_001)
        self.assert_refused("big.txt: 1000001 bytes is over")

    def test_a_named_kind_of_result_may_be_large_at_any_commit(self):
        self.track("results/compare/macOS-12302019-open-0123456789ab.json", b"x" * 1_000_001)
        self.assertEqual(self.run_check().returncode, 0)

    def test_another_kind_of_result_may_not_be_large(self):
        self.track("results/bench/macOS-itch_codec-0123456789ab.json", b"x" * 1_000_001)
        self.assert_refused("results/bench/macOS-itch_codec-0123456789ab.json: 1000001 bytes")

    def test_an_untracked_data_file_is_ignored(self):
        (self.root / "12302019.NASDAQ_ITCH50").write_bytes(b"x")
        self.assertEqual(self.run_check().returncode, 0)

    def test_every_problem_is_named(self):
        self.track("a.txt", MAC_HOME + b"\n")
        self.track("b.gz", b"x")
        done = self.run_check()
        self.assertIn("a.txt:1", done.stderr)
        self.assertIn("b.gz", done.stderr)


class ExportedTree(unittest.TestCase):
    """A tree exported with git archive, as a sync to the Linux host makes: no .git, a COMMIT
    file, and every file tracked. The scratch folder sits inside this checkout but is not the
    top of a work tree, as the export is not."""

    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        self.write("COMMIT", b"0123456789abcdef0123456789abcdef01234567\n")
        self.write(".gitignore", b"/build/\n__pycache__/\n")
        self.write("README.md", b"A clean file\n")

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def run_check(self):
        return subprocess.run([sys.executable, "-I", str(CHECK), "--root", str(self.root)],
                              capture_output=True, text=True)

    def test_a_clean_export_passes_and_says_what_it_read(self):
        done = self.run_check()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("2 files of the tree exported at 0123456789ab clean", done.stdout)

    def test_a_home_path_in_an_exported_file_is_refused(self):
        self.write("docs/notes.txt", b"see " + LINUX_HOME + b"/data\n")
        done = self.run_check()
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("docs/notes.txt:1: absolute home path", done.stderr)

    def test_a_data_file_in_an_export_is_refused(self):
        self.write("12302019.NASDAQ_ITCH50", b"x")
        done = self.run_check()
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("12302019.NASDAQ_ITCH50: data file is tracked", done.stderr)

    def test_what_the_gitignore_excludes_is_not_read(self):
        self.write("build/check/step.log", b"ran in " + LINUX_HOME + b"/x\n")
        self.write("scripts/__pycache__/a.pyc", LINUX_HOME)
        self.assertEqual(self.run_check().returncode, 0)

    def test_a_folder_with_no_commit_file_is_refused(self):
        (self.root / "COMMIT").unlink()
        done = self.run_check()
        self.assertEqual(done.returncode, 1)
        self.assertIn("neither the top of a git work tree nor an exported tree", done.stderr)


if __name__ == "__main__":
    unittest.main()
