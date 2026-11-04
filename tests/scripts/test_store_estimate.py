#!/usr/bin/env python3
"""Tests for scripts/store_estimate.py. Run with:

    python3 -I tests/scripts/test_store_estimate.py

The estimate is a sum of choices, so these feed it small hand-made results and require the
smallest exact codec per column, the smallest representation per field, the exceptions a
derived field needs, the index per block, and refusal of a subset run or an inexact one.
"""

import copy
import importlib.util
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("store_estimate",
                                              REPO_ROOT / "scripts" / "store_estimate.py")
estimator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(estimator)


def codec(name, stored, blocks=1, exact=True, metadata=8, zstd=None):
    return {"codec": name, "stored_bytes": stored, "blocks": blocks, "exact": exact,
            "metadata_bytes": metadata,
            "zstd1_or_plain_bytes": zstd if zstd is not None else stored - metadata + blocks}


def column(name, values, codecs, raw_zstd=10**12):
    return {"name": name, "values": values, "codecs": codecs, "raw_zstd1_bytes": raw_zstd,
            "raw_zstd1_mismatches": 0}


def every_column(size=100):
    """Every column the estimate reads, each with one codec of `size` bytes."""
    names = set()
    for layout in ("file_order", "per_locate"):
        for _, alternatives in estimator.groups(layout):
            for columns, _ in alternatives:
                names.update(columns)
    return {name: column(name, 10, [codec("pfor-for", size)]) for name in sorted(names)}


def analysis(unresolved=0, mismatches=0, stock_differs=0):
    return {
        "reference_locality": [{"type": t, "unresolved": unresolved,
                                "locate_mismatches": mismatches} for t in "ECXDU"],
        "stock_fields": {"A": {"messages": 5, "differ_from_directory": stock_differs}},
        "raw_unknown_type": 0,
        "raw_wrong_length": 0,
    }


def result(columns, day="12302019", **kwargs):
    return {"day": day, "block_values": 4096, "commit": "abc", "messages": 10,
            "columns_subset": None, "every_value_exact": True,
            "columns": list(columns.values()), "analysis": analysis(**kwargs)}


class BestCodec(unittest.TestCase):
    def test_takes_the_smallest_exact_codec(self):
        got = estimator.best_codec(column("c", 10, [codec("a", 50), codec("b", 30, exact=False),
                                                    codec("c", 40)]), with_zstd=False)
        self.assertEqual(got[0], 40)
        self.assertEqual(got[2], "c")

    def test_with_zstd_takes_a_smaller_frame_or_raw_zstd(self):
        col = column("c", 10, [codec("a", 50, zstd=20)], raw_zstd=5)
        self.assertEqual(estimator.best_codec(col, with_zstd=False)[0], 50)
        # raw zstd frames plus the stream metadata.
        self.assertEqual(estimator.best_codec(col, with_zstd=True), (13, 1, "raw-zstd1"))
        col["raw_zstd1_bytes"] = 10**9
        self.assertEqual(estimator.best_codec(col, with_zstd=True)[0], 28)

    def test_an_empty_column_costs_nothing(self):
        self.assertEqual(estimator.best_codec(column("c", 0, []), False)[0], 0)


class Layouts(unittest.TestCase):
    def test_sums_one_alternative_per_field_with_its_index(self):
        cols = every_column(100)
        got = estimator.estimate_layout(result(cols), "file_order", with_zstd=False)
        groups = estimator.groups("file_order")
        expected = 0
        for _, alternatives in groups:
            cheapest = min(len(columns) for columns, _ in alternatives)
            expected += cheapest * (100 + estimator.INDEX_BYTES_PER_BLOCK)
        self.assertEqual(got["total_bytes"], expected)

    def test_takes_the_smaller_representation_of_a_field(self):
        cols = every_column(100)
        cols["creation_ref"]["codecs"] = [codec("pfor-head-delta", 500)]
        got = estimator.estimate_layout(result(cols), "file_order", with_zstd=False)
        field = next(f for f in got["fields"] if f["field"].startswith("A and F reference"))
        names = [part["column"] for part in field["chosen"]["columns"]]
        self.assertNotIn("creation_ref", names)
        self.assertEqual(len(names), 2)

    def test_per_locate_reads_the_per_locate_columns(self):
        got = estimator.estimate_layout(result(every_column()), "per_locate", with_zstd=False)
        chosen = {part["column"] for f in got["fields"] for part in f["chosen"]["columns"]}
        self.assertIn("timestamp_per_locate", chosen)
        self.assertIn("add_shares_per_locate", chosen)
        self.assertNotIn("timestamp", chosen)
        self.assertNotIn("add_shares", chosen)

    def test_mixed_takes_each_field_in_its_smaller_order(self):
        cols = every_column(100)
        cols["add_price"]["codecs"] = [codec("pfor-for-scaled", 900)]
        cols["timestamp_per_locate"]["codecs"] = [codec("svb-delta", 900)]
        mixed = estimator.estimate_layout(result(cols), "mixed", with_zstd=False)
        chosen = {part["column"] for f in mixed["fields"] for part in f["chosen"]["columns"]}
        self.assertIn("timestamp", chosen)
        self.assertNotIn("add_price", chosen)
        file_order = estimator.estimate_layout(result(cols), "file_order", with_zstd=False)
        per_locate = estimator.estimate_layout(result(cols), "per_locate", with_zstd=False)
        self.assertLess(mixed["total_bytes"],
                        min(file_order["total_bytes"], per_locate["total_bytes"]))

    def test_unresolved_references_and_differing_stocks_cost_exceptions(self):
        clean = estimator.estimate_layout(result(every_column()), "file_order", False)
        costly = estimator.estimate_layout(result(every_column(), unresolved=3, stock_differs=2),
                                           "file_order", False)
        # Unresolved references are stored whole: 5 types x 3 x 8 bytes, unless a
        # representation without distances is cheaper; here every column costs the same,
        # so the raw-reference alternatives win and the cost goes to stocks alone.
        self.assertEqual(costly["derived_field_exception_bytes"]["stock_differs_from_directory"],
                         2 * (estimator.EXCEPTION_POSITION_BYTES + estimator.STOCK_BYTES))
        self.assertEqual(costly["total_bytes"] - clean["total_bytes"],
                         2 * (estimator.EXCEPTION_POSITION_BYTES + estimator.STOCK_BYTES))

    def test_a_derived_locate_pays_for_its_mismatches(self):
        cols = every_column(100)
        # Make distances with a derived locate the only cheap representation.
        for name in ["stock_locate", "referenced_ref", "referenced_ref_per_type"]:
            cols[name]["codecs"] = [codec("pfor-for", 10**6)]
        cols["stock_locate_unreferenced"]["codecs"] = [codec("pfor-for", 1)]
        base = estimator.estimate_layout(result(cols), "file_order", False)
        worse = estimator.estimate_layout(result(cols, mismatches=1), "file_order", False)
        self.assertEqual(worse["total_bytes"] - base["total_bytes"],
                         5 * (estimator.EXCEPTION_POSITION_BYTES + estimator.RAW_LOCATE_BYTES))


class Refusals(unittest.TestCase):
    def test_refuses_a_subset_run(self):
        res = result(every_column())
        res["columns_subset"] = ["add_ref"]
        with self.assertRaises(SystemExit):
            estimator.estimate([res], {"days": {}}, None)

    def test_refuses_an_inexact_run_and_a_missing_column(self):
        res = result(every_column())
        res["every_value_exact"] = False
        with self.assertRaises(SystemExit):
            estimator.estimate([res], {"days": {}}, None)
        cols = every_column()
        del cols["match_number"]
        with self.assertRaises(SystemExit):
            estimator.estimate([result(cols)], {"days": {}}, None)


class Judging(unittest.TestCase):
    PINS = {"days": {"12302019": {"decompressed_size": 8_251_407_909,
                                  "gz_size": 3_524_013_057}}}
    CAPACITY = {"day": "12302019",
                "systems": {"parquet": {"stored_bytes": 3_191_406_592},
                            "arcticdb": {"stored_bytes": 6_352_666_624},
                            "questdb": {"stored_bytes": 22_897_786_880}}}

    def test_cr1_and_cr2_follow_the_total(self):
        met = estimator.judge("12302019", 3_000_000_000, self.PINS, self.CAPACITY)
        self.assertTrue(met["CR-1"]["met"])
        self.assertTrue(met["CR-2"]["met"])
        # 8,251,407,909 / 2.3415 is about 3,523,984,000 bytes.
        missed = estimator.judge("12302019", 3_524_100_000, self.PINS, self.CAPACITY)
        self.assertFalse(missed["CR-1"]["met"])
        self.assertFalse(missed["CR-2"]["met"])
        self.assertFalse(missed["CR-2"]["systems"]["parquet"]["smaller"])
        self.assertTrue(missed["CR-2"]["systems"]["arcticdb"]["smaller"])

    def test_another_day_has_no_cr1_floor_or_cr2_systems(self):
        got = estimator.judge("10302019", 1, {"days": {}}, self.CAPACITY)
        self.assertEqual(got, {})

    def test_a_whole_estimate_is_labelled_and_judged(self):
        document = estimator.estimate([result(every_column())], self.PINS, self.CAPACITY)
        self.assertTrue(document["estimate"])
        self.assertEqual(len(document["days"][0]["layouts"]), 6)
        lines = estimator.summary_lines(document)
        self.assertEqual(len(lines), 6)
        self.assertIn("CR-1 True", lines[0])


if __name__ == "__main__":
    unittest.main()
