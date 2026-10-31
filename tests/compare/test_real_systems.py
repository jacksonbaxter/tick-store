#!/usr/bin/env python3
"""The harness's failures, on the real systems. No mock. Needs compare/setup.py to have run,
and skips visibly if it has not.

    .venv/bin/python -I tests/compare/test_real_systems.py

The first test is the reason compare/settings.py exists: a QuestDB conf holding
`cairo.commit.mode=not_a_mode` while `sync` was requested. QuestDB starts anyway, echoes the
value and runs nosync. The harness must refuse it.
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from compare.layout import SLICE_NAME, Layout

LAYOUT = Layout.default()
SETUP_DONE = (LAYOUT.java().exists() and LAYOUT.probe_classes().exists())


def build_scratch():
    (REPO_ROOT / "build").mkdir(exist_ok=True)
    return tempfile.TemporaryDirectory(dir=REPO_ROOT / "build")


@unittest.skipUnless(SETUP_DONE, "QuestDB is not set up: run compare/setup.py")
class BadCommitMode(unittest.TestCase):
    def setUp(self):
        from compare.durability import CLASS_B
        from compare.schema import TradingDay
        from compare.systems.base import RunContext
        from compare.systems.questdb_system import QuestDBSystem

        self.scratch = build_scratch()
        self.addCleanup(self.scratch.cleanup)
        LAYOUT.scratch.mkdir(parents=True, exist_ok=True)
        root = Path(self.scratch.name) / "root"
        context = RunContext(LAYOUT, root, "plain", CLASS_B, TradingDay(2019, 12, 30))
        self.system = QuestDBSystem(context)
        self.system.prepare()

    def corrupt_conf(self, value):
        path = self.system.server.conf_path
        path.write_text(path.read_text(encoding="ascii").replace(
            "cairo.commit.mode=sync", f"cairo.commit.mode={value}"), encoding="ascii")

    def test_the_harness_fails_on_not_a_mode_while_sync_is_requested(self):
        from compare.settings import SettingsMismatch
        from compare.systems.questdb_system import PORTS, commit_mode_echo, port_in_use

        self.corrupt_conf("not_a_mode")
        try:
            self.system.start_service()
            # QuestDB's silence, shown: it started, and the echo is the bad text.
            echo = commit_mode_echo()
            self.assertEqual((echo["value"], echo["value_source"]), ("not_a_mode", "conf"))
            # What it really runs, from its own parser.
            self.assertEqual(self.system.server.parsed_commit_mode(), "nosync")
            with self.assertRaisesRegex(SettingsMismatch, "not_a_mode"):
                self.system.service_settings()
        finally:
            self.system.stop_service()
        self.assertFalse(any(port_in_use(port) for port in PORTS))

    def test_a_mode_of_nosync_in_a_class_b_conf_fails(self):
        from compare.settings import SettingsMismatch

        self.corrupt_conf("nosync")
        try:
            self.system.start_service()
            with self.assertRaisesRegex(SettingsMismatch, "requested 'sync', in force 'nosync'"):
                self.system.service_settings()
        finally:
            self.system.stop_service()

    def test_a_correct_conf_passes_on_the_same_path(self):
        try:
            self.system.start_service()
            names = [item["name"] for item in self.system.service_settings()]
        finally:
            self.system.stop_service()
        self.assertIn("cairo.commit.mode (parsed)", names)


@unittest.skipUnless((LAYOUT.export_dir(SLICE_NAME) / "manifest.txt").exists() and
                     LAYOUT.tool("tick_roundtrip").exists(),
                     "the slice is not exported, or the bench tools are not built")
class ReadBackRefused(unittest.TestCase):
    """A read-back that lost one value must fail the real oracle."""

    def verify(self, directory):
        pin_source = LAYOUT.data / "slices" / f"{SLICE_NAME}.itch"
        return subprocess.run([str(LAYOUT.tool("tick_roundtrip")), "verify-columns",
                               str(pin_source), str(directory)], check=False, capture_output=True,
                              text=True).returncode

    def test_a_changed_value_is_caught_and_the_untouched_copy_passes(self):
        with build_scratch() as scratch:
            directory = Path(scratch) / "columns"
            shutil.copytree(LAYOUT.export_dir(SLICE_NAME), directory)
            self.assertEqual(self.verify(directory), 0)
            path = directory / "price.bin"
            data = bytearray(path.read_bytes())
            # A hot row's price, in the middle of the window.
            data[4 * 3_000_000] ^= 1
            path.write_bytes(data)
            self.assertNotEqual(self.verify(directory), 0)


if __name__ == "__main__":
    unittest.main(verbosity=1)
