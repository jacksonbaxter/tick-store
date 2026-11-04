#!/usr/bin/env python3
"""Assemble the full-day ITCH store results from the raw outputs of one day's runs.

    python3 -I scripts/store_results.py <raw-dir> --day DAY --commit SHA --host HOST \\
        --capacity results/fullday-capacity-12302019.json \\
        --roundtrip results/roundtrip-DAY.json --size results/store-size-DAY.json

<raw-dir> holds what tick_roundtrip printed and what /usr/bin/time -l wrote, one run per pair:

    ingest.json ingest.time      ingest, with --durability
    layout.json                  layout of the ingested store
    verify.json verify.time      G-1: verify against the pin
    g2.json                      the flips: {"seed", "data": {...}, "footer": {...}}
    g2-data-crc.json .err .time      verify, data block flipped, CRCs on
    g2-data-nocrc.json .err .time    the same with --no-checksums
    g2-footer-crc.json .err .time    verify, footer flipped, CRCs on
    g2-footer-nocrc.json .err .time  the same with --no-checksums
    g2-restored.json .time           verify after both flips are undone

Every value comes from those files; nothing is estimated. It refuses, exit 1, a G-1 that did not
pass, a G-2 run that did not exit 1 where the store was corrupt or did not pass once restored, a
CRC failure that does not name the flipped block and its offset, and a layout whose parts do not
add up to the file. CR-1 is judged on the day BENCHMARKS.md gives it (12302019); CR-2 against
the stored bytes of the comparison file, which holds that day only.

Standard library only.
"""

import argparse
import json
import re
import sys
from pathlib import Path

# BENCHMARKS.md CR-1: the pinned file's own .gz ratio, rounded up, on 12302019. The second
# day's .gz ratio is context, not a target.
CR1_FLOOR = {"12302019": 2.3415}
GZ_RATIO = {"12302019": 8_251_407_909 / 3_524_013_057, "10302019": 9_035_016_843 / 3_872_931_242}
CR2_SYSTEMS = ("parquet", "arcticdb", "questdb")


class Refused(Exception):
    pass


def load(folder, name):
    path = Path(folder) / name
    if not path.is_file():
        raise Refused(f"missing {name}")
    return path


def read_json(folder, name):
    return json.loads(load(folder, name).read_text())


def read_text(folder, name):
    return load(folder, name).read_text()


def time_figures(folder, name):
    """Real seconds, peak resident bytes and, on macOS, the peak memory footprint: from BSD
    time -l, or from GNU time -v, whose peak is in kilobytes."""
    text = read_text(folder, name)
    real = re.search(r"^\s*([0-9.]+) real", text, re.M)
    rss = re.search(r"^\s*([0-9]+)\s+maximum resident set size", text, re.M)
    footprint = re.search(r"^\s*([0-9]+)\s+peak memory footprint", text, re.M)
    if real and rss:
        return {"real_seconds": float(real.group(1)), "peak_rss_bytes": int(rss.group(1)),
                "peak_memory_footprint_bytes": int(footprint.group(1)) if footprint else None}
    wall = re.search(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([0-9:.]+)", text)
    kilobytes = re.search(r"Maximum resident set size \(kbytes\): ([0-9]+)", text)
    if wall and kilobytes:
        seconds = 0.0
        for part in wall.group(1).split(":"):
            seconds = seconds * 60 + float(part)
        return {"real_seconds": seconds, "peak_rss_bytes": int(kilobytes.group(1)) * 1024,
                "peak_memory_footprint_bytes": None}
    raise Refused(f"{name} holds no time figures")


def run(folder, stem, timed=True):
    out = read_json(folder, f"{stem}.json")
    out.pop("tool", None)
    if timed:
        figures = time_figures(folder, f"{stem}.time")
        out["real_seconds"] = figures["real_seconds"]
        out["time_peak_rss_bytes"] = figures["peak_rss_bytes"]
        out["peak_memory_footprint_bytes"] = figures["peak_memory_footprint_bytes"]
    return out


def layout_sizes(layout):
    """Every byte of the file by part, checked to add up to the file."""
    container = layout["container"]
    footer = {
        "fixed": container["footer_fixed_bytes"],
        "block_entries": container["block_entry_bytes"],
        "section_entries": container["section_entry_bytes"],
        "block_metadata": container["block_metadata_bytes"],
    }
    columns = {c["column"]: {"payload_bytes": c["payload_bytes"],
                             "metadata_bytes": c["metadata_bytes"], "blocks": c["blocks"],
                             "zstd_blocks": c["zstd_blocks"], "values": c["values"]}
               for c in layout["columns"]}
    payload = sum(c["payload_bytes"] for c in columns.values())
    total = (container["header_bytes"] + container["trailer_bytes"] + sum(footer.values())
             + sum(layout["sections"].values()) + payload)
    if total != layout["file_bytes"]:
        raise Refused(f"layout parts add to {total}, the file is {layout['file_bytes']}")
    if sum(c["metadata_bytes"] for c in columns.values()) != footer["block_metadata"]:
        raise Refused("column metadata does not add to the footer's metadata")
    return footer, columns, payload


def g2_runs(folder, flips, pin):
    data, footer = flips["data"], flips["footer"]
    corrupt = {
        "data_crc": run(folder, "g2-data-crc"),
        "data_no_checksums": run(folder, "g2-data-nocrc"),
        "footer_crc": run(folder, "g2-footer-crc"),
        "footer_no_checksums": run(folder, "g2-footer-nocrc"),
    }
    for name, result in corrupt.items():
        if result["verdict"] != "mismatch":
            raise Refused(f"G-2 {name}: verify passed on a corrupt store")
        stem = "g2-" + name.replace("_no_checksums", "-nocrc").replace("_crc", "-crc")
        result["stderr"] = read_text(folder, f"{stem}.err").strip()
    restored = run(folder, "g2-restored")
    if restored["verdict"] != "pass" or restored.get("stored_sha256") != pin:
        raise Refused("G-2: verify did not pass on the restored store")
    error = corrupt["data_crc"].get("store_error") or ""
    wanted = ("bad checksum: block", f"block {data['block']}",
              f"at offset {data['block_payload_offset']}")
    if not all(part in error for part in wanted):
        raise Refused(f"G-2: the data flip's CRC failure does not name the block: {error!r}")
    if "bad checksum: footer" not in (corrupt["footer_crc"].get("store_error") or ""):
        raise Refused("G-2: the footer flip was not a footer CRC failure")
    unchecked = corrupt["data_no_checksums"]
    first = unchecked.get("first_difference") or {}
    located = (first.get("kind") == "bytes" or
               f"column {data['column']}" in (unchecked.get("store_error") or ""))
    if not located:
        raise Refused("G-2: with CRCs off the data flip was located neither by record nor field")
    return {"gate": "G-2", "seed": flips["seed"], "injected": {"data": data, "footer": footer},
            "exit_code_corrupt": 1, "exit_code_restored": 0, **corrupt, "restored": restored}


def g1(folder, timed=True):
    ingest = run(folder, "ingest", timed)
    verify = run(folder, "verify", timed)
    pin = verify.get("expected_sha256")
    if verify["verdict"] != "pass" or not pin or verify.get("stored_sha256") != pin:
        raise Refused("G-1 did not pass against the pin")
    return ingest, verify, pin


def assemble_g1(folder, day, commit, host):
    """G-1 alone, for a host that runs only that gate. Without time files, the peaks are the
    tool's own, getrusage's ru_maxrss, which on Linux is VmHWM."""
    ingest, verify, _ = g1(folder, timed=(Path(folder) / "verify.time").is_file())
    return {"day": day, "source": ingest_source(day), "host": host, "build_preset": "bench",
            "commit": commit, "store_format": verify["store_format"],
            "durability": ingest["durability"], "segment_records": ingest["segment_records"],
            "g1": {"gate": "G-1", "verdict": "pass", "ingest": ingest, "verify": verify}}


def assemble(folder, day, commit, host, capacity):
    ingest, verify, pin = g1(folder)
    layout = read_json(folder, "layout.json")
    flips = read_json(folder, "g2.json")
    if ingest["stored_file_bytes"] != layout["file_bytes"]:
        raise Refused("the layout is not of the ingested file")
    footer, columns, payload = layout_sizes(layout)
    stored = layout["file_bytes"]
    source = ingest["bytes"]
    ratio = source / stored
    import_ref = columns.get("import_ref", {})

    cr1 = {"target": "CR-1", "floor": CR1_FLOOR.get(day), "gz_ratio": GZ_RATIO.get(day)}
    if day in CR1_FLOOR:
        cr1["verdict"] = "pass" if ratio >= CR1_FLOOR[day] else "fail"
    else:
        cr1["verdict"] = "not a CR-1 day"
        cr1["note"] = "CR-1 is set on 12302019; this day's own .gz ratio is given as context"
    cr2 = {"target": "CR-2"}
    if capacity is not None and capacity.get("day") == day:
        cr2["source"] = "results/fullday-capacity-12302019.json"
        cr2["note"] = ("the comparison systems' stored bytes are their full-day stores on the "
                       "debian host, from the capacity check; the ITCH store's bytes depend on "
                       "the records alone, the same on either host")
        systems = {}
        for name in CR2_SYSTEMS:
            entry = capacity["systems"][name]
            systems[name] = {"variant": entry["variant"], "stored_bytes": entry["stored_bytes"],
                             "outcome": entry["outcome"],
                             "ours_over_theirs": stored / entry["stored_bytes"],
                             "smaller": stored < entry["stored_bytes"]}
        cr2["systems"] = systems
        cr2["verdict"] = "pass" if all(s["smaller"] for s in systems.values()) else "fail"
    else:
        cr2["verdict"] = "no comparison"
        cr2["note"] = "the comparison systems were stored for 12302019 only"

    common = {"day": day, "source": ingest_source(day), "host": host, "build_preset": "bench",
              "commit": commit, "store_format": layout["store_format"]}
    size = {
        **common,
        "segment_records": layout["segment_records"],
        "segments": layout["segments"],
        "records": layout["records"],
        "source_bytes": source,
        "stored_bytes": stored,
        "ratio": ratio,
        "parts": {
            "container_header": layout["container"]["header_bytes"],
            "container_trailer": layout["container"]["trailer_bytes"],
            "footer": footer,
            "footer_total": sum(footer.values()),
            "sections": layout["sections"],
            "column_payloads": payload,
        },
        "segment_state": {
            "what": "the import list, the one thing a segment needs from outside its own "
                    "records to decode on its own; there is no other checkpoint",
            "import_ref_payload_bytes": import_ref.get("payload_bytes", 0),
            "import_ref_metadata_bytes": import_ref.get("metadata_bytes", 0),
            "imports": import_ref.get("values", 0),
        },
        "columns": columns,
        "cr1": cr1,
        "cr2": cr2,
    }
    roundtrip = {
        **common,
        "durability": ingest["durability"],
        "segment_records": ingest["segment_records"],
        "peak_rss_note": ("time -l's maximum resident set size counts the clean file-backed "
                          "pages of the read-only mappings of source and store; "
                          "peak_memory_footprint_bytes is what the process held beyond them"),
        "g1": {"gate": "G-1", "verdict": "pass", "ingest": ingest, "verify": verify},
        "g2": g2_runs(folder, flips, pin),
    }
    return roundtrip, size


def ingest_source(day):
    return f"{day}.NASDAQ_ITCH50"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("raw")
    parser.add_argument("--day", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--capacity")
    parser.add_argument("--roundtrip", required=True)
    parser.add_argument("--size")
    parser.add_argument("--g1-only", action="store_true",
                        help="G-1 alone, into --roundtrip; no G-2 and no sizes")
    args = parser.parse_args(argv)
    capacity = json.loads(Path(args.capacity).read_text()) if args.capacity else None
    if args.g1_only:
        try:
            roundtrip = assemble_g1(args.raw, args.day, args.commit, args.host)
        except Refused as refused:
            print(f"refused: {refused}", file=sys.stderr)
            return 1
        Path(args.roundtrip).write_text(json.dumps(roundtrip, indent=2) + "\n")
        print(f"{args.day}: G-1 pass on {args.host}")
        return 0
    if not args.size:
        parser.error("--size is required without --g1-only")
    try:
        roundtrip, size = assemble(args.raw, args.day, args.commit, args.host, capacity)
    except Refused as refused:
        print(f"refused: {refused}", file=sys.stderr)
        return 1
    Path(args.roundtrip).write_text(json.dumps(roundtrip, indent=2) + "\n")
    Path(args.size).write_text(json.dumps(size, indent=2) + "\n")
    print(f"{args.day}: {size['stored_bytes']} bytes, ratio {size['ratio']:.4f}, "
          f"CR-1 {size['cr1']['verdict']}, CR-2 {size['cr2']['verdict']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
