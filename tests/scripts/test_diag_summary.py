#!/usr/bin/env python3
"""Tests for scripts/diag_summary.py. Run with:

    python3 -I tests/scripts/test_diag_summary.py

The writer's reports in tests/scripts/diag_fixtures/ are the feasibility probe's real
`perf report --stdio` output on the Linux host (the open slice's ingest, bench preset, before
the freeze), cut down: writer-self.txt holds every line of its flat profile at 0.01% or more,
writer-children.txt its call-graph profile's first 24 lines, writer-dso-top.txt its first 29
lines with the shared object. The probe kept the lines and not perf's header, so each file has
perf's header put back in perf's own form (the sample counts are the probe's). The probe's flat
and call-graph profiles came from two recordings, and its flat profile has no sample or period
column, which a summary now needs to tie its shares to the recording: so the writer's folder
holds the probe's lines with samples and periods made in proportion to its percentages, its
percentages computed again from them, and its children report given the same recording's
header. The other kinds' inputs, which no probe made, are written here in the form the tools
print: perf report with its sample and period columns, perf stat -x, lines, tools/tick_query's
ready and answer lines, /proc/stat lines and measure.sh's load lines. Each test writes its
folder under the repo's gitignored build/ directory, and a summary written by a test that
fails goes to that folder's own results/, never the repo's.
"""

import gzip
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "tests" / "scripts" / "diag_fixtures"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_diag_summary"

sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_hygiene  # noqa: E402
import diag_summary  # noqa: E402

COMMIT = "ede0176b10bb49e505226285530d0b91a33b4f34"
OTHER = "fb19364" + "0" * 33
HOME = "/home/someone/quant-projects/tick-store/freeze"
RECORDED = ("[ perf record: Woken up 4 times to write data ]\n"
            f"[ perf record: Captured and wrote 0.905 MB {HOME}/run/perf-data/writer.data "
            "(23652 samples) ]\n")
# The probe's ingest of the open slice, as tools/tick_roundtrip printed it.
INGEST = {"tool": "tick_roundtrip", "command": "ingest", "store_format": "itch-store-v3",
          "durability": "page_cache", "segment_records": 4194304, "records": 6125294,
          "bytes": 206138339, "tail_bytes": 0, "raw_unknown_type": 0, "raw_wrong_length": 0,
          "stored_file_bytes": 43266003, "ratio": 4.764441, "flushes": [],
          "wall_seconds": 8.532048, "peak_rss_bytes": 1112043520}

# A real or code-named symbol for every pattern of every group, in the table's order: (symbol,
# shared object, level). Symbols cut short are the probe's, as it kept them.
K = ("0xffffffffafa01280", None, "k")
EXAMPLES = {
    "writer-profile": {
        "kernel_addresses": [K],
        "parse": [("tick_store::itch::decode(std::__1::span<std::byte const, 18446744073709551615ul>) - -", None, ".")],
        "book_state": [
            ("tick_store::store::SegmentBuilder::State::header(std::byte, tick_store::itch::MessageHeader const&) - -", None, "."),
            ("tick_store::store::(anonymous namespace)::RefMap::assign(unsigned long, unsigned short, unsigned int) - -", None, "."),
            ("tick_store::store::SymbolTable::learn(unsigned short, std::__1::array<std::byte, 8ul> const&) - -", None, "."),
            ("tick_store::store::SegmentBuilder::add(std::__1::variant<tick_store::itch::SystemEvent, tick_store::itch::StockDirectory, tick_store::itch::StockTrad", None, "."),
            ("tick_store::store::ItchStoreWriter::append(std::__1::variant<tick_store::itch::SystemEvent, tick_store::itch::StockDirectory, tick_store::itch::Stock", None, "."),
            ("tick_store::store::column_form_plain(std::__1::variant<tick_store::itch::SystemEvent, tick_store::itch::StockDirectory, tick_store::itch::StockTradin", None, "."),
            ("tick_store::store::SegmentBuilder::records() const", None, "."),
            ("decltype(auto) std::__1::__variant_detail::__visitation::__base::__dispatcher<10ul>::__dispatch[abi:nqe230104]<std::__1::__variant_detail::__visitati", None, "."),
        ],
        "segment_assembly": [
            ("tick_store::store::SegmentBuilder::finish(unsigned long, tick_store::store::SegmentSummary&) - -", None, "."),
            ("tick_store::store::compute_seek(std::__1::array<unsigned long, 38ul> const&, std::__1::span<unsigned long const, 18446744073709551615ul>, std::__1::s", None, "."),
        ],
        "sort": [("std::__1::pair<unsigned long*, bool> std::__1::__bitset_partition[abi:nqe230104]<std::__1::_ClassicAlgPolicy, unsigned long*, std::__1::ranges::less>", None, ".")],
        "pfor_encode": [
            ("tick_store::codec::(anonymous namespace)::choose_width(std::__1::span<unsigned char const, 18446744073709551615ul>, bool) - -", None, "."),
            ("tick_store::codec::BitWriter::write(unsigned long, unsigned int) - -", None, "."),
        ],
        "dictionary_encode": [("tick_store::codec::encode_dict_block(std::__1::span<unsigned long const, 18446744073709551615ul>, std::__1::vector<std::byte, std::__1::allocator<std", None, ".")],
        "zstd": [("ZSTD_compressBlock_fast - -", None, "."),
                 ("tick_store::codec::encode_zstd_payload(std::__1::span<std::byte const, 18446744073709551615ul>, int)", None, ".")],
        "crc": [("tick_store::store::detail::crc32c_sse42(std::__1::span<std::byte const, 18446744073709551615ul>, unsigned int)", None, ".")],
        "store_writer": [
            ("tick_store::store::ContainerWriter::~ContainerWriter() - -", None, "."),
            ("tick_store::store::ItchStoreWriter::write_segment() - -", None, "."),
            ("tick_store::store::encode_column(tick_store::store::Column, std::__1::span<unsigned long const, 18446744073709551615ul>) - -", None, "."),
        ],
        "containers": [("std::__1::vector<std::byte, std::__1::allocator<std::byte> >::emplace_back[abi:nqe230104]<std::byte>(std::byte&&)", None, ".")],
        "memory": [("malloc - -", None, ".")],
        "libc": [("0x000000000009eb03", "libc.so.6", ".")],
        "libcxx": [("std::__1::basic_ostream<char, std::__1::char_traits<char> >::flush()", "libc++.so.1.0", ".")],
        "tool": [("tick_store::harness::run_roundtrip_cli(std::__1::span<std::__1::basic_string_view<char, std::__1::char_traits<char> > const, 18446744073709551615ul>,", None, ".")],
        "unresolved": [("0x0000000000163304 - -", None, ".")],
    },
    "query-profile": {
        "kernel_addresses": [K],
        "digest": [
            ("tick_store::harness::(anonymous namespace)::rows_digest(std::__1::vector<tick_store::store::QueryRow, std::__1::allocator<tick_store::store::QueryRow> > const&, long)", None, "."),
            ("tick_store::harness::Sha256::compress(std::__1::span<std::byte const, 64ul>)", None, "."),
        ],
        "dictionary_decode": [("tick_store::codec::decode_dict_block(std::__1::span<std::byte const, 18446744073709551615ul>, std::__1::span<unsigned long, 18446744073709551615ul>)", None, ".")],
        "pfor_decode": [("tick_store::codec::decode_pfor_block(std::__1::span<std::byte const, 18446744073709551615ul>, std::__1::span<unsigned long, 18446744073709551615ul>)", None, ".")],
        "unpack_kernels": [
            ("tick_store::codec::simd::detail::(anonymous namespace)::unpack_vector_avx2(std::__1::span<std::byte const, 18446744073709551615ul>, unsigned int, std::__1::span<unsigned long, 1024ul>)", None, "."),
            ("tick_store::codec::unpack_linear(std::__1::span<std::byte const, 18446744073709551615ul>, unsigned int, std::__1::span<unsigned long, 18446744073709551615ul>)", None, "."),
        ],
        "zstd": [("HUF_decompress4X2_usingDTable_internal_bmi2", None, "."),
                 ("tick_store::codec::decode_zstd_payload(std::__1::span<std::byte const, 18446744073709551615ul>)", None, ".")],
        "run_positions": [("tick_store::store::positions_from_runs(std::__1::span<unsigned long const, 18446744073709551615ul>)", None, ".")],
        "time_bases": [("tick_store::store::time_bases(std::__1::span<unsigned long const, 18446744073709551615ul>, std::__1::span<unsigned long const, 18446744073709551615ul>, unsigned long)", None, ".")],
        "scan_and_rows": [
            ("tick_store::store::ItchQuery::rows(unsigned long, unsigned long, std::__1::optional<unsigned short>)", None, "."),
            ("tick_store::store::(anonymous namespace)::scan_run(tick_store::store::(anonymous namespace)::PlainSegment const&)", None, "."),
        ],
        "column_decode": [
            ("tick_store::codec::decode_column(std::__1::span<std::byte const, 18446744073709551615ul>, unsigned long)", None, "."),
            ("tick_store::store::decode_column(tick_store::store::ItchStoreReader const&, unsigned long, tick_store::store::Column)", None, "."),
        ],
        "store_open": [
            ("tick_store::store::ItchStoreReader::open(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > const&)", None, "."),
            ("tick_store::store::parse_store_header(std::__1::span<std::byte const, 18446744073709551615ul>)", None, "."),
            ("tick_store::store::SymbolTable::parse(std::__1::span<std::byte const, 18446744073709551615ul>, unsigned long)", None, "."),
        ],
        "crc": [("tick_store::store::crc32c(std::__1::span<std::byte const, 18446744073709551615ul>, unsigned int)", None, ".")],
        "answer_io": [
            ("tick_store::harness::run_query_cli(std::__1::span<std::__1::basic_string_view<char, std::__1::char_traits<char> > const, 18446744073709551615ul>, std::__1::basic_istream<char, std::__1::char_traits<char> >&)", None, "."),
            ("std::__1::__formatter::__format_integer[abi:nqe230104]<unsigned long, char*>(unsigned long, char*)", None, "."),
        ],
        "containers": [("std::__1::vector<unsigned long, std::__1::allocator<unsigned long> >::resize[abi:nqe230104](unsigned long)", None, ".")],
        "memory": [("__memmove_avx_unaligned_erms", None, ".")],
        "libc": [("0x00000000000a1b54", "libc.so.6", ".")],
        "libcxx": [("std::__1::locale::locale()", "libc++.so.1.0", ".")],
        "unresolved": [("0x0000000000162a33", None, ".")],
    },
    "price-decode-profile": {
        "kernel_addresses": [K],
        "zstd": [("ZSTD_decompressSequences_bmi2", None, "."),
                 ("tick_store::codec::decode_zstd_payload(std::__1::span<std::byte const, 18446744073709551615ul>)", None, ".")],
        "pfor_block_decode": [("tick_store::codec::decode_pfor_block(std::__1::span<std::byte const, 18446744073709551615ul>, std::__1::span<unsigned long, 18446744073709551615ul>)", None, ".")],
        "unpack_kernels": [
            ("tick_store::codec::simd::detail::(anonymous namespace)::delta_pass_avx2(std::__1::span<unsigned long, 18446744073709551615ul>, unsigned long)", None, "."),
            ("tick_store::codec::unpack_vector(std::__1::span<unsigned long const, 18446744073709551615ul>, unsigned int, std::__1::span<unsigned long, 1024ul>)", None, "."),
        ],
        "codec_column_decode": [("tick_store::codec::(anonymous namespace)::read_block_header(std::__1::span<std::byte const, 18446744073709551615ul>)", None, ".")],
        "store_column_decode": [("tick_store::store::decode_column(tick_store::store::ItchStoreReader const&, unsigned long, tick_store::store::Column)", None, ".")],
        "containers": [("std::__1::vector<unsigned long, std::__1::allocator<unsigned long> >::__append(unsigned long)", None, ".")],
        "memory": [("operator new(unsigned long)", None, ".")],
        "libc": [("0x000000000016330e", "libc.so.6", ".")],
        "libcxx": [("std::__1::__shared_weak_count::__release_weak()", "libc++.so.1.0", ".")],
        "bench_harness": [
            ("benchmark::State::StartKeepRunning()", None, "."),
            ("tick_store::bench_support::read_ticks()", None, "."),
            ("(anonymous namespace)::throughput(benchmark::State&, (anonymous namespace)::Session&)", None, "."),
        ],
        "unresolved": [("0x0000000000162a45", None, ".")],
    },
}


def row(symbol, dso=None, level="."):
    """An entry as parse_report makes it: the probe's trailing ' - -' is not the symbol's."""
    return {"key": diag_summary.re.sub(r"(?:\s+-)+$", "", symbol), "dso": dso, "level": level}


def header(samples="1K", event="cycles:u", event_count=1000000, lost=0):
    """perf report --stdio's header, as perf prints it: a count with no unit is padded."""
    pad = "" if samples[-1:].isalpha() else " "
    return ["# To display the perf.data header info, please use --header/--header-only options.",
            "#", "#", f"# Total Lost Samples: {lost}", "#",
            f"# Samples: {samples}{pad} of event '{event}'",
            f"# Event count (approx.): {event_count}", "#"]


def report_text(rows, columns=("Overhead", "Samples", "Period", "Shared Object", "Symbol"),
                event_count=None, samples="1K", event="cycles:u", lost=0):
    """A perf report --stdio of these rows: (percent, samples, period, dso, symbol)."""
    total = event_count if event_count is not None else sum(r[2] for r in rows)
    lines = header(samples, event, total, lost) + ["# " + "  ".join(columns), "# ........", "#"]
    for percent, count, period, dso, symbol in rows:
        lines.append(f"    {percent:6.2f}%  {count:>12}  {period:>12}  {dso:<17}  [.] {symbol}")
    return "\n".join(lines) + "\n"


def children_text(rows, samples="1K", event="cycles:u", event_count=1000000):
    lines = header(samples, event, event_count) + [
        "# Children      Self       Samples  Shared Object  Symbol", "# ........", "#"]
    for children, own, count, dso, symbol in rows:
        lines.append(f"    {children:6.2f}%  {own:6.2f}%  {count:>12}  {dso:<14}  [.] {symbol}")
    return "\n".join(lines) + "\n"


PROBE_SAMPLES = 23652
PROBE_PERIOD = 2000003


def probe_with_counts():
    """The probe's flat profile with sample and period columns: its samples, the 23,652 perf
    record wrote, shared out in proportion to its printed percentages, each sample's period
    PROBE_PERIOD, and each percentage computed again from the periods, as perf would print it."""
    report = diag_summary.parse_report((FIXTURES / "writer-self.txt").read_text(), "probe")
    weights = [r["Overhead"] for r in report["rows"]]
    exact = [PROBE_SAMPLES * w / sum(weights) for w in weights]
    counts = [int(x) for x in exact]
    for index in sorted(range(len(exact)), key=lambda i: counts[i] - exact[i])[
            :PROBE_SAMPLES - sum(counts)]:
        counts[index] += 1
    total = PROBE_SAMPLES * PROBE_PERIOD
    lines = header("23K", event_count=total) + [
        "# Overhead       Samples        Period  Shared Object         Symbol", "# ........", "#"]
    for r, count in zip(report["rows"], counts):
        lines.append(f"    {100.0 * count / PROBE_SAMPLES:5.2f}%  {count:>12}  "
                     f"{count * PROBE_PERIOD:>12}  {r['dso'] or 'tick_roundtrip':<20}  "
                     f"[{r['level']}] {r['key']}")
    return "\n".join(lines) + "\n", total


def probe_children(event_count):
    """The probe's call-graph lines under the flat profile's recording's header."""
    text = (FIXTURES / "writer-children.txt").read_text()
    text = text.replace("# Samples: 5K of event 'cycles:u'",
                        f"# Samples: 23K of event 'cycles:u'\n# Event count (approx.): "
                        f"{event_count}")
    return text


QUERY_ROWS = [
    (40.00, 400, 400000, "tick_query", "tick_store::codec::decode_dict_block(std::__1::span<std::byte const, 18446744073709551615ul>, std::__1::span<unsigned long, 18446744073709551615ul>)"),
    (25.00, 250, 250000, "tick_query", "tick_store::harness::Sha256::compress(std::__1::span<std::byte const, 64ul>)"),
    (20.00, 200, 200000, "tick_query", "tick_store::codec::simd::detail::(anonymous namespace)::unpack_vector_avx2(std::__1::span<std::byte const, 18446744073709551615ul>, unsigned int, std::__1::span<unsigned long, 1024ul>)"),
    (10.00, 100, 100000, "libc.so.6", "0x00000000000a1b54"),
    (5.00, 50, 50000, "tick_query", "tick_store::store::SomethingNew::run()"),
]
QUERY_CHILDREN = [
    (99.0, 0.0, 0, "tick_query", "main"),
    (60.0, 0.5, 5, "tick_query", "tick_store::store::ItchQuery::rows(unsigned long, unsigned long, std::__1::optional<unsigned short>)"),
    (26.0, 0.5, 5, "tick_query", "tick_store::harness::(anonymous namespace)::rows_digest(std::__1::vector<tick_store::store::QueryRow, std::__1::allocator<tick_store::store::QueryRow> > const&, long)"),
    (9.0, 0.1, 1, "tick_query", "tick_store::store::ItchStoreReader::open(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > const&)"),
]
READY = {"ready": True, "open_ns": 81234567, "segments": 65, "records": 268744780,
         "path": "indexed", "store_version": 3, "segment_records": 4194304,
         "kernel_level": "avx2", "kernel_best": "avx2"}


def answer(name, engine_ns, level="avx2", runs=None):
    kernel_runs = runs if runs is not None else {"scalar": 0, "neon": 0, level: 1200}
    if level == "scalar" and runs is None:
        kernel_runs = {"scalar": 1200, "neon": 0, "avx2": 0}
    return {"name": name, "kind": "rows", "rows": 739944, "sha256": "ab" * 32,
            "engine_ns": engine_ns, "digest_ns": 21000000, "segments": 16, "exact_segments": 0,
            "blocks": 400, "bytes_decoded": 1000000, "values_decoded": 7559833,
            "kernel_runs": kernel_runs}


PERF_STAT = """# started on Sun Nov 15 06:00:00 2026

{cycles},,cycles:u,{run},100.00,,
{instructions},,instructions:u,{run},100.00,3.26,insn per cycle
{branches},,branches:u,{run},100.00,1000.000,M/sec
{misses},,branch-misses:u,{run},100.00,0.42,of all branches
{task:.2f},msec,task-clock,{run},100.00,0.999,CPUs utilized
{user},ns,user_time,{user},100.00,,
{system},ns,system_time,{system},100.00,,
"""


def stat_lines(busy, idle):
    """/proc/stat with cpu5's user and idle jiffies as given."""
    return ("cpu  1000 0 500 90000 10 0 0 0 0 0\n"
            "cpu4 100 0 50 10000 1 0 0 0 0 0\n"
            f"cpu5 {busy} 0 0 {idle} 0 0 0 0 0 0\n")


LOAD = ("measure.sh: start load1=0.41 at=2026-11-15T06:00:00Z host=somehost cpus=4 "
        "governor=performance freq_khz=cpu4:3400000 perf_event_paranoid=2\n"
        "measure.sh: end load1=0.93 exit=0 at=2026-11-15T06:04:00Z host=somehost cpus=4 "
        "governor=performance freq_khz=cpu4:3300000 perf_event_paranoid=2\n")


class Folder:
    """One diagnostic step's staged folder, as the runner writes it."""

    def __init__(self, kind, root):
        self.path = Path(tempfile.mkdtemp(dir=root)) / kind
        self.path.mkdir()
        self.kind = kind
        self.facts = [("step", "L1x"), ("kind", kind), ("mode", "real"), ("commit", COMMIT),
                      ("host", "Linux")]

    def fact(self, key, value):
        self.facts.append((key, value))

    def write(self, name, text):
        (self.path / name).write_text(text, encoding="utf-8")

    def finish(self):
        self.write("facts.txt", "".join(f"{k}: {v}\n" for k, v in self.facts))
        return self.path


def profile_facts(folder, frequency, wrapper="heavy.sh, taskset -c 2,6,7"):
    folder.fact("wrapper", wrapper)
    folder.fact("build", "bench-lines")
    folder.fact("extra_flags", "-gline-tables-only")
    folder.fact("text_identical", "yes")
    folder.fact("build_note", "the bench preset's build with line tables; "
                              "tools/tick_roundtrip's .text is the measured build's")
    folder.fact("build_preset", "bench")
    folder.fact("event", "cycles:u")
    folder.fact("frequency_hz", str(frequency))
    folder.fact("call_graph", "dwarf,16384, user callchains")


def writer_folder(root, self_text=None):
    folder = Folder("writer-profile", root)
    profile_facts(folder, 199)
    folder.fact("input", "the day 12302019")
    folder.fact("record_command", f"perf record --no-buildid-cache -e cycles:u -F 199 -o "
                                  f"{HOME}/run/perf-data/writer.data -- "
                                  f"{HOME}/build/bench-lines/tools/tick_roundtrip ingest "
                                  f"{HOME}/../data/12302019.NASDAQ_ITCH50 {HOME}/run/scratch/diag-day.tsc "
                                  "--durability page_cache")
    for stem, extra in (("writer-self.txt", "--no-children -g none -n --show-total-period"),
                        ("writer-children.txt", "--children -g none -n --percent-limit 0.5")):
        folder.fact("report_command", f"perf report -i {HOME}/run/perf-data/writer.data "
                                      f"--stdio {extra} --sort dso,sym >{stem}")
    counted, total = probe_with_counts()
    folder.write("writer-self.txt", self_text if self_text is not None else counted)
    folder.write("writer-children.txt", probe_children(total))
    folder.write("perf-record.err", RECORDED)
    folder.write("ingest.json", json.dumps(INGEST))
    layout = {"segments": 2, "segment_records": 4194304, "records": 6125294,
              "columns": [{"column": "type", "values": 6125294},
                          {"column": "add_price", "values": 2900000},
                          {"column": "timestamp", "values": 6125294}]}
    folder.write("layout.json", json.dumps(layout))
    blocks = [{"block": 0, "segment": 0, "column": "type", "piece": 0, "codec": "dictionary",
               "payload_offset": 64, "payload_bytes": 1000},
              {"block": 1, "segment": 0, "column": "add_price", "piece": 0, "codec": "pfor",
               "payload_offset": 1064, "payload_bytes": 5000},
              {"block": 2, "segment": 1, "column": "add_price", "piece": 0, "codec": "pfor",
               "payload_offset": 6064, "payload_bytes": 4000},
              {"block": 3, "segment": 0, "column": "timestamp", "piece": 0, "codec": "pfor",
               "payload_offset": 10064, "payload_bytes": 7000},
              {"block": 4, "segment": 1, "column": "timestamp", "piece": 0, "codec": "zstd-pfor",
               "payload_offset": 17064, "payload_bytes": 6000}]
    with gzip.open(folder.path / "layout-blocks.json.gz", "wt", encoding="utf-8") as handle:
        json.dump({"block_list": blocks}, handle)
    return folder


def query_folder(root):
    folder = Folder("query-profile", root)
    profile_facts(folder, 499)
    folder.fact("query_parameters", "Linux-12302019-full-ede0176b10bb.json")
    folder.fact("input", "the store L15 wrote, by bench-lines")
    parameters = []
    for name in diag_summary.QUERIES:
        request = {"name": name, "kind": "rows", "start_ns": 34200000000000,
                   "end_ns": 37800000000000, "locate": 6556}
        parameters.append(request)
        folder.write(f"{name}.jsonl", (json.dumps(request) + "\n") * 3)
        folder.write(f"{name}.out", "\n".join(json.dumps(line) for line in
                                             [READY] + [answer(name, 74000000)] * 3) + "\n")
        folder.write(f"{name}-self.txt", report_text(QUERY_ROWS))
        folder.write(f"{name}-children.txt", children_text(QUERY_CHILDREN))
        folder.write(f"{name}-perf-record.err", RECORDED.replace("23652", "1000"))
        folder.fact("report_command", f"perf report -i {HOME}/run/perf-data/{name}.data --stdio "
                                      f"--children -g none -n --sort dso,sym --percent-limit 1 "
                                      f">{name}-children.txt")
    folder.write("parameters.json", json.dumps(parameters))
    return folder


def counters_folder(root, engines=None, scalar_runs=None):
    folder = Folder("counters-simd", root)
    folder.fact("wrapper", "measure.sh, taskset -c 4")
    folder.fact("build", "bench")
    folder.fact("extra_flags", "none")
    folder.fact("text_identical", "not-applicable")
    folder.fact("build_note", "the measured build, the binaries the comparison ran")
    folder.fact("build_preset", "bench")
    folder.fact("query_parameters", "Linux-12302019-full-ede0176b10bb.json.pending, the day's "
                                    ".pending: its validator refused the day, and its "
                                    "parameters are inputs")
    folder.fact("input", "the day 12302019")
    folder.fact("repetitions", "each query file holds one untimed first request and then the "
                               "timed ones: Q1:4,Q2:4,Q3:4,Q4:4,Q5:4")
    folder.fact("order", "round 1 AVX2 then scalar, round 2 scalar then AVX2, Q1 to Q5 in each")
    folder.fact("tool_events", "user_time and system_time counted")
    folder.fact("events", "cycles:u,instructions:u,branches:u,branch-misses:u,task-clock,"
                          "user_time,system_time")
    folder.fact("write_command", f"{HOME}/../bin/measure.sh taskset -c 4 perf stat -x, -o "
                                 f"{HOME}/run/staging/diag/counters-simd/write.perf -- tick_roundtrip")
    folder.write("topology.txt", "4-5\n")
    folder.write("boost-before.txt", "enabled\n")
    folder.write("boost-after.txt", "enabled\n")
    folder.write("write-hold.err", "=== try 1 at 2026-11-15T06:00:00Z\n" + LOAD)
    folder.write("queries-hold.err", "=== try 1 at 2026-11-15T06:05:00Z\n" + LOAD)
    folder.write("write-stat-before.txt", stat_lines(100, 1000))
    folder.write("write-stat-after.txt", stat_lines(110, 1090))
    folder.write("queries-stat-before.txt", stat_lines(200, 2000))
    folder.write("queries-stat-after.txt", stat_lines(203, 2097))
    folder.write("write.json", "=== try 1 at 2026-11-15T06:00:00Z\n" +
                 json.dumps(dict(INGEST, records=268744780)))
    folder.write("write.perf", PERF_STAT.format(
        cycles=640000000000, instructions=1400000000000, branches=200000000000,
        misses=1000000000, task=200000.0, user=180000000000, system=20000000000,
        run=200000000000))
    parameters = []
    for name in diag_summary.QUERIES:
        request = {"name": name, "kind": "rows", "start_ns": 1, "end_ns": 2, "locate": -1}
        parameters.append(request)
        folder.write(f"{name}.jsonl", (json.dumps(request) + "\n") * 4)
        for round_number in (1, 2):
            for level in diag_summary.LEVELS:
                stem = f"{name}-{level}-{round_number}"
                times = (engines or {}).get(level, [999000000, 70000000, 72000000, 74000000])
                runs = scalar_runs if level == "scalar" else None
                lines = [dict(READY, kernel_level=level)] + [answer(name, t, level, runs)
                                                             for t in times]
                folder.write(f"{stem}.out", "\n".join(json.dumps(line) for line in lines) + "\n")
                folder.write(f"{stem}.perf", PERF_STAT.format(
                    cycles=3000000000, instructions=6000000000, branches=900000000,
                    misses=3000000, task=900.0, user=850000000, system=50000000,
                    run=900000000))
                folder.write(f"{stem}.stat0", stat_lines(300, 3000))
                folder.write(f"{stem}.stat1", stat_lines(301, 3099))
    folder.write("parameters.json", json.dumps(parameters))
    return folder


PRICE_ROWS = [
    (45.00, 450, 450000, "column_decode_bench", "ZSTD_decompressSequences_bmi2"),
    (30.00, 300, 300000, "column_decode_bench", "tick_store::codec::decode_pfor_block(std::__1::span<std::byte const, 18446744073709551615ul>, std::__1::span<unsigned long, 18446744073709551615ul>)"),
    (15.00, 150, 150000, "column_decode_bench", "tick_store::store::decode_column(tick_store::store::ItchStoreReader const&, unsigned long, tick_store::store::Column)"),
    (10.00, 100, 100000, "libc.so.6", "0x000000000016330e"),
]


SRCFILE = """# Samples: 1K of event 'cycles:u'
# Event count (approx.): 1000000
#
# Overhead       Samples        Period  Source File
# ........
#
    45.00%           450        450000  /home/someone/deps/zstd-src/lib/decompress/zstd_decompress_block.c
    30.00%           300        300000  /home/someone/tick-store/src/codec/pfor.cpp
    15.00%           150        150000  /home/someone/tick-store/src/store/itch_store_reader.cpp
    10.00%           100        100000  [unknown]
"""


def price_folder(root, built=COMMIT):
    folder = Folder("price-decode-profile", root)
    profile_facts(folder, 4000)
    folder.fact("input", "store_decode/add_price on the slice 12302019-open, the timed passes")
    folder.write("price-decode-self.txt", report_text(PRICE_ROWS))
    folder.write("price-decode-srcfile.txt", SRCFILE)
    folder.write("perf-record.err", RECORDED.replace("23652", "1000"))
    folder.write("report.json", json.dumps({
        "case": "store_decode/add_price",
        "build": {"commit": built, "isa": "x86-64 baseline, avx2 kernels"},
        "verification": {"values": 2903311, "encoded_bytes": 2285511}}))
    return folder


class Base(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def run_main(self, folder, *extra, commit=COMMIT):
        out = self.root / "out.json"
        stderr = io.StringIO()
        with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
            code = diag_summary.main([str(folder), "--commit", commit, "--out", str(out), *extra])
        document = json.loads(out.read_text()) if code == 0 else None
        return code, document, stderr.getvalue()

    def assertRefused(self, folder, words, *extra, commit=COMMIT):
        code, _document, stderr = self.run_main(folder, *extra, commit=commit)
        self.assertEqual(code, 1, stderr)
        self.assertIn(words, stderr)
        self.assertFalse((self.root / "out.json").exists())


class FunctionName(unittest.TestCase):
    def test_return_types_parameters_and_template_spaces_are_dropped(self):
        cases = {
            "std::__1::pair<unsigned long*, bool> std::__1::__bitset_partition[abi:nqe230104]<std::__1::_ClassicAlgPolicy, unsigned long*, std::__1::ranges::less>(unsigned long*, unsigned long*, std::__1::ranges::less)":
                "std::__1::__bitset_partition[abi:nqe230104]<std::__1::_ClassicAlgPolicy, unsigned long*, std::__1::ranges::less>",
            "tick_store::codec::(anonymous namespace)::choose_width(std::__1::span<unsigned char const, 18446744073709551615ul>, bool) - -":
                "tick_store::codec::(anonymous namespace)::choose_width",
            "auto tick_store::itch::(anonymous namespace)::with_message_type<tick_store::itch::decode_strict(std::__1::span<std::byte const, 18446744073709551615ul>)::$_0>(std::byte)":
                "tick_store::itch::(anonymous namespace)::with_message_type<tick_store::itch::decode_strict(std::__1::span<std::byte const, 18446744073709551615ul>)::$_0>",
            "decltype(auto) std::__1::__variant_detail::__visitation::__base::__dispatcher<10ul>::__dispatch[abi:nqe230104]<int>(int)":
                "std::__1::__variant_detail::__visitation::__base::__dispatcher<10ul>::__dispatch[abi:nqe230104]<int>",
            "operator new(unsigned long)": "operator new",
            "tick_store::store::SegmentBuilder::State::operator()(tick_store::itch::OrderReplace const&)":
                "tick_store::store::SegmentBuilder::State::operator()",
            "0x0000000000163304": "0x0000000000163304",
        }
        for symbol, name in cases.items():
            self.assertEqual(diag_summary.function_name(symbol), name, symbol)

    def test_a_type_in_the_parameters_does_not_name_the_function(self):
        # The builder's add_order takes an itch type; it is book state, not parsing.
        groups = diag_summary.compile_groups("writer-profile")
        self.assertEqual(diag_summary.group_of(row(
            "tick_store::store::SegmentBuilder::State::add_order(std::byte, tick_store::itch::MessageHeader const&, unsigned long)"),
            groups), "book_state")


class Patterns(unittest.TestCase):
    def test_every_pattern_has_an_example_that_lands_in_its_group(self):
        for kind, groups in diag_summary.GROUPS.items():
            compiled = diag_summary.compile_groups(kind)
            for group, _about, patterns in groups:
                examples = EXAMPLES[kind].get(group, [])
                self.assertEqual(len(examples), len(patterns),
                                 f"{kind} {group}: one example for each of its patterns")
                for (how, pattern), (symbol, dso, level) in zip(patterns, examples):
                    example = row(symbol, dso, level)
                    self.assertEqual(diag_summary.group_of(example, compiled), group,
                                     f"{kind}: {symbol[:70]}")
                    # And the pattern itself matches it, not an earlier one of the group.
                    target = {"name": diag_summary.function_name(example["key"]),
                              "dso": dso or "", "level": level}[how]
                    if how == "level":
                        self.assertEqual(target, pattern)
                    else:
                        self.assertRegex(target, "^" + pattern)
            self.assertEqual(set(EXAMPLES[kind]), {group for group, *_ in groups}, kind)

    def test_the_probes_shared_objects_are_read(self):
        report = diag_summary.parse_report((FIXTURES / "writer-dso-top.txt").read_text(), "top")
        self.assertEqual(report["columns"], ["Overhead", "Shared Object", "Symbol"])
        groups = diag_summary.compile_groups("writer-profile")
        libcxx = [r for r in report["rows"] if r["dso"] == "libc++.so.1.0"]
        self.assertEqual([diag_summary.group_of(r, groups) for r in libcxx], ["sort"] * len(libcxx))
        self.assertEqual(report["rows"][0]["dso"], "tick_roundtrip")


class Reports(Base):
    def probe(self):
        return diag_summary.parse_report((FIXTURES / "writer-self.txt").read_text(), "self")

    def test_the_probes_flat_profile_is_grouped_and_sums_to_100(self):
        report = self.probe()
        self.assertEqual(len(report["rows"]), 217)
        self.assertEqual(report["event"], "cycles:u")
        groups = diag_summary.grouped("writer-profile", report)
        shares = {g["name"]: g["self_share_percent"] for g in groups}
        self.assertAlmostEqual(sum(shares.values()), 100.0, places=2)
        # The probe's printed shares sum to 99.66; each is taken over that sum.
        rows = report["rows"]
        self.assertAlmostEqual(sum(r["Overhead"] for r in rows), 99.66, places=2)
        self.assertEqual(max(shares, key=shares.get), "pfor_encode")
        self.assertGreater(shares["pfor_encode"], 40.0)
        self.assertGreater(shares["unresolved"], 4.0)

    def test_what_no_pattern_matches_is_its_own_share_and_spread_over_none(self):
        report = self.probe()
        compiled = diag_summary.compile_groups("writer-profile")
        shares = diag_summary.self_shares(report)
        expected = {}
        for r, share in zip(report["rows"], shares):
            group = diag_summary.group_of(r, compiled) or "unmatched"
            expected[group] = expected.get(group, 0.0) + share
        groups = diag_summary.grouped("writer-profile", report)
        for g in groups:
            self.assertAlmostEqual(g["self_share_percent"], expected.get(g["name"], 0.0), places=3,
                                   msg=g["name"])
        self.assertEqual(groups[-1]["name"], "unmatched")
        # The table names every symbol of the probe's profile; a symbol it does not know is
        # kept apart, its share its own.
        report = diag_summary.parse_report(report_text(QUERY_ROWS), "new")
        unmatched = diag_summary.grouped("query-profile", report)[-1]
        self.assertEqual((unmatched["name"], unmatched["self_share_percent"]), ("unmatched", 5.0))
        self.assertEqual(unmatched["largest"], [{"symbol": "tick_store::store::SomethingNew::run()",
                                                 "self_share_percent": 5.0}])

    def test_a_report_with_no_samples_is_refused(self):
        empty = report_text([])
        with self.assertRaisesRegex(diag_summary.Refused, "has no samples"):
            diag_summary.self_shares(diag_summary.parse_report(empty, "empty"))
        zero = report_text([(100.0, 0, 0, "tick_query", "main")], samples="0")
        with self.assertRaisesRegex(diag_summary.Refused, "has no samples"):
            diag_summary.self_shares(diag_summary.parse_report(zero, "zero"))
        folder = writer_folder(self.root, self_text=report_text([], samples="0")).finish()
        self.assertRefused(folder, "has no samples")

    def test_a_report_cut_at_a_percent_limit_is_refused(self):
        # The probe's lines at 0.05% or more: 85 lines that sum to 97.49%.
        text = (FIXTURES / "writer-self.txt").read_text()
        kept = [line for line in text.splitlines()
                if not diag_summary.DATA_LINE.match(line) or float(line.split("%")[0]) >= 0.05]
        report = diag_summary.parse_report("\n".join(kept) + "\n", "cut")
        self.assertEqual(len(report["rows"]), 85)
        with self.assertRaisesRegex(diag_summary.Refused, "not 100 within"):
            diag_summary.self_shares(report)
        # The same cut of the folder's report, with its samples and periods.
        counted = probe_with_counts()[0]
        kept = [line for line in counted.splitlines()
                if not diag_summary.DATA_LINE.match(line) or float(line.split("%")[0]) >= 0.05]
        folder = writer_folder(self.root, self_text="\n".join(kept) + "\n").finish()
        self.assertRefused(folder, "not 100 within")

    def test_periods_that_miss_the_event_count_are_refused(self):
        text = report_text(QUERY_ROWS, event_count=1000001)
        with self.assertRaisesRegex(diag_summary.Refused, "not the event count"):
            diag_summary.self_shares(diag_summary.parse_report(text, "short"))

    def test_shares_are_computed_from_the_periods(self):
        rows = [(33.33, 1, 1, "a", "f()"), (66.67, 2, 2, "a", "g()")]
        shares = diag_summary.self_shares(diag_summary.parse_report(report_text(rows), "p"))
        self.assertAlmostEqual(shares[0], 100 / 3, places=9)
        wrong = [(50.00, 1, 1, "a", "f()"), (50.00, 2, 2, "a", "g()")]
        with self.assertRaisesRegex(diag_summary.Refused, "but its period"):
            diag_summary.self_shares(diag_summary.parse_report(report_text(wrong), "w"))

    def test_a_line_the_parser_cannot_read_is_refused(self):
        text = report_text(QUERY_ROWS).replace("  [.] tick_store::harness", "  tick_store::harness")
        with self.assertRaisesRegex(diag_summary.Refused, "is not an entry of its columns"):
            diag_summary.parse_report(text, "bad")
        with self.assertRaisesRegex(diag_summary.Refused, "before perf's column header"):
            diag_summary.parse_report("    12.00%  [.] main\n", "h")
        with self.assertRaisesRegex(diag_summary.Refused, "no column header"):
            diag_summary.parse_report("# Samples: 1K of event 'cycles:u'\n", "h")


class Summaries(Base):
    def test_the_writers_summary(self):
        code, document, stderr = self.run_main(writer_folder(self.root).finish())
        self.assertEqual(code, 0, stderr)
        self.assertEqual(document["kind"], "writer-profile")
        self.assertEqual(document["commit"], COMMIT)
        self.assertEqual(document["measurement"], "diagnostic")
        self.assertIn("not times", document["statement"])
        self.assertEqual(document["build"], {
            "preset": "bench", "used": "bench-lines", "extra_flags": "-gline-tables-only",
            "text_identical": True, "note": document["build"]["note"]})
        self.assertEqual((document["event"], document["frequency_hz"]), ("cycles:u", 199))
        self.assertEqual(document["samples_recorded"], 23652)
        self.assertEqual([g["name"] for g in document["groups"]],
                         [g["name"] for g in document["group_table"]["groups"]])
        self.assertEqual(document["records"], 6125294)
        self.assertNotIn("wall_seconds", json.dumps(document))
        inclusive = document["inclusive"]["children_share_percent"]
        self.assertEqual((inclusive["encode_column"], inclusive["encode_pfor"],
                          inclusive["encode_dict"]), (59.01, 31.82, 25.62))
        self.assertIsNone(inclusive["encode_zstd_payload"])
        self.assertEqual(document["inclusive"]["limit_percent"], 0.5)
        layout = document["store_layout"]
        self.assertEqual(layout["codecs"]["pfor"], {"pieces": 3, "payload_bytes": 16000,
                                                   "values_in_single_codec_columns": 2900000})
        self.assertEqual(layout["columns_with_several_codecs"], ["timestamp"])
        self.assertTrue(all("/" not in c.split(" ")[0] for c in document["commands"]))
        self.assertIn("-o writer.data", document["commands"][0])

    def test_no_path_host_or_user_is_left_and_the_hygiene_check_passes(self):
        code, _document, stderr = self.run_main(writer_folder(self.root).finish())
        self.assertEqual(code, 0, stderr)
        text = (self.root / "out.json").read_text()
        self.assertEqual(diag_summary.ABSOLUTE.findall(text), [])
        self.assertNotIn("someone", text)
        sample = self.root / "results" / "diag"
        sample.mkdir(parents=True)
        shutil.copy(self.root / "out.json", sample / f"writer-profile-12302019-{COMMIT[:12]}.json")
        self.assertEqual(check_hygiene.problems_in(self.root, f"results/diag/writer-profile-"
                                                              f"12302019-{COMMIT[:12]}.json"), [])

    def test_a_path_the_summary_would_publish_is_refused(self):
        folder = writer_folder(self.root)
        folder.fact("input", "the day in /data/itch/12302019.NASDAQ_ITCH50")
        self.assertRefused(folder.finish(), "absolute path")
        folder = writer_folder(self.root)
        self.assertRefused(folder.finish(), "the name 'builder'", "--forbid", "builder")

    def test_a_commit_that_is_not_the_one_given_is_refused(self):
        self.assertRefused(writer_folder(self.root).finish(), "not the commit given",
                           commit=OTHER)
        self.assertRefused(price_folder(self.root, built=OTHER).finish(), "was built at")

    def test_a_build_that_is_not_the_bench_preset_is_refused(self):
        folder = writer_folder(self.root)
        folder.fact("build_preset", "dev")
        self.assertRefused(folder.finish(), "is not the bench preset")

    def test_a_smoke_is_iteration_only(self):
        folder = writer_folder(self.root)
        folder.fact("mode", "smoke")
        path = folder.finish()
        self.assertRefused(path, "iteration only")
        code, document, stderr = self.run_main(path, "--iteration")
        self.assertEqual(code, 0, stderr)
        self.assertIn("iteration only", document["measurement"])
        # Review DX-8, DX-9: never anywhere under results/, results/diag/ or not. The guard is
        # shown on this test's own results/, so a broken one writes nothing into the repo's.
        results = self.root / "results"
        with mock.patch.object(diag_summary, "RESULTS_ROOT", results), \
                mock.patch.object(diag_summary, "RESULTS", results / "diag"):
            for out in (results / "diag" / "smoke-test.json", results / "smoke-test.json",
                        results / "bench" / "smoke-test.json"):
                with redirect_stderr(io.StringIO()) as err, redirect_stdout(io.StringIO()):
                    code = diag_summary.main([str(path), "--commit", COMMIT, "--iteration",
                                              "--out", str(out)])
                self.assertEqual(code, 1, out)
                self.assertIn("never written under results/", err.getvalue())
                self.assertFalse(out.exists(), out)

    def test_the_queries_summary(self):
        code, document, stderr = self.run_main(query_folder(self.root).finish())
        self.assertEqual(code, 0, stderr)
        self.assertEqual(len(document["queries"]), 5)
        q1 = document["queries"][0]
        self.assertEqual((q1["name"], q1["repetitions"], q1["values_decoded"], q1["segments"]),
                         ("Q1", 3, 7559833, 16))
        shares = {g["name"]: g["self_share_percent"] for g in q1["groups"]}
        self.assertEqual((shares["dictionary_decode"], shares["digest"], shares["unpack_kernels"],
                          shares["libc"], shares["unmatched"]), (40.0, 25.0, 20.0, 10.0, 5.0))
        self.assertEqual((q1["engine_share_percent"], q1["digest_share_percent"],
                          q1["store_open_share_percent"]), (60.0, 26.0, 9.0))
        self.assertEqual(document["store"]["segment_records"], 4194304)
        self.assertNotIn("engine_ns", json.dumps(document))

    def test_an_answer_that_is_an_error_is_refused(self):
        folder = query_folder(self.root)
        folder.write("Q3.out", json.dumps(READY) + "\n" +
                     json.dumps({"name": "Q3", "kind": "rows", "error": "locate must be -1"}) + "\n")
        self.assertRefused(folder.finish(), "is an error")

    def test_the_counters_summary_reads_engine_times_from_the_answers(self):
        code, document, stderr = self.run_main(counters_folder(self.root).finish())
        self.assertEqual(code, 0, stderr)
        write = document["write"]
        self.assertEqual(write["records"], 268744780)
        self.assertEqual(write["cycles"], 640000000000)
        self.assertAlmostEqual(write["per_message"]["cycles"], 640000000000 / 268744780, places=3)
        self.assertEqual(write["effective_mhz"], round(640000000000 / 180 / 1e6, 1))
        self.assertEqual(write["system_share_of_cpu_time_percent"], 10.0)
        self.assertNotIn("wall_seconds", json.dumps(document))
        runs = document["queries"]
        self.assertEqual(len(runs), 20)
        # The first answer (999 ms) is the untimed warm-up; the median is of the other three.
        self.assertEqual((runs[0]["timed_repetitions"], runs[0]["engine_ms_median"],
                          runs[0]["engine_ms_slowest"]), (3, 72.0, 74.0))
        self.assertEqual(runs[0]["sibling_busy_percent"], 1.0)
        self.assertEqual(document["sibling_busy_percent"]["holds"], {"write": 10.0, "queries": 3.0})
        self.assertEqual(document["sibling_busy_percent"]["sibling"], 5)
        self.assertEqual(document["turbo"], "enabled")
        self.assertEqual(document["governor"], "performance")
        self.assertEqual(len(document["load_lines"]), 4)
        self.assertNotIn("somehost", json.dumps(document))
        self.assertEqual(document["simd_change"][0]["avx2_over_scalar_median"], 1.0)

    def test_the_simd_change_is_avx2_over_scalar(self):
        folder = counters_folder(self.root, engines={
            "avx2": [999000000, 60000000, 60000000, 60000000],
            "scalar": [999000000, 80000000, 80000000, 80000000]})
        code, document, stderr = self.run_main(folder.finish())
        self.assertEqual(code, 0, stderr)
        change = document["simd_change"][2]
        self.assertEqual((change["avx2_engine_ms_median"], change["scalar_engine_ms_median"],
                          change["avx2_over_scalar_median"]), (60.0, 80.0, 0.75))

    def test_a_run_capped_to_scalar_that_ran_avx2_kernels_is_refused(self):
        folder = counters_folder(self.root, scalar_runs={"scalar": 1000, "neon": 0, "avx2": 5})
        self.assertRefused(folder.finish(), "capped to scalar, an answer ran")

    def test_a_run_whose_tool_chose_another_level_is_refused(self):
        folder = counters_folder(self.root)
        lines = (folder.path / "Q2-scalar-1.out").read_text().splitlines()
        lines[0] = json.dumps(dict(READY, kernel_level="avx2"))
        folder.write("Q2-scalar-1.out", "\n".join(lines) + "\n")
        self.assertRefused(folder.finish(), "asked for scalar, the tool ran avx2")

    def test_the_price_decode_summary(self):
        code, document, stderr = self.run_main(price_folder(self.root).finish())
        self.assertEqual(code, 0, stderr)
        shares = {g["name"]: g["self_share_percent"] for g in document["groups"]}
        self.assertEqual((shares["zstd"], shares["pfor_block_decode"],
                          shares["store_column_decode"], shares["libc"]), (45.0, 30.0, 15.0, 10.0))
        self.assertEqual(document["case"], "store_decode/add_price")
        self.assertEqual((document["samples_recorded"], document["samples_in_report"]),
                         (1000, 1000))
        self.assertIsNone(document["inclusive"])
        self.assertEqual([(f["file"], f["self_share_percent"]) for f in document["source_files"]],
                         [("zstd_decompress_block.c", 45.0), ("pfor.cpp", 30.0),
                          ("itch_store_reader.cpp", 15.0), ("[unknown]", 10.0)])

    def test_a_folder_of_no_diagnostic_is_refused(self):
        empty = self.root / "empty"
        empty.mkdir()
        self.assertRefused(empty, "no facts.txt")


def edit_answers(folder, stem, change, first=False):
    """Each answer line of {stem}.out changed; the ready line kept, and the untimed first answer
    too unless first."""
    lines = [json.loads(line) for line in (folder.path / f"{stem}.out").read_text().splitlines()]
    kept = 1 if first else 2
    lines = lines[:kept] + [change(dict(line)) for line in lines[kept:]]
    folder.write(f"{stem}.out", "\n".join(json.dumps(line) for line in lines) + "\n")


def every_run(levels=diag_summary.LEVELS):
    return [(name, level, round_number) for name in diag_summary.QUERIES
            for level in levels for round_number in (1, 2)]


class Recording(Base):
    """Review DX-7: a profile is tied to its recording."""

    def query(self, change):
        folder = query_folder(self.root)
        for name in diag_summary.QUERIES:
            change(folder, name)
        return folder.finish()

    def test_a_count_with_no_unit_is_read_with_its_event(self):
        # perf pads a count under a thousand: "# Samples: 827  of event 'cycles:u'".
        report = diag_summary.parse_report(report_text(QUERY_ROWS, samples="827"), "small")
        self.assertEqual((report["samples_header"], report["event"]), ("827", "cycles:u"))

    def test_a_report_that_names_no_event_is_refused(self):
        def drop(folder, name):
            folder.write(f"{name}-self.txt", "\n".join(
                line for line in report_text(QUERY_ROWS).splitlines()
                if not line.startswith("# Samples")) + "\n")
        self.assertRefused(self.query(drop), "no '# Samples: ... of event' line")

    def test_a_report_of_another_event_is_refused(self):
        def other(folder, name):
            folder.write(f"{name}-self.txt", report_text(QUERY_ROWS, event="instructions:u"))
        self.assertRefused(self.query(other), "Q1-self.txt counts instructions:u, not cycles:u")

    def test_a_report_with_no_sample_or_period_column_or_event_count_is_refused(self):
        def percent_only(folder, name):
            text = report_text([(r[0], r[1], r[2], r[3], r[4]) for r in QUERY_ROWS])
            folder.write(f"{name}-self.txt", "\n".join(
                line if not line.startswith("    ") else line[:13] + line[41:]
                for line in text.splitlines()).replace(
                    "# Overhead  Samples  Period  Shared Object", "# Overhead  Shared Object")
                + "\n")
        self.assertRefused(self.query(percent_only), "no Samples or Period column")

        def no_count(folder, name):
            folder.write(f"{name}-self.txt", "\n".join(
                line for line in report_text(QUERY_ROWS).splitlines()
                if not line.startswith("# Event count")) + "\n")
        self.assertRefused(self.query(no_count), "no '# Event count' line")

    def test_a_report_short_of_its_recording_is_refused(self):
        def more(folder, name):
            folder.write(f"{name}-perf-record.err", RECORDED.replace("23652", "50000"))
        self.assertRefused(self.query(more), "Q1-self.txt holds 1000 samples, and "
                           "Q1-perf-record.err says perf recorded 50000")

    def test_a_recording_of_no_samples_is_refused(self):
        def none(folder, name):
            folder.write(f"{name}-perf-record.err", RECORDED.replace("23652", "0"))
        self.assertRefused(self.query(none), "Q1-perf-record.err says perf recorded no samples")

    def test_samples_lost_beyond_the_bound_are_refused(self):
        def lost(count):
            def change(folder, name):
                folder.write(f"{name}-self.txt", report_text(QUERY_ROWS, lost=count))
            return change
        self.assertRefused(self.query(lost(5000)), "perf lost 5000 samples beside the 1000")
        self.assertRefused(self.query(lost(2)), "perf lost 2 samples")
        code, document, stderr = self.run_main(self.query(lost(1)))
        self.assertEqual(code, 0, stderr)
        self.assertEqual(document["queries"][0]["lost_samples"], 1)

    def test_a_children_or_source_file_report_of_another_recording_is_refused(self):
        for keyword, words in (({"event": "instructions:u"}, "its event is instructions:u"),
                               ({"samples": "90K"}, "its sample count is 90K"),
                               ({"event_count": 999}, "its event count is 999")):
            def other(folder, name, keyword=keyword):
                folder.write(f"{name}-children.txt", children_text(QUERY_CHILDREN, **keyword))
            self.assertRefused(self.query(other), words)
        folder = query_folder(self.root)
        folder.write("Q5-children.txt", children_text(QUERY_CHILDREN).replace(
            "# Event count (approx.): 1000000\n", ""))
        self.assertRefused(folder.finish(), "Q5-children.txt: no sample or event count line, so "
                           "it is not shown to be of Q5-self.txt's recording")
        folder = price_folder(self.root)
        folder.write("price-decode-srcfile.txt", SRCFILE.replace("cycles:u", "instructions:u"))
        self.assertRefused(folder.finish(), "price-decode-srcfile.txt: its event is instructions:u")
        folder = price_folder(self.root)
        folder.write("price-decode-srcfile.txt", SRCFILE.replace(
            "# Event count (approx.): 1000000\n", ""))
        self.assertRefused(folder.finish(), "no '# Event count' line")

    def test_what_the_raw_output_does_not_hold_is_listed_not_established(self):
        folder = query_folder(self.root)
        (folder.path / "Q2-perf-record.err").unlink()
        code, document, stderr = self.run_main(folder.finish())
        self.assertEqual(code, 0, stderr)
        self.assertIsNone(document["queries"][1]["samples_recorded"])
        self.assertEqual([(item["path"], item["what"]) for item in document["not_established"]],
                         [(["queries", {"name": "Q2"}], "the recording's sample count")])
        text = probe_with_counts()[0].replace("# Total Lost Samples: 0\n", "")
        code, document, stderr = self.run_main(writer_folder(self.root, self_text=text).finish())
        self.assertEqual(code, 0, stderr)
        self.assertIsNone(document["lost_samples"])
        self.assertEqual([(item["path"], item["what"]) for item in document["not_established"]],
                         [([], "the samples lost")])
        self.assertIn("Total Lost Samples", document["not_established"][0]["why"])

    def test_a_clean_summary_lists_nothing_not_established(self):
        for folder in (writer_folder, query_folder, counters_folder, price_folder):
            code, document, stderr = self.run_main(folder(self.root).finish())
            self.assertEqual(code, 0, stderr)
            self.assertEqual(document["not_established"], [], folder.__name__)


class Inclusive(Base):
    """Review DX-12: inclusive shares are never added up."""

    def test_two_entries_of_one_share_are_refused(self):
        rows = QUERY_CHILDREN + [(58.0, 0.5, 5, "tick_query", "tick_store::store::ItchQuery::"
                                  "locate_counts(unsigned long)")]
        folder = query_folder(self.root)
        folder.write("Q4-children.txt", children_text(rows))
        self.assertRefused(folder.finish(), "Q4-children.txt: 2 entries match the inclusive "
                           "share 'engine'")

    def test_a_lambda_of_the_function_is_not_added_to_its_share(self):
        lam = ("tick_store::store::ItchQuery::rows(unsigned long, unsigned long, std::__1::"
               "optional<unsigned short>)::$_0::operator()(tick_store::store::QueryRow const&) "
               "const")
        self.assertEqual(diag_summary.function_name(lam), "tick_store::store::ItchQuery::rows")
        self.assertTrue(diag_summary.local_to_function(lam))
        self.assertFalse(diag_summary.local_to_function(QUERY_CHILDREN[1][4]))
        self.assertFalse(diag_summary.local_to_function("operator new(unsigned long)"))
        folder = query_folder(self.root)
        folder.write("Q1-children.txt", children_text(
            QUERY_CHILDREN + [(1.76, 0.59, 53, "tick_query", lam)]))
        code, document, stderr = self.run_main(folder.finish())
        self.assertEqual(code, 0, stderr)
        self.assertEqual(document["queries"][0]["engine_share_percent"], 60.0)


class SimdChange(Base):
    """Review DX-6: the AVX2-over-scalar ratio is SIMD's change or it is not published."""

    def test_an_avx2_run_that_ran_no_avx2_kernel_is_refused(self):
        folder = counters_folder(self.root)
        for name, level, round_number in every_run(("avx2",)):
            edit_answers(folder, f"{name}-{level}-{round_number}", lambda answer: dict(
                answer, kernel_runs={"scalar": 1200, "neon": 0, "avx2": 0}), first=True)
        self.assertRefused(folder.finish(), "Q1-avx2-1: no answer ran an AVX2 kernel, and "
                           "Q1-scalar-1's ran 3600")

    def test_answers_with_no_kernel_runs_leave_the_ratio_not_established(self):
        folder = counters_folder(self.root)
        edit_answers(folder, "Q3-scalar-2", lambda answer: {
            key: value for key, value in answer.items() if key != "kernel_runs"})
        code, document, stderr = self.run_main(folder.finish())
        self.assertEqual(code, 0, stderr)
        change = {entry["name"]: entry for entry in document["simd_change"]}
        self.assertIsNone(change["Q3"]["avx2_over_scalar_median"])
        self.assertEqual(change["Q2"]["avx2_over_scalar_median"], 1.0)
        self.assertEqual([(item["path"], item["what"]) for item in document["not_established"]],
                         [(["simd_change", {"name": "Q3"}], "the kernels each level ran")])

    def test_neither_level_running_a_kernel_leaves_the_ratio_not_established(self):
        folder = counters_folder(self.root)
        for name, level, round_number in every_run():
            if name == "Q5":
                edit_answers(folder, f"{name}-{level}-{round_number}", lambda answer: dict(
                    answer, kernel_runs={"scalar": 0, "neon": 0, "avx2": 0}), first=True)
        code, document, stderr = self.run_main(folder.finish())
        self.assertEqual(code, 0, stderr)
        self.assertIsNone(document["simd_change"][4]["avx2_over_scalar_median"])
        self.assertEqual(document["not_established"][0]["what"], "a SIMD change")

    def test_levels_or_rounds_that_answer_differently_are_refused(self):
        folder = counters_folder(self.root)
        edit_answers(folder, "Q1-scalar-1", lambda answer: dict(answer, rows=5,
                                                                sha256="cd" * 32))
        self.assertRefused(folder.finish(), "Q1-scalar-1: answer 2 is")
        folder = counters_folder(self.root)
        edit_answers(folder, "Q2-avx2-2", lambda answer: dict(answer, sha256="cd" * 32))
        self.assertRefused(folder.finish(), "the levels or the rounds answer differently")

    def test_answers_with_no_digest_leave_the_ratio_not_established(self):
        folder = counters_folder(self.root)
        edit_answers(folder, "Q4-avx2-1", lambda answer: {
            key: value for key, value in answer.items() if key != "sha256"})
        code, document, stderr = self.run_main(folder.finish())
        self.assertEqual(code, 0, stderr)
        self.assertIsNone(document["simd_change"][3]["avx2_over_scalar_median"])
        self.assertEqual(document["not_established"][0]["what"], "the levels' answers alike")


class Refusals(Base):
    """Review DX-8: a test for each refusal no test held."""

    def test_a_query_file_whose_first_line_is_no_ready_line_is_refused(self):
        folder = query_folder(self.root)
        folder.write("Q2.out", "\n".join(json.dumps(answer("Q2", 74000000)) for _ in range(4))
                     + "\n")
        self.assertRefused(folder.finish(), "Q2.out: the query tool's first line is not its "
                           "ready line")
        folder = counters_folder(self.root)
        lines = (folder.path / "Q4-scalar-2.out").read_text().splitlines()
        folder.write("Q4-scalar-2.out", "\n".join(lines[1:]) + "\n")
        self.assertRefused(folder.finish(), "Q4-scalar-2.out: the query tool's first line")

    def test_answers_that_are_not_one_to_each_request_are_refused(self):
        folder = query_folder(self.root)
        request = json.loads((folder.path / "Q2.jsonl").read_text().splitlines()[0])
        folder.write("Q2.jsonl", (json.dumps(request) + "\n") * 4)
        self.assertRefused(folder.finish(), "Q2: 3 answers to 4 requests")
        folder = counters_folder(self.root)
        request = json.loads((folder.path / "Q3.jsonl").read_text().splitlines()[0])
        folder.write("Q3.jsonl", (json.dumps(request) + "\n") * 5)
        self.assertRefused(folder.finish(), "Q3-avx2-1: 4 answers to 5 requests")
        # One request is the untimed one alone: no engine time is left.
        folder = counters_folder(self.root, engines={"avx2": [999000000],
                                                     "scalar": [999000000]})
        folder.write("Q1.jsonl", json.dumps(request) + "\n")
        self.assertRefused(folder.finish(), "Q1-avx2-1: 1 answers to 1 requests")

    def test_query_runs_on_other_stores_are_refused(self):
        folder = query_folder(self.root)
        folder.write("Q3.out", "\n".join(json.dumps(line) for line in
                                         [dict(READY, records=1, segments=1)]
                                         + [answer("Q3", 74000000)] * 3) + "\n")
        self.assertRefused(folder.finish(), "Q3.out: the tool opened a store of")
        folder = counters_folder(self.root)
        lines = (folder.path / "Q5-scalar-1.out").read_text().splitlines()
        lines[0] = json.dumps(dict(READY, kernel_level="scalar", records=1))
        folder.write("Q5-scalar-1.out", "\n".join(lines) + "\n")
        self.assertRefused(folder.finish(), "Q5-scalar-1.out: the tool opened a store of")

    def test_a_topology_without_one_sibling_is_refused(self):
        for cpus, count in (("4\n", 0), ("4-6\n", 2)):
            folder = counters_folder(self.root)
            folder.write("topology.txt", cpus)
            self.assertRefused(folder.finish(), f"topology.txt names {count} siblings of cpu4")

    def test_a_window_in_which_no_jiffies_passed_is_refused(self):
        folder = counters_folder(self.root)
        folder.write("Q2-avx2-1.stat1", stat_lines(300, 3000))
        self.assertRefused(folder.finish(), "no jiffies passed on cpu5 between Q2-avx2-1.stat0 "
                           "and Q2-avx2-1.stat1")

    def test_a_counter_missing_not_counted_or_multiplexed_is_refused(self):
        values = dict(cycles=640000000000, instructions=1400000000000, branches=200000000000,
                      misses=1000000000, task=200000.0, user=180000000000, system=20000000000,
                      run=200000000000)
        text = PERF_STAT.format(**values)
        for edited, words in (
                ("\n".join(line for line in text.splitlines() if ",branches:u," not in line),
                 "perf stat did not report branches:u"),
                (text.replace("640000000000,,cycles:u", "<not counted>,,cycles:u"),
                 "perf stat did not count cycles:u: perf said <not counted>"),
                (text.replace("instructions:u,200000000000,100.00", "instructions:u,"
                              "200000000000,12.00"),
                 "perf stat counted instructions:u 12.00% of the time, multiplexed")):
            folder = counters_folder(self.root)
            folder.write("write.perf", edited)
            self.assertRefused(folder.finish(), words)

    def test_a_commit_that_is_not_forty_hex_digits_is_refused(self):
        self.assertRefused(writer_folder(self.root).finish(), "is not 40 hex digits",
                           commit=COMMIT[:12])

    def test_this_machines_name_or_user_is_refused(self):
        for patched in ("gethostname", "getuser"):
            folder = writer_folder(self.root)
            folder.fact("input", "the day, staged by buildhostx")
            module = diag_summary.socket if patched == "gethostname" else diag_summary.getpass
            with mock.patch.object(module, patched, return_value="buildhostx"):
                self.assertRefused(folder.finish(), "the name 'buildhostx'")

    def test_the_statement_names_the_commit_it_was_run_at(self):
        code, document, stderr = self.run_main(writer_folder(self.root).finish())
        self.assertEqual(code, 0, stderr)
        self.assertIn(f"one run at commit {COMMIT[:12]}", document["statement"])
        self.assertNotIn("freeze", document["statement"])


class CommandLine(unittest.TestCase):
    def test_paths_in_commands_keep_only_their_names(self):
        self.assertEqual(diag_summary.strip_paths(
            "perf record --control=fifo:/a/b/ctl,/a/b/ack -o /x/y/w.data -- /p/tick_query "
            "--report=/q/r.json tools/tick_query"),
            "perf record --control=fifo:ctl,ack -o w.data -- tick_query --report=r.json "
            "tools/tick_query")

    def test_the_script_runs_with_isolated_python(self):
        done = subprocess.run([sys.executable, "-I", str(REPO_ROOT / "scripts" / "diag_summary.py"),
                               "--help"], capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)


if __name__ == "__main__":
    unittest.main()
