#!/usr/bin/env python3
"""compare/coldcache.py: warm and cold claims refused without their evidence. Standard
library only.

    python3 -I tests/compare/test_coldcache.py
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from compare.coldcache import ProofFailed, allocated_bytes, proof_of_cold, proof_of_warm

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


if __name__ == "__main__":
    unittest.main(verbosity=1)
