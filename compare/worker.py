#!/usr/bin/env python3
"""One step of a run, in a process of its own.

    python3 -I compare/worker.py <request.json> <response.json>

The runner starts one of these per step, so that a step's peak memory is its own and a cold
query opens the engine from nothing. Timed sections are in here. All numbers go back to the
runner, which alone reads the host's caches and devices for cold runs.
"""

import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compare import coldcache  # noqa: E402
from compare.layout import Layout  # noqa: E402
from compare.queries import (QueryMismatch, digest_result, query_from_json,  # noqa: E402
                             require_truth, run_query)
from compare.schema import (ColumnSet, TradingDay, arrow_table, table_chunks,  # noqa: E402
                            write_columns)
from compare.settings import split_dirty  # noqa: E402
from compare.systems import system_class  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402

FLUSH_PROBE_ROWS = 1 << 16
WARM_UP_RUNS = 1
WARM_ATTEMPTS = 3
FLOOR_REPETITIONS = 100
FLOOR_WINDOW = (0, 1)


def flush_report(system, layout):
    """The dirty pages left once a commit returned, read straight away."""
    files = coldcache.dirty_by_file(layout, system.dirty_paths())
    if files is None:
        return {"data_dirty_pages": None, "data_dirty_files": {}, "residual_dirty_files": {}}
    held, excused = split_dirty(files, system.flush_exempt)
    return {"data_dirty_pages": sum(held.values()), "data_dirty_files": held,
            "residual_dirty_files": excused}


def step_ingest(system, request, layout):
    columns = ColumnSet(request["columns"])
    settings = system.settings_in_force()
    if request.get("full_day"):
        # A whole day streams: no day-sized table is ever built.
        timings = system.ingest_day(columns)
        rows = columns.records
    else:
        table = arrow_table(columns, system.context.day)
        timings = system.ingest(columns, table)
        rows = table.num_rows
    settings = settings + system.settings_after_ingest()
    answer = {"timings": timings, "settings": settings, "rows": rows}
    if request.get("flush_report"):
        saved = system.dirty_paths
        system.dirty_paths = system.data_paths
        answer["flush"] = {**flush_report(system, layout), **system.flush_facts()}
        system.dirty_paths = saved
    return answer


def step_ingest_stream(system, request, layout):
    """A whole day, one chunk at a time. compare/fullday.py is the only caller."""
    columns = ColumnSet(request["columns"])
    settings = system.settings_in_force()
    chunks = table_chunks(columns, system.context.day, system.stream_chunk_rows())
    return {"timings": system.ingest_stream(chunks), "settings": settings}


def step_capacity_query(system, request, layout):
    """One query at full size, its answer against the truth. Its time is no measurement."""
    query = query_from_json(request["query"])
    elapsed, result = timed_query(system, query)
    digest = digest_result(query, result)
    require_truth(query, digest, request["truth"], system.name)
    rows = result[0][0] if query.kind == "column_sum" else result.num_rows
    return {"seconds": elapsed, "rows": rows, "digest": digest}


def step_stored_settings(system, request, layout):
    return {"stored_settings": system.stored_settings()}


def step_flush_probe(system, request, layout):
    columns = ColumnSet(request["columns"])
    table = next(table_chunks(columns, system.context.day, FLUSH_PROBE_ROWS))
    started = time.perf_counter()
    system.probe_flush(table)
    seconds = time.perf_counter() - started
    return {"rows": table.num_rows, "seconds": seconds, **flush_report(system, layout),
            **system.flush_facts()}


def step_read_back(system, request, layout):
    if request.get("full_day"):
        # A whole day does not fit in memory as one table, so it goes back one window at a time.
        started = time.perf_counter()
        answer = system.read_back_to(request["destination"],
                                     [tuple(window) for window in request["windows"]])
        written = time.perf_counter()
        return {"read_seconds": answer["read_seconds"],
                "write_columns_seconds": written - started - answer["read_seconds"],
                "rows": answer["rows"], "windows": answer["windows"],
                "method": answer["method"],
                "stored_settings": system.stored_settings()}
    started = time.perf_counter()
    table = system.read_all()
    read = time.perf_counter()
    write_columns(table, request["destination"], system.context.day)
    written = time.perf_counter()
    return {"read_seconds": read - started, "write_columns_seconds": written - read,
            "rows": table.num_rows, "stored_settings": system.stored_settings()}


def timed_query(system, query):
    started = time.perf_counter()
    result = run_query(system, query)
    return time.perf_counter() - started, result


def python_floor(system):
    """The same call over a window with nothing in it: what the Python path costs alone."""
    seconds = []
    from compare.queries import ROW_COLUMNS

    for _ in range(FLOOR_REPETITIONS):
        started = time.perf_counter()
        system.query_rows(*FLOOR_WINDOW, -1, ROW_COLUMNS)
        seconds.append(time.perf_counter() - started)
    return seconds


def step_warm_queries(system, request, layout):
    """Every query, `repetitions` times, between two residency proofs. A whole day's store can
    be larger than memory, so there the proof is the query's own: see `warm_full_day`."""
    if request.get("full_day"):
        return warm_full_day(system, request, layout)
    queries = [query_from_json(item) for item in request["queries"]]
    truth = request["truth"]
    repetitions = request["repetitions"]
    paths = [Path(path) for path in request["paths"]]
    coldcache.warm(layout, paths)
    before = coldcache.residency(layout, paths)
    answers = {}
    for query in queries:
        seconds, engine, digest = timed_repetitions(system, query, truth, repetitions)
        answers[query.name] = {"seconds": seconds, "digest": digest}
        if None not in engine:
            answers[query.name]["engine_seconds"] = engine
    after = coldcache.residency(layout, paths)
    proof = coldcache.proof_of_warm(before, after)
    floor = python_floor(system)
    facts = {query.name: system.scan_facts(query) for query in queries}
    return {"queries": answers, "proof": proof, "floor_seconds": floor, "scan_facts": facts}


def timed_repetitions(system, query, truth, repetitions):
    seconds = []
    engine = []
    digest = None
    for _ in range(repetitions):
        elapsed, result = timed_query(system, query)
        seconds.append(elapsed)
        digest = digest_result(query, result)
        require_truth(query, digest, truth[query.name], system.name)
        engine.append(engine_seconds(result))
    return seconds, engine, digest


def warm_full_day(system, request, layout):
    """Warm on a whole day, query by query. QuestDB's store is larger than this host's memory,
    so no store can be held resident whole, and the proof is instead that the timed repetitions
    read nothing from storage: each query runs untimed first, answer checked, and then the
    processes that answer it, this worker, its query tool and the system's server, must cause
    no storage read across its repetitions (coldcache.ProcessReads). The block device's
    machine-wide count is kept beside it, since another process on the host can move it. A
    failed attempt is kept in the result, with what was read, and the query runs again, up to
    WARM_ATTEMPTS times. Residency of every store file is recorded before and after."""
    queries = [query_from_json(item) for item in request["queries"]]
    truth = request["truth"]
    repetitions = request["repetitions"]
    paths = [Path(path) for path in request["paths"]]
    answers = {}
    proofs = {}
    for query in queries:
        attempts = []
        for _ in range(WARM_ATTEMPTS):
            warm_up = timed_repetitions(system, query, truth, WARM_UP_RUNS)[0]
            before = coldcache.residency(layout, paths)
            free_before = coldcache.meminfo_bytes("MemFree")
            device = coldcache.DeviceCounter(paths[0])
            reads = coldcache.ProcessReads({**request.get("service_processes", {}),
                                            **system.query_processes()})
            seconds, engine, digest = timed_repetitions(system, query, truth, repetitions)
            caused = reads.delta()
            moved = device.delta()
            free_after = coldcache.meminfo_bytes("MemFree")
            after = coldcache.residency(layout, paths)
            attempt = {"device_bytes_read": caused["bytes"], "device_counter": caused["counter"],
                       "by_process": caused["by_process"],
                       "machine_wide_device_bytes_read": moved["bytes"],
                       "machine_wide_counter": moved["counter"],
                       "warm_up_runs": len(warm_up), "warm_up_seconds": warm_up,
                       "pages": after["pages"], "resident_before": before["resident_pages"],
                       "resident_after": after["resident_pages"],
                       "mem_free_bytes_before": free_before, "mem_free_bytes_after": free_after,
                       "median_us": statistics.median(seconds) * 1e6}
            attempts.append(attempt)
            if caused["bytes"] == 0:
                break
        else:
            raise coldcache.ProofFailed(f"warm {query.name}: the query's processes read from "
                                        f"storage in each of {WARM_ATTEMPTS} attempts: "
                                        f"{attempts}")
        answers[query.name] = {"seconds": seconds, "digest": digest}
        if None not in engine:
            answers[query.name]["engine_seconds"] = engine
        proofs[query.name] = {
            "method": "per query: untimed runs first, then the query's own processes read 0 "
                      "bytes from storage across every timed repetition; the block device's "
                      "machine-wide count is kept beside it",
            "device_slack_bytes": 0, "attempts": len(attempts), "accepted": attempts[-1],
            "discarded": attempts[:-1]}
    floor = python_floor(system)
    facts = {query.name: system.scan_facts(query) for query in queries}
    return {"queries": answers, "proofs": proofs, "floor_seconds": floor, "scan_facts": facts}


def engine_seconds(result):
    """The engine's own time for an answer, where the system reports one beside the round trip."""
    return getattr(result, "engine_seconds", None)


def step_cold_query(system, request, layout):
    query = query_from_json(request["query"])
    elapsed, result = timed_query(system, query)
    digest = digest_result(query, result)
    require_truth(query, digest, request["truth"], system.name)
    return {"seconds": elapsed, "digest": digest, "engine_seconds": engine_seconds(result)}


STEPS = {"ingest": step_ingest, "flush_probe": step_flush_probe, "read_back": step_read_back,
         "warm_queries": step_warm_queries, "cold_query": step_cold_query,
         "ingest_stream": step_ingest_stream, "capacity_query": step_capacity_query,
         "stored_settings": step_stored_settings}


def main(argv):
    request = json.loads(Path(argv[0]).read_text(encoding="utf-8"))
    layout = Layout(**{key: Path(value) for key, value in request["layout"].items()})
    day = TradingDay.from_pin(request["day"])
    source = request.get("source")
    context = RunContext(layout, Path(request["root"]), request["variant"], request["class"], day,
                         full_day=request.get("full_day", False),
                         source=Path(source) if source else None)
    system = system_class(request["system"])(context)
    system.open()
    try:
        answer = STEPS[request["step"]](system, request, layout)
    finally:
        system.close()
    Path(argv[1]).write_text(json.dumps(answer), encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except QueryMismatch as mismatch:
        print(f"query mismatch: {mismatch}", file=sys.stderr)
        sys.exit(3)
