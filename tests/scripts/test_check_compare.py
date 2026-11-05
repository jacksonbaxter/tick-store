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
sys.path.insert(0, str(REPO_ROOT))

from compare import variant_rule  # noqa: E402
SCRATCH_ROOT = REPO_ROOT / "build" / "test_check_compare"

COMMIT = "0123456789abcdef0123456789abcdef01234567"
SHA = "ab" * 32
MESSAGES = 1000
BYTES = 20000
SLICE = "test-slice"
QUERY_NAMES = ("Q1", "Q2", "Q3", "Q4", "Q5")
PROVEN = "proven"
DAY = "12302019"
DAY_SHA = "cd" * 32
DAY_MESSAGES = 4000
DAY_BYTES = 80000
PINS = {"slices": {SLICE: {"sha256": SHA, "message_count": MESSAGES, "size": BYTES}},
        "days": {DAY: {"decompressed_sha256": DAY_SHA, "message_count": DAY_MESSAGES,
                       "decompressed_size": DAY_BYTES}}}
HEAP = "-Xms1g -Xmx1g"
CAPACITY_FILE = "results/fullday-capacity-12302019.json"
CAPACITY = {"questdb_heap_verdict": {"verdict": "adequate", "chosen_heap": HEAP,
                                     "tested_at_that_value": True}}
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


TICKSTORE_COLUMNS = 39


def tickstore_settings(durability):
    flushes = [] if durability == "A" else ["F_FULLFSYNC on the file, returned 0",
                                            "F_FULLFSYNC on the directory, returned 0"]
    return [setting("store version", 3), setting("segment records", 1048576),
            setting("records", MESSAGES),
            setting("durability mode", {"A": "page_cache", "B": "device_flush"}[durability]),
            setting("flushes made", flushes), setting("query path", "indexed"),
            setting("kernel level", "neon"), setting("kernel sets that ran", ["neon"])] + [
        setting(f"codec ids, column{index}", True) for index in range(TICKSTORE_COLUMNS)]


def settings_of(system, durability):
    if system == "tickstore":
        return tickstore_settings(durability)
    if system != "questdb":
        return [setting("threads", 2)]
    mode = {"A": "nosync", "B": "sync"}[durability]
    return [setting(f"cairo.commit.mode ({source})", mode)
            for source in ("conf file", "echo", "parsed")] + [
        setting("java initial heap", "-Xms1g"), setting("java heap", "-Xmx1g")]


def flush(dirty, parquet_calls=None, engine_flushes=None):
    block = {"rows": 10, "seconds": 0.1, "data_dirty_pages": dirty, "data_dirty_files": {},
             "residual_dirty_files": {}}
    if parquet_calls is not None:
        block["harness_flushes"] = parquet_calls
    if engine_flushes is not None:
        block["engine_flushes"] = copy.deepcopy(engine_flushes)
    return block


def engine_flushes_of(system, name):
    if system != "tickstore":
        return None
    if name == "A":
        return []
    return [{"call": "F_FULLFSYNC", "of": "file", "result": 0},
            {"call": "F_FULLFSYNC", "of": "directory", "result": 0}]


def durability_block(system, name, host):
    dirty = 5 if name == "A" else 0
    calls = None if system != "parquet" else (0 if name == "A" else 2)
    engine = engine_flushes_of(system, name)
    return {"class": name, "sync_primitive": "fsync", "flush_granularity": "per file",
            "like_for_like": not (host == "macOS" and system == "questdb"), "note": "n",
            "status": PROVEN, "reason": "shown",
            "flush": {"probe": flush(dirty, calls, engine), "full_ingest": flush(dirty, calls, engine)}}


def class_entry(system, name, host):
    entry = {
        "durability": durability_block(system, name, host),
        "settings": settings_of(system, name),
        "write": {"seconds_median": 2.0, "records_per_second_median": MESSAGES / 2.0,
                  "python_seconds": 0.1, "engine_seconds": 1.9, "python_note": "n",
                  "peak_rss_bytes_client": 1000},
        "stored": {"allocated_bytes": 10000, "ratio_stored_over_source": 10000 / BYTES,
                   "method": "st_blocks", "denominator": "the slice's source bytes",
                   "measured": "at rest"},
        "read_back": {"oracle": {"verdict": "pass", "exit_code": 0, "records": MESSAGES,
                                 "mismatched_records": 0, "hashes_match": True,
                                 "source_sha256": SHA, "stored_sha256": SHA},
                      "stored_settings": {"checks_passed": 3}},
    }
    if system == "tickstore":
        entry["write"]["detail"] = {"input": "its write time holds ITCH parsing"}
        entry["read_back"]["stored_settings"]["g1"] = {
            "verdict": "pass", "records": MESSAGES, "mismatched_records": 0,
            "hashes_match": True, "exit_code": 0}
    return entry


def query_entry(name, system=None):
    cold_proof = {"resident_after_eviction": 0, "evicted_pages": 10, "method": "m",
                  "resident_bytes_after_run": 4096, "device_bytes_read": 8192, "device_slack": 0}
    entry = {"digest": digest(name), "matches_ground_truth": True,
             "scan": {"rows_decoded": 5},
             "warm": {"n": 3, "median_us": 10.0, "p99_us": 12.0,
                      "proof": {"pages": 8, "resident_before": 8, "resident_after": 8}},
             "cold": {"n": 2, "median_us": 100.0, "p99_us": 110.0,
                      "proofs": [cold_proof, dict(cold_proof)]}}
    if system == "tickstore":
        entry["warm"]["engine"] = {"n": 3, "median_us": 9.0}
        entry["cold"]["engine"] = {"n": 2, "median_us": 90.0}
    return entry


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
            "queries": {name: query_entry(name, system) for name in QUERY_NAMES},
        }},
    }


def heap_block(host):
    if host == "Linux":
        return {"setting": HEAP, "full_day_check": CAPACITY_FILE,
                "statement": f"Ran with {HEAP}, as {CAPACITY_FILE} checked."}
    return {"setting": HEAP, "full_day_check": "Linux only",
            "statement": f"Ran with {HEAP}. The full-day capacity check is Linux only."}


def runtime_block(host):
    linux = host == "Linux"
    return {"jvm_vendor": "GraalVM Community" if linux else "Eclipse Adoptium",
            "jvm_vendor_version": "GraalVM CE 25.0.2" if linux else "Temurin-25.0.5+8",
            "jvm_version": "25.0.2+10" if linux else "25.0.5+8-LTS",
            "jvm_read_from": "the running JVM's own properties",
            "jemalloc_preloaded": linux,
            "jemalloc_read_from": "the libraries mapped into the running server"}


def sibling_block():
    return {"core": 4, "sibling": 5, "topology": "thread_siblings_list", "method": "jiffies",
            "cases": {"parquet": 0.5, "arcticdb": 0.1, "questdb": 1.0, "tickstore": 0.2}}


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
            "sibling_busy_percent": sibling_block() if linux else "not-available",
            "governor": "performance" if linux else "not-available",
            "turbo": "enabled" if linux else "not-available",
            "pinning": "pinned" if linux else "not-supported",
            "questdb_runtime": runtime_block(host),
            "counters": "unavailable",
            "load_lines": (LOAD_LINUX if linux else LOAD_MAC) * 4,
        },
        "label": "Slice numbers. They are not claims about a full day, and no target changes.",
        "heap": heap_block(host),
        "input": {"slice": SLICE, "sha256": SHA, "messages": MESSAGES, "bytes": BYTES,
                  "ratio_baseline": "source bytes", "clipping": "Q2 is clipped to the slice",
                  "ingest_input": "tick-store's ingest holds ITCH parsing, the others' not"},
        "method": {"warm": "how", "cold": "how"},
        "queries": {"parameters": [], "truth": {name: digest(name) for name in QUERY_NAMES}},
        "systems": {name: system_block(name, host)
                    for name in ("parquet", "arcticdb", "questdb", "tickstore")},
    }


class Harness(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.pins = self.directory / "pins.json"
        self.pins.write_text(json.dumps(PINS), encoding="utf-8")
        self.capacity = self.directory / "capacity.json"
        self.capacity.write_text(json.dumps(CAPACITY), encoding="utf-8")

    def run_on(self, document, host="macOS", capacity=None):
        path = self.directory / f"{host}-{SLICE}-{COMMIT[:12]}.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins),
                               "--capacity", str(capacity or self.capacity), str(path)],
                              check=False, capture_output=True, text=True)

    def assert_fault(self, mutate, expected, host="macOS", capacity=None):
        document = copy.deepcopy(valid_document(host))
        mutate(document)
        result = self.run_on(document, host, capacity)
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

    def test_the_repos_own_capacity_file_records_the_heap_a_linux_file_states(self):
        result = self.run_on(valid_document("Linux"), "Linux", REPO_ROOT / CAPACITY_FILE)
        self.assertEqual(result.returncode, 0, result.stderr)


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

    # tick-store's settings, each read back from the store or its tools.

    def test_a_tickstore_setting_that_differs(self):
        def fault(document):
            for item in b_of(document, "tickstore")["settings"]:
                if item["name"] == "segment records":
                    item["in_force"] = 65536
        self.assert_fault(fault, "segment records requested 1048576, in force 65536")

    def test_a_tickstore_setting_not_read_back(self):
        def fault(document):
            entry = b_of(document, "tickstore")
            entry["settings"] = [item for item in entry["settings"]
                                 if item["name"] != "kernel level"]
        self.assert_fault(fault, "tick-store's 'kernel level' was not read back")

    def test_a_tickstore_class_b_store_in_page_cache_mode(self):
        def fault(document):
            for item in b_of(document, "tickstore")["settings"]:
                if item["name"] == "durability mode":
                    item["requested"] = item["in_force"] = "page_cache"
        self.assert_fault(fault, "durability mode 'page_cache' in class B")

    def test_tickstore_class_b_with_one_flush_in_the_settings(self):
        def fault(document):
            for item in b_of(document, "tickstore")["settings"]:
                if item["name"] == "flushes made":
                    item["requested"] = item["in_force"] = item["in_force"][:1]
        self.assert_fault(fault, "flushes made")

    def test_tickstore_class_a_with_a_flush_in_the_settings(self):
        def fault(document):
            entry = document["systems"]["tickstore"]["variants"]["v"]["classes"]["A"]
            for item in entry["settings"]:
                if item["name"] == "flushes made":
                    item["requested"] = item["in_force"] = ["fsync on the file, returned 0"]
        self.assert_fault(fault, "flushes made ['fsync on the file, returned 0'] in class A")

    def test_tickstore_another_kernel_set_ran(self):
        def fault(document):
            for item in b_of(document, "tickstore")["settings"]:
                if item["name"] == "kernel sets that ran":
                    item["requested"] = item["in_force"] = ["neon", "scalar"]
        self.assert_fault(fault, "kernel sets that ran ['neon', 'scalar'], not 'neon'")

    def test_tickstore_a_column_on_another_codec(self):
        def fault(document):
            item = b_of(document, "tickstore")["settings"][-1]
            item["requested"] = item["in_force"] = False
        self.assert_fault(fault, "is not the codec docs/store.md lists")

    def test_tickstore_codecs_missing_for_a_column(self):
        def fault(document):
            b_of(document, "tickstore")["settings"].pop()
        self.assert_fault(fault, "codec ids read back for 38 columns")

    def test_tickstore_store_version_2(self):
        def fault(document):
            for item in b_of(document, "tickstore")["settings"]:
                if item["name"] == "store version":
                    item["requested"] = item["in_force"] = 2
        self.assert_fault(fault, "store version 2, not 3")

    def test_tickstore_class_b_claimed_without_the_writers_flush(self):
        def fault(document):
            block = b_of(document, "tickstore")["durability"]["flush"]["full_ingest"]
            block["engine_flushes"] = block["engine_flushes"][:1]
        self.assert_fault(fault, "without the writer's flush of the file and then its directory")

    def test_tickstore_class_b_claimed_with_a_failed_flush(self):
        def fault(document):
            block = b_of(document, "tickstore")["durability"]["flush"]["probe"]
            block["engine_flushes"][1]["result"] = -1
        self.assert_fault(fault, "without the writer's flush")

    def test_tickstore_class_a_claimed_with_a_flush(self):
        def fault(document):
            entry = document["systems"]["tickstore"]["variants"]["v"]["classes"]["A"]
            entry["durability"]["flush"]["probe"]["engine_flushes"] = [
                {"call": "fsync", "of": "file", "result": 0}]
        self.assert_fault(fault, "class A claimed with the writer's flushes in probe")

    def test_tickstore_needs_no_dirty_page_control(self):
        document = valid_document("macOS")
        entry = document["systems"]["tickstore"]["variants"]["v"]["classes"]["A"]
        entry["durability"]["flush"]["probe"]["data_dirty_pages"] = 0
        self.assertEqual(self.run_on(document).returncode, 0)

    def test_tickstore_g1_failing(self):
        def fault(document):
            b_of(document, "tickstore")["read_back"]["stored_settings"]["g1"]["verdict"] = "mismatch"
        self.assert_fault(fault, "G-1 on the store did not pass")

    def test_tickstore_write_not_saying_it_parses_itch(self):
        def fault(document):
            b_of(document, "tickstore")["write"]["detail"]["input"] = "the slice"
        self.assert_fault(fault, "does not say that tick-store's time holds ITCH parsing")

    def test_the_input_not_saying_tickstore_parses_itch(self):
        def fault(document):
            del document["input"]["ingest_input"]
        self.assert_fault(fault, "input: does not say that tick-store's ingest holds ITCH parsing")

    def test_tickstore_with_no_engine_time(self):
        def fault(document):
            del document["systems"]["tickstore"]["variants"]["v"]["queries"]["Q2"]["cold"]["engine"]
        self.assert_fault(fault, "Q2.cold: no engine time beside the round trip")

    def test_tickstore_engine_longer_than_its_round_trip(self):
        def fault(document):
            query = document["systems"]["tickstore"]["variants"]["v"]["queries"]["Q3"]
            query["warm"]["engine"]["median_us"] = 11.0
        self.assert_fault(fault, "the engine took longer than its round trip")

    def test_a_class_labelled_as_the_other(self):
        def fault(document):
            b_of(document, "arcticdb")["durability"]["class"] = "A"
        self.assert_fault(fault, "durability: labelled class 'A'")

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

    def test_a_query_with_no_p99(self):
        def fault(document):
            del q1_of(document, "tickstore")["cold"]["p99_us"]
        self.assert_fault(fault, "tickstore.v.queries.Q1.cold: no median and p99")

    def test_a_p99_below_the_median(self):
        def fault(document):
            q1_of(document)["warm"]["p99_us"] = 9.0
        self.assert_fault(fault, "parquet.v.queries.Q1.warm: the p99 is below the median")

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

    # The core's SMT sibling.

    def test_no_sibling_reading_on_linux(self):
        def fault(document):
            del document["header"]["sibling_busy_percent"]
        self.assert_fault(fault, "missing field 'sibling_busy_percent'", "Linux")

    def test_a_system_with_no_sibling_reading(self):
        def fault(document):
            del document["header"]["sibling_busy_percent"]["cases"]["questdb"]
        self.assert_fault(fault, "no reading for questdb", "Linux")

    def test_a_sibling_busy_for_6_percent_is_refused(self):
        def fault(document):
            document["header"]["sibling_busy_percent"]["cases"]["parquet"] = 6.0
        self.assert_fault(fault, "parquet reads 6.0%, over the 5.0% limit", "Linux")

    def test_a_sibling_busy_for_half_a_percent_is_accepted(self):
        result = self.run_on(valid_document("Linux"), "Linux")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_macos_claiming_a_sibling_reading(self):
        def fault(document):
            document["header"]["sibling_busy_percent"] = sibling_block()
        self.assert_fault(fault, "macOS has no pinned core")

    def test_macos_with_no_sibling_field(self):
        def fault(document):
            del document["header"]["sibling_busy_percent"]
        self.assert_fault(fault, "missing field 'sibling_busy_percent'")

    # QuestDB's runtime.

    def test_no_runtime_in_the_header(self):
        def fault(document):
            del document["header"]["questdb_runtime"]
        for host in ("macOS", "Linux"):
            with self.subTest(host=host):
                self.assert_fault(fault, "missing field 'questdb_runtime'", host)

    def test_each_runtime_field_is_required(self):
        for field in ("jvm_vendor", "jvm_version", "jvm_read_from", "jemalloc_read_from"):
            with self.subTest(field=field):
                self.assert_fault(lambda d, f=field: d["header"]["questdb_runtime"].pop(f),
                                  f"header.questdb_runtime: missing field '{field}'")

    def test_jemalloc_not_read(self):
        def fault(document):
            del document["header"]["questdb_runtime"]["jemalloc_preloaded"]
        self.assert_fault(fault, "jemalloc_preloaded must be true or false", "Linux")

    def test_a_runtime_that_was_not_read(self):
        def fault(document):
            document["header"]["questdb_runtime"] = "not-run"
        self.assert_fault(fault, "not a reading from the running server")

    # The heap.

    def test_linux_with_no_heap(self):
        def fault(document):
            del document["heap"]
        self.assert_fault(fault, "heap: Linux must state QuestDB's heap", "Linux")

    def test_linux_with_the_old_sentence_and_no_setting(self):
        def fault(document):
            document["heap"] = "The heap needed for a full day is unchecked."
        self.assert_fault(fault, "heap: Linux must state QuestDB's heap", "Linux")

    def test_linux_stating_a_heap_that_was_not_checked(self):
        def fault(document):
            for field in ("setting", "statement"):
                document["heap"][field] = document["heap"][field].replace(HEAP,
                                                                          "-Xms2g -Xmx2g")
        self.assert_fault(fault, "is not '-Xms1g -Xmx1g', the heap", "Linux")

    def test_linux_running_a_heap_other_than_it_states(self):
        def fault(document):
            for item in b_of(document)["settings"]:
                if item["name"] == "java heap":
                    item["requested"] = item["in_force"] = "-Xmx2g"
        self.assert_fault(fault, "questdb.v.B ran with '-Xms1g -Xmx2g'", "Linux")

    def test_linux_with_no_heap_read_from_the_running_command(self):
        def fault(document):
            settings = b_of(document)["settings"]
            b_of(document)["settings"] = [item for item in settings
                                          if item["name"] != "java initial heap"]
        self.assert_fault(fault, "questdb.v.B ran with 'None -Xmx1g'", "Linux")

    def test_linux_not_citing_the_capacity_check(self):
        def fault(document):
            del document["heap"]["full_day_check"]
        self.assert_fault(fault, f"Linux must cite {CAPACITY_FILE}", "Linux")

    def test_a_statement_that_does_not_say_the_setting(self):
        def fault(document):
            document["heap"]["statement"] = "It ran."
        self.assert_fault(fault, "the statement does not say", "Linux")

    def test_a_capacity_file_with_no_adequate_heap(self):
        for change in ({"verdict": "inadequate"}, {"tested_at_that_value": False}):
            with self.subTest(change=change):
                verdict = {**CAPACITY["questdb_heap_verdict"], **change}
                path = self.directory / "capacity-refused.json"
                path.write_text(json.dumps({"questdb_heap_verdict": verdict}), encoding="utf-8")
                self.assert_fault(lambda document: None, "records no heap as checked", "Linux",
                                  path)

    def test_a_capacity_file_that_cannot_be_read(self):
        result = self.run_on(valid_document("Linux"), "Linux", self.directory / "none.json")
        self.assertEqual(result.returncode, 2)
        self.assertIn("capacity check", result.stderr)

    def test_macos_with_no_heap(self):
        def fault(document):
            del document["heap"]
        self.assert_fault(fault, "heap: macOS must state QuestDB's heap")

    def test_macos_not_saying_the_capacity_check_is_linux_only(self):
        def fault(document):
            document["heap"]["statement"] = f"Ran with {HEAP}."
        self.assert_fault(fault, "capacity check is Linux only")

    def test_macos_keeps_its_own_heap(self):
        document = valid_document("macOS")
        document["heap"]["setting"] = "-Xms2g -Xmx2g"
        document["heap"]["statement"] = ("Ran with -Xms2g -Xmx2g. The full-day capacity check is "
                                         "Linux only.")
        for system in ("A", "B"):
            for item in document["systems"]["questdb"]["variants"]["v"]["classes"][system][
                    "settings"]:
                if "heap" in item["name"]:
                    item["requested"] = item["in_force"] = item["in_force"].replace("1g", "2g")
        result = self.run_on(document)
        self.assertEqual(result.returncode, 0, result.stderr)

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


OPEN_NS = 34_200 * 1_000_000_000
HOUR_NS = 3_600 * 1_000_000_000
BUSIEST = 13


def full_day_parameters():
    return [{"name": "Q1", "kind": "rows", "start_ns": OPEN_NS, "end_ns": OPEN_NS + 10**9,
             "locate": -1, "text": "One second."},
            {"name": "Q2", "kind": "rows", "start_ns": OPEN_NS, "end_ns": OPEN_NS + HOUR_NS,
             "locate": BUSIEST, "text": "One hour of one symbol."},
            {"name": "Q3", "kind": "minute_aggregate", "start_ns": OPEN_NS,
             "end_ns": OPEN_NS + HOUR_NS, "locate": BUSIEST, "text": "Per minute, one hour."},
            {"name": "Q4", "kind": "rows", "start_ns": OPEN_NS, "end_ns": OPEN_NS + 10**6,
             "locate": -1, "text": "One millisecond."},
            {"name": "Q5", "kind": "column_sum", "start_ns": 0, "end_ns": 0, "locate": -1,
             "text": "The whole day."}]


def full_day_warm(entry):
    accepted = {"device_bytes_read": 0, "device_counter": "block device stat",
                "warm_up_runs": 1, "pages": 8, "resident_before": 8, "resident_after": 8}
    entry["warm"]["proof"] = {"method": "per query", "device_slack_bytes": 0, "attempts": 1,
                              "accepted": accepted, "discarded": []}


def full_day_document():
    """The Linux file as a full day, and the slice file its variants come from."""
    document = valid_document("Linux")
    document["label"] = "A full day, every query with nothing clipped."
    document["input"] = {"day": DAY, "full_day": True, "sha256": DAY_SHA,
                         "messages": DAY_MESSAGES, "bytes": DAY_BYTES,
                         "ratio_baseline": "the day's source bytes",
                         "clipping": "A whole day. Nothing is clipped.",
                         "ingest_input": "tick-store's ingest holds ITCH parsing"}
    document["queries"]["parameters"] = full_day_parameters()
    document["queries"]["full_day"] = {"hour_window_ns": [OPEN_NS, OPEN_NS + HOUR_NS],
                                       "busiest_locate_window_ns": [OPEN_NS, OPEN_NS + HOUR_NS],
                                       "busiest_locate": BUSIEST,
                                       "q5_rows_scanned": DAY_MESSAGES, "clipped": False}
    for system in document["systems"].values():
        system["peak_memory"] = {"vmhwm_bytes_largest": 10, "mem_available_lowest_bytes": 5,
                                 "session_rss_bytes_peak": 12, "guard_fired": False,
                                 "exit": {"code": 0, "signal": None}}
        for variant in system["variants"].values():
            for entry in variant["classes"].values():
                entry["write"]["records_per_second_median"] = DAY_MESSAGES / 2.0
                entry["stored"]["ratio_stored_over_source"] = 10000 / DAY_BYTES
                entry["stored"]["denominator"] = "the day's source bytes"
                for field in ("records",):
                    entry["read_back"]["oracle"][field] = DAY_MESSAGES
                for field in ("source_sha256", "stored_sha256"):
                    entry["read_back"]["oracle"][field] = DAY_SHA
                entry["read_back"]["streamed"] = {"rows": DAY_MESSAGES, "windows": 3}
            for entry in variant["queries"].values():
                full_day_warm(entry)
    source = valid_document("Linux")
    document["variant_selection"] = {
        "rule": variant_rule.RULE, "source": f"Linux-{SLICE}-{COMMIT[:12]}.json",
        "source_commit": COMMIT, "systems": variant_rule.select(source)}
    return document, source


class FullDay(Harness):
    def run_full(self, document, source, name=None):
        (self.directory / f"Linux-{SLICE}-{COMMIT[:12]}.json").write_text(
            json.dumps(source), encoding="utf-8")
        path = self.directory / (name or f"Linux-{DAY}-full-{COMMIT[:12]}.json")
        path.write_text(json.dumps(document), encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins),
                               "--capacity", str(self.capacity), str(path)],
                              check=False, capture_output=True, text=True)

    def assert_full_fault(self, mutate, expected, name=None):
        document, source = full_day_document()
        mutate(document, source)
        result = self.run_full(document, source, name)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(expected, result.stderr)

    def test_the_valid_full_day_file_passes(self):
        result = self.run_full(*full_day_document())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_clipped_hour_passing_as_a_full_day(self):
        def fault(document, source):
            for item in document["queries"]["parameters"]:
                if item["name"] in ("Q2", "Q3"):
                    item["end_ns"] = OPEN_NS + 270 * 10**9
        self.assert_full_fault(fault, "Q2 is not the full-day query")

    def test_a_busiest_locate_chosen_over_a_clipped_window(self):
        def fault(document, source):
            document["queries"]["full_day"]["busiest_locate_window_ns"][1] = OPEN_NS + 10**9
        self.assert_full_fault(fault, "the busiest locate's window is clipped")

    def test_q5_over_less_than_the_day(self):
        def fault(document, source):
            document["queries"]["full_day"]["q5_rows_scanned"] = MESSAGES
        self.assert_full_fault(fault, "Q5 scans 1000 rows")

    def test_a_query_that_says_it_is_clipped(self):
        def fault(document, source):
            document["queries"]["parameters"][1]["text"] = "One hour, clipped to the slice."
        self.assert_full_fault(fault, "Q2 says it is clipped")

    def test_a_slice_file_named_as_a_full_day(self):
        document = valid_document("Linux")
        result = self.run_full(document, valid_document("Linux"),
                               f"Linux-{SLICE}-full-{COMMIT[:12]}.json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("a full-day file must name its day", result.stderr)

    def test_a_full_day_file_with_a_slice_name(self):
        document, source = full_day_document()
        result = self.run_full(document, source, f"Linux-{SLICE}-{COMMIT[:12]}-day.json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("a full-day file must name its day", result.stderr)

    def test_a_full_day_against_the_slice_pin(self):
        def fault(document, source):
            document["input"]["messages"] = MESSAGES
        self.assert_full_fault(fault, "input: does not match the pin")

    def test_a_ratio_over_the_slice(self):
        def fault(document, source):
            b_of(document, "parquet")["stored"]["ratio_stored_over_source"] = 10000 / BYTES
        self.assert_full_fault(fault, "the ratio is not over")

    def test_a_store_not_read_back_whole(self):
        def fault(document, source):
            b_of(document, "arcticdb")["read_back"]["streamed"]["rows"] = DAY_MESSAGES - 1
        self.assert_full_fault(fault, "the whole day was not read back")

    def test_a_warm_number_that_read_from_the_device(self):
        def fault(document, source):
            q1_of(document)["warm"]["proof"]["accepted"]["device_bytes_read"] = 4096
        self.assert_full_fault(fault, "the device read 4096 bytes")

    def test_a_discarded_warm_attempt_left_out(self):
        def fault(document, source):
            q1_of(document)["warm"]["proof"]["attempts"] = 2
        self.assert_full_fault(fault, "2 attempts")

    def test_a_slice_warm_proof_on_a_full_day(self):
        def fault(document, source):
            q1_of(document)["warm"]["proof"] = {"pages": 8, "resident_before": 8,
                                                "resident_after": 8}
        self.assert_full_fault(fault, "bytes across the timed repetitions")

    def test_no_peak_memory(self):
        def fault(document, source):
            del document["systems"]["questdb"]["peak_memory"]
        self.assert_full_fault(fault, "questdb.peak_memory: no vmhwm_bytes_largest")

    def test_the_guard_fired(self):
        def fault(document, source):
            document["systems"]["parquet"]["peak_memory"]["guard_fired"] = True
        self.assert_full_fault(fault, "the guard stopped the run")

    def test_variants_the_rule_does_not_give(self):
        def fault(document, source):
            variants = document["systems"]["parquet"]["variants"]
            variants["extra"] = copy.deepcopy(variants["v"])
        self.assert_full_fault(fault, "parquet ran ['v', 'extra']")

    def test_a_rule_applied_to_another_slice_file(self):
        def fault(document, source):
            winner = copy.deepcopy(source["systems"]["parquet"]["variants"]["v"])
            for query in winner["queries"].values():
                query["warm"]["median_us"] = query["warm"]["p99_us"] = 1.0
            source["systems"]["parquet"]["variants"]["w"] = winner
        self.assert_full_fault(fault, "variant_selection: parquet states")

    def test_no_slice_file_beside_it(self):
        def fault(document, source):
            document["variant_selection"]["source"] = "Linux-other-0123456789ab.json"
        self.assert_full_fault(fault, "is not beside this one")

    def test_a_slice_file_from_another_commit(self):
        def fault(document, source):
            source["header"]["commit"] = "f" * 40
        self.assert_full_fault(fault, "not this host's at this commit")

    def test_no_variant_selection(self):
        def fault(document, source):
            del document["variant_selection"]
        self.assert_full_fault(fault, "must state how its variants were chosen")


class VariantRule(unittest.TestCase):
    """compare/variant_rule.py: a win needs the lowest median and the lowest p99 together."""

    @staticmethod
    def system(numbers):
        """{variant: (median, p99)} on every query and kind alike, except Q1 warm."""
        variants = {}
        for name, (median, p99) in numbers.items():
            queries = {query: {kind: {"median_us": 50.0, "p99_us": 60.0}
                               for kind in ("warm", "cold")} for query in QUERY_NAMES}
            queries["Q1"]["warm"] = {"median_us": median, "p99_us": p99}
            variants[name] = {"queries": queries}
        return {"headline_variant": "h", "variants": variants}

    def test_the_lowest_median_and_p99_together_win(self):
        won = variant_rule.wins(self.system({"h": (10.0, 20.0), "a": (5.0, 15.0)}))
        self.assertIn("Q1 warm", won["a"])
        self.assertNotIn("Q1 warm", won.get("h", []))

    def test_a_lower_median_with_a_higher_p99_does_not_win(self):
        won = variant_rule.wins(self.system({"h": (10.0, 20.0), "a": (5.0, 25.0)}))
        self.assertNotIn("Q1 warm", won.get("a", []))
        self.assertNotIn("Q1 warm", won.get("h", []))

    def test_the_headline_comes_first_and_losers_stay_out(self):
        document = {"systems": {"x": self.system({"b": (1.0, 1.0), "h": (2.0, 2.0),
                                                  "c": (3.0, 3.0)})}}
        chosen = variant_rule.select(document)["x"]
        self.assertEqual(chosen["selected"][0], "h")
        self.assertIn("b", chosen["selected"])
        # Every variant ties on the other nine measures, so each of them wins those.
        self.assertEqual(sorted(chosen["selected"]), ["b", "c", "h"])
        self.assertEqual(chosen["won"]["b"][0], "Q1 warm")


if __name__ == "__main__":
    unittest.main(verbosity=1)
