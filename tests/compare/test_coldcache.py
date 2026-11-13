#!/usr/bin/env python3
"""compare/coldcache.py: warm and cold claims refused without their evidence. Standard
library only.

    python3 -I tests/compare/test_coldcache.py
"""

import os
import platform
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from compare import coldcache  # noqa: E402
from compare.coldcache import ProofFailed, allocated_bytes, proof_of_cold, proof_of_warm  # noqa: E402
from compare.layout import Layout  # noqa: E402

PAGE = 4096
EVICTED = {"pages": 10, "resident_pages": 0, "passes": 1, "method": "m"}
AFTER = {"pages": 10, "resident_pages": 4, "page_size": PAGE}


def device(count, slack=0):
    return {"bytes": count, "counter": "c", "slack": slack}


class Warm(unittest.TestCase):
    def test_full_residency_both_sides_passes(self):
        report = {"pages": 8, "resident_pages": 8}
        self.assertEqual(proof_of_warm(report, report)["pages"], 8)

    def test_a_page_missing_before_fails(self):
        with self.assertRaisesRegex(ProofFailed, "before"):
            proof_of_warm({"pages": 8, "resident_pages": 7}, {"pages": 8, "resident_pages": 8})

    def test_a_page_missing_after_fails(self):
        with self.assertRaisesRegex(ProofFailed, "after"):
            proof_of_warm({"pages": 8, "resident_pages": 8}, {"pages": 8, "resident_pages": 7})

    def test_nothing_to_prove_fails(self):
        with self.assertRaisesRegex(ProofFailed, "no pages"):
            proof_of_warm({"pages": 0, "resident_pages": 0}, {"pages": 0, "resident_pages": 0})


class Cold(unittest.TestCase):
    def test_a_device_read_reaching_the_resident_bytes_passes(self):
        proof = proof_of_cold(EVICTED, AFTER, device(4 * PAGE))
        self.assertEqual(proof["resident_after_eviction"], 0)

    def test_a_resident_page_after_eviction_fails(self):
        with self.assertRaisesRegex(ProofFailed, "before the run"):
            proof_of_cold({**EVICTED, "resident_pages": 1}, AFTER, device(4 * PAGE))

    def test_a_device_shortfall_fails(self):
        with self.assertRaisesRegex(ProofFailed, "device read"):
            proof_of_cold(EVICTED, AFTER, device(4 * PAGE - 1))

    def test_the_counters_rounding_is_allowed_for(self):
        proof_of_cold(EVICTED, AFTER, device(4 * PAGE - 1, slack=1))

    def test_no_counter_fails(self):
        with self.assertRaisesRegex(ProofFailed, "no device counter"):
            proof_of_cold(EVICTED, AFTER, {"bytes": None, "counter": "c"})


class Clock:
    """time.monotonic and time.sleep for a test: sleeping moves the clock."""

    def __init__(self):
        self.now = 0.0
        self.slept = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.now += seconds


def tool_reports(*reports):
    """A fake coldcache._tool: each call returns the next report, evict and residency alike."""
    queue = list(reports)
    calls = []

    def tool(layout, command, paths):
        calls.append(command)
        report = queue.pop(0)
        return (0 if report.get("proven", True) else 1), report

    return tool, calls


def evicted(resident=0, proven=None):
    return {"pages": 10, "resident_pages": resident, "passes": 32,
            "proven": resident == 0 if proven is None else proven, "method": "m",
            "files_with_resident_pages": [{"file": "seq.d", "resident_pages": resident}]}


def resident(pages):
    return {"pages": 10, "resident_pages": pages}


class Evict(unittest.TestCase):
    """An eviction is proven only if no page is resident after the tool's passes and none is
    back a moment later with nothing of ours running. Something else reading the store, as
    debian's hourly backup read a new day's store, is waited out, for a bounded time."""

    def evict(self, *reports, wait_seconds=coldcache.EVICTION_WAIT_SECONDS):
        tool, calls = tool_reports(*reports)
        clock = Clock()
        with mock.patch.object(coldcache, "_tool", tool):
            report = coldcache.evict(None, ["store"], wait_seconds=wait_seconds,
                                     sleep=clock.sleep, clock=clock)
        return report, calls, clock

    def test_a_quiet_store_is_proven_at_once(self):
        report, calls, clock = self.evict(evicted(), resident(0))
        self.assertEqual(calls, ["evict", "residency"])
        self.assertEqual((report["attempts"], report["resident_pages"]), (1, 0))
        self.assertEqual(clock.slept, [coldcache.QUIET_SECONDS])

    def test_a_store_refilling_after_eviction_is_not_proven(self):
        report, calls, clock = self.evict(evicted(), resident(26_000), evicted(), resident(0))
        self.assertEqual(calls, ["evict", "residency", "evict", "residency"])
        self.assertEqual(report["attempts"], 2)
        self.assertEqual(clock.slept, [coldcache.QUIET_SECONDS, 5, coldcache.QUIET_SECONDS])

    def test_pages_left_by_the_tool_are_retried_with_a_growing_wait(self):
        report, _, clock = self.evict(evicted(27_083), evicted(13_000), evicted(9), evicted(),
                                      resident(0))
        self.assertEqual(report["attempts"], 4)
        self.assertEqual(clock.slept, [5, 10, 20, coldcache.QUIET_SECONDS])
        self.assertEqual(report["waited_seconds"], 36.0)

    def test_the_wait_is_bounded_and_the_failure_names_the_files(self):
        reports = [evicted(27_083)] * 40
        with self.assertRaisesRegex(ProofFailed, r"27083 pages resident after 32 passes, in "
                                                 r"\[\{'file': 'seq.d'.*attempts over"):
            self.evict(*reports, wait_seconds=100)

    def test_a_tool_that_says_proven_with_pages_left_is_not_believed(self):
        report, _, _ = self.evict(evicted(5, proven=True), evicted(), resident(0))
        self.assertEqual(report["attempts"], 2)

    def test_the_proof_of_cold_carries_the_attempts(self):
        report, _, _ = self.evict(evicted(), resident(3), evicted(), resident(0))
        proof = proof_of_cold(report, AFTER, device(4 * PAGE))
        self.assertEqual((proof["eviction_attempts"], proof["eviction_waited_seconds"]),
                         (2, 7.0))


class Refilled(unittest.TestCase):
    def refilled(self, before, after):
        tool, _ = tool_reports(resident(before), resident(after))
        with mock.patch.object(coldcache, "_tool", tool):
            return coldcache.refilled_pages(None, ["store"], sleep=lambda seconds: None)

    def test_a_store_gaining_pages_is_being_read_by_something_else(self):
        self.assertEqual(self.refilled(100, 13_743), 13_643)

    def test_a_store_holding_or_losing_pages_is_quiet(self):
        self.assertEqual(self.refilled(100, 100), 0)
        self.assertEqual(self.refilled(100, 40), 0)


class Allocated(unittest.TestCase):
    def test_a_hard_link_counts_once_and_a_symlink_not_at_all(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as directory:
            root = Path(directory)
            (root / "a").write_bytes(b"x" * 10000)
            os.link(root / "a", root / "b")
            os.symlink(root / "a", root / "c")
            report = allocated_bytes([root])
        self.assertEqual(report["files"], 1)
        self.assertEqual(report["apparent_bytes"], 10000)


class CacheHolders(unittest.TestCase):
    def test_what_holds_the_cache_by_file_and_directory_largest_first(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as directory:
            root = Path(directory)
            data, outside = root / "data", root / "outside"
            (data / "export" / "day").mkdir(parents=True)
            outside.mkdir()
            for path in (data / "source", data / "slice", data / "export" / "day" / "a.bin",
                         data / "export" / "day" / "b.bin", outside / "copy.gz"):
                path.write_bytes(b"x")
            os.symlink(outside / "copy.gz", data / "copy.gz")
            pages = {"source": 900, "slice": 0, "a.bin": 30, "b.bin": 50, "copy.gz": 70}

            def tool(layout, command, paths):
                self.assertEqual(command, "residency")
                return 0, {"resident_pages": pages[Path(paths[0]).name]}

            with mock.patch.object(coldcache, "_tool", tool):
                found = coldcache.cache_holders(None, [data], leave_out=[data / "source"])
        self.assertEqual(found, [(str(data / "export" / "day"), 80),
                                 (str(data / "copy.gz"), 70)])



@unittest.skipUnless(platform.system() == "Linux", "reads /proc/<pid>/io")
class ProcessReadsOnLinux(unittest.TestCase):
    """The warm proof's counter, on real reads of a file taken out of the page cache."""

    def evicted_file(self, directory):
        path = Path(directory) / "data"
        with open(path, "wb") as handle:
            handle.write(os.urandom(1 << 20))
            handle.flush()
            os.fsync(handle.fileno())
        descriptor = os.open(path, os.O_RDONLY)
        os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
        os.close(descriptor)
        return path

    def test_own_reads_count_and_cached_reads_do_not(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as directory:
            path = self.evicted_file(directory)
            reads = coldcache.ProcessReads({})
            path.read_bytes()
            first = reads.delta()
            reads = coldcache.ProcessReads({})
            path.read_bytes()
            second = reads.delta()
        self.assertGreaterEqual(first["by_process"]["worker"], 1 << 20)
        self.assertEqual(second["bytes"], 0)

    def test_a_named_process_and_a_waited_child_count(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as directory:
            path = self.evicted_file(directory)
            child = subprocess.Popen([sys.executable, "-I", "-c",
                                      "import sys; sys.stdin.readline(); "
                                      f"open({str(path)!r}, 'rb').read(); print(flush=True); "
                                      "sys.stdin.readline()"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            reads = coldcache.ProcessReads({"server": child.pid})
            child.stdin.write("\n")
            child.stdin.flush()
            child.stdout.readline()
            named = reads.delta()
            child.stdin.write("\n")
            child.stdin.close()
            child.wait()
            path = self.evicted_file(directory)
            reads = coldcache.ProcessReads({})
            subprocess.run([sys.executable, "-I", "-c", f"open({str(path)!r}, 'rb').read()"],
                           check=True)
            waited = reads.delta()
        self.assertGreaterEqual(named["by_process"]["server"], 1 << 20)
        self.assertGreaterEqual(waited["by_process"]["waited children"], 1 << 20)

    def test_a_process_gone_is_refused(self):
        child = subprocess.Popen([sys.executable, "-I", "-c", "pass"])
        child.wait()
        with self.assertRaisesRegex(ProofFailed, "no I/O account"):
            coldcache.ProcessReads({"server": child.pid})


class CountedChild(unittest.TestCase):
    """macOS adds no reaped child's device reads to its parent's, so a write's child is read
    once it has exited and before it is reaped (review RF-5). Any host, with the count faked."""

    def test_a_child_is_read_before_it_is_reaped_and_its_output_kept(self):
        seen = []

        def disk_bytes_read(pid):
            os.kill(pid, 0)  # Raises once the child is reaped.
            seen.append(pid)
            return 777

        before = coldcache._REAPED_CHILDREN["bytes_read"]
        with (mock.patch.object(coldcache.platform, "system", return_value="Darwin"),
              mock.patch.object(coldcache, "disk_bytes_read", disk_bytes_read)):
            run = coldcache.run_counting_reads(
                [sys.executable, "-I", "-c", "import sys; print('out'); "
                 "print('err', file=sys.stderr); sys.exit(3)"], stdin=subprocess.DEVNULL)
        self.assertEqual((run.returncode, run.stdout, run.stderr), (3, "out\n", "err\n"))
        self.assertEqual(len(seen), 1)
        self.assertEqual(coldcache._REAPED_CHILDREN["bytes_read"] - before, 777)

    def test_linux_runs_the_child_as_before(self):
        before = coldcache._REAPED_CHILDREN["bytes_read"]
        with (mock.patch.object(coldcache.platform, "system", return_value="Linux"),
              mock.patch.object(coldcache, "disk_bytes_read", side_effect=AssertionError)):
            run = coldcache.run_counting_reads([sys.executable, "-I", "-c", "print('out')"])
        self.assertEqual((run.returncode, run.stdout), (0, "out\n"))
        self.assertEqual(coldcache._REAPED_CHILDREN["bytes_read"], before)

    def test_the_processes_and_the_waited_children_add_up(self):
        counts = {os.getpid(): 10, 42: 20}
        with mock.patch.object(coldcache, "disk_bytes_read", lambda pid: counts[pid]):
            reads = coldcache.ProcessDiskReads({"questdb": 42})
            counts[os.getpid()] += 100
            counts[42] += 200
            coldcache._REAPED_CHILDREN["bytes_read"] += 300
            found = reads.delta()
        self.assertEqual(found["by_process"], {"worker": 100, "questdb": 200,
                                               "waited children": 300})
        self.assertEqual(found["bytes"], 600)
        self.assertIn("ri_diskio_bytesread", found["counter"])


TOOLS_BUILT = Layout.default().tool("tick_coldcache").exists()


@unittest.skipUnless(platform.system() == "Darwin" and TOOLS_BUILT,
                     "macOS's own count, evicting with tick_coldcache")
class ProcessDiskReadsOnMacos(unittest.TestCase):
    """A write's device reads on macOS, on real reads of a file taken out of the page cache."""

    def evicted_file(self, directory):
        path = Path(directory) / "data"
        path.write_bytes(os.urandom(1 << 20))
        coldcache.evict(Layout.default(), [path])
        return path

    def test_own_reads_count_and_cached_reads_do_not(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as directory:
            path = self.evicted_file(directory)
            reads = coldcache.ProcessDiskReads({})
            path.read_bytes()
            first = reads.delta()
            reads = coldcache.ProcessDiskReads({})
            path.read_bytes()
            second = reads.delta()
        self.assertGreaterEqual(first["by_process"]["worker"], 1 << 20)
        self.assertEqual(second["bytes"], 0)

    def test_a_named_process_and_a_counted_child_count_and_a_reaped_one_would_not(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        read = [sys.executable, "-I", "-c", "import sys; open(sys.argv[1], 'rb').read()"]
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as directory:
            path = self.evicted_file(directory)
            child = subprocess.Popen([sys.executable, "-I", "-c",
                                      "import sys; sys.stdin.readline(); "
                                      f"open({str(path)!r}, 'rb').read(); print(flush=True); "
                                      "sys.stdin.readline()"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            reads = coldcache.ProcessDiskReads({"server": child.pid})
            child.stdin.write("\n")
            child.stdin.flush()
            child.stdout.readline()
            named = reads.delta()
            child.stdin.write("\n")
            child.stdin.close()
            child.wait()
            child.stdout.close()
            path = self.evicted_file(directory)
            reads = coldcache.ProcessDiskReads({})
            coldcache.run_counting_reads([*read, str(path)])
            counted = reads.delta()
            path = self.evicted_file(directory)
            reads = coldcache.ProcessDiskReads({})
            subprocess.run([*read, str(path)], check=True)
            reaped = reads.delta()
        self.assertGreaterEqual(named["by_process"]["server"], 1 << 20)
        self.assertGreaterEqual(counted["by_process"]["waited children"], 1 << 20)
        self.assertEqual(reaped["bytes"], 0)

    def test_a_process_gone_is_refused(self):
        child = subprocess.Popen([sys.executable, "-I", "-c", "pass"])
        child.wait()
        with self.assertRaisesRegex(ProofFailed, "no I/O account"):
            coldcache.ProcessDiskReads({"server": child.pid})


if __name__ == "__main__":
    unittest.main(verbosity=1)
