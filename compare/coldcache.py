"""Warm and cold, proven. Wraps tools/tick_coldcache and the device counters.

A warm number counts only if the files are resident before and after the run. A cold number
counts only if eviction was proven and the device read at least what became resident. These
functions raise rather than return a number they cannot back, so an unproven result never
reaches the results file.
"""

import json
import os
import platform
import resource
import subprocess
from pathlib import Path

IOSTAT_DEVICE = "disk0"
# iostat's total has two decimals of a MiB, so a delta can be off by this much.
IOSTAT_SLACK_BYTES = 2 * 10_486


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


def evict(layout, paths):
    """Returns the tool's proof, or raises if any page stayed resident."""
    code, report = _tool(layout, "evict", paths)
    if code != 0 or not report["proven"] or report["resident_pages"] != 0:
        raise ProofFailed(f"eviction not proven: {report['resident_pages']} pages resident after "
                          f"{report['passes']} passes, in "
                          f"{report.get('files_with_resident_pages')}")
    return report


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
    device = os.stat(path).st_dev
    return Path(f"/sys/dev/block/{os.major(device)}:{os.minor(device)}/stat")


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
