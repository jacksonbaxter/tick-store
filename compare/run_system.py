#!/usr/bin/env python3
"""Runs one system through every variant and class, as one process under the measurement lock.

    python3 -I compare/run_system.py --system questdb --part <file> [layout options]
    python3 -I compare/run_system.py --system questdb --part <file> --day 12302019 ...

The second is a full-day run: the day's ITCH file and its whole export, every query as
BENCHMARKS.md defines it with nothing clipped, each write streamed, each store read back a
window at a time, and on Linux warm proven query by query (compare/worker.py). Off Linux a full
day is tick-store alone, and its warm proof is the slice's, every store file resident before and
after the repetitions, since the per-query proof reads /proc and tick-store's day store fits
the host's memory.

compare/run.py is the way to call this. It leaves no process behind: a server runs only
inside a try/finally, SIGTERM is turned into an exception so that the finally runs, and on
Linux a server also dies with its parent. The part file appears only when the whole system
has finished.
"""

import argparse
import hashlib
import json
import os
import platform
import resource
import shutil
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compare import coldcache, swap  # noqa: E402
from compare.durability import CLASS_A, CLASSES, sync_tree  # noqa: E402
from compare.layout import PINS_PATH, Layout, child_environment  # noqa: E402
from compare.queries import (build_queries, full_day_facts, ground_truth,  # noqa: E402
                             query_to_json)
from compare.schema import ColumnSet, TradingDay, read_windows  # noqa: E402
from compare.settings import PROVEN  # noqa: E402
from compare.systems import SYSTEM_NAMES, system_class  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[1]
WORKER = REPO_ROOT / "compare" / "worker.py"
LINUX_CPUS = {4, 6}
THREADS = 2
WRITE_REPETITIONS = 3
# A full-day write is one sustained run of minutes, and three of each class for every variant
# would not fit the run: see "Full-day comparison" in compare/README.md.
FULL_DAY_WRITE_REPETITIONS = 1
# Rows per window of a full-day read-back: about 600 MB of Arrow at a time.
READ_WINDOW_ROWS = 1 << 22
WARM_REPETITIONS = 30
COLD_REPETITIONS = 7
# A cold repetition beside something else reading the store is run again, this many times at
# most per query.
COLD_RERUNS = 10
EXIT_FAILED = 1
# How warm is proven: every store file resident before and after (the slice, and tick-store's
# day off Linux), or the query's own processes reading nothing from storage (a day on Linux).
RESIDENCY = "residency"
PROCESS_READS = "process_reads"


class StepFailed(Exception):
    """A worker exited non-zero. Its stderr is in the run's log."""


class Terminated(Exception):
    """SIGTERM arrived. Raised so that every finally runs."""


def on_terminate(signal_number, frame):
    raise Terminated(f"signal {signal_number}")


def memory_available_bytes():
    return coldcache.meminfo_bytes("MemAvailable")


class Runner:
    def __init__(self, args):
        self.args = args
        self.layout = Layout.from_arguments(args)
        self.pins = json.loads(PINS_PATH.read_text(encoding="utf-8"))
        self.full_day = bool(args.day or args.full_day_smoke)
        if args.day:
            self.pin = day_pin(self.pins, args.day)
            self.source = self.layout.data / self.pin["name"]
            self.columns_dir = self.layout.export_dir(f"{args.day}-full")
        else:
            self.pin = self.pins["slices"][args.slice]
            self.source = self.layout.data / "slices" / self.pin["name"]
            self.columns_dir = self.layout.export_dir(args.slice)
        self.day = TradingDay.from_pin(self.pin["day"])
        self.windows = None
        self.cleared = None
        self.stores = self.layout.work / "stores" / args.system
        self.requests = self.layout.work / "requests" / args.system
        self.system_class = system_class(args.system)
        self.memory_seen = []
        self.write_repetitions = args.write_repetitions or (
            FULL_DAY_WRITE_REPETITIONS if self.full_day else WRITE_REPETITIONS)
        self.warm_proof = warm_proof_kind(self.full_day)
        self.step_number = 0

    def context(self, root, variant, durability):
        return RunContext(self.layout, root, variant, durability, self.day, THREADS,
                          full_day=self.full_day, source=self.source)

    def note(self, text):
        print(f"[{time.strftime('%H:%M:%S')}] {self.args.system}: {text}", flush=True)

    def sample_memory(self):
        available = memory_available_bytes()
        if available is not None:
            self.memory_seen.append(available)

    def worker(self, system, step, **fields):
        """One worker. Returns its answer with its own peak memory added."""
        self.step_number += 1
        request_path = self.requests / f"{self.step_number:05d}-{step}.json"
        answer_path = request_path.with_suffix(".answer")
        request = {"step": step, "system": system.name, "variant": system.context.variant,
                   "class": system.context.durability, "root": str(system.root),
                   "day": self.pin["day"], "source": str(self.source),
                   "full_day": self.full_day,
                   "layout": {key: str(getattr(self.layout, key))
                              for key in ("data", "vendor", "work", "tools")}, **fields}
        request_path.write_text(json.dumps(request), encoding="utf-8")
        environment = child_environment(self.layout)
        environment.update(system.worker_environment())
        process = subprocess.Popen([sys.executable, "-I", str(WORKER), str(request_path),
                                    str(answer_path)], stdin=subprocess.DEVNULL, env=environment)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
        if process.returncode != 0:
            raise StepFailed(f"{step} exited {process.returncode}")
        answer = json.loads(answer_path.read_text(encoding="utf-8"))
        scale = 1 if platform.system() == "Darwin" else 1024
        answer["peak_rss_bytes"] = usage.ru_maxrss * scale
        request_path.unlink()
        answer_path.unlink()
        self.sample_memory()
        return answer

    def fresh(self, root, variant, durability):
        shutil.rmtree(root, ignore_errors=True)
        root.parent.mkdir(parents=True, exist_ok=True)
        system = self.system_class(self.context(root, variant, durability))
        system.prepare()
        return system

    def flush_probe(self, variant, durability):
        """A small commit in a store of its own, and the dirty pages it left."""
        root = self.stores / "flush-probe"
        system = self.fresh(root, variant, durability)
        try:
            system.start_service()
            settings = system.service_settings()
            answer = self.worker(system, "flush_probe", columns=str(self.columns_dir))
        finally:
            system.stop_service()
        shutil.rmtree(root)
        answer["settings"] = settings
        return answer

    def verify(self, directory):
        """The same G-1 oracle as the store tests, on what the system gave back."""
        run = subprocess.run(
            [str(self.layout.tool("tick_roundtrip")), "verify-columns", str(self.source),
             str(directory), "--expect-sha256", self.pin["sha256"]],
            check=False, capture_output=True, text=True)
        try:
            report = json.loads(run.stdout)
        except json.JSONDecodeError:
            report = {"verdict": "no report", "error": run.stderr.strip()[:200]}
        report["exit_code"] = run.returncode
        return report

    def read_back(self, system):
        """The store, read whole, must rebuild the source bit for bit. Any failure is fatal."""
        destination = self.layout.work / "readback" / self.args.system
        shutil.rmtree(destination, ignore_errors=True)
        inputs = self.evict_inputs() if self.full_day else None
        answer = self.worker(system, "read_back", destination=str(destination),
                             windows=self.windows)
        report = self.verify(destination)
        shutil.rmtree(destination)
        if report["exit_code"] != 0 or report["verdict"] != "pass":
            name = f"{system.name}/{system.context.variant}/{system.context.durability}"
            raise StepFailed(f"{name} cannot rebuild the source: {report}")
        result = {"oracle": report, "seconds": answer["read_seconds"],
                  "peak_rss_bytes": answer["peak_rss_bytes"],
                  "stored_settings": answer["stored_settings"]}
        if self.full_day:
            result["streamed"] = {"windows": answer["windows"], "rows": answer["rows"],
                                  "write_columns_seconds": answer["write_columns_seconds"],
                                  "method": answer["method"], "inputs_evicted_before": inputs}
        return result

    def run_class(self, variant, durability, control_saw_dirty):
        """Flush proof, writes, then the last write's store sized and read back."""
        write_phase = swap.Phase().start()
        self.note(f"{variant} class {durability}: flush probe")
        probe = self.flush_probe(variant, durability)
        root = self.stores / variant / durability
        repetitions = []
        for repetition in range(self.write_repetitions):
            last = repetition == self.write_repetitions - 1
            self.note(f"{variant} class {durability}: write {repetition + 1}")
            system = self.fresh(root, variant, durability)
            try:
                system.start_service()
                settings = system.service_settings()
                answer = self.worker(system, "ingest", columns=str(self.columns_dir),
                                     flush_report=last)
                settle = system.settle() if last else {}
                facts = system.service_facts() if last else {}
                facts["requested"] = system.requested()
            finally:
                service = system.stop_service()
            repetitions.append({"timings": answer["timings"],
                                "peak_rss_bytes": answer["peak_rss_bytes"],
                                "service": service})
        settings = settings + answer["settings"]
        times = self.system_class.write_times(self.full_day)
        sync_tree(root)
        stored = coldcache.allocated_bytes(system.data_paths())
        stored["store_root_allocated_bytes"] = coldcache.allocated_bytes(
            [root])["allocated_bytes"]
        stored["ratio_stored_over_source"] = stored["allocated_bytes"] / self.pin["size"]
        stored["denominator"] = ("the day's source bytes" if self.args.day
                                 else "the slice's source bytes")
        stored["measured"] = "at rest, after commit, service stopped, every file flushed first"
        stored["settle"] = settle
        write_phase.stop()

        self.note(f"{variant} class {durability}: read back")
        with swap.Phase() as read_phase:
            try:
                system.start_service()
                read_back = self.read_back(system)
            finally:
                service = system.stop_service()

        if self.full_day and durability == CLASS_A:
            # Queries run on class B alone, and two stores of a day at once would not fit beside
            # the read-back: the class A store goes once it is sized and read back.
            shutil.rmtree(root)
        flush = {"probe": {key: value for key, value in probe.items()
                           if key not in ("settings", "peak_rss_bytes")},
                 "full_ingest": answer["flush"]}
        verdict = system.durability_verdict(probe, control_saw_dirty)
        if durability != CLASS_A and verdict["status"] == PROVEN:
            whole = system.durability_verdict(answer["flush"], control_saw_dirty)
            verdict = whole if whole["status"] != PROVEN else verdict
        facts_of_class = system.flush_findings(system.durability_facts(), answer["flush"])
        return {"durability": {"class": durability, **facts_of_class, **verdict, "flush": flush},
                "settings": settings, "write": summarise_writes(repetitions, self.pin, times),
                "facts": facts, "stored": stored, "read_back": read_back,
                "swap": {"write": write_phase.counts, "read_back": read_phase.counts}}, probe

    def run_queries(self, variant, durability, queries, truth):
        """Warm and cold on one store, with every answer against the ground truth."""
        root = self.stores / variant / durability
        system = self.system_class(self.context(root, variant, durability))
        paths = system.data_paths()
        warm_phase = swap.Phase().start()
        if self.full_day:
            self.cleared = self.clear_page_cache(root, paths)
        self.note(f"{variant}: warm queries")
        try:
            system.start_service()
            warm = self.worker(system, "warm_queries", paths=[str(path) for path in paths],
                               queries=[query_to_json(query) for query in queries], truth=truth,
                               repetitions=self.args.warm_repetitions,
                               service_processes=system.service_processes(),
                               warm_proof=self.warm_proof)
            if self.full_day:
                self.sample_memory()
        finally:
            system.stop_service()
        warm_phase.stop()

        cold_phase = swap.Phase().start()
        cold, cold_engine, cold_swap, proofs, run_again = self.cold_runs(
            variant, system, root, paths, queries, truth)
        cold_phase.stop()

        results = {}
        for query in queries:
            warm_query = warm["queries"][query.name]
            results[query.name] = {
                "digest": warm_query["digest"], "matches_ground_truth": True,
                "scan": warm["scan_facts"][query.name],
                "warm": {**latency(warm_query["seconds"]),
                         "proof": (warm["proofs"][query.name]
                                   if self.warm_proof == PROCESS_READS else warm["proof"]),
                         "swap_timed": swap.timed([warm_query["swap_timed"]])},
                "cold": {**latency(cold[query.name]), "proofs": proofs[query.name],
                         "run_again_refilled_pages": run_again[query.name],
                         "swap_timed": {**swap.timed(cold_swap[query.name]),
                                        "pages_in_by_repetition": [
                                            item["pages_in"] for item in cold_swap[query.name]]}},
            }
            # A system that reports its engine's own time gets it beside the round trip, which
            # stays the number that is compared.
            if "engine_seconds" in warm_query:
                results[query.name]["warm"]["engine"] = latency(warm_query["engine_seconds"])
            if None not in cold_engine[query.name]:
                results[query.name]["cold"]["engine"] = latency(cold_engine[query.name])
        floor = latency(warm["floor_seconds"])
        phases = {"warm_queries": warm_phase.counts, "cold_queries": cold_phase.counts}
        return results, floor, warm["peak_rss_bytes"], self.after_queries(system), phases

    def cold_runs(self, variant, system, root, paths, queries, truth):
        """Each query's cold repetitions. One run beside something else reading the store is
        not counted and runs again, at most COLD_RERUNS times per query; the pages each such
        run saw the store gain are kept."""
        cold = {query.name: [] for query in queries}
        cold_engine = {query.name: [] for query in queries}
        cold_swap = {query.name: [] for query in queries}
        proofs = {query.name: [] for query in queries}
        run_again = {query.name: [] for query in queries}
        for query in queries:
            self.note(f"{variant}: cold {query.name}")
            while len(cold[query.name]) < self.args.cold_repetitions:
                answer, proof, refilled = self.cold_repetition(system, root, paths, query,
                                                               truth[query.name])
                if refilled:
                    run_again[query.name].append(refilled)
                    self.note(f"{variant}: cold {query.name}: the store gained {refilled} pages "
                              f"with nothing of ours running, so this repetition runs again")
                    if len(run_again[query.name]) > COLD_RERUNS:
                        raise StepFailed(f"cold {query.name}: something else kept reading the "
                                         f"store: {run_again[query.name]}")
                    continue
                proofs[query.name].append(proof)
                cold[query.name].append(answer["seconds"])
                cold_engine[query.name].append(answer.get("engine_seconds"))
                cold_swap[query.name].append(answer["swap_timed"])
        return cold, cold_engine, cold_swap, proofs, run_again

    def cold_repetition(self, system, root, paths, query, truth):
        """One cold run: the store flushed and evicted, the service started, the query, the
        residency it left, and the service stopped. Then, with nothing of ours running, the
        store must not gain a page. If it does, something else is reading it, so this run may
        have found pages it did not read itself: the third value is the pages gained, and the
        caller runs it again."""
        sync_tree(root)
        evicted = coldcache.evict(self.layout, paths)
        device = coldcache.DeviceCounter(paths[0])
        try:
            system.start_service()
            answer = self.worker(system, "cold_query", query=query_to_json(query), truth=truth)
            after = coldcache.residency(self.layout, system.read_paths())
        finally:
            system.stop_service()
        proof = coldcache.proof_of_cold(evicted, after, device.delta())
        return answer, proof, coldcache.refilled_pages(self.layout, paths)

    def clear_page_cache(self, root, paths):
        """On a day, the page cache emptied of the inputs and the store before warm queries.

        The writes, the read-back, the oracle and the ground truth leave a day's source, its
        export and the store filling this host's 15 GB of page cache, with almost no memory
        free. Then the memory a query allocates is taken by reclaim, which can drop the pages
        the query just read, or the worker's own code, between repetitions, and the warm proof's
        device counter rightly fails. So the source, the export and every store file are
        evicted, each proven at 0 resident, with the service stopped and the store flushed, and
        MemFree is recorded either side. Each query's untimed run then reads what it needs; the
        warm proof itself is unchanged."""
        free_before = coldcache.meminfo_bytes("MemFree")
        sync_tree(root)
        inputs = self.evict_inputs()
        store = coldcache.evict(self.layout, paths)
        return {"what": "the day's ITCH file and its export, then every store file",
                "inputs": inputs, "store_pages": store["pages"],
                "resident_after": inputs["resident_after"] + store["resident_pages"],
                "store_passes": store["passes"], "method": store["method"],
                "mem_free_bytes_before": free_before,
                "mem_free_bytes_after": coldcache.meminfo_bytes("MemFree")}

    def evict_inputs(self):
        """A day's source and export, 32 GB that nothing reads until the next writes or the
        oracle, out of the page cache, proven at 0 resident, with MemFree either side. Before a
        read-back and before warm queries, so that what they allocate comes from free memory and
        not from reclaim, which on this host also swaps: the guard once stopped ArcticDB's
        read-back on swap traffic with 12.4 GB available."""
        free_before = coldcache.meminfo_bytes("MemFree")
        report = coldcache.evict(self.layout, [self.source, self.columns_dir])
        return {"what": "the day's ITCH file and its export", "pages": report["pages"],
                "resident_after": report["resident_pages"], "passes": report["passes"],
                "method": report["method"], "mem_free_bytes_before": free_before,
                "mem_free_bytes_after": coldcache.meminfo_bytes("MemFree")}

    def after_queries(self, system):
        try:
            system.start_service()
            answer = system.after_queries()
        finally:
            system.stop_service()
        if answer is None:
            return None
        sync_tree(system.root)
        answer["allocated_bytes_after"] = coldcache.allocated_bytes(
            system.data_paths())["allocated_bytes"]
        return answer

    def verify_slice(self):
        """Nothing is loaded from a slice, a day or an export that is not the pinned one."""
        digest = hashlib.sha256()
        with open(self.source, "rb") as source:
            while chunk := source.read(1 << 20):
                digest.update(chunk)
        if digest.hexdigest() != self.pin["sha256"]:
            raise StepFailed(f"the source's sha256 is {digest.hexdigest()}, not its pin")
        report = self.verify(self.columns_dir)
        if report["verdict"] != "pass" or report["exit_code"] != 0:
            raise StepFailed(f"the export does not rebuild the pinned source: {report}")

    def run(self):
        self.verify_slice()
        self.note("the source and its export match the pin")
        columns = ColumnSet(self.columns_dir)
        if columns.manifest["tail_bytes"] != 0:
            raise StepFailed("the source has a tail, and the table has nowhere to keep it")
        if columns.records != self.pin["message_count"]:
            raise StepFailed("the export does not hold the pinned message count")
        window_end = None if self.full_day else self.window_end_ns()
        queries = build_queries(columns, window_end)
        truth = {query.name: ground_truth(columns, query, self.day) for query in queries}
        facts = full_day_facts(columns, queries) if self.full_day else None
        if self.full_day:
            self.windows = read_windows(columns, READ_WINDOW_ROWS)
        self.note(f"ground truth computed for {len(queries)} queries")
        del columns

        pick = self.args.variants.split(",") if self.args.variants else list(
            self.system_class.variants)
        out = {"system": self.args.system, "versions": self.system_class.versions(),
               "headline_variant": self.system_class.headline_variant, "variants": {}}
        self.sample_memory()
        for variant in pick:
            entry = {"tuning": self.system_class.variants[variant], "classes": {}}
            control = False
            for durability in CLASSES:
                result, probe = self.run_class(variant, durability, control)
                if durability == CLASS_A:
                    control = (probe["data_dirty_pages"] or 0) > 0
                entry["classes"][durability] = result
            query_results, floor, peak, extra, phases = self.run_queries(
                variant, CLASSES[-1], queries, truth)
            if self.full_day:
                entry["page_cache_before_warm"] = self.cleared
            entry["queries"] = query_results
            entry["query_store_class"] = CLASSES[-1]
            entry["python_floor"] = {**floor, "what": "the same Python call over an empty "
                                                      "window: the path's cost, with no data"}
            entry["query_worker_peak_rss_bytes"] = peak
            entry["swap"] = phases
            if extra is not None:
                entry["after_queries"] = extra
            out["variants"][variant] = entry
            shutil.rmtree(self.stores / variant, ignore_errors=True)
        out["swap_by_phase"] = swap_by_phase(out["variants"])
        out["queries"] = [query_to_json(query) for query in queries]
        out["truth"] = truth
        if self.full_day:
            out["full_day"] = {**facts, "read_back_windows": len(self.windows),
                               "write_repetitions": self.write_repetitions}
        out["mem_available_bytes"] = ({"lowest": min(self.memory_seen),
                                       "highest": max(self.memory_seen),
                                       "samples": len(self.memory_seen)}
                                      if self.memory_seen else "not-available")
        return out

    def window_end_ns(self):
        hours, minutes, seconds = (int(part) for part in self.pin["to"].split(":"))
        return (hours * 3600 + minutes * 60 + seconds) * 1_000_000_000


def swap_by_phase(variants):
    """Pages swapped in and out per phase, over every variant and class the system ran."""
    return {"write": swap.add(*(entry["swap"]["write"] for variant in variants.values()
                                for entry in variant["classes"].values())),
            "read_back": swap.add(*(entry["swap"]["read_back"] for variant in variants.values()
                                    for entry in variant["classes"].values())),
            "warm_queries": swap.add(*(variant["swap"]["warm_queries"]
                                       for variant in variants.values())),
            "cold_queries": swap.add(*(variant["swap"]["cold_queries"]
                                       for variant in variants.values()))}


def tail_rank(count):
    """The nearest rank of the 99th percentile among `count` runs, from 1."""
    return max(1, -(-99 * count // 100))


def tail_label(count, rank):
    """What the tail figure is. Below 100 runs the nearest rank is the last run."""
    if rank == count:
        return f"slowest of {count}"
    return f"99th percentile, nearest rank: run {rank} of {count}"


def latency(seconds):
    """The median and the tail figure, `tail_us`: the 99th percentile by nearest rank, so that
    it is a value that really occurred. Below 100 runs that rank is the last, so the tail figure
    is the slowest run, and the block says so, with n and the rank used: at 30 warm and 7 cold
    repetitions it is the slowest of 30 and the slowest of 7, never a 1% tail. `p99_us` holds
    the same figure under its old name, which the committed files and the generator use. Every
    repetition's seconds are kept, in the order they ran, so that a validator can recompute
    each figure."""
    ordered = sorted(seconds)
    count = len(ordered)
    rank = tail_rank(count)
    tail = ordered[rank - 1] * 1e6
    return {"n": count, "median_us": statistics.median(ordered) * 1e6,
            "tail_us": tail, "tail_rank": rank, "tail_is_slowest": rank == count,
            "tail": tail_label(count, rank), "p99_us": tail,
            "mean_us": statistics.fmean(ordered) * 1e6,
            "min_us": ordered[0] * 1e6, "max_us": ordered[-1] * 1e6,
            "seconds_by_repetition": list(seconds)}


def summarise_writes(repetitions, pin, times):
    """The medians, and every repetition's own times beside them, so that each median can be
    recomputed. Each repetition also keeps its engine-only seconds, how much of its input was
    resident when its clock started and the device bytes read while it ran (review F-2), and
    `times` says what the total and the engine-only seconds hold on this system."""
    seconds = [item["timings"]["seconds"] for item in repetitions]
    median = statistics.median(seconds)
    field = {key: statistics.median(item["timings"][key] for item in repetitions)
             for key in ("engine_seconds", "python_seconds")}
    by_repetition = [{key: item["timings"][key]
                      for key in ("seconds", "engine_seconds", "python_seconds")}
                     for item in repetitions]
    for entry, item in zip(by_repetition, repetitions, strict=True):
        timings = item["timings"]
        if "engine_only_seconds" in timings:
            entry.update({"engine_only_seconds": timings["engine_only_seconds"],
                          "input_pages": timings["input_at_clock_start"]["pages"],
                          "input_resident_pages_at_start":
                              timings["input_at_clock_start"]["resident_pages"],
                          "device_bytes_read": timings["device_read"]["bytes"],
                          "device_counter": timings["device_read"]["counter"]})
    engine_only = [entry["engine_only_seconds"] for entry in by_repetition
                   if "engine_only_seconds" in entry]
    if len(engine_only) == len(by_repetition):
        field["engine_only_seconds_median"] = statistics.median(engine_only)
    return {"repetitions": len(seconds), "seconds": seconds, "seconds_median": median,
            "by_repetition": by_repetition, "times": times,
            "records_per_second_median": pin["message_count"] / median,
            "bytes_per_second_median": pin["size"] / median, **field,
            "python_note": repetitions[0]["timings"]["python_note"],
            "detail": repetitions[-1]["timings"],
            "peak_rss_bytes_client": max(item["peak_rss_bytes"] for item in repetitions),
            "peak_rss_bytes_service": max(item["service"].get("peak_rss_bytes", 0)
                                          for item in repetitions),
            "clock": "stops when every row is readable and durable for its class"}


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--system", required=True, choices=SYSTEM_NAMES)
    parser.add_argument("--part", type=Path, required=True)
    parser.add_argument("--slice", default="12302019-open")
    parser.add_argument("--day", help="a full-day run on this day's ITCH file and its export")
    parser.add_argument("--full-day-smoke", action="store_true",
                        help="the full-day path on the slice, to prove it before a day")
    parser.add_argument("--variants", help="comma-separated")
    parser.add_argument("--write-repetitions", type=int)
    parser.add_argument("--warm-repetitions", type=int, default=WARM_REPETITIONS)
    parser.add_argument("--cold-repetitions", type=int, default=COLD_REPETITIONS)
    for name in ("data", "vendor", "work", "tools"):
        parser.add_argument(f"--{name}", type=Path)
    return parser.parse_args(argv)


def warm_proof_kind(full_day):
    """Per-process storage reads on a Linux day, residency everywhere else."""
    return PROCESS_READS if full_day and platform.system() == "Linux" else RESIDENCY


def pin_threads():
    """On Linux, the run's two CPUs. taskset -c 4 is the wrapper's. cpu6 is the second."""
    if platform.system() != "Linux":
        return {"status": "not-available", "cpus": []}
    before = sorted(os.sched_getaffinity(0))
    os.sched_setaffinity(0, LINUX_CPUS)
    return {"status": "verified", "started_on": before, "cpus": sorted(os.sched_getaffinity(0))}


def day_pin(pins, day):
    """A day's pin in the slice pin's terms: the decompressed file is the source."""
    pin = pins["days"][day]
    return {"name": pin["name"], "day": day, "sha256": pin["decompressed_sha256"],
            "size": pin["decompressed_size"], "message_count": pin["message_count"]}


def main(argv):
    signal.signal(signal.SIGTERM, on_terminate)
    args = parse_arguments(argv)
    if (args.day or args.full_day_smoke) and platform.system() != "Linux" and \
            args.system != "tickstore":
        raise SystemExit("run_system: off Linux a full day is tick-store alone: the other "
                         "systems' warm proof on a day reads /proc")
    runner = Runner(args)
    runner.stores.mkdir(parents=True, exist_ok=True)
    runner.requests.mkdir(parents=True, exist_ok=True)
    pinning = pin_threads()
    started = time.time()
    out = runner.run()
    out["wall_seconds"] = round(time.time() - started, 1)
    out["pinning"] = pinning
    out["threads"] = THREADS
    out["children_peak_rss_bytes"] = (
        resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss *
        (1 if platform.system() == "Darwin" else 1024))
    pending = args.part.with_name(args.part.name + ".pending")
    pending.write_text(json.dumps(out, indent=1), encoding="utf-8")
    os.replace(pending, args.part)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Terminated as terminated:
        print(f"run_system: {terminated}", file=sys.stderr)
        sys.exit(143)
