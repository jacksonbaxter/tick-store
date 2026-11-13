#!/usr/bin/env python3
"""Tests for scripts/coverage_report.py. Run with:

    python3 -I tests/scripts/test_coverage_report.py

The gate is what could quietly stop meaning anything, so these feed it small function lists
and require it to fail for a function never called, for one that is gone from the binary,
and for a message type the list does not cover.
"""

import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("coverage_report",
                                              REPO_ROOT / "scripts" / "coverage_report.py")
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)

REQUIRED = (("parser", "src/a.cpp", "first", r"^ns::first\("),
            ("parser", "src/a.cpp", "second", r"^ns::second\("))


class Gate(unittest.TestCase):
    def test_every_function_called_passes(self):
        results = report.evaluate(REQUIRED, [("src/a.cpp", "ns::first(int)", 2),
                                             ("src/a.cpp", "ns::second(int)", 1)])
        self.assertEqual(report.unreached(results), [])

    def test_a_function_never_called_fails(self):
        results = report.evaluate(REQUIRED, [("src/a.cpp", "ns::first(int)", 0),
                                             ("src/a.cpp", "ns::second(int)", 1)])
        failed = report.unreached(results)
        self.assertEqual([item["name"] for item in failed], ["first"])
        self.assertIn("0 calls", report.describe(failed[0]))

    def test_a_function_gone_from_the_binary_fails(self):
        results = report.evaluate(REQUIRED, [("src/a.cpp", "ns::first(int)", 1)])
        failed = report.unreached(results)
        self.assertEqual([item["name"] for item in failed], ["second"])
        self.assertIn("no such function", report.describe(failed[0]))

    def test_a_name_in_another_file_does_not_count(self):
        results = report.evaluate(REQUIRED[:1], [("src/b.cpp", "ns::first(int)", 1)])
        self.assertEqual(len(report.unreached(results)), 1)

    def test_a_libstdcxx_abi_tag_does_not_hide_a_function(self):
        export = {"data": [{"functions": [
            {"name": "_Z1av", "filenames": ["/repo/src/a.cpp"], "count": 4},
            {"name": "_Z1bv", "filenames": ["/repo/src/a.cpp"], "count": 0}]}]}
        names = ["ns::first[abi:cxx11](std::span<std::byte const>)", "ns::second[abi:cxx11](int)"]
        original = report.demangle
        report.demangle = lambda mangled: names
        try:
            functions = report.functions_of(export, "/repo")
        finally:
            report.demangle = original
        self.assertEqual(functions, [("src/a.cpp", "ns::first(std::span<std::byte const>)", 4),
                                     ("src/a.cpp", "ns::second(int)", 0)])
        failed = report.unreached(report.evaluate(REQUIRED, functions))
        self.assertEqual([item["name"] for item in failed], ["second"])
        self.assertIn("0 calls", report.describe(failed[0]))

    def test_any_instantiation_reached_counts(self):
        results = report.evaluate(REQUIRED[:1], [("src/a.cpp", "ns::first(int)", 0),
                                                 ("src/a.cpp", "ns::first(long)", 3)])
        self.assertEqual(report.unreached(results), [])


class RequiredList(unittest.TestCase):
    def test_the_real_header_gives_all_23_types_both_ways(self):
        required = report.required_functions(report.message_types(REPO_ROOT))
        decoders = [item for item in required if item[0] == "decoder"]
        encoders = [item for item in required if item[0] == "encoder"]
        self.assertEqual((len(decoders), len(encoders)), (23, 23))

    def test_each_processor_requires_its_own_hardware_crc32c_kernel(self):
        types = report.message_types(REPO_ROOT)
        names = {machine: {item[2] for item in report.required_functions(types, machine)}
                 for machine in ("arm64", "x86_64", "riscv64")}
        self.assertIn("crc32c_armv8", names["arm64"])
        self.assertNotIn("crc32c_sse42", names["arm64"])
        self.assertIn("crc32c_sse42", names["x86_64"])
        self.assertNotIn("crc32c_armv8", names["x86_64"])
        self.assertFalse({"crc32c_armv8", "crc32c_sse42"} & names["riscv64"])
        self.assertIn("crc32c_software", names["riscv64"])

    def test_each_processor_requires_its_own_kernel_set_and_every_one_the_scalar_set(self):
        types = report.message_types(REPO_ROOT)
        names = {machine: {item[2] for item in report.required_functions(types, machine)}
                 for machine in ("arm64", "x86_64", "riscv64")}
        self.assertIn("svb_decode_neon", names["arm64"])
        self.assertNotIn("svb_decode_avx2", names["arm64"])
        self.assertIn("svb_decode_avx2", names["x86_64"])
        self.assertIn("cpu_has_avx2", names["x86_64"])
        self.assertNotIn("svb_decode_neon", names["x86_64"])
        self.assertFalse({"svb_decode_neon", "svb_decode_avx2"} & names["riscv64"])
        for machine in names:
            self.assertIn("svb_decode_scalar", names[machine])
            self.assertIn("ScalarKernelsOnThisThread", names[machine])

    def test_the_kernel_statement_is_the_kernel_sets_required(self):
        # The summary said no SIMD kernel existed after the kernels joined the list.
        types = report.message_types(REPO_ROOT)
        for machine, built in (("arm64", "NEON"), ("x86_64", "AVX2"), ("riscv64", None)):
            with self.subTest(machine=machine):
                statement = report.simd_statement(machine)
                self.assertNotIn("exist yet", statement)
                self.assertIn(report.SIMD_SCALAR, statement)
                files = {item[1] for item in report.required_functions(types, machine)
                         if item[0] == "kernel"}
                for file in files:
                    self.assertIn(file, statement)
                if built:
                    self.assertIn(built, statement)
                else:
                    self.assertIn("no SIMD set", statement)

    def test_the_docstring_does_not_say_the_kernels_are_missing(self):
        self.assertNotIn("none exists yet", report.__doc__)
        self.assertIn("kernel", report.__doc__.split("Standard library only")[0])

    def test_a_header_with_a_missing_type_stops_the_report(self):
        with tempfile.TemporaryDirectory(dir=REPO_ROOT / "build") as root:
            header = Path(root) / report.MESSAGE_HEADER
            header.parent.mkdir(parents=True)
            header.write_text("struct OnlyOne {\n    static constexpr std::byte type_code{'S'};\n")
            with self.assertRaises(SystemExit):
                report.message_types(root)


class Names(unittest.TestCase):
    def test_a_named_function_loses_its_signature(self):
        self.assertEqual(report.short_name("tick_store::itch::to_string(int)"),
                         "itch::to_string")

    def test_a_lambda_has_no_short_name(self):
        self.assertIsNone(report.short_name("_ZZN10tick_store3fooEvENK3$_0clEv"))

    def test_only_repo_paths_survive(self):
        self.assertEqual(report.relative_to_repo("/repo/src/a.cpp", "/repo"), "src/a.cpp")
        self.assertIsNone(report.relative_to_repo("/elsewhere/a.cpp", "/repo"))


class ExportedTree(unittest.TestCase):
    """A tree exported with git archive has no .git, and names its commit in a COMMIT file."""

    def setUp(self):
        scratch = REPO_ROOT / "build" / "test_coverage_report"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "COMMIT").write_text("0123456789abcdef0123456789abcdef01234567\n")
        for name in ("src/store/b.cpp", "src/a.cpp", "src/store/b.hpp", "tests/t.cpp"):
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            (self.root / name).write_text("")

    def test_the_commit_comes_from_the_commit_file(self):
        self.assertEqual(report.commit_of(self.root), "0123456789ab")

    def test_the_sources_are_every_cpp_file_under_src(self):
        self.assertEqual(report.tracked_sources(self.root), ["src/a.cpp", "src/store/b.cpp"])


if __name__ == "__main__":
    unittest.main()
