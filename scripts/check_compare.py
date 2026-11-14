#!/usr/bin/env python3
"""Validate comparison results. Exits 1 and names every problem if a file has one.

    python3 -I scripts/check_compare.py results/compare/*.json

A comparison result claims that four systems held the same records under stated settings and
gave the same answers. This checks that the file can back each claim: a requested setting must
equal the one in force, every store must have rebuilt the source through the oracle, every
answer must equal the ground truth, every warm and cold number must carry its proof and the
harness's own repetitions (30 warm and 7 cold, each class written 3 times on the slice and once
on a day, compare/repetitions.py, which compare/run_system.py runs), a class B result its flush
proof, the run its measurement lock, and a Linux heap the setting that the full-day capacity
check found adequate. It also checks that the file is safe to publish. A system that could not
prove something says so in the file, and the file is valid, as long as it does not claim the
proof.

Every file also counts the host's swap: pages swapped in and out over each phase, write,
read-back, warm queries and cold queries, for each variant and class, and over each query's
timed runs alone, with that query flagged whenever any page was swapped in. A count must be a
whole number, the flag must follow from it, and the system's totals must add up.

A full-day file, `<host>-<day>-full-<commit>.json`, is held to every rule above against the
day's pin, and to more: nothing clipped (every window as BENCHMARKS.md defines it, the busiest
locate over the whole hour, Q5 over every record), each store read back whole, warm proven query
by query from the block device's own counter, each system's peak memory from the guard, and its
variants what compare/variant_rule.py gives from the slice file it names. A slice file can never
pass as a full-day one, nor the reverse.

A tick-store-only full day, `<host>-<day>-full-tickstore-<commit>.json`, gives macOS's figures
for the targets whose host is Both, and tick-store's own Linux figures at a commit with no
four-system day. It is held to the full day's rules on tick-store, and tick-store is the only
system and every variant it has ran, the headline first. On macOS two more things differ, each
stated in the file: warm is the slice's residency proof, since the per-process proof reads
/proc, and peak memory comes from BSD time -l, since the guard reads /proc too. On Linux it
keeps the four-system day's proofs: warm by each process's storage reads, and the guard's peak
memory. Its name and its scope keep the two kinds of day apart: only a file whose scope is
tick-store-only may carry `-full-tickstore-`, and it must, so a Linux subset can never stand in
for the four-system day, which must hold all four systems, nor the reverse.

Every file states the comparison protocol it was measured under. The committed files measured
before the final review's protocol state none and count as protocol 1, held to the rules above;
only those files, each pinned by the SHA-256 of its bytes in the pins file, may state none, and
any file that states one must state exactly the integer 2. A protocol 2 file is held to more: each latency block names its tail
figure for what it is, the nearest-rank 99th percentile with n and the rank used, which below
100 runs is the slowest run and must say so; and every latency block and every write keeps each
repetition's seconds, from which its median, its tail figure and its write medians are
recomputed here. On Linux the sibling of the second pinned CPU, cpu6, is measured and held to
the first one's rule, and cpufreq boost is read before the first system and after the last and
stated only if the two agree. Every write records its total and its engine-only seconds, says what each
holds, and records how much of its input was resident when its clock started and the device
bytes read while it ran, on macOS for the processes that did it; tick-store's source must have
been wholly resident. Every cold proof records the store's bytes resident just before its clock
started, which must be 0 where the clock holds the system's whole open (Parquet's, which opens
the file inside every call, and tick-store's, whose cold figure adds the open its query tool
reports, both parts kept); and each warm worker's open is timed once. Warm repetitions follow an
untimed, answer-checked run of each query, on the slice as on a day. On the slice every system's
write starts its clock with its input wholly resident.

Standard library only. tests/scripts/test_check_compare.py shows each rule failing on an
injected fault.
"""

import argparse
import hashlib
import json
import math
import statistics
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_results  # noqa: E402

sys.path.insert(0, str(REPO_ROOT))
from compare import coldcache, repetitions, variant_rule  # noqa: E402

DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"
DEFAULT_CAPACITY = REPO_ROOT / "results" / "fullday-capacity-12302019.json"
CAPACITY_CITATION = f"results/{DEFAULT_CAPACITY.name}"
LINUX_ONLY = "Linux only"
HEAP_SETTINGS = ("java initial heap", "java heap")
SYSTEMS = ("parquet", "arcticdb", "questdb", "tickstore")
# Systems whose class proof is the flushes made, counted call by call, and not a dirty-page
# control: the harness's own flush for Parquet, the writer's recorded system calls for tick-store.
COUNTED_FLUSHES = ("parquet", "tickstore")
TICKSTORE_MODE = {"A": "page_cache", "B": "device_flush"}
TICKSTORE_COLUMNS = 39
TICKSTORE_SETTINGS = ("store version", "segment records", "records", "durability mode",
                      "flushes made", "query path", "kernel level", "kernel sets that ran")
ITCH_PARSING = "ITCH parsing"
CLASSES = ("A", "B")
PROVEN = "proven"
NOT_PROVEN = "NOT PROVEN"
QUERY_NAMES = ("Q1", "Q2", "Q3", "Q4", "Q5")
COMMIT_MODE_SOURCES = ("cairo.commit.mode (conf file)", "cairo.commit.mode (echo)",
                       "cairo.commit.mode (parsed)")
COMMIT_MODE = {"A": "nosync", "B": "sync"}
HEADER_FIELDS = ("schema", "host", "measurement", "statement", "cpu_model", "kernel", "compiler",
                 "compile_flags", "build_type", "commit", "core", "sibling_busy_percent",
                 "second_thread", "governor", "turbo", "pinning", "questdb_runtime", "load_lines")
RUNTIME_FIELDS = ("jvm_vendor", "jvm_version", "jvm_read_from", "jemalloc_read_from")
ORACLE_FIELDS = ("verdict", "exit_code", "records", "mismatched_records", "hashes_match",
                 "source_sha256", "stored_sha256")
LINUX_CPUS = [4, 6]
# Whether a system's cold clock holds its whole open of the store: compare/systems says why for
# each. Where it does, nothing of the store may be resident when the clock starts.
OPEN_INSIDE_COLD_CLOCK = {"parquet": True, "arcticdb": False, "questdb": False,
                          "tickstore": True}
# What a protocol 2 macOS write counts its device reads by: the reads of the processes that did
# it, never iostat's disk0 total, which holds the write's own output too (review RF-5). The
# statement coldcache.py writes, exactly, not any text that mentions the counter (review RV-7).
MACOS_WRITE_COUNTER = coldcache.ProcessDiskReads.COUNTER
# The second pinned CPU's sibling, measured from protocol 2 and held to the first one's rule.
SECOND_CPU = 6
SECOND_SIBLING = "second_sibling_busy_percent"
# Protocol 2: cpufreq boost read before the first system's part and after the last on Linux.
TURBO_READINGS = "turbo_readings"
TURBO_STATES = ("enabled", "disabled")
FULL_DAY = "full"
TICKSTORE_ONLY_NAME = "full-tickstore"
# tick-store alone on a day: macOS's figures for the targets marked Both, and tick-store's own
# Linux figures at a later commit, each with its host's proofs (review F-5).
TICKSTORE_ONLY_HOSTS = ("Linux", "macOS")
# The BSD time in /usr/bin, named without its path, which no results file may hold.
TIME_SOURCE = "time -l"
NANOS_PER_SECOND = 1_000_000_000
OPEN_NS = (9 * 3600 + 30 * 60) * NANOS_PER_SECOND
HOUR_NS = 3600 * NANOS_PER_SECOND
# BENCHMARKS.md's windows, unclipped: (kind, start, end, one locate or all).
FULL_DAY_QUERIES = {"Q1": ("rows", OPEN_NS, OPEN_NS + NANOS_PER_SECOND, False),
                    "Q2": ("rows", OPEN_NS, OPEN_NS + HOUR_NS, True),
                    "Q3": ("minute_aggregate", OPEN_NS, OPEN_NS + HOUR_NS, True),
                    "Q4": ("rows", OPEN_NS, OPEN_NS + 1_000_000, False),
                    "Q5": ("column_sum", 0, 0, False)}
MAX_WARM_ATTEMPTS = 3
# The first schema whose files count swap per phase. Every file must have it.
SWAP_SCHEMA = 2
CLASS_PHASES = ("write", "read_back")
QUERY_PHASES = ("warm_queries", "cold_queries")
SWAP_FIELDS = ("pages_in", "pages_out")
# A file with no protocol was measured before the final review's comparison protocol.
PROTOCOL_1 = 1
PROTOCOL_2 = 2
# The committed files measured before protocol 2 are the only ones that may state no protocol
# (review RF-1), and they are pinned by the SHA-256 of their bytes, under this key of the pins
# file, not by anything a document says of itself (review RV-6): everything the current harness
# writes is protocol 2. docs/TASKS.md removes the key once the freeze rerun has replaced them.
PROTOCOL_1_FILES = "protocol_1_comparison_files"


def check_settings(where, settings, name, durability):
    problems = []
    if not settings:
        problems.append(f"{where}.settings: nothing read back")
    for item in settings or []:
        if item.get("requested") != item.get("in_force"):
            problems.append(f"{where}.settings: {item.get('name')} requested "
                            f"{item.get('requested')!r}, in force {item.get('in_force')!r}")
        if not item.get("read_from"):
            problems.append(f"{where}.settings: {item.get('name')} does not say where it was read")
    if name == "questdb":
        found = {item.get("name"): item.get("in_force") for item in settings or []}
        for source in COMMIT_MODE_SOURCES:
            if found.get(source) != COMMIT_MODE[durability]:
                problems.append(f"{where}.settings: commit mode from {source!r} is "
                                f"{found.get(source)!r}, not {COMMIT_MODE[durability]}")
    if name == "tickstore":
        problems += check_tickstore_settings(where, settings or [], durability)
    return problems


def check_tickstore_settings(where, settings, durability):
    """tick-store's settings, read back from the store and its tools, each one present."""
    found = {item.get("name"): item.get("in_force") for item in settings}
    problems = [f"{where}.settings: tick-store's {name!r} was not read back"
                for name in TICKSTORE_SETTINGS if name not in found]
    if "store version" in found and found["store version"] != 3:
        problems.append(f"{where}.settings: store version {found['store version']!r}, not 3")
    if "durability mode" in found and found["durability mode"] != TICKSTORE_MODE[durability]:
        problems.append(f"{where}.settings: durability mode {found['durability mode']!r} in "
                        f"class {durability}")
    flushes = found.get("flushes made")
    if "flushes made" in found and (flushes != [] if durability == "A" else len(flushes or []) != 2):
        problems.append(f"{where}.settings: flushes made {flushes!r} in class {durability}")
    if "kernel level" in found and found.get("kernel sets that ran") != [found["kernel level"]]:
        problems.append(f"{where}.settings: kernel sets that ran "
                        f"{found.get('kernel sets that ran')!r}, not {found['kernel level']!r}")
    codecs = [item for item in settings if str(item.get("name")).startswith("codec ids, ")]
    if len(codecs) != TICKSTORE_COLUMNS:
        problems.append(f"{where}.settings: codec ids read back for {len(codecs)} columns, "
                        f"not {TICKSTORE_COLUMNS}")
    problems += [f"{where}.settings: {item.get('name')} is not the codec docs/store.md lists"
                 for item in codecs if item.get("in_force") is not True]
    return problems


def check_read_back(where, read_back, pin):
    oracle = (read_back or {}).get("oracle")
    if not isinstance(oracle, dict):
        return [f"{where}.read_back: missing read-back proof"]
    problems = [f"{where}.read_back: no '{field}'" for field in ORACLE_FIELDS if field not in oracle]
    if oracle.get("verdict") != "pass" or oracle.get("exit_code") != 0:
        problems.append(f"{where}.read_back: the oracle said {oracle.get('verdict')}")
    if oracle.get("mismatched_records") != 0 or oracle.get("hashes_match") is not True:
        problems.append(f"{where}.read_back: the stored form differs from the source")
    if oracle.get("records") != pin["message_count"]:
        problems.append(f"{where}.read_back: {oracle.get('records')} records")
    for field in ("source_sha256", "stored_sha256"):
        if oracle.get(field) != pin["sha256"]:
            problems.append(f"{where}.read_back: {field} is not the pin")
    if not (read_back.get("stored_settings") or {}).get("checks_passed"):
        problems.append(f"{where}.read_back: no stored settings were checked")
    if pin.get("full_day") and (read_back.get("streamed") or {}).get("rows") != pin[
            "message_count"]:
        problems.append(f"{where}.read_back: the whole day was not read back")
    return problems


def check_tickstore_read_back(where, read_back):
    """G-1 runs on tick-store's store beside the column oracle, and must pass."""
    g1 = ((read_back or {}).get("stored_settings") or {}).get("g1") or {}
    if g1.get("verdict") != "pass" or g1.get("hashes_match") is not True or \
            g1.get("mismatched_records") != 0:
        return [f"{where}.read_back: G-1 on the store did not pass: {g1.get('verdict')!r}"]
    return []


def check_stored(where, stored, pin):
    problems = []
    allocated = (stored or {}).get("allocated_bytes")
    if not check_results.is_number(allocated) or allocated <= 0:
        return [f"{where}.stored: no allocated size"]
    ratio = stored.get("ratio_stored_over_source")
    if not check_results.is_number(ratio) or abs(ratio - allocated / pin["size"]) > 1e-9:
        problems.append(f"{where}.stored: the ratio is not over the slice's source bytes")
    for field in ("method", "denominator", "measured"):
        if not stored.get(field):
            problems.append(f"{where}.stored: no {field}")
    unit = "day" if pin.get("full_day") else "slice"
    if stored.get("denominator") and unit not in str(stored["denominator"]):
        problems.append(f"{where}.stored: the ratio is not stated over the {unit}'s source bytes")
    return problems


def check_write(where, write, pin):
    problems = []
    write = write or {}
    median = write.get("seconds_median")
    rate = write.get("records_per_second_median")
    if not check_results.is_number(median) or not check_results.is_number(rate) or median <= 0:
        return [f"{where}.write: no time"]
    if abs(rate - pin["message_count"] / median) > 1e-3 * rate:
        problems.append(f"{where}.write: the rate does not follow from the time")
    for field in ("python_seconds", "engine_seconds", "python_note", "peak_rss_bytes_client"):
        if field not in write:
            problems.append(f"{where}.write: no {field}")
    return problems


def reads_add_up(by_process, total):
    """A macOS write's reads by process: the worker's among them, each a count, summing to the
    write's device reads."""
    return isinstance(by_process, dict) and "worker" in by_process and \
        all(is_count(value) for value in by_process.values()) and \
        sum(by_process.values()) == total


def check_write_record(where, write, name, full_day=False, host=None):
    """Protocol 2: each write repetition's engine-only seconds, its input's residency when the
    clock started and the device's reads while it ran; what the two times hold; tick-store's
    source wholly resident, its precondition; and on the slice every system's export wholly
    resident too (review F-10). A day's export is only recorded: its write protocol is
    unchanged (review F-2)."""
    write = write if isinstance(write, dict) else {}
    each = write.get("by_repetition") if isinstance(write.get("by_repetition"), list) else []
    problems = []
    for number, item in enumerate(each, 1):
        item = item if isinstance(item, dict) else {}
        total, engine_only = item.get("seconds"), item.get("engine_only_seconds")
        if not check_results.is_number(engine_only) or not check_results.is_number(total) or \
                not 0 <= engine_only <= total * (1 + 1e-9):
            problems.append(f"{where}.write: repetition {number} has no engine-only seconds "
                            "within its total")
        elif check_results.is_number(item.get("python_seconds")) and not same(
                engine_only, total - item["python_seconds"]):
            # Review RF-2: engine-only is the repetition's own total less its Python seconds.
            problems.append(f"{where}.write: repetition {number}'s engine-only seconds "
                            f"{figure(engine_only)} are not its seconds less its python_seconds, "
                            f"{figure(total - item['python_seconds'])}")
        pages, resident = item.get("input_pages"), item.get("input_resident_pages_at_start")
        if not is_count(pages) or pages == 0 or not is_count(resident) or resident > pages:
            problems.append(f"{where}.write: repetition {number} does not say how much of its "
                            "input was resident when its clock started")
        elif name == "tickstore" and resident != pages:
            problems.append(f"{where}.write: repetition {number} started its clock with "
                            f"{resident} of its source's {pages} pages resident")
        elif not full_day and resident != pages:
            problems.append(f"{where}.write: repetition {number} started its clock with "
                            f"{resident} of its input's {pages} pages resident")
        if not is_count(item.get("device_bytes_read")) or not item.get("device_counter"):
            problems.append(f"{where}.write: repetition {number} records no device bytes read "
                            "while it ran")
        elif host == "macOS" and item["device_counter"] != MACOS_WRITE_COUNTER:
            problems.append(f"{where}.write: repetition {number} counts its device reads by "
                            f"{item['device_counter']!r}, and a macOS write counts the reads of "
                            f"the processes that did it, by ri_diskio_bytesread")
        elif host == "macOS" and not reads_add_up(item.get("device_read_by_process"),
                                                  item["device_bytes_read"]):
            # Review RV-7: each process's reads, the worker's among them, make up the count.
            problems.append(f"{where}.write: repetition {number}'s device reads by process do "
                            f"not hold the worker and add up to its {item['device_bytes_read']} "
                            "bytes")
    engine_only = [item.get("engine_only_seconds") for item in each if isinstance(item, dict)]
    if each and all(check_results.is_number(value) for value in engine_only):
        value = statistics.median(engine_only)
        if not same(write.get("engine_only_seconds_median"), value):
            problems.append(f"{where}.write: engine_only_seconds_median "
                            f"{figure(write.get('engine_only_seconds_median'))} is not the "
                            f"median of its repetitions, {figure(value)}")
    times = write.get("times") if isinstance(write.get("times"), dict) else {}
    if not times.get("seconds") or not times.get("engine_only_seconds"):
        problems.append(f"{where}.write: does not say what its total and engine-only seconds "
                        "hold")
    return problems


def check_tickstore_write(where, write):
    """tick-store's write parses ITCH and the others' do not, and the file must say so."""
    if ITCH_PARSING not in str(((write or {}).get("detail") or {}).get("input", "")):
        return [f"{where}.write: does not say that tick-store's time holds {ITCH_PARSING}"]
    return []


def check_durability(where, durability, name, class_name, control, host):
    """A class is claimed only with its proof. NOT PROVEN is a valid thing to say."""
    block = durability or {}
    problems = []
    if not block.get("sync_primitive") or not block.get("flush_granularity"):
        problems.append(f"{where}.durability: no sync primitive or flush granularity")
    if "like_for_like" not in block:
        problems.append(f"{where}.durability: no like_for_like")
    if block.get("class") != class_name:
        problems.append(f"{where}.durability: labelled class {block.get('class')!r}")
    status = block.get("status")
    if status not in (PROVEN, NOT_PROVEN):
        return [*problems, f"{where}.durability: unknown status {status!r}"]
    if not block.get("reason"):
        problems.append(f"{where}.durability: no reason")
    flush = block.get("flush") or {}
    if class_name != "B":
        if status != PROVEN:
            return problems
        if name == "tickstore":
            for scope in ("probe", "full_ingest"):
                if (flush.get(scope) or {}).get("engine_flushes") != []:
                    problems.append(f"{where}.durability: class A claimed with the writer's "
                                    f"flushes in {scope}")
        elif name == "parquet":
            # Review F-6: the harness's flush is Parquet's only one, so class A is none made.
            for scope in ("probe", "full_ingest"):
                made = (flush.get(scope) or {}).get("harness_flushes")
                if made != 0:
                    problems.append(f"{where}.durability: class A claimed with {made} harness "
                                    f"flushes in {scope}")
        else:
            # Review F-6: the others prove class A as compare/settings.py does, by dirty pages
            # still there when the probe's commit returned, whatever their class B says.
            dirty = (flush.get("probe") or {}).get("data_dirty_pages")
            if not is_count(dirty) or dirty <= 0:
                problems.append(f"{where}.durability: class A claimed with {dirty} dirty pages "
                                "in its probe")
        return problems
    if host == "macOS" and name == "questdb" and block.get("like_for_like") is not False:
        problems.append(f"{where}.durability: macOS QuestDB class B must not be like-for-like")
    if status == PROVEN:
        for scope in ("probe", "full_ingest"):
            detail = flush.get(scope) or {}
            if detail.get("data_dirty_pages") != 0:
                problems.append(f"{where}.durability: class B claimed with "
                                f"{detail.get('data_dirty_pages')} dirty pages in {scope}")
            if name == "parquet" and detail.get("harness_flushes") != 2:
                problems.append(f"{where}.durability: class B claimed without the harness flush "
                                f"in {scope}")
            calls = detail.get("engine_flushes") or []
            if name == "tickstore" and (len(calls) != 2 or
                                        any(call.get("result") != 0 for call in calls) or
                                        [call.get("of") for call in calls] !=
                                        ["file", "directory"]):
                problems.append(f"{where}.durability: class B claimed without the writer's "
                                f"flush of the file and then its directory in {scope}")
        if name not in COUNTED_FLUSHES and not control:
            problems.append(f"{where}.durability: class B claimed, and class A's control "
                            "saw no dirty page")
        table_pages = {key: value for key, value in
                       (flush.get("full_ingest") or {}).get("residual_dirty_files", {}).items()
                       if name == "questdb" and not key.endswith("_meta")}
        if table_pages and block.get("like_for_like") is not False:
            problems.append(f"{where}.durability: table pages stayed dirty, so class B is not "
                            "like-for-like")
    return problems


def check_warm_full_day(where, warm, protocol=PROTOCOL_1):
    """On a whole day: untimed runs first, then not one byte read from storage across every
    timed repetition by the processes that answer the query, each counted, with the block
    device's machine-wide count kept beside them. From protocol 2 the untimed runs stated are
    borne out by their own seconds, as on the slice (review RV-8)."""
    proof = (warm or {}).get("proof") or {}
    accepted = proof.get("accepted") or {}
    problems = []
    if proof.get("device_slack_bytes") != 0 or accepted.get("device_bytes_read") != 0:
        problems.append(f"{where}.warm: the device read {accepted.get('device_bytes_read')!r} "
                        "bytes across the timed repetitions")
    if not check_results.is_number(accepted.get("warm_up_runs")) or accepted["warm_up_runs"] < 1:
        problems.append(f"{where}.warm: no untimed run before the repetitions")
    elif protocol >= PROTOCOL_2:
        seconds = accepted.get("warm_up_seconds")
        if not isinstance(seconds, list) or len(seconds) != accepted["warm_up_runs"] or not all(
                check_results.is_number(value) and value > 0 for value in seconds):
            problems.append(f"{where}.warm: {accepted['warm_up_runs']} untimed runs stated, and "
                            f"their seconds are {seconds!r}")
    if not accepted.get("device_counter") or not proof.get("method"):
        problems.append(f"{where}.warm: the counter or the method is not named")
    by_process = accepted.get("by_process")
    if (not isinstance(by_process, dict) or "worker" not in by_process
            or any(value != 0 for value in by_process.values())
            or sum(by_process.values()) != accepted.get("device_bytes_read")):
        problems.append(f"{where}.warm: no storage reads per process, from the worker on, that "
                        f"add up to 0: {by_process!r}")
    if not check_results.is_number(accepted.get("machine_wide_device_bytes_read")):
        problems.append(f"{where}.warm: the block device's machine-wide count is not kept")
    attempts = proof.get("attempts")
    if attempts != len(proof.get("discarded") or []) + 1 or attempts > MAX_WARM_ATTEMPTS:
        problems.append(f"{where}.warm: {attempts!r} attempts, and every one must be kept")
    return problems + ([] if (warm.get("n") or 0) > 0 else [f"{where}.warm: no repetitions"])


def check_warm(where, warm, full_day=False, protocol=PROTOCOL_1):
    if full_day:
        return check_warm_full_day(where, warm, protocol)
    proof = (warm or {}).get("proof") or {}
    pages = proof.get("pages")
    if not pages or proof.get("resident_before") != pages or proof.get("resident_after") != pages:
        return [f"{where}.warm: a warm number without residency proven before and after"]
    if protocol >= PROTOCOL_2 and (not is_count(proof.get("warm_up_runs"))
                                   or proof["warm_up_runs"] < 1):
        # Review F-3: as on a day, no timed repetition is the engine's first call.
        return [f"{where}.warm: no untimed run before the repetitions"]
    if protocol >= PROTOCOL_2:
        # Review RF-10: the stated count is borne out by the untimed runs' own seconds.
        seconds = warm.get("warm_up_seconds")
        if not isinstance(seconds, list) or len(seconds) != proof["warm_up_runs"] or not all(
                check_results.is_number(value) and value > 0 for value in seconds):
            return [f"{where}.warm: {proof['warm_up_runs']} untimed runs stated, and their "
                    f"seconds are {seconds!r}"]
    return [] if (warm.get("n") or 0) > 0 else [f"{where}.warm: no repetitions"]


def check_runs(where, block, wanted, what="a reported file runs"):
    """Review F-5: the harness's own count of repetitions, which a reported file may not
    narrow: compare/repetitions.py, shared with compare/run_system.py."""
    count = (block if isinstance(block, dict) else {}).get("n")
    if count != wanted:
        return [f"{where}: {count} repetitions, and {what} {wanted}"]
    return []


def check_write_runs(where, write, full_day):
    """Review F-5: a slice writes each class three times, a full day once."""
    write = write if isinstance(write, dict) else {}
    wanted = repetitions.FULL_DAY_WRITE if full_day else repetitions.WRITE
    count = write.get("repetitions")
    seconds = write.get("seconds")
    if count != wanted or not isinstance(seconds, list) or len(seconds) != wanted:
        kind = "full-day" if full_day else "slice"
        return [f"{where}.write: {count} repetitions, and a reported {kind} file writes {wanted}"]
    return []


def check_cold(where, cold):
    cold = cold or {}
    proofs = cold.get("proofs")
    if not proofs or len(proofs) != cold.get("n"):
        return [f"{where}.cold: a cold number without a proof for each repetition"]
    problems = []
    for index, proof in enumerate(proofs):
        if proof.get("resident_after_eviction") != 0:
            problems.append(f"{where}.cold[{index}]: eviction not proven")
        if not proof.get("evicted_pages") or not proof.get("method"):
            problems.append(f"{where}.cold[{index}]: no eviction detail")
        read = proof.get("device_bytes_read")
        wanted = proof.get("resident_bytes_after_run")
        if read is None or wanted is None or read + proof.get("device_slack", 0) < wanted:
            problems.append(f"{where}.cold[{index}]: the device read {read} bytes, and "
                            f"{wanted} became resident")
    return problems


def is_count(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def check_swap_counts(where, block):
    """Pages swapped in and out over one phase, each a whole number."""
    if not isinstance(block, dict) or not all(is_count(block.get(field)) for field in SWAP_FIELDS):
        return [f"{where}: no swap counts, pages in and out, for the phase: {block!r}"]
    return []


def check_swap_timed(where, query):
    """Each query's swap over its timed runs, flagged whenever any page came back."""
    problems = []
    for kind in ("warm", "cold"):
        here = f"{where}.{kind}.swap_timed"
        block = (query.get(kind) or {}).get("swap_timed")
        found = check_swap_counts(here, block)
        if found:
            problems += found
            continue
        if block.get("flagged") is not (block["pages_in"] > 0):
            problems.append(f"{here}: flagged is {block.get('flagged')!r} with "
                            f"{block['pages_in']} pages swapped in")
        if kind == "cold":
            each = block.get("pages_in_by_repetition")
            if not isinstance(each, list) or len(each) != (query.get("cold") or {}).get("n") or \
                    not all(is_count(item) for item in each) or sum(each) != block["pages_in"]:
                problems.append(f"{here}: no pages swapped in for each cold run that add up")
    return problems


def check_swap(name, system):
    """Swap per phase for every variant and class, and the system's totals over them."""
    problems = []
    totals = {phase: [0, 0] for phase in (*CLASS_PHASES, *QUERY_PHASES)}
    for variant_name, variant in (system.get("variants") or {}).items():
        blocks = [(f"{name}.{variant_name}.swap.{phase}", phase,
                   (variant.get("swap") or {}).get(phase)) for phase in QUERY_PHASES]
        for class_name, entry in (variant.get("classes") or {}).items():
            blocks += [(f"{name}.{variant_name}.{class_name}.swap.{phase}", phase,
                        ((entry or {}).get("swap") or {}).get(phase)) for phase in CLASS_PHASES]
        for where, phase, block in blocks:
            found = check_swap_counts(where, block)
            problems += found
            if not found:
                totals[phase][0] += block["pages_in"]
                totals[phase][1] += block["pages_out"]
        for query_name, query in (variant.get("queries") or {}).items():
            if isinstance(query, dict):
                problems += check_swap_timed(f"{name}.{variant_name}.queries.{query_name}", query)
    by_phase = system.get("swap_by_phase") or {}
    for phase, (pages_in, pages_out) in totals.items():
        if by_phase.get(phase) != {"pages_in": pages_in, "pages_out": pages_out}:
            problems.append(f"{name}.swap_by_phase.{phase}: {by_phase.get(phase)!r}, and its "
                            f"variants add up to {pages_in} in and {pages_out} out")
    return problems


def check_swap_method(document):
    swap = (document.get("method") or {}).get("swap") or {}
    problems = []
    if not swap.get("source") or not is_count(swap.get("page_bytes")) or not swap["page_bytes"]:
        problems.append("method.swap: the swap counters' source and page size are not stated")
    if "flagged" not in str(swap.get("timed_policy", "")):
        problems.append("method.swap: what a timed query with swap-in becomes is not stated")
    return problems


def check_engine_time(where, query):
    """tick-store's worker reports its engine's time beside each round trip, warm and cold."""
    problems = []
    for kind in ("warm", "cold"):
        block = query.get(kind) or {}
        engine = block.get("engine") or {}
        if engine.get("n") != block.get("n") or not check_results.is_number(
                engine.get("median_us")):
            problems.append(f"{where}.{kind}: no engine time beside the round trip")
        elif engine["median_us"] > block.get("median_us", 0):
            problems.append(f"{where}.{kind}: the engine took longer than its round trip")
    return problems


def check_engine_repetitions(where, query):
    """Protocol 2: each repetition's engine time within its own round trip, and a cold one
    within its query's, since a cold figure holds tick-store's open too (review RF-7)."""
    problems = []
    for kind, holder, called in (("warm", None, "its round trip"),
                                 ("cold", "parts", "its query's round trip")):
        block = query.get(kind) if isinstance(query.get(kind), dict) else {}
        engine = (block.get("engine") or {}).get("seconds_by_repetition")
        source = block.get(holder) if holder else block
        trips = (source if isinstance(source, dict) else {}).get(
            "query_seconds_by_repetition" if holder else "seconds_by_repetition")
        if not isinstance(engine, list) or not isinstance(trips, list) or \
                len(engine) != len(trips):
            problems.append(f"{where}.{kind}: no engine time beside each of {called}s")
            continue
        for number, (inside, trip) in enumerate(zip(engine, trips), 1):
            if check_results.is_number(inside) and check_results.is_number(trip) and \
                    inside > trip:
                problems.append(f"{where}.{kind}: repetition {number}'s engine time "
                                f"{figure(inside * 1e6)} us is longer than {called}, "
                                f"{figure(trip * 1e6)} us")
    return problems


def nearest_rank(count):
    """The nearest rank of the 99th percentile among `count` runs, from 1."""
    return max(1, -(-99 * count // 100))


def check_tail(where, block):
    """Protocol 2: the tail figure, with n and its rank, called the slowest run when it is."""
    block = block if isinstance(block, dict) else {}
    count = block.get("n")
    if not is_count(count) or count == 0:
        return [f"{where}: no n for the tail figure"]
    rank = nearest_rank(count)
    slowest = rank == count
    problems = []
    if block.get("tail_rank") != rank:
        problems.append(f"{where}: the tail figure's rank is {block.get('tail_rank')!r}, and the "
                        f"nearest rank of the 99th percentile of {count} runs is {rank}")
    if block.get("tail_is_slowest") is not slowest:
        problems.append(f"{where}: tail_is_slowest is {block.get('tail_is_slowest')!r}, and rank "
                        f"{rank} of {count} is {'' if slowest else 'not '}the slowest run")
    wanted = (f"slowest of {count}" if slowest
              else f"99th percentile, nearest rank: run {rank} of {count}")
    if block.get("tail") != wanted:
        problems.append(f"{where}: the tail figure is called {block.get('tail')!r}, not "
                        f"{wanted!r}")
    tail = block.get("tail_us")
    if not check_results.is_number(tail) or tail != block.get("p99_us"):
        problems.append(f"{where}: tail_us {tail!r} is not the figure p99_us holds")
    elif slowest and tail != block.get("max_us"):
        problems.append(f"{where}: the slowest of {count} is not the largest run")
    return problems


def figure(value):
    """A figure as a message gives it."""
    return round(value, 6) if check_results.is_number(value) else value


def same(found, wanted):
    return check_results.is_number(found) and math.isclose(found, wanted, rel_tol=1e-9,
                                                           abs_tol=1e-9)


def check_repetitions(where, block):
    """Protocol 2: a latency block's figures, recomputed from the seconds of its repetitions."""
    block = block if isinstance(block, dict) else {}
    seconds = block.get("seconds_by_repetition")
    count = block.get("n")
    if not isinstance(seconds, list) or len(seconds) != count or not all(
            check_results.is_number(value) and value >= 0 for value in seconds):
        return [f"{where}: no seconds for each of its {count!r} repetitions"]
    if count == 0:
        return []
    ordered = sorted(seconds)
    wanted = {"median_us": statistics.median(ordered) * 1e6,
              "tail_us": ordered[nearest_rank(count) - 1] * 1e6,
              "min_us": ordered[0] * 1e6, "max_us": ordered[-1] * 1e6,
              "mean_us": statistics.fmean(ordered) * 1e6}
    names = {"median_us": "the median", "tail_us": f"the {block.get('tail')}",
             "min_us": "the fastest", "max_us": "the slowest", "mean_us": "the mean"}
    return [f"{where}: {field} {figure(block.get(field))} is not {names[field]} of its "
            f"repetitions, {figure(value)}"
            for field, value in wanted.items() if not same(block.get(field), value)]


def check_cold_open(where, cold, name):
    """Protocol 2: the store's resident bytes when each cold clock started, 0 where the clock
    holds the system's whole open; and tick-store's figure its open plus its query."""
    cold = cold if isinstance(cold, dict) else {}
    inside = OPEN_INSIDE_COLD_CLOCK.get(name)
    problems = []
    for index, proof in enumerate(cold.get("proofs") or []):
        proof = proof if isinstance(proof, dict) else {}
        here = f"{where}.cold[{index}]"
        if proof.get("open_inside_clock") is not inside:
            problems.append(f"{here}: open_inside_clock is {proof.get('open_inside_clock')!r}, "
                            f"and {name}'s whole open is {'' if inside else 'not '}inside its "
                            "cold clock")
        before = proof.get("resident_bytes_before_clock")
        if not is_count(before):
            problems.append(f"{here}: no store bytes resident before the clock")
        elif inside and before != 0:
            problems.append(f"{here}: {before} bytes of the store resident before a clock that "
                            "holds its whole open")
    if name == "tickstore":
        parts = cold.get("parts") if isinstance(cold.get("parts"), dict) else {}
        opens = parts.get("open_seconds_by_repetition")
        queries = parts.get("query_seconds_by_repetition")
        totals = cold.get("seconds_by_repetition")
        if not all(isinstance(item, list) and len(item) == cold.get("n")
                   for item in (opens, queries, totals)) or not all(
                check_results.is_number(value) and value > 0 for value in opens + queries):
            return [*problems, f"{where}.cold: its open and its query are not kept for each "
                               "repetition"]
        for number, (total, opened, queried) in enumerate(zip(totals, opens, queries), 1):
            if not same(total, opened + queried):
                problems.append(f"{where}.cold: repetition {number} is not its open and its "
                                "query")
    return problems


def check_tails(where, query):
    """Protocol 2: every latency block of a query, its engine's included."""
    problems = []
    for kind in ("warm", "cold"):
        block = query.get(kind) or {}
        problems += check_tail(f"{where}.{kind}", block)
        problems += check_repetitions(f"{where}.{kind}", block)
        if "engine" in block:
            problems += check_tail(f"{where}.{kind}.engine", block["engine"])
            problems += check_repetitions(f"{where}.{kind}.engine", block["engine"])
    return problems


def check_write_repetitions(where, write):
    """Protocol 2: each write repetition's times kept, and every median recomputed from them."""
    write = write if isinstance(write, dict) else {}
    count = write.get("repetitions")
    each = write.get("by_repetition")
    seconds = write.get("seconds")
    if not is_count(count) or count == 0 or not isinstance(each, list) or len(each) != count:
        return [f"{where}.write: by_repetition holds "
                f"{len(each) if isinstance(each, list) else 'no'} repetitions, and the write ran "
                f"{count!r}"]
    fields = ("seconds", "engine_seconds", "python_seconds")
    if not all(isinstance(item, dict) and all(check_results.is_number(item.get(field))
                                              for field in fields) for item in each):
        return [f"{where}.write: a repetition without its {', '.join(fields)}"]
    if seconds != [item["seconds"] for item in each]:
        return [f"{where}.write: seconds {seconds!r} are not the repetitions' own"]
    problems = []
    for field, median in (("seconds_median", "seconds"), ("engine_seconds", "engine_seconds"),
                          ("python_seconds", "python_seconds")):
        value = statistics.median(item[median] for item in each)
        if not same(write.get(field), value):
            problems.append(f"{where}.write: {field} {figure(write.get(field))} is not the "
                            f"median of its repetitions, {figure(value)}")
    return problems


def check_latency(where, query):
    """The median and p99 RESULTS.md prints, each a number, the p99 no lower than the median."""
    problems = []
    for kind in ("warm", "cold"):
        block = query.get(kind) or {}
        median, p99 = block.get("median_us"), block.get("p99_us")
        if not (check_results.is_number(median) and check_results.is_number(p99)):
            problems.append(f"{where}.{kind}: no median and p99")
        elif p99 < median:
            problems.append(f"{where}.{kind}: the p99 is below the median")
    return problems


def check_queries(where, queries, truth, system_name=None, full_day=False, protocol=PROTOCOL_1):
    problems = []
    for name in QUERY_NAMES:
        query = (queries or {}).get(name)
        if not isinstance(query, dict):
            problems.append(f"{where}.queries: {name} missing")
            continue
        here = f"{where}.queries.{name}"
        if query.get("digest") != truth.get(name) or query.get("matches_ground_truth") is not True:
            problems.append(f"{here}: differs from the ground truth")
        problems += check_warm(here, query.get("warm"), full_day, protocol)
        problems += check_cold(here, query.get("cold"))
        problems += check_runs(f"{here}.warm", query.get("warm"), repetitions.WARM)
        problems += check_runs(f"{here}.cold", query.get("cold"), repetitions.COLD)
        problems += check_latency(here, query)
        if protocol >= PROTOCOL_2:
            problems += check_tails(here, query)
            problems += check_cold_open(here, query.get("cold"), system_name)
        if not query.get("scan"):
            problems.append(f"{here}: no rows or bytes decoded, or a statement of why not")
        if system_name == "tickstore":
            problems += check_engine_time(here, query)
            if protocol >= PROTOCOL_2:
                problems += check_engine_repetitions(here, query)
    return problems


def check_system(name, system, document, pin):
    host = document["header"].get("host")
    protocol = protocol_of(document)
    truth = (document.get("queries") or {}).get("truth") or {}
    problems = []
    if not system.get("versions"):
        problems.append(f"{name}: no versions")
    if not system.get("variants"):
        problems.append(f"{name}: no variants")
    if system.get("headline_variant") not in (system.get("variants") or {}):
        problems.append(f"{name}: the headline variant did not run")
    if "mem_available_bytes" not in system or "children_peak_rss_bytes" not in system:
        problems.append(f"{name}: no memory record")
    if host == "Linux":
        if not isinstance(system.get("mem_available_bytes"), dict):
            problems.append(f"{name}: Linux must record MemAvailable")
        if (system.get("pinning") or {}).get("cpus") != LINUX_CPUS:
            problems.append(f"{name}: not pinned to cpu4 and cpu6")
    for variant_name, variant in (system.get("variants") or {}).items():
        where = f"{name}.{variant_name}"
        if not variant.get("tuning"):
            problems.append(f"{where}: no tuning")
        classes = variant.get("classes") or {}
        probe = ((classes.get("A") or {}).get("durability") or {}).get("flush", {}).get("probe", {})
        control = (probe.get("data_dirty_pages") or 0) > 0
        for class_name in CLASSES:
            entry = classes.get(class_name)
            here = f"{where}.{class_name}"
            if not isinstance(entry, dict):
                problems.append(f"{here}: missing")
                continue
            problems += check_settings(here, entry.get("settings"), name, class_name)
            problems += check_read_back(here, entry.get("read_back"), pin)
            problems += check_stored(here, entry.get("stored"), pin)
            problems += check_write(here, entry.get("write"), pin)
            problems += check_write_runs(here, entry.get("write"), bool(pin.get("full_day")))
            if protocol >= PROTOCOL_2:
                problems += check_write_repetitions(here, entry.get("write"))
                problems += check_write_record(here, entry.get("write"), name,
                                              bool(pin.get("full_day")), host)
            if name == "tickstore":
                problems += check_tickstore_read_back(here, entry.get("read_back"))
                problems += check_tickstore_write(here, entry.get("write"))
            problems += check_durability(here, entry.get("durability"), name, class_name, control,
                                         host)
        problems += check_queries(where, variant.get("queries"), truth, name,
                                  per_process_warm(pin, host), protocol)
        if protocol >= PROTOCOL_2:
            opened = (variant.get("warm_worker_open") or {}).get("seconds")
            if not check_results.is_number(opened) or opened < 0:
                problems.append(f"{where}.warm_worker_open: the warm worker's open is not timed")
            problems += check_tail(f"{where}.python_floor", variant.get("python_floor"))
            problems += check_repetitions(f"{where}.python_floor", variant.get("python_floor"))
    problems += check_swap(name, system)
    if pin.get("full_day") and host == "Linux":
        problems += check_peak_memory(name, system.get("peak_memory"))
    if pin.get("alone") and host != "Linux":
        problems += check_time_memory(name, system.get("peak_memory"))
    return problems


def per_process_warm(pin, host):
    """A day's warm figure is proven by each process's storage reads, which Linux reads from
    /proc. tick-store alone off Linux is proven warm by residency instead: its day store fits
    the host's memory. A Linux day, tick-store alone or not, has the per-process proof."""
    return bool(pin.get("full_day")) and not (pin.get("alone") and host != "Linux")


def check_time_memory(name, memory):
    """A tick-store-only day's peak memory, from /usr/bin/time -l around its run."""
    memory = memory or {}
    problems = [f"{name}.peak_memory: no {field} from {TIME_SOURCE}"
                for field in ("maximum_resident_set_size_bytes", "peak_memory_footprint_bytes",
                              "real_seconds")
                if not check_results.is_number(memory.get(field)) or memory[field] <= 0]
    if TIME_SOURCE not in str(memory.get("source", "")):
        problems.append(f"{name}.peak_memory: does not say it comes from {TIME_SOURCE}")
    return problems


def check_peak_memory(name, memory):
    """Each system's peak memory on a whole day, from the guard that watched it."""
    memory = memory or {}
    problems = []
    for field in ("vmhwm_bytes_largest", "mem_available_lowest_bytes", "session_rss_bytes_peak"):
        if not check_results.is_number(memory.get(field)) or memory[field] <= 0:
            problems.append(f"{name}.peak_memory: no {field}")
    for field in ("swap_in_pages_during_run", "swap_out_pages_during_run"):
        if not is_count(memory.get(field)):
            problems.append(f"{name}.peak_memory: the guard's {field} is not recorded")
    if memory.get("guard_fired") is not False or (memory.get("exit") or {}).get("code") != 0:
        problems.append(f"{name}.peak_memory: the guard stopped the run, or it did not exit 0")
    return problems


def check_full_day_queries(document, pin):
    """Nothing clipped: every window as BENCHMARKS.md defines it, one busiest locate over the
    whole hour of the day, and Q5 over every record."""
    queries = document.get("queries") or {}
    facts = queries.get("full_day") or {}
    hour = [OPEN_NS, OPEN_NS + HOUR_NS]
    problems = []
    if facts.get("clipped") is not False or facts.get("hour_window_ns") != hour or \
            facts.get("busiest_locate_window_ns") != hour:
        problems.append("queries.full_day: the hour or the busiest locate's window is clipped")
    if facts.get("q5_rows_scanned") != pin["message_count"]:
        problems.append(f"queries.full_day: Q5 scans {facts.get('q5_rows_scanned')!r} rows, not "
                        "the day's")
    found = {item.get("name"): item for item in queries.get("parameters") or []
             if isinstance(item, dict)}
    for name, (kind, start, end, one_locate) in FULL_DAY_QUERIES.items():
        item = found.get(name) or {}
        locate = item.get("locate")
        wanted_locate = facts.get("busiest_locate") if one_locate else -1
        if (item.get("kind"), item.get("start_ns"), item.get("end_ns")) != (kind, start, end) \
                or locate != wanted_locate or (one_locate and not isinstance(locate, int)):
            problems.append(f"queries.parameters: {name} is not the full-day query: {item}")
        if "clipped" in str(item.get("text", "")):
            problems.append(f"queries.parameters: {name} says it is clipped")
    return problems


def check_tickstore_only(document, path):
    """tick-store alone on a day: the host, the systems, the variants, the label, the heap."""
    header = document["header"]
    systems = document.get("systems") or {}
    problems = []
    if header.get("host") not in TICKSTORE_ONLY_HOSTS:
        problems.append(f"scope: a tick-store-only full day runs on "
                        f"{' or '.join(TICKSTORE_ONLY_HOSTS)}, and this is "
                        f"{header.get('host')!r}")
    if list(systems) != ["tickstore"] or \
            (document.get("input") or {}).get("systems_run") != ["tickstore"]:
        problems.append(f"systems: a tick-store-only full day holds tick-store alone, and this "
                        f"holds {list(systems)}")
    if "tick-store alone" not in str(document.get("label", "")):
        problems.append("label: a tick-store-only full day must say it is tick-store alone")
    heap = document.get("heap") or {}
    if heap.get("setting") != "not-run" or "did not run" not in str(heap.get("statement", "")):
        problems.append("heap: a tick-store-only full day must state that QuestDB did not run")
    if header.get("questdb_runtime") != "not-run":
        problems.append("header.questdb_runtime: QuestDB did not run, so it must say not-run")
    selection = document.get("variant_selection")
    if not isinstance(selection, dict) or selection.get("rule") != variant_rule.TICKSTORE_ONLY_RULE:
        return [*problems, "variant_selection: a tick-store-only full day must state "
                           "compare/variant_rule.py's tick-store-only rule"]
    stated = ((selection.get("systems") or {}).get("tickstore") or {}).get("selected")
    ran = list(((systems.get("tickstore") or {}).get("variants")) or {})
    wanted = list(variant_rule.TICKSTORE_VARIANTS)
    if stated != wanted or ran != wanted:
        problems.append(f"variant_selection: tick-store states {stated} and ran {ran}, and "
                        f"alone it runs every variant, the headline first: {wanted}")
    if (systems.get("tickstore") or {}).get("headline_variant") != wanted[0]:
        problems.append("variant_selection: tick-store's headline is not the one it runs first")
    return problems


def protocol_of(document):
    """The protocol a file states, or 1 for a file measured before there was one."""
    return (document.get("header") or {}).get("protocol", PROTOCOL_1)


def check_protocol(header, sha256, pins):
    """A file states protocol 2, exactly the integer, unless it is one of the committed files
    measured before there was a protocol, which state none (review RF-1). Those are known by
    their bytes, `sha256`, pinned in the pins file, never by a field of the header (RV-6)."""
    if "protocol" not in header:
        if sha256 is not None and sha256 in (pins.get(PROTOCOL_1_FILES) or {}).values():
            return []
        return ["header.protocol: none stated, and only the committed files measured before "
                f"protocol 2, each pinned by its SHA-256 under {PROTOCOL_1_FILES} in "
                "scripts/data_pins.json, may state none"]
    stated = header["protocol"]
    if type(stated) is not int or stated != PROTOCOL_2:
        return [f"header.protocol: {stated!r}, and a file states protocol {PROTOCOL_2}, or none "
                f"if it is one of the committed files measured before it"]
    return []


def file_sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_second_sibling(header, cases, protocol):
    """Protocol 2: the second pinned CPU's sibling, under the first one's rule on Linux, and
    stated as not available on macOS, which pins nothing."""
    if protocol < PROTOCOL_2:
        return []
    block = header.get(SECOND_SIBLING)
    if header.get("host") == "Linux":
        return check_results.check_sibling({SECOND_SIBLING: block}, cases, SECOND_SIBLING,
                                           SECOND_CPU)
    if block != check_results.NOT_AVAILABLE:
        return [f"header.{SECOND_SIBLING}: macOS has no pinned core to measure"]
    return []


def check_turbo(header, protocol):
    """Protocol 2: on Linux, cpufreq boost read before the first system's part and after the
    last, the two the same, and the state stated theirs, as run_bench.py requires of a
    benchmark (review E-10). macOS has no boost switch to read."""
    if protocol < PROTOCOL_2:
        return []
    readings = header.get(TURBO_READINGS)
    if header.get("host") != "Linux":
        if readings != check_results.NOT_AVAILABLE:
            return [f"header.{TURBO_READINGS}: macOS has no boost switch to read"]
        return []
    if not isinstance(readings, dict):
        return [f"header.{TURBO_READINGS}: cpufreq boost was not read before the first system "
                "and after the last"]
    before, after = readings.get("before_first_system"), readings.get("after_last_system")
    problems = [f"header.{TURBO_READINGS}: {name} is {value!r}, not a reading of cpufreq boost"
                for name, value in (("before_first_system", before), ("after_last_system", after))
                if value not in TURBO_STATES]
    if problems:
        return problems
    if before != after:
        return [f"header.{TURBO_READINGS}: cpufreq boost read {before!r} before the first system "
                f"and {after!r} after the last"]
    if header.get("turbo") != after:
        return [f"header.turbo: states {header.get('turbo')!r}, and both readings are {after!r}"]
    return []


def check_variant_selection(document, path, pins, capacity):
    """The variants each system ran are what the rule gives from the slice file named, which
    must sit beside this one: same host, same commit, and itself a reported file that passes
    every rule here (review F-15)."""
    selection = document.get("variant_selection")
    if not isinstance(selection, dict):
        return ["variant_selection: a full-day file must state how its variants were chosen"]
    problems = []
    rule = variant_rule.RULE if protocol_of(document) >= PROTOCOL_2 else \
        variant_rule.RULE_PROTOCOL_1
    if selection.get("rule") != rule:
        problems.append("variant_selection: the rule is not compare/variant_rule.py's")
    header = document["header"]
    source = path.parent / str(selection.get("source"))
    try:
        slice_document = json.loads(source.read_text(encoding="utf-8"))
        slice_sha256 = file_sha256(source)
    except (OSError, json.JSONDecodeError):
        return [*problems, f"variant_selection: the slice file {selection.get('source')!r} is "
                           "not beside this one"]
    slice_header = slice_document.get("header") or {}
    if slice_header.get("commit") != header.get("commit") or \
            slice_header.get("host") != header.get("host") or \
            "slice" not in (slice_document.get("input") or {}):
        problems.append("variant_selection: the slice file is not this host's at this commit")
    if isinstance(slice_document, dict) and "slice" in (slice_document.get("input") or {}):
        # Only a slice is validated as the source: a full day named there is refused above, and
        # validating it would name its own source again, without end (review RV-9).
        problems += [f"variant_selection: the slice file {source.name}: {problem}"
                     for problem in check_document(slice_document, list(slice_document), source,
                                                   pins, capacity, sha256=slice_sha256)]
    try:
        chosen = variant_rule.select(slice_document)
    except (KeyError, TypeError) as error:
        return [*problems, f"variant_selection: the slice file cannot be ranked: {error}"]
    stated = selection.get("systems") or {}
    for name, system in (document.get("systems") or {}).items():
        expected = chosen.get(name)
        if expected is None or stated.get(name) != expected:
            problems.append(f"variant_selection: {name} states {stated.get(name)}, and the rule "
                            f"gives {expected}")
            continue
        if list(system.get("variants") or {}) != expected["selected"]:
            problems.append(f"variant_selection: {name} ran {list(system.get('variants') or {})}, "
                            f"and the rule gives {expected['selected']}")
    return problems


def checked_heap(capacity):
    """The heap the capacity file records as run at and adequate, or None if it records none."""
    verdict = capacity.get("questdb_heap_verdict") or {}
    if verdict.get("verdict") == "adequate" and verdict.get("tested_at_that_value") is True:
        return verdict.get("chosen_heap")
    return None


def check_heap_in_force(setting, questdb):
    """Every class's running command must hold the heap the file states."""
    problems = []
    for variant_name, variant in (questdb.get("variants") or {}).items():
        for class_name, entry in (variant.get("classes") or {}).items():
            found = {item.get("name"): item.get("in_force") for item in entry.get("settings") or []}
            ran = " ".join(str(found.get(name)) for name in HEAP_SETTINGS)
            if ran != setting:
                problems.append(f"heap: states {setting!r}, and questdb.{variant_name}."
                                f"{class_name} ran with {ran!r}")
    return problems


def check_heap(document, host, capacity):
    """Each host states its heap. A Linux heap must also be the one checked for a full day."""
    heap = document.get("heap")
    if not isinstance(heap, dict) or not heap.get("setting") or not heap.get("statement"):
        return [f"heap: {host} must state QuestDB's heap"]
    setting = heap["setting"]
    problems = check_heap_in_force(setting, (document.get("systems") or {}).get("questdb") or {})
    if setting not in str(heap["statement"]):
        problems.append(f"heap: the statement does not say {setting!r}")
    if host == "macOS":
        if heap.get("full_day_check") != LINUX_ONLY or LINUX_ONLY not in str(heap["statement"]):
            problems.append("heap: macOS must state that the full-day capacity check is "
                            f"{LINUX_ONLY}")
        return problems
    checked = checked_heap(capacity)
    if checked is None:
        problems.append(f"heap: {DEFAULT_CAPACITY.name} records no heap as checked and adequate")
    elif setting != checked:
        problems.append(f"heap: {setting!r} is not {checked!r}, the heap "
                        f"{DEFAULT_CAPACITY.name} records as checked")
    if heap.get("full_day_check") != CAPACITY_CITATION:
        problems.append(f"heap: Linux must cite {CAPACITY_CITATION}")
    return problems


def check_runtime(header):
    """QuestDB's JVM and allocator, read from the process and never assumed."""
    if "questdb_runtime" not in header:
        return []  # Named among the missing header fields.
    runtime = header["questdb_runtime"]
    if not isinstance(runtime, dict):
        return ["header.questdb_runtime: not a reading from the running server"]
    problems = [f"header.questdb_runtime: missing field '{field}'" for field in RUNTIME_FIELDS
                if not runtime.get(field)]
    if not isinstance(runtime.get("jemalloc_preloaded"), bool):
        problems.append("header.questdb_runtime: jemalloc_preloaded must be true or false")
    return problems


def check_document(document, top_keys, path, pins, capacity, real_path=None, sha256=None):
    header = document.get("header")
    if not isinstance(header, dict):
        return ["the file does not start with its header"]
    problems = []
    if not top_keys or top_keys[0] != "header":
        problems.append("the file does not start with its header")
    problems += [f"header: missing field '{field}'" for field in HEADER_FIELDS
                 if field not in header]
    stated = check_protocol(header, sha256, pins)
    if stated:
        return [*problems, *stated]
    systems = document.get("systems") or {}
    alone = document.get("scope") == variant_rule.TICKSTORE_ONLY
    named_alone = f"-{TICKSTORE_ONLY_NAME}-" in path.name
    if "scope" in document and not alone:
        problems.append(f"scope: {document['scope']!r} is not a scope this validator knows")
    if alone != named_alone:
        problems.append(f"scope: a tick-store-only full day must be named <host>-<day>-"
                        f"{TICKSTORE_ONLY_NAME}-<commit>.json, and only it may be")
    if not alone:
        problems += check_runtime(header)
    problems += check_results.check_load_lines(header, len(systems))
    problems += check_results.check_build(header)
    problems += check_results.check_host({"header": header}, path)
    if header.get("host") == "macOS":
        problems += check_results.check_macos({"header": header})
        if not alone:
            problems += check_heap(document, "macOS", capacity)
    elif header.get("host") == "Linux":
        problems += check_results.check_linux({key: value for key, value in header.items()
                                               if key != "frequency"}, list(systems))
        if not alone:
            problems += check_heap(document, "Linux", capacity)
    problems += check_second_sibling(header, list(systems), protocol_of(document))
    problems += check_turbo(header, protocol_of(document))
    label = str(document.get("label", ""))
    input_block = document.get("input") or {}
    full_day = "day" in input_block or f"-{FULL_DAY}-" in path.name
    if full_day:
        day = input_block.get("day")
        stem = TICKSTORE_ONLY_NAME if alone else FULL_DAY
        if not path.name.startswith(f"{header.get('host')}-{day}-{stem}-") or \
                "slice" in input_block or input_block.get("full_day") is not True:
            problems.append(f"input: a full-day file must name its day as <host>-<day>-"
                            f"{FULL_DAY}-<commit>.json and hold no slice")
        if "full day" not in label or "nothing clipped" not in label:
            problems.append("label: a full-day file must say it is a full day, nothing clipped")
        day_pin = pins.get("days", {}).get(day)
        if day_pin is None:
            return [*problems, f"input.day: no pin for {day}"]
        pin = {"sha256": day_pin["decompressed_sha256"], "size": day_pin["decompressed_size"],
               "message_count": day_pin["message_count"], "full_day": True, "alone": alone}
        if "Nothing is clipped" not in str(input_block.get("clipping", "")):
            problems.append("input: a full-day file must state that nothing is clipped")
        problems += check_full_day_queries(document, pin)
        if alone:
            problems += check_tickstore_only(document, path)
        else:
            problems += check_variant_selection(document, real_path or path, pins, capacity)
    else:
        if alone:
            problems.append("scope: only a full day can be tick-store alone")
        if "not claims about a full day" not in label:
            problems.append("label: the file must say it makes no claim about a full day")
        pin = pins.get("slices", {}).get(input_block.get("slice"))
        if pin is None:
            return [*problems, f"input.slice: no pin for {input_block.get('slice')}"]
        if "clip" not in str(input_block.get("clipping", "")):
            problems.append("input: the clipping is not stated")
    if input_block.get("sha256") != pin["sha256"] or input_block.get("messages") != pin[
            "message_count"] or input_block.get("bytes") != pin["size"]:
        problems.append("input: does not match the pin")
    if not input_block.get("ratio_baseline"):
        problems.append("input: the ratio baseline is not stated")
    if "tickstore" in systems and ITCH_PARSING not in str(input_block.get("ingest_input", "")):
        problems.append(f"input: does not say that tick-store's ingest holds {ITCH_PARSING} and "
                        "the others' does not")
    method = document.get("method") or {}
    if not method.get("warm") or not method.get("cold"):
        problems.append("method: how warm and cold were achieved is not stated")
    schema = header.get("schema")
    if not isinstance(schema, int) or schema < SWAP_SCHEMA:
        problems.append(f"header.schema: {schema!r}, and a file must count swap per phase, "
                        f"schema {SWAP_SCHEMA} or later")
    problems += check_swap_method(document)
    for name in ([] if alone else SYSTEMS):
        if name not in systems:
            problems.append(f"systems: {name} is missing")
    for name, system in systems.items():
        problems += check_system(name, system, document, pin)
    problems += check_results.check_leaks(document)
    return problems


def check_file(path, pins, capacity, name_as=None):
    try:
        top_keys = []

        def remember_order(pairs):
            top_keys[:] = [key for key, _ in pairs]
            return dict(pairs)

        document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=remember_order)
        sha256 = file_sha256(path)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"cannot read it: {error}"]
    if not isinstance(document, dict):
        return ["the file is not a JSON object"]
    named = path.with_name(name_as) if name_as else path
    return check_document(document, top_keys, named, pins, capacity, path, sha256)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate comparison results files.")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--pins", type=Path, default=DEFAULT_PINS)
    parser.add_argument("--capacity", type=Path, default=DEFAULT_CAPACITY,
                        help="the full-day capacity check a Linux heap must match")
    parser.add_argument("--pending-as", help="the file's name once published")
    args = parser.parse_args(argv)
    try:
        pins = json.loads(args.pins.read_text(encoding="utf-8"))
        capacity = json.loads(args.capacity.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"check_compare: cannot read the pins or the capacity check: {error}",
              file=sys.stderr)
        return check_results.EXIT_USAGE
    failed = False
    for path in args.files:
        problems = check_file(path, pins, capacity, args.pending_as)
        for problem in problems:
            print(f"{path.name}: {problem}", file=sys.stderr)
        if problems:
            failed = True
        else:
            print(f"{path.name}: ok")
    return check_results.EXIT_INVALID if failed else check_results.EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
