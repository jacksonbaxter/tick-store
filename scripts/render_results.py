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
the pitch, headings included, runs to at most "max_words" words. A claim of the pitch resting on
a rival's figure timed while pages were swapped in says so in the pitch's short form of that
mark, which nothing but the pitch may use. The whole first screen names no
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
import hashlib
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
# The mark of a figure from a timed phase in which the disturbance file lists a disturbance.
DISTURBED_MARK = " (disturbed)"

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


def timed_cell(entry, kind, field, disturbed=False):
    """A warm or cold number, marked when a page was swapped in while it was timed, and when
    the disturbance file lists a disturbance in its phase."""
    flagged = (entry[kind].get("swap_timed") or {}).get("flagged")
    return milliseconds(entry[kind][field]) + (SWAP_MARK if flagged else "") + \
        (DISTURBED_MARK if disturbed else "")


def phase_disturbed(entries, system, variant, phase, which):
    """Whether entries, a document's own, list a timed disturbance in a phase: a write of a
    class, or the warm or cold queries, whole or of the query named."""
    for entry in entries:
        if not entry["timed"] or (entry.get("system"), entry.get("variant")) != (system, variant):
            continue
        if phase == "write" and entry["phase"] == "write" and entry["class"] == which:
            return True
        if phase != "write" and entry["phase"] == f"{phase} queries" and \
                entry["query"] in (None, which):
            return True
    return False


def is_alone(document):
    return document.get("scope") == "tickstore-only"


def query_table(document, entries=()):
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
            warm, cold = (phase_disturbed(entries, name, variant, kind, query)
                          for kind in ("warm", "cold"))
            cells = (query, SYSTEM_LABEL[name], f"`{variant}`", f"{entry['digest']['rows']:,}",
                     timed_cell(entry, "warm", "median_us", warm),
                     timed_cell(entry, "warm", "p99_us", warm),
                     timed_cell(entry, "cold", "median_us", cold),
                     timed_cell(entry, "cold", "p99_us", cold),
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


def ingest_table(document, class_name, entries=()):
    lines = ["| " + " | ".join(INGEST_COLUMNS) + " |", "|" + "---|" * len(INGEST_COLUMNS)]
    source_bytes = document["input"]["bytes"]
    for name in ordered_systems(document):
        variant, entry = headline(document, name)
        result = entry["classes"][class_name]
        write = result["write"]
        stored = result["stored"]["allocated_bytes"]
        durability = result["durability"]
        label = SYSTEM_LABEL[name] + (" (parses ITCH)" if name == "tickstore" else "")
        mark = DISTURBED_MARK if phase_disturbed(entries, name, variant, "write", class_name) \
            else ""
        cells = (label, f"`{variant}`", f"{write['seconds_median']:.3f}{mark}",
                 engine_only_cell(write) + mark,
                 f"{write['records_per_second_median'] / 1e6:.2f}{mark}", f"{stored:,}",
                 f"{source_bytes / stored:.3f}", durability["status"],
                 "yes" if durability["like_for_like"] else "no")
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def full_day_of(document):
    return document["input"].get("day")


def gib(value):
    return f"{value / (1 << 30):,.2f} GiB"


def variant_table(document, entries=()):
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
            def held(phase, which, name=name, variant=variant):
                return DISTURBED_MARK if phase_disturbed(entries, name, variant, phase, which) \
                    else ""
            cells = [SYSTEM_LABEL[name], f"`{variant}`{mark}", f"{stored:,}",
                     f"{source_bytes / stored:.3f}",
                     f"{classes['A']['write']['seconds_median']:.1f}{held('write', 'A')} / "
                     f"{classes['B']['write']['seconds_median']:.1f}{held('write', 'B')}"]
            cells += [f"{milliseconds(entry['queries'][query]['warm']['median_us'])}"
                      f"{held('warm', query)} / "
                      f"{milliseconds(entry['queries'][query]['cold']['median_us'])}"
                      f"{held('cold', query)}"
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


def compare_for(document, entries=()):
    """One comparison file's block. entries are the disturbances the tree lists in its timed
    phases, each figure from one marked."""
    header = document["header"]
    host = header["host"]
    day = full_day_of(document)
    alone = is_alone(document)
    scope = (f"full day {day}, tick-store alone" if alone else f"full day {day}") if day else \
        "slice only"
    ranking = ("No other system ran, so nothing is ranked." if alone else
               "\"Lowest\" names the measures, warm or cold, on which a system had both the "
               "lowest median and the lowest tail figure of the four.")
    note = method_input_note(document)
    lines = [f"**{host}, {scope}.** {header['cpu_model']}, commit `{header['commit'][:12]}`. "
             f"{header['statement']} {document['label']}" + (f" {note}" if note else ""),
             "",
             "Queries, on each system's class B store, in milliseconds. Each latency is the "
             "median of its runs and its tail figure, the 99th percentile by nearest rank, which "
             f"is {measured_as(query_tails(document))}. {ranking}", ""]
    lines += query_table(document, entries)
    for class_name, meaning in (("A", "returns once data is in the OS page cache"),
                                ("B", "returns after a flush to the device")):
        unit = "day" if day else "slice"
        clause = input_clause(document, class_name)
        lines += ["", f"Ingest and size, class {class_name}: {meaning}. Stored bytes are "
                      f"allocated at rest, and the ratio is the {unit}'s source bytes over them. "
                      + (f"{MACOS_ALLOCATION} " if host == "macOS" else "")
                      + f"{WRITE_TIMES}"
                      + (f" The rates below are not like for like: {clause}." if clause else ""),
                  ""]
        lines += ingest_table(document, class_name, entries)
    lines += ["", document["input"]["ingest_input"]]
    if day:
        rule = ("every variant tick-store has, since it ran alone" if alone
                else "by the rule in compare/README.md")
        lines += ["", f"Every variant run on the full day, {rule}. "
                      "Queries are medians on the class B store.", ""]
        lines += variant_table(document, entries)
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
    if any(DISTURBED_MARK in line for line in lines):
        lines[0] += (f" A figure from a timed phase in which the disturbance file lists a "
                     f"disturbance is marked{DISTURBED_MARK}; a verdict in Targets that rests on "
                     "one says what it was beside its word.")
    return lines


def render_compare(compare_dir, root=None):
    """Every comparison file's block; with root, each figure from a timed phase in which the
    tree's disturbance files list a disturbance is marked."""
    paths = sorted(compare_dir.glob("*.json"))
    documents = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    if not documents:
        return ["No reported comparison yet."]
    entries = disturbance_entries(root) if root is not None else []
    lines = []
    for full_day in (True, False):
        for host in HOSTS:
            for path, document in zip(paths, documents):
                if document["header"]["host"] == host and bool(full_day_of(document)) == full_day:
                    own = [entry for entry in entries if entry["document"] ==
                           f"compare/{path.name}" and entry["timed"]]
                    lines += compare_for(document, own) + [""]
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


def lowest_parts(row, document, mark=None):
    """QL-4's parts: each query and kind, ours lowest of every system on each field. mark,
    given a phase read, gives what stands beside a figure shown from it, as a disturbance."""
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
            note = faster_variants(spec, document, query, kind, ours, row, mark) if met else ""
            parts.append((f"{query} {kind}", f"{values} against {others} ({names}){note}", met))
    return parts


COUNT_WORDS = {2: "two", 3: "three", 4: "four"}


def faster_variants(spec, document, query, kind, ours, row, mark=None):
    """For a part met under QL-4: that it compares headline variants, and each other system's
    variant that is not its headline and has a lower median on that query, with its figures
    and, beside them, what mark gives for the phase they were timed in (review C1, 4d)."""
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
                beside = mark(("queries", system, name, kind, query)) if mark else ""
                faster.append(f"{SYSTEM_LABEL[system]}'s `{name}` is faster, {figures}{beside}")
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
    """The verdict, and beside it what a tail figure judged against a p99 target is and, for a
    write against other systems, whose input each write read from where."""
    tails = lowest_tails(row, parts[0][4])
    inputs = write_inputs(row, parts)
    return verdict_of(parts, row) + (f"; {tails}" if tails else "") + \
        (f"; {inputs}" if inputs else "")


def and_list(items):
    return items[0] if len(items) == 1 else ", ".join(items[:-1]) + " and " + items[-1]


def counted_with(value, singular, plural=None):
    return f"{value:,} {singular if value == 1 else plural or singular + 's'}"


def input_of(write, label):
    """Where one write's input was when its clock started, from its own record: (whether it was
    wholly resident, the words), or None for a record that keeps no residency figure."""
    detail = write.get("detail") or {}
    start = detail.get("input_at_clock_start")
    if not isinstance(start, dict) and isinstance(detail.get("input_resident"), dict):
        start = {**detail["input_resident"], "made_ready": detail["input_resident"]}
    if not isinstance(start, dict):
        return None
    files = ", ".join(f"`{name}`" for name in start.get("files", []))
    files = f", {files}," if files else ""
    ready = start.get("made_ready") or {}
    resident, pages = start["resident_pages"], start["pages"]
    how = (f", made ready by {ready['what']}" if "what" in ready else
           f", made resident by {counted_with(ready['warms'], 'warm')}" if "warms" in ready else
           "")
    where = "in memory before the clock starts" if resident == pages else \
        "read from disk inside the clock"
    device = detail.get("device_read") or {}
    read = (f", and {device['bytes']:,} B read from the device inside the clock, "
            f"{device['counter']}" if "bytes" in device else ", with no device-read figure")
    return resident == pages, (f"{label} input{files} is {where}: {resident:,} of its "
                               f"{pages:,} pages resident at the clock's start{how}{read}")


def write_inputs(row, parts):
    """For a write against other systems, from each write's own record: whose input is in
    memory before the clock starts and whose is read from disk inside it, the bytes read from
    the device inside the clock, what engine-only holds, and, where the inputs differ, that
    engine-only does not take the reads out, with each other system's engine-only on the
    replaced run where there is one."""
    classes = [notes_of(part).get("write") for part in parts if notes_of(part).get("write")]
    if not classes:
        return ""
    class_name, document = classes[0], parts[0][4]
    systems = [*dict.fromkeys(["tickstore", *ordered_systems(document)])]
    writes = {system: walk(document, ["systems", system, "variants", "@headline", "classes",
                                      class_name, "write"]) for system in systems}
    inputs = {system: input_of(writes[system], f"{SYSTEM_LABEL[system]}'s")
              for system in systems}
    pieces = [found[1] for found in inputs.values() if found]
    unrecorded = [SYSTEM_LABEL[system] for system, found in inputs.items() if not found]
    if unrecorded:
        pieces.append("the records of " + and_list(unrecorded) +
                      f" keep{'s' if len(unrecorded) == 1 else ''} no residency or device-read "
                      "figure")
    held = "; ".join(f"for {SYSTEM_LABEL[system]}, {writes[system]['times']['engine_only_seconds']}"
                     for system in systems if "engine_only_seconds" in
                     (writes[system].get("times") or {}))
    text = (f"inputs of the class {class_name} writes, from each write's record: "
            + "; ".join(pieces) + ". Engine-only, by each write's record: "
            + (held or "the total less python_seconds"))
    if len({found[0] for found in inputs.values() if found}) < 2:
        return text
    text += (". Engine-only does not remove the difference in where the inputs are: the device "
             "bytes are counted over the whole clock, so engine-only may hold them as well")
    replaced = next((notes_of(part)["replaced"] for part in parts
                     if notes_of(part).get("replaced")), None)
    if replaced is not None:
        # The moves between the runs stand in a sentence of their own, with their cause said to
        # be unknown, never joined to the inputs' difference, which would read as their cause:
        # a cause is stated only from a published run that measured it (review C2-1).
        moved = []
        for system in systems:
            if system == "tickstore":
                continue
            old_write = walk(replaced[1], ["systems", system, "variants", "@headline",
                                           "classes", class_name, "write"])
            pieces = []
            for what, figure in (("total", lambda write: write["seconds_median"]),
                                 ("engine-only", engine_only)):
                try:
                    before, after = figure(old_write), figure(writes[system])
                except (NoFigure, KeyError):
                    continue
                pieces.append(f"{what} from {shown(before, row)} to {shown(after, row)}")
            if pieces:
                moved.append(f"{SYSTEM_LABEL[system]}'s " + pieces[0].replace(" from ", " went "
                             "from ", 1) + "".join(f" and its {piece}" for piece in pieces[1:]))
        if moved:
            text += (". Between the replaced run and this one, " + "; ".join(moved) + "; the "
                     f"cause of {'this move' if len(moved) == 1 else 'these moves'} "
                     f"{CAUSE_UNKNOWN}. This run's records give where each input was and the "
                     "bytes read above. " + replaced_keeps(replaced[1], class_name)[:-1])
    return text


# What a page says of a move between the replaced run and this one: its cause was not measured,
# so it is not known, and nothing may say it is (review C2-1).
CAUSE_UNKNOWN = "was not measured and is not known"
# A sentence that states a move between the runs: one that says a figure went, moved, rose or
# fell, or gives a figure on the replaced run beside one on this one.
MOVE = re.compile(r"\b(?:went|moved|rose|fell|jumped|grew|dropped)\b|"
                  r"\bon the replaced run\b.*\bon this (?:one|run)\b", re.IGNORECASE)
# Words that would give a move a cause, or join it to the inputs' difference, which reads as
# one.
CAUSAL = re.compile(r"\b(?:because|since|so|thus|hence|therefore|due to|owing to|as a result|"
                    r"caused?|causing|explains?|explained|reflects?|shows? how far|evidence|"
                    r"asymmetry|where the inputs are|cache dependence)\b", re.IGNORECASE)


# The words that say a move's cause is not known, left out when a sentence is read for a cause.
UNKNOWN_CAUSE = re.compile(r"the cause of (?:this|that|these) moves? " + re.escape(CAUSE_UNKNOWN))


def unit_sentences(unit):
    """The sentences of a unit of a page, a table row's cells apart."""
    cells = [cell.strip() for cell in unit.strip().strip("|").split(" | ")] \
        if unit.startswith("|") else [unit]
    return [sentence for cell in cells for sentence in sentences_of(cell)]


def move_problem(pages):
    """Where a sentence states a move between the replaced run and this one without saying
    that its cause was not measured and is not known, or joins it to a cause, or None. pages
    is (name, text)."""
    for name, text in pages:
        for number, unit in prose_units(text):
            for sentence in unit_sentences(unit):
                if not MOVE.search(sentence) or "replaced run" not in sentence:
                    continue
                said = " ".join(sentence.split())
                if CAUSE_UNKNOWN not in said:
                    return (f"{name}:{number}: {said[:90]!r} states a move between the replaced "
                            f"run and this one and does not say its cause {CAUSE_UNKNOWN}")
                found = CAUSAL.search(UNKNOWN_CAUSE.sub("", said))
                if found:
                    return (f"{name}:{number}: {said[:90]!r} joins a move between the runs to a "
                            f"cause with {found.group(0)!r}, and its cause {CAUSE_UNKNOWN}")
    return None


def evictions_of(block):
    """Every record of the inputs evicted before a phase, anywhere in a block."""
    found = []
    if isinstance(block, dict):
        for key, value in block.items():
            if key == "inputs_evicted_before" and isinstance(value, dict):
                found.append(value)
            else:
                found += evictions_of(value)
    return found


# How the sentence on what a replaced run's record keeps opens, wherever it stands.
KEEPS = "The replaced run's record keeps"


def replaced_keeps(document, class_name):
    """What a replaced run's record keeps of a class's headline writes and their read-backs
    that could show where each input was, word for word from its fields: whose write keeps an
    input residency figure, whose a device-read figure, and what each read-back keeps of the
    inputs evicted before it (review C2-2)."""
    systems = [*dict.fromkeys(["tickstore", *ordered_systems(document)])]
    resident, device, evicted = [], [], {}
    for system in systems:
        try:
            result = walk(document, ["systems", system, "variants", "@headline", "classes",
                                     class_name])
        except TargetError:
            continue
        detail = (result.get("write") or {}).get("detail") or {}
        for key in ("input_at_clock_start", "input_resident"):
            if isinstance(detail.get(key), dict):
                resident.append((system, key, detail[key]))
                break
        if isinstance(detail.get("device_read"), dict):
            device.append(system)
        for found in evictions_of(result.get("read_back")):
            evicted.setdefault((found.get("what"), found.get("pages"),
                                found.get("resident_after")), []).append(system)
    others = [system for system in systems if system not in [found[0] for found in resident]]
    if not resident:
        writes = "no input residency figure for any write"
    else:
        named = and_list([f"{SYSTEM_LABEL[system]}'s write (`{key}`, "
                          f"{figure['resident_pages']:,} of {figure['pages']:,} pages resident)"
                          for system, key, figure in resident])
        writes = f"an input residency figure for {named}" + (
            f" and none for {possessive(others)}" if others else "")
    reads = "no device-read figure for any write" if not device else \
        f"a device-read figure for {possessive(device)} alone" if len(device) < len(systems) \
        else "a device-read figure for every write"
    if not evicted:
        backs = "nothing on the inputs"
    else:
        backs = "; ".join(
            f"{'every system' if len(owners) == len(systems) else possessive(owners)}'s"
            f"{'' if len(owners) == len(systems) else ' read-backs'}: that {what} were evicted "
            f"before it, with {after:,} of their {pages:,} pages resident after "
            f"(`inputs_evicted_before`)"
            for (what, pages, after), owners in evicted.items())
    return (f"{KEEPS}, of the class {class_name} writes, {writes}, and {reads}; and of their "
            f"read-backs, {backs}.")


def keeps_problem(pages, root):
    """Where a sentence saying what a replaced run's record keeps is not replaced_keeps' own
    sentence for one of its classes, word for word, or None: a hand-written page may not say
    more, or less, than the record holds (review C2-2). pages is (name, text)."""
    wanted = set()
    for path in sorted((Path(root) / "results" / "compare" / REPLACED).glob("*.json")):
        document = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(document.get("systems"), dict):
            wanted |= {replaced_keeps(document, name) for name in ("A", "B")}
    wanted = {sentence[:-1] for sentence in wanted}
    for name, text in pages:
        for number, unit in prose_units(text):
            for sentence in unit_sentences(unit):
                said = " ".join(sentence.split()).rstrip(".")
                if KEEPS in said and said not in wanted:
                    return (f"{name}:{number}: {said[:90]!r} says what the replaced run's record "
                            "keeps, and is not the sentence its fields give: "
                            f"{sorted(wanted)[0] if wanted else 'no replaced record'!r}")
    return None


def input_reads(document, class_name):
    """Per system with a record of it, from its headline write of a class: (whether its input
    was wholly resident when the clock started, the bytes read from the device inside the
    clock or None)."""
    found = {}
    if not isinstance(document.get("systems"), dict):
        return found
    for system in ordered_systems(document):
        try:
            write = walk(document, ["systems", system, "variants", "@headline", "classes",
                                    class_name, "write"])
        except TargetError:
            continue
        detail = (write.get("detail") if isinstance(write, dict) else None) or {}
        start = detail.get("input_at_clock_start")
        if not isinstance(start, dict) or "pages" not in start:
            continue
        found[system] = (start["resident_pages"] == start["pages"],
                         (detail.get("device_read") or {}).get("bytes"))
    return found


def byte_range(values):
    low, high = min(values), max(values)
    return f"{low:,} B" if low == high else f"{low:,} to {high:,} B"


def input_sides(reads):
    """(the systems whose input was resident, those whose was read from disk, the bytes read
    inside each side's clocks), or None where the inputs are alike or tick-store's is unknown."""
    if "tickstore" not in reads or len({resident for resident, _ in reads.values()}) < 2:
        return None
    resident = [system for system, (whole, _) in reads.items() if whole]
    disk = [system for system, (whole, _) in reads.items() if not whole]
    return resident, disk


def possessive(systems):
    return and_list([f"{SYSTEM_LABEL[system]}'s" for system in systems])


# What engine-only holds and why the two clocks leave the difference in the inputs, as every
# input note says it.
ENGINE_ONLY_READS = ("engine-only, the clock less reading the input and putting it in the form "
                     "the system's call takes, may still hold those reads, which are counted over "
                     "the whole clock, so the two-clock rule does not take the difference out")


def input_clause(document, class_name):
    """For a comparison whose headline writes of a class did not start alike, by their records,
    the one clause every page that quotes a write verdict against another system carries beside
    it, and RESULTS.md beside the ingest table: whose input was in memory before the clock
    started and whose was read from disk inside it, the bytes each side's records show read
    from the device inside the clock, and what engine-only holds (review C1, conditions 2 and
    3). None where they started alike."""
    reads = input_reads(document, class_name)
    sides = input_sides(reads)
    if sides is None:
        return None
    resident, disk = sides
    def side(systems):
        found = [reads[system][1] for system in systems if reads[system][1] is not None]
        return f", with {byte_range(found)} read from the device inside the clock" if found \
            else ", with no device-read figure"
    return (f"inputs not alike, by each write's record: {possessive(resident)} "
            f"{'input is' if len(resident) == 1 else 'inputs are'} in memory before the clock "
            f"starts{side(resident)}; {possessive(disk)} "
            f"{'is' if len(disk) == 1 else 'are'} read from disk inside it{side(disk)}; "
            f"{ENGINE_ONLY_READS}")


def method_input_note(document):
    """The sentence the comparison's method, wherever it is described, gives for a comparison
    whose headline writes did not start alike: whose input is in memory before the clock and
    whose is read from disk inside it, the bytes read from the device inside each side's
    clocks over both classes, by the records, and what engine-only holds. None where alike."""
    sides, by_class = {}, {}
    for class_name in ("A", "B"):
        reads = input_reads(document, class_name)
        found = input_sides(reads)
        if found is None:
            continue
        by_class[class_name] = reads
        for side, systems in zip(("resident", "disk"), found):
            sides.setdefault(side, [])
            sides[side] += [system for system in systems if system not in sides[side]]
    if not by_class:
        return None
    def read_bytes(side):
        found = [reads[system][1] for reads in by_class.values() for system in sides[side]
                 if system in reads and reads[system][1] is not None]
        return byte_range(found) if found else "no device-read figure"
    return (f"Write inputs are not alike on this day, by each write's record: "
            f"{possessive(sides['resident'])} "
            f"{'input is' if len(sides['resident']) == 1 else 'inputs are'} in memory before "
            f"the clock starts, and {possessive(sides['disk'])} "
            f"{'is' if len(sides['disk']) == 1 else 'are'} read from disk inside it, with "
            f"{read_bytes('disk')} read from the device inside those headline write clocks of "
            f"class {' and '.join(by_class)} against {read_bytes('resident')} inside "
            f"{possessive(sides['resident'])}; {ENGINE_ONLY_READS}.")


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
        mark = figure_marker(root, files[0], document, row["lowest_of"]["file"], row)
        parts = [(*part, files[0], document) for part in lowest_parts(row, document, mark)]
        return finished(row, root, [row["lowest_of"]] * len(parts), parts), None
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
    return finished(row, root, row["parts"], parts), None


# A comparison file a later run replaced is kept, bytes unchanged, in this folder beside the
# comparison files, so a verdict the new figures change shows the replaced run's figures and
# outcome beside its own, and counts take the less favourable of the two.
REPLACED = "replaced"
# The pins key that holds each replaced file's SHA-256: it is kept as it was validated when it
# was the reported file, and check_compare.py cannot judge it again away from its slice.
REPLACED_PINS = "replaced_comparison_files"


def standing(met):
    """How far an outcome favours tick-store: missed, then not judged, then met."""
    return 0 if met is False else 2 if met is True else 1


def notes_of(part):
    return part[5] if len(part) > 5 else {}


def counted(part):
    """The outcome counts and claims take for a part: the less favourable of its outcome on
    this run and on the replaced run, worked out from both apart from the part's own verdict,
    so a writer that counts the favourable one shows against it."""
    change = notes_of(part).get("change")
    if not change:
        return part[2]
    return min((change["new"], change["old"]), key=standing)


def is_comparison(row):
    """A row judged against other systems: QL-4's ranking, or a part whose target is a figure
    read from a comparison file, such as another system's write seconds."""
    if "lowest_of" in row:
        return True
    for spec in row.get("parts", []):
        if not spec.get("file", "").startswith("results/compare/"):
            continue
        for item in spec.get("all") or [spec]:
            comparison = comparison_of(item, row)
            if comparison is not None and isinstance(comparison[1], dict):
                return True
    return False


def replaced_pattern(pattern):
    found = Path(pattern)
    return (found.parent / REPLACED / found.name).as_posix()


def figure_marks(entries, root, path, document, read, replaced, row):
    """What stands beside a figure shown from one timed phase, on any page: each disturbance
    recorded in that phase, with the replaced run's figure there, and the record's flag of a
    page swapped in while it was timed."""
    marks = [f"measured in {disturbance_text(entry, found, replaced, row)}"
             for entry, found in disturbed(entries, root, path, [read])]
    if swapped_reads([read], document):
        marks.append("which the record flags as timed while pages were swapped in")
    return marks


def figure_marker(root, path, document, pattern, row):
    """mark for lowest_parts: given a phase read, ", " and each of figure_marks, or nothing."""
    entries = disturbance_entries(root)
    replaced = replaced_document(root, pattern)

    def mark(read):
        return "".join(f", {text}" for text in figure_marks(entries, root, path, document, read,
                                                            replaced, row))
    return mark


def replaced_document(root, pattern):
    """(path, document) of the replaced run's file for a results pattern, its bytes held to
    their pin, or None where no file was replaced."""
    path = target_file(Path(root), replaced_pattern(pattern))
    if path is None:
        return None
    data = path.read_bytes()
    pins_path = Path(root) / "scripts" / "data_pins.json"
    pins = json.loads(pins_path.read_text(encoding="utf-8")) if pins_path.exists() else {}
    if (pins.get(REPLACED_PINS) or {}).get(path.name) != hashlib.sha256(data).hexdigest():
        raise TargetError(f"{path.relative_to(root)} is not pinned by its SHA-256 under "
                          f"{REPLACED_PINS} in scripts/data_pins.json, so its bytes could "
                          "have changed since it was the reported file")
    return path, json.loads(data.decode("utf-8"))


def judged_on_what_it_has(conditions, row, document):
    """A part judged on a replaced run's file: (met, text, the labels judged, what it lacks).
    A condition whose figure the older file does not keep is left out, and the rest decide."""
    judged = [judge(item, row, document) for item in conditions]
    with_target = [(item, met) for item, (_, met) in zip(conditions, judged)
                   if comparison_of(item, row) is not None]
    kept = [(item, met) for item, met in with_target if not isinstance(met, Unjudged)]
    missing = list(dict.fromkeys(met.what for _, met in with_target if isinstance(met, Unjudged)))
    if not kept:
        met = Unjudged(missing[0]) if missing else None
    else:
        met = False if any(met is False for _, met in kept) else True
    labels = [item.get("label") or "its figure" for item, _ in kept] if missing else None
    return met, ", ".join(text for text, _ in judged), labels, missing


def replaced_parts(row, root, specs):
    """Each part of a comparison row judged on the replaced run's file, by label: (met, text,
    the labels judged where the file lacks a figure, what it lacks, its path, its document), or
    None with no replaced file."""
    if not is_comparison(row):
        return None
    old = {}
    for spec in dict.fromkeys(json.dumps(spec, sort_keys=True) for spec in specs):
        spec = json.loads(spec)
        found = replaced_document(root, spec["file"])
        if found is None:
            return None
        path, document = found
        if "lowest_of" in row:
            for label, text, met in lowest_parts(row, document):
                old[label] = (met, text, None, [], path, document)
            continue
        for part in row["parts"]:
            if part["file"] != spec["file"]:
                continue
            conditions = part.get("all") or [{key: value for key, value in part.items()
                                              if key != "label"}]
            old[part["label"]] = (*judged_on_what_it_has(conditions, row, document), path,
                                  document)
    return old


def finished(row, root, specs, parts):
    """Each part with its notes: where the replaced run's outcome differs, both figures, both
    outcomes and the less favourable as the part's verdict; the disturbances recorded in the
    phases it rests on; and, for a write against another system, the write it compares."""
    old = replaced_parts(row, root, specs)
    entries = disturbance_entries(root)
    done = []
    for spec, (label, text, met, path, document) in zip(specs, parts):
        notes = {}
        # A row judged on no other system has no outcome of the replaced run to show, but a
        # disturbed phase it rests on still gives the replaced run's figure there (review C1).
        found = replaced_document(root, spec["file"])
        if found is not None:
            notes["replaced"] = found
        if old is not None and label in old:
            old_met, old_text, labels, missing, old_path, old_document = old[label]
            notes["replaced"] = (old_path, old_document)
            if standing(old_met) != standing(met):
                notes["change"] = {"new": met, "old": old_met}
                clock = (f", judged on {' and '.join(labels)} alone, as it keeps no "
                         f"{' and no '.join(missing)}" if labels else "")
                text = (f"{text}, and on the replaced run, `{old_path.relative_to(root)}`"
                        f"{clock}, {old_text}")
                met = min((met, old_met), key=standing)
        reads = reads_of_part(row, spec, label, document)
        found = disturbed(entries, root, path, reads)
        if found:
            notes["disturbed"] = found
        # Each figure the part rests on that the record flags as timed with a page swapped in,
        # and, where the replaced run's outcome is counted with this run's, the replaced run's.
        swapped = [(*read[:2], False) for read in swapped_reads(reads, document)]
        if notes.get("change"):
            swapped += [(*read[:2], True) for read in
                        swapped_reads(reads, notes["replaced"][1])]
        if swapped:
            notes["swapped"] = list(dict.fromkeys(swapped))
        writes = [read for read in reads if read[0] == "write"]
        if writes and is_comparison(row):
            notes["write"] = writes[0][3]
        done.append((label, text, met, path, document, notes))
    return done


def phase_of_path(path, document):
    """The timed phase a path into a comparison file reads, or None: a write of a class, with
    the field read from it, or a query of a kind, of a system's variant."""
    if len(path) < 7 or path[0] != "systems" or path[2] != "variants":
        return None
    system = path[1]
    variant = path[3]
    if variant == "@headline":
        variant = walk(document, ["systems", system, "headline_variant"])
    if path[4] == "classes" and path[6] == "write":
        return ("write", system, variant, path[5], path[7] if len(path) > 7 else None)
    if path[4] == "queries":
        return ("queries", system, variant, path[6], path[5])
    return None


def reads_of(expression, document):
    """The timed phases an expression reads."""
    found = []
    if not isinstance(expression, dict):
        return found
    for key in ("path", "engine_only"):
        if key in expression:
            phase = phase_of_path(expression[key], document)
            found += [phase] if phase else []
    for key in ("case", "cases_median"):
        if key in expression:
            found.append(("case", expression[key]))
    for key in ("div", "mul"):
        for item in expression.get(key, ()):
            found += reads_of(item, document)
    return found


def reads_of_part(row, spec, label, document):
    """The timed phases a part's verdict rests on: every figure its conditions read."""
    if "lowest_of" in row:
        query, kind = label.split(" ", 1)
        return [("queries", system, walk(document, ["systems", system, "headline_variant"]),
                 kind, query) for system in [spec["ours"], *spec["others"]]]
    found = []
    for item in spec.get("all") or [spec]:
        found += reads_of(item.get("value"), document)
        comparison = comparison_of(item, row)
        if comparison is not None:
            found += reads_of(comparison[1], document)
    return list(dict.fromkeys(found))


# The record of what else ran on the host during a campaign's timed steps, one per campaign.
DISTURBANCES = "results/disturbances-*.json"
DISTURBANCE_SCHEMA = "tick-store disturbances 1"
DISTURBANCE_PHASES = {"write": ("system", "variant", "class", "repetition"),
                      "read back": ("system", "variant", "class"),
                      "warm queries": ("system", "variant", "query"),
                      "cold queries": ("system", "variant", "query"),
                      "case": ("case",)}
DISTURBANCE_COMMON = ("start", "end", "precision", "what", "document", "phase", "timed")
UTC = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ")
FULL_COMMIT = re.compile(r"[0-9a-f]{40}")
_DISTURBANCES = {}


def disturbance_entries(root):
    """Every entry of the tree's disturbance files, each checked."""
    entries = []
    for path in sorted(Path(root).glob(DISTURBANCES)):
        stat = path.stat()
        key = (str(path.resolve()), stat.st_mtime_ns, stat.st_size)
        if key not in _DISTURBANCES:
            _DISTURBANCES[key] = check_disturbances(path, Path(root))
        entries += _DISTURBANCES[key]
    return entries


def check_disturbances(path, root):
    """A disturbance file's entries, its shape refused unless whole: its commit, matching its
    name and the commit of every document it places an entry in; how the list was made; what
    it leaves out and why; and each entry's UTC span, what it was in words, and the document,
    system, variant and phase it fell in, which must exist there, with whether that phase is
    timed."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TargetError(f"{path.name}: {error}") from error
    what = path.name
    if not isinstance(document, dict) or document.get("schema") != DISTURBANCE_SCHEMA:
        raise TargetError(f"{what} is not a {DISTURBANCE_SCHEMA!r} file")
    unknown = sorted(set(document) - {"schema", "commit", "how", "not_listed", "entries"})
    if unknown:
        raise TargetError(f"{what} holds {unknown[0]!r}, which no disturbance file has")
    commit = document.get("commit")
    if not isinstance(commit, str) or not FULL_COMMIT.fullmatch(commit) or \
            what != f"disturbances-{commit[:12]}.json":
        raise TargetError(f"{what}: its commit {commit!r} is not a full commit its name gives")
    if not isinstance(document.get("how"), str) or not document["how"].strip():
        raise TargetError(f"{what} says nothing of how its list was made")
    for item in document.get("not_listed", []):
        if not isinstance(item, dict) or set(item) != {"what", "why"} or \
                not all(isinstance(item[key], str) and item[key].strip() for key in item):
            raise TargetError(f"{what}: each thing it leaves out gives what it was and why")
    entries = document.get("entries")
    if not isinstance(entries, list) or not entries:
        raise TargetError(f"{what} lists no disturbance")
    checked = []
    for index, entry in enumerate(entries):
        where = f"{what} entry {index + 1}"
        if not isinstance(entry, dict):
            raise TargetError(f"{where} is not an object")
        phase = entry.get("phase")
        if phase not in DISTURBANCE_PHASES:
            raise TargetError(f"{where}: no phase {phase!r}; one of {sorted(DISTURBANCE_PHASES)}")
        wanted = {*DISTURBANCE_COMMON, *DISTURBANCE_PHASES[phase]}
        if set(entry) != wanted:
            raise TargetError(f"{where} gives {sorted(entry)}, and a {phase} entry gives "
                              f"{sorted(wanted)}")
        start, end = entry["start"], entry["end"]
        if not all(isinstance(value, str) and UTC.fullmatch(value) for value in (start, end)) \
                or end < start:
            raise TargetError(f"{where}: its span {start!r} to {end!r} is not UTC, start first")
        if entry["precision"] not in ("second", "minute"):
            raise TargetError(f"{where}: its precision is the second or the minute")
        if entry["precision"] == "minute" and (not start.endswith(":00Z") or
                                               not end.endswith(":00Z") or start == end):
            raise TargetError(f"{where}: a time known to the minute spans whole minutes")
        if not isinstance(entry["what"], str) or not entry["what"].strip() or \
                NUMBER.search(entry["what"]):
            raise TargetError(f"{where} says in words, with no figure, what it was")
        timed = phase != "read back"
        if entry["timed"] is not timed:
            raise TargetError(f"{where}: a {phase} phase is {'' if timed else 'not '}timed")
        name = Path(str(entry["document"]))
        source = root / "results" / name
        if ".." in name.parts or name.is_absolute() or not source.is_file():
            raise TargetError(f"{where}: no results file {entry['document']!r}")
        placed = json.loads(source.read_text(encoding="utf-8"))
        if (placed.get("header") or {}).get("commit") != commit:
            raise TargetError(f"{where}: {entry['document']} was not measured at {commit[:12]}")
        try:
            if phase == "case":
                if entry["case"] not in [case.get("case") for case in placed["results"]]:
                    raise KeyError(entry["case"])
            else:
                variant = placed["systems"][entry["system"]]["variants"][entry["variant"]]
                if "class" in entry:
                    variant["classes"][entry["class"]]
                if phase == "write" and (isinstance(entry["repetition"], bool) or not isinstance(
                        entry["repetition"], int) or not 1 <= entry["repetition"] <=
                        variant["classes"][entry["class"]]["write"]["repetitions"]):
                    raise KeyError(f"repetition {entry['repetition']!r}")
                if "query" in entry and entry["query"] is not None and \
                        entry["query"] not in QUERY_NAMES:
                    raise KeyError(entry["query"])
        except (KeyError, TypeError) as error:
            raise TargetError(f"{where}: {entry['document']} has no {error}") from error
        checked.append(entry)
    return checked


def disturbed(entries, root, path, reads):
    """The entries that fall in a timed phase a part rests on: (entry, the phase's read)."""
    try:
        name = Path(path).resolve().relative_to((Path(root) / "results").resolve()).as_posix()
    except ValueError:
        return []
    found = []
    for entry in entries:
        if entry["document"] != name:
            continue
        for read in reads:
            if read[0] == "case":
                hit = entry["phase"] == "case" and fnmatch.fnmatchcase(entry["case"], read[1])
            elif read[0] == "write":
                hit = entry["phase"] == "write" and \
                    (entry["system"], entry["variant"], entry["class"]) == read[1:4]
            else:
                hit = entry["phase"] == f"{read[3]} queries" and \
                    (entry["system"], entry["variant"]) == read[1:3] and \
                    entry["query"] in (None, read[4])
            if hit:
                found.append((entry, read))
                break
    return found


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


def utc_span(entry):
    """An entry's span as it reads: its date, then its times, UTC."""
    start, end = entry["start"], entry["end"]
    date = start[:10]
    if entry["precision"] == "minute":
        return f"{date} {start[11:16]} UTC, to the minute"
    if start == end:
        return f"{date} {start[11:19]} UTC"
    return f"{date} {start[11:19]} to {end[11:19]} UTC"


def phase_words(entry, read):
    """The phase an entry fell in, in words: a query phase the entry names whole is its kind's
    queries, so the words hold no query's name the README's first screen refuses."""
    if read[0] == "case":
        return f"the case `{entry['case']}`"
    system = SYSTEM_LABEL.get(entry["system"], entry["system"])
    if read[0] == "write":
        return f"{system}'s `{entry['variant']}` class {entry['class']} write {entry['repetition']}"
    if entry.get("query") is None:
        return f"{system}'s `{entry['variant']}` {read[3]} queries"
    return f"{system}'s `{entry['variant']}` {read[3]} {entry['query']}"


def replaced_figure(read, replaced, row):
    """The replaced run's figure of a phase: a write's rate where the part reads its rate, else
    its total seconds, or a query's median."""
    if read[0] == "case" or replaced is None:
        return None
    try:
        variant = walk(replaced[1], ["systems", read[1], "variants", read[2]])
        if read[0] == "write":
            if len(read) > 4 and read[4] == "records_per_second_median":
                return "rate", walk(variant, ["classes", read[3], "write", read[4]])
            return "total", walk(variant, ["classes", read[3], "write", "seconds_median"])
        return "median", walk(variant, ["queries", read[4], read[3], "median_us"])
    except TargetError:
        return None


def disturbance_text(entry, read, replaced, row):
    """One disturbance in a phase, its span, and the replaced run's figure there if any."""
    text = f"{phase_words(entry, read)}, which held {entry['what']}, {utc_span(entry)}"
    figure = replaced_figure(read, replaced, row or {})
    if figure is not None:
        text += f", where the replaced run's {figure[0]} is {shown(figure[1], row or {})}"
    return text


# What a mark says of a figure timed while a page was swapped in, as the record's own
# timed_policy has it: the number is kept and marked, and a verdict that rests on it says so.
# The systems follow a semicolon, so a sentence read for a faster write ends before them.
SWAPPED = "rests on figures timed while pages were swapped in; the record flags "
# The same mark in the README's pitch, which has no room for the systems and variants: a claim
# there that rests on a rival's flagged figure says this, and the table below it keeps the whole
# mark. It is held only in the pitch; a quote of a verdict anywhere else takes SWAPPED.
PITCH_SWAPPED = "some rivals' figures were timed with pages swapped in"


def swapped_reads(reads, document):
    """The query phases among reads whose record flags a page swapped in while they were
    timed: (system, variant, kind, query)."""
    found = []
    for read in reads:
        if read[0] != "queries":
            continue
        try:
            block = walk(document, ["systems", read[1], "variants", read[2], "queries", read[4],
                                    read[3]])
        except TargetError:
            continue
        if isinstance(block, dict) and (block.get("swap_timed") or {}).get("flagged"):
            found.append(read[1:])
    return found


def change_text(part, labelled=True):
    """A part whose outcome the replaced run's figures change: its outcome on this run first,
    the verdict word the rule gives it, then the replaced run's, whether the change favours
    tick-store, and the one every count, ranking and first screen takes."""
    change = notes_of(part)["change"]
    favours = ", a change that favours tick-store," if \
        standing(change["new"]) > standing(change["old"]) else ""
    return ((f"({part[0]}) " if labelled else "") +
            f"{verdict_word(change['new'])} on this run's figures{favours} and "
            f"{verdict_word(change['old'])} on the replaced run's, so counted as "
            f"{verdict_word(counted(part))} in every count, ranking and first screen on these pages")


def change_marks(parts):
    return [change_text(part) for part in parts if notes_of(part).get("change")]


def phase_marks(parts, row=None, labelled=True):
    """What stands beside a verdict for the phases its parts rest on: each disturbance recorded
    in one, with the replaced run's figure there where there is one; and each figure the record
    flags as timed while a page was swapped in. Unlabelled, as on the README's first screen,
    the marks name no part."""
    held = {}
    for part in parts:
        for entry, read in notes_of(part).get("disturbed", []):
            text = "rests on " + disturbance_text(entry, read, notes_of(part).get("replaced"), row)
            held.setdefault(text, []).append(part[0])
    for part in parts:
        swapped = notes_of(part).get("swapped") or []
        if swapped:
            text = SWAPPED + and_list([f"{SYSTEM_LABEL.get(system, system)}'s `{variant}`" + (
                " on the replaced run" if old else "") for system, variant, old in swapped])
            held.setdefault(text, []).append(part[0])
    marks = []
    for text, labels in held.items():
        who = "" if not labelled else "every part " if len(labels) == len(parts) > 1 else \
            ", ".join(f"({label})" for label in labels) + " "
        marks.append(f"{who}{text}")
    return marks


def verdict_marks(parts, row=None):
    """Every mark that stands beside a verdict, as the essay quotes them: each part whose
    outcome the replaced run's figures change, with both outcomes, whether the change favours
    tick-store and the one counts take; and each mark of the phases its parts rest on."""
    return change_marks(parts) + phase_marks(parts, row)


def verdict_of(parts, row=None, labelled=True):
    """The verdict as the rule gives it, each changed part under its own outcome with its mark
    beside the word, and the marks of the phases it rests on."""
    marks = phase_marks(parts, row, labelled)
    return shown_verdict(parts) + "".join(f"; {mark}" for mark in marks)


def shown_verdict(parts):
    """The verdict words: a part the replaced run's figures change stands apart, under the
    outcome the rule gives it on this run, with both outcomes and the counted one beside that
    word, and the others' counted outcomes as core_verdict gives them (review C1, E)."""
    changed = [part for part in parts if notes_of(part).get("change")]
    if not changed:
        return core_verdict(parts)
    pieces = [change_text(part) + ", both runs' figures under Measured" for part in changed]
    rest = [part for part in parts if not notes_of(part).get("change")]
    if not rest:
        return "; ".join(pieces)
    words = core_verdict(rest)
    if words in ("met", "missed"):
        words = f"{words} on " + ", ".join(f"({part[0]})" for part in rest)
    met = [piece for piece in words.split("; ") if piece.startswith("met on ")]
    others = [piece for piece in words.split("; ") if not piece.startswith("met on ")]
    return "; ".join(met + pieces + others)


def core_verdict(parts):
    """The verdict word or words on the parts' counted outcomes, with nothing beside them."""
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
    paths = dict.fromkeys([*(part[3] for part in parts),
                           *(notes_of(part)["replaced"][0] for part in parts
                             if notes_of(part).get("change"))])
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
                items[kind].append((row["host"], marked_name(name, part, row)))
    return items, met_parts, counts, named


def marked_name(name, part, row):
    """A left-out part's plain name on the line under the table, with each mark its verdict
    carries beside it in brackets, unlabelled, as the first screen names no part (review C1)."""
    marks = ([change_text(part, labelled=False)] if notes_of(part).get("change") else []) + \
        phase_marks([part], row, labelled=False)
    return name + "".join(f" ({mark})" for mark in marks)


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
    miss. A part whose verdict carries a mark is named with it, as marked_name gives it: each
    such name is read whole before the line is split, and a name without its mark is not the
    part's name (review C1)."""
    tokens = {}
    for row in definitions["rows"]:
        key = (row["id"], row["host"])
        if key not in shown:
            continue
        for part in judged_parts(row, root)[0]:
            name = names[key].get("parts", {}).get(part[0])
            if part[0] in shown[key] or not name or class_of([counted(part)]) == "met":
                continue
            token = marked_name(name, part, row)
            if token != name:
                tokens.setdefault(token, f"\x00{len(tokens)}\x00")
    for token in sorted(tokens, key=len, reverse=True):
        line = line.replace(token, tokens[token])
    whole = {shield: token for token, shield in tokens.items()}
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
            kind = class_of([counted(part) for part in parts]) if parts is not None else \
                "not judged"
            expected[kind] = expected.get(kind, 0) + 1
            if key in asked and kind != "met":
                expected_named.setdefault(kind, []).append(names[key]["name"])
            continue
        total_parts += len(parts)
        for part in parts:
            if part[0] in shown[key]:
                shown_parts_count += 1
                continue
            kind = class_of([counted(part)])
            if kind == "met":
                met_parts += 1
                continue
            name = marked_name(names[key].get("parts", {}).get(part[0]), part, row)
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
        given = {(where, re.sub("\x00\\d+\x00", lambda found: whole[found.group(0)], name))
                 for where, name in given}
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


def under_sentence(under, row, parts, hosts, root):
    """The sentence under the table, with its row's figure and verdict."""
    return filled([under["text"]], under, row, parts, hosts, "the sentence under the table",
                  {"verdict": verdict_of(parts, row, labelled=False),
                   **value_marks(under, parts, root, row)})[0]


def check_under(sentence, under, row, parts, hosts, root):
    """The sentence under the table, checked apart from the code that writes it: a row met or
    missed in whole says that word, and any other says neither (review DX-4)."""
    kind = class_of([counted(part) for part in parts])
    marks = "".join(f"; {mark}" for mark in phase_marks(parts, row, labelled=False))
    beside = value_marks(under, parts, root, row)
    if kind in ("met", "missed"):
        expected = filled([under["text"]], under, row, parts, hosts,
                          "the sentence under the table", {"verdict": kind + marks, **beside})[0]
        if sentence != expected:
            raise TargetError(f"the sentence under the table reads {sentence!r}, and its row is "
                              f"{kind}: {expected!r}")
        return
    for word in ("met", "missed"):
        if sentence == filled([under["text"]], under, row, parts, hosts,
                              "the sentence under the table", {"verdict": word + marks,
                                                               **beside})[0]:
            raise TargetError(f"the sentence under the table says {word}, and its row is {kind}")


def value_marks(spec, parts, root, row):
    """For each named value of a headline text read from a timed phase that holds a recorded
    disturbance, or that the record flags as timed with a page swapped in, the value
    "<name>_mark" with what figure_marks says of it. filled() refuses a value no text names, so
    a text that shows such a figure and not its mark is refused (review C1, 4d)."""
    marks = {}
    entries = disturbance_entries(root)
    for name, value in (spec.get("values") or {}).items():
        expression = value.get("value") if isinstance(value, dict) else None
        if not isinstance(expression, dict):
            continue
        found = []
        for path, document in {part[3]: part[4] for part in parts}.items():
            replaced = notes_of(next(part for part in parts if part[3] == path)).get("replaced")
            for read in reads_of(expression, document):
                found += figure_marks(entries, root, path, document, read, replaced, row)
        if found:
            marks[f"{name}_mark"] = ", ".join(dict.fromkeys(found))
    return marks


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
                           "its bar and measured", value_marks(entry, parts, root, row))
    return (entry["what"], f"{bar} ({entry['source']})", measured,
            verdict_of(parts, row, labelled=False)), parts


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
            sentence = under_sentence(under, row, parts, hosts, root)
            check_under(sentence, under, row, parts, hosts, root)
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


def pitch_swapped(condition, definitions, root):
    """The systems whose figures, flagged by the record as timed while pages were swapped in,
    one condition of a pitch claim rests on: a target's parts as RESULTS.md marks them, or
    each system's headline query a fastest claim compares."""
    if "target" in condition:
        spec = condition["target"]
        row = next((row for row in definitions["rows"]
                    if (row["id"], row["host"]) == (spec["id"], spec["host"])), None)
        parts = judged_parts(row, root)[0] if row is not None else None
        if parts is None:
            return []
        if spec.get("parts"):
            parts = [part for part in parts if part[0] in spec["parts"]]
        return [system for part in parts
                for system, _, _ in notes_of(part).get("swapped") or []]
    if "fastest" in condition:
        path = target_file(root, condition["file"])
        if path is None:
            return []
        document = json.loads(path.read_text(encoding="utf-8"))
        spec = condition["fastest"]
        found = []
        for system in spec["systems"]:
            for query in spec["queries"]:
                for kind in spec["kinds"]:
                    try:
                        block = walk(document, ["systems", system, "variants", "@headline",
                                                "queries", query, kind])
                    except TargetError:
                        continue
                    if isinstance(block, dict) and (block.get("swap_timed") or {}).get("flagged"):
                        found.append(system)
        return found
    return []


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
    at most "max_words" words, and holds no character a reader cannot see. A claim resting on a figure the record flags as timed
    while pages were swapped in says so, in PITCH_SWAPPED's words."""
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
            # A claim resting on a figure timed while pages were swapped in says so, in the
            # pitch's short form; that form speaks of rivals, so tick-store's own flagged figure
            # cannot rest under it (review C1-9).
            flagged = pitch_swapped(condition, definitions, root)
            if "tickstore" in flagged:
                raise TargetError(f"the README's pitch says {claim['text']!r}, which rests on "
                                  "tick-store's own figures timed while pages were swapped in, "
                                  f"and the pitch's mark, {PITCH_SWAPPED!r}, speaks of rivals")
            if flagged and PITCH_SWAPPED not in claim["text"]:
                raise TargetError(f"the README's pitch says {claim['text']!r}, which rests on "
                                  "figures the record flags as timed while pages were swapped "
                                  f"in, and does not say {PITCH_SWAPPED!r}")
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
        # A claim takes each part's less favourable outcome, worked out here from both runs.
        verdict = core_verdict([(part[0], part[1], counted(part), *part[3:]) for part in parts])
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


# A sentence that says one system writes faster than another: a word for a write, and after a
# word for faster, the other system's name.
WRITE_WORDS = re.compile(r"\b(?:writes?|wrote|written|writing|ingest(?:s|ed|ing)?|loads?|loaded|"
                         r"loading)\b", re.IGNORECASE)
FASTER_WORDS = re.compile(r"\b(?:faster|quicker|sooner|fastest|quickest|no slower|not slower|"
                          r"less time|outpac(?:e|es|ed|ing)|outruns?|beats?|beating|ahead of)\b",
                          re.IGNORECASE)


def disk_read_writers(root):
    """Each other system whose headline write, in a four-system comparison file of the tree,
    read its input from disk inside its clock, by its record: its input not wholly resident
    when the clock started. Where is that write, by name, for the reason."""
    found = {}
    for path in sorted((Path(root) / "results" / "compare").glob("*.json")):
        document = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(document.get("systems"), dict) or is_alone(document):
            continue
        for system in ordered_systems(document):
            if system == "tickstore":
                continue
            variant, entry = headline(document, system)
            for class_name, result in sorted(entry.get("classes", {}).items()):
                start = ((result.get("write") or {}).get("detail") or {}).get(
                    "input_at_clock_start") or {}
                if "pages" in start and start["resident_pages"] < start["pages"]:
                    found.setdefault(SYSTEM_LABEL[system],
                                     f"`{path.name}`, `{variant}` class {class_name}")
    return found


def faster_write_problem(texts, writers):
    """Why one of the texts says tick-store writes faster than a system on a clock that holds
    that system's disk reads, or None: no generated sentence on any page may, since tick-store's
    input is in memory before its clock starts and the other's is read inside it."""
    for text in texts:
        for sentence in re.split(r"(?<=[.;!?])\s+|\s+\|\s+|\n", text):
            if not WRITE_WORDS.search(sentence):
                continue
            for found in FASTER_WORDS.finditer(sentence):
                after = sentence[found.end():]
                for label, where in writers.items():
                    if re.search(rf"\b{re.escape(label)}\b", after):
                        return (f"the sentence {sentence.strip()!r} says a write is "
                                f"{found.group(0)!r} than {label}'s, whose clock holds its "
                                f"disk reads ({where}: its input was not resident when the "
                                "clock started); say what each clock holds instead")
    return None


# The pages that present results, beside the README and RESULTS.md, which --readme and
# --results-md name: paths under --targets-root, whose whole prose the faster-write rule reads,
# as it reads the README's and RESULTS.md's. A page the tree lacks is not read, and the tests
# hold this list to the repository's pages. Left out: docs/NOTES.md, docs/TASKS.md and
# docs/REVIEW.md, and the research folder, are the working record, which quotes each run's
# verdicts and the reviews' findings as they stood, not pages that present results; docs/format.md
# and docs/store.md describe the store and state no comparison; tests/unit/crashers/README.md
# says where a crash input goes; and the essay is held to the rule by render_writeup.py.
RESULTS_PAGES = ("BENCHMARKS.md", "compare/README.md", "fuzz/README.md")
# A line that starts a unit of prose of its own rather than carrying on a wrapped paragraph.
UNIT_START = re.compile(r"\s*(?:$|[|#>]|```|<!--|(?:[-*+]|\d+[.)])\s)")
NOT_CARRIED_ON = re.compile(r"\s*(?:$|[|#]|```|<!--)")


def prose_units(text):
    """(line, text) of each paragraph, list item, table row and heading of a page, a wrapped
    paragraph or item joined into one, so a sentence is read whole across its line breaks."""
    units = []
    carry = False
    for number, line in enumerate(text.split("\n"), 1):
        if carry and not UNIT_START.match(line):
            units[-1] = (units[-1][0], units[-1][1] + " " + line.strip())
        elif line.strip():
            units.append((number, line.strip()))
        carry = not NOT_CARRIED_ON.match(line)
    return units


def page_problem(pages, writers):
    """Where and why a page's prose says tick-store writes faster than a system on a clock
    that holds that system's disk reads, or None. pages is (name, text) pairs."""
    for name, text in pages:
        for number, unit in prose_units(text):
            problem = faster_write_problem([unit], writers)
            if problem:
                return f"{name}:{number}: {problem}"
    return None


# A target's id as a page writes it, or a run of them, "QL-1 to QL-3"; a word that gives a
# verdict; and the words that name each host on the pages.
TARGET_ID = re.compile(r"(?<![\w-])([A-Z]{1,3})-(\d+)(?: to (?:\1-)?(\d+))?(?![\w-])")
VERDICT_WORDS = re.compile(r"\b(?:met|missed|meets?|misses|not judged)\b", re.IGNORECASE)
HOST_WORDS = {"Linux": ("Linux", "the Ryzen"), "macOS": ("macOS", "the laptop", "the Mac")}
HOST_CELLS = {"any": ("any",), "linux": ("Linux",), "macos": ("macOS",),
              "both": ("Linux", "macOS", "any")}
# The pages whose hand-written text a reader takes a verdict from: every one the faster-write
# rule reads, the generated blocks left out, which carry the marks by construction.
GENERATED_BLOCKS = ((TARGETS_BEGIN, TARGETS_END), (BEGIN, END), (COMPARE_BEGIN, COMPARE_END),
                    (HEADLINE_BEGIN, HEADLINE_END))


def without_blocks(text):
    """A page with its generated blocks blanked, their lines kept, so line numbers hold."""
    for begin, end in GENERATED_BLOCKS:
        found = block_of(text, begin, end)
        if found is not None:
            before, block, after = found
            text = before + "\n" * block.count("\n") + after
    return text


def row_marks(row, root):
    """Every mark a quote of a row's verdict carries: each mark RESULTS.md sets beside it, word
    for word, a changed part's own included, and for a write against other systems the input
    clause input_clause gives (review C1, item 8)."""
    parts, _ = judged_parts(row, root)
    if parts is None:
        return []
    marks = change_marks(parts) + phase_marks(parts, row)
    classes = [notes_of(part)["write"] for part in parts if notes_of(part).get("write")]
    clause = input_clause(parts[0][4], classes[0]) if classes else None
    return marks + ([clause] if clause else [])


def quoted_rows(unit, rows):
    """(text, the (id, host) rows whose verdict it quotes) for a unit of a page: a table row
    whose first cell is a target's id and second its host, whole; or each sentence that names
    a target's id and a verdict word, on the host it names where the target has a row there."""
    if unit.startswith("|"):
        cells = [cell.strip() for cell in unit.strip().strip("|").split("|")]
        hosts = HOST_CELLS.get(cells[1].lower()) if len(cells) > 2 else None
        if not hosts or not any(key[0] == cells[0] for key in rows):
            return []
        return [(unit, {key for key in rows if key[0] == cells[0] and key[1] in hosts})]
    found = []
    for sentence in sentences_of(unit):
        if not VERDICT_WORDS.search(sentence):
            continue
        named = set()
        for match in TARGET_ID.finditer(sentence):
            last = int(match.group(3) or match.group(2))
            named.update(f"{match.group(1)}-{number}"
                         for number in range(int(match.group(2)), last + 1))
        hosts = [host for host, words in HOST_WORDS.items()
                 if any(re.search(rf"(?<![\w-]){re.escape(word)}(?![\w-])", sentence)
                        for word in words)]
        keys = set()
        for ident in named:
            own = {key for key in rows if key[0] == ident}
            there = {key for key in own if len(hosts) == 1 and key[1] in (hosts[0], "any")}
            keys |= there or own
        if keys:
            found.append((sentence, keys))
    return found


def quote_problem(pages, definitions, root):
    """Where and why a hand-written row or sentence quotes the verdict of a target-and-machine
    row without a mark RESULTS.md sets beside that verdict, or None. pages is (name, text)."""
    rows = {(row["id"], row["host"]): row for row in definitions["rows"]}
    marks = {}
    for name, text in pages:
        for number, unit in prose_units(without_blocks(text)):
            for quote, keys in quoted_rows(unit, rows):
                said = " ".join(quote.split())
                for key in sorted(keys):
                    if key not in marks:
                        marks[key] = row_marks(rows[key], root)
                    missing = [mark for mark in marks[key] if mark not in said]
                    if missing:
                        return (f"{name}:{number}: {quote[:80]!r} quotes the verdict of {key[0]} "
                                f"on {key[1]} without a mark RESULTS.md sets beside it: "
                                f"{missing[0]!r}")
    return None


# The sections that describe the comparison's method by hand, each of which gives the input
# note of every comparison whose writes did not start alike (review C1, condition 2).
METHOD_SECTIONS = (("README.md", "## Comparison"), ("RESULTS.md", "## Comparison"))


def method_problem(pages, root):
    """Where a section describing the comparison's method lacks method_input_note's sentence for
    a comparison file of the tree, or None. pages is (name, text); its generated blocks do not
    count, so the hand-written method says it."""
    notes = []
    for path in sorted((Path(root) / "results" / "compare").glob("*.json")):
        note = method_input_note(json.loads(path.read_text(encoding="utf-8")))
        if note:
            notes.append((path.name, note))
    texts = dict(pages)
    for name, heading in METHOD_SECTIONS:
        if name not in texts or not notes:
            continue
        lines = without_blocks(texts[name]).split("\n")
        if heading not in lines:
            return f"{name} has no {heading!r} section to describe the comparison's method"
        start = lines.index(heading) + 1
        end = next((index for index in range(start, len(lines))
                    if re.match(r"#{1,2} ", lines[index])), len(lines))
        section = " ".join(" ".join(lines[start:end]).split())
        for file_name, note in notes:
            if note not in section:
                return (f"{name}'s {heading!r} does not say where the writes' inputs were in "
                        f"{file_name}: {note!r}")
    return None


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
            blocks.append((COMPARE_BEGIN, COMPARE_END, render_compare(args.compare_results,
                                                                      args.targets_root)))
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
        disturbance_entries(args.targets_root)
        writers = disk_read_writers(args.targets_root)
        generated = [line for _, _, lines in blocks + headline for line in lines]
        problem = faster_write_problem(generated, writers)
        if problem:
            raise TargetError(problem)
        pages = [(md_name, text)]
        if readme is not None:
            pages.append((args.readme.name, readme))
        pages += [(name, (args.targets_root / name).read_text(encoding="utf-8"))
                  for name in RESULTS_PAGES if (args.targets_root / name).is_file()]
        problem = page_problem(pages, writers)
        if problem:
            raise TargetError(problem)
        # The pages as they will be, their generated blocks as rendered now, for the moves.
        written = [(name, without_blocks(text)) for name, text in pages] + \
            [("the generated blocks", "\n".join(generated))]
        problem = move_problem(written) or keeps_problem(written, args.targets_root)
        if problem:
            raise TargetError(problem)
        if has_targets:
            problem = quote_problem(pages, json.loads(args.targets.read_text(
                encoding="utf-8")), args.targets_root) or method_problem(pages,
                                                                         args.targets_root)
            if problem:
                raise TargetError(problem)
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
