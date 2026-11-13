"""Warm and cold, proven. Wraps tools/tick_coldcache and the device counters.

A warm number counts only if the files are resident before and after the run. A cold number
counts only if eviction was proven and the device read at least what became resident. These
functions raise rather than return a number they cannot back, so an unproven result never
reaches the results file.
"""

import ctypes
import json
import os
import platform
import resource
import subprocess
import threading
import time
from pathlib import Path

IOSTAT_DEVICE = "disk0"
# iostat's total has two decimals of a MiB, so a delta can be off by this much.
IOSTAT_SLACK_BYTES = 2 * 10_486
# Something else reading a store refills it as fast as it is evicted: on debian, the host's
# hourly backup read a new day's store, over several minutes. An eviction waits that out,
# retrying 5 s apart and doubling to 60 s, for at most this long.
EVICTION_WAIT_SECONDS = 1800
EVICTION_RETRY_SECONDS = (5, 60)
# With nothing of ours running, a store that gains a page within this long is being read by
# something else.
QUIET_SECONDS = 1.0


class ProofFailed(Exception):
    """A warm or cold claim that the evidence does not support."""


def _tool(layout, command, paths):
    run = subprocess.run([str(layout.tool("tick_coldcache")), command, *map(str, paths)],
                         check=False, capture_output=True, text=True)
    if run.returncode not in (0, 1):
        raise ProofFailed(f"tick_coldcache {command}: {run.stderr.strip()}")
    return run.returncode, json.loads(run.stdout)


def residency(layout, paths):
    return _tool(layout, "residency", paths)[1]


def dirty_by_file(layout, paths):
    """{path relative to its root: dirty pages}, or None if the host cannot count them.

    Each file gets its own call, so this reads a store of many files in a few milliseconds
    per file, well inside the writeback interval. A file with no dirty page is left out."""
    found = {}
    for root in paths:
        root = Path(root)
        members = [root] if root.is_file() else sorted(root.rglob("*"))
        for path in members:
            if path.is_symlink() or not path.is_file():
                continue
            dirty = residency(layout, [path]).get("dirty_pages")
            if dirty is None:
                return None
            if dirty:
                found[path.name if path == root else str(path.relative_to(root))] = dirty
    return found


def warm(layout, paths):
    return _tool(layout, "warm", paths)[1]


def cache_holders(layout, roots, leave_out=(), largest=8):
    """[(path, resident pages)], largest first and at most `largest` of them: what holds the
    page cache under `roots`, when a file cannot be made resident. A file directly under a root
    counts alone; a file deeper down counts with its directory, so the day's export or a store
    is one entry. A symlink is read at its target. `leave_out` and anything already seen are
    skipped, and nothing with no resident page is listed. One call per file, milliseconds each."""
    skip = {Path(path).resolve() for path in leave_out}
    found = {}
    for root in roots:
        root = Path(root)
        members = [root] if root.is_file() else sorted(root.rglob("*")) if root.is_dir() else []
        for path in members:
            target = path.resolve()
            if target in skip or not target.is_file():
                continue
            skip.add(target)
            pages = residency(layout, [target])["resident_pages"]
            if pages:
                holder = str(path if path.parent == root else path.parent)
                found[holder] = found.get(holder, 0) + pages
    return sorted(found.items(), key=lambda item: -item[1])[:largest]


def evict(layout, paths, wait_seconds=EVICTION_WAIT_SECONDS, sleep=time.sleep,
          clock=time.monotonic):
    """Returns the tool's proof, or raises if pages stay resident.

    Proven means no page resident after the tool's passes and none back QUIET_SECONDS later,
    with nothing of ours running. A store that refills is being read by something else, and
    the whole eviction is tried again, for at most `wait_seconds`. The report gains the
    attempts and the seconds waited."""
    started = clock()
    delay, longest = EVICTION_RETRY_SECONDS
    attempts = 0
    while True:
        attempts += 1
        code, report = _tool(layout, "evict", paths)
        if code == 0 and report["proven"] and report["resident_pages"] == 0:
            sleep(QUIET_SECONDS)
            back = residency(layout, paths)["resident_pages"]
            if back == 0:
                return {**report, "attempts": attempts,
                        "waited_seconds": round(clock() - started, 1)}
            failure = (f"{back} pages back {QUIET_SECONDS} s after eviction, with nothing of "
                       f"ours running")
        else:
            failure = (f"{report['resident_pages']} pages resident after {report['passes']} "
                       f"passes, in {report.get('files_with_resident_pages')}")
        if clock() - started >= wait_seconds:
            raise ProofFailed(f"eviction not proven: {failure}, after {attempts} attempts over "
                              f"{round(clock() - started)} s")
        sleep(delay)
        delay = min(delay * 2, longest)


def refilled_pages(layout, paths, sleep=time.sleep):
    """Pages a store gains over QUIET_SECONDS with nothing of ours running: more than 0 means
    something else is reading it, and a cold repetition just run beside that is not cold."""
    before = residency(layout, paths)["resident_pages"]
    sleep(QUIET_SECONDS)
    return max(0, residency(layout, paths)["resident_pages"] - before)


def proof_of_warm(before, after):
    """Both residency reports must read 100%, over the same pages."""
    if before["pages"] == 0:
        raise ProofFailed("warm: no pages to prove")
    for label, report in (("before", before), ("after", after)):
        if report["resident_pages"] != report["pages"]:
            raise ProofFailed(f"warm: {report['resident_pages']} of {report['pages']} pages "
                              f"resident {label} the run")
    return {"method": "mincore on a fresh mapping of every store file",
            "pages": after["pages"], "resident_before": before["resident_pages"],
            "resident_after": after["resident_pages"]}


def proof_of_cold(evicted, after, device_delta):
    """`device_delta` is {"bytes", "counter", "note"}. Pages resident after the query came off
    the device, since eviction left none, so the machine-wide counter must reach them. A
    shortfall is conclusive. An excess is not, since the counter is machine-wide."""
    if evicted["resident_pages"] != 0:
        raise ProofFailed("cold: pages were resident before the run")
    expected = after["resident_pages"] * after["page_size"]
    if device_delta["bytes"] is None:
        raise ProofFailed("cold: no device counter on this host")
    if device_delta["bytes"] + device_delta.get("slack", 0) < expected:
        raise ProofFailed(f"cold: the device read {device_delta['bytes']} bytes, and "
                          f"{expected} bytes became resident")
    return {"evicted_pages": evicted["pages"], "resident_after_eviction": 0,
            "eviction_passes": evicted["passes"], "method": evicted["method"],
            "eviction_attempts": evicted.get("attempts", 1),
            "eviction_waited_seconds": evicted.get("waited_seconds", 0),
            "resident_bytes_after_run": expected, "device_bytes_read": device_delta["bytes"],
            "device_counter": device_delta["counter"],
            "device_slack": device_delta.get("slack", 0),
            "metadata": "stays warm: neither method reaches file system metadata"}


def meminfo_bytes(field):
    """A /proc/meminfo field in bytes, or None where there is no such file (macOS)."""
    if platform.system() != "Linux":
        return None
    for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
        if line.startswith(f"{field}:"):
            return int(line.split()[1]) * 1024
    return None


def _iostat_megabytes():
    text = subprocess.run(["iostat", "-Id", IOSTAT_DEVICE], check=True, capture_output=True,
                          text=True).stdout
    return float(text.strip().splitlines()[-1].split()[2])


def _block_device_stat(path):
    """The stat file of the block device under `path`. A file system with no block device of
    its own, such as tmpfs, overlayfs or a btrfs subvolume, has none (review RF-11)."""
    device = os.stat(path).st_dev
    number = f"{os.major(device)}:{os.minor(device)}"
    stat = Path(f"/sys/dev/block/{number}/stat")
    if not stat.exists():
        raise ProofFailed(f"{path} is on device {number}, which has no {stat}, so its device "
                          "reads cannot be counted: put it on a file system with a block "
                          "device of its own")
    return stat


class DeviceCounter:
    """Bytes the device under `path` has moved since this was made.

    macOS: iostat's total for disk0, reads and writes together. Linux: sectors read, from the
    block device's own stat file. Either way a shortfall is conclusive and an excess is not.
    """

    def __init__(self, path):
        self.macos = platform.system() == "Darwin"
        self.stat_path = None if self.macos else _block_device_stat(path)
        self.start = self._read()

    def _read(self):
        if self.macos:
            return round(_iostat_megabytes() * (1 << 20))
        fields = self.stat_path.read_text(encoding="ascii").split()
        return int(fields[2]) * 512

    def delta(self):
        return {"bytes": self._read() - self.start,
                "counter": (f"iostat's total for {IOSTAT_DEVICE}, MiB read and written, machine-wide"
                            if self.macos else "block device stat, sectors read, machine-wide"),
                "slack": IOSTAT_SLACK_BYTES if self.macos else 0}


class _RusageInfoV2(ctypes.Structure):
    """<sys/resource.h>'s struct rusage_info_v2, which proc_pid_rusage fills."""

    _fields_ = [("ri_uuid", ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64) for name in (
            "ri_user_time", "ri_system_time", "ri_pkg_idle_wkups", "ri_interrupt_wkups",
            "ri_pageins", "ri_wired_size", "ri_resident_size", "ri_phys_footprint",
            "ri_proc_start_abstime", "ri_proc_exit_abstime", "ri_child_user_time",
            "ri_child_system_time", "ri_child_pkg_idle_wkups", "ri_child_interrupt_wkups",
            "ri_child_pageins", "ri_child_elapsed_abstime", "ri_diskio_bytesread",
            "ri_diskio_byteswritten")]


RUSAGE_INFO_V2 = 2
_LIBPROC = []


def disk_bytes_read(pid):
    """macOS: the bytes the device has read for process `pid`, ri_diskio_bytesread from
    proc_pid_rusage, page-ins of a mapping and read-ahead included; a resident page reads
    nothing. Any process of this user, and one that has exited but is not reaped yet."""
    if not _LIBPROC:
        library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
        library.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        _LIBPROC.append(library)
    info = _RusageInfoV2()
    if _LIBPROC[0].proc_pid_rusage(int(pid), RUSAGE_INFO_V2, ctypes.byref(info)) != 0:
        raise ProofFailed(f"no I/O account for {pid}: proc_pid_rusage failed, errno "
                          f"{ctypes.get_errno()}")
    return info.ri_diskio_bytesread


# macOS adds no reaped child's device reads to its parent's, so run_counting_reads keeps the sum
# for the children it ran, each read once the child exited and before it was reaped.
_REAPED_CHILDREN = {"bytes_read": 0}


def run_counting_reads(command, **options):
    """subprocess.run(command, capture_output=True, text=True, check=False, **options). On
    macOS the child's device reads are read once it has exited, before it is reaped, and added
    to the sum ProcessDiskReads counts as `waited children`: Linux counts those in ru_inblock,
    and macOS nowhere. Linux runs it as before."""
    if platform.system() != "Darwin":
        return subprocess.run(command, capture_output=True, text=True, check=False, **options)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, **options)
    errors = []
    drain = threading.Thread(target=lambda: errors.append(process.stderr.read()))
    drain.start()
    output = process.stdout.read()
    drain.join()
    os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOWAIT)
    try:
        _REAPED_CHILDREN["bytes_read"] += disk_bytes_read(process.pid)
    finally:
        process.wait()
        process.stdout.close()
        process.stderr.close()
    return subprocess.CompletedProcess(command, process.returncode, output, errors[0])


class ProcessDiskReads:
    """macOS: bytes the device read since this was made for the processes that did a write:
    this worker, each process named in `processes` ({role: pid}, a server), and each child the
    worker ran through run_counting_reads (review RF-5). iostat's disk0 total, the cold proof's
    counter, also counts the write's own output, so it is no count of reads."""

    COUNTER = ("bytes the device read for the processes that did the write: proc_pid_rusage's "
               "ri_diskio_bytesread of the worker, of each server process, and of each child "
               "the worker ran, read before it was reaped")

    def __init__(self, processes):
        self.processes = {"worker": os.getpid(), **processes}
        self.start = self._read()

    def _read(self):
        found = {role: disk_bytes_read(pid) for role, pid in self.processes.items()}
        found["waited children"] = _REAPED_CHILDREN["bytes_read"]
        return found

    def delta(self):
        now = self._read()
        by_process = {role: now[role] - self.start[role] for role in now}
        return {"bytes": sum(by_process.values()), "by_process": by_process,
                "counter": self.COUNTER, "slack": 0}


def write_counter(path, processes):
    """The device reads over a write: on Linux the block device's sectors read under `path`,
    machine-wide, as before; on macOS the reads of the processes that did it."""
    if platform.system() == "Darwin":
        return ProcessDiskReads(processes)
    return DeviceCounter(path)


def _process_read_bytes(pid):
    """read_bytes from /proc/<pid>/io: what the process caused to be fetched from storage,
    every thread of it, page faults on a mapping and readahead included."""
    try:
        text = Path(f"/proc/{pid}/io").read_text(encoding="ascii")
    except OSError as error:
        raise ProofFailed(f"no I/O account for {pid}: {error}") from error
    for line in text.splitlines():
        if line.startswith("read_bytes:"):
            return int(line.split()[1])
    raise ProofFailed(f"/proc/{pid}/io has no read_bytes")


class ProcessReads:
    """Bytes that the processes answering a query caused to be read from storage since this
    was made: this process, each process named in `processes` ({role: pid}, a query tool or a
    server), and every child this process has waited for. Linux only.

    The block device's counter is machine-wide, so on a shared host another process's read
    fails an attempt that read nothing. This counts only the query's own processes, and the
    warm proof records the machine-wide count beside it."""

    COUNTER = ("read_bytes in /proc/<pid>/io of the worker and of each process that answers its "
               "queries, and ru_inblock of the children it waited for: storage reads those "
               "processes caused, page faults and readahead included")

    def __init__(self, processes):
        self.processes = {"worker": "self", **processes}
        self.start = self._read()

    def _read(self):
        found = {role: _process_read_bytes(pid) for role, pid in self.processes.items()}
        found["waited children"] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_inblock * 512
        return found

    def delta(self):
        now = self._read()
        by_process = {role: now[role] - self.start[role] for role in now}
        return {"bytes": sum(by_process.values()), "by_process": by_process,
                "counter": self.COUNTER}


def allocated_bytes(paths):
    """Allocated size at rest, from st_blocks, with a hard-linked file counted once."""
    seen = set()
    total = 0
    apparent = 0
    files = 0
    for root in paths:
        root = Path(root)
        members = [root] if root.is_file() else root.rglob("*")
        for path in members:
            if path.is_symlink() or not path.is_file():
                continue
            info = path.stat()
            key = (info.st_dev, info.st_ino)
            if key in seen:
                continue
            seen.add(key)
            total += info.st_blocks * 512
            apparent += info.st_size
            files += 1
    return {"allocated_bytes": total, "apparent_bytes": apparent, "files": files,
            "method": "sum of st_blocks * 512 over every file, each inode once"}
