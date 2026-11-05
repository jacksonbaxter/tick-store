#!/usr/bin/env python3
"""One system through one whole day, as an iteration run: a capacity check, never a benchmark.

    <guard> python3 -I compare/fullday.py --system questdb [--heap 1g] [layout options]

Run it under compare/guard.py, which owns the memory limit, on the export of the whole day in
<data>/export/<day>-full. Ingest streams in bounded chunks (`compare.schema.table_chunks`), then
one full-column scan and one short window run, each answer against the ground truth. The store
is not read back whole, since that is a day on the heap, and warm and cold are not proven, so
nothing here can stand beside a number in results/compare/.

Everything goes to <work>/fullday/<tag>/report.json, updated after every step, so a run that dies
keeps what it found. It refuses any work folder under a results folder, and scripts/check_compare.py
refuses the file. The CPU mask is the caller's: this never calls run_system's pin_threads, which
would widen it to the reported run's CPUs.
"""

import argparse
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compare import coldcache  # noqa: E402
from compare.durability import CLASS_B, sync_tree  # noqa: E402
from compare.layout import PINS_PATH, Layout, child_environment  # noqa: E402
from compare.queries import (NANOS_PER_SECOND, OPEN_NS, Query, ground_truth,  # noqa: E402
                             query_to_json)
from compare.run_system import Terminated, on_terminate  # noqa: E402
from compare.schema import ColumnSet, TradingDay  # noqa: E402
from compare.systems import SYSTEM_NAMES, system_class  # noqa: E402
from compare.systems.base import RunContext  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[1]
WORKER = REPO_ROOT / "compare" / "worker.py"
THREADS = 2  # As in a reported run, so that QuestDB's worker pools are the same size.
MEASUREMENT = "iteration-only"
LABEL = "A capacity check on one whole day, not a benchmark. No number here is a reported result."
QUERIES = (Query("window", "rows", OPEN_NS, OPEN_NS + NANOS_PER_SECOND,
                 text="One second of all symbols at the open, [09:30:00, 09:30:01)."),
           Query("scan", "column_sum", 0, 0,
                 text="Full scan of one column, the count and sum of shares, over the whole day."))
QUERY_ORDER = ("scan", "window")


class StepFailed(Exception):
    """A step died, or a system refused. The report holds which and how."""


def refuse_results_folder(work):
    """No path out of here may lead into a results folder."""
    if "results" in Path(work).resolve().parts:
        raise SystemExit(f"fullday: a full-day run stays out of results folders: {work}")


def exit_fields(code):
    return ({"code": code, "signal": None} if code >= 0
            else {"code": None, "signal": signal.Signals(-code).name})


def cpu_flag_counts():
    flags = next(line for line in Path("/proc/cpuinfo").read_text(encoding="ascii").splitlines()
                 if line.startswith("flags")).split()
    return {"avx2": "avx2" in flags, "avx512f": "avx512f" in flags}


class FullDay:
    def __init__(self, args):
        self.args = args
        self.layout = Layout.from_arguments(args)
        refuse_results_folder(self.layout.work)
        pins = json.loads(PINS_PATH.read_text(encoding="utf-8"))
        if args.slice_smoke:
            self.pin = pins["slices"][args.slice_smoke]
            self.columns_dir = self.layout.export_dir(args.slice_smoke)
            self.day = TradingDay.from_pin(self.pin["day"])
        else:
            self.pin = pins["days"][args.day]
            self.columns_dir = self.layout.export_dir(f"{args.day}-full")
            self.day = TradingDay.from_pin(args.day)
        self.root = self.layout.work / "fullday" / "stores" / args.system
        self.run_dir = self.layout.work / "fullday" / args.tag
        self.requests = self.run_dir / "requests"
        self.system_class = system_class(args.system)
        self.variant = self.system_class.headline_variant
        self.report = {
            "measurement": MEASUREMENT, "label": LABEL, "system": args.system,
            "variant": self.variant, "durability_class": args.durability,
            "smoke_on_slice": args.slice_smoke, "expected_rows": self.pin["message_count"],
            "cpu_mask": sorted(os.sched_getaffinity(0)), "threads": THREADS,
            "machine": platform.machine(), "cpu_flags": cpu_flag_counts(),
            "commit": self.commit(), "steps": [], "outcome": "running"}

    @staticmethod
    def commit():
        marker = REPO_ROOT / "COMMIT"
        return marker.read_text(encoding="ascii").strip() if marker.exists() else "working tree"

    def save(self):
        path = self.run_dir / "report.json"
        pending = path.with_name(path.name + ".pending")
        pending.write_text(json.dumps(self.report, indent=1) + "\n", encoding="utf-8")
        os.replace(pending, path)

    def note(self, text):
        print(f"[{time.strftime('%H:%M:%S')}] {self.args.system}: {text}", flush=True)

    def context(self):
        return RunContext(self.layout, self.root, self.variant, self.args.durability, self.day,
                          THREADS, full_day=True, launcher=True, heap=self.args.heap,
                          gc_log=self.run_dir / "gc-%p.log")

    def timed(self, name, action):
        """A runner-side stage, in the report with its wall time."""
        started = time.time()
        self.note(f"{name} starts")
        result = action()
        self.report["steps"].append({"step": name, "wall_seconds": round(time.time() - started, 1),
                                     "exit": exit_fields(0)})
        self.save()
        return result

    def step(self, system, name, **fields):
        """One worker. Its wait status goes in the report before anything can fail."""
        started = time.time()
        self.note(f"{name} starts")
        request_path = self.requests / f"{len(self.report['steps']):03d}-{name}.json"
        answer_path = request_path.with_suffix(".answer")
        request = {"step": name, "system": system.name, "variant": self.variant,
                   "class": self.args.durability, "root": str(self.root),
                   "day": self.pin["day"] if self.args.slice_smoke else self.args.day,
                   "full_day": True,
                   "layout": {key: str(getattr(self.layout, key))
                              for key in ("data", "vendor", "work", "tools")}, **fields}
        request_path.write_text(json.dumps(request), encoding="utf-8")
        environment = child_environment(self.layout)
        environment.update(system.worker_environment())
        process = subprocess.Popen([sys.executable, "-I", "-X", "faulthandler", str(WORKER),
                                    str(request_path), str(answer_path)],
                                   stdin=subprocess.DEVNULL, env=environment)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
        entry = {"step": name, "wall_seconds": round(time.time() - started, 1),
                 "exit": exit_fields(process.returncode),
                 "peak_rss_bytes": usage.ru_maxrss * 1024,
                 "user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime}
        self.report["steps"].append(entry)
        if process.returncode != 0:
            self.save()
            raise StepFailed(f"{name} exited {entry['exit']}")
        answer = json.loads(answer_path.read_text(encoding="utf-8"))
        entry["answer"] = answer
        self.save()
        return answer

    def truth(self):
        """Bounded: Q1's rows and a sum in place, from the mapped files."""
        columns = ColumnSet(self.columns_dir)
        if columns.manifest["tail_bytes"] != 0:
            raise StepFailed("the export has a tail, and the table has nowhere to keep it")
        if columns.records != self.pin["message_count"]:
            raise StepFailed(f"the export holds {columns.records} records, not the pin's")
        return {query.name: ground_truth(columns, query, self.day) for query in QUERIES}

    def fresh(self):
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.parent.mkdir(parents=True, exist_ok=True)
        system = self.system_class(self.context())
        system.prepare()
        return system

    def ingest(self, system):
        try:
            self.timed("start service", system.start_service)
            self.report["settings"] = system.service_settings()
            self.step(system, "ingest_stream", columns=str(self.columns_dir))
            self.report["settle"] = self.timed("settle", system.settle)
            self.report["facts"] = {**system.service_facts(), "requested": system.requested(),
                                    "versions": system.versions()}
        finally:
            self.report["service_after_ingest"] = system.stop_service()
            self.save()

    def size(self, system):
        sync_tree(self.root)
        stored = coldcache.allocated_bytes(system.data_paths())
        stored["store_root_allocated_bytes"] = coldcache.allocated_bytes(
            [self.root])["allocated_bytes"]
        stored["ratio_stored_over_source"] = stored["allocated_bytes"] / self.pin.get(
            "decompressed_size", self.pin.get("size"))
        self.report["stored"] = stored

    def query(self, system, truth):
        by_name = {query.name: query for query in QUERIES}
        try:
            self.timed("start service", system.start_service)
            for name in QUERY_ORDER:
                answer = self.step(system, "capacity_query", query=query_to_json(by_name[name]),
                                   truth=truth[name])
                if answer["rows"] != (truth[name]["rows"] if name == "window"
                                      else self.pin["message_count"]):
                    raise StepFailed(f"{name} counted {answer['rows']} rows")
                self.report[f"{name}_rows"] = answer["rows"]
            self.step(system, "stored_settings")
        finally:
            self.report["service_after_queries"] = system.stop_service()
            self.save()

    def keep_server_logs(self):
        log = self.root / "log"
        if log.is_dir():
            shutil.copytree(log, self.run_dir / "server-log", dirs_exist_ok=True)
        self.report["hs_err_files"] = [path.name for path in self.root.rglob("hs_err*")]

    def run(self):
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.requests.mkdir(exist_ok=True)
        self.save()
        code = 1
        try:
            truth = self.timed("ground truth", self.truth)
            system = self.fresh()
            self.ingest(system)
            self.timed("size at rest", lambda: self.size(system))
            self.query(system, truth)
            self.report["outcome"] = "finished"
            code = 0
        except Terminated as terminated:
            self.report["outcome"] = f"stopped: {terminated}"
        except Exception as failure:  # noqa: BLE001 A failed system is the finding: write it down.
            self.report["outcome"] = f"failed: {type(failure).__name__}: {failure}"
            print(self.report["outcome"], file=sys.stderr, flush=True)
        finally:
            self.keep_server_logs()
            self.save()
        return code


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--system", required=True, choices=SYSTEM_NAMES)
    parser.add_argument("--tag", help="the run's folder name, by default the system's")
    parser.add_argument("--day", default="12302019")
    parser.add_argument("--slice-smoke", metavar="SLICE",
                        help="the same path on a slice, to prove the path before a day")
    parser.add_argument("--durability", default=CLASS_B)
    parser.add_argument("--heap", default="1g", help="QuestDB's -Xms and -Xmx, or `default`")
    for name in ("data", "vendor", "work", "tools"):
        parser.add_argument(f"--{name}", type=Path)
    args = parser.parse_args(argv)
    args.tag = args.tag or args.system
    return args


def main(argv):
    if platform.system() != "Linux":
        raise SystemExit("fullday: Linux only, and never on the Mac: it loads a whole day")
    signal.signal(signal.SIGTERM, on_terminate)
    return FullDay(parse_arguments(argv)).run()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
