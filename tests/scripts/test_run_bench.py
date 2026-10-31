#!/usr/bin/env python3
"""Tests for scripts/run_bench.py. Run with:

    python3 -I tests/scripts/test_run_bench.py

The script runs as a subprocess against stand-ins generated under the repo's gitignored
build/ directory: a fake benchmark binary, a fake measurement wrapper, and on Linux fake
taskset and perf, which show the exact command line the real ones would get. Which host
path runs depends on the machine, and the tests follow it.
"""

import importlib.util
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

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "run_bench.py"
CHECK = REPO_ROOT / "scripts" / "check_results.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_run_bench"

HOST = "macOS" if platform.system() == "Darwin" else "Linux"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
SHA = "ab" * 32
MESSAGES = 1000
SLICE = "test-slice"

PINS = {"slices": {SLICE: {"sha256": SHA, "message_count": MESSAGES, "size": 5000,
                           "path": "data/none.itch"}}}


def load_fixtures():
    sys.dont_write_bytecode = True
    path = REPO_ROOT / "tests" / "scripts" / "test_check_results.py"
    spec = importlib.util.spec_from_file_location("check_results_fixtures", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FIXTURES = load_fixtures()


def fake_report(commit, dirty, case):
    return {
        "case": case,
        "build": {"compiler": "Clang 23.1.4", "compile_flags": "-O3 -DNDEBUG -std=c++23",
                  "build_type": "Release", "sanitizers": "none", "commit": commit,
                  "tree_dirty": dirty,
                  "isa": ("arm64 baseline, scalar kernels" if HOST == "macOS"
                          else "x86-64 baseline, scalar kernels")},
        "pinning": {"status": "not-supported" if HOST == "macOS" else "pinned",
                    "requested_core": -1, "observed_core": None},
        "counters": "not-supported" if HOST == "macOS" else "gated",
        "timer": FIXTURES.timer(HOST),
        "timer_end": FIXTURES.timer(HOST),
        "verification": {"messages": MESSAGES, "bytes": 5000, "input_sha256": SHA,
                         "reencoded_sha256": SHA},
        "warmup": {"passes_per_repetition": 2, "first_repetition_pass_ms": [90.0, 89.0]},
        "gated": {"messages": 10000, "wall_ns": 1000000.0},
        "latency": FIXTURES.valid_document(HOST)["results"][0]["latency"],
    }


def fake_gbench(case):
    rows = [{"name": f"{case}/throughput/real_time", "run_type": "iteration"}]
    for name, rate in (("mean", 6e7), ("median", 6e7), ("stddev", 1e5), ("cv", 0.01)):
        rows.append({"name": f"{case}/throughput/real_time_{name}", "run_type": "aggregate",
                     "aggregate_name": name, "real_time": 90.0, "time_unit": "ms",
                     "items_per_second": rate, "bytes_per_second": rate * 33})
    return {"context": {"library_build_type": "release", "host_name": "should-never-appear",
                        "executable": "/secret/path"},
            "benchmarks": rows}


BINARY = """#!{python} -I
import json, sys
arguments = dict(a[2:].split("=", 1) for a in sys.argv[1:] if "=" in a)
if "--list-cases" in sys.argv:
    print("itch_decode\\nitch_decode_encode")
    sys.exit(0)
with open({record!r}, "a") as out:
    out.write(" ".join(sys.argv[1:]) + "\\n")
with open(arguments["report"], "w") as out:
    out.write({reports!r}[arguments["case"]])
with open(arguments["benchmark_out"], "w") as out:
    out.write({gbench!r}[arguments["case"]])
sys.exit({exit_code})
"""

WRAPPER = """#!/bin/bash
echo "measure.sh: start load1=1.10 host=secret-box at=2026-10-30T20:00:00Z{extra}" >&2
"$@"
rc=$?
echo "measure.sh: end load1=1.20 exit=$rc at=2026-10-30T20:00:09Z{extra}" >&2
exit $rc
"""

REFUSING_WRAPPER = """#!/bin/bash
echo "measure.sh: lock busy for 3600s; not measuring" >&2
exit 75
"""

TASKSET = """#!/bin/bash
echo "taskset $*" >> {record}
shift 2
exec "$@"
"""

PERF = """#!/bin/bash
echo "perf $*" >> {record}
while [ "$1" != "--" ]; do
  if [ "$1" = "-o" ]; then out=$2; fi
  shift
done
shift
"$@"
rc=$?
{{
  echo "# started on some date"
  echo
  echo "4000,,cycles:u,1000000,100.00,,"
  echo "8000,,instructions:u,1000000,100.00,,"
  echo "120,,branch-misses:u,1000000,100.00,,"
}} > "$out"
exit $rc
"""


def write_script(path, text):
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class RunBenchTest(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.dir)
        self.results = self.dir / "results"
        self.iteration = self.dir / "iteration"
        self.work = self.dir / "work"
        self.record = self.dir / "commands.txt"
        self.pins = self.dir / "pins.json"
        self.pins.write_text(json.dumps(PINS), encoding="utf-8")
        self.slice = self.dir / "slice.itch"
        self.slice.write_bytes(b"\0")
        self.commit = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"], capture_output=True, text=True,
            check=False).stdout.strip() or COMMIT
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        write_script(self.bin / "taskset", TASKSET.format(record=self.record))
        write_script(self.bin / "perf", PERF.format(record=self.record))

    def make_binary(self, commit=None, dirty=False, exit_code=0):
        cases = ("itch_decode", "itch_decode_encode")
        reports = {c: json.dumps(fake_report(commit or self.commit, dirty, c)) for c in cases}
        gbench = {c: json.dumps(fake_gbench(c)) for c in cases}
        path = self.dir / "itch_codec_bench"
        write_script(path, BINARY.format(python=sys.executable, record=str(self.record),
                                         reports=reports, gbench=gbench, exit_code=exit_code))
        return path

    def make_wrapper(self, text=WRAPPER):
        extra = "" if HOST == "macOS" else (
            " cpus=4 governor=performance freq_khz=cpu4:3200000 perf_event_paranoid=2")
        path = self.dir / "wrapper.sh"
        write_script(path, text.format(extra=extra))
        return path

    def run_bench(self, binary, wrapper=None):
        environment = {key: value for key, value in os.environ.items()
                       if key != "TICK_STORE_MEASURE"}
        environment["PATH"] = f"{self.bin}{os.pathsep}{environment['PATH']}"
        if wrapper is not None:
            environment["TICK_STORE_MEASURE"] = str(wrapper)
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), str(binary), "--slice", SLICE,
             "--slice-path", str(self.slice), "--pins", str(self.pins),
             "--results-dir", str(self.results), "--iteration-dir", str(self.iteration),
             "--work-dir", str(self.work)],
            capture_output=True, text=True, env=environment, check=False)

    def result_path(self):
        return self.results / f"{HOST}-itch_codec-{self.commit[:12]}.json"

    def test_a_measured_run_writes_a_file_the_validator_passes(self):
        result = self.run_bench(self.make_binary(), self.make_wrapper())
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        checked = subprocess.run(
            [sys.executable, "-I", str(CHECK), "--pins", str(self.pins), str(self.result_path())],
            capture_output=True, text=True, check=False)
        self.assertEqual(checked.returncode, 0, msg=checked.stderr)

    def test_the_header_comes_first_and_carries_both_load_lines_per_case(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        document = json.loads(self.result_path().read_text(encoding="utf-8"))
        self.assertEqual(next(iter(document)), "header")
        self.assertEqual(document["header"]["host"], HOST)
        self.assertEqual(len(document["header"]["load_lines"]), 4)
        self.assertEqual([row["case"] for row in document["results"]],
                         ["itch_decode", "itch_decode_encode"])

    def test_the_hostname_never_reaches_the_results_file(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        text = self.result_path().read_text(encoding="utf-8")
        self.assertNotIn("secret-box", text)
        self.assertNotIn("host=", text)
        self.assertNotIn("should-never-appear", text)
        self.assertNotIn("/secret/path", text)

    def test_retry_later_passes_the_wrappers_exit_code_and_writes_nothing(self):
        result = self.run_bench(self.make_binary(), self.make_wrapper(REFUSING_WRAPPER))
        self.assertEqual(result.returncode, 75)
        self.assertIn("retry later", result.stderr)
        self.assertFalse(self.results.exists())

    def test_no_wrapper_means_iteration_only_and_the_validator_refuses_it(self):
        result = self.run_bench(self.make_binary())
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("iteration-only", result.stdout)
        self.assertFalse(self.results.exists())
        path = self.iteration / f"{HOST}-itch_codec-{self.commit[:12]}.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(document["header"]["measurement"], "iteration-only")
        self.assertEqual(document["header"]["load_lines"], [])
        checked = subprocess.run(
            [sys.executable, "-I", str(CHECK), "--pins", str(self.pins), str(path)],
            capture_output=True, text=True, check=False)
        self.assertEqual(checked.returncode, 1)
        self.assertIn("iteration-only", checked.stderr)

    def test_a_failed_benchmark_keeps_its_code_and_writes_nothing(self):
        result = self.run_bench(self.make_binary(exit_code=3), self.make_wrapper())
        self.assertEqual(result.returncode, 3)
        self.assertFalse(self.result_path().exists())

    def test_a_binary_from_another_commit_is_refused(self):
        result = self.run_bench(self.make_binary(commit=COMMIT[::-1]), self.make_wrapper())
        self.assertEqual(result.returncode, 2)
        self.assertIn("rebuild it", result.stderr)
        self.assertFalse(self.result_path().exists())

    def test_a_binary_from_a_dirty_tree_is_refused(self):
        result = self.run_bench(self.make_binary(dirty=True), self.make_wrapper())
        self.assertEqual(result.returncode, 2)
        self.assertIn("dirty tree", result.stderr)

    def test_a_file_the_validator_rejects_is_never_published(self):
        wrapper = self.make_wrapper(WRAPPER.replace("exit=$rc", "exit=9"))
        result = self.run_bench(self.make_binary(), wrapper)
        self.assertEqual(result.returncode, 1)
        self.assertIn("no file was written", result.stderr)
        self.assertEqual(list(self.results.iterdir()), [])

    def test_each_case_runs_directly_under_the_wrapper(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        commands = self.record.read_text(encoding="utf-8")
        self.assertEqual(commands.count("--case=itch_decode "), 1)
        self.assertEqual(commands.count("--case=itch_decode_encode "), 1)

    @unittest.skipUnless(HOST == "Linux", "the pinned path exists only on Linux")
    def test_linux_pins_and_counts_as_the_brief_says(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        commands = self.record.read_text(encoding="utf-8")
        self.assertIn("taskset -c 4 perf stat -x, -e cycles:u,instructions:u,branch-misses:u",
                      commands)
        self.assertIn("--core=4", commands)
        document = json.loads(self.result_path().read_text(encoding="utf-8"))
        header = document["header"]
        self.assertEqual(header["governor"], "performance")
        self.assertEqual(header["frequency"]["snapshot_khz"][0], 3200000)
        counters = document["results"][0]["counters"]
        self.assertEqual(counters["cycles"]["value"], 4000)
        self.assertEqual(counters["cache_misses"]["status"], "unavailable")
        self.assertEqual(counters["per_message"]["cycles"], 0.4)

    @unittest.skipUnless(HOST == "macOS", "the unpinned path exists only on macOS")
    def test_macos_stays_unpinned_with_no_counters(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        commands = self.record.read_text(encoding="utf-8")
        self.assertNotIn("taskset", commands)
        self.assertNotIn("perf ", commands)
        self.assertNotIn("--core", commands)
        document = json.loads(self.result_path().read_text(encoding="utf-8"))
        self.assertIn("Unpinned", document["header"]["statement"])
        self.assertEqual(document["results"][0]["counters"]["cycles"]["status"], "unavailable")


if __name__ == "__main__":
    unittest.main()
