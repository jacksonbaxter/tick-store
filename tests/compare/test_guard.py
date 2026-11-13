#!/usr/bin/env python3
"""compare/guard.py's stop rule and compare/swap.py's counters. Standard library only, so it
runs everywhere scripts/check.sh's py-scripts step does, CI included.

    python3 -I tests/compare/test_guard.py

Each stop condition is shown firing, and swap-out alone, however fast and however full the
swap device, is shown not stopping a run: "The guard's swap rule" in docs/NOTES.md.
"""

import io
import signal
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from compare import guard, swap  # noqa: E402
from compare.guard import (GIB, PAGE_BYTES, SAMPLE_SECONDS, SWAP_EXHAUSTED_FRACTION,  # noqa: E402
                           SWAP_STRIKES, HostSample, Limits, breach, exhausted_floor_bytes,
                           parse_meminfo, parse_status, rates, session_of, swap_exhausted,
                           wait_status_fields)

MIB = 1 << 20
LIMITS = Limits()
SWAP_TOTAL = 999_420 * 1024  # debian's swap device.


def host(available=12 * GIB, swap_free=650 * MIB, swap_in=0, swap_out=0, disk=200 * GIB,
         swap_total=SWAP_TOTAL):
    return HostSample(available, swap_free, swap_total, swap_in, swap_out, disk)


def pages_per_sample(bytes_per_second):
    return int(bytes_per_second * SAMPLE_SECONDS / PAGE_BYTES)


def replay(samples):
    """Runs the guard's rule over a sequence of samples as Guard.take_sample does. Returns the
    first reason and the sample it fired on, or (None, None)."""
    strikes = 0
    last = None
    for index, sample in enumerate(samples):
        swap_in, _ = rates(sample, last, SAMPLE_SECONDS)
        last = sample
        reason, strikes = breach(sample, swap_in, strikes, LIMITS)
        if reason:
            return reason, index
    return None, None


class Breach(unittest.TestCase):
    def test_a_healthy_host_goes_on(self):
        self.assertEqual(breach(host(), 0, 0, LIMITS), (None, 0))

    # (a) MemAvailable under the floor.

    def test_memory_under_the_floor_fires_at_once(self):
        self.assertIn("MemAvailable under 3 GiB", breach(host(3 * GIB - 1), 0, 0, LIMITS)[0])

    def test_memory_at_the_floor_goes_on(self):
        self.assertIsNone(breach(host(3 * GIB), 0, 0, LIMITS)[0])

    def test_a_full_disk_fires(self):
        self.assertIn("disk", breach(host(disk=GIB), 0, 0, LIMITS)[0])

    # (b) Sustained swap-in.

    def test_sustained_swap_in_stops(self):
        fast = LIMITS.max_swap_in_bytes_per_second + 1
        strikes = 0
        for _ in range(SWAP_STRIKES - 1):
            reason, strikes = breach(host(), fast, strikes, LIMITS)
            self.assertIsNone(reason)
        self.assertIn("swap-in over", breach(host(), fast, strikes, LIMITS)[0] or "no stop")

    def test_a_swap_in_burst_does_not_stop(self):
        fast = LIMITS.max_swap_in_bytes_per_second + 1
        strikes = 0
        for _ in range(SWAP_STRIKES - 1):
            reason, strikes = breach(host(), fast, strikes, LIMITS)
        self.assertEqual(breach(host(), 0, strikes, LIMITS), (None, 0))

    def test_swap_in_at_the_limit_goes_on(self):
        strikes = 0
        for _ in range(SWAP_STRIKES * 2):
            reason, strikes = breach(host(), LIMITS.max_swap_in_bytes_per_second, strikes, LIMITS)
            self.assertIsNone(reason)

    def test_swap_in_replayed_from_proc_counters_stops(self):
        rate = 12 * MIB
        samples = [host(swap_in=index * pages_per_sample(rate)) for index in range(6)]
        reason, at = replay(samples)
        self.assertIn("swap-in", reason or "no stop")
        self.assertEqual(at, SWAP_STRIKES)  # The first sample has no rate.

    # (c) Swap exhausted with too little memory left.

    def test_the_exhausted_floor_is_floor_a_plus_the_swap_device(self):
        self.assertEqual(exhausted_floor_bytes(host(), LIMITS), 3 * GIB + SWAP_TOTAL)

    def test_swap_exhausted_under_the_second_floor_stops(self):
        sample = host(available=3 * GIB + SWAP_TOTAL - 1, swap_free=12 * 1024)
        self.assertIn("swap exhausted", breach(sample, 0, 0, LIMITS)[0])

    def test_swap_exhausted_with_memory_to_spare_goes_on(self):
        self.assertIsNone(breach(host(available=8 * GIB, swap_free=0), 0, 0, LIMITS)[0])
        self.assertIsNone(breach(host(available=3 * GIB + SWAP_TOTAL, swap_free=0), 0, 0,
                                 LIMITS)[0])

    def test_the_second_floor_needs_swap_exhausted(self):
        sample = host(available=3 * GIB + 1, swap_free=int(SWAP_TOTAL * SWAP_EXHAUSTED_FRACTION) + 1)
        self.assertFalse(swap_exhausted(sample))
        self.assertIsNone(breach(sample, 0, 0, LIMITS)[0])

    def test_a_host_with_no_swap_has_floor_a_alone(self):
        sample = host(available=3 * GIB, swap_free=0, swap_total=0)
        self.assertFalse(swap_exhausted(sample))
        self.assertIsNone(breach(sample, 0, 0, LIMITS)[0])
        self.assertIn("MemAvailable", breach(host(3 * GIB - 1, 0, swap_total=0), 0, 0, LIMITS)[0])

    # Swap-out alone.

    def test_swap_out_alone_never_stops(self):
        """The dd5de38 stops replayed: swap-out at about 80 MiB/s, free swap from 650 MiB to
        12 KiB in about 6 s, and on to the end of a long run, with 11.6 GiB available and a
        trickle of swap-in."""
        samples = []
        pages_out = 0
        free = 650 * MIB
        for index in range(600):
            out = pages_per_sample(80 * MIB) if index < 4 else pages_per_sample(MIB)
            pages_out += out
            free = max(12 * 1024, free - out * PAGE_BYTES)
            samples.append(host(available=int(11.6 * GIB), swap_free=free, swap_in=index * 50,
                                swap_out=pages_out))
        self.assertEqual(replay(samples), (None, None))

    def test_the_rate_splits_in_from_out(self):
        before = host(swap_in=10, swap_out=100)
        after = host(swap_in=12, swap_out=600)
        self.assertEqual(rates(after, before, SAMPLE_SECONDS),
                         (2 * PAGE_BYTES / SAMPLE_SECONDS, 500 * PAGE_BYTES / SAMPLE_SECONDS))
        self.assertEqual(rates(after, None, None), (0.0, 0.0))

    def test_the_rate_is_over_the_time_between_the_samples(self):
        # Review F-15: a sample comes SAMPLE_SECONDS after the last one's end, so the time
        # between two is longer, and a rate over the nominal 2 s overstates it.
        args = SimpleNamespace(min_available_gib=3, min_free_disk_gib=50, command=["true"],
                               watch="unused")
        watcher = guard.Guard(args)
        samples = iter([host(swap_in=0), host(swap_in=1000)])
        with (mock.patch.object(guard, "read_host", lambda watch: next(samples)),
              mock.patch.object(watcher, "note_processes", return_value=0)):
            watcher.take_sample(1, io.StringIO(), 10.0)
            watcher.take_sample(1, io.StringIO(), 12.5)
        self.assertEqual(watcher.swap_in_peak, 1000 * PAGE_BYTES / 2.5)


class ProcFiles(unittest.TestCase):
    def test_meminfo(self):
        fields = parse_meminfo("MemTotal:  10 kB\nMemAvailable:  4 kB\nSwapTotal:  2 kB\n"
                               "HugePages_Total:  0\n")
        self.assertEqual(fields["MemAvailable"], 4096)
        self.assertEqual(fields["SwapTotal"], 2048)

    def test_vmstat_keeps_in_and_out_apart(self):
        self.assertEqual(swap.parse_vmstat("pswpin 3\npswpout 4\nnr_free_pages 9\n"), (3, 4))

    def test_macos_vm_stat(self):
        text = ("Mach Virtual Memory Statistics: (page size of 16384 bytes)\n"
                "Pages free:                                  1050922.\n"
                "Swapins:                                     5022651.\n"
                "Swapouts:                                    8769446.\n")
        self.assertEqual(swap.parse_vm_stat(text), (5022651, 8769446, 16384))
        with self.assertRaises(ValueError):
            swap.parse_vm_stat("Mach Virtual Memory Statistics: (page size of 16384 bytes)\n")

    def test_status_fields(self):
        status = parse_status("Name:\tjava\nVmHWM:\t  100 kB\nRssFile:\t  7 kB\nThreads:\t9\n")
        self.assertEqual(status, {"VmHWM": 102400, "RssFile": 7168})

    def test_a_comm_with_spaces_and_a_bracket_does_not_move_the_session(self):
        self.assertEqual(session_of("12 (a b) c) S 1 12 77 0 -1 4194304"), 77)

    def test_a_wait_status_names_its_signal(self):
        self.assertEqual(wait_status_fields(-signal.SIGILL), {"code": None, "signal": "SIGILL"})
        self.assertEqual(wait_status_fields(3), {"code": 3, "signal": None})


class SwapCounts(unittest.TestCase):
    def test_a_phase_counts_the_change(self):
        self.assertEqual(swap.counts((10, 20, 4096), (15, 21, 4096)),
                         {"pages_in": 5, "pages_out": 1})

    def test_phases_add(self):
        self.assertEqual(swap.add({"pages_in": 1, "pages_out": 2}, {"pages_in": 3, "pages_out": 4}),
                         {"pages_in": 4, "pages_out": 6})
        self.assertEqual(swap.add(), {"pages_in": 0, "pages_out": 0})

    def test_a_timed_query_is_flagged_on_any_swap_in(self):
        quiet = {"pages_in": 0, "pages_out": 900}
        self.assertFalse(swap.timed([quiet, quiet])["flagged"])
        self.assertTrue(swap.timed([quiet, {"pages_in": 1, "pages_out": 0}])["flagged"])

    def test_this_host_can_be_read(self):
        pages_in, pages_out, page_bytes = swap.read()
        self.assertGreaterEqual(min(pages_in, pages_out), 0)
        self.assertGreater(page_bytes, 0)
        with swap.Phase() as phase:
            pass
        self.assertGreaterEqual(phase.counts["pages_in"], 0)
        self.assertIn("flagged", swap.method_block()["timed_policy"])


if __name__ == "__main__":
    unittest.main()
