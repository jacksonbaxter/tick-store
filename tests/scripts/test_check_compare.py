#!/usr/bin/env python3
"""Tests for scripts/check_compare.py. Run with:

    python3 -I tests/scripts/test_check_compare.py

The validator runs as a subprocess, as a user would run it, on results files generated under
the repo's gitignored build/ directory. A check that cannot fail proves nothing, so each rule
gets one injected fault in an otherwise valid file, and the test requires the exit code and
the message naming that rule. Both hosts' valid files are shown passing first.
"""

import copy
import hashlib
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

# The fixtures stand for the committed protocol 1 files, so they carry one of their commits:
# a file that states no protocol is valid only from those (review RF-1).
COMMIT = "67ae574f566674b883e9199b375f88e12dbeeeca"
OTHER_COMMIT = "0123456789abcdef0123456789abcdef01234567"
# What check_compare.py says of a file that states no protocol and is not one of the committed
# protocol 1 files, pinned by their SHA-256 (review RV-6).
PROTOCOL_1_UNPINNED = ("header.protocol: none stated, and only the committed files measured "
                       "before protocol 2, each pinned by its SHA-256 under "
                       "protocol_1_comparison_files in scripts/data_pins.json, may state none")
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


def swap_counts(pages_in=0, pages_out=0):
    return {"pages_in": pages_in, "pages_out": pages_out}


def class_entry(system, name, host):
    entry = {
        "swap": {"write": swap_counts(1, 100), "read_back": swap_counts(0, 20)},
        "durability": durability_block(system, name, host),
        "settings": settings_of(system, name),
        "write": {"seconds_median": 2.0, "records_per_second_median": MESSAGES / 2.0,
                  "python_seconds": 0.1, "engine_seconds": 1.9, "python_note": "n",
                  "peak_rss_bytes_client": 1000, "repetitions": WRITE_RUNS,
                  "seconds": [2.1, 2.0, 1.9]},
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


# The harness's repetitions, which every reported file holds (review F-5).
WARM_RUNS = 30
COLD_RUNS = 7
WRITE_RUNS = 3
FULL_DAY_WRITE_RUNS = 1


def query_entry(name, system=None):
    cold_proof = {"resident_after_eviction": 0, "evicted_pages": 10, "method": "m",
                  "resident_bytes_after_run": 4096, "device_bytes_read": 8192, "device_slack": 0}
    entry = {"digest": digest(name), "matches_ground_truth": True,
             "scan": {"rows_decoded": 5},
             "warm": {"n": WARM_RUNS, "median_us": 10.0, "p99_us": 12.0,
                      "proof": {"pages": 8, "resident_before": 8, "resident_after": 8},
                      "swap_timed": {**swap_counts(0, 4), "flagged": False}},
             "cold": {"n": COLD_RUNS, "median_us": 100.0, "p99_us": 110.0,
                      "proofs": [dict(cold_proof) for _ in range(COLD_RUNS)],
                      "swap_timed": {**swap_counts(2, 0), "flagged": True,
                                     "pages_in_by_repetition": [0] * (COLD_RUNS - 1) + [2]}}}
    if system == "tickstore":
        entry["warm"]["engine"] = {"n": WARM_RUNS, "median_us": 9.0}
        entry["cold"]["engine"] = {"n": COLD_RUNS, "median_us": 90.0}
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
            "swap": {"warm_queries": swap_counts(0, 7), "cold_queries": swap_counts(12, 3)},
        }},
        "swap_by_phase": {"write": swap_counts(2, 200), "read_back": swap_counts(0, 40),
                          "warm_queries": swap_counts(0, 7), "cold_queries": swap_counts(12, 3)},
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
            "schema": 2, "host": host, "measurement": "reported",
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
        "method": {"warm": "how", "cold": "how",
                   "swap": {"source": "pswpin and pswpout" if linux else "vm_stat",
                            "page_bytes": 4096 if linux else 16384, "scope": "host-wide",
                            "timed_policy": "a timed query with swap-in is flagged"}},
        "queries": {"parameters": [], "truth": {name: digest(name) for name in QUERY_NAMES}},
        "systems": {name: system_block(name, host)
                    for name in ("parquet", "arcticdb", "questdb", "tickstore")},
    }


# The commits of the committed protocol 1 files, which the fixtures that state no protocol stand
# for.
PROTOCOL_1_FIXTURE_COMMITS = (COMMIT, "2503d1e7c5f68c657fab7c02ddb59c489b005c73")


def pin_protocol_1(pins_path, folder):
    """check_compare.py knows the committed protocol 1 files by their bytes, pinned in the pins
    file (review RV-6). A fixture that states no protocol, with one of their commits, stands for
    one of them, so each such file in `folder` is pinned as written."""
    pins = json.loads(pins_path.read_text(encoding="utf-8"))
    pinned = {}
    for path in sorted(folder.glob("*.json")):
        try:
            header = json.loads(path.read_text(encoding="utf-8")).get("header")
        except (AttributeError, json.JSONDecodeError):
            continue
        if isinstance(header, dict) and "protocol" not in header and \
                header.get("commit") in PROTOCOL_1_FIXTURE_COMMITS:
            pinned[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    pins["protocol_1_comparison_files"] = pinned
    pins_path.write_text(json.dumps(pins), encoding="utf-8")


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
        pin_protocol_1(self.pins, self.directory)
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


def a_of(document, system="questdb"):
    return document["systems"][system]["variants"]["v"]["classes"]["A"]


class Faults(Harness):
    # Class A's proof (review F-6).

    def test_a_parquet_class_a_proven_with_the_harness_flush_made(self):
        for scope in ("probe", "full_ingest"):
            with self.subTest(scope=scope):
                def fault(document, scope=scope):
                    a_of(document, "parquet")["durability"]["flush"][scope]["harness_flushes"] = 2
                self.assert_fault(fault, "parquet.v.A.durability: class A claimed with 2 harness "
                                         f"flushes in {scope}")

    def test_a_class_a_proven_with_no_dirty_page_in_its_probe(self):
        # Class B NOT PROVEN, so the control rule, which reads the same probe, does not fire.
        for system in ("arcticdb", "questdb"):
            for dirty in (0, None):
                with self.subTest(system=system, dirty=dirty):
                    def fault(document, system=system, dirty=dirty):
                        a_of(document, system)["durability"]["flush"]["probe"][
                            "data_dirty_pages"] = dirty
                        b_of(document, system)["durability"]["status"] = "NOT PROVEN"
                    self.assert_fault(fault, f"{system}.v.A.durability: class A claimed with "
                                             f"{dirty} dirty pages in its probe")

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

    # Review F-5: a reported file holds the harness's own repetitions, never fewer.

    def test_a_reported_file_with_one_warm_and_one_cold_repetition(self):
        def fault(document):
            for system in document["systems"].values():
                for query in system["variants"]["v"]["queries"].values():
                    query["warm"]["n"] = query["cold"]["n"] = 1
                    query["cold"]["proofs"] = query["cold"]["proofs"][:1]
                    query["cold"]["swap_timed"]["pages_in_by_repetition"] = [2]
                    for kind in ("warm", "cold"):
                        if "engine" in query[kind]:
                            query[kind]["engine"]["n"] = 1
        self.assert_fault(fault, "parquet.v.queries.Q1.warm: 1 repetitions, and a reported file "
                                 "runs 30")
        self.assert_fault(fault, "tickstore.v.queries.Q5.cold: 1 repetitions, and a reported "
                                 "file runs 7")

    def test_a_slice_written_once(self):
        def fault(document):
            write = b_of(document, "arcticdb")["write"]
            write["repetitions"], write["seconds"] = 1, [2.0]
        self.assert_fault(fault, "arcticdb.v.B.write: 1 repetitions, and a reported slice file "
                                 "writes 3")

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

    def test_load_lines_that_are_not_a_list(self):
        # Review E-2: nothing else in a comparison file reads the load lines, so a string held
        # no proof that the run held the lock, and passed check-compare and render-check.
        for value in ("measure.sh: start load1=0.10", {"start": "load1=0.10"}):
            with self.subTest(value=value):
                def fault(document, value=value):
                    document["header"]["load_lines"] = value
                self.assert_fault(fault, "header.load_lines: not a list")

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
    accepted = {"device_bytes_read": 0, "device_counter": "read_bytes per process",
                "by_process": {"worker": 0, "waited children": 0},
                "machine_wide_device_bytes_read": 4096,
                "machine_wide_counter": "block device stat",
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
                                 "swap_in_pages_during_run": 3,
                                 "swap_out_pages_during_run": 250000,
                                 "exit": {"code": 0, "signal": None}}
        for variant in system["variants"].values():
            for entry in variant["classes"].values():
                entry["write"]["records_per_second_median"] = DAY_MESSAGES / 2.0
                entry["write"]["repetitions"] = FULL_DAY_WRITE_RUNS
                entry["write"]["seconds"] = [2.0]
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
        "rule": variant_rule.RULE_PROTOCOL_1, "source": f"Linux-{SLICE}-{COMMIT[:12]}.json",
        "source_commit": COMMIT, "systems": variant_rule.select(source)}
    return document, source


class FullDay(Harness):
    def run_full(self, document, source, name=None):
        (self.directory / f"Linux-{SLICE}-{COMMIT[:12]}.json").write_text(
            json.dumps(source), encoding="utf-8")
        path = self.directory / (name or f"Linux-{DAY}-full-{COMMIT[:12]}.json")
        path.write_text(json.dumps(document), encoding="utf-8")
        pin_protocol_1(self.pins, self.directory)
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

    def test_a_full_day_written_three_times(self):
        def fault(document, source):
            write = b_of(document, "parquet")["write"]
            write["repetitions"], write["seconds"] = 3, [2.1, 2.0, 1.9]
        self.assert_full_fault(fault, "parquet.v.B.write: 3 repetitions, and a reported full-day "
                                      "file writes 1")

    def test_a_store_not_read_back_whole(self):
        def fault(document, source):
            b_of(document, "arcticdb")["read_back"]["streamed"]["rows"] = DAY_MESSAGES - 1
        self.assert_full_fault(fault, "the whole day was not read back")

    def test_a_warm_number_that_read_from_the_device(self):
        def fault(document, source):
            q1_of(document)["warm"]["proof"]["accepted"]["device_bytes_read"] = 4096
        self.assert_full_fault(fault, "the device read 4096 bytes")

    def test_a_warm_number_whose_server_read_from_storage(self):
        def fault(document, source):
            q1_of(document)["warm"]["proof"]["accepted"]["by_process"]["questdb"] = 8192
        self.assert_full_fault(fault, "no storage reads per process")

    def test_a_warm_number_counted_without_the_worker(self):
        def fault(document, source):
            del q1_of(document)["warm"]["proof"]["accepted"]["by_process"]["worker"]
        self.assert_full_fault(fault, "no storage reads per process")

    def test_a_warm_number_with_no_machine_wide_count(self):
        def fault(document, source):
            del q1_of(document)["warm"]["proof"]["accepted"]["machine_wide_device_bytes_read"]
        self.assert_full_fault(fault, "machine-wide count is not kept")

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

    def test_no_swap_counts_from_the_guard(self):
        def fault(document, source):
            del document["systems"]["questdb"]["peak_memory"]["swap_in_pages_during_run"]
        self.assert_full_fault(fault, "the guard's swap_in_pages_during_run is not recorded")

    def test_a_full_day_file_from_before_swap_was_counted(self):
        def fault(document, source):
            document["header"]["schema"] = 1
        self.assert_full_fault(fault, "a file must count swap per phase")

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

    def test_a_slice_file_measured_iteration_only(self):
        # Review F-15: the slice file a day's variants come from is itself a reported result.
        def fault(document, source):
            source["header"]["measurement"] = "iteration-only"
        self.assert_full_fault(fault, "variant_selection: the slice file "
                                      f"Linux-{SLICE}-{COMMIT[:12]}.json: header.measurement: "
                                      "iteration-only, so this is not a reported result")

    def test_a_slice_file_its_own_validation_refuses(self):
        def fault(document, source):
            source["systems"]["parquet"]["variants"]["v"]["classes"]["B"]["settings"][0][
                "in_force"] = 3
        self.assert_full_fault(fault, "variant_selection: the slice file "
                                      f"Linux-{SLICE}-{COMMIT[:12]}.json: parquet.v.B")

    def test_a_variant_source_that_is_a_full_day_is_refused_without_a_traceback(self):
        # Review RV-9: a day naming itself, or another day, as its slice file is refused once,
        # not validated again and again until the stack runs out.
        name = f"Linux-{DAY}-full-{COMMIT[:12]}.json"
        document, source = full_day_document()
        document["variant_selection"]["source"] = name
        result = self.run_full(document, source, name)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("variant_selection: the slice file is not this host's at this commit",
                      result.stderr)


def tickstore_only_document():
    """tick-store alone on the day, on macOS: the full day's rules, every variant it has, warm
    by residency, and peak memory from /usr/bin/time -l."""
    document, _ = full_day_document()
    linux_only = valid_document("macOS")
    document["header"] = linux_only["header"]
    document["header"]["questdb_runtime"] = "not-run"
    document["header"]["load_lines"] = LOAD_MAC
    document["method"]["swap"] = linux_only["method"]["swap"]
    document["label"] = ("A full day of tick-store alone, every query with nothing clipped, and "
                         "no other system.")
    document["scope"] = "tickstore-only"
    document["heap"] = {"setting": "not-run", "full_day_check": "Linux only",
                        "statement": "QuestDB did not run: tick-store ran alone."}
    document["input"]["systems_run"] = ["tickstore"]
    system = document["systems"]["tickstore"]
    document["systems"] = {"tickstore": system}
    del system["peak_memory"]
    system["pinning"] = {"cpus": []}
    system["mem_available_bytes"] = "not-available"
    only = system["variants"].pop("v")
    for entry in only["queries"].values():
        entry["warm"]["proof"] = {"pages": 8, "resident_before": 8, "resident_after": 8}
    for name in variant_rule.TICKSTORE_VARIANTS:
        system["variants"][name] = copy.deepcopy(only)
    system["headline_variant"] = variant_rule.TICKSTORE_VARIANTS[0]
    count = len(variant_rule.TICKSTORE_VARIANTS)
    system["swap_by_phase"] = {phase: {field: value * count for field, value in counts.items()}
                               for phase, counts in system["swap_by_phase"].items()}
    system["peak_memory"] = {"maximum_resident_set_size_bytes": 12 << 30,
                             "peak_memory_footprint_bytes": 2 << 30, "real_seconds": 1800.0,
                             "source": "BSD time -l around the system's whole run"}
    document["variant_selection"] = variant_rule.tickstore_only()
    return document


ALONE_NAME = f"macOS-{DAY}-full-tickstore-{COMMIT[:12]}.json"
LINUX_ALONE_NAME = f"Linux-{DAY}-full-tickstore-{COMMIT[:12]}.json"


def linux_tickstore_only_document():
    """tick-store alone on the day, on Linux (review F-5): tick-store alone as on macOS, with
    Linux's own proofs, warm by each process's storage reads and peak memory from the guard."""
    document = tickstore_only_document()
    linux, _ = full_day_document()
    document["header"] = linux["header"]
    document["header"]["questdb_runtime"] = "not-run"
    document["header"]["load_lines"] = LOAD_LINUX
    document["header"]["sibling_busy_percent"] = {**sibling_block(), "cases": {"tickstore": 0.2}}
    document["method"]["swap"] = linux["method"]["swap"]
    document["heap"] = {"setting": "not-run", "full_day_check": "not-run",
                        "statement": "QuestDB did not run: tick-store ran alone."}
    system = document["systems"]["tickstore"]
    ran = linux["systems"]["tickstore"]
    for field in ("pinning", "mem_available_bytes", "peak_memory"):
        system[field] = ran[field]
    for name in variant_rule.TICKSTORE_VARIANTS:
        system["variants"][name]["queries"] = copy.deepcopy(ran["variants"]["v"]["queries"])
    return document


class TickstoreOnly(Harness):
    """A full day of tick-store alone, on either host: never a subset passing for the
    four-system day."""

    def run_alone(self, document, name=ALONE_NAME):
        path = self.directory / name
        path.write_text(json.dumps(document), encoding="utf-8")
        pin_protocol_1(self.pins, self.directory)
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins),
                               "--capacity", str(self.capacity), str(path)],
                              check=False, capture_output=True, text=True)

    def assert_alone_fault(self, mutate, expected, name=ALONE_NAME):
        document = tickstore_only_document()
        mutate(document)
        result = self.run_alone(document, name)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(expected, result.stderr)

    def test_the_valid_tickstore_only_day_passes(self):
        result = self.run_alone(tickstore_only_document())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_tickstore_only_day_on_linux_passes(self):
        # Review F-5: tick-store's Linux figures at a later commit, without the other three.
        result = self.run_alone(linux_tickstore_only_document(), LINUX_ALONE_NAME)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_linux_tickstore_only_day_warm_by_residency(self):
        document = linux_tickstore_only_document()
        for variant in document["systems"]["tickstore"]["variants"].values():
            for entry in variant["queries"].values():
                entry["warm"]["proof"] = {"pages": 8, "resident_before": 8, "resident_after": 8}
        result = self.run_alone(document, LINUX_ALONE_NAME)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("bytes across the timed repetitions", result.stderr)

    def test_a_linux_tickstore_only_day_without_the_guards_memory(self):
        document = linux_tickstore_only_document()
        document["systems"]["tickstore"]["peak_memory"] = \
            tickstore_only_document()["systems"]["tickstore"]["peak_memory"]
        result = self.run_alone(document, LINUX_ALONE_NAME)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("tickstore.peak_memory: no vmhwm_bytes_largest", result.stderr)

    def test_a_macos_tickstore_only_day_without_time_memory(self):
        def fault(document):
            del document["systems"]["tickstore"]["peak_memory"]["peak_memory_footprint_bytes"]
        self.assert_alone_fault(fault, "no peak_memory_footprint_bytes from")

    def test_another_system_beside_it(self):
        def fault(document):
            document["systems"]["parquet"] = system_block("parquet", "macOS")
        self.assert_alone_fault(fault, "holds tick-store alone, and this holds")

    def test_a_variant_left_out(self):
        def fault(document):
            del document["systems"]["tickstore"]["variants"][variant_rule.TICKSTORE_VARIANTS[1]]
        self.assert_alone_fault(fault, "alone it runs every variant")

    def test_the_four_system_rule_stated(self):
        def fault(document):
            document["variant_selection"]["rule"] = variant_rule.RULE
        self.assert_alone_fault(fault, "tick-store-only rule")

    def test_a_tickstore_only_day_named_as_the_four_system_day(self):
        result = self.run_alone(tickstore_only_document(),
                                f"macOS-{DAY}-full-{COMMIT[:12]}.json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("must be named <host>-<day>-full-tickstore-<commit>.json", result.stderr)

    def test_a_four_system_day_named_as_tickstore_only(self):
        document, _ = full_day_document()
        result = self.run_alone(document, f"Linux-{DAY}-full-tickstore-{COMMIT[:12]}.json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("and only it may be", result.stderr)

    def test_no_peak_memory_from_time(self):
        def fault(document):
            del document["systems"]["tickstore"]["peak_memory"]["peak_memory_footprint_bytes"]
        self.assert_alone_fault(fault, "no peak_memory_footprint_bytes from time -l")

    def test_a_warm_number_without_residency(self):
        def fault(document):
            variant = document["systems"]["tickstore"]["variants"]["v3-seg1Mi"]
            variant["queries"]["Q3"]["warm"]["proof"]["resident_after"] = 7
        self.assert_alone_fault(fault, "without residency proven before and after")

    def test_a_label_that_does_not_say_alone(self):
        def fault(document):
            document["label"] = "A full day, every query with nothing clipped."
        self.assert_alone_fault(fault, "must say it is tick-store alone")

    def test_a_heap_claimed_for_a_questdb_that_did_not_run(self):
        def fault(document):
            document["heap"] = heap_block("macOS")
        self.assert_alone_fault(fault, "must state that QuestDB did not run")

    def test_a_slice_cannot_be_tickstore_only(self):
        document = valid_document("macOS")
        document["scope"] = "tickstore-only"
        result = self.run_alone(document, f"macOS-{SLICE}-{COMMIT[:12]}.json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("only a full day can be tick-store alone", result.stderr)


class Swap(Harness):
    """Swap per phase, and each timed query's swap-in with its flag."""

    def test_a_class_with_no_write_phase(self):
        def fault(document):
            del b_of(document, "questdb")["swap"]["write"]
        self.assert_fault(fault, "questdb.v.B.swap.write: no swap counts")

    def test_a_class_with_no_read_back_phase(self):
        def fault(document):
            b_of(document, "tickstore")["swap"]["read_back"]["pages_out"] = -1
        self.assert_fault(fault, "tickstore.v.B.swap.read_back: no swap counts", "Linux")

    def test_a_variant_with_no_warm_or_cold_phase(self):
        for phase in ("warm_queries", "cold_queries"):
            def fault(document, phase=phase):
                del document["systems"]["arcticdb"]["variants"]["v"]["swap"][phase]
            self.assert_fault(fault, f"arcticdb.v.swap.{phase}: no swap counts")

    def test_totals_that_do_not_add_up(self):
        def fault(document):
            document["systems"]["parquet"]["swap_by_phase"]["cold_queries"]["pages_in"] = 0
        self.assert_fault(fault, "parquet.swap_by_phase.cold_queries")

    def test_a_query_with_no_timed_swap(self):
        def fault(document):
            del q1_of(document)["warm"]["swap_timed"]
        self.assert_fault(fault, "parquet.v.queries.Q1.warm.swap_timed: no swap counts")

    def test_swap_in_during_a_timed_query_left_unflagged(self):
        def fault(document):
            q1_of(document, "questdb")["cold"]["swap_timed"]["flagged"] = False
        self.assert_fault(fault, "Q1.cold.swap_timed: flagged is False with 2 pages swapped in",
                          "Linux")

    def test_a_flag_with_no_swap_in(self):
        def fault(document):
            q1_of(document)["warm"]["swap_timed"]["flagged"] = True
        self.assert_fault(fault, "Q1.warm.swap_timed: flagged is True with 0 pages")

    def test_cold_runs_whose_swap_in_does_not_add_up(self):
        def fault(document):
            q1_of(document)["cold"]["swap_timed"]["pages_in_by_repetition"] = [0, 1]
        self.assert_fault(fault, "no pages swapped in for each cold run that add up")

    def test_no_swap_method(self):
        def fault(document):
            del document["method"]["swap"]["timed_policy"]
        self.assert_fault(fault, "what a timed query with swap-in becomes is not stated")

    def test_a_slice_file_from_before_swap_was_counted(self):
        def fault(document):
            document["header"]["schema"] = 1
        self.assert_fault(fault, "header.schema: 1, and a file must count swap per phase")


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


# Protocol 2: the documents written since the final review's comparison protocol. The committed
# files carry no protocol and count as 1, held to the rules they were measured by; everything
# below applies to protocol 2 alone.

SLOWEST = "slowest of {}"


def nearest_rank(count):
    return max(1, -(-99 * count // 100))


def latency_block(seconds):
    """A latency block as compare/run_system.py writes it, worked out here independently."""
    ordered = sorted(seconds)
    count = len(ordered)
    rank = nearest_rank(count)
    middle = count // 2
    median = ordered[middle] if count % 2 else (ordered[middle - 1] + ordered[middle]) / 2
    tail = ordered[rank - 1] * 1e6
    return {"n": count, "median_us": median * 1e6, "p99_us": tail, "tail_us": tail,
            "tail_rank": rank, "tail_is_slowest": rank == count,
            "tail": (SLOWEST.format(count) if rank == count
                     else f"99th percentile, nearest rank: run {rank} of {count}"),
            "mean_us": sum(ordered) / count * 1e6, "min_us": ordered[0] * 1e6,
            "max_us": ordered[-1] * 1e6, "seconds_by_repetition": list(seconds)}


WARM_SECONDS = [10e-6, 9e-6, 12e-6] * 10
COLD_SECONDS = [90e-6, 110e-6] + [100e-6] * 5
OPEN_INSIDE = {"parquet": True, "arcticdb": False, "questdb": False, "tickstore": True}
TICKSTORE_OPEN_SECONDS = [30e-6, 40e-6] + [35e-6] * 5
ENGINE_WARM_SECONDS = [9e-6, 8e-6, 11e-6] * 10
# Each no longer than its repetition's query, the cold figure less the open (review RF-7).
ENGINE_COLD_SECONDS = [50e-6, 65e-6] + [60e-6] * 5


WRITE_COUNTER = {
    "Linux": "block device stat, sectors read, machine-wide",
    "macOS": ("bytes the device read for the processes that did the write: proc_pid_rusage's "
              "ri_diskio_bytesread of the worker, of each server process, and of each child the "
              "worker ran, read before it was reaped")}


def second_sibling_block():
    return {"core": 6, "sibling": 7, "topology": "thread_siblings_list", "method": "jiffies",
            "cases": {"parquet": 0.4, "arcticdb": 0.2, "questdb": 1.5, "tickstore": 0.1}}


def protocol_2(document):
    """The same document as protocol 2 writes it: every new field present and true."""
    document["header"]["protocol"] = 2
    document["header"]["second_sibling_busy_percent"] = (
        second_sibling_block() if document["header"]["host"] == "Linux" else "not-available")
    document["header"]["turbo_readings"] = (
        {"before_first_system": "enabled", "after_last_system": "enabled"}
        if document["header"]["host"] == "Linux" else "not-available")
    if "variant_selection" in document and \
            document["variant_selection"].get("rule") == variant_rule.RULE_PROTOCOL_1:
        document["variant_selection"]["rule"] = variant_rule.RULE
    for name, system in document["systems"].items():
        for variant in system["variants"].values():
            for entry in variant["queries"].values():
                warm_extra = {key: value for key, value in entry["warm"].items()
                              if key in ("proof", "swap_timed")}
                if "pages" in warm_extra["proof"]:
                    warm_extra["proof"]["warm_up_runs"] = 1
                    warm_extra["warm_up_seconds"] = [50e-6]
                if "accepted" in warm_extra["proof"]:
                    warm_extra["proof"]["accepted"]["warm_up_seconds"] = [50e-6]
                cold_extra = {key: value for key, value in entry["cold"].items()
                              if key in ("proofs", "swap_timed", "run_again_refilled_pages")}
                entry["warm"] = {**latency_block(WARM_SECONDS), **warm_extra}
                entry["cold"] = {**latency_block(COLD_SECONDS), **cold_extra}
                for proof in entry["cold"]["proofs"]:
                    proof.update({"open_inside_clock": OPEN_INSIDE[name],
                                  "resident_bytes_before_clock": 0 if OPEN_INSIDE[name] else 8192,
                                  "resident_bytes_before_open": 0,
                                  "resident_bytes_before_query": 8192})
                if name == "tickstore":
                    entry["warm"]["engine"] = latency_block(ENGINE_WARM_SECONDS)
                    entry["cold"]["engine"] = latency_block(ENGINE_COLD_SECONDS)
                    entry["cold"]["parts"] = {
                        "open_seconds_by_repetition": list(TICKSTORE_OPEN_SECONDS),
                        "query_seconds_by_repetition": [
                            round(total - opened, 12) for total, opened in
                            zip(COLD_SECONDS, TICKSTORE_OPEN_SECONDS, strict=True)]}
            variant["warm_worker_open"] = {"seconds": 0.01}
            variant["python_floor"] = {**latency_block([1e-6, 2e-6, 3e-6]), "what": "floor"}
            for entry in variant["classes"].values():
                write = entry["write"]
                day = "day" in document["input"]
                write["repetitions"] = FULL_DAY_WRITE_RUNS if day else WRITE_RUNS
                write["seconds"] = [2.0] if day else [2.1, 2.0, 1.9]
                write["by_repetition"] = [
                    {"seconds": total, "engine_seconds": engine, "python_seconds": 0.1,
                     "engine_only_seconds": round(total - 0.1, 9), "input_pages": 100,
                     "input_resident_pages_at_start": 100, "device_bytes_read": 0,
                     "device_counter": WRITE_COUNTER[document["header"]["host"]],
                     **({"device_read_by_process": {"worker": 0, "waited children": 0}}
                        if document["header"]["host"] == "macOS" else {})}
                    for total, engine in zip(write["seconds"], (1.9,) if day else (2.0, 1.9, 1.8),
                                             strict=True)]
                write["engine_only_seconds_median"] = 1.9
                write["times"] = {"seconds": "the whole write", "engine_only_seconds": "less "
                                  "reading and preparing the input"}
    return document


class Protocol2(Harness):
    def assert_fault_2(self, mutate, expected, host="macOS"):
        document = protocol_2(copy.deepcopy(valid_document(host)))
        mutate(document)
        result = self.run_on(document, host)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(expected, result.stderr)

    def test_the_valid_protocol_2_files_pass(self):
        for host in ("macOS", "Linux"):
            result = self.run_on(protocol_2(valid_document(host)), host)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_an_unknown_protocol(self):
        def fault(document):
            document["header"]["protocol"] = 3
        self.assert_fault_2(fault, "header.protocol: 3")

    # F-4: the tail figure is named for what it is.

    def test_the_slowest_of_n_not_called_the_slowest(self):
        def fault(document):
            q1_of(document)["cold"]["tail_is_slowest"] = False
        self.assert_fault_2(fault, "Q1.cold: tail_is_slowest is False, and rank 7 of 7 is the "
                                   "slowest run")

    def test_the_slowest_of_n_called_a_99th_percentile(self):
        def fault(document):
            q1_of(document)["warm"]["tail"] = "99th percentile"
        self.assert_fault_2(fault, "Q1.warm: the tail figure is called '99th percentile', "
                                   "not 'slowest of 30'")

    def test_a_tail_figure_with_no_rank(self):
        def fault(document):
            del q1_of(document, "tickstore")["warm"]["engine"]["tail_rank"]
        self.assert_fault_2(fault, "Q1.warm.engine: the tail figure's rank is None")

    def test_a_tail_figure_that_is_not_the_one_p99_holds(self):
        def fault(document):
            q1_of(document)["warm"]["tail_us"] = 11.0
        self.assert_fault_2(fault, "Q1.warm: tail_us 11.0 is not the figure p99_us holds")

    def test_a_floor_with_no_tail_figure(self):
        def fault(document):
            del document["systems"]["arcticdb"]["variants"]["v"]["python_floor"]["tail"]
        self.assert_fault_2(fault, "arcticdb.v.python_floor: the tail figure is called None")

    # F-14: the repetitions themselves, and every summary recomputed from them.

    def test_a_latency_block_with_no_repetitions(self):
        def fault(document):
            del q1_of(document, "questdb")["cold"]["seconds_by_repetition"]
        self.assert_fault_2(fault, "questdb.v.queries.Q1.cold: no seconds for each of its 7 "
                                   "repetitions")

    def test_a_median_that_does_not_follow_from_the_repetitions(self):
        def fault(document):
            seconds = q1_of(document)["warm"]["seconds_by_repetition"]
            seconds[:] = [11e-6 if value == 10e-6 else value for value in seconds]
        self.assert_fault_2(fault, "Q1.warm: median_us 10.0 is not the median of its "
                                   "repetitions, 11.0")

    def test_a_tail_that_does_not_follow_from_the_repetitions(self):
        def fault(document):
            q1_of(document, "tickstore")["cold"]["engine"]["seconds_by_repetition"][1] = 64e-6
        self.assert_fault_2(fault, "Q1.cold.engine: tail_us 65.0 is not the slowest of 7 of "
                                   "its repetitions, 64.0")

    def test_a_write_median_that_does_not_follow_from_the_repetitions(self):
        def fault(document):
            b_of(document, "parquet")["write"]["seconds"][0] = 1.0
            b_of(document, "parquet")["write"]["by_repetition"][0]["seconds"] = 1.0
        self.assert_fault_2(fault, "parquet.v.B.write: seconds_median 2.0 is not the median "
                                   "of its repetitions, 1.9")

    def test_a_write_engine_median_that_does_not_follow(self):
        def fault(document):
            b_of(document, "arcticdb")["write"]["by_repetition"][1]["engine_seconds"] = 1.95
        self.assert_fault_2(fault, "arcticdb.v.B.write: engine_seconds 1.9 is not the median "
                                   "of its repetitions, 1.95")

    def test_a_write_with_no_repetition_kept(self):
        def fault(document):
            b_of(document, "tickstore")["write"]["by_repetition"].pop()
        self.assert_fault_2(fault, "tickstore.v.B.write: by_repetition holds 2 repetitions, "
                                   "and the write ran 3")

    def test_a_protocol_2_file_of_5_warm_and_3_cold_repetitions(self):
        # Review RF-6: its tail honestly says "slowest of 5", and it is still not the protocol.
        def fault(document):
            query = q1_of(document, "questdb")
            query["warm"].update(latency_block(WARM_SECONDS[:5]))
            query["cold"].update(latency_block(COLD_SECONDS[:3]))
            query["cold"]["proofs"] = query["cold"]["proofs"][:3]
            query["cold"]["swap_timed"]["pages_in_by_repetition"] = [0, 0, 2]
        self.assert_fault_2(fault, "questdb.v.queries.Q1.warm: 5 repetitions, and a reported "
                                   "file runs 30")
        self.assert_fault_2(fault, "questdb.v.queries.Q1.cold: 3 repetitions, and a reported "
                                   "file runs 7")

    # E-3: the second pinned CPU's sibling, held to the first one's rule.

    def test_a_linux_file_with_no_reading_of_the_second_sibling(self):
        def fault(document):
            del document["header"]["second_sibling_busy_percent"]
        self.assert_fault_2(fault, "header.second_sibling_busy_percent: Linux must measure it, "
                                   "case by case", "Linux")

    def test_a_busy_second_sibling(self):
        def fault(document):
            document["header"]["second_sibling_busy_percent"]["cases"]["questdb"] = 6.0
        self.assert_fault_2(fault, "header.second_sibling_busy_percent: questdb reads 6.0%, "
                                   "over the 5.0% limit", "Linux")

    def test_a_system_with_no_reading_of_the_second_sibling(self):
        def fault(document):
            del document["header"]["second_sibling_busy_percent"]["cases"]["tickstore"]
        self.assert_fault_2(fault, "header.second_sibling_busy_percent: no reading for "
                                   "tickstore", "Linux")

    def test_the_first_cpus_sibling_passed_off_as_the_seconds(self):
        def fault(document):
            document["header"]["second_sibling_busy_percent"].update(core=4, sibling=5)
        self.assert_fault_2(fault, "header.second_sibling_busy_percent: core 4 is not the "
                                   "pinned core 6", "Linux")

    def test_a_macos_file_claiming_a_second_sibling(self):
        def fault(document):
            document["header"]["second_sibling_busy_percent"] = second_sibling_block()
        self.assert_fault_2(fault, "header.second_sibling_busy_percent: macOS has no pinned "
                                   "core to measure")

    # F-2, recording only: both times, the input's residency at the start, the device's reads.

    def test_a_write_with_no_engine_only_time(self):
        def fault(document):
            del b_of(document, "questdb")["write"]["by_repetition"][2]["engine_only_seconds"]
        self.assert_fault_2(fault, "questdb.v.B.write: repetition 3 has no engine-only seconds "
                                   "within its total")

    def test_an_engine_only_time_longer_than_the_total(self):
        def fault(document):
            b_of(document, "parquet")["write"]["by_repetition"][0]["engine_only_seconds"] = 2.2
        self.assert_fault_2(fault, "parquet.v.B.write: repetition 1 has no engine-only seconds "
                                   "within its total")

    def test_an_engine_only_time_that_is_not_the_total_less_python(self):
        # Review RF-2: within the total, and its median right, but not the repetition's own.
        def fault(document):
            write = b_of(document, "parquet")["write"]
            for item in write["by_repetition"]:
                item["engine_only_seconds"] = 0.5
            write["engine_only_seconds_median"] = 0.5
        self.assert_fault_2(fault, "parquet.v.B.write: repetition 1's engine-only seconds 0.5 are "
                                   "not its seconds less its python_seconds, 2.0")

    def test_an_engine_only_median_that_does_not_follow(self):
        def fault(document):
            b_of(document, "arcticdb")["write"]["engine_only_seconds_median"] = 1.8
        self.assert_fault_2(fault, "arcticdb.v.B.write: engine_only_seconds_median 1.8 is not "
                                   "the median of its repetitions, 1.9")

    def test_a_write_not_saying_how_much_input_was_resident(self):
        def fault(document):
            del b_of(document, "questdb")["write"]["by_repetition"][0][
                "input_resident_pages_at_start"]
        self.assert_fault_2(fault, "questdb.v.B.write: repetition 1 does not say how much of "
                                   "its input was resident when its clock started")

    def test_a_write_with_no_device_reads(self):
        def fault(document):
            del b_of(document, "arcticdb")["write"]["by_repetition"][1]["device_bytes_read"]
        self.assert_fault_2(fault, "arcticdb.v.B.write: repetition 2 records no device bytes "
                                   "read while it ran")

    def test_a_macos_write_counting_disk0s_reads_and_writes(self):
        # Review RF-5: iostat's disk0 total holds the write's own output, so it is no count of
        # the reads; macOS counts the reads of the processes that did the write.
        def fault(document):
            b_of(document, "questdb")["write"]["by_repetition"][0]["device_counter"] = (
                "iostat's total for disk0, MiB read and written, machine-wide")
        self.assert_fault_2(fault, "questdb.v.B.write: repetition 1 counts its device reads by "
                                   "\"iostat's total for disk0, MiB read and written, "
                                   "machine-wide\", and a macOS write counts the reads of the "
                                   "processes that did it, by ri_diskio_bytesread")

    def test_a_macos_write_counter_that_only_mentions_the_counter(self):
        # Review RV-7: the counter is the one statement, not any text that names it.
        def fault(document):
            b_of(document, "questdb")["write"]["by_repetition"][0]["device_counter"] = (
                "iostat disk0 total; ri_diskio_bytesread unavailable")
        self.assert_fault_2(fault, "questdb.v.B.write: repetition 1 counts its device reads by "
                                   "'iostat disk0 total; ri_diskio_bytesread unavailable'")

    def test_a_macos_write_whose_reads_by_process_are_not_its_reads(self):
        # Review RV-7: the reads of each process that did the write, the worker among them, add
        # up to the write's device reads.
        for by_process in (None, {"questdb": 0}, {"worker": 4096}, {"worker": -1, "x": 1},
                           "worker"):
            def fault(document, by_process=by_process):
                item = b_of(document, "questdb")["write"]["by_repetition"][0]
                if by_process is None:
                    del item["device_read_by_process"]
                else:
                    item["device_read_by_process"] = by_process
            with self.subTest(by_process=by_process):
                self.assert_fault_2(fault, "questdb.v.B.write: repetition 1's device reads by "
                                           "process do not hold the worker and add up to its "
                                           "0 bytes")

    def test_a_write_not_saying_what_its_times_hold(self):
        def fault(document):
            del b_of(document, "parquet")["write"]["times"]["engine_only_seconds"]
        self.assert_fault_2(fault, "parquet.v.B.write: does not say what its total and "
                                   "engine-only seconds hold")

    def test_a_tickstore_write_from_a_source_not_resident(self):
        def fault(document):
            b_of(document, "tickstore")["write"]["by_repetition"][1][
                "input_resident_pages_at_start"] = 60
        self.assert_fault_2(fault, "tickstore.v.B.write: repetition 2 started its clock with 60 "
                                   "of its source's 100 pages resident")

    def test_a_slice_write_from_an_export_not_resident(self):
        # Review F-10: on the slice every system's export is read through before its write.
        def fault(document):
            b_of(document, "parquet")["write"]["by_repetition"][1][
                "input_resident_pages_at_start"] = 60
        self.assert_fault_2(fault, "parquet.v.B.write: repetition 2 started its clock with 60 "
                                   "of its input's 100 pages resident")

    # F-1: the cold clock's start, and what was resident when it started.

    def test_a_cold_proof_with_no_residency_before_its_clock(self):
        def fault(document):
            del q1_of(document, "arcticdb")["cold"]["proofs"][1]["resident_bytes_before_clock"]
        self.assert_fault_2(fault, "arcticdb.v.queries.Q1.cold[1]: no store bytes resident "
                                   "before the clock")

    def test_pages_resident_before_a_clock_that_holds_the_whole_open(self):
        def fault(document):
            q1_of(document, "parquet")["cold"]["proofs"][0]["resident_bytes_before_clock"] = 4096
        self.assert_fault_2(fault, "parquet.v.queries.Q1.cold[0]: 4096 bytes of the store "
                                   "resident before a clock that holds its whole open")

    def test_a_system_claiming_its_open_is_where_it_is_not(self):
        def fault(document):
            for proof in q1_of(document, "tickstore")["cold"]["proofs"]:
                proof["open_inside_clock"] = False
        self.assert_fault_2(fault, "tickstore.v.queries.Q1.cold[0]: open_inside_clock is False, "
                                   "and tickstore's whole open is inside its cold clock")

    def test_a_tickstore_cold_figure_without_its_open(self):
        def fault(document):
            cold = q1_of(document, "tickstore")["cold"]
            cold["seconds_by_repetition"][0] = cold["parts"]["query_seconds_by_repetition"][0]
        self.assert_fault_2(fault, "tickstore.v.queries.Q1.cold: repetition 1 is not its open "
                                   "and its query")

    def test_a_tickstore_cold_block_without_its_parts(self):
        def fault(document):
            del q1_of(document, "tickstore")["cold"]["parts"]
        self.assert_fault_2(fault, "tickstore.v.queries.Q1.cold: its open and its query are not "
                                   "kept for each repetition")

    def test_a_variant_with_no_open_time_for_its_warm_worker(self):
        def fault(document):
            del document["systems"]["questdb"]["variants"]["v"]["warm_worker_open"]
        self.assert_fault_2(fault, "questdb.v.warm_worker_open: the warm worker's open is not "
                                   "timed")

    # Review RF-7: tick-store's engine time, repetition by repetition, within what holds it.

    def test_a_cold_engine_time_longer_than_its_query(self):
        # The cold figure holds the open too, so the median rule alone let this pass.
        def fault(document):
            q1_of(document, "tickstore")["cold"]["engine"] = latency_block(
                [80e-6, 100e-6] + [60e-6] * 5)
        self.assert_fault_2(fault, "tickstore.v.queries.Q1.cold: repetition 1's engine time 80.0 "
                                   "us is longer than its query's round trip, 60.0 us")

    def test_a_warm_engine_time_longer_than_its_round_trip(self):
        def fault(document):
            seconds = list(ENGINE_WARM_SECONDS)
            seconds[1] = 9.5e-6
            q1_of(document, "tickstore")["warm"]["engine"] = latency_block(seconds)
        self.assert_fault_2(fault, "tickstore.v.queries.Q1.warm: repetition 2's engine time 9.5 "
                                   "us is longer than its round trip, 9.0 us")

    # F-3: the slice's warm path warms up untimed first, as the day's does.

    def test_a_slice_warm_figure_with_no_warm_up(self):
        def fault(document):
            q1_of(document, "questdb")["warm"]["proof"]["warm_up_runs"] = 0
        self.assert_fault_2(fault, "questdb.v.queries.Q1.warm: no untimed run before the "
                                   "repetitions")

    def test_untimed_runs_stated_with_other_seconds_beside_them(self):
        # Review RF-10: the stated count is borne out by the untimed runs' own seconds.
        for seconds in (None, [], [50e-6, 60e-6], [0.0], ["x"]):
            with self.subTest(seconds=seconds):
                def fault(document, seconds=seconds):
                    warm = q1_of(document, "questdb")["warm"]
                    if seconds is None:
                        del warm["warm_up_seconds"]
                    else:
                        warm["warm_up_seconds"] = seconds
                self.assert_fault_2(fault, "questdb.v.queries.Q1.warm: 1 untimed runs stated, "
                                           f"and their seconds are {seconds!r}")

    def test_protocol_1_files_keep_the_rules_they_were_measured_by(self):
        result = self.run_on(valid_document("Linux"), "Linux")
        self.assertEqual(result.returncode, 0, result.stderr)

    # Review RF-1: protocol 1 is the committed files' alone, never a way round protocol 2.

    def test_a_current_harness_document_without_its_protocol(self):
        def fault(document):
            document["header"]["commit"] = OTHER_COMMIT
            del document["header"]["protocol"]
            del q1_of(document)["warm"]["proof"]["warm_up_runs"]
        self.assert_fault_2(fault, PROTOCOL_1_UNPINNED)

    def test_a_current_harness_document_stating_a_committed_files_commit(self):
        # Review RV-6: a header's commit is the document's own word, so it exempts nothing.
        document = protocol_2(copy.deepcopy(valid_document("Linux")))
        del document["header"]["protocol"]
        del q1_of(document)["warm"]["proof"]["warm_up_runs"]
        document["header"]["commit"] = COMMIT
        path = self.directory / f"Linux-{SLICE}-{COMMIT[:12]}.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins),
                                 "--capacity", str(self.capacity), str(path)],
                                check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(PROTOCOL_1_UNPINNED, result.stderr)

    def test_a_committed_protocol_1_file_with_a_byte_changed(self):
        # Review RV-6: the committed files are pinned by their bytes. The same document written
        # again, its content unchanged and its bytes not, is no longer one of them.
        committed = REPO_ROOT / "results" / "compare" / "macOS-12302019-open-67ae574f5666.json"
        document = json.loads(committed.read_text(encoding="utf-8"))
        copied = self.directory / committed.name
        copied.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        for path, code in ((committed, 0), (copied, 1)):
            with self.subTest(path=path.parent.name):
                result = subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)],
                                        check=False, capture_output=True, text=True)
                self.assertEqual(result.returncode, code, result.stderr)
        self.assertIn(PROTOCOL_1_UNPINNED, result.stderr)

    def test_a_document_declaring_protocol_1(self):
        for commit in (COMMIT, OTHER_COMMIT):
            def fault(document, commit=commit):
                document["header"]["commit"] = commit
                document["header"]["protocol"] = 1
            with self.subTest(commit=commit):
                self.assert_fault_2(fault, "header.protocol: 1, and a file states protocol 2")

    def test_a_protocol_that_is_not_the_integer_2(self):
        for stated in (True, 2.0, "2", None):
            def fault(document, stated=stated):
                document["header"]["protocol"] = stated
            with self.subTest(stated=stated):
                self.assert_fault_2(fault, f"header.protocol: {stated!r}, and a file states "
                                           "protocol 2")

    # cpufreq boost read before the first system and after the last (review E-10's leftover).

    def test_a_linux_turbo_read_only_after_the_run(self):
        def fault(document):
            del document["header"]["turbo_readings"]
        self.assert_fault_2(fault, "header.turbo_readings: cpufreq boost was not read before "
                                   "the first system and after the last", "Linux")

    def test_a_linux_turbo_not_read_before_the_first_system(self):
        for reading in (None, "unavailable: no cpufreq boost switch, as on a virtual machine"):
            def fault(document, reading=reading):
                readings = document["header"]["turbo_readings"]
                if reading is None:
                    del readings["before_first_system"]
                else:
                    readings["before_first_system"] = reading
            with self.subTest(reading=reading):
                self.assert_fault_2(fault, f"header.turbo_readings: before_first_system is "
                                           f"{reading!r}, not a reading of cpufreq boost",
                                    "Linux")

    def test_a_linux_turbo_that_changed_across_the_run(self):
        def fault(document):
            document["header"]["turbo_readings"]["before_first_system"] = "disabled"
        self.assert_fault_2(fault, "header.turbo_readings: cpufreq boost read 'disabled' before "
                                   "the first system and 'enabled' after the last", "Linux")

    def test_a_linux_turbo_stated_against_its_readings(self):
        def fault(document):
            document["header"]["turbo_readings"] = {"before_first_system": "disabled",
                                                    "after_last_system": "disabled"}
        self.assert_fault_2(fault, "header.turbo: states 'enabled', and both readings are "
                                   "'disabled'", "Linux")

    def test_a_macos_document_claiming_turbo_readings(self):
        def fault(document):
            document["header"]["turbo_readings"] = {"before_first_system": "enabled",
                                                    "after_last_system": "enabled"}
        self.assert_fault_2(fault, "header.turbo_readings: macOS has no boost switch to read")

    def test_a_protocol_2_document_from_any_commit_passes(self):
        document = protocol_2(valid_document("Linux"))
        document["header"]["commit"] = OTHER_COMMIT
        result = self.run_on(document, "Linux")
        self.assertEqual(result.returncode, 0, result.stderr)


class Protocol2FullDay(Harness):
    run_full = FullDay.run_full

    def test_the_valid_protocol_2_full_day_passes(self):
        document, source = full_day_document()
        result = self.run_full(protocol_2(document), protocol_2(source))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_day_write_may_start_with_its_export_partly_resident(self):
        # The full day's write protocol is unchanged (review F-2): the others' export is taken
        # as the cache holds it, and only recorded.
        document, source = full_day_document()
        protocol_2(document)
        for system in ("parquet", "arcticdb", "questdb"):
            for variant in document["systems"][system]["variants"].values():
                for entry in variant["classes"].values():
                    entry["write"]["by_repetition"][0]["input_resident_pages_at_start"] = 60
        result = self.run_full(document, protocol_2(source))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_tickstore_day_write_from_a_source_not_resident(self):
        # tick-store's source is whole on a day too, unlike the others' export (review RF-9).
        document, source = full_day_document()
        protocol_2(document)
        b_of(document, "tickstore")["write"]["by_repetition"][0][
            "input_resident_pages_at_start"] = 60
        result = self.run_full(document, protocol_2(source))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("tickstore.v.B.write: repetition 1 started its clock with 60 of its "
                      "source's 100 pages resident", result.stderr)

    def test_the_rule_as_protocol_1_stated_it(self):
        document, source = full_day_document()
        protocol_2(document)
        document["variant_selection"]["rule"] = variant_rule.RULE_PROTOCOL_1
        result = self.run_full(document, protocol_2(source))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("variant_selection: the rule is not compare/variant_rule.py's",
                      result.stderr)

    def test_a_day_warm_up_stated_with_other_seconds_beside_it(self):
        # Review RV-8: a day's per-process warm proof holds its untimed runs to their seconds,
        # as the slice's residency proof does (RF-10).
        for seconds in (None, [], [50e-6, 60e-6], [0.0], ["x"]):
            with self.subTest(seconds=seconds):
                document, source = full_day_document()
                protocol_2(document)
                accepted = q1_of(document, "questdb")["warm"]["proof"]["accepted"]
                if seconds is None:
                    del accepted["warm_up_seconds"]
                else:
                    accepted["warm_up_seconds"] = seconds
                result = self.run_full(document, protocol_2(source))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("questdb.v.queries.Q1.warm: 1 untimed runs stated, and their "
                              f"seconds are {seconds!r}", result.stderr)

    def test_a_tickstore_alone_day_passes(self):
        # Review RV-4: a protocol 2 day of tick-store alone, the only kind the harness writes
        # now, passes on each host, from a commit that must state its protocol (RF-1).
        alone = TickstoreOnly.run_alone
        for host, make in (("macOS", tickstore_only_document),
                           ("Linux", linux_tickstore_only_document)):
            with self.subTest(host=host):
                document = protocol_2(make())
                document["header"]["commit"] = OTHER_COMMIT
                if host == "Linux":
                    # The second sibling is read for the one system that ran, as the first is.
                    document["header"]["second_sibling_busy_percent"]["cases"] = {
                        "tickstore": 0.1}
                result = alone(self, document, f"{host}-{DAY}-full-tickstore-"
                                               f"{OTHER_COMMIT[:12]}.json")
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
