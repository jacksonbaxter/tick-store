#!/usr/bin/env python3
"""Generate the results tables of RESULTS.md and the README's headline from the results files.

    python3 -I scripts/render_results.py           rewrite both files between their markers
    python3 -I scripts/render_results.py --check   exit 1 if either file is stale

The tables are never typed. RESULTS.md holds every generated results table: the targets block,
the benchmark block and the comparison block. README.md holds only the headline block, a few
rows of the targets block. --check regenerates every block of both files and compares, so a hand
edit inside any pair of markers shows as stale. A README that still holds a RESULTS.md block,
or a file without the markers it needs, exits 2.

The benchmark block gives each host its own table, because numbers from the two hosts are never
mixed, and every file must pass check_results.py to appear.

The comparison block comes from results/compare/ and every file there must pass check_compare.py.
Each file has its own tables, a full day's first, then each slice's, labelled slice only, so a
full-day number and a slice number never share a table. Each host has its own tables, and so does
each durability class: ingest in class A and ingest in class B never share a table, and queries
are on each system's class B store, as the harness runs them. Each system shows its headline
variant; a full day also lists every variant it ran, and each system's peak memory and swap per
phase. Each latency's tail figure is named for what it is, from the file: at 30 warm and 7 cold
runs the 99th percentile by nearest rank is the slowest run, so a column reads "slowest of 30",
never p99, and a file whose figure is not what its n makes it stops the render. A timed
number with any page swapped in while it ran carries "(swap-in)". A tick-store-only full day,
on either host, is labelled so, and ranks nothing, since no other system ran; its peak memory is
BSD time -l's on macOS and the guard's on Linux, where the guard watched it. A target row never
reads a tick-store-only day for the four-system day, or the reverse, by its name
("-full-tickstore-") and its scope alike.

The targets block has one row for every target of BENCHMARKS.md on each host it states: the
target, the measured value read from a committed results file, met, missed or not judged, and the
file. scripts/targets.json says where each value is and how it is judged, and this script computes
every verdict from the value and the target; a row with no figure gives the reason the
definitions state. A file a row needs that is missing, or a field that is not in it, stops the
render rather than leave a gap.

The headline block is the README's first screen below the pitch: a table of the rows
scripts/targets.json lists under "headline", each a target row of the targets block, or some of
its parts, judged by the same code, so every verdict in it is the targets block's own. Each row
says what it is in plain words, then its bar and the bar's source, then what was measured, from
a short template whose every figure is a named value read from the results files of the parts
shown; a template that types a figure, names a value it is not given, or is given a value it
does not name stops the render. A sentence under the table gives another row's figure and
verdict the same way. The line under that names, in plain words, every part of a target on the
screen that the screen leaves out, and every other target, that is missed or not judged, and
counts the met ones; it is checked against the targets apart from the code that writes it, by
verdict classes of its own from the parts' raw verdicts, each name read back whole under its
machine and each target asked for under its own count, and so is the verdict of the sentence
under the table; a headline with no missed target stops the render. A value that is the lowest
of the other systems takes every other system the comparison ran. The prose above the
headline, the pitch, is written by hand: every figure it states is one scripts/targets.json
lists under "pitch" and is checked against the results, or a text listed there with its reason
for being no result, found once, so --check stops on a pitch figure the results no longer give.
Each heading and sentence of it is a claim, a whole sentence listed there whose conditions must
hold on the results, which holds the words its target lists for its bar and names the one
machine it is judged on; a sentence holding a figure; or a text listed as stating no result,
with its reason. A word that ranks or judges must sit inside a claim or a figure's words, and
the pitch, headings included, runs to at most "max_words" words. The whole first screen names no
target id and no query's name, holds no character a reader cannot see, ends at the next heading
after the headline, and introduces each host once by what it is, never calling it by its
operating system after. What none of this checks is that the prose is true beyond its figures
and claims: a figure sentence or a sentence listed as stating no result is read for ranking
words only, and a claim's words beyond those its target lists are held as written.

G-3's file, results/fuzz-g3-<commit>.json, must pass check_fuzz.py first, against the fuzz
targets fuzz/CMakeLists.txt defines: one that leaves a target out, holds a clean run under
1,800 s, or whose commit is older than the fuzzed code, stops the render. Where git has no
history to tell the last, check_fuzz.py says so, the render goes on, and this exits
EXIT_UNJUDGED once it is done, which scripts/check.sh reports as a skip. Each results/roundtrip-*.json, which G-1 and G-2 read, must pass
check_roundtrip.py, so G-2's exit codes are the ones its runs exited with.
"""

import argparse
import fnmatch
import json
import operator
import re
import statistics
import string
import subprocess
import sys
import unicodedata
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_fuzz  # noqa: E402

CHECK_RESULTS = REPO_ROOT / "scripts" / "check_results.py"
CHECK_COMPARE = REPO_ROOT / "scripts" / "check_compare.py"
CHECK_FUZZ = REPO_ROOT / "scripts" / "check_fuzz.py"
CHECK_ROUNDTRIP = REPO_ROOT / "scripts" / "check_roundtrip.py"
DEFAULT_README = REPO_ROOT / "README.md"
DEFAULT_RESULTS_MD = REPO_ROOT / "RESULTS.md"
DEFAULT_RESULTS = REPO_ROOT / "results" / "bench"
DEFAULT_COMPARE = REPO_ROOT / "results" / "compare"
DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"
DEFAULT_CAPACITY = REPO_ROOT / "results" / "fullday-capacity-12302019.json"
DEFAULT_TARGETS = REPO_ROOT / "scripts" / "targets.json"

BEGIN = "<!-- results:begin, generated by scripts/render_results.py, do not edit -->"
END = "<!-- results:end -->"
COMPARE_BEGIN = "<!-- compare:begin, generated by scripts/render_results.py, do not edit -->"
COMPARE_END = "<!-- compare:end -->"
TARGETS_BEGIN = "<!-- targets:begin, generated by scripts/render_results.py, do not edit -->"
TARGETS_END = "<!-- targets:end -->"
TARGET_COLUMNS = ("Target", "Host", "Target value", "Measured", "Verdict", "Results file")
HEADLINE_BEGIN = "<!-- headline:begin, generated by scripts/render_results.py, do not edit -->"
HEADLINE_END = "<!-- headline:end -->"
HEADLINE_COLUMNS = ("What", "Bar (source)", "Measured", "Result")
# Where the README's headline links for the full targets block.
TARGETS_ANCHOR = "RESULTS.md#targets"
# The blocks that belong in RESULTS.md, never in the README.
RESULTS_MD_BLOCKS = (("targets", TARGETS_BEGIN), ("benchmark", BEGIN),
                     ("comparison", COMPARE_BEGIN))
COMPARATORS = {"at_least": (operator.ge, "at least"), "at_most": (operator.le, "at most"),
               "above": (operator.gt, "above"), "below": (operator.lt, "below"),
               "equals": (operator.eq, "equal to")}
SYSTEM_ORDER = ("parquet", "arcticdb", "questdb", "tickstore")
SYSTEM_LABEL = {"parquet": "Parquet", "arcticdb": "ArcticDB", "questdb": "QuestDB",
                "tickstore": "tick-store"}
QUERY_NAMES = ("Q1", "Q2", "Q3", "Q4", "Q5")
INGEST_COLUMNS = ("System", "Variant", "Total s, median", "Engine-only s, median", "M records/s",
                  "Stored bytes", "Ratio", "Proof", "Like-for-like")
# What a write's two times hold, as compare/README.md's "A write's two times" has it per system.
WRITE_TIMES = ("\"Total s\" is the whole write clock; \"engine-only s\" is that less the time "
               "reading the input and putting it in the form the system's call takes, as "
               "\"A write's two times\" in compare/README.md sets out for each system. "
               "tick-store's both hold its ITCH parsing. A write of several repetitions measured "
               "before protocol 2 kept medians alone, so its engine-only seconds are not "
               "recorded.")
# docs/NOTES.md, "macOS st_blocks and QuestDB's converted partition": the same Parquet bytes
# allocate about a fifth more on APFS when written as the harness writes them.
MACOS_ALLOCATION = ("On macOS, APFS can keep blocks allocated past the end of a file that grew "
                    "in many small writes over time, so a stored figure here can exceed the "
                    "file's own bytes: compare stored sizes from Linux.")
VARIANT_COLUMNS = ("System", "Variant", "Stored bytes, B", "Ratio", "Total write s, A / B",
                   *(f"{name} ms, warm / cold" for name in QUERY_NAMES))
MEMORY_COLUMNS = ("System", "Largest VmHWM", "Peak RSS of its processes together",
                  "MemAvailable, lowest", "Swap in / out over the run, MiB")
SWAP_PHASES = (("write", "Write"), ("read_back", "Read-back"), ("warm_queries", "Warm queries"),
               ("cold_queries", "Cold queries"))
SWAP_COLUMNS = ("System", *(f"{label}, in / out MiB" for _, label in SWAP_PHASES),
                "Timed queries with swap-in")
SWAP_MARK = " (swap-in)"

EXIT_STALE = 1
EXIT_USAGE = 2
# Everything passed, but G-3's file could not be tied to the code: check_fuzz.py's own code.
EXIT_UNJUDGED = 3

COLUMNS = ("Case", "Commit", "Passes", "Messages/s", "Spread (CV)", "Batch", "p50 µs", "p99 µs",
           "p99.9 µs", "Cycles/msg", "Instr/msg", "Branch misses/msg", "Load at start")
HOSTS = ("Linux", "macOS")
COLUMN_COLUMNS = ("Case", "Commit", "Kernels", "Values", "Bits/value", "ns/value",
                  "MB/s of 8-byte values", "Spread (CV)", "Cycles/value", "Instr/value",
                  "Branch misses/value", "Load at start")
KERNEL_COLUMNS = ("Kernel", "Set", "Commit", "Cases", "G values/s, median", "Range",
                  "Largest CV", "Cycles/value, median", "Instr/value, median",
                  "Branch misses/value, median", "Load at start")
WIDTH = re.compile(r"/w=\d+")


def percentile_us(latency, percentile):
    for point in latency["percentiles"]:
        if point["percentile"] == percentile:
            return f"{point['value_ns'] / 1000.0:.2f}"
    return "n/a"


def per_message(counters, name):
    value = counters.get("per_message", {}).get(name)
    return "n/a" if value is None else f"{value:.2f}"


def start_load(line):
    return line.split("load1=", 1)[1].split()[0]


def kind_of(document):
    return document.get("input", {}).get("kind", "slice")


def starts_of(document):
    """Each case's start load, in the order of its results."""
    return [start_load(line) for line in document["header"]["load_lines"][0::2]]


def per_value(counters, name):
    value = counters.get("per_value", {}).get(name)
    return None if value is None else value


def number_or_na(value, digits=2):
    return "n/a" if value is None else f"{value:.{digits}f}"


def header_line(host, document, what):
    header = document["header"]
    return (f"**{host}, {what}.** {header['cpu_model']}, {header['compiler']}, "
            f"`{header['compile_flags']}`. {header['statement']}")


def column_table(host, document):
    """Real-data column decodes, one row a case: values per second and the column's size."""
    lines = [header_line(host, document, "real-data column decodes of the slice"), "",
             "| " + " | ".join(COLUMN_COLUMNS) + " |", "|" + "---|" * len(COLUMN_COLUMNS)]
    commit = document["header"]["commit"][:12]
    for result, start in zip(document["results"], starts_of(document)):
        rate = result["throughput"]["values_per_second"]
        counters = result["counters"]
        cells = (f"`{result['case']}`", f"`{commit}`", result["isa"].split(", ", 1)[-1],
                 f"{result['verification']['values']:,}",
                 f"{result['ratio']['bits_per_value']:.2f}", f"{1e9 / rate['median']:.3f}",
                 f"{rate['median'] * 8 / 1e6:,.0f}", f"{rate['cv'] * 100:.2f}%",
                 number_or_na(per_value(counters, "cycles")),
                 number_or_na(per_value(counters, "instructions")),
                 number_or_na(per_value(counters, "branch_misses"), 3), start)
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def median_or_none(values):
    present = [value for value in values if value is not None]
    return statistics.median(present) if present else None


def kernel_table(host, document):
    """Seeded kernel cases, one row a kernel and set: the median across its widths."""
    groups = {}
    for result, start in zip(document["results"], starts_of(document)):
        family, _, kernel_set = WIDTH.sub("", result["case"]).rpartition("/")
        groups.setdefault((family, kernel_set), []).append((result, float(start)))
    lines = [header_line(host, document, "decode kernels on seeded L1-resident input"), "",
             "Each row is one kernel on one set. A kernel with a width ran at each width "
             "listed, one case each, and the row gives the median across them.", "",
             "| " + " | ".join(KERNEL_COLUMNS) + " |", "|" + "---|" * len(KERNEL_COLUMNS)]
    commit = document["header"]["commit"][:12]
    for (family, kernel_set), rows in groups.items():
        rates = [row["throughput"]["values_per_second"]["median"] / 1e9 for row, _ in rows]
        cvs = [row["throughput"]["values_per_second"]["cv"] for row, _ in rows]
        loads = [load for _, load in rows]

        def median_counter(name, rows=rows):
            return median_or_none([per_value(row["counters"], name) for row, _ in rows])

        cells = (f"`{family}`", kernel_set, f"`{commit}`", str(len(rows)),
                 f"{statistics.median(rates):.2f}", f"{min(rates):.2f} to {max(rates):.2f}",
                 f"{max(cvs) * 100:.2f}%", number_or_na(median_counter("cycles"), 3),
                 number_or_na(median_counter("instructions"), 3),
                 number_or_na(median_counter("branch_misses"), 4),
                 f"{min(loads):.2f} to {max(loads):.2f}")
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def table_for(host, documents):
    lines = [f"**{host}.** {documents[0]['header']['cpu_model']}, "
             f"{documents[0]['header']['compiler']}, `{documents[0]['header']['compile_flags']}`, "
             f"{documents[0]['header']['isa']}. {documents[0]['header']['statement']}", "",
             "| " + " | ".join(COLUMNS) + " |", "|" + "---|" * len(COLUMNS)]
    for document in documents:
        header = document["header"]
        starts = header["load_lines"][0::2]
        for result, start in zip(document["results"], starts):
            throughput = result["throughput"]
            rate = throughput["messages_per_second"]
            latency = result["latency"]
            counters = result["counters"]
            cells = (f"`{result['case']}`", f"`{header['commit'][:12]}`",
                     str(throughput["repetitions"]), f"{rate['median'] / 1e6:.2f} M",
                     f"{rate['cv'] * 100:.2f}%", f"{latency['batch_messages']} msgs",
                     percentile_us(latency, 50.0), percentile_us(latency, 99.0),
                     percentile_us(latency, 99.9), per_message(counters, "cycles"),
                     per_message(counters, "instructions"),
                     per_message(counters, "branch_misses"), start_load(start))
            lines.append("| " + " | ".join(cells) + " |")
    resolution = documents[0]["results"][0]["latency"]["timer_resolution_ns"]
    lines += ["", "Percentiles are the time for one whole batch, in microseconds, with the "
                  f"timer resolving {resolution:.2f} ns."]
    return lines


def render(results_dir):
    """The block between the markers, from every results file."""
    paths = sorted(results_dir.glob("*.json"))
    documents = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    if not documents:
        return ["No reported results yet."]
    lines = []
    for host in HOSTS:
        mine = [document for document in documents if document["header"]["host"] == host]
        messages = [document for document in mine if kind_of(document) == "slice"]
        if messages:
            lines += table_for(host, messages) + [""]
        for document in mine:
            if kind_of(document) == "column":
                lines += column_table(host, document) + [""]
        for document in mine:
            if kind_of(document) == "seeded":
                lines += kernel_table(host, document) + [""]
    return lines[:-1]


def milliseconds(microseconds):
    return f"{microseconds / 1000.0:,.2f}"


def headline(document, name):
    system = document["systems"][name]
    return system["headline_variant"], system["variants"][system["headline_variant"]]


def ordered_systems(document):
    return [name for name in SYSTEM_ORDER if name in document["systems"]]


def nearest_rank(count):
    """The nearest rank of the 99th percentile among `count` runs, from 1, as
    check_compare.py has it."""
    return max(1, -(-99 * count // 100))


def tail_name(block):
    """What a latency block's tail figure, its p99_us, is: the name a protocol 2 file gives it
    in `tail`, or for a file before protocol 2 the same name worked out from its n. The 99th
    percentile by nearest rank of 30 or of 7 runs is the slowest run, never a 1% tail."""
    named = block.get("tail")
    if named is not None:
        return named
    count = block.get("n")
    if isinstance(count, bool) or not isinstance(count, int) or count < 1:
        raise TargetError(f"a latency block with no n to name its tail figure: n is {count!r}")
    rank = nearest_rank(count)
    if rank < count:
        return f"99th percentile, nearest rank: run {rank} of {count}"
    if "max_us" in block and block.get("p99_us") != block["max_us"]:
        raise TargetError(f"the tail figure of {count} runs is the slowest by nearest rank, and "
                          f"p99_us {block.get('p99_us')} is not max_us {block['max_us']}")
    return f"slowest of {count}"


def one_tail(blocks, what):
    """The one tail figure name a set of blocks shares, so a column or a comparison never
    mixes two."""
    names = {tail_name(block) for block in blocks}
    if len(names) != 1:
        raise TargetError(f"{what}: tail figures of different kinds, {sorted(names)}")
    return names.pop()


def tail_phrase(name):
    return f"the {name} runs" if name.startswith("slowest of ") else f"the {name}"


def measured_as(tails):
    """'the slowest of 30 runs warm and the slowest of 7 runs cold', from (kind, name) pairs."""
    return " and ".join(f"{tail_phrase(name)} {kind}" for kind, name in tails)


def query_tails(document):
    """(kind, tail figure name) for warm and cold over every headline query of the file."""
    return [(kind, one_tail([headline(document, name)[1]["queries"][query][kind]
                             for name in ordered_systems(document) for query in QUERY_NAMES],
                            f"the {kind} queries"))
            for kind in ("warm", "cold")]


def lowest_mark(values, name):
    """"warm, cold" for the measures on which this system had the lowest median and the lowest
    tail figure."""
    marks = []
    for kind in ("warm", "cold"):
        if all(values[name][kind][field] == min(item[kind][field] for item in values.values())
               for field in ("median_us", "p99_us")):
            marks.append(kind)
    return ", ".join(marks) or "no"


def timed_cell(entry, kind, field):
    """A warm or cold number, marked when a page was swapped in while it was timed."""
    flagged = (entry[kind].get("swap_timed") or {}).get("flagged")
    return milliseconds(entry[kind][field]) + (SWAP_MARK if flagged else "")


def is_alone(document):
    return document.get("scope") == "tickstore-only"


def query_table(document):
    names = ordered_systems(document)
    alone = is_alone(document)
    (_, warm), (_, cold) = query_tails(document)
    columns = ("Query", "System", "Variant", "Rows", "Warm median ms", f"Warm {warm} ms",
               "Cold median ms", f"Cold {cold} ms", "Lowest")
    lines = ["| " + " | ".join(columns) + " |", "|" + "---|" * len(columns)]
    for query in QUERY_NAMES:
        values = {name: headline(document, name)[1]["queries"][query] for name in names}
        for name in names:
            variant, _ = headline(document, name)
            entry = values[name]
            cells = (query, SYSTEM_LABEL[name], f"`{variant}`", f"{entry['digest']['rows']:,}",
                     timed_cell(entry, "warm", "median_us"), timed_cell(entry, "warm", "p99_us"),
                     timed_cell(entry, "cold", "median_us"), timed_cell(entry, "cold", "p99_us"),
                     "not ranked: alone" if alone else lowest_mark(values, name))
            lines.append("| " + " | ".join(cells) + " |")
    return lines


class NoFigure(Exception):
    """A value a condition has a target for and the file does not hold, with what is missing."""

    def __init__(self, what, reason):
        super().__init__(f"{what}: {reason}")
        self.what = what
        self.reason = reason


class Unjudged:
    """The verdict of a condition, or a part, with a target and no figure to judge it on."""

    def __init__(self, what):
        self.what = what


def engine_only(write):
    """A write's engine-only seconds, its seconds less its Python seconds (review RF-3): a
    protocol 2 file's own median, or a single repetition's difference. Several repetitions with
    medians alone give none, since the median of the differences is not the difference of the
    medians."""
    if "engine_only_seconds_median" in write:
        return write["engine_only_seconds_median"]
    repetitions = write.get("repetitions")
    if repetitions == 1 and "seconds_median" in write and "python_seconds" in write:
        return write["seconds_median"] - write["python_seconds"]
    raise NoFigure("engine-only seconds", f"a write of {repetitions} repetitions that keeps no "
                   "engine_only_seconds_median")


def engine_only_cell(write):
    try:
        return f"{engine_only(write):.3f}"
    except NoFigure:
        return "not recorded"


def ingest_table(document, class_name):
    lines = ["| " + " | ".join(INGEST_COLUMNS) + " |", "|" + "---|" * len(INGEST_COLUMNS)]
    source_bytes = document["input"]["bytes"]
    for name in ordered_systems(document):
        variant, entry = headline(document, name)
        result = entry["classes"][class_name]
        write = result["write"]
        stored = result["stored"]["allocated_bytes"]
        durability = result["durability"]
        label = SYSTEM_LABEL[name] + (" (parses ITCH)" if name == "tickstore" else "")
        cells = (label, f"`{variant}`", f"{write['seconds_median']:.3f}",
                 engine_only_cell(write),
                 f"{write['records_per_second_median'] / 1e6:.2f}", f"{stored:,}",
                 f"{source_bytes / stored:.3f}", durability["status"],
                 "yes" if durability["like_for_like"] else "no")
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def full_day_of(document):
    return document["input"].get("day")


def gib(value):
    return f"{value / (1 << 30):,.2f} GiB"


def variant_table(document):
    """Every variant each system ran, so a tuning variant's trade-off shows beside its
    headline. Queries are medians on the class B store."""
    lines = ["| " + " | ".join(VARIANT_COLUMNS) + " |", "|" + "---|" * len(VARIANT_COLUMNS)]
    source_bytes = document["input"]["bytes"]
    for name in ordered_systems(document):
        system = document["systems"][name]
        for variant, entry in system["variants"].items():
            classes = entry["classes"]
            stored = classes["B"]["stored"]["allocated_bytes"]
            mark = " (headline)" if variant == system["headline_variant"] else ""
            cells = [SYSTEM_LABEL[name], f"`{variant}`{mark}", f"{stored:,}",
                     f"{source_bytes / stored:.3f}",
                     f"{classes['A']['write']['seconds_median']:.1f} / "
                     f"{classes['B']['write']['seconds_median']:.1f}"]
            cells += [f"{milliseconds(entry['queries'][query]['warm']['median_us'])} / "
                      f"{milliseconds(entry['queries'][query]['cold']['median_us'])}"
                      for query in QUERY_NAMES]
            lines.append("| " + " | ".join(cells) + " |")
    return lines


def mib_of_pages(pages, page_bytes):
    return f"{pages * page_bytes / (1 << 20):,.1f}"


def memory_table(document):
    lines = ["| " + " | ".join(MEMORY_COLUMNS) + " |", "|" + "---|" * len(MEMORY_COLUMNS)]
    page_bytes = document["method"]["swap"]["page_bytes"]
    for name in ordered_systems(document):
        memory = document["systems"][name]["peak_memory"]
        largest = max(memory["vmhwm_bytes_by_process"].items(), key=lambda item: item[1])
        cells = (SYSTEM_LABEL[name], f"{gib(largest[1])} (`{largest[0]}`)",
                 gib(memory["session_rss_bytes_peak"]), gib(memory["mem_available_lowest_bytes"]),
                 f"{mib_of_pages(memory['swap_in_pages_during_run'], page_bytes)} / "
                 f"{mib_of_pages(memory['swap_out_pages_during_run'], page_bytes)}")
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def flagged_queries(system):
    """Every variant's timed query, warm or cold, that had a page swapped in."""
    found = []
    for variant, entry in system["variants"].items():
        for query in QUERY_NAMES:
            for kind in ("warm", "cold"):
                if (entry["queries"][query][kind].get("swap_timed") or {}).get("flagged"):
                    found.append(f"`{variant}` {query} {kind}")
    return ", ".join(found) or "none"


def swap_table(document):
    """Host-wide pages swapped in and out per phase, over every variant and class a system
    ran, from the counts in the results file."""
    lines = ["| " + " | ".join(SWAP_COLUMNS) + " |", "|" + "---|" * len(SWAP_COLUMNS)]
    page_bytes = document["method"]["swap"]["page_bytes"]
    for name in ordered_systems(document):
        system = document["systems"][name]
        cells = [SYSTEM_LABEL[name]]
        for phase, _ in SWAP_PHASES:
            counts = system["swap_by_phase"][phase]
            cells.append(f"{mib_of_pages(counts['pages_in'], page_bytes)} / "
                         f"{mib_of_pages(counts['pages_out'], page_bytes)}")
        cells.append(flagged_queries(system))
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def time_memory_lines(document):
    """A tick-store-only day's peak memory, from BSD time -l around its run."""
    lines = []
    for name in ordered_systems(document):
        memory = document["systems"][name]["peak_memory"]
        lines.append(f"Peak memory of {SYSTEM_LABEL[name]}'s whole run, by BSD time -l: "
                     f"maximum resident set {gib(memory['maximum_resident_set_size_bytes'])}, "
                     "which counts the mapped day and store, and run_system.py's peak memory "
                     f"footprint {gib(memory['peak_memory_footprint_bytes'])}, over "
                     f"{memory['real_seconds']:,.0f} s.")
    return lines


def compare_for(document):
    header = document["header"]
    host = header["host"]
    day = full_day_of(document)
    alone = is_alone(document)
    scope = (f"full day {day}, tick-store alone" if alone else f"full day {day}") if day else \
        "slice only"
    ranking = ("No other system ran, so nothing is ranked." if alone else
               "\"Lowest\" names the measures, warm or cold, on which a system had both the "
               "lowest median and the lowest tail figure of the four.")
    lines = [f"**{host}, {scope}.** {header['cpu_model']}, commit `{header['commit'][:12]}`. "
             f"{header['statement']} {document['label']}", "",
             "Queries, on each system's class B store, in milliseconds. Each latency is the "
             "median of its runs and its tail figure, the 99th percentile by nearest rank, which "
             f"is {measured_as(query_tails(document))}. {ranking}", ""]
    lines += query_table(document)
    for class_name, meaning in (("A", "returns once data is in the OS page cache"),
                                ("B", "returns after a flush to the device")):
        unit = "day" if day else "slice"
        lines += ["", f"Ingest and size, class {class_name}: {meaning}. Stored bytes are "
                      f"allocated at rest, and the ratio is the {unit}'s source bytes over them. "
                      + (f"{MACOS_ALLOCATION} " if host == "macOS" else "")
                      + f"{WRITE_TIMES}", ""]
        lines += ingest_table(document, class_name)
    lines += ["", document["input"]["ingest_input"]]
    if day:
        rule = ("every variant tick-store has, since it ran alone" if alone
                else "by the rule in compare/README.md")
        lines += ["", f"Every variant run on the full day, {rule}. "
                      "Queries are medians on the class B store.", ""]
        lines += variant_table(document)
        if host == "Linux":
            lines += ["", "Peak memory per system, from compare/guard.py across the system's "
                          "whole run.", ""]
            lines += memory_table(document)
        if alone and host != "Linux":
            lines += [""] + time_memory_lines(document)
        guard = ("No guard ran beside tick-store alone. " if alone and host != "Linux" else
                 "Swap-out alone does not stop a run. ")
        lines += ["", "Swap per phase, host-wide, over every variant and class each system ran. "
                      f"{guard}A timed query with any page swapped in is kept and "
                      f"marked{SWAP_MARK}. See \"The guard's swap rule\" in docs/NOTES.md.", ""]
        lines += swap_table(document)
    return lines


def render_compare(compare_dir):
    paths = sorted(compare_dir.glob("*.json"))
    documents = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    if not documents:
        return ["No reported comparison yet."]
    lines = []
    for full_day in (True, False):
        for host in HOSTS:
            for document in documents:
                if document["header"]["host"] == host and bool(full_day_of(document)) == full_day:
                    lines += compare_for(document) + [""]
    return lines[:-1]


class TargetError(Exception):
    """A row of scripts/targets.json that the results files cannot back."""


# The name every tick-store-alone full day carries, <host>-<day>-full-tickstore-<commit>.json.
ALONE_NAME = "-full-tickstore-"


def names_alone(pattern):
    return ALONE_NAME in pattern


def target_file(root, pattern):
    """The one committed file a pattern names, or None if there is none. A pattern that does
    not name a tick-store-alone day never matches one, so Linux-<day>-full-*.json is the
    four-system day even with tick-store's alone day beside it (review RV-1)."""
    found = sorted(path for path in root.glob(pattern)
                   if names_alone(pattern) or ALONE_NAME not in path.name)
    if len(found) > 1:
        raise TargetError(f"{pattern} matches {len(found)} files: {[path.name for path in found]}")
    return found[0] if found else None


def walk(document, path):
    """Keys into a document. "@headline" is the headline_variant of the object above."""
    above, here = None, document
    for key in path:
        if key == "@headline":
            key = (above or {}).get("headline_variant")
        if not isinstance(here, dict) or key not in here:
            raise TargetError(f"no {key!r} on the path {path}")
        above, here = here, here[key]
    return here


def codec_column(document, name):
    found = [column for column in document.get("columns", []) if column.get("name") == name]
    if len(found) != 1:
        raise TargetError(f"no codec-eval column {name!r}")
    return found[0]


# A per-locate column's codec-eval bits leave out the locate sequence that rebuilds file order
# (codec-eval's not_counted and permutation_estimate), so a verdict on them says so (D-10).
PERMUTED_ORDERS = ("per_locate", "per_locate_side")
PERMUTATION_CAVEAT = "leaving out the locate sequence that rebuilds file order"


def excludes_permutation(document, name):
    """Whether a codec-eval column's bits leave out the permutation to file order: the
    comparison's excludes_permutation, which tick_codec_eval writes from version 4, or for an
    earlier file the column's order, from which the tool sets that field."""
    column = codec_column(document, name)
    comparison = column.get("comparison") or {}
    if "excludes_permutation" in comparison:
        return comparison["excludes_permutation"] is True
    version = document.get("tool_version")
    if isinstance(version, int) and version >= 4 and "target" in comparison:
        raise TargetError(f"{name}: a version 4 verdict with no excludes_permutation")
    return column.get("order") in PERMUTED_ORDERS


def codec_columns_of(expression):
    """The codec-eval columns an expression reads, through div and mul, so a ratio of
    per-locate figures carries the permutation caveat as the figures do (review RG-3)."""
    if not isinstance(expression, dict):
        return []
    if "best" in expression:
        return [expression["best"]]
    if "codec" in expression:
        return [expression["column"]]
    return [name for key in ("div", "mul") for operand in expression.get(key, ())
            for name in codec_columns_of(operand)]


def bench_cases(document, pattern):
    found = [result for result in document.get("results", [])
             if fnmatch.fnmatchcase(result.get("case", ""), pattern)]
    if not found:
        raise TargetError(f"no bench case matches {pattern!r}")
    return found


def evaluate(expression, document):
    """A value from a results document, by scripts/targets.json's expression forms. Raises,
    never guesses, on anything missing."""
    if isinstance(expression, (int, float)) and not isinstance(expression, bool):
        return expression
    if not isinstance(expression, dict):
        raise TargetError(f"not an expression: {expression!r}")
    if "path" in expression:
        return walk(document, expression["path"])
    if "case" in expression:
        cases = bench_cases(document, expression["case"])
        if len(cases) != 1 or cases[0]["case"] != expression["case"]:
            raise TargetError(f"no single bench case {expression['case']!r}")
        return walk(cases[0], expression["field"])
    if "cases_median" in expression:
        return statistics.median(walk(case, expression["field"])
                                 for case in bench_cases(document, expression["cases_median"]))
    if "best" in expression:
        # Only a codec that gave every value back counts, as in store_estimate.py (review E-4).
        codecs = [codec for codec in codec_column(document, expression["best"])["codecs"]
                  if codec.get("family") == expression.get("family", codec.get("family"))
                  and codec.get("family") != expression.get("not_family")
                  and codec.get("exact") is True]
        if not codecs:
            raise TargetError(f"no exact codec of that family on {expression['best']!r}")
        return min(codec["bits_per_value"] for codec in codecs)
    if "codec" in expression:
        codecs = [codec for codec in codec_column(document, expression["column"])["codecs"]
                  if codec.get("codec") == expression["codec"]]
        if len(codecs) != 1:
            raise TargetError(f"no codec {expression['codec']!r} on {expression['column']!r}")
        return walk(codecs[0], [expression["field"]])
    if "store_bits" in expression:
        columns = walk(document, ["columns"])
        stored = sum(walk(columns, [name, "payload_bytes"]) + walk(columns, [name, "metadata_bytes"])
                     for name in expression["store_bits"])
        return stored * 8 / walk(columns, [expression["per"], "values"])
    if "min_variant" in expression:
        system, *path = expression["min_variant"]
        variants = walk(document, ["systems", system, "variants"])
        return min(walk(variant, path) for variant in variants.values())
    if "lowest_system" in expression or "lowest_value" in expression:
        spec = expression.get("lowest_system") or expression.get("lowest_value")
        values = {system: walk(document, ["systems", system, "variants", "@headline",
                                          *spec["path"]])
                  for system in spec["systems"]}
        lowest = min(values, key=values.get)
        return lowest if "lowest_system" in expression else values[lowest]
    if "engine_only" in expression:
        return engine_only(walk(document, expression["engine_only"]))
    if "div" in expression:
        numerator, denominator = (evaluate(item, document) for item in expression["div"])
        return numerator / denominator
    if "mul" in expression:
        first, second = (evaluate(item, document) for item in expression["mul"])
        return first * second
    raise TargetError(f"unknown expression {sorted(expression)}")


def shown(value, row):
    """A value as the row displays it: scaled, to its digits, with its unit."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return str(value).lower() if isinstance(value, bool) else str(value)
    if isinstance(value, int) and "scale" not in row and "digits" not in row:
        return f"{value:,}{row.get('unit', '')}"
    digits = row.get("digits", 3)
    return f"{value * row.get('scale', 1):,.{digits}f}{row.get('unit', '')}"


def comparison_of(item, row):
    """(comparator name, its operand) for a part or a condition, or None with no target."""
    for name in COMPARATORS:
        if name in item:
            return name, item[name]
    target = row.get("target") or {}
    for name in COMPARATORS:
        if name in target:
            return name, target[name]
    return None


DISPLAY = ("digits", "scale", "unit")


def judge(item, row, document):
    """(text, met or None) for one condition on one document. A condition may set its own
    digits, scale and unit, and a label shown before its value."""
    row = {**row, **{key: item[key] for key in DISPLAY if key in item}}
    comparison = comparison_of(item, row)
    try:
        value = evaluate(item["value"], document)
        if comparison is not None:
            operand = comparison[1]
            against = operand if not isinstance(operand, dict) else evaluate(operand, document)
    except NoFigure as missing:
        if comparison is None:
            raise TargetError(str(missing)) from missing
        label = f"{item['label']} " if item.get("label") and item is not row else ""
        return f"{label}not judged on {missing.what}: {missing.reason}", Unjudged(missing.what)
    if comparison is None:
        text = shown(value, row)
        labelled = item.get("label") and item is not row
        return (f"{item['label']} {text}" if labelled else text), None
    name, operand = comparison
    if isinstance(against, bool) or not isinstance(against, (int, float)):
        met = COMPARATORS[name][0](value, against)
    elif isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TargetError(f"{value!r} is not a number to compare with {against!r}")
    else:
        met = COMPARATORS[name][0](value, against)
    text = shown(value, row)
    if isinstance(operand, dict):
        text += f" against {shown(against, row)}"
    if any(excludes_permutation(document, name) for name in codec_columns_of(item["value"])):
        text += f", {PERMUTATION_CAVEAT}"
    if item.get("label") and item is not row:
        text = f"{item['label']} {text}"
    return text, met


def lowest_parts(row, document):
    """QL-4's parts: each query and kind, ours lowest of every system on each field."""
    spec = row["lowest_of"]
    parts = []
    for query in spec["queries"]:
        for kind in spec["kinds"]:
            def entry(system, query=query, kind=kind):
                return walk(document, ["systems", system, "variants", "@headline", "queries",
                                       query, kind])
            ours = entry(spec["ours"])
            theirs = {system: entry(system) for system in spec["others"]}
            met = all(ours[field] <= min(other[field] for other in theirs.values())
                      for field in spec["fields"])
            # Per field, the lowest of the others and whose it is, so the figures shown are
            # the ones the verdict compares with (review E-8).
            best = min(theirs, key=lambda system: theirs[system][spec["fields"][0]])
            lowest = [min(theirs, key=lambda system, field=field: (theirs[system][field],
                                                                    system != best))
                      for field in spec["fields"]]
            values = " / ".join(shown(ours[field], row) for field in spec["fields"])
            if all(system == best for system in lowest):
                others = f"{SYSTEM_LABEL[best]}'s " + " / ".join(
                    shown(theirs[best][field], row) for field in spec["fields"])
            else:
                others = " / ".join(f"{SYSTEM_LABEL[system]}'s {shown(theirs[system][field], row)}"
                                    for system, field in zip(lowest, spec["fields"]))
            tail = one_tail([ours, *theirs.values()], f"{query} {kind}")
            names = " / ".join("median" if field == "median_us" else
                               tail if field in TAIL_FIELDS else field
                               for field in spec["fields"])
            note = faster_variants(spec, document, query, kind, ours, row) if met else ""
            parts.append((f"{query} {kind}", f"{values} against {others} ({names}){note}", met))
    return parts


COUNT_WORDS = {2: "two", 3: "three", 4: "four"}


def faster_variants(spec, document, query, kind, ours, row):
    """For a part met under QL-4: that it compares headline variants, and each other system's
    variant that is not its headline and has a lower median on that query, with its figures."""
    count = 1 + len(spec["others"])
    note = f", met among the {COUNT_WORDS.get(count, str(count))} headline variants"
    first = spec["fields"][0]
    faster = []
    for system in spec["others"]:
        headline_name = walk(document, ["systems", system, "headline_variant"])
        for name, variant in walk(document, ["systems", system, "variants"]).items():
            block = walk(variant, ["queries", query, kind])
            if name != headline_name and block[first] < ours[first]:
                figures = " / ".join(shown(block[field], row) for field in spec["fields"])
                faster.append(f"{SYSTEM_LABEL[system]}'s `{name}` is faster, {figures}")
    return note + (", though " + " and ".join(faster) if faster else "")


# The fields that hold a latency block's tail figure: p99_us under its old name.
TAIL_FIELDS = ("p99_us", "tail_us")


def lowest_tails(row, document):
    """For a row judged on tail figures, what they are: "p99 measured as the slowest of 30
    runs warm and ...", so a target that says p99 keeps its words and the verdict says what
    was measured. None for any other row."""
    spec = row.get("lowest_of")
    if not spec or not any(field in TAIL_FIELDS for field in spec["fields"]):
        return None
    systems = [spec["ours"], *spec["others"]]
    tails = [(kind, one_tail([walk(document, ["systems", system, "variants", "@headline",
                                              "queries", query, kind])
                              for system in systems for query in spec["queries"]],
                             f"the {kind} queries"))
             for kind in spec["kinds"]]
    return f"p99 measured as {measured_as(tails)}"


def verdict_text(row, parts):
    """The verdict, and beside it what a tail figure judged against a p99 target is."""
    tails = lowest_tails(row, parts[0][4])
    return verdict_of(parts) + (f"; {tails}" if tails else "")


def target_text_of(row):
    return row.get("target_text") or " ".join(
        f"{COMPARATORS[name][1]} {shown(value, row)}" for name, value in row["target"].items())


def judged_parts(row, root):
    """A row judged on its files: (parts, None), each part (label, text, met, file, document),
    or (None, the reason it has no figure)."""
    if "no_figure" in row:
        return None, f"not judged: {row['no_figure']}"
    specs = [row["lowest_of"]] if "lowest_of" in row else row["parts"]
    files = []
    for spec in specs:
        path = target_file(root, spec["file"])
        if path is None:
            if "if_missing" in row:
                return None, f"not judged: {row['if_missing']}"
            raise TargetError(f"no results file matches {spec['file']}")
        files.append(path)
    documents = {path: json.loads(path.read_text(encoding="utf-8")) for path in files}
    for spec, path in zip(specs, files):
        if names_alone(spec["file"]) and not is_alone(documents[path]):
            raise TargetError(f"{path.name} is not a tick-store-alone day, and {spec['file']} "
                              "names one")
        if not names_alone(spec["file"]) and is_alone(documents[path]):
            raise TargetError(f"{path.name} is a tick-store-alone day, and {spec['file']} names "
                              "the four-system day")
    if "lowest_of" in row:
        document = documents[files[0]]
        return [(*part, files[0], document) for part in lowest_parts(row, document)], None
    parts = []
    for spec, path in zip(row["parts"], files):
        conditions = spec.get("all") or [{key: value for key, value in spec.items()
                                          if key != "label"}]
        judged = [judge(item, row, documents[path]) for item in conditions]
        # A part is judged on its conditions that have a target, and a condition with none is
        # only shown, so it never hides a miss beside it (review E-7). A miss decides the part;
        # then a condition with a target and no figure leaves it not judged.
        verdicts = [met for item, (_, met) in zip(conditions, judged)
                    if comparison_of(item, row) is not None]
        unjudged = [met for met in verdicts if isinstance(met, Unjudged)]
        if not verdicts:
            met = None
        elif False in verdicts:
            met = False
        elif unjudged:
            met = unjudged[0]
        else:
            met = True
        untied = made_before_the_tie(path, root)
        if untied is not None and met is not None:
            met = Unjudged(untied)
        parts.append((spec["label"], ", ".join(text for text, _ in judged), met, path,
                      documents[path]))
    return parts, None


def made_before_the_tie(path, root):
    """Why a part read from G-3's file is not judged at this code, or None: the one file made
    before G-3 was tied to the code, which check_fuzz.py lets through by its pin, judged an
    older commit, with each timed run holding its replay (review RG-6)."""
    pins_path = root / "scripts" / "data_pins.json"
    if not path.name.startswith("fuzz-g3-") or not pins_path.exists():
        return None
    _, exempt = check_fuzz.pinned_before_the_tie(path, json.loads(pins_path.read_text(
        encoding="utf-8")))
    if not exempt:
        return None
    commit = json.loads(path.read_text(encoding="utf-8"))["header"]["commit"]
    return (f"this code: `{path.name}` was made at `{commit[:12]}`, before G-3 was tied to the "
            "code it judged, and each timed run held its corpus replay")


def measured_of(parts):
    if len(parts) == 1:
        return parts[0][1]
    return "; ".join(f"{label}: {text}" for label, text, *_ in parts)


def verdict_word(met):
    if isinstance(met, Unjudged):
        return f"not judged on {met.what}"
    return VERDICT_WORD[met]


def verdict_of(parts):
    verdicts = [part[2] for part in parts]
    if all(met is None for met in verdicts):
        return "not judged: no published figure, reported as measured"
    if None in verdicts:
        raise TargetError("a row with a target on some parts and not on others")
    unjudged = [part for part in parts if isinstance(part[2], Unjudged)]
    if unjudged:
        whats = ", ".join(dict.fromkeys(part[2].what for part in unjudged))
        if len(unjudged) == len(parts):
            return f"not judged on {whats}"
        pieces = [f"{word} on " + ", ".join(f"({part[0]})" for part in parts if part[2] is met)
                  for word, met in (("met", True), ("missed", False))
                  if any(part[2] is met for part in parts)]
        return "; ".join(pieces + [f"not judged on {whats} for " +
                                   ", ".join(f"({part[0]})" for part in unjudged)])
    if all(verdicts):
        return "met"
    if not any(verdicts):
        return "missed"
    return ("met on " + ", ".join(f"({part[0]})" for part in parts if part[2]) +
            "; missed on " + ", ".join(f"({part[0]})" for part in parts if not part[2]))


def files_of(parts, root):
    paths = dict.fromkeys(part[3] for part in parts)
    return ", ".join(f"`{path.relative_to(root)}`" for path in paths)


def target_row(row, root):
    """The cells of one row: (target text, measured, verdict, files)."""
    target_text = target_text_of(row)
    parts, reason = judged_parts(row, root)
    if parts is None:
        return target_text, "no figure", reason, "none"
    return target_text, measured_of(parts), verdict_text(row, parts), files_of(parts, root)


def render_targets(definitions_path, root):
    """The targets block: every row of scripts/targets.json, judged from the results files."""
    definitions = json.loads(definitions_path.read_text(encoding="utf-8"))
    lines = ["Every target in BENCHMARKS.md, on each host it states. Each value is read from the "
             "results file named, and each verdict is computed from that value and the target "
             "by `scripts/render_results.py`, from `scripts/targets.json`. \"any\" marks a "
             "target that does not depend on the host. macOS figures are unpinned, with no "
             "counters; Linux figures are pinned to cpu4.", "",
             "| " + " | ".join(TARGET_COLUMNS) + " |", "|" + "---|" * len(TARGET_COLUMNS)]
    for row in definitions["rows"]:
        try:
            target_text, measured, verdict, names = target_row(row, root)
        except (TargetError, KeyError, TypeError, ZeroDivisionError) as error:
            raise TargetError(f"{row.get('id')} on {row.get('host')}: {error}") from error
        if row.get("judged"):
            target_text += f"; judged on {row['judged']}"
        cells = (row["id"], row["host"], target_text, measured, verdict, names)
        lines.append("| " + " | ".join(cells) + " |")
    return lines


VERDICT_WORD = {True: "met", False: "missed", None: "not judged"}


def has_headline(definitions_path):
    if not definitions_path.exists():
        return False
    return "headline" in json.loads(definitions_path.read_text(encoding="utf-8"))


# A figure as prose writes it: digits, with thousands commas or a decimal point between them.
NUMBER = re.compile(r"\d+(?:[.,]\d+)*")
FORMATTER = string.Formatter()
# The verdict classes the line under the headline's table names, each with the word it opens.
LEFT_OUT_HEADS = {"missed": "Missed", "missed in part": "Missed in part",
                  "not judged": "Not judged", "not judged in part": "Not judged in part"}


def headline_hosts(headline):
    """Each host's introduction on the first screen, by what it is, and the short name the
    screen calls it after, with the words for both hosts at once."""
    hosts = headline.get("hosts") or {}
    for host in HOSTS:
        entry = hosts.get(host)
        if not isinstance(entry, dict) or not entry.get("introduced") or not entry.get("then"):
            raise TargetError(f"headline: no introduction and short name for the host {host}")
    if not hosts.get("both"):
        raise TargetError("headline: no words for both hosts at once")
    return hosts


def template_fields(text, what):
    """(the literal text, the field names) of a template; a field is one plain name."""
    literal, names = [], []
    for before, field, spec, conversion in FORMATTER.parse(text):
        literal.append(before)
        if field is None:
            continue
        if not field.isidentifier() or spec or conversion:
            raise TargetError(f"{what}: {{{field}}} is not a plain named value")
        names.append(field)
    return " ".join(literal), names


def no_typed_figure(text, hosts, what):
    """A text of the headline types no figure: each comes from a results file or a target, as
    a named value. A host's introduction, which says what its processor is, is the exception."""
    for host in HOSTS:
        text = text.replace(hosts[host]["introduced"], " ")
    numeric_characters(text, what)
    typed = NUMBER.findall(text)
    if typed:
        raise TargetError(f"{what} types the figure {typed[0]!r} by hand; give it as a named "
                          "value read from the results")


def lowest_systems(expression, row, documents, name):
    """A headline value that is the lowest of other systems takes every other system of the
    comparison, and, where its row's target ranks among some, exactly those: "vs QuestDB" under
    a bar of the four is the best of the other three, never of a few (review DX-5)."""
    if not isinstance(expression, dict):
        return
    for key in ("lowest_system", "lowest_value"):
        if key not in expression:
            continue
        systems = set(expression[key]["systems"])
        ranked = row.get("lowest_of")
        ours = ranked["ours"] if ranked else "tickstore"
        for document in documents:
            others = set(walk(document, ["systems"])) - {ours}
            if systems != others:
                raise TargetError(f"{{{name}}} takes the lowest of {sorted(systems)}, and the "
                                  f"comparison ran {sorted(others)} beside {ours}")
        if ranked and systems != set(ranked["others"]):
            raise TargetError(f"{{{name}}} takes the lowest of {sorted(systems)}, and its "
                              f"target ranks among {sorted(ranked['others'])}")


def headline_value(spec, row, parts, name):
    """One named value of a headline text, as the text shows it: an expression on the results
    file of each part shown, or the row's own target value."""
    if "target" in spec:
        value = (row.get("target") or {}).get(spec["target"])
        if value is None:
            raise TargetError(f"{{{name}}}: the row has no {spec['target']!r} target")
    else:
        documents = list({part[3]: part[4] for part in parts}.values())
        lowest_systems(spec["value"], row, documents, name)
        values = [evaluate(spec["value"], document) for document in documents]
        over = spec.get("over")
        if over is None:
            if len(values) != 1:
                raise TargetError(f"{{{name}}} reads {len(values)} files; say with over how to "
                                  "take them together")
            value = values[0]
        elif over == "sum":
            value = sum(values)
        elif over == "all":
            value = all(item is True for item in values)
        else:
            raise TargetError(f"{{{name}}}: no way {over!r} to take values together")
    if "words" in spec:
        if not isinstance(value, bool):
            raise TargetError(f"{{{name}}}: words for {value!r}, which is not true or false")
        return spec["words"]["true" if value else "false"]
    if spec.get("label") == "system":
        if value not in SYSTEM_LABEL:
            raise TargetError(f"{{{name}}}: {value!r} is no system")
        return SYSTEM_LABEL[value]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TargetError(f"{{{name}}}: {value!r} is not a number to format")
    return spec["format"].format(value * spec.get("scale", 1))


def filled(texts, spec, row, parts, hosts, what, given=None):
    """Each of texts with its {names} filled from spec's values and from given, such as the
    verdict. A name with no value, a value no text names and a figure typed by hand are each
    refused, so every figure shown is one read from the results under its own name."""
    given = given or {}
    values = spec.get("values", {})
    named = []
    for text in texts:
        literal, names = template_fields(text, what)
        no_typed_figure(literal, hosts, f"{what} {text!r}")
        named += names
    unknown = [name for name in named if name not in values and name not in given]
    if unknown:
        raise TargetError(f"{what} names {{{unknown[0]}}}, and gives no value for it")
    unused = [name for name in [*values, *given] if name not in named]
    if unused:
        raise TargetError(f"{what} takes the value {unused[0]!r} and names it nowhere, so a "
                          "figure could be read and never shown")
    shown_values = {name: headline_value(value, row, parts, name)
                    for name, value in values.items()}
    shown_values.update(given)
    return [text.format(**shown_values) for text in texts]


def shown_parts(entry, row, root):
    """A headline entry's target row judged: (every part, the parts the entry shows)."""
    parts, reason = judged_parts(row, root)
    if parts is None:
        raise TargetError(f"a headline row needs a figure, and this one is {reason}")
    wanted = entry.get("parts")
    if not wanted:
        return parts, parts
    names = [part[0] for part in parts]
    unknown = [label for label in wanted if label not in names]
    if unknown:
        raise TargetError(f"no part {unknown} in {names}")
    return parts, [part for part in parts if part[0] in wanted]


def verdict_class(parts):
    """Met, missed, not judged, or missed or not judged in part, for parts judged, or None for
    a row with no figure."""
    if parts is None:
        return "not judged"
    verdict_of(parts)
    verdicts = [part[2] for part in parts]
    if all(met is True for met in verdicts):
        return "met"
    if all(met is False for met in verdicts):
        return "missed"
    if all(met is None or isinstance(met, Unjudged) for met in verdicts):
        return "not judged"
    return "missed in part" if any(met is False for met in verdicts) else "not judged in part"


def plain_names(definitions, hosts):
    """Each target row's name in plain words, and its parts' where given, by (id, host)."""
    names, owners = {}, {}
    for entry in definitions["headline"].get("names", []):
        key = (entry["id"], entry["host"])
        if key in names:
            raise TargetError(f"two plain names for {key[0]} on {key[1]}")
        for label, text in [(None, entry["name"]), *entry.get("parts", {}).items()]:
            no_typed_figure(text, hosts, f"the plain name {text!r}")
            # The line under the table is read back name by name, under its machine, so a name
            # is whole, and on one machine it is one target's, and one part's within it: a name
            # inside a list, or shared by two parts, could stand in for another (review DX-13).
            if ", " in text or "; " in text or text.startswith("on ") or not text.strip():
                raise TargetError(f"the plain name {text!r} is empty or holds a list's or a "
                                  "machine's words, so the line under the table could not be "
                                  "read back")
            for host, ident, other in owners.get(text, []):
                same_machine = "any" in (host, entry["host"]) or host == entry["host"]
                if same_machine and (ident != entry["id"] or
                                     None not in (label, other) and label != other):
                    raise TargetError(f"the plain name {text!r} names both {ident} "
                                      f"({other or 'the target'}) and {entry['id']} "
                                      f"({label or 'the target'}) on one machine")
            owners.setdefault(text, []).append((entry["host"], entry["id"], label))
        names[key] = entry
    for row in definitions["rows"]:
        if (row["id"], row["host"]) not in names:
            raise TargetError(f"no plain name for {row['id']} on {row['host']}, so the line "
                              "under the table could not name it")
    return names


def left_out_items(definitions, root, shown, names):
    """What the line under the table holds: per verdict class, (host, plain name) of each part
    of a target with a row on the screen that the screen leaves out and that is not met; the
    count of those met; and, for every other row of the targets, the count of each verdict
    class, with the plain name of each target the headline asks to name among its count."""
    items = {kind: [] for kind in LEFT_OUT_HEADS}
    met_parts = 0
    counts = {kind: 0 for kind in COUNT_ORDER}
    named = {kind: [] for kind in COUNT_ORDER}
    asked = {(entry["id"], entry["host"]) for entry in
             definitions["headline"].get("name_among_counts", [])}
    for row in definitions["rows"]:
        key = (row["id"], row["host"])
        parts, _ = judged_parts(row, root)
        if key not in shown:
            kind = verdict_class(parts)
            counts[kind] += 1
            if key in asked and kind != "met":
                named[kind].append(names[key]["name"])
            continue
        for part in parts:
            if part[0] in shown[key]:
                continue
            name = names[key].get("parts", {}).get(part[0])
            if not name:
                raise TargetError(f"no plain name for the part {part[0]!r} of {key[0]} on "
                                  f"{key[1]}, which the screen leaves out")
            kind = verdict_class([part])
            if kind == "met":
                met_parts += 1
            else:
                items[kind].append((row["host"], name))
    return items, met_parts, counts, named


def host_groups(found, hosts):
    """"a, b; on both machines, c; on the Ryzen, d", from (host, name) pairs in their order."""
    by_host = {}
    for host, name in found:
        if host not in ("any", *HOSTS):
            raise TargetError(f"no words for the host {host!r}")
        names = by_host.setdefault(host, [])
        if name not in names:
            names.append(name)
    both = [name for name in by_host.get(HOSTS[0], []) if name in by_host.get(HOSTS[1], [])]
    groups = [", ".join(by_host["any"])] if by_host.get("any") else []
    if both:
        groups.append(f"on {hosts['both']}, " + ", ".join(both))
    for host in HOSTS:
        alone = [name for name in by_host.get(host, []) if name not in both]
        if alone:
            groups.append(f"on {hosts[host]['then']}, " + ", ".join(alone))
    return "; ".join(groups)


# The verdict classes of the count of the other rows, in the order the line gives them.
COUNT_ORDER = ("met", "missed", "missed in part", "not judged", "not judged in part")
# What the count of the other rows counts: rows of the Targets block, a target on a host each.
COUNTED = "target-and-machine rows of the [full table]"
COUNT_CLAUSE = re.compile(r"Of the other (\d+) " + re.escape(COUNTED) + r"\(" +
                          re.escape(TARGETS_ANCHOR) + r"\): (.*)\.$")


def left_out_line(items, met_parts, counts, named, hosts):
    sentences = [f"{LEFT_OUT_HEADS[kind]}: {host_groups(found, hosts)}."
                 for kind, found in items.items() if found]
    if met_parts:
        sentences.append(f"Met: {met_parts} more of their {'part' if met_parts == 1 else 'parts'}.")
    pieces = [f"{counts[kind]} {kind}" + (f", {' and '.join(named[kind])} among them"
                                         if named[kind] else "")
              for kind in COUNT_ORDER if counts[kind]]
    return " ".join(["Not shown above.", *sentences,
                     f"Of the other {sum(counts.values())} {COUNTED}({TARGETS_ANCHOR}): "
                     + ", ".join(pieces) + "."])


def class_of(mets):
    """The verdict class of a row or a part, from its parts' raw verdicts: worked out here apart
    from verdict_class, which the writer of the line uses, so a fault in one shows against the
    other (review DX-4). True is met and False missed; anything else is not judged."""
    if not mets:
        return "not judged"
    met = sum(1 for value in mets if value is True)
    missed = sum(1 for value in mets if value is False)
    if met == len(mets):
        return "met"
    if missed == len(mets):
        return "missed"
    if met + missed == 0:
        return "not judged"
    return "missed in part" if missed else "not judged in part"


def stated_items(sentence, hosts, kind):
    """The (where, name) pairs one class sentence of the line states: where is "any", "both"
    or a host, from the group's opening words, and each name is whole, split at ", "."""
    pairs = []
    heads = [("both", f"on {hosts['both']}, "),
             *((host, f"on {hosts[host]['then']}, ") for host in HOSTS)]
    for group in sentence.split("; "):
        where = "any"
        for key, words in heads:
            if group.startswith(words):
                where, group = key, group[len(words):]
                break
        for name in group.split(", "):
            if not name or (where, name) in pairs:
                raise TargetError(f"the line under the table names {name!r} twice or empty "
                                  f"under {kind}")
            pairs.append((where, name))
    return set(pairs)


def where_words(where, hosts):
    return {"any": "with no machine", "both": f"on {hosts['both']}"}.get(
        where, f"on {hosts[where]['then'] if where in hosts else where}")


def check_left_out(line, definitions, root, shown, names, hosts):
    """The line under the table, walked here apart from the code that writes it: its own
    verdict classes from the parts' raw verdicts, not the writer's; it names each left-out part
    of a target with a row on the screen that is missed or not judged, whole, under its verdict
    and its machine, and nothing else there; its count of each verdict over the other rows is
    the targets' own, with each target asked for named among its own class's count and no
    other; and what the screen shows, names and counts adds up to every row of the targets and
    every part of the targets it shows, so nothing drops out unseen. A headline never hides a
    miss."""
    sentences = {}
    clause = COUNT_CLAUSE.search(line)
    if clause is None:
        raise TargetError("the line under the table gives no count of the other rows")
    body = line[:clause.start()].strip()
    if not body.startswith("Not shown above."):
        raise TargetError("the line under the table does not open with what it is")
    for sentence in [piece for piece in body[len("Not shown above."):].strip().split(". ")
                     if piece]:
        head = sentence.split(":", 1)[0]
        if head not in [*LEFT_OUT_HEADS.values(), "Met"] or head in sentences:
            raise TargetError(f"the line under the table says {sentence!r}, which is no class "
                              "of its own or a second of one")
        sentences[head] = sentence.rstrip(".")
    others, stated, stated_named, last = int(clause.group(1)), {}, {}, None
    for piece in clause.group(2).split(", "):
        found = re.fullmatch(r"(\d+) (.+)", piece)
        if found and found.group(2) in COUNT_ORDER and found.group(2) not in stated:
            last = found.group(2)
            stated[last] = int(found.group(1))
            continue
        among = re.fullmatch(r"(.+) among them", piece)
        if among and last is not None and last not in stated_named:
            stated_named[last] = among.group(1)
            continue
        raise TargetError(f"the line under the table counts {piece!r}, which is no count of a "
                          "verdict and no name among one")
    met = re.fullmatch(r"Met: (\d+) more of their parts?", sentences.get("Met", "Met: 0 more of "
                                                                          "their parts"))
    asked = [(entry["id"], entry["host"]) for entry in
             definitions["headline"].get("name_among_counts", [])]
    expected, expected_named, named_parts, shown_parts_count, total_parts, met_parts = \
        {}, {}, 0, 0, 0, 0
    found_items = {kind: {} for kind in LEFT_OUT_HEADS}
    for row in definitions["rows"]:
        key = (row["id"], row["host"])
        parts, _ = judged_parts(row, root)
        if key not in shown:
            kind = class_of([part[2] for part in parts]) if parts is not None else "not judged"
            expected[kind] = expected.get(kind, 0) + 1
            if key in asked and kind != "met":
                expected_named.setdefault(kind, []).append(names[key]["name"])
            continue
        total_parts += len(parts)
        for part in parts:
            if part[0] in shown[key]:
                shown_parts_count += 1
                continue
            kind = class_of([part[2]])
            if kind == "met":
                met_parts += 1
                continue
            name = names[key].get("parts", {}).get(part[0])
            found_items[kind].setdefault(name, {})[row["host"]] = key
            named_parts += 1
    for kind, head in LEFT_OUT_HEADS.items():
        wanted = {}
        for name, by_host in found_items[kind].items():
            machines = set(by_host)
            where = "any" if "any" in machines else "both" if machines >= set(HOSTS) else \
                next(iter(machines))
            wanted[(where, name)] = next(iter(by_host.values()))
        sentence = sentences.get(head)
        given = stated_items(sentence[len(head) + 2:], hosts, kind) if sentence else set()
        for (where, name), key in wanted.items():
            if (where, name) in given:
                continue
            elsewhere = [other for other, item in given if item == name]
            if elsewhere:
                raise TargetError(f"the line under the table names {name!r} "
                                  f"{where_words(elsewhere[0], hosts)}, and it is {kind} "
                                  f"{where_words(where, hosts)} ({key[0]} on {key[1]})")
            raise TargetError(f"the line under the table does not name {name!r}, which is "
                              f"{kind} ({key[0]} on {key[1]}), and a headline never hides a miss")
        extra = sorted(given - set(wanted))
        if extra:
            raise TargetError(f"the line under the table names {extra[0][1]!r} "
                              f"{where_words(extra[0][0], hosts)} as {kind}, and the targets give "
                              "no such part")
    if stated != expected:
        raise TargetError(f"the line under the table counts the other rows as {stated}, and the "
                          f"targets give {expected}")
    expected_named = {kind: " and ".join(items) for kind, items in expected_named.items()}
    if stated_named != expected_named:
        raise TargetError(f"the line under the table names {stated_named} among its counts, and "
                          f"the targets give {expected_named}")
    stated_met = int(met.group(1)) if met else -1
    if stated_met != met_parts:
        raise TargetError(f"the line under the table counts {stated_met} more parts met, and "
                          f"the targets give {met_parts}")
    if len(shown) + others != len(definitions["rows"]) or sum(stated.values()) != others:
        raise TargetError(f"the screen shows {len(shown)} rows and counts {others} others, "
                          f"{sum(stated.values())} by verdict, of the targets' "
                          f"{len(definitions['rows'])}")
    if shown_parts_count + named_parts + stated_met != total_parts:
        raise TargetError(f"the screen shows {shown_parts_count} parts of its targets, names "
                          f"{named_parts} and counts {stated_met} met, of their {total_parts}")


def under_sentence(under, row, parts, hosts):
    """The sentence under the table, with its row's figure and verdict."""
    return filled([under["text"]], under, row, parts, hosts, "the sentence under the table",
                  {"verdict": verdict_of(parts)})[0]


def check_under(sentence, under, row, parts, hosts):
    """The sentence under the table, checked apart from the code that writes it: a row met or
    missed in whole says that word, and any other says neither (review DX-4)."""
    kind = class_of([part[2] for part in parts])
    if kind in ("met", "missed"):
        expected = filled([under["text"]], under, row, parts, hosts,
                          "the sentence under the table", {"verdict": kind})[0]
        if sentence != expected:
            raise TargetError(f"the sentence under the table reads {sentence!r}, and its row is "
                              f"{kind}: {expected!r}")
        return
    for word in ("met", "missed"):
        if sentence == filled([under["text"]], under, row, parts, hosts,
                              "the sentence under the table", {"verdict": word})[0]:
            raise TargetError(f"the sentence under the table says {word}, and its row is {kind}")


def headline_cells(entry, rows, root, hosts):
    """One table row of the headline: its cells and its parts shown."""
    row = rows.get((entry["id"], entry["host"]))
    if row is None:
        raise TargetError("no such row in the targets")
    for key in ("what", "bar", "source", "measured"):
        if not entry.get(key):
            raise TargetError(f"no {key}: each row says what it is, then its bar and where the "
                              "bar comes from, before what was measured")
    for key in ("what", "source"):
        no_typed_figure(entry[key], hosts, f"its {key} {entry[key]!r}")
    _, parts = shown_parts(entry, row, root)
    bar, measured = filled([entry["bar"], entry["measured"]], entry, row, parts, hosts,
                           "its bar and measured")
    return (entry["what"], f"{bar} ({entry['source']})", measured, verdict_of(parts)), parts


def render_headline(definitions_path, root):
    """The README's headline: the rows "headline" names, judged as in the targets block on the
    parts each shows, in plain words with a bar and its source before what was measured; the
    sentence under the table; and the line that names, in plain words, everything left out that
    is missed or not judged."""
    definitions = json.loads(definitions_path.read_text(encoding="utf-8"))
    headline = definitions["headline"]
    hosts = headline_hosts(headline)
    rows = {(row["id"], row["host"]): row for row in definitions["rows"]}
    lines = ["| " + " | ".join(HEADLINE_COLUMNS) + " |", "|" + "---|" * len(HEADLINE_COLUMNS)]
    # Per target row, the labels of the parts the screen shows.
    shown = {}
    missed = False
    for entry in headline["rows"]:
        try:
            cells, parts = headline_cells(entry, rows, root, hosts)
        except (TargetError, KeyError, TypeError, ValueError, ZeroDivisionError) as error:
            raise TargetError(f"headline {entry.get('id')} on {entry.get('host')}: {error}") \
                from error
        shown.setdefault((entry["id"], entry["host"]), set()).update(part[0] for part in parts)
        missed = missed or any(part[2] is False for part in parts)
        lines.append("| " + " | ".join(cells) + " |")
    if not missed:
        raise TargetError("headline: no row is a missed target, and the headline must show one")
    try:
        under = headline.get("under")
        if under:
            row = rows.get((under["id"], under["host"]))
            if row is None:
                raise TargetError(f"no row {under['id']} on {under['host']} for the sentence "
                                  "under the table")
            _, parts = shown_parts(under, row, root)
            shown.setdefault((under["id"], under["host"]), set()).update(p[0] for p in parts)
            sentence = under_sentence(under, row, parts, hosts)
            check_under(sentence, under, row, parts, hosts)
            lines += ["", sentence]
        names = plain_names(definitions, hosts)
        items, met_parts, counts, named = left_out_items(definitions, root, shown, names)
        line = left_out_line(items, met_parts, counts, named, hosts)
        check_left_out(line, definitions, root, shown, names, hosts)
    except (TargetError, KeyError, TypeError, ValueError, ZeroDivisionError) as error:
        raise TargetError(f"headline: {error}") from error
    return lines + ["", line]


def sentences_of(prose):
    return re.split(r"(?<=[.!?])\s+(?=[A-Z])", prose)


# A heading as GitHub's Markdown takes it: at most three spaces, one to six #, then a space or
# the line's end. "#It beats ..." is a paragraph, so it is prose like any other (review DX-3).
HEADING = re.compile(r"^ {0,3}#{1,6}(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")


def screen_units(before):
    """The pitch as a reader sees it, in the order it shows: each heading's text, its #s left
    out, and each sentence of the prose around them. A heading ends the prose before it."""
    units, prose = [], []

    def flush():
        text = " ".join(" ".join(prose).split())
        units.extend(sentence for sentence in sentences_of(text) if sentence)
        prose.clear()
    for line in before.splitlines():
        found = HEADING.match(line)
        if found:
            flush()
            if (found.group(1) or "").strip():
                units.append(" ".join(found.group(1).split()))
        else:
            prose.append(line)
    flush()
    return units


# Words that rank tick-store or state an outcome. Each one on the first screen must sit in a
# sentence that is a claim, which ties it to the results, or inside a figure's own words. The
# list is the second line of defence: a sentence of the pitch must be a claim, hold a figure or
# be listed as stating no result (review DX-2), and the words a reviewer showed passing false
# sentences are on it too (review DX-10).
RANKING = re.compile(r"\b(?:faster|fastest|slower|slowest|quicker|quickest|least|most|smaller|"
                     r"smallest|larger|largest|lower|lowest|higher|highest|below|above|beats?|"
                     r"beating|beaten|outperform(?:s|ing|ed)?|wins|won|loses|lost|best|worst|"
                     r"exact|met|meets|missed|misses|exceeds|less|fewer|more|better|worse|ahead|"
                     r"behind|trails?|trailing|leads?|leading|keeps? up|kept up|keeping up|clean|"
                     r"pass(?:es|ed)?|fails?|failed|surpass(?:es|ed)?|twice|half|halves|double|"
                     r"triple|third|quarter|matches)\b", re.IGNORECASE)


def invisible_characters(text, what):
    """A character a reader cannot see on the first screen, such as a zero-width space, which
    would join words for the count and hide what they say, is refused (review DX-11)."""
    for character in text:
        if unicodedata.category(character) in ("Cf", "Zl", "Zp"):
            raise TargetError(f"{what} holds the character U+{ord(character):04X}, which a reader "
                              "does not see and the checks would read past")


def numeric_characters(text, what):
    """A figure written with a character other than 0 to 9, such as a vulgar fraction, is one
    the digit pattern misses, so it is refused as typed (review DX-11)."""
    for character in text:
        if not "0" <= character <= "9" and unicodedata.numeric(character, None) is not None:
            raise TargetError(f"{what} types the figure {character!r} as a character, which no "
                              "check reads; give it in digits where a check reads it")


def check_screen(definitions_path, readme, headline_lines):
    """The README's first screen, the pitch and the headline as rendered, names no target's id
    and no query's name, so each says in words what it is; and introduces each host once, by
    what it is, before any short name for it, and never calls it by its operating system after.
    It ends at the headline's end marker: text between that and the next heading would still be
    on the first screen with none of these checks, so it is refused (review DX-11), and so is a
    character a reader cannot see anywhere on it."""
    definitions = json.loads(definitions_path.read_text(encoding="utf-8"))
    hosts = headline_hosts(definitions["headline"])
    after = readme.split(HEADLINE_END, 1)[1] if HEADLINE_END in readme else ""
    for line in after.splitlines():
        if HEADING.match(line):
            break
        if line.strip():
            raise TargetError(f"the README's first screen goes on past the headline with "
                              f"{line.strip()[:60]!r}, which no check reads; start a section "
                              "with a heading first")
    screen = " ".join((readme.split(HEADLINE_BEGIN)[0] + "\n" +
                       "\n".join(headline_lines)).split())
    invisible_characters(screen, "the README's first screen")
    for name in [*dict.fromkeys(row["id"] for row in definitions["rows"]), *QUERY_NAMES]:
        if re.search(rf"(?<![\w-]){re.escape(name)}(?![\w-])", screen):
            raise TargetError(f"the README's first screen names {name!r}; say in words what "
                              "it is")
    rest = screen
    for host in HOSTS:
        introduced, then = hosts[host]["introduced"], hosts[host]["then"]
        count = screen.count(introduced)
        if count != 1:
            raise TargetError(f"the README's first screen introduces the host {host} as "
                              f"{introduced!r} {count} times, and should once")
        if 0 <= screen.find(then) < screen.index(introduced):
            raise TargetError(f"the README's first screen calls a host {then!r} before it "
                              f"introduces it as {introduced!r}")
        rest = rest.replace(introduced, " ")
    for host in HOSTS:
        if re.search(rf"\b{re.escape(host)}\b", rest):
            raise TargetError(f"the README's first screen calls a host {host!r} past its "
                              f"introduction; call it {hosts[host]['then']!r}")


def conditions_of(claim):
    """The conditions a claim's sentence rests on: its own, or each of "all"."""
    if "all" in claim:
        if not claim["all"] or not all(isinstance(item, dict) for item in claim["all"]):
            raise TargetError(f"pitch: the claim {claim['text']!r} has an empty or malformed all")
        return claim["all"]
    return [claim]


def condition_host(condition, definitions, root):
    """The machine a condition is judged on: its target row's host, or its results file's."""
    if "target" in condition:
        return condition["target"]["host"]
    path = target_file(root, condition["file"])
    if path is None:
        raise TargetError(f"pitch: no results file matches {condition['file']}")
    document = json.loads(path.read_text(encoding="utf-8"))
    header = document.get("header") if isinstance(document.get("header"), dict) else {}
    return header.get("host") or document.get("host") or "any"


def target_words(condition, definitions):
    """The words a target condition's sentence must hold: its target row lists, under "pitch",
    the words that name its bar with the parts they cover, so a claim cannot be pointed at
    another target of the same verdict (review DX-2)."""
    spec = condition["target"]
    row = next((row for row in definitions["rows"]
                if (row["id"], row["host"]) == (spec["id"], spec["host"])), None)
    if row is None or not row.get("pitch"):
        raise TargetError(f"pitch: {spec['id']} on {spec['host']} lists no words for a claim's "
                          "sentence to hold, so the claim could name another target's bar")
    wanted = sorted(spec.get("parts") or [])
    return [item["words"] for item in row["pitch"] if sorted(item.get("parts") or []) == wanted]


def pitch_words(definitions):
    """Every target row's pitch words, each held by one row alone."""
    owners = {}
    for row in definitions["rows"]:
        for item in row.get("pitch", []):
            if not isinstance(item, dict) or not item.get("words"):
                raise TargetError(f"{row['id']} on {row['host']} has pitch words with no text")
            owner = owners.setdefault(item["words"], (row["id"], row["host"]))
            if owner != (row["id"], row["host"]):
                raise TargetError(f"the pitch words {item['words']!r} name both {owner[0]} on "
                                  f"{owner[1]} and {row['id']} on {row['host']}")


def check_pitch(definitions_path, root, readme):
    """The README's pitch, the prose above its headline with its headings, states figures by
    hand. Each one scripts/targets.json lists under "pitch" must appear in it as the results
    give it now, and every other number in it must be a text listed with the reason it is no
    result, such as a language version, each found once, so no figure there goes stale unseen
    (review E-9). Each figure's format carries the words that say what it is, and no two
    figures share one (review RG-4). Each heading and sentence is one of three things: a
    claim, the whole sentence listed under "claims", which ranks tick-store or states an
    outcome and must be borne out by its conditions (reviews RG-13, DX-2); a sentence holding a
    figure; or a text listed under "no_result_sentences" with the reason it states no result.
    A claim on a target holds the words that target's row lists for its bar, and a claim
    judged on one machine names that machine and no other. A word that ranks or judges must sit in a claim
    or a figure's words, headings included (review DX-3). The pitch, headings included, runs to
    at most "max_words" words, and holds no character a reader cannot see."""
    definitions = json.loads(definitions_path.read_text(encoding="utf-8"))
    pitch = definitions.get("pitch")
    if not pitch:
        return
    before = readme.split(HEADLINE_BEGIN)[0]
    invisible_characters(before, "the README's pitch")
    numeric_characters(before, "the README's pitch")
    text = " ".join(before.split())
    units = screen_units(before)
    limit = pitch.get("max_words")
    if isinstance(limit, bool) or not isinstance(limit, int) or limit < 1:
        raise TargetError("pitch: no max_words, the most words the prose before the table may "
                          "run to")
    words = sum(len(unit.split()) for unit in units)
    if words > limit:
        raise TargetError(f"the README's pitch runs to {words} words before the table, over its "
                          f"{limit}: the first screen keeps its prose short")
    remaining = text
    # A figure is found by its format's words as well as its digits. Two figures of one format
    # could trade places in the text and both still be found, so each format is one figure's,
    # and carries the words that say what its figure is (review RG-4).
    formats = [figure["format"] for figure in pitch["figures"]]
    shared = sorted({form for form in formats if formats.count(form) > 1})
    if shared:
        raise TargetError(f"pitch: two figures share the format {shared[0]!r}, so they could "
                          "trade places in the text unseen; give each the words around it")
    stated_figures = []
    for figure in pitch["figures"]:
        path = target_file(root, figure["file"])
        if path is None:
            raise TargetError(f"pitch: no results file matches {figure['file']}")
        document = json.loads(path.read_text(encoding="utf-8"))
        values = [evaluate(value, document) * figure.get("scale", 1)
                  for value in figure["values"]]
        stated = figure["format"].format(*values)
        if stated not in text:
            raise TargetError(f"the README's pitch does not state {stated!r}, which "
                              f"{path.relative_to(root)} gives")
        remaining = remaining.replace(stated, " ")
        stated_figures.append(stated)
    # A text kept out of the figure check is kept out once: the year of a processor is no
    # licence for the same digits elsewhere (review DX-11).
    for item in pitch.get("not_results", []):
        if not item.get("why"):
            raise TargetError(f"pitch: {item.get('text')!r} is kept out of the check with no "
                              "reason")
        found = remaining.count(item["text"])
        if found != 1:
            raise TargetError(f"pitch: {item['text']!r}, kept out of the check as "
                              f"{item['why']}, is in the pitch {found} times, and may be once")
        remaining = remaining.replace(item["text"], " ")
    left = NUMBER.findall(remaining)
    if left:
        raise TargetError(f"a figure in the README's pitch that nothing checks: {left[0]!r}")
    claims = {}
    pitch_words(definitions)
    hosts = definitions["headline"].get("hosts") if "headline" in definitions else None
    for claim in pitch.get("claims", []):
        if claim["text"] not in units:
            raise TargetError(f"the README's pitch no longer says {claim['text']!r}, which "
                              "scripts/targets.json ties to the results as a whole sentence; "
                              "change the claim there with the sentence")
        if claim["text"] in claims:
            raise TargetError(f"pitch: two claims for {claim['text']!r}")
        judged_on = {condition_host(condition, definitions, root)
                     for condition in conditions_of(claim)}
        for host in HOSTS:
            if host in judged_on or "any" in judged_on or not hosts or host not in hosts:
                continue
            for words in (hosts[host]["introduced"], hosts[host]["then"], hosts.get("both")):
                if words and words in claim["text"]:
                    raise TargetError(f"the README's pitch says {claim['text']!r}, which names "
                                      f"{words!r}, and is judged on {sorted(judged_on)} alone")
        for condition in conditions_of(claim):
            problem = claim_problem(condition, definitions, root)
            if problem:
                raise TargetError(f"the README's pitch says {claim['text']!r}, and the results "
                                  f"do not bear it out: {problem}; change the sentence")
            if "target" in condition and not any(
                    words in claim["text"] for words in target_words(condition, definitions)):
                spec = condition["target"]
                raise TargetError(f"the README's pitch ties {claim['text']!r} to {spec['id']} on "
                                  f"{spec['host']}, and the sentence holds none of the words "
                                  "that target lists for its bar on those parts")
            host = condition_host(condition, definitions, root)
            if host in HOSTS:
                if not hosts or host not in hosts:
                    raise TargetError(f"pitch: no words for the host {host}, so the claim "
                                      f"{claim['text']!r} cannot name its machine")
                if hosts[host]["introduced"] not in claim["text"] and \
                        hosts[host]["then"] not in claim["text"]:
                    raise TargetError(f"the README's pitch says {claim['text']!r}, judged on "
                                      f"{host} alone, and does not name the machine; say "
                                      f"{hosts[host]['then']!r} in it")
        claims[claim["text"]] = claim
    no_result = {}
    for item in pitch.get("no_result_sentences", []):
        if not item.get("why"):
            raise TargetError(f"pitch: {item.get('text')!r} is listed as stating no result with "
                              "no reason")
        no_result[item["text"]] = item
    for unit in units:
        if unit in claims:
            continue
        masked = unit
        for words in stated_figures:
            masked = masked.replace(words, " ")
        found = RANKING.search(masked)
        if found:
            raise TargetError(f"the README's pitch ranks or judges with {found.group(0)!r} in "
                              f"the sentence {unit!r}, and no claim in scripts/targets.json "
                              "ties those words to the results")
        if unit not in no_result and not any(words in unit for words in stated_figures):
            raise TargetError(f"the README's pitch says {unit!r}, which is no claim tied to the "
                              "results, holds no figure checked against them, and is not listed "
                              "as stating no result")
    for text in no_result:
        if text not in units:
            raise TargetError(f"pitch: {text!r} is listed as stating no result, and the README's "
                              "pitch does not say it")


def claim_problem(claim, definitions, root):
    """Why the results do not bear out one condition of a sentence of the pitch that ranks
    tick-store, or None (review RG-13). "target" is a row's verdict, judged as in the targets
    block, on the parts named; "below" pairs of figures of one file, each first below its
    second; "fastest" the system with the lowest of each field among the systems' headline
    variants, for each query and kind, one of those named. A fastest claim is judged on every
    field its words could be read on, median and tail alike (review DX-14)."""
    if "target" in claim:
        spec = claim["target"]
        row = next((row for row in definitions["rows"]
                    if (row["id"], row["host"]) == (spec["id"], spec["host"])), None)
        if row is None:
            raise TargetError(f"pitch: no target row {spec['id']} on {spec['host']}")
        parts, reason = judged_parts(row, root)
        if parts is None:
            return f"{spec['id']} on {spec['host']} is {reason}"
        if spec.get("parts"):
            unknown = [label for label in spec["parts"] if label not in [p[0] for p in parts]]
            if unknown:
                raise TargetError(f"pitch: no part {unknown} in {spec['id']} on {spec['host']}")
            parts = [part for part in parts if part[0] in spec["parts"]]
        verdict = verdict_of(parts)
        return None if verdict == spec["verdict"] else \
            f"{spec['id']} on {spec['host']} is {verdict}, not {spec['verdict']}"
    path = target_file(root, claim["file"])
    if path is None:
        raise TargetError(f"pitch: no results file matches {claim['file']}")
    document = json.loads(path.read_text(encoding="utf-8"))
    if "below" in claim:
        for lower, higher in claim["below"]:
            first, second = evaluate(lower, document), evaluate(higher, document)
            if not first < second:
                return f"{first} is not below {second}"
        return None
    if "fastest" in claim:
        spec = claim["fastest"]
        if not spec.get("fields"):
            raise TargetError("pitch: a fastest claim names no fields to judge fastest on")
        for query in spec["queries"]:
            for kind in spec["kinds"]:
                for field in spec["fields"]:
                    values = {system: walk(document, ["systems", system, "variants", "@headline",
                                                      "queries", query, kind, field])
                              for system in spec["systems"]}
                    fastest = min(values, key=values.get)
                    if fastest not in spec["one_of"]:
                        return f"{query} {kind}: {fastest} has the lowest {field}"
        return None
    raise TargetError(f"pitch: the claim {claim.get('text', 'condition')!r} names no condition")


def block_of(text, begin=BEGIN, end=END):
    """(before, block, after) around the markers, or None if they are not both there."""
    first = text.find(begin)
    last = text.find(end)
    if first < 0 or last < first:
        return None
    start = first + len(begin)
    return text[:start], text[start:last], text[last:]


def validate(paths, pins, validator=CHECK_RESULTS, extra=(), echo=False):
    """True if the validator passed the files; its exit code instead with echo, which also
    prints what it said of each file that passed."""
    if not paths:
        return 0 if echo else True
    command = [sys.executable, "-I", str(validator), *(("--pins", str(pins)) if pins else ()),
               *extra, *[str(path) for path in paths]]
    checked = subprocess.run(command, check=False, capture_output=True, text=True)
    print(checked.stderr, end="", file=sys.stderr)
    if echo:
        print(checked.stdout, end="")
        return checked.returncode
    return checked.returncode == 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate RESULTS.md's results tables and the README's headline.")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if a block of either file is stale")
    parser.add_argument("--readme", type=Path, default=DEFAULT_README,
                        help="the file with the headline block")
    parser.add_argument("--results-md", type=Path, default=DEFAULT_RESULTS_MD,
                        help="the file with the targets, benchmark and comparison blocks")
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument("--pins", type=Path, default=DEFAULT_PINS)
    parser.add_argument("--compare-results", type=Path, default=DEFAULT_COMPARE)
    parser.add_argument("--compare-pins", type=Path,
                        help="pins for the comparison files, if not --pins")
    parser.add_argument("--capacity", type=Path, default=DEFAULT_CAPACITY)
    parser.add_argument("--targets", type=Path, default=DEFAULT_TARGETS)
    parser.add_argument("--targets-root", type=Path, default=REPO_ROOT,
                        help="the folder the targets' results paths are relative to")
    args = parser.parse_args(argv)

    if not validate(sorted(args.results.glob("*.json")), args.pins):
        print("render_results: a results file failed validation", file=sys.stderr)
        return EXIT_USAGE
    compare_paths = sorted(args.compare_results.glob("*.json"))
    if not validate(compare_paths, args.compare_pins or args.pins, CHECK_COMPARE,
                    ("--capacity", str(args.capacity))):
        print("render_results: a comparison results file failed validation", file=sys.stderr)
        return EXIT_USAGE
    # G-3's file, which the targets block reads, against the fuzz targets of the same tree.
    fuzz_paths = sorted((args.targets_root / "results").glob("fuzz-g3-*.json"))
    fuzz_code = validate(fuzz_paths, None, CHECK_FUZZ,
                         ("--cmake", str(args.targets_root / "fuzz" / "CMakeLists.txt"),
                          "--repo", str(args.targets_root),
                          "--pins", str(args.targets_root / "scripts" / "data_pins.json")),
                         echo=True)
    if fuzz_code not in (0, EXIT_UNJUDGED):
        print("render_results: a fuzzing results file failed validation", file=sys.stderr)
        return EXIT_USAGE
    done = EXIT_UNJUDGED if fuzz_code == EXIT_UNJUDGED else 0
    roundtrip_paths = sorted((args.targets_root / "results").glob("roundtrip-*.json"))
    if not validate(roundtrip_paths, None, CHECK_ROUNDTRIP):
        print("render_results: a round-trip results file failed validation", file=sys.stderr)
        return EXIT_USAGE
    if not args.results_md.exists():
        print(f"render_results: there is no {args.results_md.name}", file=sys.stderr)
        return EXIT_USAGE
    text = args.results_md.read_text(encoding="utf-8")
    md_name = args.results_md.name
    if block_of(text) is None:
        print(f"render_results: the markers are not in {md_name}", file=sys.stderr)
        return EXIT_USAGE
    blocks = [(BEGIN, END, render(args.results))]
    if block_of(text, COMPARE_BEGIN, COMPARE_END) is not None:
        try:
            blocks.append((COMPARE_BEGIN, COMPARE_END, render_compare(args.compare_results)))
        except TargetError as error:
            print(f"render_results: comparison: {error}", file=sys.stderr)
            return EXIT_USAGE
    elif compare_paths:
        print(f"render_results: the comparison markers are not in {md_name}", file=sys.stderr)
        return EXIT_USAGE
    has_targets = block_of(text, TARGETS_BEGIN, TARGETS_END) is not None
    if has_targets != args.targets.exists():
        print(f"render_results: {md_name} has the targets markers and there is no "
              f"{args.targets.name}, or the reverse: both or neither", file=sys.stderr)
        return EXIT_USAGE

    readme = args.readme.read_text(encoding="utf-8") if args.readme.exists() else None
    for label, begin in RESULTS_MD_BLOCKS:
        if readme is not None and begin in readme:
            print(f"render_results: {args.readme.name} holds the {label} block's markers, and "
                  f"that block belongs in {md_name} alone", file=sys.stderr)
            return EXIT_USAGE
    try:
        wants_headline = has_headline(args.targets)
    except (OSError, json.JSONDecodeError) as error:
        print(f"render_results: targets: {error}", file=sys.stderr)
        return EXIT_USAGE
    has_markers = readme is not None and block_of(readme, HEADLINE_BEGIN, HEADLINE_END) is not None
    if has_markers != wants_headline:
        print(f"render_results: {args.readme.name} has the headline markers and "
              f"{args.targets.name} no headline, or the reverse: both or neither",
              file=sys.stderr)
        return EXIT_USAGE
    headline = []
    try:
        if has_targets:
            blocks.append((TARGETS_BEGIN, TARGETS_END, render_targets(args.targets,
                                                                      args.targets_root)))
        if wants_headline:
            headline.append((HEADLINE_BEGIN, HEADLINE_END,
                             render_headline(args.targets, args.targets_root)))
        if has_targets and readme is not None:
            check_pitch(args.targets, args.targets_root, readme)
        if headline:
            check_screen(args.targets, readme, headline[0][2])
    except (OSError, json.JSONDecodeError, TargetError) as error:
        print(f"render_results: targets: {error}", file=sys.stderr)
        return EXIT_USAGE

    files = [(args.results_md, text, blocks)]
    if headline:
        files.append((args.readme, readme, headline))
    stale = []
    rendered = []
    for path, content, wanted_blocks in files:
        fresh = content
        for begin, end, lines in wanted_blocks:
            before, _, after = block_of(fresh, begin, end)
            fresh = before + "\n" + "\n".join(lines) + "\n" + after
        if fresh != content:
            stale.append(path.name)
        rendered.append((path, content, fresh))
    if args.check:
        for name in stale:
            print(f"render_results: the table in {name} is stale or was edited by hand. "
                  "Regenerate it.", file=sys.stderr)
        if stale:
            return EXIT_STALE
        print(" and ".join(path.name for path, _, _ in rendered) + ": tables are current")
        return untied(done)
    for path, content, fresh in rendered:
        if fresh != content:
            path.write_text(fresh, encoding="utf-8")
    return untied(done)


def untied(code):
    if code == EXIT_UNJUDGED:
        print("render_results: G-3's file could not be tied to the code it judged, for want of "
              "git history here", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
