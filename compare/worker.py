#!/usr/bin/env python3
"""One step of a run, in a process of its own.

    python3 -I compare/worker.py <request.json> <response.json>

The runner starts one of these per step, so that a step's peak memory is its own and a cold
query opens the engine from nothing. Timed sections are in here. All numbers go back to the
runner, which alone reads the host's caches and devices for cold runs.
"""

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compare import coldcache  # noqa: E402
from compare.layout import Layout  # noqa: E402
from compare.queries import (QueryMismatch, digest_result, query_from_json,  # noqa: E402
                             require_truth, run_query)
from compare.schema import ColumnSet, TradingDay, arrow_table, write_columns  # noqa: E402
from compare.settings import split_dirty  # noqa: E402
from compare.systems import system_class  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402

FLUSH_PROBE_ROWS = 1 << 16
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
    table = arrow_table(columns, system.context.day)
    settings = system.settings_in_force()
    timings = system.ingest(columns, table)
    answer = {"timings": timings, "settings": settings, "rows": table.num_rows}
    if request.get("flush_report"):
        saved = system.dirty_paths
        system.dirty_paths = system.data_paths
        answer["flush"] = {**flush_report(system, layout), **system.flush_facts()}
        system.dirty_paths = saved
    return answer


def step_flush_probe(system, request, layout):
    columns = ColumnSet(request["columns"])
    table = arrow_table(columns, system.context.day).slice(0, FLUSH_PROBE_ROWS)
    started = time.perf_counter()
    system.probe_flush(table)
    seconds = time.perf_counter() - started
    return {"rows": table.num_rows, "seconds": seconds, **flush_report(system, layout),
            **system.flush_facts()}


def step_read_back(system, request, layout):
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
    """Every query, `repetitions` times, between two residency proofs."""
    queries = [query_from_json(item) for item in request["queries"]]
    truth = request["truth"]
    repetitions = request["repetitions"]
    paths = [Path(path) for path in request["paths"]]
    coldcache.warm(layout, paths)
    before = coldcache.residency(layout, paths)
    answers = {}
    for query in queries:
        seconds = []
        for _ in range(repetitions):
            elapsed, result = timed_query(system, query)
            seconds.append(elapsed)
            digest = digest_result(query, result)
            require_truth(query, digest, truth[query.name], system.name)
        answers[query.name] = {"seconds": seconds, "digest": digest}
    after = coldcache.residency(layout, paths)
    proof = coldcache.proof_of_warm(before, after)
    floor = python_floor(system)
    facts = {query.name: system.scan_facts(query) for query in queries}
    return {"queries": answers, "proof": proof, "floor_seconds": floor, "scan_facts": facts}


def step_cold_query(system, request, layout):
    query = query_from_json(request["query"])
    elapsed, result = timed_query(system, query)
    digest = digest_result(query, result)
    require_truth(query, digest, request["truth"], system.name)
    return {"seconds": elapsed, "digest": digest}


STEPS = {"ingest": step_ingest, "flush_probe": step_flush_probe, "read_back": step_read_back,
         "warm_queries": step_warm_queries, "cold_query": step_cold_query}


def main(argv):
    request = json.loads(Path(argv[0]).read_text(encoding="utf-8"))
    layout = Layout(**{key: Path(value) for key, value in request["layout"].items()})
    day = TradingDay.from_pin(request["day"])
    context = RunContext(layout, Path(request["root"]), request["variant"], request["class"], day)
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
