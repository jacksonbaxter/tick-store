#!/usr/bin/env python3
"""Tests for scripts/check_compare.py. Run with:

    python3 -I tests/scripts/test_check_compare.py

The validator runs as a subprocess, as a user would run it, on results files generated under
the repo's gitignored build/ directory. A check that cannot fail proves nothing, so each rule
gets one injected fault in an otherwise valid file, and the test requires the exit code and
the message naming that rule. Both hosts' valid files are shown passing first.
"""

import copy
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_compare.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_compare"

COMMIT = "0123456789abcdef0123456789abcdef01234567"
SHA = "ab" * 32
MESSAGES = 1000
BYTES = 20000
SLICE = "test-slice"
QUERY_NAMES = ("Q1", "Q2", "Q3", "Q4", "Q5")
PROVEN = "proven"
PINS = {"slices": {SLICE: {"sha256": SHA, "message_count": MESSAGES, "size": BYTES}}}
LOAD_MAC = ["measure.sh: start load1=1.00 at=2026-10-30T00:00:00Z",
            "measure.sh: end load1=1.00 exit=0 at=2026-10-30T00:00:01Z"]
LOAD_LINUX = [
    "measure.sh: start load1=0.10 at=2026-10-30T00:00:00Z cpus=4 governor=performance "
    "freq_khz=cpu4:3200000 perf_event_paranoid=1",
    "measure.sh: end load1=0.10 exit=0 at=2026-10-30T00:00:01Z cpus=4 governor=performance "
    "freq_khz=cpu4:3200000 perf_event_paranoid=1",
]


def digest(name):
    return {"rows": 5, "sha256": name.encode("ascii").hex().ljust(64, "0")}


def setting(name, value):
    return {"name": name, "requested": value, "in_force": value, "read_from": "somewhere"}


def settings_of(system, durability):
    if system != "questdb":
        return [setting("threads", 2)]
    mode = {"A": "nosync", "B": "sync"}[durability]
    return [setting(f"cairo.commit.mode ({source})", mode)
            for source in ("conf file", "echo", "parsed")]


def flush(dirty, parquet_calls=None):
    block = {"rows": 10, "seconds": 0.1, "data_dirty_pages": dirty, "data_dirty_files": {},
             "residual_dirty_files": {}}
    if parquet_calls is not None:
        block["harness_flushes"] = parquet_calls
    return block


def durability_block(system, name, host):
    dirty = 5 if name == "A" else 0
    calls = None if system != "parquet" else (0 if name == "A" else 2)
    return {"class": name, "sync_primitive": "fsync", "flush_granularity": "per file",
            "like_for_like": not (host == "macOS" and system == "questdb"), "note": "n",
            "status": PROVEN, "reason": "shown",
            "flush": {"probe": flush(dirty, calls), "full_ingest": flush(dirty, calls)}}


def class_entry(system, name, host):
    return {
        "durability": durability_block(system, name, host),
        "settings": settings_of(system, name),
        "write": {"seconds_median": 2.0, "records_per_second_median": MESSAGES / 2.0,
                  "python_seconds": 0.1, "engine_seconds": 1.9, "python_note": "n",
                  "peak_rss_bytes_client": 1000},
        "stored": {"allocated_bytes": 10000, "ratio_stored_over_source": 10000 / BYTES,
                   "method": "st_blocks", "denominator": "source", "measured": "at rest"},
        "read_back": {"oracle": {"verdict": "pass", "exit_code": 0, "records": MESSAGES,
                                 "mismatched_records": 0, "hashes_match": True,
                                 "source_sha256": SHA, "stored_sha256": SHA},
                      "stored_settings": {"checks_passed": 3}},
    }


def query_entry(name):
    cold_proof = {"resident_after_eviction": 0, "evicted_pages": 10, "method": "m",
                  "resident_bytes_after_run": 4096, "device_bytes_read": 8192, "device_slack": 0}
    return {"digest": digest(name), "matches_ground_truth": True,
            "scan": {"rows_decoded": 5},
            "warm": {"n": 3, "median_us": 10.0,
                     "proof": {"pages": 8, "resident_before": 8, "resident_after": 8}},
            "cold": {"n": 2, "median_us": 100.0, "proofs": [cold_proof, dict(cold_proof)]}}


def system_block(system, host):
    return {
        "versions": {system: "1.0"}, "headline_variant": "v", "threads": 2,
        "pinning": {"status": "verified", "cpus": [4, 6]} if host == "Linux" else {"cpus": []},
        "mem_available_bytes": ({"lowest": 1, "highest": 2, "samples": 2}
                                if host == "Linux" else "not-available"),
        "children_peak_rss_bytes": 1000,
        "variants": {"v": {
            "tuning": {"k": 1},
            "classes": {name: class_entry(system, name, host) for name in ("A", "B")},
            "queries": {name: query_entry(name) for name in QUERY_NAMES},
        }},
    }


def valid_document(host):
    linux = host == "Linux"
    return {
        "header": {
            "schema": 1, "host": host, "measurement": "reported",
            "statement": "Pinned." if linux else "Unpinned. No hardware counters.",
            "cpu_model": "cpu", "kernel": "Linux 6.12" if linux else "Darwin 27.2.0",
            "compiler": "clang 23", "compile_flags": "-O3 -DNDEBUG", "build_type": "Release",
            "commit": COMMIT, "core": 4 if linux else "not-available",
            "second_thread": "cpu6" if linux else "not-available",
            "sibling": "cpu5 idle as stated, not enforced" if linux else "not-available",
            "governor": "performance" if linux else "not-available",
            "turbo": "enabled" if linux else "not-available",
            "pinning": "pinned" if linux else "not-supported",
            "counters": "unavailable",
            "load_lines": (LOAD_LINUX if linux else LOAD_MAC) * 3,
        },
        "label": "Slice numbers. They are not claims about a full day, and no target changes.",
        "heap": "The heap for a full day is unchecked." if linux else "-Xmx1g",
        "input": {"slice": SLICE, "sha256": SHA, "messages": MESSAGES, "bytes": BYTES,
                  "ratio_baseline": "source bytes", "clipping": "Q2 is clipped to the slice"},
        "method": {"warm": "how", "cold": "how"},
        "queries": {"parameters": [], "truth": {name: digest(name) for name in QUERY_NAMES}},
        "systems": {name: system_block(name, host)
                    for name in ("parquet", "arcticdb", "questdb")},
    }


class Harness(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.pins = self.directory / "pins.json"
        self.pins.write_text(json.dumps(PINS), encoding="utf-8")

    def run_on(self, document, host="macOS"):
        path = self.directory / f"{host}-{SLICE}-{COMMIT[:12]}.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins),
                               str(path)], check=False, capture_output=True, text=True)

    def assert_fault(self, mutate, expected, host="macOS"):
        document = copy.deepcopy(valid_document(host))
        mutate(document)
        result = self.run_on(document, host)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(expected, result.stderr)


def b_of(document, system="questdb"):
    return document["systems"][system]["variants"]["v"]["classes"]["B"]


def q1_of(document, system="parquet"):
    return document["systems"][system]["variants"]["v"]["queries"]["Q1"]


class Valid(Harness):
    def test_the_valid_files_pass(self):
        for host in ("macOS", "Linux"):
            result = self.run_on(valid_document(host), host)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(": ok", result.stdout)


class Faults(Harness):
    # Settings.

    def test_a_setting_that_differs(self):
        def fault(document):
            b_of(document, "arcticdb")["settings"][0]["in_force"] = 3
        self.assert_fault(fault, "threads requested 2, in force 3")

    def test_no_settings_at_all(self):
        def fault(document):
            b_of(document, "parquet")["settings"] = []
        self.assert_fault(fault, "nothing read back")

    def test_a_setting_with_no_source(self):
        def fault(document):
            b_of(document, "parquet")["settings"][0]["read_from"] = ""
        self.assert_fault(fault, "does not say where")

    def test_a_commit_mode_of_not_a_mode(self):
        def fault(document):
            for item in b_of(document)["settings"]:
                item["requested"] = item["in_force"] = "not_a_mode"
        self.assert_fault(fault, "is 'not_a_mode', not sync")

    def test_a_commit_mode_with_no_parser_proof(self):
        def fault(document):
            settings = b_of(document)["settings"]
            b_of(document)["settings"] = [item for item in settings if "parsed" not in item["name"]]
        self.assert_fault(fault, "commit mode from 'cairo.commit.mode (parsed)'")

    # Read back.

    def test_a_missing_read_back(self):
        def fault(document):
            del b_of(document, "arcticdb")["read_back"]["oracle"]
        self.assert_fault(fault, "missing read-back proof")

    def test_the_oracle_saying_mismatch(self):
        def fault(document):
            b_of(document)["read_back"]["oracle"]["verdict"] = "mismatch"
        self.assert_fault(fault, "the oracle said mismatch")

    def test_a_stored_hash_that_is_not_the_pin(self):
        def fault(document):
            b_of(document)["read_back"]["oracle"]["stored_sha256"] = "cd" * 32
        self.assert_fault(fault, "stored_sha256 is not the pin")

    def test_mismatched_records(self):
        def fault(document):
            b_of(document)["read_back"]["oracle"]["mismatched_records"] = 1
        self.assert_fault(fault, "differs from the source")

    def test_no_stored_settings(self):
        def fault(document):
            b_of(document)["read_back"]["stored_settings"] = {}
        self.assert_fault(fault, "no stored settings")

    # Queries.

    def test_a_query_that_differs(self):
        def fault(document):
            q1_of(document)["digest"]["rows"] = 6
        self.assert_fault(fault, "differs from the ground truth")

    def test_a_missing_query(self):
        def fault(document):
            del document["systems"]["questdb"]["variants"]["v"]["queries"]["Q4"]
        self.assert_fault(fault, "Q4 missing")

    # Warm and cold.

    def test_a_warm_number_with_no_proof(self):
        def fault(document):
            del q1_of(document)["warm"]["proof"]
        self.assert_fault(fault, "a warm number without residency")

    def test_a_warm_number_with_a_page_out(self):
        def fault(document):
            q1_of(document)["warm"]["proof"]["resident_after"] = 7
        self.assert_fault(fault, "a warm number without residency")

    def test_a_cold_number_with_no_proof(self):
        def fault(document):
            q1_of(document)["cold"]["proofs"] = []
        self.assert_fault(fault, "a cold number without a proof")

    def test_a_cold_number_with_eviction_unproven(self):
        def fault(document):
            q1_of(document)["cold"]["proofs"][0]["resident_after_eviction"] = 128
        self.assert_fault(fault, "eviction not proven")

    def test_a_cold_number_with_a_device_shortfall(self):
        def fault(document):
            q1_of(document)["cold"]["proofs"][1]["device_bytes_read"] = 1
        self.assert_fault(fault, "the device read 1 bytes")

    # Class B.

    def test_class_b_claimed_with_dirty_pages(self):
        def fault(document):
            b_of(document, "arcticdb")["durability"]["flush"]["probe"]["data_dirty_pages"] = 4
        self.assert_fault(fault, "class B claimed with 4 dirty pages")

    def test_class_b_claimed_with_no_flush_record(self):
        def fault(document):
            b_of(document, "arcticdb")["durability"]["flush"] = {}
        self.assert_fault(fault, "class B claimed with None dirty pages")

    def test_class_b_claimed_without_the_harness_flush(self):
        def fault(document):
            b_of(document, "parquet")["durability"]["flush"]["full_ingest"]["harness_flushes"] = 0
        self.assert_fault(fault, "without the harness flush")

    def test_class_b_claimed_with_no_working_control(self):
        def fault(document):
            classes = document["systems"]["questdb"]["variants"]["v"]["classes"]
            classes["A"]["durability"]["flush"]["probe"]["data_dirty_pages"] = 0
        self.assert_fault(fault, "class A's control")

    def test_class_b_with_table_pages_dirty_called_like_for_like(self):
        def fault(document):
            block = b_of(document)["durability"]
            block["flush"]["full_ingest"]["residual_dirty_files"] = {"2019-12-30/ts.d": 9}
        self.assert_fault(fault, "not like-for-like", "Linux")

    def test_macos_questdb_called_like_for_like(self):
        def fault(document):
            b_of(document)["durability"]["like_for_like"] = True
        self.assert_fault(fault, "must not be like-for-like")

    def test_a_status_that_is_neither(self):
        def fault(document):
            b_of(document)["durability"]["status"] = "probably"
        self.assert_fault(fault, "unknown status")

    def test_no_sync_primitive(self):
        def fault(document):
            b_of(document)["durability"]["sync_primitive"] = ""
        self.assert_fault(fault, "no sync primitive")

    def test_not_proven_is_valid_when_it_is_said_plainly(self):
        document = valid_document("macOS")
        block = b_of(document)["durability"]
        block["status"] = "NOT PROVEN"
        block["reason"] = "the parser probe could not run"
        block["flush"]["probe"]["data_dirty_pages"] = None
        self.assertEqual(self.run_on(document).returncode, 0)

    def test_not_proven_needs_a_reason(self):
        def fault(document):
            block = b_of(document)["durability"]
            block["status"] = "NOT PROVEN"
            block["reason"] = ""
        self.assert_fault(fault, "no reason")

    # The measurement.

    def test_missing_load_lines(self):
        def fault(document):
            document["header"]["load_lines"] = []
        self.assert_fault(fault, "no load lines")

    def test_a_load_line_short(self):
        def fault(document):
            document["header"]["load_lines"].pop()
        self.assert_fault(fault, "expected a start and an end")

    def test_an_iteration_file(self):
        def fault(document):
            document["header"]["measurement"] = "iteration-only"
        self.assert_fault(fault, "iteration-only")

    def test_a_run_that_failed(self):
        def fault(document):
            lines = document["header"]["load_lines"]
            lines[1] = lines[1].replace("exit=0", "exit=1")
        self.assert_fault(fault, "ended with exit 1")

    # Leaks.

    def test_a_home_path(self):
        def fault(document):
            b_of(document)["durability"]["note"] = "/Users/someone/x"
        self.assert_fault(fault, "a home-style path")

    def test_an_absolute_path(self):
        def fault(document):
            document["method"]["cold"] = "read from /var/data/x"
        self.assert_fault(fault, "an absolute path")

    def test_an_address(self):
        def fault(document):
            document["method"]["warm"] = "bound to 192.0.2.1"
        self.assert_fault(fault, "an IP address")

    def test_a_hostname_field(self):
        def fault(document):
            document["header"]["hostname"] = "x"
        self.assert_fault(fault, "identifies a machine")

    # Everything else a result must state.

    def test_a_ratio_on_another_baseline(self):
        def fault(document):
            b_of(document)["stored"]["ratio_stored_over_source"] = 0.1
        self.assert_fault(fault, "not over the slice's source bytes")

    def test_a_rate_that_does_not_follow(self):
        def fault(document):
            b_of(document)["write"]["records_per_second_median"] = 1.0
        self.assert_fault(fault, "does not follow")

    def test_python_time_hidden(self):
        def fault(document):
            del b_of(document)["write"]["python_seconds"]
        self.assert_fault(fault, "no python_seconds")

    def test_a_system_missing(self):
        def fault(document):
            del document["systems"]["arcticdb"]
        self.assert_fault(fault, "arcticdb is missing")

    def test_no_label(self):
        def fault(document):
            document["label"] = "numbers"
        self.assert_fault(fault, "no claim about a full day")

    def test_no_clipping_statement(self):
        def fault(document):
            document["input"]["clipping"] = ""
        self.assert_fault(fault, "clipping")

    def test_another_input(self):
        def fault(document):
            document["input"]["sha256"] = "cd" * 32
        self.assert_fault(fault, "does not match the pin")

    def test_no_method_for_cold(self):
        def fault(document):
            document["method"]["cold"] = ""
        self.assert_fault(fault, "how warm and cold")

    def test_linux_with_no_heap_statement(self):
        def fault(document):
            document["heap"] = "-Xmx1g"
        self.assert_fault(fault, "full-day heap is unchecked", "Linux")

    def test_linux_not_pinned(self):
        def fault(document):
            document["systems"]["parquet"]["pinning"]["cpus"] = [0, 1]
        self.assert_fault(fault, "not pinned to cpu4 and cpu6", "Linux")

    def test_linux_with_no_memavailable(self):
        def fault(document):
            document["systems"]["parquet"]["mem_available_bytes"] = "not-available"
        self.assert_fault(fault, "MemAvailable", "Linux")

    def test_mixed_hosts(self):
        def fault(document):
            document["header"]["kernel"] = "Linux 6.12"
        self.assert_fault(fault, "mixed hosts")

    def test_a_debug_build(self):
        def fault(document):
            document["header"]["build_type"] = "Debug"
        self.assert_fault(fault, "is not Release")

    def test_a_header_not_first(self):
        document = valid_document("macOS")
        shuffled = {"label": document.pop("label"), **document}
        result = self.run_on(shuffled)
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not start with its header", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
