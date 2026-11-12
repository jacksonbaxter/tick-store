"""The host's swap counters, in pages, and their change over a phase of a run.

Linux reads pswpin and pswpout from /proc/vmstat, macOS vm_stat's Swapins and Swapouts. Both
count since boot, for the whole host: a change says how many pages went in or out while a phase
ran, not whose they were. A timed query with any page swapped in is flagged, never rerun or
refused: see "The guard's swap rule" in docs/NOTES.md.

Standard library only, so that compare/guard.py can use it.
"""

import os
import platform
import re
import subprocess

LINUX_SOURCE = "pswpin and pswpout in the kernel's vmstat, proc/vmstat"
MACOS_SOURCE = "vm_stat Swapins and Swapouts"
METHOD = ("Host-wide counts of pages swapped in and out, read before and after each phase: "
          "write, read-back, warm queries and cold queries, for each variant and class. Each "
          "query's swap_timed is read just before and after its timed repetitions alone. "
          "A count says how many pages moved while the phase ran, not whose they were.")
TIMED_POLICY = ("A timed query with any page swapped in is flagged, the same way for every "
                "system: its number is kept and marked, and a verdict that rests on it says so. "
                "It is neither rerun nor refused, since a host-wide count cannot say whose page "
                "came back.")


def parse_vmstat(text):
    """(pages in, pages out) from /proc/vmstat."""
    counters = dict(line.split() for line in text.splitlines() if line.strip())
    return int(counters["pswpin"]), int(counters["pswpout"])


def parse_vm_stat(text):
    """(pages in, pages out, page bytes) from macOS vm_stat."""
    size = re.search(r"page size of (\d+) bytes", text)
    found = {name: re.search(rf"^{name}:\s+(\d+)\.?\s*$", text, re.MULTILINE)
             for name in ("Swapins", "Swapouts")}
    if size is None or None in found.values():
        raise ValueError("vm_stat printed no page size, Swapins or Swapouts")
    return int(found["Swapins"][1]), int(found["Swapouts"][1]), int(size[1])


def source():
    return MACOS_SOURCE if platform.system() == "Darwin" else LINUX_SOURCE


def page_bytes():
    if platform.system() == "Darwin":
        return read()[2]
    return os.sysconf("SC_PAGE_SIZE")


def read():
    """(pages in, pages out, page bytes) since boot."""
    if platform.system() == "Darwin":
        text = subprocess.run(["/usr/bin/vm_stat"], check=True, capture_output=True,
                              text=True).stdout
        return parse_vm_stat(text)
    with open("/proc/vmstat", encoding="ascii") as vmstat:
        pages_in, pages_out = parse_vmstat(vmstat.read())
    return pages_in, pages_out, os.sysconf("SC_PAGE_SIZE")


def counts(before, after):
    return {"pages_in": after[0] - before[0], "pages_out": after[1] - before[1]}


def add(*blocks):
    """The sum of several phases' counts."""
    return {"pages_in": sum(block["pages_in"] for block in blocks),
            "pages_out": sum(block["pages_out"] for block in blocks)}


def timed(blocks):
    """The counts over a query's timed runs, flagged if any page came back from swap."""
    total = add(*blocks)
    return {**total, "flagged": total["pages_in"] > 0}


class Phase:
    """Phase().start() ... .stop(), or a with block: the counts are in .counts afterwards."""

    def __init__(self):
        self.before = None
        self.counts = None

    def start(self):
        self.before = read()
        return self

    def stop(self):
        self.counts = counts(self.before, read())
        return self.counts

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        return False


def method_block():
    return {"source": source(), "page_bytes": page_bytes(), "scope": "host-wide",
            "method": METHOD, "timed_policy": TIMED_POLICY}
