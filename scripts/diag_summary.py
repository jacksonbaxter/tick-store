#!/usr/bin/env python3
"""Turn one diagnostic run's raw text reports into one small results/diag/ file.

    python3 -I scripts/diag_summary.py DIR --commit SHA [--out PATH] [--forbid WORD ...]

DIR is the folder one of the freeze runner's diagnostic steps fills on the Linux host
(`staging/diag/<kind>/`, docs/NOTES.md, The diagnostic runs at the freeze). Its `facts.txt`
names the kind, the commit, the build, the wrapper, the event and every command that made a
report; the reports are perf's own text, `perf report --stdio` and `perf stat -x,`, and the
tools' own JSON lines. The four kinds and what each file holds:

    writer-profile        D1: the writer's sampled user-mode cycles over the day, by group of
                          functions, with inclusive shares and the store's codecs by column
    query-profile         D2: the same for each of Q1 to Q5 on that store
    counters-simd         D3: user-mode counters of the write and of each query process, and
                          each query's engine times at AVX2 and capped to scalar, read from the
                          query tool's own answer lines, never from wall time
    price-decode-profile  D4: the price column's decode in column_decode_bench's timed passes

Each is a diagnostic: one run, at one commit, judging no target. A profile's figures are shares
of sampled user cycles, computed here from perf's per-line periods (or its percentages where a
report has no period column) and grouped by the GROUPS table below: a symbol goes to the first
group with a pattern that matches it, and whatever no pattern matches is its own share,
`unmatched`, with its largest symbols listed, never spread over the groups. No time is reported
but D3's engine times.

It refuses, exit 1, input it cannot account for: a report with no samples, a commit other than
--commit, self shares that do not sum to 100 within the report's rounding, or whose periods do
not add up to its event count, a line it cannot read, a build other than the bench preset, a
query answer that is an error or ran other kernels than its cap allows, and output that would
hold an absolute path, this machine's name or user, or a --forbid word. A profile is tied to its
recording: its self report must name its event, give each line's samples and period and the
event count, and hold every sample perf record says it wrote; more than LOST_AT_MOST of the
samples lost is refused; the children and source-file reports must be of the same recording,
the same event, sample count and event count. An inclusive share that two entries would add up
is refused, and so are query runs on different stores, a counter perf multiplexed, an AVX2 run
whose answers ran no AVX2 kernel where its scalar twin ran kernels, and answers whose rows or
digest differ between the levels or the rounds. A smoke's folder is iteration only: it is
refused unless --iteration is given, and never written anywhere under results/.

What the raw output may not hold is not guessed: a report with no lost-samples line, a recording
whose sample count perf record did not print, or a query whose answers give no kernel runs or no
digest leaves that fact out, listed in the summary's not_established with the path it covers
and why, and the ratio it would need is null. scripts/render_writeup.py refuses a token that
reads a path so listed.

Standard library only, with scripts/run_bench.py's own readings of /proc/stat, the load lines
and the boost state, so D3 reads them as the benchmarks do.
"""

import argparse
import getpass
import gzip
import json
import re
import socket
import statistics
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import run_bench  # noqa: E402

SCHEMA = "tick-store-diag-1"
DAY = "12302019"
RESULTS_ROOT = REPO_ROOT / "results"
RESULTS = RESULTS_ROOT / "diag"
PRESET = "bench"
CORE = run_bench.LINUX_CORE
TOP_UNMATCHED = 10
SYMBOL_CHARS = 120
# A printed percentage is rounded to two places, so each line is off by at most half a unit.
HALF_UNIT = 0.005
# The most of a recording's samples that may be lost, kept plus lost, before its shares are no
# longer the run's: perf drops samples in bursts, so the loss is not spread evenly.
LOST_AT_MOST = 0.001
# An absolute path: a slash that starts a word or follows = , ; or ( , not one inside a URL.
ABSOLUTE = re.compile(r"(?<![\w:/.~-])/[^\s,;:()'\"]+")
# A path in a command: from a slash at a word's start, or after = : , or a quote, to the next
# space, quote, comma or closing bracket. Only its last part is kept.
PATH_IN_COMMAND = re.compile(r"(?<![\w.~-])/[^\s'\",)]*")

KINDS = ("writer-profile", "query-profile", "counters-simd", "price-decode-profile")
QUERIES = ("Q1", "Q2", "Q3", "Q4", "Q5")
LEVELS = ("avx2", "scalar")
# Kernel sets a run capped to a level must not call: none above it. Under AVX2 a kernel with no
# AVX2 form may still run its scalar one.
ABOVE = {"scalar": ("neon", "avx2"), "avx2": ("neon",)}

STATEMENTS = {
    "writer-profile": (
        "A diagnostic, one run at commit {commit}, judging no target: the writer's sampled "
        "user-mode cycles (cycles:u) while it stores the whole day, as shares by group of "
        "functions. Shares of samples, not times; no time is reported."),
    "query-profile": (
        "A diagnostic, one run at commit {commit}, judging no target: each query process's "
        "sampled user-mode cycles (cycles:u), store open, engine and digest together, as shares "
        "by group of functions. Shares of samples, not times; no time is reported."),
    "counters-simd": (
        "A diagnostic, one run at commit {commit}, judging no target: user-mode counter totals "
        "of the write and of each query process under measure.sh on cpu4, and each query's "
        "engine times at AVX2 and capped to scalar, read from the query tool's own answer "
        "lines (engine_ns), not from wall time. Those engine times are the only times here."),
    "price-decode-profile": (
        "A diagnostic, one run at commit {commit}, judging no target: the sampled user-mode "
        "cycles (cycles:u) of the price column's decode, store_decode/add_price, recorded only "
        "inside column_decode_bench's timed passes, as shares by group of functions. Shares of "
        "samples, not times; no time is reported."),
}

KERNEL = ("level", "k")
UNRESOLVED = ("name", r"(?:0x[0-9a-f]+|\[unknown\])$")
MEMORY = ("name", r"(?:malloc|free|calloc|realloc|cfree|_int_malloc|_int_free\w*|_int_realloc|"
                  r"malloc_consolidate|sysmalloc|tcache\w*|unlink_chunk\w*|__libc_(?:malloc|free|"
                  r"calloc|realloc)|operator new|operator delete|__mem(?:move|cpy|set|cmp)\w*|"
                  r"__mempcpy\w*|mem(?:cpy|move|set|cmp)|mmap\w*|munmap|brk|sbrk)(?:@plt)?$")
CONTAINERS = ("name", r"std::(?:__1::)?(?:vector|basic_string|deque|unordered_map|"
                      r"__hash_table)<")
LIBC = ("dso", r"libc\.so")
LIBCXX = ("dso", r"libc\+\+(?:abi)?\.so")
ZSTD = (("name", r"(?:ZSTD|HUF|FSE|HIST|ZSTDMT|POOL|ZDICT|ERR|XXH)\w*"),
        ("name", r"tick_store::codec::(?:\(anonymous namespace\)::)?(?:encode|decode)_zstd_payload$"))
CODEC = r"tick_store::codec::(?:\(anonymous namespace\)::)?"
STORE = r"tick_store::store::(?:\(anonymous namespace\)::)?"

# GROUPS[kind]: (group, what it is, ((how, pattern), ...)), in order; the first match wins. A
# pattern of kind "name" is matched with re.match against the function's qualified name, the
# symbol without its return type and parameters (function_name); "dso" against the shared
# object; "level" is perf's [k] or [.] mark. tests/scripts/test_diag_summary.py holds a real
# symbol for every pattern.
GROUPS = {
    "writer-profile": (
        ("kernel_addresses", "samples perf placed at a kernel address though only user mode was "
         "counted", (KERNEL,)),
        ("parse", "ITCH framing and decoding", (("name", r"tick_store::itch::"),)),
        ("book_state", "each message taken into the segment: the builder's per-order state, the "
         "reference map, the symbol table and the dispatch on each message's type",
         (("name", r"tick_store::store::SegmentBuilder::State::"),
          ("name", STORE + r"RefMap::"),
          ("name", r"tick_store::store::SymbolTable::"),
          ("name", r"tick_store::store::SegmentBuilder::add$"),
          ("name", r"tick_store::store::ItchStoreWriter::append$"),
          ("name", STORE + r"(?:column_form_plain|hot_fields)$"),
          ("name", r"tick_store::store::SegmentBuilder::records$"),
          ("name", r"std::(?:__1::)?__variant_detail::"))),
        ("segment_assembly", "turning a full segment's messages into columns: the builder's "
         "finish, grouping, time bases, seek and maxima",
         (("name", r"tick_store::store::SegmentBuilder::finish$"),
          ("name", STORE + r"(?:compute_seek|column_maxima|group_by_key|order_by_key|"
                           r"time_bases|time_residuals|column_form_\w+)$"))),
        ("sort", "the standard library's sorts", (
            ("name", r"std::(?:__1::)?(?:ranges::)?(?:__)?(?:introsort|bitset_partition|"
                     r"insertion_sort\w*|partition_with_equals\w*|pdqsort\w*|sort\w*|"
                     r"floyd_sift_down|sift_down|partial_sort\w*|nth_element\w*|"
                     r"stable_sort\w*|merge\w*)"),)),
        ("pfor_encode", "PFOR encoding, the dictionary's code streams included: width and scale "
         "search, packing, patches",
         (("name", CODEC + r"(?:choose_width|encode_scaled|encode_block_with|encode_pfor\w*|"
                           r"plan_vector|emit_vector|scale_candidates|pack_\w+|"
                           r"append_words_le|append_patches)$"),
          ("name", r"tick_store::codec::BitWriter::"))),
        ("dictionary_encode", "the dictionary encoder's own work, outside the PFOR it calls",
         (("name", CODEC + r"(?:encode_dict\w*|encode_ranked_\w+|encode_sorted_mtf|"
                           r"huffman_lengths|scan_for_repeats)$"),)),
        ("zstd", "zstd and its entropy coders", ZSTD),
        ("crc", "CRC32C", (("name", r"tick_store::store::.*crc32c"),)),
        ("store_writer", "the per-column driver, metadata and container writing",
         (("name", r"tick_store::store::(?:ContainerWriter|ContainerLayout)::"),
          ("name", r"tick_store::store::ItchStoreWriter::(?:write_segment|finish|write_last)$"),
          ("name", STORE + r"(?:encode_column|encode_column_metadata|encode_store_header|"
                           r"code_pfor|code_values|pfor_options)$"))),
        ("containers", "the standard library's vectors, strings and hash tables, as compiled "
         "into the binary: growth, copies, assignment", (CONTAINERS,)),
        ("memory", "allocation and copies, where the C library's symbols resolve", (MEMORY,)),
        ("libc", "the C library, its addresses unresolved", (LIBC,)),
        ("libcxx", "the C++ library outside its sorts", (LIBCXX,)),
        ("tool", "the command-line driver", (("name", r"tick_store::harness::"),)),
        ("unresolved", "addresses with no symbol", (UNRESOLVED,)),
    ),
    "query-profile": (
        ("kernel_addresses", "samples perf placed at a kernel address though only user mode was "
         "counted", (KERNEL,)),
        ("digest", "the answer's SHA-256 digest, made after the engine's clock stops",
         (("name", r"tick_store::harness::(?:\(anonymous namespace\)::)?(?:rows_digest|"
                   r"aggregate_digest|load_be32)$"),
          ("name", r"tick_store::harness::(?:Sha256::|sha256$|to_hex$)"))),
        ("dictionary_decode", "dictionary and ranked decode",
         (("name", CODEC + r"(?:decode_dict\w*|read_huffman)$"),)),
        ("pfor_decode", "PFOR block decode", (("name", CODEC + r"decode_pfor\w*$"),)),
        ("unpack_kernels", "the unpack, delta and add kernels of every kernel set",
         (("name", r"tick_store::codec::simd::"),
          ("name", r"tick_store::codec::(?:unpack_vector|unpack_linear)$"))),
        ("zstd", "zstd and its entropy coders", ZSTD),
        ("run_positions", "positions from the per-locate runs",
         (("name", STORE + r"positions_from_runs$"),)),
        ("time_bases", "timestamps from their time bases",
         (("name", STORE + r"(?:time_bases|time_residuals|each_with_base)$"),)),
        ("scan_and_rows", "the query engine's scan and its rows",
         (("name", r"tick_store::store::ItchQuery::"),
          ("name", STORE + r"(?:scan_run|locate_segment|all_segment|all_positions|"
                           r"open_scan_columns|plain_segment|open_stocks|stock_of|"
                           r"count_whole_segment|exact_segment|zone_range|file_range|"
                           r"seed_before|may_hold|bit_of|rank_of)$"))),
        ("column_decode", "the column and block decode drivers",
         (("name", CODEC + r"(?:decode_column|read_block_header|check_block_values|"
                           r"reserve_for_block)$"),
          ("name", STORE + r"(?:decode_column|column_block_decoder|block_decoder|"
                           r"unwrap_column|widen)$"))),
        ("store_open", "opening the store: container, footer, metadata and symbols",
         (("name", r"tick_store::store::(?:ItchStoreReader|ContainerReader|MappedFile)::"),
          ("name", STORE + r"(?:parse_store_header|parse_column_metadata|parse_seek|"
                           r"check_run_directory)$"),
          ("name", r"tick_store::store::SymbolTable::"))),
        ("crc", "CRC32C", (("name", r"tick_store::store::.*crc32c"),)),
        ("answer_io", "reading requests and writing answers",
         (("name", r"tick_store::harness::"),
          ("name", r"std::(?:__1::)?(?:__format\w*|vformat\w*|format\w*|__formatter\w*|"
                   r"basic_ostream|basic_istream|getline|__put_character_sequence)"))),
        ("containers", "the standard library's vectors, strings and hash tables, as compiled "
         "into the binary: growth, copies, assignment", (CONTAINERS,)),
        ("memory", "allocation and copies, where the C library's symbols resolve", (MEMORY,)),
        ("libc", "the C library, its addresses unresolved", (LIBC,)),
        ("libcxx", "the C++ library", (LIBCXX,)),
        ("unresolved", "addresses with no symbol", (UNRESOLVED,)),
    ),
    "price-decode-profile": (
        ("kernel_addresses", "samples perf placed at a kernel address though only user mode was "
         "counted", (KERNEL,)),
        ("zstd", "zstd inflate and its entropy decoders", ZSTD),
        ("pfor_block_decode", "PFOR block decode: header, scale and patches, and what is "
         "inlined into it", (("name", CODEC + r"decode_pfor\w*$"),)),
        ("unpack_kernels", "the unpack, delta and add kernels of every kernel set",
         (("name", r"tick_store::codec::simd::"),
          ("name", r"tick_store::codec::(?:unpack_vector|unpack_linear)$"))),
        ("codec_column_decode", "the codec's column driver: block headers, value checks, "
         "reserving", (("name", CODEC + r"(?:decode_column|read_block_header|"
                                        r"check_block_values|reserve_for_block)$"),)),
        ("store_column_decode", "the store's column decode: its block decoder, range check and "
         "append", (("name", STORE + r"(?:decode_column|column_block_decoder|block_decoder|"
                                     r"unwrap_column|widen)$"),)),
        ("containers", "the standard library's vectors, strings and hash tables, as compiled "
         "into the binary: growth, copies, assignment", (CONTAINERS,)),
        ("memory", "allocation and copies, where the C library's symbols resolve", (MEMORY,)),
        ("libc", "the C library, its addresses unresolved", (LIBC,)),
        ("libcxx", "the C++ library", (LIBCXX,)),
        ("bench_harness", "the benchmark's own loop and checks",
         (("name", r"benchmark::"), ("name", r"tick_store::bench_support::"),
          ("name", r"\(anonymous namespace\)::"))),
        ("unresolved", "addresses with no symbol", (UNRESOLVED,)),
    ),
}

# Inclusive shares: (label, pattern on the function's name), from the --children report.
INCLUSIVE = {
    "writer-profile": (
        ("ItchStoreWriter::append", r"tick_store::store::ItchStoreWriter::append$"),
        ("ItchStoreWriter::write_segment", r"tick_store::store::ItchStoreWriter::write_segment$"),
        ("SegmentBuilder::finish", r"tick_store::store::SegmentBuilder::finish$"),
        ("encode_column", r"tick_store::store::encode_column$"),
        ("encode_pfor", r"tick_store::codec::encode_pfor$"),
        ("encode_pfor_block", r"tick_store::codec::encode_pfor_block$"),
        ("encode_dict", r"tick_store::codec::encode_dict$"),
        ("encode_zstd_payload", CODEC + r"encode_zstd_payload$"),
        ("itch::decode", r"tick_store::itch::decode$"),
    ),
    "query-profile": (
        ("engine", r"tick_store::store::ItchQuery::(?:rows|minute_aggregate|shares_sum|"
                   r"locate_counts)$"),
        ("digest", r"tick_store::harness::(?:\(anonymous namespace\)::)?(?:rows_digest|"
                   r"aggregate_digest)$"),
        ("store_open", r"tick_store::store::ItchStoreReader::open$"),
        ("decode_dict", CODEC + r"decode_dict$"),
        ("decode_pfor", CODEC + r"decode_pfor$"),
        ("positions_from_runs", STORE + r"positions_from_runs$"),
        ("decode_zstd_payload", CODEC + r"decode_zstd_payload$"),
    ),
    "price-decode-profile": (
        ("store decode_column", STORE + r"decode_column$"),
        ("codec decode_column", CODEC + r"decode_column$"),
        ("decode_pfor", CODEC + r"decode_pfor$"),
        ("decode_zstd_payload", CODEC + r"decode_zstd_payload$"),
    ),
}

PERCENT_COLUMNS = ("Overhead", "Children", "Self")
INTEGER_COLUMNS = ("Samples", "Period")
WORD_COLUMNS = ("Command", "Shared Object")
REST_COLUMNS = ("Source File", "Source:Line")
KNOWN_COLUMNS = PERCENT_COLUMNS + INTEGER_COLUMNS + WORD_COLUMNS + REST_COLUMNS + ("Symbol",)
DATA_LINE = re.compile(r"^\s*\d+\.\d+%")
# perf pads a count with no unit with a space: "# Samples: 827  of event 'cycles:u'".
SAMPLES_LINE = re.compile(r"# Samples: (\S+)\s+of event '([^']+)'")
RECORDED = re.compile(r"Captured and wrote .*\((\d+) samples\)")


class Refused(Exception):
    """Input the summary cannot account for. Nothing is written."""


# --- Reading perf report --stdio -------------------------------------------------------------

def function_name(symbol):
    """The qualified name of a demangled symbol: its return type and parameters dropped. A
    space or a parenthesis inside template arguments, (anonymous namespace), decltype(auto) or
    an operator's name does not end it. A lambda or class local to a function is named by that
    function, as f(int)::$_0::operator()() is by f, so its samples go to f's group."""
    text = symbol.strip()
    start, end = name_bounds(text)
    return text[start:end]


def local_to_function(symbol):
    """Whether the symbol is a lambda or class local to a function, f(int)::$_0::operator()(),
    which runs inside f: its own entry is never added to f's inclusive share."""
    text = symbol.strip()
    _start, end = name_bounds(text)
    depth = 0
    for index in range(end, len(text)):
        depth += {"(": 1, ")": -1}.get(text[index], 0)
        if depth == 0:
            return text.startswith("::", index + 1)
    return False


def name_bounds(text):
    """Where the qualified name runs in a demangled symbol: its start, and the index of the
    parameter list's parenthesis, or the text's end."""
    depth = 0
    start = 0
    index = 0
    while index < len(text):
        if text.startswith("(anonymous namespace)", index):
            index += len("(anonymous namespace)")
            continue
        if text.startswith("decltype(auto)", index):
            index += len("decltype(auto)")
            continue
        if text.startswith("operator", index) and (index == 0 or not (
                text[index - 1].isalnum() or text[index - 1] == "_")):
            match = re.match(r"operator(?:\(\)|\[\]|<=>|<<=?|>>=?|->\*?|&&|\|\||\+\+|--|"
                             r" new(?:\[\])?| delete(?:\[\])?|[-+*/%^&|~!=<>,]=?)", text[index:])
            if match:
                index += match.end()
                continue
        char = text[index]
        if char in "<[{":
            depth += 1
        elif char in ">]}":
            depth = max(0, depth - 1)
        elif char == "(" and depth == 0:
            return start, index
        elif char == " " and depth == 0:
            start = index + 1
        index += 1
    return start, len(text)


def column_names(line):
    """The column names of perf's header line, or None if the line is not one."""
    if not line.startswith("# "):
        return None
    names = [name.strip() for name in re.split(r"\s{2,}", line[2:].strip()) if name.strip()]
    if not names or names[0] not in PERCENT_COLUMNS or not all(n in KNOWN_COLUMNS for n in names):
        return None
    return names


def row_pattern(names):
    parts = []
    for position, name in enumerate(names):
        gap = r"\s*" if position == 0 else r"\s+"
        key = re.sub(r"\W", "_", name)
        if name in PERCENT_COLUMNS:
            parts.append(gap + rf"(?P<{key}>\d+\.\d+)%")
        elif name in INTEGER_COLUMNS:
            parts.append(gap + rf"(?P<{key}>\d+)")
        elif name in WORD_COLUMNS:
            parts.append(gap + rf"(?P<{key}>\S+)")
        elif name == "Symbol":
            parts.append(gap + r"\[(?P<level>.)\]\s+(?P<symbol>.*?)")
        else:
            parts.append(r"(?:\s+(?P<key>.*?))?")
    return re.compile("^" + "".join(parts) + r"(?:\s+-)*\s*$")


def parse_report(text, name):
    """perf report --stdio's header facts and its entry lines, call-graph lines skipped."""
    report = {"name": name, "event": None, "samples_header": None, "event_count": None,
              "lost": None, "columns": None, "rows": []}
    pattern = None
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("#"):
            match = SAMPLES_LINE.match(line)
            if match:
                report["samples_header"], report["event"] = match.group(1), match.group(2)
            match = re.match(r"# Event count \(approx\.\): (\d+)", line)
            if match:
                report["event_count"] = int(match.group(1))
            match = re.match(r"# Total Lost Samples: (\d+)", line)
            if match:
                report["lost"] = int(match.group(1))
            names = column_names(line)
            if names:
                report["columns"] = names
                pattern = row_pattern(names)
            continue
        if not DATA_LINE.match(line):
            continue
        if pattern is None:
            raise Refused(f"{name}: an entry at line {number} before perf's column header")
        match = pattern.match(line)
        if not match:
            raise Refused(f"{name}: line {number} is not an entry of its columns: {line[:80]!r}")
        values = match.groupdict()
        row = {"key": (values.get("symbol") or values.get("key") or "").strip(),
               "level": values.get("level"), "dso": values.get("Shared_Object")}
        for column in PERCENT_COLUMNS:
            if values.get(column) is not None:
                row[column] = float(values[column])
        for column in INTEGER_COLUMNS:
            if values.get(column) is not None:
                row[column] = int(values[column])
        report["rows"].append(row)
    if report["columns"] is None:
        raise Refused(f"{name}: no column header, so not a perf report --stdio")
    return report


def self_column(report):
    return "Self" if "Self" in report["columns"] else "Overhead"


def self_shares(report):
    """Each entry's share of the report's self samples, computed from its period, or from its
    printed percentage where the report has no period column. Refuses a report with no
    samples, one whose percentages do not sum to 100 within their rounding, whose periods do
    not add up to its event count, or whose shares are not its periods'."""
    name = report["name"]
    rows = report["rows"]
    column = self_column(report)
    if not rows or report["samples_header"] == "0":
        raise Refused(f"{name}: the report has no samples")
    if "Samples" in report["columns"] and sum(row["Samples"] for row in rows) == 0:
        raise Refused(f"{name}: the report has no samples")
    printed = sum(row[column] for row in rows)
    allowed = HALF_UNIT * len(rows) + 1e-9
    if abs(printed - 100.0) > allowed:
        raise Refused(f"{name}: its self shares sum to {printed:.2f}%, not 100 within the "
                      f"{allowed:.3f} its {len(rows)} rounded lines allow, so lines are missing")
    if "Period" in report["columns"]:
        weights = [row["Period"] for row in rows]
        total = sum(weights)
        if total == 0:
            raise Refused(f"{name}: the report has no samples")
        if report["event_count"] is not None and total != report["event_count"]:
            raise Refused(f"{name}: its periods add up to {total}, not the event count "
                          f"{report['event_count']}, so lines are missing")
    else:
        weights = [row[column] for row in rows]
        total = sum(weights)
        if total == 0:
            raise Refused(f"{name}: the report has no samples")
    shares = [100.0 * weight / total for weight in weights]
    for row, share in zip(rows, shares):
        if abs(share - row[column]) > HALF_UNIT + 1e-6 and "Period" in report["columns"]:
            raise Refused(f"{name}: {row['key'][:60]!r} prints {row[column]:.2f}% but its "
                          f"period is {share:.4f}% of the total")
    return shares


def compile_groups(kind):
    return [(group, about, [(how, re.compile(pattern)) for how, pattern in patterns],
             [pattern for _how, pattern in patterns])
            for group, about, patterns in GROUPS[kind]]


def group_of(row, groups):
    name = function_name(row["key"])
    for group, _about, matchers, _patterns in groups:
        for how, pattern in matchers:
            if how == "level" and row.get("level") == pattern.pattern:
                return group
            if how == "dso" and row.get("dso") and pattern.match(row["dso"]):
                return group
            if how == "name" and pattern.match(name):
                return group
    return None


def grouped(kind, report):
    """The self shares by group, in the table's order, then the unmatched share as its own
    entry with its largest symbols. The shares sum to 100."""
    shares = self_shares(report)
    groups = compile_groups(kind)
    totals = {group: 0.0 for group, *_rest in groups}
    unmatched = []
    for row, share in zip(report["rows"], shares):
        group = group_of(row, groups)
        if group is None:
            unmatched.append((share, row["key"]))
        else:
            totals[group] += share
    rest = sum(share for share, _key in unmatched)
    if abs(sum(totals.values()) + rest - 100.0) > 1e-6:
        raise Refused(f"{report['name']}: the groups' shares do not add up to 100")
    out = [{"name": group, "self_share_percent": round(totals[group], 3)}
           for group, *_rest in groups]
    unmatched.sort(key=lambda item: -item[0])
    out.append({"name": "unmatched", "self_share_percent": round(float(rest), 3),
                "largest": [{"symbol": key[:SYMBOL_CHARS], "self_share_percent": round(share, 3)}
                            for share, key in unmatched[:TOP_UNMATCHED]]})
    return out


def group_table(kind):
    """The groups as the summary states them once: each one's meaning and its patterns."""
    table = [{"name": group, "what": about, "patterns": [f"{how}: {pattern}"
                                                          for how, pattern in patterns]}
             for group, about, patterns in GROUPS[kind]]
    table.append({"name": "unmatched", "what": "symbols no pattern above matches, kept apart "
                  "and never spread over the groups", "patterns": []})
    return {"rule": ("a symbol goes to the first group with a pattern that matches it: name "
                     "patterns are matched from the start of the function's qualified name, its "
                     "return type and parameters dropped; dso against the shared object; level "
                     "against perf's [k] or [.] mark"),
            "groups": table}


def inclusive(kind, report, limit):
    """Each named function's inclusive share from the --children report, or null where it is
    under the report's limit."""
    out = {}
    for label, pattern in INCLUSIVE[kind]:
        compiled = re.compile(pattern)
        found = [row for row in report["rows"]
                 if "Children" in row and compiled.match(function_name(row["key"]))
                 and not local_to_function(row["key"])]
        if len(found) > 1:
            # One function may call another, so their inclusive shares do not add up.
            raise Refused(f"{report['name']}: {len(found)} entries match the inclusive share "
                          f"{label!r}: " + "; ".join(row["key"][:60] for row in found))
        out[label] = round(found[0]["Children"], 3) if found else None
    return {"limit_percent": limit, "note": "null: under the report's limit, or not called",
            "children_share_percent": out}


def source_files(report):
    """Self shares by source file, from a --sort srcfile report, file names only."""
    shares = self_shares(report)
    totals = {}
    for row, share in zip(report["rows"], shares):
        key = row["key"].rsplit("/", 1)[-1] or "(no line table)"
        totals[key] = totals.get(key, 0.0) + share
    return [{"file": key, "self_share_percent": round(value, 3)}
            for key, value in sorted(totals.items(), key=lambda item: -item[1])]


# --- Reading the step's other files ---------------------------------------------------------

def read_facts(folder):
    path = folder / "facts.txt"
    if not path.is_file():
        raise Refused(f"no facts.txt in {folder.name}, so not a diagnostic step's folder")
    facts = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition(": ")
        if key:
            facts.setdefault(key, []).append(value)
    return facts


def fact(facts, key, required=True):
    values = facts.get(key)
    if not values:
        if required:
            raise Refused(f"facts.txt has no {key}")
        return None
    return values[-1]


def strip_paths(text):
    """A command as recorded on the host, each path reduced to its last part."""
    return PATH_IN_COMMAND.sub(lambda match: match.group(0).rstrip("/").rsplit("/", 1)[-1],
                               text)


def recorded_samples(folder, stem):
    path = folder / f"{stem}.err"
    if not path.is_file():
        return None
    found = RECORDED.findall(path.read_text(encoding="utf-8", errors="replace"))
    return int(found[-1]) if found else None


def last_try(text):
    """What a command wrote in its last try, after the runner's '=== try N' lines."""
    parts = re.split(r"^=== try \d+ at \S+\n", text, flags=re.M)
    return parts[-1]


def json_lines(path):
    lines = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            lines.append(json.loads(line))
    return lines


def report_file(folder, name, required=True):
    path = folder / name
    if not path.is_file():
        if required:
            raise Refused(f"no {name}")
        return None
    return parse_report(path.read_text(encoding="utf-8", errors="replace"), name)


def limit_of(facts, stem):
    for command in facts.get("report_command", []):
        if f">{stem}" in command:
            match = re.search(r"--percent-limit (\S+)", command)
            if match:
                return float(match.group(1))
    return None


def build_block(facts):
    preset = strip_paths(fact(facts, "build_preset"))
    if preset != PRESET:
        raise Refused(f"the build is not the {PRESET} preset: build_preset.py said {preset!r}")
    identical = {"yes": True, "no": False}.get(fact(facts, "text_identical"))
    return {"preset": preset, "used": fact(facts, "build"),
            "extra_flags": fact(facts, "extra_flags"), "text_identical": identical,
            "note": strip_paths(fact(facts, "build_note"))}


def require_counts(report):
    """A report the summary takes shares from: its event named, each line's samples and period
    given, and its event count, as the runner's -n --show-total-period reports have them."""
    name = report["name"]
    if report["event"] is None:
        raise Refused(f"{name}: no '# Samples: ... of event' line, so the event it counts is "
                      "not known")
    missing = [column for column in INTEGER_COLUMNS if column not in report["columns"]]
    if missing:
        raise Refused(f"{name}: no {' or '.join(missing)} column, so its shares cannot be tied "
                      "to the recording: make it with -n --show-total-period")
    if report["event_count"] is None:
        raise Refused(f"{name}: no '# Event count' line, so its periods cannot be checked")


def same_recording(main, other):
    """A children or source-file report must be of the self report's recording."""
    if other["event"] is None or other["event_count"] is None:
        raise Refused(f"{other['name']}: no sample or event count line, so it is not shown to "
                      f"be of {main['name']}'s recording")
    for key, what in (("event", "event"), ("samples_header", "sample count"),
                      ("event_count", "event count")):
        if other[key] != main[key]:
            raise Refused(f"{other['name']}: its {what} is {other[key]}, and "
                          f"{main['name']}'s is {main[key]}, so it is another recording's")


def profile_block(facts, folder, kind, stem, record_stem):
    """A profile's shares and its tie to its recording. Returns the block and what of the tie
    the raw output does not establish, as (what, why) pairs."""
    self_report = report_file(folder, f"{stem}-self.txt")
    require_counts(self_report)
    if self_report["event"] != fact(facts, "event"):
        raise Refused(f"{stem}-self.txt counts {self_report['event']}, not "
                      f"{fact(facts, 'event')}")
    block = {
        "samples_recorded": recorded_samples(folder, record_stem),
        "samples_in_report": sum(row["Samples"] for row in self_report["rows"]),
        "samples_header": self_report["samples_header"],
        "lost_samples": self_report["lost"],
        "groups": grouped(kind, self_report),
    }
    missing = []
    recorded = block["samples_recorded"]
    if recorded == 0:
        raise Refused(f"{record_stem}.err says perf recorded no samples")
    if recorded is None:
        missing.append(("the recording's sample count", f"{record_stem}.err holds no perf "
                        "record line with its sample count, so the report is not shown to "
                        "hold the whole recording"))
    elif recorded != block["samples_in_report"]:
        raise Refused(f"{stem}-self.txt holds {block['samples_in_report']} samples, and "
                      f"{record_stem}.err says perf recorded {recorded}, so it is not that "
                      "recording's whole report")
    lost = block["lost_samples"]
    if lost is None:
        missing.append(("the samples lost", f"{stem}-self.txt has no '# Total Lost Samples' "
                        "line, so the samples perf dropped are not known"))
    elif lost > LOST_AT_MOST * (block["samples_in_report"] + lost):
        raise Refused(f"{stem}-self.txt: perf lost {lost} samples beside the "
                      f"{block['samples_in_report']} it kept, more than "
                      f"{LOST_AT_MOST:.1%} of them, so the shares are not the run's")
    block["lost_samples_at_most"] = f"{LOST_AT_MOST:.1%} of the samples kept and lost"
    children = report_file(folder, f"{stem}-children.txt", required=False)
    if children:
        same_recording(self_report, children)
    block["inclusive"] = (inclusive(kind, children, limit_of(facts, f"{stem}-children.txt"))
                          if children else None)
    srcfile = report_file(folder, f"{stem}-srcfile.txt", required=False)
    if srcfile:
        require_counts(srcfile)
        same_recording(self_report, srcfile)
    block["source_files"] = source_files(srcfile) if srcfile else None
    return block, missing


def not_established(path, missing):
    """Entries of a summary's not_established list, each with the path it covers."""
    return [{"path": path, "what": what, "why": why} for what, why in missing]


def common(facts, folder, commit, kind):
    if fact(facts, "kind") != kind:
        raise Refused(f"facts.txt says {fact(facts, 'kind')}, not {kind}")
    if fact(facts, "commit") != commit:
        raise Refused(f"the run is at {fact(facts, 'commit')}, not the commit given, {commit}")
    return {"schema": SCHEMA, "kind": kind, "day": DAY, "commit": commit,
            "host": fact(facts, "host"), "measurement": "diagnostic",
            "statement": STATEMENTS[kind].format(commit=commit[:12]), "runs": 1,
            "input": fact(facts, "input"), "build": build_block(facts),
            "wrapper": fact(facts, "wrapper"), "not_established": []}


def profile_header(facts):
    return {"event": fact(facts, "event"), "frequency_hz": int(fact(facts, "frequency_hz")),
            "call_graph": fact(facts, "call_graph")}


def commands(facts, *keys):
    return [strip_paths(value) for key in keys for value in facts.get(key, [])]


# --- The four kinds --------------------------------------------------------------------------

def layout_block(folder):
    layout = json.loads((folder / "layout.json").read_text(encoding="utf-8"))
    values = {column["column"]: column["values"] for column in layout["columns"]}
    with gzip.open(folder / "layout-blocks.json.gz", "rt", encoding="utf-8") as handle:
        blocks = json.load(handle)["block_list"]
    by_column = {}
    codecs = {}
    for block in blocks:
        by_column.setdefault(block["column"], set()).add(block["codec"])
        entry = codecs.setdefault(block["codec"], {"pieces": 0, "payload_bytes": 0,
                                                   "values_in_single_codec_columns": 0})
        entry["pieces"] += 1
        entry["payload_bytes"] += block["payload_bytes"]
    mixed = sorted(column for column, used in by_column.items() if len(used) > 1)
    for column, used in by_column.items():
        if len(used) == 1:
            codecs[next(iter(used))]["values_in_single_codec_columns"] += values.get(column, 0)
    return {"segments": layout["segments"], "segment_records": layout["segment_records"],
            "records": layout["records"], "codecs": dict(sorted(codecs.items())),
            "columns_with_several_codecs": mixed,
            "note": ("values are counted only for columns whose every piece has one codec; a "
                     "column with several is listed instead")}


def writer_profile(facts, folder, commit):
    out = common(facts, folder, commit, "writer-profile")
    out.update(profile_header(facts))
    ingest = json.loads((folder / "ingest.json").read_text(encoding="utf-8"))
    out["records"] = ingest["records"]
    block, missing = profile_block(facts, folder, "writer-profile", "writer", "perf-record")
    out.update(block)
    out["not_established"] += not_established([], missing)
    out["group_table"] = group_table("writer-profile")
    out["store_layout"] = layout_block(folder)
    out["commands"] = commands(facts, "record_command", "report_command")
    return out


def answers_of(path, name):
    lines = json_lines(path)
    if not lines or not lines[0].get("ready"):
        raise Refused(f"{name}: the query tool's first line is not its ready line")
    answers = lines[1:]
    for answer in answers:
        if "error" in answer:
            raise Refused(f"{name}: an answer is an error: {answer['error']}")
    return lines[0], answers


STORE_KEYS = ("records", "segments", "store_version", "segment_records")


def same_store(store, ready, name, keys=STORE_KEYS):
    """Every query run opens the one store: its ready line's store facts are the first's."""
    facts = {key: ready.get(key) for key in keys}
    if store is not None and facts != store:
        raise Refused(f"{name}: the tool opened a store of {facts}, and the first run's is "
                      f"{store}")
    return facts


def constant(answers, key):
    values = [json.dumps(answer.get(key), sort_keys=True) for answer in answers]
    return json.loads(values[0]) if len(set(values)) == 1 else None


def query_profile(facts, folder, commit):
    out = common(facts, folder, commit, "query-profile")
    out.update(profile_header(facts))
    out["query_parameters_from"] = strip_paths(fact(facts, "query_parameters"))
    out["parameters"] = json.loads((folder / "parameters.json").read_text(encoding="utf-8"))
    queries = []
    store = None
    for name in QUERIES:
        ready, answers = answers_of(folder / f"{name}.out", f"{name}.out")
        requested = len(json_lines(folder / f"{name}.jsonl"))
        if len(answers) != requested:
            raise Refused(f"{name}: {len(answers)} answers to {requested} requests")
        store = same_store(store, ready, f"{name}.out", STORE_KEYS + ("kernel_level",))
        entry = {"name": name, "repetitions": len(answers),
                 "values_decoded": constant(answers, "values_decoded"),
                 "segments": constant(answers, "segments"),
                 "kernel_runs": constant(answers, "kernel_runs")}
        block, missing = profile_block(facts, folder, "query-profile", name,
                                       f"{name}-perf-record")
        entry.update(block)
        out["not_established"] += not_established(["queries", {"name": name}], missing)
        shares = (entry["inclusive"] or {}).get("children_share_percent", {})
        if shares.get("engine") is not None and shares.get("digest") is not None:
            entry["engine_share_percent"] = shares["engine"]
            entry["digest_share_percent"] = shares["digest"]
            entry["store_open_share_percent"] = shares.get("store_open")
        queries.append(entry)
    out["store"] = store
    out["queries"] = queries
    out["group_table"] = group_table("query-profile")
    out["commands"] = commands(facts, "record_command", "report_command")
    return out


def perf_stat(path):
    """perf stat -x, output: {event: {"value", "unit", "coverage_percent"}}, or a status."""
    counters = {}
    for line in last_try(path.read_text(encoding="utf-8")).splitlines():
        fields = line.split(",")
        if line.startswith("#") or len(fields) < 5:
            continue
        event = fields[2]
        try:
            value = float(fields[0])
        except ValueError:
            counters[event] = {"status": "unavailable", "reason": f"perf said {fields[0]}"}
            continue
        counters[event] = {"value": value, "unit": fields[1],
                           "coverage_percent": float(fields[4] or 0.0)}
    return counters


TIME_UNITS = {"ns": 1e-9, "us": 1e-6, "msec": 1e-3, "ms": 1e-3, "s": 1.0, "sec": 1.0}


def seconds_of(counter):
    if not counter or "value" not in counter or counter["unit"] not in TIME_UNITS:
        return None
    return counter["value"] * TIME_UNITS[counter["unit"]]


def counter_block(counters):
    out = {}
    for event, key in (("cycles:u", "cycles"), ("instructions:u", "instructions"),
                       ("branches:u", "branches"), ("branch-misses:u", "branch_misses")):
        counter = counters.get(event)
        if counter is None:
            raise Refused(f"perf stat did not report {event}")
        if "value" not in counter:
            raise Refused(f"perf stat did not count {event}: {counter['reason']}")
        if counter["coverage_percent"] < 100.0:
            raise Refused(f"perf stat counted {event} {counter['coverage_percent']:.2f}% of the "
                          "time, multiplexed, so its total is perf's estimate")
        out[key] = int(counter["value"])
        out[f"{key}_coverage_percent"] = counter["coverage_percent"]
    task = seconds_of(counters.get("task-clock"))
    user = seconds_of(counters.get("user_time"))
    system = seconds_of(counters.get("system_time"))
    if user is not None and user > 0:
        out["effective_mhz"] = round(out["cycles"] / user / 1e6, 1)
        out["effective_mhz_over"] = "user time (perf's user_time)"
    elif task:
        out["effective_mhz"] = round(out["cycles"] / task / 1e6, 1)
        out["effective_mhz_over"] = "task-clock, user and system time, so a lower bound"
    if user is not None and system is not None and user + system > 0:
        out["system_share_of_cpu_time_percent"] = round(100.0 * system / (user + system), 3)
    out["ipc"] = round(out["instructions"] / out["cycles"], 4) if out["cycles"] else None
    out["branch_miss_rate_percent"] = (round(100.0 * out["branch_misses"] / out["branches"], 4)
                                       if out["branches"] else None)
    return out


def sibling_of(folder):
    cpus = run_bench.parse_cpu_list((folder / "topology.txt").read_text(encoding="ascii"))
    others = [cpu for cpu in cpus if cpu != CORE]
    if len(others) != 1:
        raise Refused(f"topology.txt names {len(others)} siblings of cpu{CORE}, not 1")
    return others[0]


def busy(folder, before, after, sibling):
    first = (folder / before).read_text(encoding="ascii")
    second = (folder / after).read_text(encoding="ascii")
    busy_before, total_before = run_bench.cpu_jiffies(first, sibling)
    busy_after, total_after = run_bench.cpu_jiffies(second, sibling)
    if total_after <= total_before:
        raise Refused(f"no jiffies passed on cpu{sibling} between {before} and {after}")
    return round(100.0 * (busy_after - busy_before) / (total_after - total_before), 3)


def answer_identity(answer):
    """What an answer says, apart from how long it took: its kind, rows and digest, or None
    where it gives no rows or digest."""
    if answer.get("rows") is None or not answer.get("sha256"):
        return None
    return (answer.get("name"), answer.get("kind"), answer["rows"], answer["sha256"])


def counters_simd(facts, folder, commit):
    out = common(facts, folder, commit, "counters-simd")
    out["events"] = fact(facts, "events")
    out["tool_events"] = fact(facts, "tool_events")
    out["query_parameters_from"] = strip_paths(fact(facts, "query_parameters"))
    out["parameters"] = json.loads((folder / "parameters.json").read_text(encoding="utf-8"))
    out["repetitions"] = fact(facts, "repetitions")
    out["order"] = fact(facts, "order")
    sibling = sibling_of(folder)
    lines = []
    for name in ("write-hold.err", "queries-hold.err"):
        lines += run_bench.clean_load_lines((folder / name).read_text(encoding="utf-8",
                                                                       errors="replace"))
    out["load_lines"] = lines
    governors = sorted({match[1] for line in lines for match in
                        [run_bench.GOVERNOR.search(line)] if match})
    out["governor"] = ",".join(governors) or "unavailable"
    out["turbo"] = run_bench.turbo_across(
        (folder / "boost-before.txt").read_text(encoding="utf-8").strip() or None,
        (folder / "boost-after.txt").read_text(encoding="utf-8").strip())
    holds = {"write": busy(folder, "write-stat-before.txt", "write-stat-after.txt", sibling),
             "queries": busy(folder, "queries-stat-before.txt", "queries-stat-after.txt",
                             sibling)}
    write_tool = json.loads(last_try((folder / "write.json").read_text(encoding="utf-8")))
    write = {"records": write_tool["records"]}
    write.update(counter_block(perf_stat(folder / "write.perf")))
    write["per_message"] = {key: round(write[key] / write["records"], 4) for key in
                            ("cycles", "instructions", "branches", "branch_misses")}
    out["write"] = write
    runs = []
    pooled = {}
    store = None
    # Per query: the answers of its first file, and why its ratio is not established.
    first_answers = {}
    unshown = {name: [] for name in QUERIES}
    kernels = {}
    for round_number in (1, 2):
        for level in LEVELS:
            for name in QUERIES:
                stem = f"{name}-{level}-{round_number}"
                ready, answers = answers_of(folder / f"{stem}.out", f"{stem}.out")
                if ready.get("kernel_level") != level:
                    raise Refused(f"{stem}: asked for {level}, the tool ran {ready.get('kernel_level')}")
                store = same_store(store, ready, f"{stem}.out")
                requested = len(json_lines(folder / f"{name}.jsonl"))
                if len(answers) != requested or requested < 2:
                    raise Refused(f"{stem}: {len(answers)} answers to {requested} requests")
                identities = [answer_identity(answer) for answer in answers]
                if any(identity is None for identity in identities):
                    unshown[name].append(("the levels' answers alike", f"{stem}: an answer "
                                          "gives no rows or sha256, so AVX2 and scalar are not "
                                          "shown to answer alike"))
                elif name not in first_answers:
                    first_answers[name] = (stem, identities)
                elif identities != first_answers[name][1]:
                    at = next(index for index, (one, two) in enumerate(
                        zip(identities, first_answers[name][1])) if one != two)
                    raise Refused(f"{stem}: answer {at + 1} is {identities[at]}, and "
                                  f"{first_answers[name][0]}'s is {first_answers[name][1][at]}: "
                                  "the levels or the rounds answer differently")
                timed = answers[1:]
                if any(not isinstance(answer.get("kernel_runs"), dict) for answer in timed):
                    unshown[name].append(("the kernels each level ran", f"{stem}: an answer "
                                          "gives no kernel_runs, so its cap is not shown to "
                                          "hold"))
                for answer in timed:
                    other = {lv: n for lv, n in (answer.get("kernel_runs") or {}).items()
                             if lv in ABOVE[level] and n}
                    if other:
                        raise Refused(f"{stem}: capped to {level}, an answer ran {other}")
                kernels[(name, level, round_number)] = (
                    sum((answer.get("kernel_runs") or {}).get(level, 0) for answer in timed),
                    sum(sum((answer.get("kernel_runs") or {}).values()) for answer in timed))
                engine = [answer["engine_ns"] for answer in timed]
                pooled.setdefault((name, level), []).extend(engine)
                values = constant(timed, "values_decoded")
                process = counter_block(perf_stat(folder / f"{stem}.perf"))
                if values:
                    process["cycles_per_value_decoded"] = round(
                        process["cycles"] / (values * len(answers)), 4)
                runs.append({"name": name, "level": level, "round": round_number,
                             "timed_repetitions": len(timed),
                             "engine_ms_median": round(statistics.median(engine) / 1e6, 4),
                             "engine_ms_slowest": round(max(engine) / 1e6, 4),
                             "values_decoded": values,
                             "kernel_runs": constant(timed, "kernel_runs"),
                             "process_counters": process,
                             "sibling_busy_percent": busy(folder, f"{stem}.stat0",
                                                          f"{stem}.stat1", sibling)})
    out["queries"] = runs
    out["store"] = store
    for name in QUERIES:
        for round_number in (1, 2):
            avx2, _ = kernels[(name, "avx2", round_number)]
            _, scalar = kernels[(name, "scalar", round_number)]
            if avx2 == 0 and scalar > 0:
                raise Refused(f"{name}-avx2-{round_number}: no answer ran an AVX2 kernel, and "
                              f"{name}-scalar-{round_number}'s ran {scalar}, so the AVX2 run "
                              "measured scalar code")
            if avx2 == 0 and scalar == 0 and not unshown[name]:
                unshown[name].append(("a SIMD change", f"{name}: neither level ran a kernel in "
                                      f"round {round_number}, so the two run the same code"))
    out["simd_change"] = []
    for name in QUERIES:
        ratio = round(statistics.median(pooled[(name, "avx2")])
                      / statistics.median(pooled[(name, "scalar")]), 4)
        out["simd_change"].append({
            "name": name,
            "avx2_engine_ms_median": round(statistics.median(pooled[(name, "avx2")]) / 1e6, 4),
            "scalar_engine_ms_median": round(statistics.median(pooled[(name, "scalar")]) / 1e6,
                                             4),
            "avx2_over_scalar_median": None if unshown[name] else ratio})
        out["not_established"] += not_established(["simd_change", {"name": name}],
                                                  unshown[name])
    out["sibling_busy_percent"] = {
        "core": CORE, "sibling": sibling, "method": run_bench.SIBLING_METHOD,
        "holds": holds,
        "per_command": "each query run's own share is in queries[].sibling_busy_percent"}
    out["commands"] = commands(facts, "write_command", "queries_command")
    out["note"] = ("process_counters cover the whole query process: the store's open, every "
                   "request and every digest. The first request of each file is untimed; the "
                   "engine times are the tool's engine_ns over the others.")
    return out


def price_decode_profile(facts, folder, commit):
    out = common(facts, folder, commit, "price-decode-profile")
    out.update(profile_header(facts))
    bench = json.loads((folder / "report.json").read_text(encoding="utf-8"))
    built = bench["build"]["commit"]
    if not built or not (commit.startswith(built) or built.startswith(commit)):
        raise Refused(f"column_decode_bench was built at {built}, not {commit}")
    out["case"] = bench["case"]
    out["isa"] = bench["build"]["isa"]
    out["values"] = bench["verification"]["values"]
    block, missing = profile_block(facts, folder, "price-decode-profile", "price-decode",
                                   "perf-record")
    out.update(block)
    out["not_established"] += not_established([], missing)
    out["group_table"] = group_table("price-decode-profile")
    out["commands"] = commands(facts, "record_command", "report_command")
    return out


BUILDERS = {"writer-profile": writer_profile, "query-profile": query_profile,
            "counters-simd": counters_simd, "price-decode-profile": price_decode_profile}


# --- Output ----------------------------------------------------------------------------------

def refuse_leaks(text, forbidden):
    """No absolute path, no name of this machine or its user, nor a --forbid word."""
    problems = [f"the absolute path {path}" for path in ABSOLUTE.findall(text)]
    names = {socket.gethostname().strip().lower(), getpass.getuser().strip().lower()}
    names |= {name.split(".")[0] for name in names}
    names |= {word.lower() for word in forbidden}
    lowered = text.lower()
    problems += [f"the name {name!r}" for name in sorted(names)
                 if name and len(name) > 2 and name in lowered]
    if problems:
        raise Refused("the summary would hold " + "; ".join(problems))


def summarise(folder, commit, iteration=False):
    facts = read_facts(folder)
    kind = fact(facts, "kind")
    if kind not in BUILDERS:
        raise Refused(f"facts.txt names the kind {kind!r}, none of {', '.join(KINDS)}")
    mode = fact(facts, "mode")
    if mode != "real" and not iteration:
        raise Refused(f"a {mode} run is iteration only: give --iteration and an --out outside "
                      "results/")
    out = BUILDERS[kind](facts, folder, commit)
    if mode != "real":
        out["measurement"] = f"{mode}, iteration only: nothing here is a result"
    return kind, out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("folder", type=Path, help="one diagnostic step's staging/diag/<kind>/")
    parser.add_argument("--commit", required=True, help="the freeze commit, 40 hex digits")
    parser.add_argument("--out", type=Path,
                        help="the file to write; results/diag/<kind>-12302019-<commit12>.json")
    parser.add_argument("--forbid", action="append", default=[],
                        help="a word the output must not hold, such as the host's name")
    parser.add_argument("--iteration", action="store_true",
                        help="take a smoke's folder, never written under results/")
    args = parser.parse_args(argv)
    try:
        if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
            raise Refused(f"--commit {args.commit!r} is not 40 hex digits")
        kind, document = summarise(args.folder, args.commit, args.iteration)
        out = args.out or RESULTS / f"{kind}-{DAY}-{args.commit[:12]}.json"
        if args.iteration and RESULTS_ROOT.resolve() in out.resolve().parents:
            raise Refused("an iteration summary is never written under results/")
        text = json.dumps(document, indent=1) + "\n"
        refuse_leaks(text, args.forbid)
    except (Refused, OSError, ValueError, KeyError, TypeError, run_bench.Failure) as error:
        print(f"diag_summary: {error}", file=sys.stderr)
        return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    print(f"diag_summary: wrote {out.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
