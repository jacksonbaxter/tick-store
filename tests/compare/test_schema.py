#!/usr/bin/env python3
"""compare/schema.py and compare/queries.py on a tiny column directory. Needs the repo's .venv.

    .venv/bin/python -I tests/compare/test_schema.py
"""

import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

try:
    import numpy as np
    import pyarrow as pa
except ImportError:
    raise unittest.SkipTest("needs the .venv: compare/setup.py makes it") from None

from compare.queries import (NANOS_PER_MINUTE, OPEN_NS, Query, QueryMismatch, aggregate_digest,
                             busiest_locate, build_queries, digest_result, full_day_facts,
                             ground_truth, require_truth, rows_digest)
from compare.schema import (FIXED_COLUMNS, TEXT_COLUMNS, ColumnsError, ColumnSet, ColumnWriter,
                            TradingDay, arrow_table, format_manifest, read_manifest,
                            table_chunks, write_columns)

DAY = TradingDay(2019, 12, 30)
RECORDS = 6


def write_fixture(directory):
    """Six rows: a K with locate 0, four rows of locate 7 and 9, a rare row."""
    fixed = {"seq": list(range(RECORDS)),
             "ts_ns": [OPEN_NS - 5, OPEN_NS, OPEN_NS + 1, OPEN_NS + NANOS_PER_MINUTE,
                       OPEN_NS + NANOS_PER_MINUTE + 1, OPEN_NS + 2 * NANOS_PER_MINUTE],
             "msg_type": [ord("S"), ord("A"), ord("F"), ord("A"), ord("E"), ord("L")],
             "locate": [0, 7, 7, 9, 7, 7], "tracking": [0, 1, 1, 1, 1, 1],
             "order_ref": [0, 1, 2, 3, 1, 0], "new_ref": [0] * RECORDS, "side": [0, 66, 83, 66, 0, 0],
             "shares": [0, 100, 200, 300, 50, 0], "price": [0, 10, 20, 30, 0, 0],
             "match": [0, 0, 0, 0, 5, 0], "printable": [0] * RECORDS}
    for name, numpy_type, _ in FIXED_COLUMNS:
        np.array(fixed[name], dtype=numpy_type).tofile(directory / f"{name}.bin")
    text = {"stock": ["", "AAPL    ", "AAPL    ", "MSFT    ", "", ""],
            "mpid": ["", "", "GSCO", "", "", ""], "rest": ["ab", "", "", "", "", "00ff"]}
    sizes = {}
    for name in TEXT_COLUMNS:
        data = "".join(text[name]).encode("ascii")
        offsets = np.cumsum([0, *[len(value) for value in text[name]]], dtype="<u8")
        offsets.tofile(directory / f"{name}.off")
        (directory / f"{name}.dat").write_bytes(data)
        sizes[name] = len(data)
    (directory / "tail.bin").write_bytes(b"")
    (directory / "manifest.txt").write_text(format_manifest(RECORDS, 0, sizes), encoding="ascii")


class Fixture(unittest.TestCase):
    def setUp(self):
        (REPO_ROOT / "build").mkdir(exist_ok=True)
        self.scratch = tempfile.TemporaryDirectory(dir=REPO_ROOT / "build")
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name) / "in"
        self.directory.mkdir()
        write_fixture(self.directory)
        self.columns = ColumnSet(self.directory)


class Columns(Fixture):
    def test_arrow_and_back_is_byte_identical(self):
        table = arrow_table(self.columns, DAY)
        out = Path(self.scratch.name) / "out"
        write_columns(table, out, DAY)
        for path in sorted(self.directory.iterdir()):
            self.assertEqual(path.read_bytes(), (out / path.name).read_bytes(), path.name)

    def test_table_chunks_are_the_same_table_in_order(self):
        whole = arrow_table(self.columns, DAY)
        for size in (1, 2, 4, RECORDS, RECORDS + 3):
            chunks = list(table_chunks(self.columns, DAY, size))
            self.assertEqual(len(chunks), -(-RECORDS // size))
            self.assertLessEqual(max(chunk.num_rows for chunk in chunks), size)
            self.assertTrue(pa.concat_tables(chunks).equals(whole), size)

    def test_chunks_and_dictionaries_change_nothing(self):
        table = arrow_table(self.columns, DAY)
        stock = table.column("stock").dictionary_encode()
        table = table.set_column(table.schema.get_field_index("stock"), "stock", stock)
        chunked = pa.concat_tables([table.slice(0, 2), table.slice(2)])
        out = Path(self.scratch.name) / "out"
        write_columns(chunked, out, DAY)
        self.assertEqual((self.directory / "stock.dat").read_bytes(),
                         (out / "stock.dat").read_bytes())
        self.assertEqual((self.directory / "stock.off").read_bytes(),
                         (out / "stock.off").read_bytes())

    def test_a_null_that_came_back_fails(self):
        table = arrow_table(self.columns, DAY)
        index = table.schema.get_field_index("shares")
        broken = pa.array([None, *table.column("shares").to_pylist()[1:]], pa.int32())
        with self.assertRaisesRegex(ColumnsError, "nulls"):
            write_columns(table.set_column(index, "shares", broken),
                          Path(self.scratch.name) / "out", DAY)

    def test_a_value_too_wide_for_its_column_fails(self):
        table = arrow_table(self.columns, DAY)
        index = table.schema.get_field_index("side")
        broken = pa.array([300] * RECORDS, pa.int32())
        with self.assertRaisesRegex(ColumnsError, "does not fit"):
            write_columns(table.set_column(index, "side", broken),
                          Path(self.scratch.name) / "out", DAY)

    def test_microseconds_coming_back_fail(self):
        table = arrow_table(self.columns, DAY)
        index = table.schema.get_field_index("ts")
        broken = table.column("ts").cast(pa.timestamp("us"), safe=False)
        with self.assertRaisesRegex(ColumnsError, "precision"):
            write_columns(table.set_column(index, "ts", broken),
                          Path(self.scratch.name) / "out", DAY)

    def test_a_missing_column_fails(self):
        table = arrow_table(self.columns, DAY).drop_columns(["rest"])
        with self.assertRaisesRegex(ColumnsError, "missing"):
            write_columns(table, Path(self.scratch.name) / "out", DAY)

    def test_a_truncated_column_file_is_refused(self):
        path = self.directory / "price.bin"
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaisesRegex(ColumnsError, "price.bin"):
            ColumnSet(self.directory)

    def test_a_manifest_out_of_form_is_refused(self):
        path = self.directory / "manifest.txt"
        path.write_text(path.read_text(encoding="ascii") + "x\n", encoding="ascii")
        with self.assertRaisesRegex(ColumnsError, "exact form"):
            read_manifest(self.directory)

    def test_the_day_comes_from_the_pin_label(self):
        self.assertEqual(TradingDay.from_pin("12302019").iso, "2019-12-30")


class Queries(Fixture):
    def test_locate_0_is_never_the_busiest(self):
        self.assertEqual(busiest_locate(self.columns, 0, OPEN_NS * 2), 7)

    def test_the_hour_is_clipped_to_the_slice(self):
        queries = {query.name: query for query in build_queries(self.columns, OPEN_NS + 100)}
        self.assertEqual(queries["Q2"].end_ns, OPEN_NS + 100)
        self.assertEqual(queries["Q3"].end_ns, OPEN_NS + 100)

    def test_a_full_day_clips_nothing(self):
        queries = {query.name: query for query in build_queries(self.columns)}
        hour = 3600 * 1_000_000_000
        for name in ("Q2", "Q3"):
            self.assertEqual((queries[name].start_ns, queries[name].end_ns),
                             (OPEN_NS, OPEN_NS + hour))
            self.assertNotIn("clipped", queries[name].text)
        self.assertEqual(queries["Q2"].locate, busiest_locate(self.columns, OPEN_NS,
                                                              OPEN_NS + hour))
        self.assertIn(f"all {RECORDS:,} records", queries["Q5"].text)
        facts = full_day_facts(self.columns, tuple(queries.values()))
        self.assertEqual(facts["q5_rows_scanned"], RECORDS)
        self.assertFalse(facts["clipped"])

    def test_a_table_written_in_pieces_is_the_table_written_whole(self):
        table = arrow_table(self.columns, DAY)
        out = Path(self.scratch.name) / "pieces"
        with ColumnWriter(out, DAY) as writer:
            for first, count in ((0, 1), (1, 0), (1, 3), (4, 2)):
                writer.append(table.slice(first, count))
        for path in sorted(self.directory.iterdir()):
            self.assertEqual(path.read_bytes(), (out / path.name).read_bytes(), path.name)

    def test_a_writer_that_fails_leaves_no_manifest(self):
        out = Path(self.scratch.name) / "failed"
        with self.assertRaises(ColumnsError), ColumnWriter(out, DAY) as writer:
            writer.append(arrow_table(self.columns, DAY).drop_columns(["rest"]))
        self.assertFalse((out / "manifest.txt").exists())

    def test_a_system_answer_in_another_row_order_still_matches(self):
        table = arrow_table(self.columns, DAY)
        query = Query("Qx", "rows", OPEN_NS, OPEN_NS + 3 * NANOS_PER_MINUTE, 7)
        truth = ground_truth(self.columns, query, DAY)
        wanted = pa.array([4, 2, 1, 5])
        answer = table.take(wanted)
        self.assertEqual(digest_result(query, answer), truth)
        self.assertEqual(truth["rows"], 4)

    def test_one_wrong_value_changes_the_digest(self):
        table = arrow_table(self.columns, DAY)
        query = Query("Qx", "rows", OPEN_NS, OPEN_NS + 3 * NANOS_PER_MINUTE, 7)
        truth = ground_truth(self.columns, query, DAY)
        answer = table.take(pa.array([1, 2, 4, 5]))
        index = answer.schema.get_field_index("price")
        wrong = pa.array([10, 20, 0, 1], pa.int32())
        digest = rows_digest(answer.set_column(index, "price", wrong))
        with self.assertRaises(QueryMismatch):
            require_truth(query, digest, truth, "test")

    def test_a_missing_row_changes_the_digest(self):
        table = arrow_table(self.columns, DAY)
        query = Query("Qx", "rows", OPEN_NS, OPEN_NS + 3 * NANOS_PER_MINUTE, 7)
        truth = ground_truth(self.columns, query, DAY)
        with self.assertRaises(QueryMismatch):
            require_truth(query, rows_digest(table.take(pa.array([1, 2, 4]))), truth, "test")

    def test_the_minute_aggregate(self):
        query = Query("Qx", "minute_aggregate", OPEN_NS, OPEN_NS + 3 * NANOS_PER_MINUTE, 7)
        expected = [[OPEN_NS + DAY.midnight_ns, 2, 300, 10, 20]]
        self.assertEqual(ground_truth(self.columns, query, DAY), aggregate_digest(expected))

    def test_the_column_sum(self):
        query = Query("Qx", "column_sum", 0, 0)
        self.assertEqual(ground_truth(self.columns, query, DAY),
                         aggregate_digest([[RECORDS, 650]]))


if __name__ == "__main__":
    unittest.main(verbosity=1)
