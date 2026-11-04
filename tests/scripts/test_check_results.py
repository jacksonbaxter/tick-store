#!/usr/bin/env python3
"""Tests for scripts/check_results.py. Run with:

    python3 -I tests/scripts/test_check_results.py

The validator runs as a subprocess, as a user would run it, on results files generated
under the repo's gitignored build/ directory. A check that cannot fail proves nothing,
so each rule gets one injected fault into an otherwise valid file, and the test
requires the exit code and the message naming that rule. The two valid files are shown
passing, so a fault test cannot pass by the file having been broken all along.
"""

import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_results.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_results"

COMMIT = "0123456789abcdef0123456789abcdef01234567"
SHA = "ab" * 32
MESSAGES = 1000
SLICE = "test-slice"

PINS = {"slices": {SLICE: {"sha256": SHA, "message_count": MESSAGES, "size": 5000}}}

START_MAC = "measure.sh: start load1=1.20 at=2026-10-30T20:00:00Z"
END_MAC = "measure.sh: end load1=1.40 exit=0 at=2026-10-30T20:00:10Z"
LINUX_TAIL = "cpus=4 governor=performance freq_khz=cpu4:3200000 perf_event_paranoid=2"
START_LINUX = f"measure.sh: start load1=0.05 at=2026-10-30T20:00:00Z {LINUX_TAIL}"
END_LINUX = f"measure.sh: end load1=0.90 exit=0 at=2026-10-30T20:00:10Z {LINUX_TAIL}"


def timer(host):
    if host == "macOS":
        return {"source": "mach_absolute_time", "calibrated_against": "CLOCK_MONOTONIC_RAW",
                "ticks_per_second": 24000000.0, "resolution_ticks": 1, "resolution_ns": 41.667,
                "read_cost_ns": 5.0, "min_publishable_ns": 416.67,
                "calibration_window_ns": 2e8}
    return {"source": "rdtscp+lfence", "calibrated_against": "CLOCK_MONOTONIC_RAW",
            "ticks_per_second": 1997940000.0, "resolution_ticks": 20, "resolution_ns": 10.01,
            "read_cost_ns": 19.0, "min_publishable_ns": 100.1, "calibration_window_ns": 2e8}


def counters(host):
    if host == "macOS":
        reason = "macOS has no unprivileged hardware counters"
        return {name: {"status": "unavailable", "reason": reason}
                for name in ("cycles", "instructions", "branch_misses", "cache_misses")}
    return {"cycles": {"value": 1000, "unit": "core cycles, user mode", "coverage_percent": 100.0},
            "instructions": {"value": 2000, "unit": "instructions, user mode",
                             "coverage_percent": 100.0},
            "branch_misses": {"value": 30, "unit": "mispredicted branches, user mode",
                              "coverage_percent": 100.0},
            "cache_misses": {"status": "unavailable", "reason": "not confirmed on Zen 1"},
            "per_message": {"cycles": 40.0, "instructions": 80.0, "branch_misses": 0.3}}


def sibling_block():
    return {"core": 4, "sibling": 5, "topology": "thread_siblings_list", "method": "jiffies",
            "cases": {"itch_decode": 0.5}}


def valid_document(host):
    linux = host == "Linux"
    header = {
        "schema": 1,
        "host": host,
        "measurement": "reported",
        "statement": "Pinned." if linux else "Unpinned. No hardware counters.",
        "cpu_model": "AMD Ryzen Embedded V1605B" if linux else "Apple M4 Max",
        "kernel": "Linux 6.12.0" if linux else "Darwin 27.2.0",
        "compiler": "Clang 23.1.4",
        "compile_flags": "-O3 -DNDEBUG -std=c++23",
        "build_type": "Release",
        "sanitizers": "none",
        "isa": "x86-64 baseline, scalar kernels" if linux else "arm64 baseline, scalar kernels",
        "commit": COMMIT,
        "core": 4 if linux else "not-available",
        "sibling": "cpu5 idle as stated, not enforced" if linux else "not-available",
        "sibling_busy_percent": sibling_block() if linux else "not-available",
        "second_thread": "cpu6 reserved, unused" if linux else "not-available",
        "governor": "performance" if linux else "not-available",
        "frequency": ({"snapshot_khz": [3200000, 3200000], "effective_mhz": {"itch_decode": 3390.0}}
                      if linux else "not-available"),
        "turbo": "enabled" if linux else "not-available",
        "pinning": "pinned" if linux else "not-supported",
        "counters": "perf stat, :u events" if linux else "unavailable",
        "timer": timer(host),
        "load_lines": [START_LINUX, END_LINUX] if linux else [START_MAC, END_MAC],
        "peak_rss_bytes": 500000000,
    }
    result = {
        "case": "itch_decode",
        "host": host,
        "timer": timer(host),
        "warmup": {"passes_per_repetition": 2, "first_repetition_pass_ms": [90.0, 89.0]},
        "verification": {"messages": MESSAGES, "bytes": 5000, "input_sha256": SHA,
                         "reencoded_sha256": SHA},
        "throughput": {"repetitions": 10, "pass_ms_median": 90.0,
                       "messages_per_second": {"median": 6e7, "mean": 6e7, "stddev": 1e5,
                                               "cv": 0.01}},
        "counters": counters(host),
        "ratio": {"status": "not-applicable", "reason": "decode only"},
        "latency": {"unit": "ns per batch", "batch_messages": 1024, "batches": 5000,
                    "batch_phase_seed": 1, "timer_resolution_ns": header["timer"]["resolution_ns"],
                    "min_publishable_ns": header["timer"]["min_publishable_ns"],
                    "significant_figures": 3, "min_ns": 15000.0, "max_ns": 90000.0,
                    "percentiles": [{"percentile": 50.0, "value_ns": 20000.0},
                                    {"percentile": 99.0, "value_ns": 30000.0}]},
    }
    return {"header": header, "benchmark": "itch_codec",
            "input": {"slice": SLICE, "sha256": SHA, "messages": MESSAGES},
            "results": [result]}

def seeded_document(host):
    """A kernel micro-benchmark's file: input made from a seed, so values, not messages."""
    document = valid_document(host)
    document["benchmark"] = "simd_kernel"
    document["input"] = {"kind": "seeded", "items": "values",
                         "seed": "each case makes its own input from a fixed seed"}
    result = document["results"][0]
    result["case"] = "pfor_unpack/w=13/scalar"
    result["verification"] = {"input_seed": 13, "items_per_call": 1024,
                              "calls_per_pass": 8192, "against": "the scalar set",
                              "matched": True}
    throughput = result["throughput"]
    throughput["values_per_second"] = throughput.pop("messages_per_second")
    result["latency"]["batch_items"] = result["latency"].pop("batch_messages")
    result["ratio"] = {"status": "not-applicable", "reason": "a decode kernel"}
    if host == "Linux":
        counters_block = result["counters"]
        counters_block["per_value"] = counters_block.pop("per_message")
        document["header"]["sibling_busy_percent"]["cases"] = {result["case"]: 0.5}
        document["header"]["frequency"]["effective_mhz"] = {result["case"]: 3390.0}
    return document


def remove(document, *keys):
    target = document
    for key in keys[:-1]:
        target = target[key]
    del target[keys[-1]]


def set_value(document, value, *keys):
    target = document
    for key in keys[:-1]:
        target = target[key]
    target[keys[-1]] = value


class CheckResultsTest(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.dir)
        self.pins = self.dir / "pins.json"
        self.pins.write_text(json.dumps(PINS), encoding="utf-8")

    def run_check(self, document, name):
        path = self.dir / name
        path.write_text(json.dumps(document), encoding="utf-8")
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins), str(path)],
            capture_output=True, text=True, check=False)

    def check(self, host, mutate=None, name=None):
        document = copy.deepcopy(valid_document(host))
        if mutate is not None:
            mutate(document)
        return self.run_check(document, name or f"{host}-itch_codec-{COMMIT[:12]}.json")

    def assert_fails(self, host, mutate, expected, name=None):
        result = self.check(host, mutate, name)
        self.assertEqual(result.returncode, 1, msg=result.stdout + result.stderr)
        self.assertIn(expected, result.stderr)

    # The controls.

    def test_a_valid_linux_file_passes(self):
        result = self.check("Linux")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_a_valid_macos_file_passes(self):
        result = self.check("macOS")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_a_clean_unavailable_marking_passes_on_linux(self):
        def mark(document):
            document["results"][0]["counters"]["cycles"] = {
                "status": "unavailable", "reason": "perf said <not counted>"}
        self.assertEqual(self.check("Linux", mark).returncode, 0)

    def test_a_repo_relative_path_is_not_a_leak(self):
        def add(document):
            document["input"]["where"] = "data/slices/12302019-open.itch"
        self.assertEqual(self.check("Linux", add).returncode, 0)

    # Missing header fields, each one.

    def test_every_header_field_is_required(self):
        fields = ("schema", "host", "measurement", "statement", "cpu_model", "kernel", "compiler",
                  "compile_flags", "build_type", "sanitizers", "isa", "commit", "core", "sibling",
                  "sibling_busy_percent", "second_thread", "governor", "frequency", "turbo",
                  "pinning", "counters", "timer", "load_lines", "peak_rss_bytes")
        for field in fields:
            with self.subTest(field=field):
                self.assert_fails("Linux", lambda d, f=field: remove(d, "header", f),
                                  f"'{field}'")

    def test_every_timer_field_is_required(self):
        for field in ("source", "calibrated_against", "ticks_per_second", "resolution_ns",
                      "read_cost_ns", "min_publishable_ns"):
            with self.subTest(field=field):
                self.assert_fails("Linux", lambda d, f=field: remove(d, "header", "timer", f),
                                  f"header.timer: missing field '{field}'")

    def test_the_header_must_come_first(self):
        def move(document):
            header = document.pop("header")
            document["header"] = header
        self.assert_fails("Linux", move, "does not start with its header")

    # macOS claiming what it cannot have.

    def test_macos_claiming_pinned(self):
        self.assert_fails("macOS", lambda d: set_value(d, "pinned", "header", "pinning"),
                          "macOS cannot be pinned")

    def test_macos_claiming_a_core(self):
        self.assert_fails("macOS", lambda d: set_value(d, 4, "header", "core"),
                          "macOS claims core 4")

    def test_macos_claiming_counters_in_the_header(self):
        self.assert_fails("macOS",
                          lambda d: set_value(d, "perf stat", "header", "counters"),
                          "macOS claims perf stat")

    def test_macos_claiming_a_counter_value(self):
        self.assert_fails(
            "macOS", lambda d: set_value(d, {"value": 5}, "results", 0, "counters", "cycles"),
            "macOS claims a hardware counter")

    def test_macos_not_stating_it_plainly(self):
        self.assert_fails("macOS", lambda d: set_value(d, "Fine.", "header", "statement"),
                          "state plainly")

    def test_proc_pid_rusage_in_place_of_counters(self):
        self.assert_fails(
            "macOS",
            lambda d: set_value(d, "from proc_pid_rusage", "results", 0, "counters", "cycles",
                                "reason"),
            "proc_pid_rusage is not used")

    # Linux missing what it must record.

    def test_linux_without_a_core(self):
        self.assert_fails("Linux", lambda d: set_value(d, "not-available", "header", "core"),
                          "header.core: expected 4")

    def test_linux_without_a_governor(self):
        self.assert_fails("Linux", lambda d: set_value(d, "unavailable", "header", "governor"),
                          "header.governor: Linux must record it")

    def test_linux_without_a_frequency(self):
        self.assert_fails("Linux", lambda d: set_value(d, "not-available", "header", "frequency"),
                          "header.frequency: Linux must record observed frequency")

    def test_linux_without_turbo(self):
        self.assert_fails("Linux", lambda d: set_value(d, "unavailable", "header", "turbo"),
                          "header.turbo: Linux must record it")

    def test_linux_without_turbo_but_with_the_reason(self):
        # What run_bench.py records on a host with no boost switch, such as a CI runner.
        self.assert_fails("Linux", lambda d: set_value(
            d, "unavailable: no cpufreq boost switch, as on a virtual machine", "header", "turbo"),
                          "header.turbo: Linux must record it")

    def test_linux_with_an_unknown_turbo_state(self):
        self.assert_fails("Linux", lambda d: set_value(d, "partly", "header", "turbo"),
                          "header.turbo: unknown value partly")

    def test_linux_not_pinned(self):
        self.assert_fails("Linux", lambda d: set_value(d, "not-requested", "header", "pinning"),
                          "Linux results must be pinned")

    def test_linux_claiming_the_sibling_is_idle(self):
        self.assert_fails("Linux", lambda d: set_value(d, "cpu5 idle", "header", "sibling"),
                          "recorded as stated, not claimed")

    # The core's SMT sibling, measured.

    def test_a_sibling_at_half_a_percent_is_accepted(self):
        self.assertEqual(self.check("Linux").returncode, 0)

    def test_a_sibling_at_the_limit_is_accepted(self):
        def at_limit(document):
            set_value(document, 5.0, "header", "sibling_busy_percent", "cases", "itch_decode")
        self.assertEqual(self.check("Linux", at_limit).returncode, 0)

    def test_a_sibling_at_6_percent_is_refused(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, 6.0, "header", "sibling_busy_percent", "cases",
                                              "itch_decode"),
                          "itch_decode reads 6.0%, over the 5.0% limit")

    def test_linux_with_no_sibling_reading(self):
        self.assert_fails("Linux", lambda d: remove(d, "header", "sibling_busy_percent"),
                          "missing field 'sibling_busy_percent'")

    def test_linux_marking_the_sibling_not_available(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, "not-available", "header",
                                              "sibling_busy_percent"),
                          "Linux must measure it, case by case")

    def test_a_case_with_no_sibling_reading(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, {"other": 0.5}, "header", "sibling_busy_percent",
                                              "cases"),
                          "no reading for itch_decode")

    def test_a_sibling_reading_that_is_not_a_percentage(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, -1, "header", "sibling_busy_percent", "cases",
                                              "itch_decode"),
                          "which is not a percentage")

    def test_a_sibling_reading_for_the_wrong_core(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, 2, "header", "sibling_busy_percent", "core"),
                          "core 2 is not the pinned core 4")

    def test_the_core_read_as_its_own_sibling(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, 4, "header", "sibling_busy_percent", "sibling"),
                          "4 is not a sibling of the core")

    def test_a_sibling_reading_with_no_method(self):
        self.assert_fails("Linux",
                          lambda d: remove(d, "header", "sibling_busy_percent", "method"),
                          "no method")

    def test_macos_claiming_a_sibling_reading(self):
        self.assert_fails("macOS",
                          lambda d: set_value(d, sibling_block(), "header",
                                              "sibling_busy_percent"),
                          "macOS has no pinned core")

    # No lock, no reported result.

    def test_no_load_lines(self):
        self.assert_fails("Linux", lambda d: set_value(d, [], "header", "load_lines"),
                          "no load lines")

    def test_marked_iteration_only(self):
        self.assert_fails("macOS",
                          lambda d: set_value(d, "iteration-only", "header", "measurement"),
                          "iteration-only, so this is not a reported result")

    def test_a_start_line_with_no_end_line(self):
        self.assert_fails("Linux", lambda d: d["header"]["load_lines"].pop(),
                          "expected a start and an end")

    def test_a_forged_load_line(self):
        self.assert_fails("Linux", lambda d: set_value(d, ["load was fine"] * 2, "header",
                                                       "load_lines"),
                          "not a measure.sh load line")

    def test_a_run_that_failed_under_the_lock(self):
        def fail_it(document):
            document["header"]["load_lines"][1] = END_LINUX.replace("exit=0", "exit=1")
        self.assert_fails("Linux", fail_it, "ended with exit 1")

    # Anything that would identify the machine or the user.

    def test_a_home_path_in_the_flags(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, "-O3 -I/home/someone/include", "header",
                                              "compile_flags"),
                          "a home-style path")

    def test_a_mac_home_path_anywhere(self):
        self.assert_fails("macOS",
                          lambda d: set_value(d, "/Users/someone/data", "input", "note"),
                          "a home-style path")

    def test_a_tilde_path(self):
        self.assert_fails("Linux", lambda d: set_value(d, "~/quant/data", "input", "note"),
                          "a home-style path")

    def test_any_absolute_path(self):
        self.assert_fails("Linux", lambda d: set_value(d, "/opt/tools/bin", "input", "note"),
                          "an absolute path")

    def test_an_include_flag(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, "-O3 -Iinclude", "header", "compile_flags"),
                          "an include flag")

    def test_a_hostname_in_a_load_line(self):
        def add(document):
            document["header"]["load_lines"][0] = START_LINUX.replace(
                " at=", " host=somebox at=", 1)
        self.assert_fails("Linux", add, "a hostname field")

    def test_a_hostname_field(self):
        self.assert_fails("Linux", lambda d: set_value(d, "somebox", "header", "host_name"),
                          "identifies a machine")

    def test_an_ip_address(self):
        self.assert_fails("Linux", lambda d: set_value(d, "reached 10.1.2.3", "input", "note"),
                          "an IP address")

    def test_a_network_name(self):
        self.assert_fails("Linux", lambda d: set_value(d, "box.local", "input", "note"),
                          "a network name")

    def test_this_machines_own_user_name(self):
        import getpass
        self.assert_fails("Linux",
                          lambda d: set_value(d, f"run by {getpass.getuser()}", "input", "note"),
                          "this machine's name")

    # Latency the timer cannot resolve.

    def test_a_percentile_below_the_timer_floor(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, 90.0, "results", 0, "latency", "percentiles", 0,
                                              "value_ns"),
                          "below the 100.1 ns the timer can resolve")

    def test_a_percentile_below_the_mac_tick_floor(self):
        self.assert_fails("macOS",
                          lambda d: set_value(d, 125.0, "results", 0, "latency", "percentiles", 1,
                                              "value_ns"),
                          "below the 416.67 ns the timer can resolve")

    def test_a_minimum_below_the_timer_floor(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, 50.0, "results", 0, "latency", "min_ns"),
                          "min_ns: 50.0 ns is below")

    def test_percentiles_without_a_batch_size(self):
        self.assert_fails("Linux", lambda d: remove(d, "results", 0, "latency", "batch_messages"),
                          "'batch_messages' must sit next to the percentiles")

    def test_percentiles_without_a_unit(self):
        self.assert_fails("Linux", lambda d: remove(d, "results", 0, "latency", "unit"),
                          "'unit' must sit next to the percentiles")

    def test_percentiles_without_a_resolution(self):
        self.assert_fails("Linux",
                          lambda d: remove(d, "results", 0, "latency", "timer_resolution_ns"),
                          "'timer_resolution_ns' must sit next to the percentiles")

    def test_a_timer_that_was_never_calibrated(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, "assumed", "results", 0, "timer",
                                              "calibrated_against"),
                          "not calibrated against CLOCK_MONOTONIC_RAW")

    # Two hosts in one file.

    def test_a_macos_result_in_a_linux_file(self):
        self.assert_fails("Linux", lambda d: set_value(d, "macOS", "results", 0, "host"),
                          "mixed hosts: results[0].host is macOS")

    def test_a_linux_row_appended_to_a_macos_file(self):
        def mix(document):
            document["results"].append(valid_document("Linux")["results"][0])
            document["header"]["load_lines"] *= 2
        self.assert_fails("macOS", mix, "mixed hosts: results[1].host is Linux")

    def test_a_timer_from_the_other_host(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, "mach_absolute_time", "header", "timer", "source"),
                          "mixed hosts: header.timer.source")

    def test_an_isa_from_the_other_host(self):
        self.assert_fails("macOS", lambda d: set_value(d, "x86-64 baseline", "header", "isa"),
                          "mixed hosts: header.isa")

    def test_a_kernel_from_the_other_host(self):
        self.assert_fails("macOS", lambda d: set_value(d, "Linux 6.12.0", "header", "kernel"),
                          "mixed hosts: header.kernel")

    def test_a_linux_load_line_in_a_macos_file(self):
        self.assert_fails("macOS", lambda d: set_value(d, [START_LINUX, END_LINUX], "header",
                                                       "load_lines"),
                          "a Linux load line in a macOS file")

    def test_a_file_named_for_the_other_host(self):
        self.assert_fails("Linux", None, "does not match header.host",
                          name=f"macOS-itch_codec-{COMMIT[:12]}.json")

    def test_an_unknown_host_label(self):
        self.assert_fails("Linux", lambda d: set_value(d, "debian", "header", "host"),
                          "header.host: 'debian' is not one of")

    # Verification, build and metrics.

    def test_output_that_does_not_match_the_pin(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, "cd" * 32, "results", 0, "verification",
                                              "reencoded_sha256"),
                          "reencoded_sha256: not the slice's pin")

    def test_an_input_with_no_pin(self):
        self.assert_fails("Linux", lambda d: set_value(d, "other", "input", "slice"),
                          "'other' has no pin")

    def test_a_debug_build(self):
        self.assert_fails("Linux", lambda d: set_value(d, "Debug", "header", "build_type"),
                          "Debug is not Release")

    def test_a_sanitizer_build(self):
        self.assert_fails("Linux", lambda d: set_value(d, "tsan", "header", "sanitizers"),
                          "would distort any number")

    def test_flags_without_o3(self):
        self.assert_fails("Linux", lambda d: set_value(d, "-O1", "header", "compile_flags"),
                          "no -O3")

    def test_a_commit_that_is_not_a_commit(self):
        self.assert_fails("Linux", lambda d: set_value(d, "unknown", "header", "commit"),
                          "not a full commit hash")

    def test_a_counter_simply_missing(self):
        self.assert_fails("Linux", lambda d: remove(d, "results", 0, "counters", "cache_misses"),
                          "missing, and not marked unavailable")

    def test_a_counter_with_no_reason(self):
        self.assert_fails("Linux",
                          lambda d: set_value(d, {"status": "unavailable"}, "results", 0,
                                              "counters", "cycles"),
                          "neither a value nor marked unavailable")

    def test_no_ratio_field(self):
        self.assert_fails("Linux", lambda d: remove(d, "results", 0, "ratio"), "ratio: missing")

    def test_warm_up_not_recorded(self):
        self.assert_fails("Linux", lambda d: remove(d, "results", 0, "warmup"),
                          "warm-up must be explicit and recorded")

    def test_no_spread_across_repetitions(self):
        self.assert_fails("Linux",
                          lambda d: remove(d, "results", 0, "throughput", "messages_per_second",
                                           "cv"),
                          "no variability")

    def test_a_file_with_no_results(self):
        self.assert_fails("Linux", lambda d: set_value(d, [], "results"), "no results")

    def test_a_file_that_is_not_json(self):
        path = self.dir / "Linux-broken.json"
        path.write_text("{", encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins), str(path)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot read it", result.stderr)

    def test_a_second_bad_file_still_fails_the_run(self):
        good = self.dir / f"Linux-itch_codec-{COMMIT[:12]}.json"
        good.write_text(json.dumps(valid_document("Linux")), encoding="utf-8")
        bad = self.dir / "Linux-empty.json"
        bad.write_text("[]", encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins), str(good), str(bad)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)


    # A seeded kernel benchmark: no slice to pin, so the seed and the match stand in for it.

    def check_seeded(self, host, mutate=None):
        document = copy.deepcopy(seeded_document(host))
        if mutate is not None:
            mutate(document)
        return self.run_check(document, f"{host}-simd_kernel-{COMMIT[:12]}.json")

    def assert_seeded_fails(self, host, mutate, expected):
        result = self.check_seeded(host, mutate)
        self.assertEqual(result.returncode, 1, msg=result.stdout + result.stderr)
        self.assertIn(expected, result.stderr)

    def test_a_valid_seeded_file_passes_on_both_hosts(self):
        for host in ("Linux", "macOS"):
            with self.subTest(host=host):
                result = self.check_seeded(host)
                self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_a_seeded_file_must_name_its_seed(self):
        self.assert_seeded_fails(
            "Linux", lambda d: remove(d, "results", 0, "verification", "input_seed"),
            "verification.input_seed")

    def test_a_seeded_file_must_show_the_output_matched(self):
        self.assert_seeded_fails(
            "Linux", lambda d: set_value(d, False, "results", 0, "verification", "matched"),
            "verification.matched")

    def test_a_seeded_file_counts_values_not_messages(self):
        def rename(document):
            throughput = document["results"][0]["throughput"]
            throughput["messages_per_second"] = throughput.pop("values_per_second")
        self.assert_seeded_fails("Linux", rename, "throughput: no median")

    def test_a_seeded_latency_needs_its_batch_size(self):
        self.assert_seeded_fails(
            "macOS", lambda d: remove(d, "results", 0, "latency", "batch_items"),
            "batch_items")

    def test_a_slice_file_cannot_drop_its_pin_by_omitting_the_kind(self):
        self.assert_fails("Linux", lambda d: remove(d, "input", "slice"), "has no pin")


if __name__ == "__main__":
    os.environ.pop("PYTHONPATH", None)
    unittest.main()
