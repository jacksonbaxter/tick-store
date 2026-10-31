#!/usr/bin/env python3
"""Validate comparison results. Exits 1 and names every problem if a file has one.

    python3 -I scripts/check_compare.py results/compare/*.json

A comparison result claims that three systems held the same records under stated settings
and gave the same answers. This checks that the file can back each claim: a requested setting
must equal the one in force, every store must have rebuilt the source through the oracle,
every answer must equal the ground truth, every warm and cold number must carry its proof, a
class B result its flush proof, the run its measurement lock, and a Linux heap the setting that
the full-day capacity check found adequate. It also checks that the file is safe to publish. A
system that could not prove something says so in the file, and the file is valid, as long as it
does not claim the proof.

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

DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"
DEFAULT_CAPACITY = REPO_ROOT / "results" / "fullday-capacity-12302019.json"
CAPACITY_CITATION = f"results/{DEFAULT_CAPACITY.name}"
LINUX_ONLY = "Linux only"
HEAP_SETTINGS = ("java initial heap", "java heap")
SYSTEMS = ("parquet", "arcticdb", "questdb")
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
    return problems


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


def check_durability(where, durability, name, class_name, control, host):
    """A class is claimed only with its proof. NOT PROVEN is a valid thing to say."""
    block = durability or {}
    problems = []
    if not block.get("sync_primitive") or not block.get("flush_granularity"):
        problems.append(f"{where}.durability: no sync primitive or flush granularity")
    if "like_for_like" not in block:
        problems.append(f"{where}.durability: no like_for_like")
    status = block.get("status")
    if status not in (PROVEN, NOT_PROVEN):
        return [*problems, f"{where}.durability: unknown status {status!r}"]
    if not block.get("reason"):
        problems.append(f"{where}.durability: no reason")
    if class_name != "B":
        return problems
    if host == "macOS" and name == "questdb" and block.get("like_for_like") is not False:
        problems.append(f"{where}.durability: macOS QuestDB class B must not be like-for-like")
    flush = block.get("flush") or {}
    if status == PROVEN:
        for scope in ("probe", "full_ingest"):
            detail = flush.get(scope) or {}
            if detail.get("data_dirty_pages") != 0:
                problems.append(f"{where}.durability: class B claimed with "
                                f"{detail.get('data_dirty_pages')} dirty pages in {scope}")
            if name == "parquet" and detail.get("harness_flushes") != 2:
                problems.append(f"{where}.durability: class B claimed without the harness flush "
                                f"in {scope}")
        if name != "parquet" and not control:
            problems.append(f"{where}.durability: class B claimed, and class A's control "
                            "saw no dirty page")
        table_pages = {key: value for key, value in
                       (flush.get("full_ingest") or {}).get("residual_dirty_files", {}).items()
                       if name == "questdb" and not key.endswith("_meta")}
        if table_pages and block.get("like_for_like") is not False:
            problems.append(f"{where}.durability: table pages stayed dirty, so class B is not "
                            "like-for-like")
    return problems


def check_warm(where, warm):
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


def check_queries(where, queries, truth):
    problems = []
    for name in QUERY_NAMES:
        query = (queries or {}).get(name)
        if not isinstance(query, dict):
            problems.append(f"{where}.queries: {name} missing")
            continue
        here = f"{where}.queries.{name}"
        if query.get("digest") != truth.get(name) or query.get("matches_ground_truth") is not True:
            problems.append(f"{here}: differs from the ground truth")
        problems += check_warm(here, query.get("warm"))
        problems += check_cold(here, query.get("cold"))
        if not query.get("scan"):
            problems.append(f"{here}: no rows or bytes decoded, or a statement of why not")
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
            problems += check_durability(here, entry.get("durability"), name, class_name, control,
                                         host)
        problems += check_queries(where, variant.get("queries"), truth)
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


def check_document(document, top_keys, path, pins, capacity):
    header = document.get("header")
    if not isinstance(header, dict):
        return ["the file does not start with its header"]
    problems = []
    if not top_keys or top_keys[0] != "header":
        problems.append("the file does not start with its header")
    problems += [f"header: missing field '{field}'" for field in HEADER_FIELDS
                 if field not in header]
    problems += check_runtime(header)
    systems = document.get("systems") or {}
    problems += check_results.check_load_lines(header, len(systems))
    problems += check_results.check_build(header)
    problems += check_results.check_host({"header": header}, path)
    if header.get("host") == "macOS":
        problems += check_results.check_macos({"header": header})
        problems += check_heap(document, "macOS", capacity)
    elif header.get("host") == "Linux":
        problems += check_results.check_linux({key: value for key, value in header.items()
                                               if key != "frequency"}, list(systems))
        problems += check_heap(document, "Linux", capacity)
    label = str(document.get("label", ""))
    if "not claims about a full day" not in label:
        problems.append("label: the file must say it makes no claim about a full day")
    input_block = document.get("input") or {}
    pin = pins.get("slices", {}).get(input_block.get("slice"))
    if pin is None:
        return [*problems, f"input.slice: no pin for {input_block.get('slice')}"]
    if input_block.get("sha256") != pin["sha256"] or input_block.get("messages") != pin[
            "message_count"] or input_block.get("bytes") != pin["size"]:
        problems.append("input: does not match the pin")
    if "clip" not in str(input_block.get("clipping", "")) or not input_block.get("ratio_baseline"):
        problems.append("input: the clipping or the ratio baseline is not stated")
    method = document.get("method") or {}
    if not method.get("warm") or not method.get("cold"):
        problems.append("method: how warm and cold were achieved is not stated")
    for name in SYSTEMS:
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
    return check_document(document, top_keys, Path(name_as) if name_as else path, pins, capacity)


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
