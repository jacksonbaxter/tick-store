#!/usr/bin/env python3
"""Tests for scripts/check_fuzz.py and scripts/fuzz_results.py. Run with:

    python3 -I tests/scripts/test_check_fuzz.py

The validator runs as a subprocess on files generated under the repo's gitignored build/
directory, against a fuzz/CMakeLists.txt written there too. Each rule gets one injected fault
into an otherwise valid file, and the test requires the exit code and the message naming the
rule; the valid file is shown passing first. The assembler is run on raw logs written in
libFuzzer's own format, clean and not. The files judge a commit of a small git repository made
there, so the tie to the code is judged against its history.
"""

import copy
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_fuzz.py"
ASSEMBLER = REPO_ROOT / "scripts" / "fuzz_results.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_fuzz"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_fuzz  # noqa: E402

# A repository with one file in each place the tie names, and some it does not.
CODE = {"src/codec/pfor.cpp": "int f();\n", "include/tick_store/codec/pfor.hpp": "int f();\n",
        "fuzz/fuzz_pfor.cpp": "int g();\n", "fuzz/launch.txt": "flags -timeout=10\n",
        "fuzz/README.md": "# Fuzzing\n", "cmake/Warnings.cmake": "# warnings\n",
        "CMakeLists.txt": "project(x)\n", "CMakePresets.json": "{}\n",
        "scripts/fuzz_g3.py": "# launcher\n", "scripts/check.sh": "# checks\n",
        "docs/NOTES.md": "# Notes\n", "tests/scripts/test_x.py": "# test\n"}
TIED = ("src/codec/pfor.cpp", "include/tick_store/codec/pfor.hpp", "fuzz/fuzz_pfor.cpp",
        "fuzz/launch.txt", "cmake/Warnings.cmake", "CMakeLists.txt", "CMakePresets.json",
        "scripts/fuzz_g3.py")
NOT_TIED = ("fuzz/README.md", "scripts/check.sh", "docs/NOTES.md", "tests/scripts/test_x.py")
REPO = SCRATCH_ROOT / "repo"
PINS = {"fuzz_g3_file_before_the_tie": {}}


def git(repo, *arguments):
    return subprocess.run(["git", "-C", str(repo), "-c", "user.name=test", "-c",
                           "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
                           *arguments], capture_output=True, text=True, check=True).stdout.strip()


def make_repo(root):
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    git(root, "init", "-q")
    for name, text in CODE.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_text(text, encoding="utf-8")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "the code G-3 judged")
    return git(root, "rev-parse", "HEAD")


COMMIT = make_repo(REPO)
NAME = f"fuzz-g3-{COMMIT[:12]}.json"
TARGETS = sorted({"fuzz_columns", "fuzz_itch_message", *check_fuzz.LINUX_REQUIRED})
CMAKE = "".join(f"tick_store_add_fuzzer({name})\n" for name in reversed(TARGETS))

HOST_FACTS = {
    "macOS": {"cpu_model": "Apple M4 Max", "kernel": "Darwin 27.2.0",
              "compiler": "clang version 23.1.4", "compile_flags": "-O2 -g -fsanitize=fuzzer",
              "sanitizer_options": "ASAN_OPTIONS=detect_leaks=1",
              "concurrency": "four at once", "peak_memory": "BSD time -l"},
    "Linux": {"cpu_model": "AMD Ryzen Embedded V1605B", "kernel": "Linux 6.12.116",
              "compiler": "clang version 23.1.4", "compile_flags": "-O2 -g -fsanitize=fuzzer",
              "sanitizer_options": "ASAN_OPTIONS=abort_on_error=0",
              "concurrency": "three at once, one a CPU", "peak_memory": "VmHWM"},
}


def command(target, seconds=1800):
    return (f"ASAN_OPTIONS=abort_on_error=0:exitcode=77 build/fuzz/fuzz/{target} "
            f"-max_total_time={seconds} -max_len=70000 -rss_limit_mb=2048 -malloc_limit_mb=256 "
            "-timeout=10 -print_final_stats=1 -artifact_prefix=build/fuzz/artifacts/ "
            f"build/fuzz/corpus/{target} build/fuzz/fuzz-seeds/{target}")


def clean_run(target, host, level):
    return {
        "target": target, "host": host, "commit": COMMIT, "cpu": "unpinned",
        "kernel_level": level, "seconds": 1801, "wall_seconds": 1803, "runs": 90050,
        "exec_per_second": 50, "coverage": {"cov": 1500, "ft": 7000},
        "corpus": {"units": 900, "size": "2900Kb"}, "peak_rss_mb": 1200, "clean": True,
        "exit_code": 0, "findings": [],
        "final_status_line": "#90050 DONE cov: 1500 ft: 7000 corp: 900/2900Kb lim: 16384 "
                             "exec/s: 50 rss: 1199Mb",
        "done_line": "Done 90050 runs in 1801 second(s)",
        "final_stats": {"number_of_executed_units": 90050, "average_exec_per_sec": 50,
                        "new_units_added": 12, "slowest_unit_time_sec": 0, "peak_rss_mb": 1200},
        "command": command(target),
    }


def valid_document():
    runs = [clean_run(name, "macOS", "neon") for name in TARGETS]
    runs += [clean_run(name, "Linux", "avx2") for name in check_fuzz.LINUX_REQUIRED]
    header = {"schema": check_fuzz.SCHEMA, "gate": "G-3", "commit": COMMIT,
              "statement": "every target for 1,800 s", "required_seconds": 1800,
              "targets_file": "fuzz/CMakeLists.txt", "targets": TARGETS,
              "avx2_path_targets": list(check_fuzz.AVX2_PATH),
              "crc32c_path_targets": list(check_fuzz.CRC32C_PATH),
              "hosts": copy.deepcopy(HOST_FACTS)}
    document = {"header": header, "runs": runs}
    document["summary"] = check_fuzz.summary_of(header, runs)
    return document


def find(document, target, host):
    return next(run for run in document["runs"]
                if run["target"] == target and run["host"] == host)


def resummarise(document):
    document["summary"] = check_fuzz.summary_of(document["header"], document["runs"])


class Scratch(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.dir)
        self.cmake = self.dir / "CMakeLists.txt"
        self.cmake.write_text(CMAKE, encoding="utf-8")


class Checking(Scratch):
    def run_check(self, document, name=NAME, repo=REPO, pins=None):
        path = self.dir / name
        path.write_text(json.dumps(document, indent=1), encoding="utf-8")
        pins_path = self.dir / "pins.json"
        pins_path.write_text(json.dumps(PINS if pins is None else pins), encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--cmake", str(self.cmake),
                               "--repo", str(repo), "--pins", str(pins_path), str(path)],
                              capture_output=True, text=True, check=False)


class CheckFuzzTest(Checking):
    def assert_fails(self, mutate, expected, name=NAME):
        document = valid_document()
        mutate(document)
        result = self.run_check(document, name)
        self.assertEqual(result.returncode, check_fuzz.EXIT_INVALID, result.stdout)
        self.assertIn(expected, result.stderr)

    def test_a_valid_file_passes(self):
        result = self.run_check(valid_document())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ok", result.stdout)

    def test_a_missing_macos_target_is_refused(self):
        def drop(document):
            document["runs"].remove(find(document, "fuzz_columns", "macOS"))
            resummarise(document)
        self.assert_fails(drop, "no run of fuzz_columns on macOS at neon")

    def test_a_missing_avx2_target_on_linux_is_refused(self):
        def drop(document):
            document["runs"].remove(find(document, "fuzz_pfor", "Linux"))
            resummarise(document)
        self.assert_fails(drop, "no run of fuzz_pfor on Linux at avx2")

    def test_a_scalar_run_does_not_stand_in_for_avx2(self):
        def scalar(document):
            find(document, "fuzz_pfor", "Linux")["kernel_level"] = "scalar"
            resummarise(document)
        self.assert_fails(scalar, "no run of fuzz_pfor on Linux at avx2")

    def test_a_clean_run_under_1800_seconds_is_refused(self):
        def short(document):
            run = find(document, "fuzz_itch_message", "macOS")
            run["seconds"] = 1799
            run["done_line"] = "Done 90050 runs in 1799 second(s)"
            resummarise(document)
        self.assert_fails(short, "seconds: 1799 s is under the 1800 s")

    def test_a_short_wall_time_is_refused(self):
        def short(document):
            find(document, "fuzz_itch_message", "macOS")["wall_seconds"] = 1500
        self.assert_fails(short, "wall_seconds: 1500 s is under the 1800 s")

    def test_a_short_budget_in_the_command_is_refused(self):
        def short(document):
            find(document, "fuzz_columns", "macOS")["command"] = command("fuzz_columns", 600)
        self.assert_fails(short, "-max_total_time is not at least 1800")

    def test_a_looser_memory_cap_is_refused(self):
        def loose(document):
            run = find(document, "fuzz_columns", "macOS")
            run["command"] = run["command"].replace("-malloc_limit_mb=256", "-malloc_limit_mb=0")
        self.assert_fails(loose, "no -malloc_limit_mb=256")

    def test_the_header_cannot_lower_the_time(self):
        def lower(document):
            document["header"]["required_seconds"] = 60
        self.assert_fails(lower, "header.required_seconds: G-3 is 1800 s")

    def test_a_target_list_other_than_cmakes_is_refused(self):
        def fewer(document):
            document["header"]["targets"] = TARGETS[1:]
        self.assert_fails(fewer, "is not the list fuzz/CMakeLists.txt defines")

    def test_the_avx2_list_is_the_validators(self):
        def fewer(document):
            document["header"]["avx2_path_targets"] = list(check_fuzz.AVX2_PATH[1:])
        self.assert_fails(fewer, "header.avx2_path_targets")

    def test_the_crc32c_list_is_the_validators(self):
        def fewer(document):
            document["header"]["crc32c_path_targets"] = list(check_fuzz.CRC32C_PATH[1:])
        self.assert_fails(fewer, "header.crc32c_path_targets")

    def test_linux_must_run_the_sse42_crc_target_that_uses_no_simd(self):
        # fuzz_container reaches no SIMD kernel, only the SSE4.2 CRC32C kernel.
        self.assertNotIn("fuzz_container", check_fuzz.AVX2_PATH)

        def drop(document):
            document["runs"] = [run for run in document["runs"]
                                if (run["target"], run["host"]) != ("fuzz_container", "Linux")]
            resummarise(document)
        self.assert_fails(drop, "no run of fuzz_container on Linux at avx2: G-3 needs every target")

    def test_a_done_line_that_disagrees_is_refused(self):
        def disagree(document):
            find(document, "fuzz_columns", "macOS")["done_line"] = "Done 5 runs in 1801 second(s)"
        self.assert_fails(disagree, "done_line: says 5 runs")

    def test_a_status_line_that_disagrees_is_refused(self):
        def disagree(document):
            find(document, "fuzz_columns", "macOS")["coverage"]["cov"] = 1
        self.assert_fails(disagree, "final_status_line: its runs, coverage or corpus")

    def test_final_stats_that_disagree_are_refused(self):
        def disagree(document):
            find(document, "fuzz_columns", "macOS")["final_stats"]["peak_rss_mb"] = 7
        self.assert_fails(disagree, "final_stats: not the run's")

    def test_a_clean_run_with_a_nonzero_exit_is_refused(self):
        def exited(document):
            find(document, "fuzz_columns", "macOS")["exit_code"] = 77
        self.assert_fails(exited, "clean, but the fuzzer exited 77")

    def test_a_failed_run_must_name_its_finding(self):
        def failed(document):
            run = find(document, "fuzz_columns", "macOS")
            run.update(clean=False, exit_code=77, seconds=40)
            resummarise(document)
        self.assert_fails(failed, "findings: a run that was not clean names each finding")

    def test_a_failed_run_with_its_finding_passes_and_counts_as_not_clean(self):
        document = valid_document()
        run = find(document, "fuzz_columns", "macOS")
        run.update(clean=False, exit_code=77, seconds=40,
                   findings=[{"kind": "crash", "detail": "ERROR: AddressSanitizer"}])
        resummarise(document)
        self.assertEqual(document["summary"]["macOS"]["targets_clean"], len(TARGETS) - 1)
        self.assertEqual(document["summary"]["macOS"]["shortest_seconds"], 40)
        result = self.run_check(document)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_summary_that_is_not_the_runs_is_refused(self):
        def inflate(document):
            document["summary"]["Linux"]["targets_clean"] = 99
        self.assert_fails(inflate, "summary:")

    def test_a_second_run_of_one_target_is_refused(self):
        def twice(document):
            document["runs"].append(copy.deepcopy(find(document, "fuzz_columns", "macOS")))
        self.assert_fails(twice, "a second run of fuzz_columns on macOS")

    def test_a_run_at_another_commit_is_refused(self):
        def other(document):
            find(document, "fuzz_columns", "macOS")["commit"] = "f" * 40
        self.assert_fails(other, "commit: not the header's commit")

    def test_a_wrong_file_name_is_refused(self):
        self.assert_fails(lambda document: None, f"is not {NAME}",
                          name="fuzz-g3-latest.json")

    def test_a_home_path_is_refused(self):
        def leak(document):
            find(document, "fuzz_columns", "macOS")["command"] += " /Users/someone/corpus"
        self.assert_fails(leak, "a home-style path")

    def test_the_repos_own_cmake_lists_every_avx2_target(self):
        targets = check_fuzz.targets_in(check_fuzz.DEFAULT_CMAKE)
        self.assertEqual(len(targets), 13)
        for name in check_fuzz.LINUX_REQUIRED:
            self.assertIn(name, targets)


class TieTest(Checking):
    """A file holds for the code of its commit: anything a fuzz binary is built from or
    launched with that changed since refuses it, and where git has no history it is not
    judged. Each test starts from the repository as committed."""

    def setUp(self):
        super().setUp()
        self.addCleanup(self.restore)

    def restore(self):
        git(REPO, "reset", "-q", "--hard", COMMIT)
        git(REPO, "clean", "-q", "-fd")

    def change(self, name, commit=True):
        with (REPO / name).open("a", encoding="utf-8") as file:
            file.write("// changed\n")
        if commit:
            git(REPO, "commit", "-q", "-am", f"change {name}")

    def test_a_file_at_unchanged_code_is_tied_and_passes(self):
        result = self.run_check(valid_document())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"ok, tied: nothing fuzzed changed since {COMMIT[:12]}", result.stdout)

    def test_a_file_older_than_a_fuzzed_code_change_is_refused(self):
        for name in TIED:
            with self.subTest(name=name):
                self.change(name)
                result = self.run_check(valid_document())
                self.assertEqual(result.returncode, check_fuzz.EXIT_INVALID, result.stdout)
                self.assertIn(f"the fuzzed code changed after {COMMIT[:12]}", result.stderr)
                self.assertIn(name, result.stderr)
                self.restore()

    def test_an_uncommitted_or_new_fuzzed_file_is_refused(self):
        self.change("src/codec/pfor.cpp", commit=False)
        result = self.run_check(valid_document())
        self.assertEqual(result.returncode, check_fuzz.EXIT_INVALID, result.stdout)
        self.assertIn("src/codec/pfor.cpp", result.stderr)
        self.restore()
        (REPO / "fuzz" / "fuzz_new.cpp").write_text("int h();\n", encoding="utf-8")
        result = self.run_check(valid_document())
        self.assertEqual(result.returncode, check_fuzz.EXIT_INVALID, result.stdout)
        self.assertIn("fuzz/fuzz_new.cpp", result.stderr)

    def test_prose_scripts_and_tests_do_not_untie_it(self):
        for name in NOT_TIED:
            self.change(name)
        result = self.run_check(valid_document())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_no_history_is_not_judged(self):
        exported = self.dir / "exported"
        shutil.copytree(REPO, exported, ignore=shutil.ignore_patterns(".git"))
        result = self.run_check(valid_document(), repo=exported)
        self.assertEqual(result.returncode, check_fuzz.EXIT_UNJUDGED, result.stdout)
        self.assertIn(f"could not judge whether the fuzzed code changed after {COMMIT[:12]}: "
                      "exported is not a git checkout", result.stderr)
        self.assertNotIn("ok", result.stdout)

    def test_a_clone_too_shallow_to_hold_the_commit_is_not_judged(self):
        self.change("docs/NOTES.md")
        shallow = self.dir / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth", "1", REPO.as_uri(), str(shallow)],
                       check=True, capture_output=True)
        result = self.run_check(valid_document(), repo=shallow)
        self.assertEqual(result.returncode, check_fuzz.EXIT_UNJUDGED, result.stdout)
        self.assertIn(f"{COMMIT[:12]} is not in this checkout's history", result.stderr)

    def pre_tie_document(self):
        """A valid file named as the one made before the tie, at a commit this repository
        does not hold, so only the pin can let it through."""
        document = valid_document()
        commit = check_fuzz.PRE_TIE_FILE[len("fuzz-g3-"):-len(".json")] + "0" * 28
        document["header"]["commit"] = commit
        for run in document["runs"]:
            run["commit"] = commit
        return document

    def pin_of(self, document):
        digest = hashlib.sha256(json.dumps(document, indent=1).encode()).hexdigest()
        return {check_fuzz.PRE_TIE_PINS: {check_fuzz.PRE_TIE_FILE: digest}}

    def test_the_file_made_before_the_tie_passes_by_its_pin(self):
        document = self.pre_tie_document()
        result = self.run_check(document, check_fuzz.PRE_TIE_FILE, pins=self.pin_of(document))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("made before the tie and pinned, so not tied", result.stdout)

    def test_the_pin_lets_through_no_other_content(self):
        document = self.pre_tie_document()
        pins = self.pin_of(document)
        find(document, "fuzz_columns", "macOS")["cpu"] = "cpu 3"
        result = self.run_check(document, check_fuzz.PRE_TIE_FILE, pins=pins)
        self.assertEqual(result.returncode, check_fuzz.EXIT_UNJUDGED, result.stdout)
        self.assertIn("is not in this checkout's history", result.stderr)
        self.assertNotIn("pinned", result.stdout)

    def test_no_other_file_may_be_pinned(self):
        self.change("src/codec/pfor.cpp")
        document = valid_document()
        pins = {check_fuzz.PRE_TIE_PINS: {NAME: hashlib.sha256(
            json.dumps(document, indent=1).encode()).hexdigest()}}
        result = self.run_check(document, pins=pins)
        self.assertEqual(result.returncode, check_fuzz.EXIT_INVALID, result.stdout)
        self.assertIn(f"{NAME} is pinned, and only {check_fuzz.PRE_TIE_FILE} may be",
                      result.stderr)
        self.assertIn("the fuzzed code changed after", result.stderr)

    def test_the_committed_file_is_the_one_pinned(self):
        pins = json.loads(check_fuzz.DEFAULT_PINS.read_text(encoding="utf-8"))
        path = REPO_ROOT / "results" / check_fuzz.PRE_TIE_FILE
        self.assertEqual(pins[check_fuzz.PRE_TIE_PINS],
                         {path.name: hashlib.sha256(path.read_bytes()).hexdigest()})
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("made before the tie and pinned", result.stdout)


LOG_CLEAN = """INFO: Seed: 1
#5099\tINITED cov: 1534 ft: 7839 corp: 916/2909Kb exec/s: 63 rss: 1151Mb
#90050\tDONE   cov: 1500 ft: 7000 corp: 900/2900Kb lim: 16384 exec/s: 50 rss: 1199Mb
Done 90050 runs in 1801 second(s)
stat::number_of_executed_units: 90050
stat::average_exec_per_sec:     50
stat::new_units_added:          12
stat::slowest_unit_time_sec:    0
stat::peak_rss_mb:              1200
"""

LOG_CRASH = """INFO: Seed: 1
#5099\tINITED cov: 1534 ft: 7839 corp: 916/2909Kb exec/s: 63 rss: 1151Mb
check_pfor_column: the kernel set and the scalar set decoded a block differently
==1== ERROR: libFuzzer: deadly signal
artifact_prefix='build/fuzz/artifacts/'; Test unit written to build/fuzz/artifacts/crash-ab
"""


class AssembleTest(Scratch):
    def write_host(self, host, logs):
        folder = self.dir / host
        folder.mkdir()
        facts = {**HOST_FACTS[host], "default_kernel_level": "neon" if host == "macOS" else "avx2"}
        (folder / "host.json").write_text(json.dumps(facts), encoding="utf-8")
        names = TARGETS if host == "macOS" else check_fuzz.LINUX_REQUIRED
        for name in names:
            tag = name if host == "macOS" else f"{name}-avx2"
            exit_code = 77 if logs.get(name) is LOG_CRASH else 0
            run_command = command(name).split(" ", 1)[1].replace("build/", "/x/repo/build/")
            if host == "Linux":
                run_command = f"taskset -c 2 {run_command}"
            (folder / f"{tag}.meta").write_text(
                f"commit={COMMIT}\nstart=2026-11-11T10:00:00Z epoch=1000\n"
                f"ASAN_OPTIONS=abort_on_error=0:exitcode=77 UBSAN_OPTIONS=print_stacktrace=1"
                f"{' TICK_STORE_SIMD=avx2' if host == 'Linux' else ''}\n"
                f"command={run_command}\n"
                f"end=2026-11-11T10:30:03Z epoch=2803 exit={exit_code}\n", encoding="utf-8")
            (folder / f"{tag}.log").write_text(logs.get(name, LOG_CLEAN), encoding="utf-8")
        return folder

    def assemble(self, crash=None):
        mac = self.write_host("macOS", {crash: LOG_CRASH} if crash else {})
        linux = self.write_host("Linux", {})
        out = self.dir / NAME
        result = subprocess.run(
            [sys.executable, "-I", str(ASSEMBLER), "--commit", COMMIT, "--out", str(out),
             "--host", f"macOS={mac}", "--host", f"Linux={linux}", "--strip", "/x/repo",
             "--cmake", str(self.cmake)], capture_output=True, text=True, check=False)
        return result, out

    def test_clean_logs_assemble_to_a_file_the_validator_passes(self):
        result, out = self.assemble()
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(document["summary"]["macOS"],
                         {"targets_required": len(TARGETS), "targets_clean": len(TARGETS),
                          "shortest_seconds": 1801})
        linux = find(document, "fuzz_pfor", "Linux")
        self.assertEqual((linux["kernel_level"], linux["cpu"]), ("avx2", "cpu 2"))
        self.assertNotIn("/x/repo", out.read_text(encoding="utf-8"))
        checked = subprocess.run([sys.executable, "-I", str(SCRIPT), "--cmake", str(self.cmake),
                                  "--repo", str(REPO), str(out)], capture_output=True, text=True,
                                 check=False)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_a_crash_is_recorded_as_not_clean(self):
        result, out = self.assemble(crash="fuzz_pfor")
        self.assertEqual(result.returncode, 0, result.stderr)
        run = find(json.loads(out.read_text(encoding="utf-8")), "fuzz_pfor", "macOS")
        self.assertFalse(run["clean"])
        self.assertEqual(run["findings"][0]["kind"], "crash")
        self.assertIn("check_pfor_column", run["findings"][0]["detail"])


if __name__ == "__main__":
    os.environ.pop("PYTHONPATH", None)
    unittest.main()
