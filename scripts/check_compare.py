#!/usr/bin/env python3
"""Validate comparison results. Exits 1 and names every problem if a file has one.

    python3 -I scripts/check_compare.py results/compare/*.json

A comparison result claims that four systems held the same records under stated settings
and gave the same answers. This checks that the file can back each claim: a requested setting
must equal the one in force, every store must have rebuilt the source through the oracle,
every answer must equal the ground truth, every warm and cold number must carry its proof, a
class B result its flush proof, the run its measurement lock, and a Linux heap the setting that
the full-day capacity check found adequate. It also checks that the file is safe to publish. A
system that could not prove something says so in the file, and the file is valid, as long as it
does not claim the proof.

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

A tick-store-only full day, `<host>-<day>-full-tickstore-<commit>.json`, is the macOS figure for
the targets whose host is Both. It is held to the full day's rules on tick-store, with three
differences, each stated in the file: tick-store is the only system and every variant it has ran,
the headline first; warm is the slice's residency proof, since the per-process proof reads
/proc; and peak memory comes from BSD time -l, since the guard reads /proc too. It runs on
macOS only, so that a Linux subset can never stand in for the four-system day, and a four-system
file can never carry its name, nor the reverse.

Standard library only. tests/scripts/test_check_compare.py shows each rule failing on an
injected fault.
"""

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_results  # noqa: E402

sys.path.insert(0, str(REPO_ROOT))
from compare import variant_rule  # noqa: E402

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
FULL_DAY = "full"
TICKSTORE_ONLY_NAME = "full-tickstore"
TICKSTORE_ONLY_HOSTS = ("macOS",)
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
        if name == "tickstore" and status == PROVEN:
            for scope in ("probe", "full_ingest"):
                if (flush.get(scope) or {}).get("engine_flushes") != []:
                    problems.append(f"{where}.durability: class A claimed with the writer's "
                                    f"flushes in {scope}")
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


def check_warm_full_day(where, warm):
    """On a whole day: untimed runs first, then not one byte read from storage across every
    timed repetition by the processes that answer the query, each counted, with the block
    device's machine-wide count kept beside them."""
    proof = (warm or {}).get("proof") or {}
    accepted = proof.get("accepted") or {}
    problems = []
    if proof.get("device_slack_bytes") != 0 or accepted.get("device_bytes_read") != 0:
        problems.append(f"{where}.warm: the device read {accepted.get('device_bytes_read')!r} "
                        "bytes across the timed repetitions")
    if not check_results.is_number(accepted.get("warm_up_runs")) or accepted["warm_up_runs"] < 1:
        problems.append(f"{where}.warm: no untimed run before the repetitions")
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


def check_warm(where, warm, full_day=False):
    if full_day:
        return check_warm_full_day(where, warm)
    proof = (warm or {}).get("proof") or {}
    pages = proof.get("pages")
    if not pages or proof.get("resident_before") != pages or proof.get("resident_after") != pages:
        return [f"{where}.warm: a warm number without residency proven before and after"]
    return [] if (warm.get("n") or 0) > 0 else [f"{where}.warm: no repetitions"]


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


def check_queries(where, queries, truth, system_name=None, full_day=False):
    problems = []
    for name in QUERY_NAMES:
        query = (queries or {}).get(name)
        if not isinstance(query, dict):
            problems.append(f"{where}.queries: {name} missing")
            continue
        here = f"{where}.queries.{name}"
        if query.get("digest") != truth.get(name) or query.get("matches_ground_truth") is not True:
            problems.append(f"{here}: differs from the ground truth")
        problems += check_warm(here, query.get("warm"), full_day)
        problems += check_cold(here, query.get("cold"))
        problems += check_latency(here, query)
        if not query.get("scan"):
            problems.append(f"{here}: no rows or bytes decoded, or a statement of why not")
        if system_name == "tickstore":
            problems += check_engine_time(here, query)
    return problems


def check_system(name, system, document, pin):
    host = document["header"].get("host")
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
            if name == "tickstore":
                problems += check_tickstore_read_back(here, entry.get("read_back"))
                problems += check_tickstore_write(here, entry.get("write"))
            problems += check_durability(here, entry.get("durability"), name, class_name, control,
                                         host)
        problems += check_queries(where, variant.get("queries"), truth, name,
                                  bool(pin.get("full_day")) and not pin.get("alone"))
    problems += check_swap(name, system)
    if pin.get("full_day") and host == "Linux":
        problems += check_peak_memory(name, system.get("peak_memory"))
    if pin.get("alone"):
        problems += check_time_memory(name, system.get("peak_memory"))
    return problems


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
                        f"{', '.join(TICKSTORE_ONLY_HOSTS)} alone, and this is "
                        f"{header.get('host')!r}: Linux runs the four-system day")
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


def check_variant_selection(document, path):
    """The variants each system ran are what the rule gives from the slice file named, which
    must sit beside this one: same host, same commit."""
    selection = document.get("variant_selection")
    if not isinstance(selection, dict):
        return ["variant_selection: a full-day file must state how its variants were chosen"]
    problems = []
    if selection.get("rule") != variant_rule.RULE:
        problems.append("variant_selection: the rule is not compare/variant_rule.py's")
    header = document["header"]
    source = path.parent / str(selection.get("source"))
    try:
        slice_document = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return [*problems, f"variant_selection: the slice file {selection.get('source')!r} is "
                           "not beside this one"]
    slice_header = slice_document.get("header") or {}
    if slice_header.get("commit") != header.get("commit") or \
            slice_header.get("host") != header.get("host") or \
            "slice" not in (slice_document.get("input") or {}):
        problems.append("variant_selection: the slice file is not this host's at this commit")
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


def check_document(document, top_keys, path, pins, capacity, real_path=None):
    header = document.get("header")
    if not isinstance(header, dict):
        return ["the file does not start with its header"]
    problems = []
    if not top_keys or top_keys[0] != "header":
        problems.append("the file does not start with its header")
    problems += [f"header: missing field '{field}'" for field in HEADER_FIELDS
                 if field not in header]
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
        problems += check_heap(document, "Linux", capacity)
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
            problems += check_variant_selection(document, real_path or path)
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
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"cannot read it: {error}"]
    if not isinstance(document, dict):
        return ["the file is not a JSON object"]
    named = path.with_name(name_as) if name_as else path
    return check_document(document, top_keys, named, pins, capacity, path)


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
