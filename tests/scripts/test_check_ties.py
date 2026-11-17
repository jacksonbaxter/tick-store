#!/usr/bin/env python3
"""Tests for scripts/check_ties.py. Run with:

    python3 -I tests/scripts/test_check_ties.py

Each test runs the check on a small git repository of its own, under the repo's gitignored
build/ folder: one file in every place a kind of results file measures, and one results file
of each kind naming the commit it was measured at. The paths each kind must measure are written
out here, apart from the script's table, so a kind or a path dropped from the table fails a
test: a change in each measured path fails the files of every kind that measures it and names
it, a change elsewhere passes, a pinned file is reported and not judged, and a file that names
no commit, a commit the clone lacks and a tree with no history each get their own verdict. A
refusal's exit code is shown on a tree whose proofs are brought up to date, so that it is the
refusal's own and not the proofs file's, which any uncommitted results file unties.
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
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_ties.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_ties"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_ties  # noqa: E402
import render_results  # noqa: E402

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
    "scripts/check_fuzz.py": "scripts/check_fuzz.py",
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
    "disturbances": set(),
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
    "fuzz": {"scripts/fuzz_results.py", "scripts/fuzz_host.py", "scripts/build_preset.py",
             "scripts/check_fuzz.py"},
}
# The exact paths under results/ each validator's pin may name, written out here, apart from the
# script's PIN_NAMES.
PIN_NAMES = {
    "protocol_1_comparison_files": (),
    "fuzz_g3_file_before_the_tie": ("fuzz-g3-ea1dde4c02e4.json",),
}


def results_of(commit):
    """One results file of each kind, by its path under results/, naming the commit as files
    of that kind do."""
    return {
        "bench": ("bench/macOS-itch_codec-x.json", {"header": {"commit": commit}}),
        "compare": ("compare/Linux-12302019-full-67ae574f5666.json",
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
        "disturbances": ("disturbances-ede0176b10bb.json", {"commit": commit[:12]}),
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
        said = result.said(FILES[kind])
        if MEASURED.get(kind) == set() and kind not in ASSEMBLED:
            self.assertIn("ok, names", said, kind)
        else:
            self.assertIn("ok, nothing it measures changed since", said, kind)

    def commit_with_proofs(self, message):
        """Commits the tree, then the proofs naming that commit, as prove_checks.py keys them,
        so that only the file under test can untie anything."""
        git(REPO, "add", "-A")
        git(REPO, "commit", "-q", "-m", message)
        head = git(REPO, "rev-parse", "HEAD")
        write_json(REPO, "results/check-proofs.json", {"source_commit": head[:12]})
        git(REPO, "commit", "-q", "-am", "the proofs again")

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
        self.assertIn("check_ties: 13 files: 13 same, 0 pinned, 0 differs", result.out)

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

    def test_a_file_not_committed_as_it_is_is_not_judged_against_what_assembled_it(self):
        # Review TD-2: no commit wrote it as it is, so there is nothing to compare from.
        self.change(FILE_OF["scripts/diag_summary.py"], commit=False)
        name, document = results_of(CODE)["diag"]
        document["again"] = True
        write_json(REPO, f"results/{name}", document)
        write_json(REPO, "results/store-size-20202020.json", {"commit": CODE[:7]})
        result = run_check()
        for said in (result.said(FILES["diag"]),
                     result.said("results/store-size-20202020.json")):
            self.assertIn("not judged against what assembled it: commit it first", said)
            self.assertNotIn("ok", said)
        self.assertIn("2 unjudged", result.out)
        # A kind with nothing assembled is judged as it is.
        write_json(REPO, "results/census-20202020.json", {"commit": CODE[:7]})
        self.assertIn("ok, nothing it measures changed since",
                      run_check().said("results/census-20202020.json"))

    def test_an_uncommitted_results_file_is_no_pass(self):
        # Not judged itself, and it unties the proofs, whose key holds every results file but
        # the proofs and the coverage summary: the step fails, as it already did.
        path = REPO / FILES["roundtrip"]
        path.write_text(path.read_text(encoding="utf-8") + " ", encoding="utf-8")
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out + result.err)
        self.assertIn("not judged against what assembled it: commit it first",
                      result.said(FILES["roundtrip"]))
        self.assert_measured_change(result, "proofs", FILES["roundtrip"])


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
        self.change_pins("fuzz_g3_file_before_the_tie", {"fuzz-g3-ea1dde4c02e4.json": "00"})
        result = run_check()
        for kind in MEASURED:
            self.assert_same(result, kind)


class Pinned(TieTest):
    def pins_for(self, *kinds, names=None):
        pins = {}
        keys = {"compare": "protocol_1_comparison_files", "fuzz": "fuzz_g3_file_before_the_tie"}
        for kind in kinds:
            path = REPO / FILES[kind]
            name = (names or {}).get(kind, path.name)
            pins.setdefault(keys[kind], {})[name] = hashlib.sha256(
                path.read_bytes()).hexdigest()
        target = self.dir / "pins.json"
        write_json(self.dir, "pins.json", pins)
        return target

    def test_a_pinned_file_is_reported_and_not_judged(self):
        pins = self.pins_for("fuzz")
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        for kind, key in (("fuzz", "fuzz_g3_file_before_the_tie"),):
            with self.subTest(kind=kind):
                said = result.said(FILES[kind])
                self.assertIn(f"pinned under {key} in the pins file: measured at", said)
                self.assertIn("1 measured paths changed since; not judged", said)
                self.assertIn(said, result.out)
        # The others are judged, and refused.
        self.assertEqual(result.code, check_ties.EXIT_REFUSED)
        self.assert_measured_change(result, "store-size", FILE_OF["src"])
        self.assertIn("11 differs", result.out)
        self.assertIn("1 pinned", result.out)

    def test_pinned_files_alone_pass(self):
        pins = self.pins_for("fuzz")
        result = run_check(REPO, "--pins", str(pins))
        self.assertEqual(result.code, check_ties.EXIT_OK, result.err)

    def test_a_pin_of_other_bytes_does_not_hold(self):
        pins = self.pins_for("fuzz")
        document = json.loads((REPO / FILES["fuzz"]).read_text(encoding="utf-8"))
        document["edited"] = True
        write_json(REPO, FILES["fuzz"], document)
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        self.assertNotIn("pinned", result.said(FILES["fuzz"]))
        self.assert_measured_change(result, "fuzz", FILE_OF["src"])

    def test_another_file_is_no_pin_and_its_line_is_refused(self):
        # Review TD-4: a validator's key takes any SHA-256 under it, whatever the name, so only
        # the exact paths PIN_NAMES holds may be pinned there.
        source = REPO / FILES["fuzz"]
        other = REPO / "results/fuzz-g3-0000aaaa0000.json"
        other.write_bytes(source.read_bytes() + b" ")
        self.commit_with_proofs("another fuzz file")
        pins = self.pins_for("fuzz")
        document = json.loads(pins.read_text(encoding="utf-8"))
        document["fuzz_g3_file_before_the_tie"][other.name] = hashlib.sha256(
            other.read_bytes()).hexdigest()
        write_json(self.dir, "pins.json", document)
        result = run_check(REPO, "--pins", str(pins))
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn("check_ties: fuzz_g3_file_before_the_tie in the pins file pins "
                      "fuzz-g3-0000aaaa0000.json, and check_ties.py lets only "
                      "fuzz-g3-ea1dde4c02e4.json", result.err)
        self.assertNotIn("pinned", result.said("results/" + other.name))
        self.assertIn("pinned under", result.said(FILES["fuzz"]))

    def test_no_file_may_be_pinned_as_protocol_1(self):
        # The freeze replaced every protocol 1 comparison file, so a line under the comparison
        # validator's key, even for a comparison file's own name and bytes, is refused and pins
        # nothing.
        result = run_check(REPO, "--pins", str(self.pins_for("compare")))
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn("check_ties: protocol_1_comparison_files in the pins file pins "
                      "Linux-12302019-full-67ae574f5666.json, and check_ties.py lets no file be "
                      "pinned there", result.err)
        self.assertNotIn("pinned", result.said(FILES["compare"]))

    def test_a_pin_names_its_file_by_its_path_under_results(self):
        # The same bytes under the same name in another folder are no pinned file.
        copy = REPO / "results/old" / Path(FILES["fuzz"]).name
        copy.parent.mkdir()
        copy.write_bytes((REPO / FILES["fuzz"]).read_bytes())
        self.commit_with_proofs("a copy elsewhere")
        result = run_check(REPO, "--pins", str(self.pins_for("fuzz")))
        self.assertIn("pinned under", result.said(FILES["fuzz"]))
        self.assertNotIn("pinned under", result.said("results/old/" + copy.name))

    def test_the_names_that_may_be_pinned_are_fixed(self):
        self.assertEqual(check_ties.PIN_NAMES, PIN_NAMES)
        self.assertEqual(check_ties.REPLACED_KEPT, (
            ("compare/replaced/Linux-12302019-full-67ae574f5666.json",
             "e8d447073c29d83c2341357df8ca01fa9afaa50d4f235590d5506e0d10a41a25"),))
        # One key for a replaced file, the one render_results.py reads it by.
        self.assertEqual(check_ties.REPLACED_PINS, render_results.REPLACED_PINS)
        self.assertEqual(check_ties.REPLACED_FOLDER,
                         f"compare/{render_results.REPLACED}")
        self.assertEqual(check_ties.NOT_RESULTS, ())

    def replaced(self, kept_at, code_digest=None, pinned_digest=None, listed=None):
        """A comparison file kept for the record at results/<kept_at>, REPLACED_KEPT naming it
        with `code_digest`, and the pins file naming `listed` with `pinned_digest`, each by
        default the path and the SHA-256 of its bytes."""
        kept = REPO / "results" / kept_at
        kept.parent.mkdir(parents=True, exist_ok=True)
        kept.write_bytes((REPO / FILES["compare"]).read_bytes() + b" ")
        self.commit_with_proofs("a replaced file kept")
        digest = hashlib.sha256(kept.read_bytes()).hexdigest()
        write_json(self.dir, "pins.json", {"replaced_comparison_files": {
            Path(listed or kept_at).name: pinned_digest or digest}})
        self.change(FILE_OF["compare"])
        with mock.patch.object(check_ties, "REPLACED_KEPT", ((kept_at, code_digest or digest),)):
            result = run_check(REPO, "--pins", str(self.dir / "pins.json"))
        return result, f"results/{kept_at}"

    KEPT = "compare/replaced/Linux-12302019-full-67ae574f5666.json"

    def test_a_replaced_file_kept_for_the_record_is_pinned_by_path_and_sha256(self):
        result, kept = self.replaced(self.KEPT)
        self.assertIn("pinned under replaced_comparison_files in the pins file",
                      result.said(kept))
        self.assertNotIn("check_ties: replaced", result.err)

    def test_a_replaced_file_is_pinned_only_with_its_own_sha256_in_both_places(self):
        for digests in ({"pinned_digest": "0" * 64}, {"code_digest": "0" * 64}):
            with self.subTest(**digests):
                result, kept = self.replaced(self.KEPT, **digests)
                self.restore()
                self.assertNotIn("pinned", result.said(kept))
                self.assertIn("what it measures changed since", result.said(kept))

    def test_a_replaced_file_outside_a_replaced_folder_is_refused(self):
        result, kept = self.replaced("compare/kept/Linux-12302019-full-67ae574f5666.json")
        self.assertEqual(result.code, check_ties.EXIT_REFUSED)
        self.assertIn("check_ties: REPLACED_KEPT holds compare/kept/Linux-12302019-full-"
                      "67ae574f5666.json: a file kept for the record lives in compare/replaced/",
                      result.err)
        self.assertNotIn("pinned", result.said(kept))
        self.assertIn("what it measures changed since", result.said(kept))

    def test_a_replaced_pin_no_code_names_is_refused(self):
        result, kept = self.replaced(self.KEPT, listed="compare/replaced/another.json")
        self.assertEqual(result.code, check_ties.EXIT_REFUSED)
        self.assertIn("check_ties: replaced_comparison_files in the pins file pins "
                      "another.json", result.err)
        self.assertNotIn("pinned", result.said(kept))

    def test_a_round_trip_file_is_never_pinned(self):
        # The round-trip files from before the exit codes were replaced at the freeze, and
        # their key went with them: a line under it for a file's own name and bytes pins nothing.
        pins = self.dir / "pins.json"
        write_json(self.dir, "pins.json", {"roundtrip_files_before_exit_codes": {
            Path(FILES["roundtrip"]).name: hashlib.sha256(
                (REPO / FILES["roundtrip"]).read_bytes()).hexdigest()}})
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        self.assertNotIn("pinned", result.said(FILES["roundtrip"]))
        self.assert_measured_change(result, "roundtrip", FILE_OF["src"])

    def test_a_pin_holds_only_for_its_own_kind(self):
        pins = self.dir / "pins.json"
        write_json(self.dir, "pins.json", {"fuzz_g3_file_before_the_tie": {
            "store-size-12302019.json": hashlib.sha256(
                (REPO / FILES["store-size"]).read_bytes()).hexdigest()}})
        self.change(FILE_OF["src"])
        result = run_check(REPO, "--pins", str(pins))
        self.assertNotIn("pinned", result.said(FILES["store-size"]))


class NoCommit(TieTest):
    # Review TD-8: each file is committed with the proofs brought up to date, so the exit code
    # is the refusal's own.
    def assert_alone(self, result, outcome):
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn(f"0 differs", result.out)
        self.assertIn(f"1 {outcome}", result.out)

    def test_a_file_that_names_no_commit_fails(self):
        write_json(REPO, "results/census-10302019.json", {"day": "10302019"})
        self.commit_with_proofs("a file naming no commit")
        result = run_check()
        self.assert_alone(result, "no commit")
        self.assertIn("census-10302019.json: names no commit it was measured at", result.err)

    def test_a_commit_git_cannot_read_fails(self):
        for value in ("unknown", "abcd", "0123ab", 7):
            with self.subTest(value=value):
                write_json(REPO, "results/census-10302019.json", {"commit": value})
                self.commit_with_proofs("a file naming no readable commit")
                result = run_check()
                self.restore()
                self.assert_alone(result, "refused")
                self.assertIn(f"census-10302019.json: commit is {value!r}, not a commit",
                              result.err)

    def test_a_file_of_no_kind_fails(self):
        write_json(REPO, "results/latency-12302019.json", {"commit": CODE})
        self.commit_with_proofs("a file of no kind")
        result = run_check()
        self.assert_alone(result, "no kind")
        self.assertIn("latency-12302019.json: no kind in check_ties.py's KINDS", result.err)

    def test_every_file_under_results_is_examined(self):
        # Review TD-3: a results format that is not JSON meets "no kind" too.
        write(REPO, "results/latency-1.csv", f"commit,{'0' * 40}\n")
        write(REPO, "results/census-2.json.gz", "not even gzip")
        write(REPO, "results/notes/whatever.txt", "x")
        self.commit_with_proofs("files that are not JSON")
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        for name in ("latency-1.csv", "census-2.json.gz", "notes/whatever.txt"):
            self.assertIn(f"{name}: no kind in check_ties.py's KINDS", result.err)
        self.assertIn("16 files:", result.out)
        self.assertIn("3 no kind", result.out)

    def test_a_file_not_results_lists_is_named_and_not_judged(self):
        write(REPO, "results/notes/whatever.txt", "x")
        self.commit_with_proofs("a listed file")
        with mock.patch.object(check_ties, "NOT_RESULTS",
                               (("notes/*.txt", "a stand-in for a listed kind of file"),)):
            result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_OK, result.err)
        self.assertIn("notes/whatever.txt: not a results file: a stand-in for a listed kind of "
                      "file", result.out)
        self.assertIn("13 files:", result.out)

    def test_a_file_that_cannot_be_read_fails(self):
        for content in (b"{not json", b'{"commit": "\xff"}'):
            with self.subTest(content=content):
                (REPO / "results/census-10302019.json").write_bytes(content)
                self.commit_with_proofs("an unreadable file")
                result = run_check()
                self.restore()
                self.assert_alone(result, "refused")
                self.assertIn("census-10302019.json: cannot read it:", result.err)

    def test_a_kind_with_no_paths_holds_only_its_commit(self):
        result = run_check()
        self.assertIn("ok, names " + CODE[:12] + ", which HEAD's history holds; nothing "
                      "tracked builds, runs or writes it (disturbances)",
                      result.said(FILES["disturbances"]))
        write_json(REPO, FILES["disturbances"], {"commit": MissingCommit.MADE_UP})
        self.commit_with_proofs("a disturbances file naming a made-up commit")
        self.assert_alone(run_check(), "refused")


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

    def test_a_difference_beside_a_commit_a_shallow_clone_lacks_fails(self):
        # Review TD-8: one commit differs and one the clone lacks; the difference decides.
        shallow = self.dir / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth", "1", REPO.as_uri(), str(shallow)],
                       check=True, capture_output=True)
        head = git(shallow, "rev-parse", "HEAD")
        name, document = results_of(CODE)["capacity"]
        document["systems"]["questdb"]["commit"] = head
        write_json(shallow, f"results/{name}", document)
        with (shallow / FILE_OF["src"]).open("a", encoding="utf-8") as file:
            file.write("changed\n")
        result = run_check(shallow)
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out + result.err)
        said = result.said(FILES["capacity"])
        self.assertIn(f"measured at {head[:12]}, and what it measures changed since", said)
        self.assertNotIn("not judged", said)

    def test_a_commit_not_in_heads_history_fails(self):
        # Review TD-5: a commit of another branch, or one rebased away that this clone still
        # holds, which CI's fresh clone would not.
        git(REPO, "checkout", "-q", "-b", "side")
        self.addCleanup(git, REPO, "branch", "-q", "-D", "side")
        self.change("docs/NOTES.md")
        side = git(REPO, "rev-parse", "HEAD")
        git(REPO, "checkout", "-q", "-")
        write_json(REPO, "results/census-10302019.json", {"commit": side})
        self.commit_with_proofs("a file naming a commit of another branch")
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn(f"census-10302019.json: {side[:12]} is not in HEAD's history", result.err)
        self.assertIn("1 refused", result.out)

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
        self.assertIn("13 unjudged", result.out)

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


class GitErrors(TieTest):
    """Review TD-1 and TD-7: a git command that fails is a refusal naming it, never "nothing
    changed"; only a tree with no repository at all is not judged."""

    def copy(self):
        copy = self.dir / "copy"
        shutil.copytree(REPO, copy, symlinks=True)
        return copy

    def test_a_missing_object_is_a_refusal_naming_the_command(self):
        copy = self.copy()
        with (copy / FILE_OF["scripts/store_results.py"]).open("a", encoding="utf-8") as file:
            file.write("changed\n")
        git(copy, "commit", "-q", "-am", "the assembler changed")
        lost = git(copy, "rev-parse", "HEAD")
        with (copy / "docs/NOTES.md").open("a", encoding="utf-8") as file:
            file.write("later\n")
        git(copy, "commit", "-q", "-am", "later")
        self.assertIn("what assembled it changed since",
                      run_check(copy).said(FILES["roundtrip"]))
        (copy / ".git" / "objects" / lost[:2] / lost[2:]).unlink()
        result = run_check(copy)
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        said = result.said(FILES["roundtrip"])
        self.assertIn("cannot compare: git ", said)
        self.assertIn("exited 128", said)
        self.assertNotIn("ok", said)

    def test_a_git_command_git_refuses_is_a_refusal(self):
        census = next(kind for kind in check_ties.KINDS if kind.name == "census")
        broken = check_ties.Kind(census.name, census.files, (("src", "x"), (":(bogus)x", "y")))
        kinds = tuple(broken if kind is census else kind for kind in check_ties.KINDS)
        with mock.patch.object(check_ties, "KINDS", kinds):
            result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assertIn("cannot compare: git diff --name-only", result.said(FILES["census"]))
        self.assert_same(result, "bench")

    def test_a_git_dir_git_cannot_read_is_refused(self):
        copy = self.copy()
        (copy / ".git" / "HEAD").write_text("garbage\n", encoding="utf-8")
        result = run_check(copy)
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out + result.err)
        self.assertIn("check_ties: cannot judge: copy holds a .git that git does not read",
                      result.err)

    def test_a_folder_inside_a_checkout_is_refused(self):
        result = run_check(REPO / "results", "--results", str(REPO / "results"), "--pins",
                           str(REPO / PINS))
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out + result.err)
        self.assertIn("check_ties: cannot judge: results is a folder inside the checkout repo",
                      result.err)

    def test_a_results_folder_outside_the_checkout_is_refused(self):
        outside = self.dir / "results"
        shutil.copytree(REPO / "results", outside)
        result = run_check(REPO, "--results", str(outside))
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out + result.err)
        self.assertIn("is outside the checkout repo, so git cannot say which commit wrote its "
                      "files", result.err)

    def test_git_that_cannot_be_run_is_a_refusal(self):
        done = subprocess.run([sys.executable, "-I", str(SCRIPT), "--repo", str(REPO)],
                              capture_output=True, text=True, check=False,
                              env={"PATH": str(self.dir)})
        self.assertEqual(done.returncode, check_ties.EXIT_REFUSED, done.stdout)
        self.assertIn("check_ties: cannot judge: git cannot be run", done.stderr)
        self.assertNotIn("Traceback", done.stderr)

    def test_a_symbolic_link_in_a_measured_path_is_refused(self):
        # Review TD-6: the comparison sees the link, not what it points at.
        (REPO / "src/shared.hpp").symlink_to("../docs/NOTES.md")
        git(REPO, "add", "-A")
        git(REPO, "commit", "-q", "-m", "a link")
        write_json(REPO, "results/census-10302019.json", {"commit": git(REPO, "rev-parse",
                                                                         "HEAD")})
        self.commit_with_proofs("a census at the link")
        result = run_check()
        self.assertIn("census-10302019.json: src/shared.hpp: a symbolic link git tracks in what "
                      "it measures", result.err)
        self.assertNotIn("ok", result.said("results/census-10302019.json"))


class Writer(TieTest):
    """The last commit that wrote a file, on what git answers: each rule shown on git's
    answer itself, since a real clone gives these answers only with merges or grafts."""

    def test_a_writer_where_a_shallow_clone_is_cut_is_not_taken(self):
        writer = git(REPO, "log", "-1", "--format=%H", "--", FILES["roundtrip"])
        self.assertEqual(check_ties.written_at(REPO, REPO / FILES["roundtrip"]), (writer, None))
        with mock.patch.object(check_ties, "shallow_cuts", lambda repo: frozenset({writer})):
            base, why = check_ties.written_at(REPO, REPO / FILES["roundtrip"])
        self.assertIsNone(base)
        self.assertIn(f"may be past this shallow clone's history, which is cut at "
                      f"{writer[:12]}; fetch it whole", why)

    def test_no_writer_for_a_tracked_unchanged_file_is_a_refusal(self):
        real = check_ties.git

        def no_log(repo, *arguments):
            done = real(repo, *arguments)
            if arguments[:1] == ("log",):
                done.stdout = ""
            return done
        with mock.patch.object(check_ties, "git", no_log):
            with self.assertRaisesRegex(check_ties.Refusal, "names no commit that wrote it"):
                check_ties.written_at(REPO, REPO / FILES["roundtrip"])


class Usage(TieTest):
    def test_an_empty_or_missing_results_folder_is_a_usage_error(self):
        (self.dir / "empty").mkdir()
        for folder in (self.dir / "empty", self.dir / "missing"):
            with self.subTest(folder=folder.name):
                result = run_check(REPO, "--results", str(folder))
                self.assertEqual(result.code, check_ties.EXIT_USAGE, result.out + result.err)
                self.assertIn("check_ties: no results files in", result.err)

    def test_an_unreadable_pins_file_is_a_usage_error(self):
        write(self.dir, "pins.json", "{not json")
        for pins in (self.dir / "pins.json", self.dir / "missing.json"):
            with self.subTest(pins=pins.name):
                result = run_check(REPO, "--pins", str(pins))
                self.assertEqual(result.code, check_ties.EXIT_USAGE, result.out + result.err)
                self.assertIn("check_ties: cannot read the pins:", result.err)

    def test_an_input_pins_key_missing_fails(self):
        # Removed since the commit, and missing at the commit and now alike: a run on real data
        # verifies its input against a pin that is not there.
        document = json.loads((REPO / PINS).read_text(encoding="utf-8"))
        del document["slices"]
        write_json(REPO, PINS, document)
        git(REPO, "commit", "-q", "-am", "the slices' pins removed")
        result = run_check()
        self.assertEqual(result.code, check_ties.EXIT_REFUSED, result.out)
        self.assert_measured_change(result, "census", f"{PINS} (slices)")
        write_json(REPO, "results/census-10302019.json", {"commit": git(REPO, "rev-parse",
                                                                         "HEAD")})
        self.commit_with_proofs("a census with no slices' pins")
        said = run_check().said("results/census-10302019.json")
        self.assertIn(f"what it measures changed since: {PINS} (slices)", said)


class Repository(unittest.TestCase):
    def test_every_committed_results_file_has_a_kind_and_names_a_commit(self):
        for path in sorted(p for p in (REPO_ROOT / "results").rglob("*") if p.is_file()):
            name = path.relative_to(REPO_ROOT / "results").as_posix()
            with self.subTest(name=name):
                self.assertIsNotNone(check_ties.kind_of(name))
                self.assertTrue(check_ties.commits_named(
                    json.loads(path.read_text(encoding="utf-8"))))

    def test_a_disturbance_file_and_a_replaced_comparison_file_have_kinds(self):
        # Group 3: the record of what else ran is its own kind, measuring no code, and a
        # comparison file a later run replaced, kept under compare/replaced/, is a comparison
        # file, so its protocol 1 pin still holds it.
        kind = check_ties.kind_of("disturbances-ede0176b10bb.json")
        self.assertEqual(getattr(kind, "name", None), "disturbances")
        self.assertEqual(getattr(kind, "measured", None), ())
        self.assertEqual(check_ties.kind_of(
            "compare/replaced/Linux-12302019-full-67ae574f5666.json").name, "compare")

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
