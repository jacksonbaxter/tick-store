#!/usr/bin/env python3
"""Estimate the whole store's size from tick_codec_eval results, for two layouts.

    python3 -I scripts/store_estimate.py OUTPUT.json EVAL.json [EVAL.json ...]
        [--capacity results/fullday-capacity-12302019.json] [--pins scripts/data_pins.json]

This is an estimate, not a store: no store file is written. For every field an ITCH day
needs to be re-serialised exactly, it takes the smallest exact codec tick_codec_eval
measured on the column that holds it, block headers and per-stream counts included, and
adds an index entry per block and the exceptions a derived field needs. Where a field has
more than one representation (references as distances or as values, locates stored for
every message or only for messages that name no order), each is costed and the smallest
taken. The two layouts:

  file_order  every column in file order, one column per field and message type, with the
              stock locate stored.
  per_locate  timestamps and the add orders' price, reference, shares and side in one
              stream per stock locate, the file-order locate column stored to rebuild file
              order, and every other column as in file_order, since only those columns
              were measured per locate.
  mixed       each of those columns in whichever of the two orders is smaller. Columns are
              read apart, and the stored locate column tells a reader which per-locate
              stream holds a row's next value, so one column's order need not be another's.

Each is costed twice: with the codecs alone, and with zstd level 1 taken per block where
it pays (on the encoded block, or on the raw values), each block paying a byte to say
which. An EVAL.json may be from any block size; the estimate says which.

Standard library only.
"""

import argparse
import json
import sys
from pathlib import Path

SCHEMA = "tick-store-store-estimate-1"
# An index entry per block of every column: its file offset, its first row and its first
# key, 8 bytes each. A guess at a plain layout, stated so it can be checked.
INDEX_BYTES_PER_BLOCK = 24
# A reference the book cannot resolve is stored whole, and with its locate where the
# locate would have come from the order.
RAW_REFERENCE_BYTES = 8
RAW_LOCATE_BYTES = 2
# A derived field that differs from what it derives from is stored as an exception: the
# message's position and the field.
EXCEPTION_POSITION_BYTES = 4
STOCK_BYTES = 8
TYPE_SLOTS = "SRHYLVWKJhAFECXDUPQBINO?"
NAMING_TYPES = "ECXDU"
CR1_FLOOR = {"12302019": 2.3415}
CR2_SYSTEMS = ("parquet", "arcticdb", "questdb")
LAYOUTS = ("file_order", "per_locate", "mixed")


def best_codec(column, with_zstd):
    """The smallest exact way to store a column: (bytes, blocks, how). Bytes include block
    headers and per-stream counts, as tick_codec_eval reports them."""
    if column["values"] == 0:
        return 0, 0, "empty"
    metadata = column["codecs"][0]["metadata_bytes"] if column["codecs"] else 0
    options = []
    for codec in column["codecs"]:
        if not codec["exact"]:
            continue
        options.append((codec["stored_bytes"], codec["blocks"], codec["codec"]))
        if with_zstd:
            options.append((codec["zstd1_or_plain_bytes"] + metadata, codec["blocks"],
                            f"{codec['codec']}+zstd1-where-smaller"))
    if with_zstd and column.get("raw_zstd1_mismatches", 1) == 0 and column["codecs"]:
        options.append((column["raw_zstd1_bytes"] + metadata, column["codecs"][0]["blocks"],
                        "raw-zstd1"))
    if not options:
        raise SystemExit(f"column {column['name']} has no exact codec")
    return min(options)


def references_by_type(analysis):
    return {r["type"]: r for r in analysis["reference_locality"]}


def exceptions(analysis):
    """Bytes the derived fields need beyond their columns, by what they are."""
    out = {}
    stock = analysis["stock_fields"]
    differing = sum(stock[t]["differ_from_directory"] for t in "AFPQI" if t in stock)
    out["stock_differs_from_directory"] = differing * (EXCEPTION_POSITION_BYTES + STOCK_BYTES)
    raw = analysis["raw_unknown_type"] + analysis["raw_wrong_length"]
    # A raw record keeps its whole payload; none were seen on either day, and the cost of
    # one is not modelled beyond its length.
    out["length_prefix_off_spec"] = raw * 2
    return out


def groups(layout):
    """Each field group with its alternatives, each a list of columns plus a tag for the
    exceptions it needs."""
    if layout == "mixed":
        merged = []
        for (field, file_order), (_, per_locate) in zip(groups("file_order"),
                                                        groups("per_locate")):
            merged.append((field, file_order + [a for a in per_locate if a not in file_order]))
        return merged
    distances = [f"ref_distance_{t}" for t in NAMING_TYPES]
    locate_distances = [f"ref_locate_distance_{t}" for t in NAMING_TYPES]
    per_locate = layout == "per_locate"
    return [
        ("message type", [(["message_type"], None)]),
        ("tracking number", [(["tracking_number"], None), (["tracking_number_per_type"], None)]),
        ("timestamp", [(["timestamp_per_locate" if per_locate else "timestamp"], None)]),
        ("stock locate and the references of E, C, X, D and U", [
            (["stock_locate"] + locate_distances, "unresolved"),
            (["stock_locate_unreferenced"] + distances, "unresolved_and_locate"),
            (["stock_locate"] + distances, "unresolved"),
            (["stock_locate", "referenced_ref"], None),
            (["stock_locate", "referenced_ref_per_type"], None),
        ]),
        ("A and F reference, U new reference", [
            (["add_ref_per_locate", "replace_new_ref"], None),
            (["add_ref_per_locate", "replace_new_minus_original"], None),
        ] if per_locate else [
            (["creation_ref"], None),
            (["add_ref", "replace_new_ref"], None),
            (["add_ref", "replace_new_minus_original"], None),
            (["add_ref_vs_running_max", "replace_new_ref"], None),
            (["add_ref_vs_running_max", "replace_new_minus_original"], None),
        ]),
        ("A and F price", [(["add_price_per_locate"], None), (["add_price_per_locate_side"], None)]
         if per_locate else [(["add_price"], None)]),
        ("A and F shares", [(["add_shares_per_locate" if per_locate else "add_shares"], None)]),
        ("A and F side", [(["add_side_per_locate" if per_locate else "add_side"], None)]),
        ("F attribution", [(["mpid"], None)]),
        ("E, C and X shares", [
            (["executed_shares", "executed_with_price_shares", "cancelled_shares"], None),
            (["reduction_shares_vs_remaining"], None),
        ]),
        ("match number of E, C, P, Q and B", [(["match_number"], None)]),
        ("C printable", [(["printable"], None)]),
        ("C price", [(["executed_price"], None)]),
        ("U shares", [(["replace_shares"], None)]),
        ("U price", [(["replace_price"], None)]),
        ("P reference", [(["trade_ref"], None)]),
        ("P side", [(["trade_side"], None)]),
        ("P shares", [(["trade_shares"], None)]),
        ("P price", [(["trade_price"], None)]),
        ("Q shares", [(["cross_shares"], None)]),
        ("Q price", [(["cross_price"], None)]),
        ("Q cross type", [(["cross_type"], None)]),
        ("I shares", [(["imbalance_shares"], None)]),
        ("I prices", [(["imbalance_price"], None)]),
        ("I flags", [(["imbalance_flags"], None)]),
        ("V levels", [(["mwcb_level"], None)]),
        ("rest of S, R, H, Y, L, W, K, J, h, N and O", [(["rare_rest_bytes"], None)]),
    ]


def alternative_cost(columns, tag, by_name, analysis, with_zstd):
    parts = []
    total = 0
    blocks = 0
    for name in columns:
        if name not in by_name:
            raise SystemExit(f"the results have no column {name}: run tick_codec_eval on "
                             "every column")
        size, column_blocks, how = best_codec(by_name[name], with_zstd)
        column = by_name[name]
        parts.append({"column": name, "how": how, "values": column["values"], "bytes": size,
                      "blocks": column_blocks,
                      "bits_per_value": size * 8 / column["values"] if column["values"] else 0})
        total += size
        blocks += column_blocks
    extra = 0
    if tag is not None:
        refs = references_by_type(analysis)
        unresolved = sum(refs[t]["unresolved"] for t in NAMING_TYPES)
        extra += unresolved * RAW_REFERENCE_BYTES
        if tag == "unresolved_and_locate":
            mismatched = sum(refs[t]["locate_mismatches"] for t in NAMING_TYPES)
            extra += unresolved * RAW_LOCATE_BYTES
            extra += mismatched * (EXCEPTION_POSITION_BYTES + RAW_LOCATE_BYTES)
    index = blocks * INDEX_BYTES_PER_BLOCK
    return {"columns": parts, "column_bytes": total, "exception_bytes": extra,
            "blocks": blocks, "index_bytes": index, "bytes": total + extra + index}


def estimate_layout(result, layout, with_zstd):
    by_name = {c["name"]: c for c in result["columns"]}
    analysis = result["analysis"]
    fields = []
    total = 0
    for field, alternatives in groups(layout):
        costed = [alternative_cost(cols, tag, by_name, analysis, with_zstd)
                  for cols, tag in alternatives]
        chosen = min(costed, key=lambda a: a["bytes"])
        fields.append({"field": field, "chosen": chosen,
                       "alternatives_bytes": [a["bytes"] for a in costed]})
        total += chosen["bytes"]
    derived = exceptions(analysis)
    total += sum(derived.values())
    return {"layout": layout, "with_zstd1": with_zstd, "fields": fields,
            "derived_field_exception_bytes": derived, "total_bytes": total}


def judge(day, total, pins, capacity):
    pin = pins["days"].get(day)
    out = {}
    if pin is not None:
        ratio = pin["decompressed_size"] / total
        out["ratio_to_source"] = ratio
        out["source_gz_ratio"] = pin["decompressed_size"] / pin["gz_size"]
        if day in CR1_FLOOR:
            out["CR-1"] = {"what": "Whole store ratio beats the pinned file's own .gz: at "
                                   "least 2.3415 on 12302019",
                           "ratio_at_least": CR1_FLOOR[day], "met": ratio >= CR1_FLOOR[day]}
    if capacity is not None and capacity["day"] == day:
        systems = {}
        for name in CR2_SYSTEMS:
            stored = capacity["systems"][name]["stored_bytes"]
            systems[name] = {"stored_bytes": stored, "estimate_over_system": total / stored,
                             "smaller": total < stored}
        out["CR-2"] = {"what": "Whole store is smaller than Parquet, ArcticDB and QuestDB, "
                               "each tuned per its docs",
                       "systems": systems, "met": all(s["smaller"] for s in systems.values())}
    return out


def estimate(results, pins, capacity):
    days = []
    for result in results:
        if result.get("columns_subset") is not None:
            raise SystemExit(f"{result['day']}: the results cover a subset of columns; the "
                             "estimate needs a run of every column")
        if not result["every_value_exact"]:
            raise SystemExit(f"{result['day']}: not every value came back exact")
        layouts = []
        for layout in LAYOUTS:
            for with_zstd in (False, True):
                costed = estimate_layout(result, layout, with_zstd)
                costed["judged"] = judge(result["day"], costed["total_bytes"], pins, capacity)
                layouts.append(costed)
        days.append({"day": result["day"], "block_values": result["block_values"],
                     "commit": result["commit"], "messages": result["messages"],
                     "layouts": layouts})
    return {"schema": SCHEMA, "estimate": True,
            "what": "An estimate of the whole store from per-column measurements, not a "
                    "store. See scripts/store_estimate.py for what is counted.",
            "index_bytes_per_block": INDEX_BYTES_PER_BLOCK, "days": days}


def summary_lines(document):
    lines = []
    for day in document["days"]:
        for layout in day["layouts"]:
            judged = layout["judged"]
            cr1 = judged.get("CR-1", {}).get("met")
            cr2 = judged.get("CR-2", {}).get("met")
            lines.append(
                f"{day['day']} block {day['block_values']:>5} {layout['layout']:<10} "
                f"{'zstd1' if layout['with_zstd1'] else 'codecs':<6} "
                f"{layout['total_bytes']:>14,} bytes  ratio {judged.get('ratio_to_source', 0):.4f}"
                f"  CR-1 {cr1}  CR-2 {cr2}")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output")
    parser.add_argument("results", nargs="+")
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--pins", default=str(root / "scripts" / "data_pins.json"))
    parser.add_argument("--capacity",
                        default=str(root / "results" / "fullday-capacity-12302019.json"))
    args = parser.parse_args(argv)
    results = [json.loads(Path(p).read_text()) for p in args.results]
    pins = json.loads(Path(args.pins).read_text())
    capacity = json.loads(Path(args.capacity).read_text()) if args.capacity else None
    document = estimate(results, pins, capacity)
    Path(args.output).write_text(json.dumps(document, indent=2) + "\n")
    print("\n".join(summary_lines(document)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
