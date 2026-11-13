#!/usr/bin/env python3
"""Tests for scripts/prove_checks.py. Run with:

    python3 -I tests/scripts/test_prove_checks.py

The verdict is what stands between a fault proof and a false claim, so these give it each way
a detection can be wrong, a build error above all. The rest ties the fault list to the checks:
every check has a fault, and every patch in faults/ is used.
"""

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRATCH_ROOT = REPO_ROOT / "build" / "test_prove_checks"
SPEC = importlib.util.spec_from_file_location("prove_checks",
                                              REPO_ROOT / "scripts" / "prove_checks.py")
prove = importlib.util.module_from_spec(SPEC)
sys.modules["prove_checks"] = prove
SPEC.loader.exec_module(prove)

FAULT = prove.Fault("example", "dev-test", "an example", 8, "the expected string")


def run_of(script_exit=1, status="FAIL", step_exit=8, log="... the expected string ...",
           later_status=""):
    return prove.StepRun(script_exit, status, step_exit, log, later_status)


class Verdict(unittest.TestCase):
    def test_the_expected_failure_is_proven(self):
        self.assertEqual(prove.judge(FAULT, run_of()), "PROVEN")

    def test_a_check_that_still_passes_is_undetected(self):
        self.assertEqual(prove.judge(FAULT, run_of(0, "PASS", 0, "")), "UNDETECTED")

    def test_a_build_error_is_not_a_detection(self):
        verdict = prove.judge(FAULT, run_of(status="BLOCKED", step_exit=-1, log=""))
        self.assertIn("WRONG REASON", verdict)

    def test_the_wrong_exit_code_is_not_a_detection(self):
        self.assertIn("WRONG REASON", prove.judge(FAULT, run_of(step_exit=1)))

    def test_the_wrong_output_is_not_a_detection(self):
        self.assertIn("WRONG REASON", prove.judge(FAULT, run_of(log="something else")))

    def test_a_usage_error_from_the_script_is_not_a_detection(self):
        self.assertIn("WRONG REASON", prove.judge(FAULT, run_of(script_exit=2)))

    def test_a_later_step_that_did_not_run_is_not_proven(self):
        fault = prove.Fault("example", "hygiene", "an example", 8, "the expected string",
                            later_step="render-check")
        self.assertIn("WRONG REASON", prove.judge(fault, run_of(later_status="")))
        self.assertEqual(prove.judge(fault, run_of(later_status="PASS")), "PROVEN")

    def test_a_tolerated_fault_must_pass_and_show_the_warning(self):
        fault = prove.Fault("example", "dev-build", "an example", 0, "warning", tolerated=True)
        self.assertEqual(prove.judge(fault, run_of(0, "PASS", 0, "warning")), "PROVEN")
        self.assertEqual(prove.judge(fault, run_of(0, "PASS", 0, "")), "NOT TOLERATED")
        self.assertEqual(prove.judge(fault, run_of(1, "FAIL", 1, "warning")), "NOT TOLERATED")

    def test_the_control_must_go_unnoticed(self):
        fault = prove.Fault("example", "render-check", "an example", 0, "", control=True)
        self.assertEqual(prove.judge(fault, run_of(0, "PASS", 0, "")), "UNDETECTED")
        self.assertEqual(prove.judge(fault, run_of()), "CONTROL FAILED")


class FaultList(unittest.TestCase):
    def test_every_check_has_a_fault(self):
        steps = subprocess.run([str(REPO_ROOT / "scripts" / "check.sh"), "--list"],
                               capture_output=True, text=True, check=True).stdout.split()
        # A build's failure is the step that needs it, and the -Werror faults cover dev-build.
        checks = {step for step in steps if not step.endswith("-build")}
        proven = {fault.check for fault in prove.FAULTS if not fault.control}
        self.assertEqual(sorted(checks - proven), [])

    def test_every_fault_names_a_real_step(self):
        steps = subprocess.run([str(REPO_ROOT / "scripts" / "check.sh"), "--list"],
                               capture_output=True, text=True, check=True).stdout.split()
        for fault in prove.FAULTS:
            self.assertIn(fault.check, steps, fault.id)

    def test_every_patch_is_used(self):
        ids = {fault.id for fault in prove.FAULTS}
        for patch in prove.FAULTS_DIR.glob("*.patch"):
            self.assertIn(patch.stem, ids)

    def test_fault_ids_are_unique(self):
        ids = [fault.id for fault in prove.FAULTS]
        self.assertEqual(len(ids), len(set(ids)))

    def test_a_changed_fault_has_a_changed_fingerprint(self):
        other = prove.Fault("example", "dev-test", "an example", 8, "another string")
        self.assertNotEqual(prove.fingerprint(FAULT), prove.fingerprint(other))


def mutation_named_alike(argument, helper_value="one"):
    """A mutation called `mutate` that calls a helper with `argument`, as the prover's own
    mutations call edit_json and edit_text; the helper returns `helper_value`."""
    namespace = {}
    exec(compile(f"def helper(argument):\n    return {helper_value!r}\n\n"
                 f"def mutate():\n    return helper({argument!r})\n", "<test mutation>",
                 "exec"), namespace)
    return namespace["mutate"]


class Fingerprint(unittest.TestCase):
    """Review E-5: the key that keeps a proof must change when anything that made the verdict
    changes, the mutation's code, the prover's verdict path, and the prose a fault edits,
    which is outside the code commit the key also holds."""

    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)

    def test_a_mutation_with_another_body_has_another_fingerprint(self):
        a = prove.Fault("example", "render-check", "x", 1, "s", mutation_named_alike("a"))
        b = prove.Fault("example", "render-check", "x", 1, "s", mutation_named_alike("b"))
        self.assertEqual(a.mutate.__name__, b.mutate.__name__)
        self.assertNotEqual(prove.fingerprint(a), prove.fingerprint(b))

    def test_a_mutation_whose_helper_changed_has_another_fingerprint(self):
        a = prove.Fault("example", "render-check", "x", 1, "s", mutation_named_alike("a", "one"))
        b = prove.Fault("example", "render-check", "x", 1, "s", mutation_named_alike("a", "two"))
        self.assertNotEqual(prove.fingerprint(a), prove.fingerprint(b))

    def test_an_edited_verdict_changes_every_fingerprint(self):
        before = prove.fingerprint(FAULT)
        original = prove.judge
        self.addCleanup(setattr, prove, "judge", original)
        # An edit to judge() in the script itself: a new body in the script's own globals.
        exec("def judge(fault, result):\n    return 'PROVEN'\n", vars(prove))
        self.assertNotEqual(prove.fingerprint(FAULT), before)

    def test_the_prose_a_patch_edits_is_in_its_fingerprint(self):
        patch = self.root / "example.patch"
        patch.write_text("diff --git a/README.md b/README.md\n--- a/README.md\n"
                         "+++ b/README.md\n@@ -1 +1 @@\n-a\n+b\n")
        original = prove.FAULTS_DIR
        self.addCleanup(setattr, prove, "FAULTS_DIR", original)
        prove.FAULTS_DIR = self.root
        fault = prove.Fault("example", "render-check", "x", 1, "s")
        one = prove.fingerprint(fault, blob_of={"README.md": "1" * 40}.get)
        two = prove.fingerprint(fault, blob_of={"README.md": "2" * 40}.get)
        self.assertNotEqual(one, two)

    def test_the_prose_a_mutation_edits_is_in_its_fingerprint(self):
        fault = next(fault for fault in prove.FAULTS if fault.id == "readme-pitch-figure-edited")
        one = prove.fingerprint(fault, blob_of={"README.md": "1" * 40}.get)
        two = prove.fingerprint(fault, blob_of={"README.md": "2" * 40}.get)
        self.assertNotEqual(one, two)

    def test_prose_a_fault_does_not_touch_leaves_it_alone(self):
        one = prove.fingerprint(FAULT, blob_of={"README.md": "1" * 40}.get)
        two = prove.fingerprint(FAULT, blob_of={"README.md": "2" * 40}.get)
        self.assertEqual(one, two)


class EveryFaultApplies(unittest.TestCase):
    """Each listed fault breaks what it says on the committed tree: its patch applies, its
    mutation changes a file. Three patches once stopped applying, and nothing saw it until a
    full re-proof reported them."""

    @classmethod
    def setUpClass(cls):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        cls.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        cls.tree = cls.root / "tree"
        subprocess.run(["git", "clone", "--quiet", "--shared", str(REPO_ROOT), str(cls.tree)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root)

    def git(self, *arguments, root=None):
        return subprocess.run(["git", "-C", str(root or self.tree), *arguments],
                              capture_output=True, text=True, check=False)

    def test_every_listed_patch_applies_to_the_committed_tree(self):
        # A fetched source is outside git; the prover copies it into its clone, so it is
        # checked where it was fetched, when it was.
        fetched = (REPO_ROOT / "build" / "_deps").is_dir()
        for fault in prove.FAULTS:
            patch = prove.patch_of(fault)
            if patch is None:
                continue
            with self.subTest(fault=fault.id):
                done = self.git("apply", "--check", "--whitespace=nowarn",
                                "--exclude=build/_deps/*", str(patch))
                self.assertEqual(done.returncode, 0, done.stderr)
                if fetched and "build/_deps/" in patch.read_text():
                    done = self.git("apply", "--check", "--whitespace=nowarn",
                                    "--include=build/_deps/*", str(patch), root=REPO_ROOT)
                    self.assertEqual(done.returncode, 0, done.stderr)

    def test_every_listed_mutation_changes_the_committed_tree(self):
        original = prove.TREE
        self.addCleanup(setattr, prove, "TREE", original)
        prove.TREE = self.tree
        for fault in prove.FAULTS:
            if fault.mutate is None or prove.patch_of(fault) is not None:
                continue
            with self.subTest(fault=fault.id):
                try:
                    fault.mutate()
                    changed = self.git("status", "--porcelain").stdout
                finally:
                    self.git("reset", "--quiet", "--hard")
                    self.git("clean", "--quiet", "-fd")
                self.assertTrue(changed.strip(), f"{fault.mutate.__name__} changed nothing")

    def test_every_fault_breaks_something(self):
        for fault in prove.FAULTS:
            with self.subTest(fault=fault.id):
                self.assertTrue(prove.patch_of(fault) or fault.mutate or fault.fault_arguments)


class MacosResults(unittest.TestCase):
    """The faults that edit a macOS results file find it by pattern, since its name carries the
    commit it was measured at and changes on every re-measure."""

    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        (self.root / "results" / "bench").mkdir(parents=True)

    def add(self, name):
        (self.root / "results" / "bench" / name).write_text("{}")

    def test_the_one_file_is_found_whatever_its_commit(self):
        self.add("macOS-itch_codec-0123456789ab.json")
        self.add("Linux-itch_codec-0123456789ab.json")
        found = prove.macos_results("bench", self.root)
        self.assertEqual(found.name, "macOS-itch_codec-0123456789ab.json")

    def test_no_file_is_a_clear_stop(self):
        self.add("Linux-itch_codec-0123456789ab.json")
        with self.assertRaisesRegex(SystemExit, "exactly one macOS-.*has 0"):
            prove.macos_results("bench", self.root)

    def test_two_files_are_a_clear_stop(self):
        self.add("macOS-itch_codec-0123456789ab.json")
        self.add("macOS-itch_codec-ba9876543210.json")
        with self.assertRaisesRegex(SystemExit, "exactly one macOS-.*has 2"):
            prove.macos_results("bench", self.root)

    def test_other_benchmarks_beside_it_do_not_count(self):
        self.add("macOS-itch_codec-0123456789ab.json")
        self.add("macOS-simd_kernel-0123456789ab.json")
        self.add("macOS-column_decode-0123456789ab.json")
        found = prove.macos_results("bench", self.root, benchmark="itch_codec")
        self.assertEqual(found.name, "macOS-itch_codec-0123456789ab.json")
        with self.assertRaisesRegex(SystemExit, "exactly one macOS-\\*.json.*has 3"):
            prove.macos_results("bench", self.root)

    def test_the_committed_tree_has_one_of_each(self):
        bench = prove.BENCH_FAULT_BENCHMARK
        self.assertTrue(prove.macos_results("bench", REPO_ROOT, bench).is_file())
        self.assertTrue(prove.linux_results("bench", REPO_ROOT, bench).is_file())
        self.assertTrue(prove.macos_results("compare", REPO_ROOT,
                                            prove.COMPARE_FAULT_SLICE).is_file())


if __name__ == "__main__":
    unittest.main()
