#!/usr/bin/env python3
"""Runs one system through every variant and class, as one process under the measurement lock.

    python3 -I compare/run_system.py --system questdb --part <file> [layout options]

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

from compare import coldcache  # noqa: E402
from compare.durability import CLASS_A, CLASSES, sync_tree  # noqa: E402
from compare.layout import PINS_PATH, Layout, child_environment  # noqa: E402
from compare.queries import (build_queries, ground_truth, query_to_json)  # noqa: E402
from compare.schema import ColumnSet, TradingDay  # noqa: E402
from compare.settings import PROVEN  # noqa: E402
from compare.systems import SYSTEM_NAMES, system_class  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[1]
WORKER = REPO_ROOT / "compare" / "worker.py"
LINUX_CPUS = {4, 6}
THREADS = 2
WRITE_REPETITIONS = 3
WARM_REPETITIONS = 30
COLD_REPETITIONS = 7
EXIT_FAILED = 1


class StepFailed(Exception):
    """A worker exited non-zero. Its stderr is in the run's log."""


class Terminated(Exception):
    """SIGTERM arrived. Raised so that every finally runs."""


def on_terminate(signal_number, frame):
    raise Terminated(f"signal {signal_number}")


def memory_available_bytes():
    if platform.system() != "Linux":
        return None
    for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    return None


class Runner:
    def __init__(self, args):
        self.args = args
        self.layout = Layout.from_arguments(args)
        self.pins = json.loads(PINS_PATH.read_text(encoding="utf-8"))
        self.pin = self.pins["slices"][args.slice]
        self.source = self.layout.data / "slices" / self.pin["name"]
        self.columns_dir = self.layout.export_dir(args.slice)
        self.day = TradingDay.from_pin(self.pin["day"])
        self.stores = self.layout.work / "stores" / args.system
        self.requests = self.layout.work / "requests" / args.system
        self.system_class = system_class(args.system)
        self.memory_seen = []
        self.write_repetitions = args.write_repetitions
        self.step_number = 0

    def context(self, root, variant, durability):
        return RunContext(self.layout, root, variant, durability, self.day, THREADS)

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
                   "day": self.pin["day"],
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
        answer = self.worker(system, "read_back", destination=str(destination))
        report = self.verify(destination)
        shutil.rmtree(destination)
        if report["exit_code"] != 0 or report["verdict"] != "pass":
            name = f"{system.name}/{system.context.variant}/{system.context.durability}"
            raise StepFailed(f"{name} cannot rebuild the source: {report}")
        return {"oracle": report, "seconds": answer["read_seconds"],
                "peak_rss_bytes": answer["peak_rss_bytes"],
                "stored_settings": answer["stored_settings"]}

    def run_class(self, variant, durability, control_saw_dirty):
        """Flush proof, writes, then the last write's store sized and read back."""
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
        sync_tree(root)
        stored = coldcache.allocated_bytes(system.data_paths())
        stored["store_root_allocated_bytes"] = coldcache.allocated_bytes(
            [root])["allocated_bytes"]
        stored["ratio_stored_over_source"] = stored["allocated_bytes"] / self.pin["size"]
        stored["denominator"] = "the slice's source bytes"
        stored["measured"] = "at rest, after commit, service stopped, every file flushed first"
        stored["settle"] = settle

        self.note(f"{variant} class {durability}: read back")
        try:
            system.start_service()
            read_back = self.read_back(system)
        finally:
            service = system.stop_service()

        flush = {"probe": {key: value for key, value in probe.items()
                           if key not in ("settings", "peak_rss_bytes")},
                 "full_ingest": answer["flush"]}
        verdict = system.durability_verdict(probe, control_saw_dirty)
        if durability != CLASS_A and verdict["status"] == PROVEN:
            whole = system.durability_verdict(answer["flush"], control_saw_dirty)
            verdict = whole if whole["status"] != PROVEN else verdict
        facts_of_class = system.flush_findings(system.durability_facts(), answer["flush"])
        return {"durability": {"class": durability, **facts_of_class, **verdict, "flush": flush},
                "settings": settings, "write": summarise_writes(repetitions, self.pin),
                "facts": facts, "stored": stored, "read_back": read_back}, probe

    def run_queries(self, variant, durability, queries, truth):
        """Warm and cold on one store, with every answer against the ground truth."""
        root = self.stores / variant / durability
        system = self.system_class(self.context(root, variant, durability))
        paths = system.data_paths()
        self.note(f"{variant}: warm queries")
        try:
            system.start_service()
            warm = self.worker(system, "warm_queries", paths=[str(path) for path in paths],
                               queries=[query_to_json(query) for query in queries], truth=truth,
                               repetitions=self.args.warm_repetitions)
        finally:
            system.stop_service()

        cold = {query.name: [] for query in queries}
        proofs = {query.name: [] for query in queries}
        for query in queries:
            self.note(f"{variant}: cold {query.name}")
            for _ in range(self.args.cold_repetitions):
                sync_tree(root)
                evicted = coldcache.evict(self.layout, paths)
                device = coldcache.DeviceCounter(paths[0])
                try:
                    system.start_service()
                    answer = self.worker(system, "cold_query", query=query_to_json(query),
                                         truth=truth[query.name])
                    after = coldcache.residency(self.layout, system.read_paths())
                finally:
                    system.stop_service()
                proofs[query.name].append(coldcache.proof_of_cold(evicted, after, device.delta()))
                cold[query.name].append(answer["seconds"])

        results = {}
        for query in queries:
            warm_query = warm["queries"][query.name]
            results[query.name] = {
                "digest": warm_query["digest"], "matches_ground_truth": True,
                "scan": warm["scan_facts"][query.name],
                "warm": {**latency(warm_query["seconds"]), "proof": warm["proof"]},
                "cold": {**latency(cold[query.name]), "proofs": proofs[query.name]},
            }
        floor = latency(warm["floor_seconds"])
        return results, floor, warm["peak_rss_bytes"], self.after_queries(system)

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
        """Nothing is loaded from a slice or an export that is not the pinned one."""
        digest = hashlib.sha256()
        with open(self.source, "rb") as source:
            while chunk := source.read(1 << 20):
                digest.update(chunk)
        if digest.hexdigest() != self.pin["sha256"]:
            raise StepFailed(f"the slice's sha256 is {digest.hexdigest()}, not its pin")
        report = self.verify(self.columns_dir)
        if report["verdict"] != "pass" or report["exit_code"] != 0:
            raise StepFailed(f"the export does not rebuild the pinned slice: {report}")

    def run(self):
        self.verify_slice()
        self.note("the slice and its export match the pin")
        columns = ColumnSet(self.columns_dir)
        if columns.manifest["tail_bytes"] != 0:
            raise StepFailed("the slice has a tail, and the table has nowhere to keep it")
        if columns.records != self.pin["message_count"]:
            raise StepFailed("the export does not hold the pinned message count")
        window_end = self.window_end_ns()
        queries = build_queries(columns, window_end)
        truth = {query.name: ground_truth(columns, query, self.day) for query in queries}
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
            query_results, floor, peak, extra = self.run_queries(variant, CLASSES[-1], queries,
                                                                 truth)
            entry["queries"] = query_results
            entry["query_store_class"] = CLASSES[-1]
            entry["python_floor"] = {**floor, "what": "the same Python call over an empty "
                                                      "window: the path's cost, with no data"}
            entry["query_worker_peak_rss_bytes"] = peak
            if extra is not None:
                entry["after_queries"] = extra
            out["variants"][variant] = entry
            shutil.rmtree(self.stores / variant, ignore_errors=True)
        out["queries"] = [query_to_json(query) for query in queries]
        out["truth"] = truth
        out["mem_available_bytes"] = ({"lowest": min(self.memory_seen),
                                       "highest": max(self.memory_seen),
                                       "samples": len(self.memory_seen)}
                                      if self.memory_seen else "not-available")
        return out

    def window_end_ns(self):
        hours, minutes, seconds = (int(part) for part in self.pin["to"].split(":"))
        return (hours * 3600 + minutes * 60 + seconds) * 1_000_000_000


def latency(seconds):
    """Nearest-rank p99, so that a small n gives a value that really occurred."""
    ordered = sorted(seconds)
    rank = max(0, -(-99 * len(ordered) // 100) - 1)
    return {"n": len(ordered), "median_us": statistics.median(ordered) * 1e6,
            "p99_us": ordered[rank] * 1e6, "min_us": ordered[0] * 1e6, "max_us": ordered[-1] * 1e6}


def summarise_writes(repetitions, pin):
    seconds = [item["timings"]["seconds"] for item in repetitions]
    median = statistics.median(seconds)
    field = {key: statistics.median(item["timings"][key] for item in repetitions)
             for key in ("engine_seconds", "python_seconds")}
    return {"repetitions": len(seconds), "seconds": seconds, "seconds_median": median,
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
    parser.add_argument("--variants", help="comma-separated, for iteration")
    parser.add_argument("--write-repetitions", type=int, default=WRITE_REPETITIONS)
    parser.add_argument("--warm-repetitions", type=int, default=WARM_REPETITIONS)
    parser.add_argument("--cold-repetitions", type=int, default=COLD_REPETITIONS)
    for name in ("data", "vendor", "work", "tools"):
        parser.add_argument(f"--{name}", type=Path)
    return parser.parse_args(argv)


def pin_threads():
    """On Linux, the run's two CPUs. taskset -c 4 is the wrapper's. cpu6 is the second."""
    if platform.system() != "Linux":
        return {"status": "not-available", "cpus": []}
    before = sorted(os.sched_getaffinity(0))
    os.sched_setaffinity(0, LINUX_CPUS)
    return {"status": "verified", "started_on": before, "cpus": sorted(os.sched_getaffinity(0))}


def main(argv):
    signal.signal(signal.SIGTERM, on_terminate)
    args = parse_arguments(argv)
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
