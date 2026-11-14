#!/usr/bin/env python3
"""Tests for scripts/codec_eval_results.py. Run with:

    python3 -I tests/scripts/test_codec_eval_results.py

The script runs as a subprocess on a stand-in of tick_codec_eval's output, a BSD time -l
report and a build folder's CMakeCache.txt, all under the repo's gitignored build/ directory.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "codec_eval_results.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_codec_eval_results"

sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO_ROOT / "tests" / "scripts"))

from test_build_preset import BENCH, DEV, write_cache  # noqa: E402

TOOL = {"tool": "tick_codec_eval", "schema": 1, "tool_version": "4", "day": "12302019",
        "source": "12302019.NASDAQ_ITCH50", "host": "macOS", "commit": "0d55bfef90d3",
        "tree_dirty": False, "compiler": "Clang 23.1.4", "build_type": "Release",
        "sanitizers": "none", "block_values": 4096, "columns_subset": None,
        "every_value_exact": True, "columns": [], "wall_seconds": 947.3}

BSD_TIME = """      947.52 real       930.10 user        12.40 sys
         14115012608  maximum resident set size
                   0  average shared memory size
             1203411  page reclaims
                  27  page faults
          6013035928  peak memory footprint
"""

GNU_TIME = """\tCommand being timed: "tick_codec_eval"
\tElapsed (wall clock) time (h:mm:ss or m:ss): 15:47.52
\tMaximum resident set size (kbytes): 13784192
"""

# The committed files' order: the tool's own keys, with these four right after `sanitizers`.
ADDED = ["build_preset", "peak_rss_bytes", "peak_rss_note", "peak_memory_footprint_bytes"]
NOTE = ("Peak resident size counts the clean file-backed pages of the read-only mapping of the "
        "source. peak_memory_footprint_bytes is what the process held beyond that mapping. Both "
        "are from /usr/bin/time -l around the whole run, the two exhaustive Price(4) proofs "
        "included.")


class CodecEvalResults(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.build = self.dir / "build"
        self.build.mkdir()
        self.tool = self.dir / "codec-eval.tool.json"
        self.time = self.dir / "eval.time"
        self.out = self.dir / "codec-eval-12302019.json"

    def run_script(self, tool=TOOL, time=BSD_TIME, cache=BENCH):
        self.tool.write_text(json.dumps(tool, indent=2) + "\n", encoding="utf-8")
        self.time.write_text(time, encoding="utf-8")
        write_cache(self.build, cache)
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--tool-output", str(self.tool),
                               "--time", str(self.time), "--build", str(self.build),
                               "--out", str(self.out)],
                              capture_output=True, text=True, check=False)

    def test_the_four_fields_follow_sanitizers_and_the_tools_own_output_is_kept(self):
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        written = json.loads(self.out.read_text(encoding="utf-8"))
        keys = list(TOOL)
        at = keys.index("sanitizers") + 1
        self.assertEqual(list(written), keys[:at] + ADDED + keys[at:])
        for key, value in TOOL.items():
            self.assertEqual(written[key], value, key)
        self.assertEqual(written["build_preset"], "bench")
        self.assertEqual(written["peak_rss_bytes"], 14115012608)
        self.assertEqual(written["peak_memory_footprint_bytes"], 6013035928)
        self.assertEqual(written["peak_rss_note"], NOTE)

    def test_the_preset_is_read_from_the_build_folder_not_assumed(self):
        done = self.run_script(tool=dict(TOOL, build_type="Debug"), cache=DEV)
        self.assertEqual(done.returncode, 0, done.stderr)
        written = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertEqual(written["build_preset"], "dev")

    def test_a_build_folder_no_preset_configures_is_refused(self):
        done = self.run_script(cache=dict(BENCH, **{"TICK_STORE_SANITIZE:STRING": "tsan"}))
        self.assertEqual(done.returncode, 1)
        self.assertIn("no preset in CMakePresets.json", done.stderr)
        self.assertFalse(self.out.exists())

    def test_a_tool_built_otherwise_than_the_folder_is_refused(self):
        done = self.run_script(tool=dict(TOOL, build_type="Debug"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("build_type 'Debug'", done.stderr)
        self.assertFalse(self.out.exists())

    def test_a_time_report_without_both_bsd_peaks_is_refused(self):
        done = self.run_script(time=GNU_TIME)
        self.assertEqual(done.returncode, 1)
        self.assertIn("peak memory footprint", done.stderr)
        self.assertFalse(self.out.exists())

    def test_output_that_is_not_the_tools_or_already_merged_is_refused(self):
        done = self.run_script(tool=dict(TOOL, tool="tick_reference_eval"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("not tick_codec_eval's output", done.stderr)
        done = self.run_script(tool=dict(TOOL, build_preset="bench"))
        self.assertEqual(done.returncode, 1)
        self.assertIn("already holds build_preset", done.stderr)
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
