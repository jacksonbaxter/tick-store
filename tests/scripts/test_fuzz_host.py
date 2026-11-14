#!/usr/bin/env python3
"""Tests for scripts/fuzz_host.py. Run with:

    python3 -I tests/scripts/test_fuzz_host.py

Each test makes a stand-in fuzz build under the repo's gitignored build/ directory: its
CMakeCache.txt, compile_commands.json and build.ninja, and a compiler that prints a version
line. The host is this machine, as the script reads it.
"""

import io
import json
import os
import platform
import stat
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRATCH_ROOT = REPO_ROOT / "build" / "test_fuzz_host"

sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "tests" / "scripts"))

import check_fuzz  # noqa: E402
import fuzz_host  # noqa: E402
from test_build_preset import BENCH, FUZZ, write_cache  # noqa: E402

HOST = "macOS" if platform.system() == "Darwin" else "Linux"
ARM = platform.machine() in ("arm64", "aarch64")
DEFINES = (["-DTICK_STORE_CRC32C_ARMV8", "-DTICK_STORE_SIMD_NEON"] if ARM
           else ["-DTICK_STORE_CRC32C_SSE42", "-DTICK_STORE_SIMD_AVX2"])
FLAGS = ("-O2 -g -DNDEBUG -std=c++23 -fsanitize=fuzzer-no-link,address,undefined "
         "-fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wextra -Werror")
CONCURRENCY = "three targets at once, each pinned with taskset, in one heavy.sh job"
PEAK = "libFuzzer's peak_rss_mb"


class FuzzHost(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.dir = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.build = self.dir / "fuzz"
        self.build.mkdir()
        self.compiler = self.dir / "clang++"
        self.compiler.write_text("#!/bin/sh\necho 'clang version 23.1.4 (stand-in)'\n"
                                 "echo 'Target: some-target'\n", encoding="utf-8")
        self.compiler.chmod(self.compiler.stat().st_mode | stat.S_IXUSR)
        self.out = self.dir / "host.json"
        self.make_build()

    def make_build(self, cache=FUZZ, link_extra=""):
        write_cache(self.build, cache)
        source = REPO_ROOT / "fuzz" / "fuzz_pfor.cpp"
        sysroot = "-arch arm64 -isysroot /opt/sdk/platform.sdk " if HOST == "macOS" else ""
        pfor = (f"{self.compiler} -I{REPO_ROOT}/include -isystem {self.build}/_deps/zstd/lib "
                f"{FLAGS} {sysroot}-MD -MT fuzz/x.o -MF fuzz/x.o.d -o fuzz/x.o -c {source}")
        core = (f"{self.compiler} {' '.join(DEFINES)} -I{REPO_ROOT}/include {FLAGS} "
                f"-o core.o -c {REPO_ROOT}/src/codec/simd/dispatch.cpp")
        commands = [{"directory": str(self.build), "command": core,
                     "file": str(REPO_ROOT / "src" / "codec" / "simd" / "dispatch.cpp")},
                    {"directory": str(self.build), "command": pfor, "file": str(source)}]
        (self.build / "compile_commands.json").write_text(json.dumps(commands),
                                                          encoding="utf-8")
        link = (f"-Xlinker -exported_symbols_list -Xlinker {self.build}/no-exported-symbols.txt "
                f"-Xlinker --dependency-file={self.build}/fuzz/x.d "
                f"-fsanitize=address,undefined -fsanitize=fuzzer{link_extra}")
        (self.build / "build.ninja").write_text(
            "build fuzz/fuzz_dictionary: CXX_EXECUTABLE_LINKER__fuzz_dictionary x.o\n"
            "  LINK_FLAGS = -fsanitize=fuzzer -Wl,/elsewhere/other.txt\n\n"
            "build fuzz/fuzz_pfor: CXX_EXECUTABLE_LINKER__fuzz_pfor_RelWithDebInfo x.o | a.a\n"
            "  CONFIG = RelWithDebInfo\n"
            f"  LINK_FLAGS = {link}\n"
            "  OBJECT_DIR = fuzz/CMakeFiles/fuzz_pfor.dir\n", encoding="utf-8")

    def run_main(self, level="default", hostname="stand-in-host", concurrency=CONCURRENCY):
        argv = ["--build", str(self.build), "--host", HOST, "--level", level,
                "--concurrency", concurrency, "--peak-memory", PEAK, "--out", str(self.out)]
        errors = io.StringIO()
        with mock.patch("socket.gethostname", return_value=hostname), redirect_stderr(errors):
            code = fuzz_host.main(argv)
        return code, errors.getvalue()

    def test_the_facts_are_the_builds_and_this_hosts(self):
        code, errors = self.run_main()
        self.assertEqual(code, 0, errors)
        facts = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertEqual(sorted(facts), sorted(check_fuzz.HOST_FIELDS + ("default_kernel_level",)))
        self.assertTrue(facts["kernel"].startswith(check_fuzz.HOSTS[HOST]["kernel"]))
        self.assertEqual(facts["compiler"], "clang version 23.1.4 (stand-in)")
        self.assertTrue(facts["cpu_model"])
        system = "Darwin" if HOST == "macOS" else "Linux"
        asan = fuzz_host.fuzz_g3.read_launch()["asan_options"][system]
        self.assertEqual(facts["sanitizer_options"],
                         f"ASAN_OPTIONS={asan} UBSAN_OPTIONS=print_stacktrace=1, from "
                         "fuzz/launch.txt, which scripts/fuzz_g3.py applies")
        self.assertEqual(facts["compile_flags"],
                         f"fuzz preset, RelWithDebInfo: {FLAGS}, {' '.join(sorted(DEFINES))}; "
                         "targets linked with -Xlinker -exported_symbols_list -Xlinker "
                         "build/fuzz/no-exported-symbols.txt -fsanitize=address,undefined "
                         "-fsanitize=fuzzer")
        self.assertEqual((facts["concurrency"], facts["peak_memory"]), (CONCURRENCY, PEAK))
        self.assertEqual(facts["default_kernel_level"], "neon" if ARM else "avx2")

    def test_a_capped_level_is_in_the_options(self):
        code, errors = self.run_main(level="scalar")
        self.assertEqual(code, 0, errors)
        facts = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertIn("UBSAN_OPTIONS=print_stacktrace=1 TICK_STORE_SIMD=scalar, from",
                      facts["sanitizer_options"])

    def test_no_absolute_path_is_left_in_the_file(self):
        self.make_build(link_extra=" -Wl,-rpath,/opt/toolchain/lib")
        code, errors = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("/opt/toolchain/lib", errors)
        self.assertIn("absolute path", errors)
        self.assertFalse(self.out.exists())

    def test_the_hosts_name_is_never_in_the_file(self):
        code, errors = self.run_main(hostname="stand-in-host.example",
                                     concurrency=CONCURRENCY + " on stand-in-host")
        self.assertEqual(code, 1)
        self.assertIn("the host's name", errors)
        self.assertFalse(self.out.exists())

    def test_a_build_that_is_not_the_fuzz_preset_is_refused(self):
        self.make_build(cache=BENCH)
        code, errors = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("bench preset, not the fuzz preset", errors)
        self.assertFalse(self.out.exists())

    def test_a_build_without_the_targets_link_is_refused(self):
        (self.build / "build.ninja").write_text("build fuzz/fuzz_dictionary: X x.o\n"
                                                "  LINK_FLAGS = -fsanitize=fuzzer\n",
                                                encoding="utf-8")
        code, errors = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("build.ninja has no link of fuzz/fuzz_pfor", errors)

    def test_the_host_must_be_this_machine(self):
        other = "Linux" if HOST == "macOS" else "macOS"
        errors = io.StringIO()
        with redirect_stderr(errors):
            code = fuzz_host.main(["--build", str(self.build), "--host", other,
                                   "--concurrency", CONCURRENCY, "--peak-memory", PEAK,
                                   "--out", str(self.out)])
        self.assertEqual(code, 1)
        self.assertIn(f"this machine is not {other}", errors.getvalue())


if __name__ == "__main__":
    os.environ.pop("TICK_STORE_SIMD", None)
    unittest.main()
