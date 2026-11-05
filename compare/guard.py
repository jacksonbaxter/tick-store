#!/usr/bin/env python3
"""Runs one command in a session of its own, and stops it before it can hurt the host.

    python3 -I compare/guard.py --report <file> --samples <file> --watch <dir> -- <command...>

For a full-day run on a shared machine, whose services matter more than the run. Every two
seconds this reads MemAvailable, swap traffic, free disk and each process of the session. Under
a limit it sends the session SIGTERM, then SIGKILL, and writes that down as the result. It
touches nothing outside the session. The report gives the command's wait status with its signal
named, and each process's peak memory by kind, and appears again every 30 seconds, so a run that
stops early still leaves its evidence. Linux only. Exits with the command's code, or 70 if the
guard fired.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

GIB = 1 << 30
PAGE_BYTES = 4096
SAMPLE_SECONDS = 2
REPORT_EVERY_SAMPLES = 15
TERM_GRACE_SECONDS = 90
SWAP_STRIKES = 3  # Consecutive samples over the limit, so that one burst does not fire it.
EXIT_GUARD_FIRED = 70
STATUS_FIELDS = ("VmHWM", "VmRSS", "RssAnon", "RssFile", "RssShmem", "VmSwap")


@dataclass(frozen=True)
class Limits:
    min_available_bytes: int = 3 * GIB
    hard_available_bytes: int = 2 * GIB  # Under this, a stopping job gets no more grace.
    min_free_disk_bytes: int = 50 * GIB
    max_swap_bytes_per_second: int = 10 * (1 << 20)


@dataclass(frozen=True)
class HostSample:
    available_bytes: int
    swap_free_bytes: int
    swapped_pages: int  # pswpin + pswpout since boot.
    free_disk_bytes: int


def parse_meminfo(text):
    """{field: bytes} for the kB fields of /proc/meminfo."""
    fields = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].endswith(":"):
            fields[parts[0][:-1]] = int(parts[1]) * 1024
    return fields


def parse_swap_pages(vmstat_text):
    counters = dict(line.split() for line in vmstat_text.splitlines())
    return int(counters["pswpin"]) + int(counters["pswpout"])


def parse_status(text):
    """The memory fields of /proc/<pid>/status, in bytes. A kernel thread has none."""
    found = {}
    for line in text.splitlines():
        name, _, rest = line.partition(":")
        if name in STATUS_FIELDS:
            found[name] = int(rest.split()[0]) * 1024
    return found


def session_of(stat_text):
    """The session from /proc/<pid>/stat. A comm can hold spaces and brackets."""
    return int(stat_text.rsplit(")", 1)[1].split()[3])


def breach(sample, swap_bytes_per_second, swap_strikes, limits):
    """Why the job must stop, or None, with the strikes to carry to the next sample."""
    strikes = swap_strikes + 1 if swap_bytes_per_second > limits.max_swap_bytes_per_second else 0
    if sample.available_bytes < limits.min_available_bytes:
        return f"MemAvailable under {limits.min_available_bytes / GIB:g} GiB", strikes
    if sample.free_disk_bytes < limits.min_free_disk_bytes:
        return f"free disk under {limits.min_free_disk_bytes / GIB:g} GiB", strikes
    if strikes >= SWAP_STRIKES:
        return f"swap traffic over {limits.max_swap_bytes_per_second} bytes/s", strikes
    return None, strikes


def wait_status_fields(code):
    """A subprocess returncode as {"code"} or {"signal"}, with the signal named."""
    if code >= 0:
        return {"code": code, "signal": None}
    return {"code": None, "signal": signal.Signals(-code).name}


def read_host(watch):
    memory = parse_meminfo(Path("/proc/meminfo").read_text(encoding="ascii"))
    disk = os.statvfs(watch)
    return HostSample(memory["MemAvailable"], memory["SwapFree"],
                      parse_swap_pages(Path("/proc/vmstat").read_text(encoding="ascii")),
                      disk.f_bavail * disk.f_frsize)


def session_members(session):
    members = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if session_of((entry / "stat").read_text(encoding="ascii", errors="replace")) == session:
                members.append(int(entry.name))
        except (OSError, IndexError, ValueError):
            continue  # It went between the listing and the read.
    return members


def signal_session(session, number):
    for pid in session_members(session):
        try:
            os.kill(pid, number)
        except OSError:
            pass


class Guard:
    def __init__(self, args):
        self.args = args
        self.limits = Limits(min_available_bytes=int(args.min_available_gib * GIB),
                             hard_available_bytes=int(args.min_available_gib * GIB * 2 / 3),
                             min_free_disk_bytes=int(args.min_free_disk_gib * GIB))
        self.report = {"command": args.command, "sample_seconds": SAMPLE_SECONDS,
                       "limits": vars(self.limits), "guard_fired": False, "guard_reason": None,
                       "exit": None, "processes": {}}
        self.lowest_available = None
        self.lowest_disk = None
        self.first = None
        self.last = None
        self.strikes = 0
        self.fired_at = None
        self.asked_to_stop = None
        self.samples_taken = 0
        self.session_rss_peak = 0

    def on_signal(self, number, frame):
        self.asked_to_stop = signal.Signals(number).name

    def note_processes(self, session):
        """Peak of each field per process, keyed by pid and name, and their sum now."""
        total = 0
        for pid in session_members(session):
            try:
                status = parse_status(Path(f"/proc/{pid}/status").read_text(encoding="ascii"))
                name = Path(f"/proc/{pid}/comm").read_text(encoding="ascii").strip()
            except OSError:
                continue
            total += status.get("VmRSS", 0)
            peaks = self.report["processes"].setdefault(f"{pid} {name}", {})
            for field, value in status.items():
                peaks[field] = max(peaks.get(field, 0), value)
        return total

    def take_sample(self, session, samples, elapsed):
        sample = read_host(self.args.watch)
        rate = 0.0
        if self.last is not None:
            rate = (sample.swapped_pages - self.last.swapped_pages) * PAGE_BYTES / SAMPLE_SECONDS
        self.first = self.first or sample
        self.last = sample
        reason, self.strikes = breach(sample, rate, self.strikes, self.limits)
        self.lowest_available = min(self.lowest_available or sample.available_bytes,
                                    sample.available_bytes)
        self.lowest_disk = min(self.lowest_disk or sample.free_disk_bytes, sample.free_disk_bytes)
        rss = self.note_processes(session)
        self.session_rss_peak = max(self.session_rss_peak, rss)
        samples.write(f"{elapsed:.1f},{sample.available_bytes},{sample.swap_free_bytes},"
                      f"{rate:.0f},{sample.free_disk_bytes},{rss}\n")
        samples.flush()
        return reason

    def write_report(self, started):
        self.report.update({
            "wall_seconds": round(time.monotonic() - started, 1),
            "mem_available_start_bytes": self.first and self.first.available_bytes,
            "mem_available_lowest_bytes": self.lowest_available,
            "swap_free_start_bytes": self.first and self.first.swap_free_bytes,
            "swap_free_last_bytes": self.last and self.last.swap_free_bytes,
            "swapped_pages_during_run": (self.last.swapped_pages - self.first.swapped_pages
                                         if self.first else None),
            "free_disk_start_bytes": self.first and self.first.free_disk_bytes,
            "free_disk_lowest_bytes": self.lowest_disk,
            "session_rss_bytes_peak": self.session_rss_peak,
        })
        pending = self.args.report.with_name(self.args.report.name + ".pending")
        pending.write_text(json.dumps(self.report, indent=1) + "\n", encoding="utf-8")
        os.replace(pending, self.args.report)

    def stop_job(self, session, reason, started):
        """SIGTERM once, and SIGKILL when the grace ends or memory reaches the hard floor."""
        self.report.update({"guard_fired": True, "guard_reason": reason,
                            "guard_fired_at_seconds": round(time.monotonic() - started, 1)})
        self.fired_at = time.monotonic()
        signal_session(session, signal.SIGTERM)

    def overdue(self):
        late = time.monotonic() - self.fired_at > TERM_GRACE_SECONDS
        return late or self.last.available_bytes < self.limits.hard_available_bytes

    def run(self):
        signal.signal(signal.SIGTERM, self.on_signal)
        signal.signal(signal.SIGINT, self.on_signal)
        started = time.monotonic()
        process = subprocess.Popen(self.args.command, stdin=subprocess.DEVNULL,
                                   start_new_session=True)
        session = process.pid
        with open(self.args.samples, "w", encoding="ascii") as samples:
            samples.write("seconds,mem_available,swap_free,swap_bytes_per_s,free_disk,session_rss\n")
            while process.poll() is None:
                elapsed = time.monotonic() - started
                reason = self.take_sample(session, samples, elapsed)
                if self.asked_to_stop:
                    reason = f"asked to stop by {self.asked_to_stop}"
                if self.fired_at is None and reason:
                    self.stop_job(session, reason, started)
                elif self.fired_at is not None and self.overdue():
                    signal_session(session, signal.SIGKILL)
                self.samples_taken += 1
                if self.samples_taken % REPORT_EVERY_SAMPLES == 0:
                    self.write_report(started)
                try:
                    process.wait(SAMPLE_SECONDS)
                except subprocess.TimeoutExpired:
                    pass
        self.report["exit"] = wait_status_fields(process.returncode)
        self.report["left_behind"] = self.sweep(session)
        self.write_report(started)
        if self.report["guard_fired"]:
            return EXIT_GUARD_FIRED
        code = process.returncode
        return code if code >= 0 else 128 - code

    def sweep(self, session):
        """Whatever the command left in its session: a server whose parent died, say."""
        left = session_members(session)
        if left:
            signal_session(session, signal.SIGTERM)
            deadline = time.monotonic() + TERM_GRACE_SECONDS
            while session_members(session) and time.monotonic() < deadline:
                time.sleep(0.5)
            signal_session(session, signal.SIGKILL)
        return left


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--watch", type=Path, required=True, help="a path on the disk to watch")
    parser.add_argument("--min-available-gib", type=float, default=3.0)
    parser.add_argument("--min-free-disk-gib", type=float, default=50.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    args.command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not args.command:
        parser.error("no command given")
    return args


if __name__ == "__main__":
    sys.exit(Guard(parse_arguments(sys.argv[1:])).run())
