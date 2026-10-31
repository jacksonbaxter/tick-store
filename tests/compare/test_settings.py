#!/usr/bin/env python3
"""compare/settings.py: each rule, with one injected fault. A check that cannot fail proves
nothing. Standard library only.

    python3 -I tests/compare/test_settings.py
"""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from compare.settings import (NOT_PROVEN, PROVEN, Setting, SettingsMismatch, check_commit_mode,
                              durability_verdict, parse_conf, require_all_match, split_dirty)

GOOD_CONF = "cairo.commit.mode=sync\n"
GOOD_ECHO = {"value": "sync", "value_source": "conf"}


class RequireAllMatch(unittest.TestCase):
    def test_equal_settings_pass_and_are_recorded(self):
        checked = require_all_match([Setting("a", 1, 1, "there")])
        self.assertEqual(checked, [{"name": "a", "requested": 1, "in_force": 1,
                                    "read_from": "there"}])

    def test_a_difference_fails_and_names_every_one(self):
        with self.assertRaises(SettingsMismatch) as caught:
            require_all_match([Setting("a", 1, 2, "x"), Setting("b", 1, 1, "x"),
                               Setting("c", "p", "q", "y")])
        self.assertIn("a: requested 1", str(caught.exception))
        self.assertIn("c: requested 'p'", str(caught.exception))
        self.assertNotIn("b:", str(caught.exception))


class CommitMode(unittest.TestCase):
    def test_all_sources_agreeing_pass(self):
        checked = check_commit_mode("sync", GOOD_CONF, GOOD_ECHO, "sync")
        self.assertEqual(len(checked), 3)

    def test_a_requested_value_outside_the_enum_is_refused(self):
        with self.assertRaisesRegex(SettingsMismatch, "not one of"):
            check_commit_mode("not_a_mode", "cairo.commit.mode=not_a_mode\n",
                              {"value": "not_a_mode", "value_source": "conf"}, "nosync")

    def test_a_bad_conf_value_under_a_good_request_is_refused(self):
        with self.assertRaisesRegex(SettingsMismatch, "silently run nosync"):
            check_commit_mode("sync", "cairo.commit.mode=not_a_mode\n", GOOD_ECHO, "sync")

    def test_an_echo_of_another_mode_is_refused(self):
        with self.assertRaisesRegex(SettingsMismatch, "echo"):
            check_commit_mode("sync", GOOD_CONF, {"value": "nosync", "value_source": "conf"},
                              "sync")

    def test_a_value_not_from_the_conf_is_refused(self):
        with self.assertRaisesRegex(SettingsMismatch, "not the conf"):
            check_commit_mode("sync", GOOD_CONF, {"value": "sync", "value_source": "default"},
                              "sync")

    def test_the_parser_outranks_a_matching_echo(self):
        with self.assertRaisesRegex(SettingsMismatch, "parsed"):
            check_commit_mode("sync", GOOD_CONF, GOOD_ECHO, "nosync")

    def test_a_missing_key_is_refused(self):
        with self.assertRaisesRegex(SettingsMismatch, "None"):
            check_commit_mode("sync", "other=1\n", GOOD_ECHO, "sync")

    def test_async_never_stands_in_for_sync(self):
        with self.assertRaises(SettingsMismatch):
            check_commit_mode("sync", "cairo.commit.mode=async\n",
                              {"value": "async", "value_source": "conf"}, "async")


class ParseConf(unittest.TestCase):
    def test_comments_and_blanks_are_skipped(self):
        self.assertEqual(parse_conf("# c\n\na=b=c\n"), {"a": "b=c"})

    def test_a_key_given_twice_is_refused(self):
        with self.assertRaisesRegex(SettingsMismatch, "twice"):
            parse_conf("a=1\na=2\n")

    def test_a_line_with_no_value_is_refused(self):
        with self.assertRaisesRegex(SettingsMismatch, "not a key"):
            parse_conf("a\n")


class Verdict(unittest.TestCase):
    def test_class_a_needs_dirty_pages(self):
        self.assertEqual(durability_verdict("A", {"data_dirty_pages": 5}, False)["status"], PROVEN)
        self.assertEqual(durability_verdict("A", {"data_dirty_pages": 0}, False)["status"],
                         NOT_PROVEN)

    def test_class_b_needs_zero_and_a_working_control(self):
        self.assertEqual(durability_verdict("B", {"data_dirty_pages": 0}, True)["status"], PROVEN)

    def test_class_b_with_a_dirty_page_is_not_proven(self):
        self.assertEqual(durability_verdict("B", {"data_dirty_pages": 1}, True)["status"],
                         NOT_PROVEN)

    def test_class_b_with_no_control_is_not_proven(self):
        verdict = durability_verdict("B", {"data_dirty_pages": 0}, False)
        self.assertEqual(verdict["status"], NOT_PROVEN)
        self.assertIn("proves nothing", verdict["reason"])

    def test_no_detector_is_not_proven(self):
        for name in ("A", "B"):
            self.assertEqual(durability_verdict(name, {"data_dirty_pages": None}, True)["status"],
                             NOT_PROVEN)

    def test_residual_files_are_named_in_the_reason(self):
        flush = {"data_dirty_pages": 0, "residual_dirty_files": {"stock.k": 2}}
        self.assertIn("residual_dirty_files", durability_verdict("B", flush, True)["reason"])


class SplitDirty(unittest.TestCase):
    def test_exempt_files_are_kept_apart_and_nothing_is_lost(self):
        held, excused = split_dirty({"a.d": 1, "lock.mdb": 2}, (re.compile("lock"),))
        self.assertEqual((held, excused), ({"a.d": 1}, {"lock.mdb": 2}))


if __name__ == "__main__":
    unittest.main(verbosity=1)
