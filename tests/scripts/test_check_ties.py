#!/usr/bin/env python3
"""Tests for scripts/check_ties.py. Run with:

    python3 -I tests/scripts/test_check_ties.py

Each test runs the check on a small git repository of its own, under the repo's gitignored
build/ folder: one file in every place a kind of results file measures, and one results file
of each kind naming the commit it was measured at. The paths each kind must measure are written
out here, apart from the script's table, so a kind or a path dropped from the table fails a
test: a change in each measured path fails the files of every kind that measures it and names
it, a change elsewhere passes, a pinned file is reported and not judged, and a file that names
no commit, a commit the clone lacks and a tree with no history each get their own verdict.
"""

import contextlib
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_ties.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_ties"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_ties  # noqa: E402

# One file for each path a kind measures or is assembled by, and some no kind does.
FILE_OF = {
    "src": "src/codec/pfor.cpp", "include": "include/tick_store/pfor.hpp",
    "cmake": "cmake/Warnings.cmake", "CMakeLists.txt": "CMakeLists.txt",
    "CMakePresets.json": "CMakePresets.json", "tools": "tools/tick_roundtrip.cpp",
    "bench": "bench/itch_codec_bench.cpp", "compare": "compare/run.py",
    "tests/unit": "tests/unit/pfor_test.cpp", "fuzz": "fuzz/fuzz_pfor.cpp",
    "scripts/run_bench.py": "scripts/run_bench.py",
    "scripts/store_results.py": "scripts/store_results.py",
    "scripts/build_preset.py": "scripts/build_preset.py",
    "scripts/coverage.sh": "scripts/coverage.sh",
    "scripts/coverage_report.py": "scripts/coverage_report.py",
    "scripts/fuzz_g3.py": "scripts/fuzz_g3.py",
    "scripts/codec_eval_results.py": "scripts/codec_eval_results.py",
    "scripts/store_estimate.py": "scripts/store_estimate.py",
    "scripts/diag_summary.py": "scripts/diag_summary.py",
    "scripts/fuzz_results.py": "scripts/fuzz_results.py",
    "scripts/fuzz_host.py": "scripts/fuzz_host.py",
}
# Files no kind measures: prose, faults, the prover, and what prove_checks.py leaves out of its
# key; then scripts and tests that are code to the prover alone.
PROSE = ("docs/NOTES.md", "compare/README.md", "fuzz/README.md", "tools/README.md",
         "faults/a-fault.patch", "scripts/prove_checks.py")
PROVER_ONLY = ("scripts/g2_plan.py", "scripts/check_results.py", "scripts/check_ties.py",
               "scripts/fetch_data.py", "scripts/render_results.py", "tests/scripts/test_x.py",
               "tests/cmake/flags_test.cmake", ".github/workflows/ci.yml")
PINS = "scripts/data_pins.json"
INPUT_PINS = {"days": {"12302019": {"message_count": 268744780}},
              "slices": {"12302019-open": {"sha256": "6ef6"}}}

BUILT = {"src", "include", "cmake", "CMakeLists.txt", "CMakePresets.json"}
# What each kind measures, from what its producing scripts build, run and read; and "pins" for
# the input pins of the pins file. Written out here, not read from the script.
MEASURED = {
    "bench": BUILT | {"bench", "scripts/run_bench.py", "pins"},
    "compare": BUILT | {"tools", "compare", "scripts/run_bench.py", "scripts/store_results.py",
                        "scripts/build_preset.py", "pins"},
    "capacity": BUILT | {"tools", "compare", "pins"},
    "census": BUILT | {"tools", "pins"},
    "codec-eval": BUILT | {"tools", "pins"},
    "roundtrip": BUILT | {"tools", "pins"},
    "store-size": BUILT | {"tools", "pins"},
    "store-estimate": BUILT | {"tools", "pins"},
    "coverage": BUILT | {"tests/unit", "fuzz", "scripts/coverage.sh",
                         "scripts/coverage_report.py"},
    "diag": BUILT | {"tools", "bench", "scripts/run_bench.py", "pins"},
    "fuzz": BUILT | {"fuzz", "scripts/fuzz_g3.py"},
}
ASSEMBLED = {
    "codec-eval": {"scripts/codec_eval_results.py", "scripts/build_preset.py",
                   "scripts/store_results.py"},
    "roundtrip": {"scripts/store_results.py", "scripts/build_preset.py"},
    "store-size": {"scripts/store_results.py", "scripts/build_preset.py",
                   "results/fullday-capacity-12302019.json"},
    "store-estimate": {"scripts/store_estimate.py", "results/codec-eval-12302019.json",
                       "results/fullday-capacity-12302019.json"},
    "diag": {"scripts/diag_summary.py"},
    "fuzz": {"scripts/fuzz_results.py", "scripts/fuzz_host.py"},
}


def results_of(commit):
    """One results file of each kind, by its path under results/, naming the commit as files
    of that kind do."""
    return {
        "bench": ("bench/macOS-itch_codec-x.json", {"header": {"commit": commit}}),
        "compare": ("compare/Linux-12302019-full-x.json",
                    {"header": {"commit": commit}, "variant_selection": {
                        "source_commit": commit}}),
        "capacity": ("fullday-capacity-12302019.json",
                     {"systems": {"parquet": {"commit": commit},
                                  "questdb": {"commit": commit}}}),
        "census": ("census-12302019.json", {"commit": commit[:7]}),
        "codec-eval": ("codec-eval-12302019.json", {"commit": commit[:12]}),
        "roundtrip": ("roundtrip-10302019.json", {"commit": commit[:7]}),
        "store-size": ("store-size-12302019.json", {"commit": commit[:7]}),
        "store-estimate": ("store-estimate.json", {"days": [{"commit": commit[:12]}]}),
        "coverage": ("coverage.json", {"commit": commit[:12]}),
        "diag": ("diag/writer-profile-12302019-x.json", {"commit": commit}),
        "fuzz": ("fuzz-g3-ea1dde4c02e4.json", {"header": {"commit": commit},
                                              "runs": [{"commit": commit}]}),
    }


def git(repo, *arguments):
    return subprocess.run(["git", "-C", str(repo), "-c", "user.name=test", "-c",
                           "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
                           *arguments], capture_output=True, text=True, check=True).stdout.strip()


def write(root, name, text):
    (root / name).parent.mkdir(parents=True, exist_ok=True)
    (root / name).write_text(text, encoding="utf-8")


def write_json(root, name, document):
    write(root, name, json.dumps(document, indent=1) + "\n")


def make_repo(root):
    """The code at CODE; the results naming it at RESULTS; the proofs naming RESULTS, the last
    commit that changed code as prove_checks.py keys it, committed last."""
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    git(root, "init", "-q")
    for name in (*FILE_OF.values(), *PROSE, *PROVER_ONLY):
        write(root, name, "first\n")
    write_json(root, PINS, INPUT_PINS)
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "the code measured")
    code = git(root, "rev-parse", "HEAD")
    for name, document in results_of(code).values():
        write_json(root, f"results/{name}", document)
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "the results")
    results = git(root, "rev-parse", "HEAD")
    write_json(root, "results/check-proofs.json", {"source_commit": results[:12]})
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "the proofs")
    return code, results, git(root, "rev-parse", "HEAD")


SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
SCRATCH = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
REPO = SCRATCH / "repo"
CODE, RESULTS, PROOFS = make_repo(REPO)
FILES = {kind: f"results/{name}" for kind, (name, _) in results_of(CODE).items()}
FILES["proofs"] = "results/check-proofs.json"


def tearDownModule():
    shutil.rmtree(SCRATCH, ignore_errors=True)


class Run:
    def __init__(self, code, out, err):
        self.code, self.out, self.err = code, out, err

    def lines_of(self, name):
        """What the check said of one file, by its path under results/."""
        prefix = name.removeprefix("results/") + ": "
        return [line for line in (self.out + self.err).splitlines() if line.startswith(prefix)]

    def said(self, name):
        return "\n".join(self.lines_of(name))


def run_check(repo=REPO, *arguments):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = check_ties.main(["--repo", str(repo), *arguments])
    return Run(code, out.getvalue(), err.getvalue())


class TieTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH))
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.addCleanup(self.restore)

    def restore(self):
        git(REPO, "reset", "-q", "--hard", PROOFS)
        git(REPO, "clean", "-q", "-fdx")

    def change(self, name, commit=True):
        with (REPO / name).open("a", encoding="utf-8") as file:
            file.write("changed\n")
        if commit:
            git(REPO, "commit", "-q", "-am", f"change {name}")

    def change_pins(self, key, value):
        document = json.loads((REPO / PINS).read_text(encoding="utf-8"))
        document[key] = value
        write_json(REPO, PINS, document)
        git(REPO, "commit", "-q", "-am", f"change the pins' {key}")

    def assert_same(self, result, kind):
        self.assertIn("ok, nothing it measures changed since", result.said(FILES[kind]), kind)

    def assert_measured_change(self, result, kind, path):
        said = result.said(FILES[kind])
        self.assertIn("what it measures changed since", said, kind)
        self.assertIn(path, said, kind)


class SameContent(TieTest):
    def test_unchanged_code_passes_for_every_kind(self):
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_OK, result.err)
        for kind in FILES:
            with self.subTest(kind=kind):
                self.assert_same(result, kind)
        self.assertIn("check_ties: 12 files: 12 same, 0 pinned, 0 differs", result.out)

    def test_a_short_commit_is_resolved(self):
        result = run_check()
        self.assertIn(f"changed since {CODE[:7]} (census)", result.said(FILES["census"]))

    def test_the_command_line_exits_with_the_verdict(self):
        done = subprocess.run([sys.executable, "-I", str(SCRIPT), "--repo", str(REPO)],
                              capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.change(FILE_OF["src"])
        done = subprocess.run([sys.executable, "-I", str(SCRIPT), "--repo", str(REPO)],
                              capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, check_ties.EXIT_REFUSED, done.stdout)
        self.assertIn(FILE_OF["src"], done.stderr)


class MeasuredChange(TieTest):
    def test_a_change_in_each_measured_path_fails_every_kind_that_measures_it(self):
        paths = sorted(set().union(*MEASURED.values()) - {"pins"})
        for path in paths:
            with self.subTest(path=path):
                self.change(FILE_OF[path])
                result = run_check()
                self.restore()
                self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
                for kind, measured in MEASURED.items():
                    said = result.said(FILES[kind])
                    if path in measured:
                        self.assert_measured_change(result, kind, FILE_OF[path])
                        self.assertIn(f"measured at {CODE[:7]}", said)
                    else:
                        # A script one kind runs may assemble another: that is the other tier.
                        self.assertNotIn("what it measures changed", said, kind)
                # The prover's key is all code: every change of code is one.
                self.assert_measured_change(result, "proofs", FILE_OF[path])

    def test_a_change_in_the_input_pins_fails_every_kind_on_real_data(self):
        for key in ("days", "slices"):
            with self.subTest(key=key):
                self.change_pins(key, {"another": {"sha256": "0000"}})
                result = run_check()
                self.restore()
                self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
                for kind, measured in MEASURED.items():
                    if "pins" in measured:
                        self.assert_measured_change(result, kind, f"{PINS} ({key})")
                    else:
                        self.assert_same(result, kind)

    def test_an_uncommitted_or_new_measured_file_fails(self):
        self.change(FILE_OF["src"], commit=False)
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assert_measured_change(result, "bench", FILE_OF["src"])
        self.restore()
        write(REPO, "tools/tick_new.cpp", "int h();\n")
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assert_measured_change(result, "census", "tools/tick_new.cpp")
        self.assert_same(result, "bench")

    def test_the_file_the_commit_and_the_paths_are_named(self):
        self.change(FILE_OF["bench"])
        result = run_check()
        self.assertIn(f"bench/macOS-itch_codec-x.json: measured at {CODE[:12]}, and what it "
                      f"measures changed since: {FILE_OF['bench']}. Measure it again at this "
                      "code (bench)", result.err)

    def test_every_commit_a_file_names_is_judged(self):
        other = git(REPO, "rev-parse", "HEAD")
        name, document = results_of(CODE)["capacity"]
        document["systems"]["questdb"]["commit"] = other
        write_json(REPO, f"results/{name}", document)
        git(REPO, "commit", "-q", "-am", "a second system measured later")
        self.change(FILE_OF["compare"])
        result = run_check()
        said = result.said(FILES["capacity"])
        self.assertIn(f"measured at {CODE[:12]}", said)
        self.assertIn(f"measured at {other[:12]}", said)


class AssembledChange(TieTest):
    def test_a_change_to_what_assembled_a_file_after_it_was_written_fails_it(self):
        for path in sorted(set().union(*ASSEMBLED.values())):
            with self.subTest(path=path):
                if path.startswith("results/"):
                    document = json.loads((REPO / path).read_text(encoding="utf-8"))
                    document["measured_again"] = True
                    write_json(REPO, path, document)
                    git(REPO, "commit", "-q", "-am", f"change {path}")
                else:
                    self.change(FILE_OF[path])
                result = run_check()
                self.restore()
                self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
                for kind, assembled in ASSEMBLED.items():
                    said = result.said(FILES[kind])
                    if path in assembled:
                        self.assertIn(f"written at {RESULTS[:12]}, and what assembled it "
                                      f"changed since: {path}", said, kind)
                    else:
                        self.assertNotIn("what assembled it changed", said, kind)

    def test_an_assembler_changed_before_the_file_was_written_passes(self):
        # diag_summary.py did not exist at the freeze commit the diagnostic files name.
        git(REPO, "reset", "-q", "--hard", CODE)
        self.change(FILE_OF["scripts/diag_summary.py"])
        name, document = results_of(CODE)["diag"]
        write_json(REPO, f"results/{name}", document)
        git(REPO, "add", "-A")
        git(REPO, "commit", "-q", "-m", "the diagnostic file")
        result = run_check()
        self.assert_same(result, "diag")

    def test_a_file_being_written_now_is_assembled_from_the_tree(self):
        self.change(FILE_OF["scripts/diag_summary.py"], commit=False)
        name, document = results_of(CODE)["diag"]
        document["again"] = True
        write_json(REPO, f"results/{name}", document)
        result = run_check()
        self.assert_same(result, "diag")


class ChangeElsewhere(TieTest):
    def test_prose_faults_and_the_prover_change_no_figure(self):
        for name in PROSE:
            self.change(name)
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_OK, result.err)
        for kind in FILES:
            self.assert_same(result, kind)

    def test_scripts_and_tests_no_measurement_runs_untie_only_the_proofs(self):
        for name in PROVER_ONLY:
            with self.subTest(name=name):
                self.change(name)
                result = run_check()
                self.restore()
                self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
                for kind in MEASURED:
                    self.assert_same(result, kind)
                self.assert_measured_change(result, "proofs", name)

    def test_the_g2_planner_does_not_untie_the_round_trip_file(self):
        self.change("scripts/g2_plan.py")
        result = run_check()
        self.assert_same(result, "roundtrip")
        self.assert_same(result, "store-size")

    def test_the_exemption_pins_change_no_input(self):
        self.change_pins("roundtrip_files_before_exit_codes", {"roundtrip-10302019.json": "00"})
        result = run_check()
        for kind in MEASURED:
            self.assert_same(result, kind)


class Pinned(TieTest):
    def pins_for(self, *kinds, names=None):
        pins = {}
        keys = {"compare": "protocol_1_comparison_files", "fuzz": "fuzz_g3_file_before_the_tie",
                "roundtrip": "roundtrip_files_before_exit_codes"}
        for kind in kinds:
            path = REPO / FILES[kind]
            name = (names or {}).get(kind, path.name)
            pins.setdefault(keys[kind], {})[name] = hashlib.sha256(
                path.read_bytes()).hexdigest()
        target = self.dir / "pins.json"
        write_json(self.dir, "pins.json", pins)
        return target

    def test_a_pinned_file_is_reported_and_not_judged(self):
        pins = self.pins_for("compare", "fuzz", "roundtrip")
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        for kind, key in (("compare", "protocol_1_comparison_files"),
                          ("fuzz", "fuzz_g3_file_before_the_tie"),
                          ("roundtrip", "roundtrip_files_before_exit_codes")):
            with self.subTest(kind=kind):
                said = result.said(FILES[kind])
                self.assertIn(f"pinned under {key} in the pins file: measured at", said)
                self.assertIn("1 measured paths changed since; not judged", said)
                self.assertIn(said, result.out)
        # The others are judged, and refused.
        self.assertEqual(result.code, check_ties.EXIT_REFUSED)
        self.assert_measured_change(result, "store-size", FILE_OF["src"])
        self.assertIn("9 differs", result.out)
        self.assertIn("3 pinned", result.out)

    def test_pinned_files_alone_pass(self):
        pins = self.pins_for("compare", "fuzz", "roundtrip")
        result = run_check(REPO, "--pins", str(pins))
        self.assertEqual(result.code, check_ties.EXIT_OK, result.err)

    def test_a_pin_of_other_bytes_does_not_hold(self):
        pins = self.pins_for("roundtrip")
        document = json.loads((REPO / FILES["roundtrip"]).read_text(encoding="utf-8"))
        document["edited"] = True
        write_json(REPO, FILES["roundtrip"], document)
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        self.assertNotIn("pinned", result.said(FILES["roundtrip"]))
        self.assert_measured_change(result, "roundtrip", FILE_OF["src"])

    def test_a_pin_only_its_validator_allows_holds(self):
        # Only the three round-trip files from before the exit codes may be pinned there, and
        # only G-3's file from before the tie, so a pin of another name is no pin.
        name, document = results_of(CODE)["roundtrip"]
        write_json(REPO, "results/roundtrip-20202020.json", document)
        git(REPO, "add", "-A")
        git(REPO, "commit", "-q", "-m", "another day")
        pins = self.dir / "pins.json"
        write_json(self.dir, "pins.json", {"roundtrip_files_before_exit_codes": {
            "roundtrip-20202020.json": hashlib.sha256(
                (REPO / "results/roundtrip-20202020.json").read_bytes()).hexdigest()}})
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        self.assertNotIn("pinned", result.said("results/roundtrip-20202020.json"))
        self.assert_measured_change(result, "roundtrip", FILE_OF["src"])

    def test_a_pin_holds_only_for_its_own_kind(self):
        pins = self.dir / "pins.json"
        write_json(self.dir, "pins.json", {"protocol_1_comparison_files": {
            "store-size-12302019.json": hashlib.sha256(
                (REPO / FILES["store-size"]).read_bytes()).hexdigest()}})
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        self.assertNotIn("pinned", result.said(FILES["store-size"]))


class NoCommit(TieTest):
    def test_a_file_that_names_no_commit_fails(self):
        write_json(REPO, "results/census-10302019.json", {"day": "10302019"})
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn("census-10302019.json: names no commit it was measured at", result.err)
        self.assertIn("1 no commit", result.out)

    def test_a_commit_git_cannot_read_fails(self):
        write_json(REPO, "results/census-10302019.json", {"commit": "unknown"})
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn("census-10302019.json: commit is 'unknown', not a commit", result.err)

    def test_a_file_of_no_kind_fails(self):
        write_json(REPO, "results/latency-12302019.json", {"commit": CODE})
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn("latency-12302019.json: no kind in check_ties.py's KINDS", result.err)


class MissingCommit(TieTest):
    MADE_UP = "0123456789ab" + "0" * 28

    def test_a_commit_a_full_clone_lacks_fails(self):
        write_json(REPO, FILES["bench"], {"header": {"commit": self.MADE_UP}})
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn(f"{self.MADE_UP[:12]} is in no commit of this checkout's full history",
                      result.said(FILES["bench"]))

    def test_a_commit_a_shallow_clone_lacks_is_not_judged(self):
        shallow = self.dir / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth", "1", REPO.as_uri(), str(shallow)],
                       check=True, capture_output=True)
        result = run_check(shallow)
        self.assertEqual(result.code, check_ties.EXIT_UNJUDGED, result.out + result.err)
        self.assertIn(f"not judged: {CODE[:12]} is not in this checkout's history, as in a "
                      "shallow clone", result.said(FILES["bench"]))
        self.assertNotIn("ok", result.said(FILES["bench"]))

    def test_a_refusal_beside_a_shallow_clone_still_fails(self):
        shallow = self.dir / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth", "1", REPO.as_uri(), str(shallow)],
                       check=True, capture_output=True)
        write_json(shallow, "results/census-10302019.json", {"day": "10302019"})
        result = run_check(shallow)
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)


class NoHistory(TieTest):
    def exported(self):
        exported = self.dir / "exported"
        shutil.copytree(REPO, exported, ignore=shutil.ignore_patterns(".git"))
        (exported / "COMMIT").write_text(PROOFS + "\n", encoding="utf-8")
        return exported

    def test_an_exported_tree_is_not_judged_and_says_so(self):
        result = run_check(self.exported())
        self.assertEqual(result.code, check_ties.EXIT_UNJUDGED, result.out + result.err)
        for kind in FILES:
            with self.subTest(kind=kind):
                said = result.said(FILES[kind])
                self.assertIn("not judged: no history here: measured at", said)
                self.assertNotIn("ok", said)
        self.assertIn("check_ties: not judged: no history here: exported is not a git checkout",
                      result.err)
        self.assertIn("12 unjudged", result.out)

    def test_a_pinned_file_is_still_reported_pinned(self):
        tree = self.exported()
        pins = tree / "exemptions.json"
        write_json(tree, "exemptions.json", {"fuzz_g3_file_before_the_tie": {
            "fuzz-g3-ea1dde4c02e4.json": hashlib.sha256(
                (tree / FILES["fuzz"]).read_bytes()).hexdigest()}})
        result = run_check(tree, "--pins", str(pins))
        self.assertIn("pinned under fuzz_g3_file_before_the_tie", result.said(FILES["fuzz"]))

    def test_a_file_with_no_commit_fails_there_too(self):
        tree = self.exported()
        write_json(tree, "results/census-10302019.json", {"day": "10302019"})
        result = run_check(tree)
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)


class Repository(unittest.TestCase):
    def test_every_committed_results_file_has_a_kind_and_names_a_commit(self):
        for path in sorted((REPO_ROOT / "results").rglob("*.json")):
            name = path.relative_to(REPO_ROOT / "results").as_posix()
            with self.subTest(name=name):
                self.assertIsNotNone(check_ties.kind_of(name))
                self.assertTrue(check_ties.commits_named(
                    json.loads(path.read_text(encoding="utf-8"))))

    def test_the_table_has_a_reason_for_every_path(self):
        for kind in check_ties.KINDS:
            for path, why in (*kind.measured, *kind.assembled, *kind.excluded, *kind.left_out):
                with self.subTest(kind=kind.name, path=path):
                    self.assertTrue(path and len(why) > 20)

    def test_the_fuzz_kind_is_check_fuzz_s_tie(self):
        import check_fuzz
        fuzz = next(kind for kind in check_ties.KINDS if kind.name == "fuzz")
        self.assertEqual(check_ties.pathspecs(fuzz.measured, fuzz.excluded),
                         (*check_fuzz.TIED_PATHS, *check_fuzz.NOT_TIED))


if __name__ == "__main__":
    unittest.main()
