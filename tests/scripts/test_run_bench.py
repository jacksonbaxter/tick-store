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
from unittest import mock

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
                  "tree_dirty": dirty, "commit_source": "git",
                  "isa": ("arm64 baseline, neon kernels" if HOST == "macOS"
                          else "x86-64 baseline, avx2 kernels")},
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


SEEDED_CASES = ("pfor_unpack/w=13/scalar", "svb_decode/scalar",
                "svb_decode/" + ("neon" if HOST == "macOS" else "avx2"))


def fake_seeded_report(commit, case):
    report = fake_report(commit, False, case)
    baseline = "arm64 baseline" if HOST == "macOS" else "x86-64 baseline"
    report["build"]["isa"] = f"{baseline}, {case.rsplit('/', 1)[1]} kernels"
    report["verification"] = {"input_seed": 13, "items_per_call": 1024, "calls_per_pass": 8192,
                              "against": "the scalar set", "matched": True}
    report["gated"] = {"items": 10000, "wall_ns": 1000000.0}
    latency = report["latency"]
    latency["batch_items"] = latency.pop("batch_messages")
    return report


COLUMN_CASES = ("gorilla_xor/add_price", "store_decode/add_price")


def fake_column_report(commit, case):
    """A real-data column decode: the slice's checks, then its column given back exactly."""
    report = fake_seeded_report(commit, case)
    report["build"]["isa"] = fake_report(commit, False, case)["build"]["isa"]
    report["verification"] = {"messages": MESSAGES, "bytes": 5000, "input_sha256": SHA,
                              "reencoded_sha256": SHA, "column": "add_price", "values": 3000,
                              "encoded_bytes": 2400, "items_per_call": 3000,
                              "calls_per_pass": 1, "matched": True}
    return report


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
    print({cases!r})
    sys.exit(0)
if "--input-kind" in sys.argv:
    print({kind!r})
    sys.exit(0)
with open({record!r}, "a") as out:
    out.write("binary " + " ".join(sys.argv[1:]) + "\\n")
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

# Each run adds one case's jiffies to the stand-in stat file, so every case sees time pass.
PERF = """#!/bin/bash
echo "perf $*" >> {record}
awk 'NR == FNR {{ step[$1] = $0; next }}
     $1 in step {{ split(step[$1], add); for (i = 2; i <= NF; i++) $i += add[i] }}
     {{ print }}' {stat}.step {stat} > {stat}.next && mv {stat}.next {stat}
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


# cpu4 is the pinned core, whose counters must not be the ones read.
STAT_BEFORE = ("cpu  9 9 9 9 9 9 9 9 0 0\ncpu4 9000 0 9000 0 0 0 0 0 0 0\n"
               "cpu5 100 0 100 800 0 0 0 0 0 0\ncpu6 1 1 1 1 1 1 1 1 0 0\nintr 1\n")
# Over 1000 jiffies: 2 user, 1 nice, 1 system, 1 irq, 985 idle, 10 iowait. So 0.5% busy.
STAT_AFTER = ("cpu  9 9 9 9 9 9 9 9 0 0\ncpu4 9000 0 9000 1000 0 0 0 0 0 0\n"
              "cpu5 102 1 101 1785 10 1 0 0 0 0\ncpu6 1 1 1 1 1 1 1 1 0 0\nintr 1\n")
# 2000 busy jiffies on the sibling while another job held the lock, and then the same case.
STAT_AFTER_A_WAIT = STAT_BEFORE.replace("cpu5 100 0 100", "cpu5 1100 0 1100")
STAT_AFTER_THE_WAIT_AND_CASE = STAT_AFTER.replace("cpu5 102 1 101", "cpu5 1102 1 1101")


def stat_step(before, after):
    """The lines one case moves, as what it adds to each field."""
    lines = []
    for old, new in zip(before.splitlines(), after.splitlines()):
        if old != new:
            name, *values = new.split()
            added = (int(n) - int(o) for n, o in zip(values, old.split()[1:]))
            lines.append(" ".join([name, *map(str, added)]))
    return "\n".join(lines) + "\n"


STAT_STEP = stat_step(STAT_BEFORE, STAT_AFTER)

WAITING_WRAPPER = """#!/bin/bash
cp STAT.waited STAT
"$@"
"""


def load_run_bench():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("run_bench_under_test", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


RUN_BENCH = load_run_bench()


def load_check_results():
    spec = importlib.util.spec_from_file_location("check_results_under_test", CHECK)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


CHECK_RESULTS = load_check_results()


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
        # The script's own lookup: git in a checkout, the COMMIT file in an exported tree.
        self.commit = RUN_BENCH.current_commit()
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        self.stat = self.dir / "stat"
        self.stat.write_text(STAT_BEFORE, encoding="ascii")
        (self.dir / "stat.step").write_text(STAT_STEP, encoding="ascii")
        self.sysfs = self.dir / "cpu"
        self.write_topology("thread_siblings_list", "4-5")
        # The boost switch, so no test reads the host's own sysfs: a hosted runner has none.
        self.boost = self.sysfs / "cpufreq" / "boost"
        self.boost.parent.mkdir(parents=True)
        self.boost.write_text("1\n", encoding="ascii")
        write_script(self.bin / "taskset", TASKSET.format(record=self.record))
        write_script(self.bin / "perf", PERF.format(record=self.record, stat=self.stat))

    def write_topology(self, name, text):
        directory = self.sysfs / "cpu4" / "topology"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text(f"{text}\n", encoding="ascii")

    def make_binary(self, commit=None, dirty=False, exit_code=0):
        cases = ("itch_decode", "itch_decode_encode")
        reports = {c: json.dumps(fake_report(commit or self.commit, dirty, c)) for c in cases}
        gbench = {c: json.dumps(fake_gbench(c)) for c in cases}
        path = self.dir / "itch_codec_bench"
        write_script(path, BINARY.format(python=sys.executable, record=str(self.record),
                                         reports=reports, gbench=gbench, exit_code=exit_code,
                                         cases="\n".join(cases), kind="slice"))
        return path

    def make_seeded_binary(self, kind="seeded", matched=True):
        reports = {}
        for case in SEEDED_CASES:
            report = fake_seeded_report(self.commit, case)
            report["verification"]["matched"] = matched
            reports[case] = json.dumps(report)
        gbench = {c: json.dumps(fake_gbench(c)) for c in SEEDED_CASES}
        path = self.dir / "simd_kernel_bench"
        write_script(path, BINARY.format(python=sys.executable, record=str(self.record),
                                         reports=reports, gbench=gbench, exit_code=0,
                                         cases="\n".join(SEEDED_CASES), kind=kind))
        return path

    def make_column_binary(self, matched=True):
        reports = {}
        for case in COLUMN_CASES:
            report = fake_column_report(self.commit, case)
            report["verification"]["matched"] = matched
            reports[case] = json.dumps(report)
        gbench = {c: json.dumps(fake_gbench(c)) for c in COLUMN_CASES}
        path = self.dir / "column_decode_bench"
        write_script(path, BINARY.format(python=sys.executable, record=str(self.record),
                                         reports=reports, gbench=gbench, exit_code=0,
                                         cases="\n".join(COLUMN_CASES), kind="column"))
        return path

    def make_wrapper(self, text=WRAPPER):
        extra = "" if HOST == "macOS" else (
            " cpus=4 governor=performance freq_khz=cpu4:3200000 perf_event_paranoid=2")
        path = self.dir / "wrapper.sh"
        write_script(path, text.format(extra=extra))
        return path

    def run_bench(self, binary, wrapper=None, extra=()):
        environment = {key: value for key, value in os.environ.items()
                       if key != "TICK_STORE_MEASURE"}
        environment["PATH"] = f"{self.bin}{os.pathsep}{environment['PATH']}"
        if wrapper is not None:
            environment["TICK_STORE_MEASURE"] = str(wrapper)
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), str(binary), "--slice", SLICE,
             "--slice-path", str(self.slice), "--pins", str(self.pins),
             "--results-dir", str(self.results), "--iteration-dir", str(self.iteration),
             "--work-dir", str(self.work), "--sysfs-cpu", str(self.sysfs),
             "--proc-stat", str(self.stat), *extra],
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

    def test_the_header_says_how_the_builds_commit_was_established(self):
        """The build's commit_source reaches the published file, not only its report (RA-5)."""
        self.run_bench(self.make_binary(), self.make_wrapper())
        document = json.loads(self.result_path().read_text(encoding="utf-8"))
        self.assertEqual(document["header"]["commit"], self.commit)
        self.assertEqual(document["header"]["commit_source"], "git")

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

    # DS-4's rule keeps every attempt's document, the refused ones too: --keep-document writes
    # the assembled document to a path before validation, whatever the verdict.

    def test_a_refused_document_is_kept_where_asked_and_still_never_published(self):
        wrapper = self.make_wrapper(WRAPPER.replace("exit=$rc", "exit=9"))
        kept = self.dir / "attempts" / "attempt-1" / "document.json"
        result = self.run_bench(self.make_binary(), wrapper, ("--keep-document", str(kept)))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no file was written", result.stderr)
        self.assertEqual(list(self.results.iterdir()), [])
        self.assertTrue(kept.exists(), "the refused document was not kept")
        document = json.loads(kept.read_text(encoding="utf-8"))
        self.assertEqual(next(iter(document)), "header")
        self.assertEqual(document["header"]["measurement"], "reported")
        self.assertTrue(any("exit=9" in line for line in document["header"]["load_lines"]))

    def test_a_kept_document_is_the_published_file_byte_for_byte(self):
        kept = self.dir / "kept.json"
        result = self.run_bench(self.make_binary(), self.make_wrapper(),
                                ("--keep-document", str(kept)))
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertEqual(kept.read_bytes(), self.result_path().read_bytes())

    def test_each_case_runs_directly_under_the_wrapper(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        # On Linux the fake taskset and perf record the same arguments, so only the binary's.
        runs = [line for line in self.record.read_text(encoding="utf-8").splitlines()
                if line.startswith("binary ")]
        self.assertEqual(sum("--case=itch_decode " in line for line in runs), 1)
        self.assertEqual(sum("--case=itch_decode_encode " in line for line in runs), 1)

    @unittest.skipUnless(HOST == "Linux", "the pinned path exists only on Linux")
    def test_linux_pins_and_counts_as_the_brief_says(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        commands = self.record.read_text(encoding="utf-8")
        self.assertIn("taskset -c 4 sh -c", commands)
        self.assertIn("perf stat -x, -e cycles:u,instructions:u,branch-misses:u", commands)
        self.assertIn("--core=4", commands)
        document = json.loads(self.result_path().read_text(encoding="utf-8"))
        header = document["header"]
        self.assertEqual(header["governor"], "performance")
        self.assertEqual(header["frequency"]["snapshot_khz"][0], 3200000)
        counters = document["results"][0]["counters"]
        self.assertEqual(counters["cycles"]["value"], 4000)
        self.assertEqual(counters["cache_misses"]["status"], "unavailable")
        self.assertEqual(counters["per_message"]["cycles"], 0.4)
        self.assertEqual(header["sibling_busy_percent"]["cases"],
                         {"itch_decode": 0.5, "itch_decode_encode": 0.5})

    @unittest.skipUnless(HOST == "macOS", "the unpinned path exists only on macOS")
    def test_macos_stays_unpinned_with_no_counters(self):
        self.run_bench(self.make_binary(), self.make_wrapper())
        commands = self.record.read_text(encoding="utf-8")
        self.assertNotIn("taskset", commands)
        self.assertNotIn("perf ", commands)
        self.assertNotIn("--core", commands)
        document = json.loads(self.result_path().read_text(encoding="utf-8"))
        self.assertIn("Unpinned", document["header"]["statement"])
        self.assertEqual(document["header"]["sibling_busy_percent"], "not-available")
        self.assertEqual(document["results"][0]["counters"]["cycles"]["status"], "unavailable")

    # The Linux path of a case, on either host, with the fake taskset and perf above.

    def linux_case(self, wrapper):
        self.work.mkdir()
        environment = {"PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}"}
        meter = RUN_BENCH.SiblingMeter(self.sysfs, self.stat)
        with mock.patch.dict(os.environ, environment):
            return RUN_BENCH.run_case("Linux", str(wrapper), self.make_binary(), "itch_decode",
                                      self.slice, PINS["slices"][SLICE], self.work, meter)

    def test_a_linux_case_records_its_siblings_busy_share(self):
        run = self.linux_case(self.make_wrapper())
        self.assertEqual(run["sibling_busy_percent"], 0.5)
        commands = self.record.read_text(encoding="utf-8")
        self.assertIn("taskset -c 4 sh -c", commands)
        self.assertIn("perf stat -x,", commands)

    def test_a_linux_document_meets_the_validators_sibling_rule(self):
        run = self.linux_case(self.make_wrapper())
        document = self.linux_document(run)
        header = document["header"]
        self.assertEqual(header["turbo"], "enabled")
        self.assertEqual(header["sibling_busy_percent"]["cases"], {"itch_decode": 0.5})
        self.assertEqual(CHECK_RESULTS.check_sibling(header, ["itch_decode"]), [])
        self.assertEqual(CHECK_RESULTS.check_linux({"sibling": header["sibling"]}, []), [])
        self.assertEqual(CHECK_RESULTS.check_leaks(document), [])

    def linux_document(self, run, turbo_before="enabled"):
        meter = RUN_BENCH.SiblingMeter(self.sysfs, self.stat)
        with mock.patch.object(RUN_BENCH, "cpu_model", return_value="cpu"):
            return RUN_BENCH.build_document("Linux", "itch_codec", SLICE, PINS["slices"][SLICE],
                                            COMMIT, "wrapper", [run], meter, self.sysfs,
                                            turbo_before=turbo_before)

    def test_turbo_that_changed_across_the_run_is_not_stated(self):
        # Review E-10: boost read once, after the last case, outside the lock, would state
        # what held after the run. It is read before the first case too, and must agree.
        run = self.linux_case(self.make_wrapper())
        header = self.linux_document(run, turbo_before="disabled")["header"]
        self.assertTrue(header["turbo"].startswith("unavailable: "), header["turbo"])
        self.assertIn("'disabled' before the first case and 'enabled' after the last",
                      header["turbo"])
        self.assertIn("header.turbo: Linux must record it",
                      CHECK_RESULTS.check_linux(header, ["itch_decode"]))

    def test_turbo_not_read_before_the_run_is_not_stated(self):
        run = self.linux_case(self.make_wrapper())
        header = self.linux_document(run, turbo_before=None)["header"]
        self.assertIn("header.turbo: Linux must record it",
                      CHECK_RESULTS.check_linux(header, ["itch_decode"]))

    def test_turbo_is_read_from_the_given_sysfs_and_the_pinned_core_comes_first(self):
        self.assertEqual(RUN_BENCH.linux_turbo(self.sysfs), "enabled")
        self.boost.write_text("0\n", encoding="ascii")
        self.assertEqual(RUN_BENCH.linux_turbo(self.sysfs), "disabled")
        core = self.sysfs / "cpu4" / "cpufreq" / "boost"
        core.parent.mkdir(parents=True)
        core.write_text("1\n", encoding="ascii")
        self.assertEqual(RUN_BENCH.linux_turbo(self.sysfs), "enabled")

    def test_with_no_boost_switch_turbo_is_unavailable_with_why_and_the_validator_refuses(self):
        self.boost.unlink()
        run = self.linux_case(self.make_wrapper())
        header = self.linux_document(run, RUN_BENCH.linux_turbo(self.sysfs))["header"]
        state, _, reason = header["turbo"].partition(": ")
        self.assertEqual(state, "unavailable")
        self.assertIn("no cpufreq boost switch", reason)
        self.assertIn("header.turbo: Linux must record it",
                      CHECK_RESULTS.check_linux(header, ["itch_decode"]))
        self.assertEqual(CHECK_RESULTS.check_leaks(self.linux_document(run)), [])

    @unittest.skipUnless(HOST == "Linux", "the pinned path exists only on Linux")
    def test_a_linux_run_with_no_boost_switch_publishes_nothing(self):
        self.boost.unlink()
        result = self.run_bench(self.make_binary(), self.make_wrapper())
        self.assertEqual(result.returncode, 1)
        self.assertIn("header.turbo: Linux must record it", result.stderr)
        self.assertEqual(list(self.results.iterdir()), [])

    def test_a_wait_for_the_lock_is_outside_the_window(self):
        self.stat.with_suffix(".waited").write_text(STAT_AFTER_A_WAIT, encoding="ascii")
        wrapper = self.make_wrapper(WAITING_WRAPPER.replace("STAT", str(self.stat)))
        self.assertEqual(self.linux_case(wrapper)["sibling_busy_percent"], 0.5)
        self.assertEqual(self.stat.read_text(encoding="ascii"), STAT_AFTER_THE_WAIT_AND_CASE)

    def test_the_document_carries_the_reading_for_each_case(self):
        meter = RUN_BENCH.SiblingMeter(self.sysfs, self.stat)
        block = meter.block({"itch_decode": 0.5})
        self.assertEqual((block["core"], block["sibling"]), (4, 5))
        self.assertEqual(block["topology"], "thread_siblings_list")
        self.assertIn("not enforced", meter.statement())
        self.assertIn("cpu5", meter.statement())
        document = {"header": {"sibling": meter.statement(), "sibling_busy_percent": block}}
        self.assertEqual(CHECK_RESULTS.check_leaks(document), [])
        self.assertEqual(CHECK_RESULTS.check_sibling(document["header"], ["itch_decode"]), [])

    def test_the_sibling_is_read_and_never_assumed(self):
        self.write_topology("thread_siblings_list", "2,4")
        self.assertEqual(RUN_BENCH.SiblingMeter(self.sysfs, self.stat).sibling, 2)

    def test_core_cpus_list_is_the_fallback(self):
        (self.sysfs / "cpu4" / "topology" / "thread_siblings_list").unlink()
        self.write_topology("core_cpus_list", "4,7")
        meter = RUN_BENCH.SiblingMeter(self.sysfs, self.stat)
        self.assertEqual((meter.sibling, meter.topology), (7, "core_cpus_list"))

    def test_no_topology_is_refused(self):
        (self.sysfs / "cpu4" / "topology" / "thread_siblings_list").unlink()
        with self.assertRaisesRegex(RUN_BENCH.Failure, "no topology file"):
            RUN_BENCH.SiblingMeter(self.sysfs, self.stat)

    def test_a_core_with_no_sibling_is_refused(self):
        self.write_topology("thread_siblings_list", "4")
        with self.assertRaisesRegex(RUN_BENCH.Failure, "0 siblings"):
            RUN_BENCH.SiblingMeter(self.sysfs, self.stat)

    def test_a_sibling_missing_from_proc_stat_is_refused(self):
        self.write_topology("thread_siblings_list", "4,9")
        meter = RUN_BENCH.SiblingMeter(self.sysfs, self.stat)
        snapshot = self.dir / "snapshot"
        snapshot.write_text(STAT_BEFORE, encoding="ascii")
        with self.assertRaisesRegex(RUN_BENCH.Failure, "no cpu9"):
            meter.busy_percent(snapshot)

    def test_no_time_passing_is_not_an_idle_sibling(self):
        meter = RUN_BENCH.SiblingMeter(self.sysfs, self.stat)
        snapshot = self.dir / "snapshot"
        snapshot.write_text(STAT_BEFORE, encoding="ascii")
        with self.assertRaisesRegex(RUN_BENCH.Failure, "no jiffies passed"):
            meter.busy_percent(snapshot)

    def test_no_first_reading_is_refused(self):
        meter = RUN_BENCH.SiblingMeter(self.sysfs, self.stat)
        with self.assertRaisesRegex(RUN_BENCH.Failure, "no reading of cpu5"):
            meter.busy_percent(self.dir / "absent")


    # A seeded kernel benchmark: no slice, values per second, case names with slashes.

    def seeded_path(self):
        return self.results / f"{HOST}-simd_kernel-{self.commit[:12]}.json"

    def test_a_seeded_run_writes_a_file_the_validator_passes(self):
        result = self.run_bench(self.make_seeded_binary(), self.make_wrapper())
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        document = json.loads(self.seeded_path().read_text(encoding="utf-8"))
        self.assertEqual(document["input"]["kind"], "seeded")
        self.assertEqual([row["case"] for row in document["results"]], list(SEEDED_CASES))
        self.assertEqual(document["results"][0]["isa"].split(", ")[1], "scalar kernels")
        self.assertEqual(document["header"]["isa"].count("scalar kernels"), 1)
        throughput = document["results"][0]["throughput"]
        self.assertIn("values_per_second", throughput)
        self.assertNotIn("messages_per_second", throughput)
        checked = subprocess.run(
            [sys.executable, "-I", str(CHECK), "--pins", str(self.pins), str(self.seeded_path())],
            capture_output=True, text=True, check=False)
        self.assertEqual(checked.returncode, 0, msg=checked.stderr)

    def test_a_seeded_case_gets_no_slice_and_its_files_no_slashes(self):
        self.run_bench(self.make_seeded_binary(), self.make_wrapper())
        runs = [line for line in self.record.read_text(encoding="utf-8").splitlines()
                if line.startswith("binary ")]
        self.assertEqual(len(runs), len(SEEDED_CASES))
        for line in runs:
            self.assertNotIn("--slice=", line)
            self.assertNotIn("--expect-", line)
        self.assertIn("pfor_unpack_w=13_scalar.report", "".join(runs))

    def test_a_seeded_case_that_did_not_match_the_scalar_set_is_never_published(self):
        result = self.run_bench(self.make_seeded_binary(matched=False), self.make_wrapper())
        self.assertEqual(result.returncode, 1)
        self.assertIn("verification.matched", result.stderr)
        self.assertEqual(list(self.results.iterdir()), [])

    def test_an_unknown_input_kind_is_refused_before_any_case_runs(self):
        result = self.run_bench(self.make_seeded_binary(kind="guessed"), self.make_wrapper())
        self.assertEqual(result.returncode, 2)
        self.assertIn("'guessed'", result.stderr)
        self.assertFalse(self.record.exists())

    @unittest.skipUnless(HOST == "Linux", "the pinned path exists only on Linux")
    def test_linux_counts_a_seeded_case_per_value(self):
        self.run_bench(self.make_seeded_binary(), self.make_wrapper())
        document = json.loads(self.seeded_path().read_text(encoding="utf-8"))
        counters = document["results"][0]["counters"]
        self.assertEqual(counters["per_value"]["cycles"], 0.4)
        self.assertEqual(counters["gated_values"], 10000)

    # A real-data column decode: the pinned slice goes in, values per second come out.

    def column_path(self):
        return self.results / f"{HOST}-column_decode-{self.commit[:12]}.json"

    def test_a_column_run_writes_a_file_the_validator_passes(self):
        result = self.run_bench(self.make_column_binary(), self.make_wrapper())
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        document = json.loads(self.column_path().read_text(encoding="utf-8"))
        self.assertEqual(document["input"]["kind"], "column")
        self.assertEqual(document["input"]["sha256"], SHA)
        self.assertEqual([row["case"] for row in document["results"]], list(COLUMN_CASES))
        row = document["results"][0]
        self.assertIn("values_per_second", row["throughput"])
        self.assertEqual(row["ratio"]["value"], 10.0)
        self.assertEqual(row["ratio"]["bits_per_value"], 6.4)
        checked = subprocess.run(
            [sys.executable, "-I", str(CHECK), "--pins", str(self.pins), str(self.column_path())],
            capture_output=True, text=True, check=False)
        self.assertEqual(checked.returncode, 0, msg=checked.stderr)

    def test_a_column_case_gets_the_slice_and_its_pin(self):
        self.run_bench(self.make_column_binary(), self.make_wrapper())
        runs = [line for line in self.record.read_text(encoding="utf-8").splitlines()
                if line.startswith("binary ")]
        self.assertEqual(len(runs), len(COLUMN_CASES))
        for line in runs:
            self.assertIn(f"--slice={self.slice}", line)
            self.assertIn(f"--expect-sha256={SHA}", line)
            self.assertIn(f"--expect-messages={MESSAGES}", line)

    def test_a_column_case_that_did_not_give_back_its_column_is_never_published(self):
        result = self.run_bench(self.make_column_binary(matched=False), self.make_wrapper())
        self.assertEqual(result.returncode, 1)
        self.assertIn("give back its column", result.stderr)
        self.assertEqual(list(self.results.iterdir()), [])

    @unittest.skipUnless(HOST == "Linux", "the pinned path exists only on Linux")
    def test_linux_counts_a_column_case_per_value(self):
        self.run_bench(self.make_column_binary(), self.make_wrapper())
        document = json.loads(self.column_path().read_text(encoding="utf-8"))
        counters = document["results"][0]["counters"]
        self.assertEqual(counters["per_value"]["cycles"], 0.4)


if __name__ == "__main__":
    unittest.main()
