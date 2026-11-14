#!/usr/bin/env python3
"""Tests for scripts/build_preset.py. Run with:

    python3 -I tests/scripts/test_build_preset.py

Each test writes a CMakeCache.txt under the repo's gitignored build/ directory and names its
preset against the repository's own CMakePresets.json and CMakeLists.txt.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "build_preset.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_build_preset"

sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import build_preset  # noqa: E402

# What a configure of each preset leaves in its cache, the lines the matching reads.
BENCH = {"CMAKE_BUILD_TYPE:STRING": "Release", "CMAKE_CXX_FLAGS_RELEASE:STRING": "-O3 -DNDEBUG",
         "TICK_STORE_BUILD_BENCH:BOOL": "ON", "TICK_STORE_BUILD_FUZZ:BOOL": "OFF",
         "TICK_STORE_BUILD_TESTS:BOOL": "ON", "TICK_STORE_BUILD_TOOLS:BOOL": "ON",
         "TICK_STORE_SANITIZE:STRING": "none"}
DEV = dict(BENCH, **{"CMAKE_BUILD_TYPE:STRING": "Debug", "TICK_STORE_BUILD_BENCH:BOOL": "OFF"})
FUZZ = dict(BENCH, **{"CMAKE_BUILD_TYPE:STRING": "RelWithDebInfo",
                      "CMAKE_CXX_FLAGS_RELEASE:STRING": "-O3 -DNDEBUG",
                      "TICK_STORE_BUILD_BENCH:BOOL": "OFF", "TICK_STORE_BUILD_FUZZ:BOOL": "ON",
                      "TICK_STORE_BUILD_TESTS:BOOL": "OFF", "TICK_STORE_BUILD_TOOLS:BOOL": "OFF",
                      "TICK_STORE_SANITIZE:STRING": "fuzz"})


def write_cache(folder, entries):
    lines = ["# This is the CMakeCache file.", "//Build type", ""]
    lines += [f"{key}={value}" for key, value in entries.items()]
    lines += ["//ADVANCED property for variable: CMAKE_CXX_FLAGS_RELEASE",
              "CMAKE_CXX_FLAGS_RELEASE-ADVANCED:INTERNAL=1", ""]
    (folder / "CMakeCache.txt").write_text("\n".join(lines), encoding="utf-8")


class BuildPreset(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))

    def preset(self, entries):
        write_cache(self.dir, entries)
        return build_preset.preset_of(self.dir)

    def test_each_presets_cache_is_named_by_its_preset(self):
        self.assertEqual(self.preset(BENCH), "bench")
        self.assertEqual(self.preset(DEV), "dev")
        self.assertEqual(self.preset(FUZZ), "fuzz")

    def test_a_cache_no_preset_configures_is_refused(self):
        # A dev build configured with the benchmarks on is neither dev nor bench.
        with self.assertRaisesRegex(build_preset.Refused, "no preset in CMakePresets.json"):
            self.preset(dict(DEV, **{"TICK_STORE_BUILD_BENCH:BOOL": "ON"}))
        # A release build with other flags is not the bench preset.
        with self.assertRaisesRegex(build_preset.Refused, "no preset in CMakePresets.json"):
            self.preset(dict(BENCH, **{"CMAKE_CXX_FLAGS_RELEASE:STRING": "-O2 -DNDEBUG"}))

    def test_a_folder_with_no_cache_is_refused(self):
        with self.assertRaisesRegex(build_preset.Refused, "no CMakeCache.txt"):
            build_preset.preset_of(self.dir)

    def test_the_command_line_prints_the_preset(self):
        write_cache(self.dir, BENCH)
        done = subprocess.run([sys.executable, "-I", str(SCRIPT), str(self.dir)],
                              capture_output=True, text=True, check=False)
        self.assertEqual((done.returncode, done.stdout), (0, "bench\n"), done.stderr)
        write_cache(self.dir, dict(DEV, **{"TICK_STORE_SANITIZE:STRING": "tsan"}))
        done = subprocess.run([sys.executable, "-I", str(SCRIPT), str(self.dir)],
                              capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 1)
        self.assertIn("no preset in CMakePresets.json", done.stderr)


if __name__ == "__main__":
    unittest.main()
