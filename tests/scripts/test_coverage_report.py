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


if __name__ == "__main__":
    unittest.main()
