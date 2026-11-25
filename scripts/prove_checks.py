#!/usr/bin/env python3
"""Show every check failing on an injected fault. A check that cannot fail proves nothing.

    python3 -I scripts/prove_checks.py [--force] [--only id,id] [--raw-logs DIR] [--list]

Each fault in FAULTS names one step of scripts/check.sh and a patch in faults/ or a mutation
below. For each fault this script:

  1. runs the step on the clean tree, which must pass (the baseline),
  2. applies the fault and runs the same step,
  3. requires that step to fail with the expected exit code AND the expected string in its
     log, so a build error cannot pass for a detection,
  4. reverts the fault.

Faults touch only a throwaway clone under build/fault-proofs/, never the working tree, and
nothing in the production code knows about them. The clone keeps one incremental build per
preset, and runs one step at a time, so sanitizer builds never overlap.

A fault that goes undetected is reported as UNDETECTED, and the script exits 1. Nothing is
weakened to get a pass: the gap gets fixed, and the fault stays.

Results go to results/check-proofs.json after every fault, so a stopped run resumes where it
stopped. A fault already PROVEN for this code is skipped unless --force. The key is the last
commit that changed code, not prose or the summaries, plus a fingerprint of the fault itself:
its fields and patch, the code of its mutation and of this script's verdict path, and the
committed content of each file it edits, prose included.
Raw logs go to --raw-logs or CHECK_PROOF_LOGS, by default build/fault-proofs/logs. The
toolchain comes from the environment.

Standard library only.
"""

import argparse
import copy
import functools
import hashlib
import inspect
import json
import os
import platform
import shutil
import subprocess
import sys
import types
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FAULTS_DIR = REPO_ROOT / "faults"
WORK = REPO_ROOT / "build" / "fault-proofs"
TREE = WORK / "tree"
OUTPUT = REPO_ROOT / "results" / "check-proofs.json"
SCHEMA = "tick-store-check-proofs-1"
NOT_CODE = (":(exclude)results/check-proofs.json", ":(exclude)results/coverage.json",
            ":(exclude)*.md", ":(exclude)faults", ":(exclude)scripts/prove_checks.py")
CHECK_FAILED = 1
FUZZ_ARGUMENTS = ("--fuzz-seconds", "45", "--fuzz-seed", "1", "--fresh-corpus")

# What a run of ctest and libFuzzer exit with on a detection.
EXIT_CTEST = 8
EXIT_BUILD = 1
EXIT_LIBFUZZER = 77
# The fuzz build when the seed generator it runs aborts on an uncaught exception.
EXIT_SEED_GENERATOR = 134
# render_results.py when a results file it reads fails its validator.
EXIT_RENDER_REFUSED = 2

TIDY_ONE_FILE = ("--tidy-files", "src/itch/frame.cpp")
STORE_CORRUPT = ("--test-regex", "ItchStoreCorrupt")
QUERY_CORRUPT = ("--test-regex", "ItchQueryCorrupt")


def edit_json(path, change):
    """Writes the document back only if `change` changed it, so a mutation that changes
    nothing leaves the tree as it was, which the prover's tests then see (review RP-13)."""
    target = TREE / path
    document = json.loads(target.read_text())
    before = copy.deepcopy(document)
    change(document)
    if document != before:
        target.write_text(json.dumps(document, indent=2) + "\n")


def edit_text(path, old, new):
    target = TREE / path
    text = target.read_text()
    if old not in text:
        raise RuntimeError(f"{path} has no {old!r} to replace")
    target.write_text(text.replace(old, new, 1))


# results/bench holds one file per host and benchmark; the faults that edit one take the ITCH
# codec benchmark's, which every host has.
BENCH_FAULT_BENCHMARK = "itch_codec"
# results/compare holds a host's slice and its full days; the faults that edit one take the
# macOS slice, which has all four systems.
COMPARE_FAULT_SLICE = "12302019-open"


def host_results(host, folder, root=None, benchmark=None):
    """The one results file of a host in results/<folder>, by pattern, so a re-measure cannot
    strand the faults that edit it. In a folder of several benchmarks, name the benchmark.
    The tree is the clone unless named, read when called, so a test can point it elsewhere."""
    root = TREE if root is None else root
    pattern = f"{host}-{benchmark}-*.json" if benchmark else f"{host}-*.json"
    found = sorted((root / "results" / folder).glob(pattern))
    if len(found) != 1:
        raise SystemExit(f"prove_checks: results/{folder} needs exactly one {pattern}, "
                         f"and has {len(found)}: {[path.name for path in found]}")
    return found[0]


def macos_results(folder, root=None, benchmark=None):
    return host_results("macOS", folder, root, benchmark)


def linux_results(folder, root=None, benchmark=None):
    """From the pinned host."""
    return host_results("Linux", folder, root, benchmark)


def add_file(path, content):
    target = TREE / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    subprocess.run(["git", "-C", str(TREE), "add", "-f", path], check=True)


def drop_load_lines():
    edit_json(macos_results("bench", benchmark=BENCH_FAULT_BENCHMARK), lambda d: d["header"].update(load_lines=[]))


def unmeasured_turbo_on_linux():
    """What run_bench.py records on a host with no boost switch, such as a hosted runner."""
    edit_json(linux_results("bench", benchmark=BENCH_FAULT_BENCHMARK), lambda d: d["header"].update(
        turbo="unavailable: no cpufreq boost switch, as on a virtual machine"))


def claim_pinned_on_macos():
    edit_json(macos_results("bench", benchmark=BENCH_FAULT_BENCHMARK), lambda d: d["header"].update(pinning="pinned"))


def drop_commit_source():
    """The macOS ITCH codec file without the header's commit_source, as the files from before
    review RA-5 were."""
    edit_json(macos_results("bench", benchmark=BENCH_FAULT_BENCHMARK),
              lambda d: d["header"].pop("commit_source"))


def sub_resolution_percentile():
    def change(document):
        document["results"][0]["latency"]["percentiles"][0]["value_ns"] = 0.5
    edit_json(macos_results("bench", benchmark=BENCH_FAULT_BENCHMARK), change)


def questdb_variant(document):
    return next(iter(document["systems"]["questdb"]["variants"].values()))


def edit_compare_slice(change):
    """The macOS slice, edited. It states protocol 2, as every file in results/compare/ does
    since the freeze, so no pin holds its bytes, and only the rule the fault is about can
    refuse it. A protocol 1 slice was re-pinned here once; no file is pinned that way now."""
    edit_json(macos_results("compare", benchmark=COMPARE_FAULT_SLICE), change)


def restore_a_pin(key, name, digest):
    """A line put back under a pins key the freeze's files retired."""
    edit_json("scripts/data_pins.json", lambda pins: pins.setdefault(key, {}).update({name: digest}))


def protocol_1_pin_restored():
    """The kept Linux day pinned again as a protocol 1 file, as it was before the freeze's files
    retired the key."""
    restore_a_pin("protocol_1_comparison_files", "Linux-12302019-full-67ae574f5666.json",
                  "e8d447073c29d83c2341357df8ca01fa9afaa50d4f235590d5506e0d10a41a25")


def roundtrip_pin_restored():
    """12302019's round-trip file from before the exit codes pinned again by its bytes."""
    restore_a_pin("roundtrip_files_before_exit_codes", "roundtrip-12302019.json",
                  "b22de55c624c3bb1c0570a982b376d07c4bd75d777a85de318797b74d4c10001")


def mismatched_commit_mode():
    def change(document):
        for setting in questdb_variant(document)["classes"]["B"]["settings"]:
            if setting["name"] == "cairo.commit.mode (conf file)":
                setting["in_force"] = "nosync"
    edit_compare_slice(change)


def load_lines_as_a_string():
    """The macOS slice's load lines as one string, no list of a lock's start and end lines
    (review E-2)."""
    def change(document):
        document["header"]["load_lines"] = " ".join(document["header"]["load_lines"])
    edit_compare_slice(change)


def failed_read_back():
    def change(document):
        oracle = questdb_variant(document)["classes"]["B"]["read_back"]["oracle"]
        oracle["verdict"] = "mismatch"
        oracle["exit_code"] = 1
    edit_compare_slice(change)


def query_digest_mismatch():
    def change(document):
        questdb_variant(document)["queries"]["Q1"]["digest"] = "0" * 64
    edit_compare_slice(change)


def unflagged_swap_in():
    def change(document):
        cold = questdb_variant(document)["queries"]["Q1"]["cold"]["swap_timed"]
        cold["pages_in"] += 1
        cold["pages_in_by_repetition"][0] += 1
        cold["flagged"] = False
    edit_compare_slice(change)


def drop_g3_run(target, host):
    """G-3's results file with one target's run on a host taken out, and its summary
    recounted, so only the check that the target is required there can refuse it."""
    found = sorted((TREE / "results").glob("fuzz-g3-*.json"))
    if len(found) != 1:
        raise SystemExit(f"prove_checks: results/ needs exactly one fuzz-g3-*.json, has {len(found)}")

    def change(document):
        document["runs"] = [run for run in document["runs"]
                            if (run["target"], run["host"]) != (target, host)]
        document["summary"][host]["targets_clean"] -= 1
    edit_json(found[0].relative_to(TREE), change)


def fuzz_g3_target_missing():
    """Every target of fuzz/CMakeLists.txt on the Mac."""
    drop_g3_run("fuzz_columns", "macOS")


def fuzz_g3_crc_target_missing():
    """The container target on Linux, which reaches the SSE4.2 CRC32C kernel and no SIMD."""
    drop_g3_run("fuzz_container", "Linux")


def hand_edit_results_md():
    edit_text("RESULTS.md", "**macOS.**", "**macOS, hand edited.**")


def edit_table_cells(path, prefix, cells):
    """One table row of a tracked file, found by how it starts, with some cells replaced, so a
    re-measure that changes the figures cannot strand the fault."""
    target = TREE / path
    lines = target.read_text().split("\n")
    found = [index for index, line in enumerate(lines) if line.startswith(prefix)]
    if not found:
        raise RuntimeError(f"{path} has no row starting {prefix!r}")
    row = lines[found[0]].strip("|").split("|")
    for column, value in cells.items():
        if row[column].strip() == value:
            raise RuntimeError(f"{path}: {prefix!r} already holds {value!r}")
        row[column] = f" {value} "
    lines[found[0]] = "|" + "|".join(row) + "|"
    target.write_text("\n".join(lines))


def roundtrip_exit_codes_typed():
    """12302019's round-trip file in the form the assembler of 1669ae0 wrote: G-2's two exit
    codes typed as summaries and no exit code on any run. The three committed files of that
    form were taken by their bytes until the freeze replaced them; every file now carries each
    run's exit code (reviews E-1 and RG-5)."""
    def change(document):
        runs = [document["g1"]["ingest"], document["g1"]["verify"]]
        runs += [document["g2"][name] for name in ("data_crc", "data_no_checksums", "footer_crc",
                                                   "footer_no_checksums", "restored")]
        for run in runs:
            run.pop("exit_code", None)
    edit_json("results/roundtrip-12302019.json", change)


def hand_edit_targets_value():
    """WT-1 on Linux in RESULTS.md's Targets block, its figure raised past the target and the
    verdict turned to met."""
    edit_table_cells("RESULTS.md", "| WT-1 | Linux |", {3: "9.000 M records/s", 4: "met"})


def hand_edit_compare_value():
    """tick-store's Q2 warm median in RESULTS.md's first comparison table, the Linux full day."""
    edit_table_cells("RESULTS.md", "| Q2 | tick-store |", {4: "1.00"})


def hand_edit_proofs_paragraph():
    """The README's proofs paragraph with words typed into it by hand."""
    edit_text("README.md", "runs, made at source commit", "runs in all, made at source commit")


def hand_edit_roundtrip_sentence():
    """The README's sentence on the round trip's times with words typed into it (review P1-3)."""
    edit_text("README.md", " records all four.", " records all four of them.")


def hand_edit_fuzz_runs_table():
    """RESULTS.md's table of G-3's runs, one run's executions typed by hand (review P1-5)."""
    document = json.loads((TREE / "results" / "fuzz-g3-8616263b7561.json").read_text())
    run = document["runs"][0]
    edit_text("RESULTS.md", f"| {run['runs']:,} | {run['exec_per_second']:,} |",
              f"| {run['runs'] + 1:,} | {run['exec_per_second']:,} |")


def hand_edit_headline_value():
    """The README's headline, its one-thread ingest typed past the bar and the verdict met."""
    edit_table_cells("README.md", "| One-thread ingest |", {2: "9.000 M messages/s", 3: "met"})


def hand_edit_pitch_figure():
    """The README's pitch, its store's size raised by hand (review E-9)."""
    document = json.loads((TREE / "results" / "store-size-12302019.json").read_text())
    stored = document["stored_bytes"] * 1e-9
    edit_text("README.md", f"in {stored:.2f} GB, down from", f"in {stored + 1:.2f} GB, down from")


def reword_the_writeup_template():
    """The essay's template reworded and WRITEUP.md not rendered again. The page is rendered
    once every results file it reads is committed; until then writeup-check is not judged, so
    this fault's baseline does not pass and the prover records it as such."""
    edit_text("docs/writeup/WRITEUP.template.md", "gives every message back bit-exact",
              "gives every message back bit-exact, by hand")


def move_a_committed_footer_flip():
    """The first round-trip file that records G-2 with its footer flip one byte back, onto the
    CRC32C byte before it, its seed unchanged (review TD-12)."""
    found = sorted(path for path in (TREE / "results").glob("roundtrip-*.json")
                   if "g2" in json.loads(path.read_text()))
    if not found:
        raise SystemExit("prove_checks: no results/roundtrip-*.json records G-2")

    def back(document):
        document["g2"]["injected"]["footer"]["store_byte_offset"] -= 1
    edit_json(found[0].relative_to(TREE), back)


def swap_pitch_store_sizes():
    """The README's pitch with the source's size and the store's in each other's places, both
    still figures the results give (review RG-4)."""
    document = json.loads((TREE / "results" / "store-size-12302019.json").read_text())
    source, stored = (f"{document[key] * 1e-9:.2f} GB" for key in ("source_bytes",
                                                                   "stored_bytes"))
    edit_text("README.md", f"in {stored}, down from {source}", f"in {source}, down from {stored}")


def reword_a_pitch_claim():
    """The pitch's query sentence reworded to rank tick-store first on short windows too, in
    its clause on Q1 and Q4, which holds no figure for the figure check to catch (reviews
    RG-13, DX-2)."""
    edit_text("README.md", "; ArcticDB or QuestDB wins short", "; tick-store wins short")


def type_an_unchecked_pitch_figure():
    """A figure typed into the README's pitch that no row of its check covers (review E-9)."""
    # The pitch sits at its word limit, so the figure takes a word's place.
    edit_text("README.md", "It gives every message back", "It gives 42 messages back")


def give_a_pitch_figure_without_its_mark():
    """The pitch quoting ArcticDB's warm hour with a table per stock again, a figure from a
    timed phase in which the records list two remote logins, bare, its claim and its figure's
    format reworded to match, as the pitch read before review R1-4."""
    def change(document):
        figure = document["pitch"]["figures"][1]
        figure["values"].append({"mul": [{"path": [
            "systems", "arcticdb", "variants", "rows100k-cols127-per-security", "queries", "Q2",
            "warm", "median_us"]}, 0.001]})
        figure["format"] += ", ArcticDB with a table per stock in {:.2f} ms"
        for claim in document["pitch"]["claims"]:
            claim["text"] = claim["text"].replace(
                "though ArcticDB with a table per stock is faster",
                "ArcticDB with a table per stock in 44.58 ms")
    edit_json("scripts/targets.json", change)
    edit_text("README.md", "though ArcticDB with a table per\nstock is faster",
              "ArcticDB with a table per\nstock in 44.58 ms")


def claim_every_event_is_marked():
    """The sentence under the README's table saying again that every job or login in a timed
    step is marked, as it read before review R1-15, which the records' unmarked runs belie."""
    def change(document):
        under = document["headline"]["under"]
        start = under["text"].index(" A verdict is marked where")
        under["text"] = (under["text"][:start].rstrip(".") + "; where the logs show one of those "
                         "jobs, or a remote login, running during a timed step, the result says "
                         "so.")
    edit_json("scripts/targets.json", change)


def drop_a_pitch_claims_tie():
    """The claim that ties the pitch's one-thread ingest, below Nasdaq's peak, to its verdict
    taken out of scripts/targets.json, so the sentence ranks with nothing to hold it to."""
    def change(document):
        document["pitch"]["claims"] = [claim for claim in document["pitch"]["claims"]
                                       if "Nasdaq's published peak" not in claim["text"]]
    edit_json("scripts/targets.json", change)


def show_the_fuzz_row():
    """The fuzz target added to the headline's table, as the proposal for the first screen
    considered, and named by its id on the screen. When this fault was made the row's verdict
    cell, written from the results, named the id; it no longer does, and no headline row or
    sentence under the table may type a digit, so the row alone took no rule of the screen's and
    the render stopped only at a stale README (review R1-7). The id stands in the pitch here,
    declared among its texts that are no result, the one place on the screen where words with a
    digit may still stand, taking a word's place since the pitch sits at its word limit."""
    def change(document):
        document["headline"]["rows"].append({
            "id": "G-3", "host": "any", "what": "Fuzzing",
            "bar": "every target clean for half an hour", "source": "the project's gate",
            "measured": "{clean} targets clean",
            "values": {"clean": {"value": {"path": ["summary", "macOS", "targets_clean"]},
                                 "format": "{:,}"}}})
        document["pitch"]["not_results"].append({"text": "G-3", "why": "the fuzz gate's id"})
    edit_json("scripts/targets.json", change)
    edit_text("README.md", "It stores a day's", "G-3 stores a day's")


def call_a_host_by_its_system():
    """The pitch calling the Linux host "Linux" again, in its figure sentence, which no claim
    ties, so the screen's own rule is the one that stops it; the word takes another's place,
    since the pitch sits at its word limit."""
    edit_text("README.md", "It stores a day's", "Linux stores a day's")


def drop_the_moves_unknown_cause():
    """BENCHMARKS.md's move of ArcticDB's class B write between the runs with its unknown cause
    taken out, as the pages read before review C2."""
    edit_text("BENCHMARKS.md", "; the cause of that move was not measured and is not known.", ".")


def widen_what_the_replaced_record_lacks():
    """BENCHMARKS.md's sentence on what the replaced run's record keeps as review C2 found it,
    saying it keeps no residency figure for any system but tick-store."""
    edit_text("BENCHMARKS.md", "The replaced run's record keeps, of the class B writes,",
              "The replaced run's record keeps no residency or device-read figure for any system "
              "but tick-store, and, of the class B writes,")


def quote_wt4_with_the_old_word():
    """A WT-4 quote on BENCHMARKS.md as the review found its row: the ArcticDB part under
    "missed" and its outcome on this run's figures in a later clause, not the mark RESULTS.md
    gives (review C1, E). The page now links to RESULTS.md for every verdict, so the quote is
    put back beside that link (review P1-22)."""
    edit_text("BENCHMARKS.md", "Every verdict is in [RESULTS.md](RESULTS.md#targets).",
              "Every verdict is in [RESULTS.md](RESULTS.md#targets). WT-4 on Linux is met against "
              "QuestDB only, not like-for-like; missed against Parquet and ArcticDB; against "
              "ArcticDB met on this run's figures and missed on the replaced run's, a change that "
              "favours tick-store, so counted missed.")


def drop_the_readme_input_note():
    """The README's comparison section as the review found it, with no word of where each
    system's write input was (review C1, condition 2)."""
    edit_text("README.md", "Write inputs are not alike on this day", "Write inputs differ")


def headline_row(document, what):
    return next(row for row in document["headline"]["rows"] if row["what"] == what)


def type_a_figure_into_a_template():
    """The disk row's measured template with Parquet's size typed in, not read."""
    def change(document):
        row = headline_row(document, "Disk, store flushed (written through to the drive)")
        row["measured"] = row["measured"].replace("{parquet}", "3.19")
    edit_json("scripts/targets.json", change)


def give_a_template_a_value_it_does_not_name():
    """The one-thread ingest row given its flushed rate, which its template never shows."""
    def change(document):
        headline_row(document, "One-thread ingest")["values"]["flushed"] = {
            "value": {"path": ["systems", "tickstore", "variants", "@headline", "classes", "B",
                               "write", "records_per_second_median"]},
            "scale": 1e-6, "format": "{:.3f}"}
    edit_json("scripts/targets.json", change)


def drop_a_rows_source():
    """The one-thread ingest row without the source of its bar."""
    edit_json("scripts/targets.json",
              lambda document: headline_row(document, "One-thread ingest").pop("source"))


def drop_headline_markers():
    target = TREE / "README.md"
    lines = target.read_text().split("\n")
    kept = [line for line in lines if not line.startswith(("<!-- headline:begin",
                                                           "<!-- headline:end"))]
    if len(kept) != len(lines) - 2:
        raise RuntimeError("README.md does not hold one pair of headline markers")
    target.write_text("\n".join(kept))


def readme_keeps_the_targets_block():
    """The README with a copy of RESULTS.md's Targets block, markers and all, as if the move
    had left the old block behind: a copy --check would otherwise never compare."""
    text = (TREE / "RESULTS.md").read_text()
    begin = text.index("<!-- targets:begin")
    end = text.index("<!-- targets:end -->") + len("<!-- targets:end -->")
    readme = TREE / "README.md"
    readme.write_text(readme.read_text() + "\n" + text[begin:end] + "\n")


def headline_without_a_miss():
    """The headline cut to the gates G-1 and G-3, both met, so it shows no missed target."""
    def change(document):
        document["headline"]["rows"] = [row for row in document["headline"]["rows"]
                                        if row["id"].startswith("G-")]
    edit_json("scripts/targets.json", change)


def break_workflow():
    edit_text(".github/workflows/ci.yml", "runs-on: ${{ matrix.host.runner }}",
              "runs-on: ${{ matrix.host.no_such_key }}")


def inject_home_path():
    add_file("notes-injected.txt", b"see /" + b"Users/someuser/data\n")


def inject_orphan_source():
    add_file("src/orphan.cpp", b"int orphan() { return 1; }\n")


def empty_the_results_folder():
    for path in (TREE / "results" / "bench").glob("*.json"):
        path.unlink()


def inject_data_file():
    add_file("12302019.NASDAQ_ITCH50", b"\x00\x0cS")


@dataclass(frozen=True)
class Fault:
    id: str
    check: str
    what: str
    expected_exit: int
    expected_string: str
    mutate: object = None
    # Narrows the step to what the fault is about, for the baseline and the fault alike.
    arguments: tuple = ()
    # Arguments for the faulted run only.
    fault_arguments: tuple = ()
    # A step that must still run and pass after the faulted one.
    later_step: str = ""
    # The fault must pass: a dependency's warning stays out of our build.
    tolerated: bool = False
    # A control: nothing is broken, so the prover must report it as undetected.
    control: bool = False


FAULTS = (
    Fault("unit-field-swap", "dev-test", "OrderReplace's two reference numbers swapped in the "
          "table the decoder is generated from", EXIT_CTEST, "ItchGolden.OrderReplace"),
    Fault("golden-reserved-byte", "dev-test", "the encoder drops the reserved byte of a stock "
          "trading action", EXIT_CTEST, "ItchGolden.StockTradingAction"),
    # Reviews RL-1 and RL-2: a kernel case's pass is sized from the fastest of three rounds.
    Fault("kernel-pass-sized-from-one-round", "dev-test", "the kernel bench sizes a case's "
          "pass from one timed round, so one slow round shortens every pass of the case",
          EXIT_CTEST, "KernelPassSizing.APassIsSizedFromTheFastestOfThreeRounds",
          arguments=("--test-regex", "KernelPassSizing")),
    Fault("asan-frame-overread", "asan-test", "the frame cursor reads its length prefix before "
          "checking that two bytes remain, one byte past a heap buffer", EXIT_CTEST,
          "heap-buffer-overflow"),
    Fault("ubsan-shift-64", "asan-test", "the big-endian reader shifts an 8-byte value by 64",
          EXIT_CTEST, "shift exponent 64 is too large"),
    Fault("asan-bit-reader-bound", "asan-test", "the codecs' bit reader stops checking a read "
          "against the bits left, so a cut block is read past its end", EXIT_CTEST,
          "heap-buffer-overflow"),
    # LeakSanitizer is off by default under ASan on macOS, where these two once passed.
    Fault("asan-leak-trailing-bytes", "asan-test", "the strict decoder leaks two sizes each "
          "time it refuses trailing bytes", EXIT_CTEST,
          "ERROR: LeakSanitizer: detected memory leaks",
          arguments=("--test-regex", "ItchTruncation")),
    Fault("fuzz-itch-message-leak", "fuzz-itch-message", "the decoder leaks a copy of each "
          "unknown type byte it refuses", EXIT_LIBFUZZER,
          "ERROR: LeakSanitizer: detected memory leaks", arguments=FUZZ_ARGUMENTS),
    Fault("tsan-queue-race", "tsan-test", "the bounded queue pushes after releasing its lock",
          EXIT_CTEST, "ThreadSanitizer: data race"),
    Fault("tidy-c-style-cast", "clang-tidy", "a C-style cast in src/itch/frame.cpp", CHECK_FAILED,
          "[modernize-avoid-c-style-cast", arguments=TIDY_ONE_FILE),
    Fault("tidy-file-in-no-build", "clang-tidy", "a tracked source no compile database holds, "
          "which clang-tidy would otherwise never see", CHECK_FAILED,
          "no compile database holds src/orphan.cpp", inject_orphan_source,
          arguments=TIDY_ONE_FILE, fault_arguments=("--tidy-files", "src/orphan.cpp")),
    Fault("werror-unused-variable", "dev-build", "an unused variable in our code", EXIT_BUILD,
          "-Werror,-Wunused-variable"),
    Fault("werror-dependency-tolerated", "dev-build", "unused variables in GoogleTest, in a "
          "source file and in a header our tests include, patched into its fetched copy under "
          "build/_deps/, so the patch applies only once a configure has fetched it", 0,
          "unused variable 'never_used_in_source'", tolerated=True),
    Fault("fuzz-itch-message-trailing", "fuzz-itch-message", "the strict decoder stops refusing "
          "trailing bytes", EXIT_LIBFUZZER, "check_message:", arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-itch-frame-lost-byte", "fuzz-itch-frame", "the cursor reports a clean end with "
          "one byte left", EXIT_LIBFUZZER, "check_stream:", arguments=FUZZ_ARGUMENTS),
    # Review RR-3: the frame oracle's check of a shorter input's stop (RA-2), on an input whose
    # whole stream ends cleanly, so no other check sees the misstated stop.
    Fault("frame-short-stop-misstated", "dev-test", "the cursor states a truncated prefix with "
          "one byte more left than there is", EXIT_CTEST,
          "FuzzOracles.TheFrameOracleChecksTheShorterInputsStop",
          arguments=("--test-regex", "FuzzOracles")),
    Fault("fuzz-stub-store-record-count", "fuzz-stub-store", "the parser stops comparing the "
          "record count with the records it read", EXIT_LIBFUZZER, "check_store:",
          arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-columns-text-bound", "fuzz-columns", "the column reader stops checking a text "
          "offset against its file", EXIT_LIBFUZZER, "AddressSanitizer: heap-buffer-overflow",
          arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-gorilla-timestamps-bucket-check", "fuzz-gorilla-timestamps", "the timestamp "
          "decoder stops refusing a delta-of-delta stored in a wider bucket than it needs",
          EXIT_LIBFUZZER, "check_timestamp_column:", arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-gorilla-xor-leading-cap", "fuzz-gorilla-xor", "the XOR encoder caps the leading "
          "zero count at 32, one past what its 5 bits hold", EXIT_LIBFUZZER, "check_xor_column:",
          arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-pfor-zigzag-order", "fuzz-pfor", "the PFOR decoder zigzag-decodes a vector's "
          "offsets before adding its base, not after", EXIT_LIBFUZZER, "check_pfor_column:",
          arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-stream-vbyte-length-check", "fuzz-stream-vbyte", "the Stream VByte decoder "
          "accepts a value stored in more bytes than it needs", EXIT_LIBFUZZER,
          "check_svb_column:", arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-svb-neon-canonical-dropped", "fuzz-stream-vbyte", "the NEON Stream VByte "
          "kernel stops refusing a value stored in more bytes than it needs, which only the "
          "kernel-against-scalar oracle sees", EXIT_LIBFUZZER,
          "check_svb_column: the kernel set and the scalar set decoded a block differently",
          arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-pfor-head-delta-escape", "fuzz-pfor", "the head-delta inverse puts an escaped "
          "value over the front head instead of pushing the heads down", EXIT_LIBFUZZER,
          "check_pfor_column:", arguments=FUZZ_ARGUMENTS),
    # The fuzz oracle's bounds (fuzz/README.md, "Bounds"): a value inside the cross-check
    # budget still meets the re-encodes in the fuzz target; one past it, which the target no
    # longer re-encodes, meets them in the unit test on the named costliest seed; the
    # decoder-side run-count check, never budgeted, still sees a block past the budget; a
    # kernel fault in a block past the cap meets the kernel-against-scalar comparison the
    # target still makes there; and one in a block no -max_len holds meets the unit grid.
    Fault("pfor-encoder-value-in-budget", "fuzz-pfor", "the PFOR encoder changes value 5,000 "
          "of a delta-of-delta, scaled block, inside the fuzz target's cross-check budget",
          EXIT_LIBFUZZER,
          "check_pfor_column: the re-encoded block did not decode to the same values",
          arguments=FUZZ_ARGUMENTS),
    Fault("pfor-encoder-value-past-budget", "dev-test", "the PFOR encoder changes value 100,000 "
          "of a delta-of-delta, scaled block, past the fuzz target's cross-check budget",
          EXIT_CTEST, "BoundSeeds.PassTheWholeCheck/pfor (Failed)",
          arguments=("--test-regex", "BoundSeeds")),
    Fault("rle-run-count-check-past-budget", "fuzz-run-length", "the run-length decoder stops "
          "refusing a run value equal to the one before, which the named seed holds at run "
          "100,000, past the cross-check budget", EXIT_LIBFUZZER,
          "check_rle_column: a run-length block's run count is not its count of maximal runs",
          arguments=FUZZ_ARGUMENTS),
    Fault("neon-delta-pass-past-cap", "fuzz-pfor", "the NEON delta pass changes the first "
          "value of the 129th and each later vector of a block, in the named seed's block past "
          "the cap", EXIT_LIBFUZZER,
          "check_pfor_column: past the cap, the kernel set and the scalar set decoded a block "
          "differently",
          arguments=FUZZ_ARGUMENTS),
    Fault("neon-unpack-past-max-len", "dev-test", "the NEON vector unpack changes a value once "
          "a block's full vectors pass 131,072 packed bytes, more than any -max_len holds",
          EXIT_CTEST, "OracleGrid.PassTheWholeCheck/ranked_pfor_none_scaled_1048576 (Failed)",
          arguments=("--test-regex", "OracleGrid.PassTheWholeCheck/ranked_pfor_none_scaled_1048576$")),
    # Past the cap the chosen kernel set decodes a block from an exact copy of its bytes, so a
    # read past the block is seen where the input's next bytes would hide it; and a refused
    # block there must leave the output alone.
    Fault("neon-unpack-delta-overread-past-cap", "fuzz-pfor", "the NEON delta pass loads 16 "
          "bytes from where the vector's packed bytes end, from the 129th vector of a delta "
          "block on, which at the last vector of the named seed's block past the cap is past "
          "the block (NEON does not fuse the unpack and the delta pass, so the fault is in the "
          "pass it runs)", EXIT_LIBFUZZER, "AddressSanitizer: heap-buffer-overflow",
          arguments=FUZZ_ARGUMENTS),
    Fault("pfor-refusal-keeps-values-past-cap", "fuzz-pfor", "the PFOR decoder keeps the values "
          "it decoded of a refused block of more than 131,072 values, as the named seed's block "
          "past the cap is", EXIT_LIBFUZZER,
          "check_pfor_column: past the cap, a refused block changed the output",
          arguments=FUZZ_ARGUMENTS),
    # Past the dictionary's cap the compare tier reaches a maximal block, and past run-length's
    # it reaches the blocks after the first.
    Fault("dict-mtf-chosen-set-past-2p19", "fuzz-dictionary", "under the chosen kernel set, not "
          "the scalar one, a move-to-front block of more than 2^19 values decodes its last value "
          "to the entry beside it, as the named seed's maximal block past the cap is",
          EXIT_LIBFUZZER,
          "check_dict_column: past the cap, the kernel set and the scalar set decoded a block "
          "differently",
          arguments=FUZZ_ARGUMENTS),
    Fault("rle-overread-later-block", "fuzz-run-length", "the run-length decoder reads the byte "
          "after a maximal block of fewer runs than values, as the named seed's second block, "
          "past the cap, is", EXIT_LIBFUZZER, "AddressSanitizer: heap-buffer-overflow",
          arguments=FUZZ_ARGUMENTS),
    # The cap counts the values before a block twice, as the wrong-count check decodes every
    # block but the last again: the oracle's own cap test, and the tiers the named seeds of
    # small blocks are held to.
    Fault("codec-cap-measure-unused", "dev-test", "the codec oracle's cap counts the values "
          "taken before a block once, as the values it adds are", EXIT_CTEST,
          "FuzzOracles.TheCapCountsTheValuesBeforeABlockTwice (Failed)",
          arguments=("--test-regex", "FuzzOracles.TheCapCountsTheValuesBeforeABlockTwice$")),
    Fault("codec-cap-counts-values-before-once", "dev-test", "the measure the codec oracle's "
          "cap and the unit test's tiers share counts the values before a block once, so the "
          "run-length named seed's small block falls within the cap", EXIT_CTEST,
          "BoundSeeds.PassTheWholeCheck/run_length_small_blocks (Failed)",
          arguments=("--test-regex", "BoundSeeds.PassTheWholeCheck/run_length_small_blocks$")),
    # Past the budget the re-encodes take a prefix of each of the next few blocks, and stop.
    Fault("codec-prefix-past-budget-dropped", "dev-test", "the codec oracle's re-encodes take "
          "no prefix of the blocks past the budget", EXIT_CTEST,
          "FuzzOracles.TheReEncodesTakeTheBudgetThenAPrefixOfTheNextBlocks (Failed)",
          arguments=("--test-regex", "FuzzOracles.TheReEncodesTakeTheBudgetThenAPrefixOfTheNextBlocks$")),
    Fault("codec-prefix-past-budget-unbounded", "dev-test", "the codec oracle's re-encodes take "
          "a prefix of every block past the budget, not only of the next few", EXIT_CTEST,
          "FuzzOracles.TheReEncodesTakeTheBudgetThenAPrefixOfTheNextBlocks (Failed)",
          arguments=("--test-regex", "FuzzOracles.TheReEncodesTakeTheBudgetThenAPrefixOfTheNextBlocks$")),
    # The count table (fuzz/README.md, "Bounds"): its numbers held to the constants, and the
    # named seeds' counts to its numbers.
    Fault("codec-prefix-constant-moved-without-the-table", "dev-test", "the prefix past the "
          "budget is halved and the count table is not moved with it", EXIT_CTEST,
          "FuzzOracles.TheCountTableHoldsTheConstants (Failed)",
          arguments=("--test-regex", "FuzzOracles.TheCountTableHoldsTheConstants$")),
    Fault("codec-compare-past-its-limit", "dev-test", "the codec oracle compares one maximal "
          "block past its compare limit", EXIT_CTEST,
          "BoundSeeds.CountWhatTheTableGivesThem/pfor (Failed)",
          arguments=("--test-regex", "BoundSeeds.CountWhatTheTableGivesThem/pfor$")),
    Fault("store-cap-at-the-compare-limit", "dev-test", "the store oracle takes the whole check "
          "up to its compare limit", EXIT_CTEST,
          "BoundSeeds.CountWhatTheTableGivesThem/itch_store_compared (Failed)",
          arguments=("--test-regex", "BoundSeeds.CountWhatTheTableGivesThem/itch_store_compared$")),
    Fault("query-repeat-ask-every-request", "dev-test", "the query oracle's repeat ask takes "
          "every request past the budget", EXIT_CTEST,
          "BoundSeeds.CountWhatTheTableGivesThem/itch_query (Failed)",
          arguments=("--test-regex", "BoundSeeds.CountWhatTheTableGivesThem/itch_query$")),
    Fault("codec-count-the-cut-as-decoded", "dev-test", "the codec oracle's count takes a cut "
          "block at the values its header states, which the decoder never decodes", EXIT_CTEST,
          "BoundSeeds.CountWhatTheTableGivesThem/run_length (Failed)",
          arguments=("--test-regex", "BoundSeeds.CountWhatTheTableGivesThem/run_length$")),
    Fault("codec-block-examined-twice", "dev-test", "the codec oracle decodes every block in a "
          "first pass before part 1 examines it", EXIT_CTEST,
          "BoundSeeds.CountWhatTheTableGivesThem/pfor_most_blocks (Failed)",
          arguments=("--test-regex", "BoundSeeds.CountWhatTheTableGivesThem/pfor_most_blocks$")),
    Fault("store-each-segment-read-twice", "dev-test", "the store oracle reads each segment on "
          "its own twice", EXIT_CTEST,
          "BoundSeeds.CountWhatTheTableGivesThem/itch_store_most_segments (Failed)",
          arguments=("--test-regex", "BoundSeeds.CountWhatTheTableGivesThem/itch_store_most_segments$")),
    Fault("fuzz-dictionary-repeated-entry", "fuzz-dictionary", "the dictionary decoder stops "
          "refusing a ranked dictionary that holds one value twice", EXIT_LIBFUZZER,
          "check_dict_column:", arguments=FUZZ_ARGUMENTS),
    # The writer holds its hot columns to ranked PFOR too, so the faulted writer stores a
    # one-byte raw record's segment in fewer bytes, 230 of them in -max_len, and the seed
    # generator's most-segments table (219) stops fuzz-build before any fuzzing. The patch moves
    # that table to 230, as a change that updated the table with the encoder would, so the
    # fuzzer is reached and refuses the mode by name.
    Fault("fuzz-dictionary-mode-set-ignored", "fuzz-dictionary", "the dictionary encoder tries "
          "Huffman whatever its mode set allows, so a column held to ranked PFOR can take "
          "Huffman; the patch also moves the seed table's kStoreWriterMostSegments from 219 to "
          "the faulted writer's 230, as a change that updated the table with the encoder would, "
          "so the seed generator does not stop fuzz-build and the fuzzer is reached",
          EXIT_LIBFUZZER,
          "check_dict_column: the encoder held to ranked PFOR took another mode",
          arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-dictionary-mode-set-larger-block", "fuzz-dictionary", "the dictionary encoder "
          "takes Huffman even when it is up to 64 bytes larger than ranked PFOR", EXIT_LIBFUZZER,
          "check_dict_column:", arguments=FUZZ_ARGUMENTS),
    # The writer and the seed generator refuse first, so fuzz-build fails before any fuzzing;
    # the unit test is where the refusal shows by name.
    Fault("dict-pfor-only-refuses", "dev-test", "the dictionary encoder held to "
          "ranked PFOR refuses a block of more than two distinct values", EXIT_CTEST,
          "Dictionary.AModeSetLimitsTheEncoderAlone", arguments=("--test-regex", "Dictionary")),
    Fault("fuzz-run-length-maximal-runs", "fuzz-run-length", "the run-length decoder stops "
          "refusing two neighbouring runs of one value", EXIT_LIBFUZZER, "check_rle_column:",
          arguments=FUZZ_ARGUMENTS),
    # The seed generator takes real store files apart and rebuilds them, so a parser that
    # misreads every valid file stops fuzz-build before any fuzzing, as it does here: the
    # rebuilt metadata names a codec that does not exist. The fault after it is one only a
    # corrupt file shows, which reaches the fuzzer.
    Fault("fuzz-container-metadata-start", "fuzz-build", "the container parser starts the "
          "footer's metadata area one byte past the section entries", EXIT_SEED_GENERATOR,
          "uncaught exception of type std::runtime_error: invalid argument: codec id"),
    Fault("fuzz-container-block-reserved", "fuzz-container", "the container parser stops "
          "refusing a block entry whose reserved field is not zero, so a file the writer cannot "
          "make is accepted", EXIT_LIBFUZZER, "check_container:", arguments=FUZZ_ARGUMENTS),
    Fault("container-block-crc-skipped", "dev-test", "a block lookup stops checking the "
          "block's CRC32C", EXIT_CTEST, "ContainerBlock.RefusesAPayloadWhoseCrcDiffers",
          arguments=("--test-regex", "ContainerBlock")),
    Fault("container-footer-offset-bound", "dev-test", "the trailer's footer offset bound is off "
          "by one, refusing a footer that ends exactly at the trailer", EXIT_CTEST,
          "ContainerOpen.AcceptsTheSmallestFileAndEmptyParts",
          arguments=("--test-regex", "ContainerOpen")),
    Fault("container-directory-sync-dropped", "dev-test", "the writer flushes the directory "
          "after the rename in page_cache mode instead of device_flush mode", EXIT_CTEST,
          "ContainerCommit.EachModeMakesItsCallsInOrder",
          arguments=("--test-regex", "ContainerCommit")),
    Fault("crc32c-armv8-last-byte", "dev-test", "the ARMv8 CRC32C kernel drops an odd last byte",
          EXIT_CTEST, "Crc32c.HardwareMatchesSoftwareOnRandomLengthsAndOffsets",
          arguments=("--test-regex", "Crc32c")),
    Fault("neon-add-base-tail-off-by-one", "dev-test", "the NEON frame-of-reference kernel "
          "starts its scalar tail one value late, so the first value past the whole steps keeps "
          "its offset", EXIT_CTEST, "SimdKernels.AddBaseMatchesScalarOnEveryTail",
          arguments=("--test-regex", "SimdKernels")),
    Fault("g1-store-alters-a-byte", "oracle", "the ITCH store's writer flips the low bit of every "
          "add order's shares, so record 2 re-serialises one bit off", CHECK_FAILED,
          "first difference: bytes at record 2, source byte"),
    Fault("itch-store-import-not-appended", "dev-test", "the ITCH store's writer stops appending "
          "an import to its locate's list, so the distances after it in the segment are off",
          EXIT_CTEST, "ItchStoreReferences.AnOrderFromAnEarlierSegmentIsImportedOnceThenADistance",
          arguments=("--test-regex", "ItchStore")),
    # The fuzzer reaches a distance past its list only by chance: of four runs at seed 1, one
    # found it at 43 s and three did not, at 45 s and twice at 240 s. The unit test that sets
    # one is what reliably fails, so the fault is proven there.
    Fault("store-ref-distance-bound", "dev-test", "the ITCH store's segment decoder stops "
          "checking a reference distance against its locate's list", EXIT_CTEST,
          "ItchStoreCorrupt.ValuesThatBreakTheRulesTyingColumnsTogether",
          arguments=STORE_CORRUPT),
    # fuzz_itch_store fails on what changes an accepted file's round trip: a field misread
    # comes back misread twice through the writer, on the first seeds.
    Fault("fuzz-itch-store-tracking-misread", "fuzz-itch-store", "the ITCH store's segment "
          "decoder reads every tracking number one high", EXIT_LIBFUZZER,
          "check_itch_store: the rewritten file reads back differently",
          arguments=FUZZ_ARGUMENTS),
    Fault("query-window-group-boundary", "dev-test", "the query's time zone map treats a seek "
          "group whose last time equals the window's start as before the window, an off-by-one "
          "at the time boundary", EXIT_CTEST, "ItchQuery.BothPathsEqualTheColumnFormAtEveryBoundary",
          arguments=("--test-regex", "ItchQuery\\.")),
    Fault("query-run-order-unchecked", "dev-test",
          "the run directory check stops refusing "
          "locate run ids that fall, refusing only a repeated one", EXIT_CTEST,
          "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery", arguments=QUERY_CORRUPT),
    Fault("store-piece-index-next", "dev-test",
          "the reader stops checking that a column's next piece "
          "carries the next piece index",
          EXIT_CTEST, "ItchStoreCorrupt.PieceFields",
          arguments=STORE_CORRUPT),
    Fault("store-piece-value-count", "dev-test",
          "the reader accepts a piece before a column's last that "
          "holds fewer than 16,384 values",
          EXIT_CTEST, "ItchStoreCorrupt.PieceFields",
          arguments=STORE_CORRUPT),
    Fault("store-segment-records-sum", "dev-test",
          "the reader stops comparing the type column's pieces "
          "together with the summary's records",
          EXIT_CTEST, "ItchStoreCorrupt.PieceFields",
          arguments=STORE_CORRUPT),
    Fault("store-metadata-records", "dev-test",
          "the summary parser accepts segment records below its "
          "own piece's values",
          EXIT_CTEST, "ItchStoreCorrupt.PieceFields",
          arguments=STORE_CORRUPT),
    Fault("store-run-start-floor", "dev-test",
          "the run directory check accepts a run starting where the "
          "run before it starts",
          EXIT_CTEST, "ItchStoreCorrupt.LocateRunFields",
          arguments=STORE_CORRUPT),
    Fault("store-run-add-start-records", "dev-test",
          "the segment decoder stops comparing a run's add start "
          "with the adds stored before it",
          EXIT_CTEST, "ItchStoreCorrupt.LocateRunFields",
          arguments=STORE_CORRUPT),
    Fault("store-run-index-repeated", "dev-test",
          "the locate column's decoder accepts a file position "
          "named by two records",
          EXIT_CTEST, "ItchStoreCorrupt.LocateRunFields",
          arguments=STORE_CORRUPT),
    Fault("store-time-base-unchecked", "dev-test",
          "the segment decoder stops comparing the stored time "
          "bases with the ones the restored times give",
          EXIT_CTEST, "ItchStoreCorrupt.TimeBaseFields",
          arguments=STORE_CORRUPT),
    Fault("store-time-past-48-bits", "dev-test",
          "times restored from residuals are no longer checked "
          "against 48 bits",
          EXIT_CTEST, "ItchStoreCorrupt.TimeBaseFields",
          arguments=STORE_CORRUPT),
    Fault("store-typed-after-raw", "dev-test",
          "the segment decoder accepts a typed record stored "
          "after a raw one",
          EXIT_CTEST, "ItchStoreCorrupt.ATypedRecordStoredAfterARawOneIsRefused",
          arguments=STORE_CORRUPT),
    Fault("store-seek-file-max-time", "dev-test",
          "the seek index check accepts a file group's greatest "
          "time past 48 bits",
          EXIT_CTEST, "ItchStoreCorrupt.SeekIndexFields",
          arguments=STORE_CORRUPT),
    Fault("store-seek-file-min-time", "dev-test",
          "the seek index check accepts a file group's least "
          "time above its greatest",
          EXIT_CTEST, "ItchStoreCorrupt.SeekIndexFields",
          arguments=STORE_CORRUPT),
    Fault("query-run-id-unchecked", "dev-test",
          "the query takes the run at a locate's rank without "
          "checking that the run is the locate's",
          EXIT_CTEST, "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery",
          arguments=QUERY_CORRUPT),
    Fault("query-run-start-unchecked", "dev-test",
          "the query stops checking a run's start and end "
          "against each other and the typed records",
          EXIT_CTEST, "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery",
          arguments=QUERY_CORRUPT),
    Fault("query-add-start-unchecked", "dev-test",
          "the query stops comparing a run's add start with the "
          "adds of the records before the run",
          EXIT_CTEST, "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery",
          arguments=QUERY_CORRUPT),
    Fault("query-run-adds-unchecked", "dev-test",
          "the query stops checking a run's adds against its add "
          "starts before reading their sides",
          EXIT_CTEST, "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery",
          arguments=QUERY_CORRUPT),
    Fault("query-import-start-unchecked", "dev-test",
          "the query reads a run's imports past the run's "
          "import end",
          EXIT_CTEST, "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery",
          arguments=QUERY_CORRUPT),
    Fault("query-run-index-unchecked", "dev-test",
          "the query forms a file position from a gap without "
          "checking it against the records",
          EXIT_CTEST, "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery",
          arguments=QUERY_CORRUPT),
    Fault("query-time-past-48-bits", "dev-test",
          "the query stops checking a restored time against 48 "
          "bits",
          EXIT_CTEST, "ItchQueryCorrupt.LocateRunFieldsRefusedByTheQuery",
          arguments=QUERY_CORRUPT),
    Fault("query-cli-digest-ts", "dev-test", "the query worker's row digest puts every ts one "
          "nanosecond late", EXIT_CTEST, "QueryCli.ServesEveryKindWithTheHarnessDigests",
          arguments=("--test-regex", "QueryCli")),
    Fault("fuzz-itch-query-first-run-skipped", "fuzz-itch-query", "the symbol filter skips a "
          "locate's run when it is the segment's first, so its rows go missing", EXIT_LIBFUZZER,
          "check_itch_query:", arguments=FUZZ_ARGUMENTS),
    # Review RR-3: the query oracle's column form reads the type byte itself (RD-6), so a raw
    # row mapped wrong by row_values() no longer moves all three answers alike.
    Fault("query-raw-row-type-zeroed", "dev-test", "row_values() gives a raw row the type byte "
          "0", EXIT_CTEST, "FuzzOracles.TheQueryOracleSeesASegmentBothPathsWouldSkip",
          arguments=("--test-regex", "FuzzOracles")),
    Fault("g2-weakened-compare", "dev-test", "the oracle's compare ignores bit 0 and stops "
          "requiring the hashes to match", EXIT_CTEST, "Value of: report.passed()",
          arguments=("--test-regex", "OracleExhaustiveFlip")),
    Fault("data-sha256-removed", "py-scripts", "fetch_data.py stops comparing SHA-256 digests",
          CHECK_FAILED, "FAIL: test_one_flipped_byte (__main__.CompressedHash"),
    Fault("taskset-removed", "py-scripts", "run_bench.py drops the taskset -c 4 pin from a "
          "Linux case's command", CHECK_FAILED,
          "FAIL: test_a_linux_case_records_its_siblings_busy_share (__main__.RunBenchTest"),
    Fault("sibling-meter-whole-uptime", "py-scripts", "run_bench.py's sibling meter divides "
          "a case's busy jiffies by every jiffy since boot, so a busy sibling reads near 0% and "
          "passes the validator's 5% rule", CHECK_FAILED,
          "FAIL: test_a_linux_document_meets_the_validators_sibling_rule (__main__.RunBenchTest"),
    # Review E-10: boost is stated only if it read the same before and after the run.
    Fault("turbo-read-after-the-run-only", "py-scripts", "run_bench.py states the boost it "
          "reads after the last case, though it read another before the first", CHECK_FAILED,
          "FAIL: test_turbo_that_changed_across_the_run_is_not_stated (__main__.RunBenchTest"),
    # DS-4's rule keeps every attempt's document: run_bench.py writes it before validating.
    Fault("run-bench-refused-document-not-kept", "py-scripts", "run_bench.py writes the copy "
          "--keep-document asks for only after the validator has passed the document, so a "
          "refused attempt's document is lost", CHECK_FAILED,
          "FAIL: test_a_refused_document_is_kept_where_asked_and_still_never_published "
          "(__main__.RunBenchTest"),
    # The helpers that make published inputs read what they state, and publish no machine's own.
    Fault("build-preset-option-defaults-ignored", "py-scripts", "build_preset.py compares only "
          "the variables a preset sets, so a dev build configured with the benchmarks on is "
          "named dev", CHECK_FAILED,
          "FAIL: test_a_cache_no_preset_configures_is_refused (__main__.BuildPreset"),
    Fault("codec-eval-preset-assumed", "py-scripts", "codec_eval_results.py writes "
          "build_preset as bench whatever the build folder's cache says", CHECK_FAILED,
          "FAIL: test_the_preset_is_read_from_the_build_folder_not_assumed "
          "(__main__.CodecEvalResults"),
    Fault("codec-eval-build-type-unchecked", "py-scripts", "codec_eval_results.py takes a "
          "build folder whose build type is not the one the tool states it was built with",
          CHECK_FAILED, "FAIL: test_a_tool_built_otherwise_than_the_folder_is_refused "
          "(__main__.CodecEvalResults"),
    Fault("store-results-preset-assumed", "py-scripts", "store_results.py writes the "
          "round-trip and store-size files' build_preset as bench whatever the build folder's "
          "cache says", CHECK_FAILED,
          "FAIL: test_the_preset_is_read_from_the_build_folder_not_assumed "
          "(__main__.StoreResults"),
    Fault("store-results-unconfigured-build-taken", "py-scripts", "store_results.py takes a "
          "build folder no preset configured, or one with no cache, and writes it as bench",
          CHECK_FAILED, "FAIL: test_a_build_folder_no_preset_configures_is_refused "
          "(__main__.StoreResults"),
    Fault("g2-plan-footer-trailer-counted", "py-scripts", "g2_plan.py counts the trailer as "
          "footer, so the footer flip is not the footer's middle byte", CHECK_FAILED,
          "FAIL: test_the_committed_days_flips_follow_from_its_layout (__main__.G2Plan"),
    # docs/REVIEW.md, G-2 footer flip: the final run's footer flip fell in a CRC32C. The rule
    # moves the flip to the first byte the reader reads with checksums off, and the oracle
    # step's sweep of every footer byte of a real store holds it to the tool's verdicts.
    Fault("g2-plan-crc-move-removed", "py-scripts", "g2_plan.py flips the footer's middle byte "
          "even when it falls in a CRC32C, which nothing reads with checksums off", CHECK_FAILED,
          "FAIL: test_a_middle_byte_in_a_block_entrys_crc_moves_to_the_byte_after_it "
          "(__main__.G2Plan"),
    Fault("g2-plan-move-one-short", "py-scripts", "g2_plan.py moves a footer flip out of a "
          "CRC32C one byte short, onto the field's last byte", CHECK_FAILED,
          "FAIL: test_from_every_footer_byte_the_rule_flips_one_verify_caught "
          "(__main__.FooterSweep"),
    Fault("g2-plan-section-crc-classed-read", "py-scripts", "g2_plan.py classes a section "
          "entry's CRC32C as read with checksums off, which the real store's sweep shows it "
          "is not", CHECK_FAILED,
          "FAIL: test_every_byte_the_rule_reads_was_caught_and_every_byte_it_skips_was_not "
          "(__main__.FooterSweep"),
    Fault("g2-plan-no-read-byte-unchecked", "py-scripts", "g2_plan.py plans a footer flip past "
          "the footer's end when no byte at or after its middle is read", CHECK_FAILED,
          "FAIL: test_a_footer_with_no_read_byte_left_is_refused (__main__.G2Plan"),
    Fault("g2-plan-block-entry-size-unchecked", "py-scripts", "g2_plan.py takes a footer whose "
          "block entries are not 32 bytes a block, so its CRC32C positions are not the file's",
          CHECK_FAILED, "FAIL: test_a_footer_whose_entries_are_not_the_sizes_the_rule_reads_is_"
          "refused (__main__.G2Plan"),
    Fault("g2-plan-footer-parts-unchecked", "py-scripts", "g2_plan.py takes a footer whose "
          "parts do not add up to it", CHECK_FAILED,
          "FAIL: test_a_footer_whose_parts_do_not_add_up_to_it_is_refused (__main__.G2Plan"),
    Fault("g2-plan-block-crc-offset-drifted", "py-scripts", "g2_plan.py places a block entry's "
          "CRC32C four bytes before container.hpp's kBlockCrc", CHECK_FAILED,
          "FAIL: test_the_footer_constants_are_the_containers (__main__.G2Plan"),
    # Review TD-14: each live refusal of the oracle step has a test.
    Fault("oracle-sweep-disagreement-passes", "py-scripts", "check_oracle.py passes a footer "
          "sweep whose verdicts disagree with the planner's rule", CHECK_FAILED,
          "FAIL: test_each_wrong_answer_is_refused_with_its_reason (__main__.LiveRefusals"),
    # Review TD-12: each committed round-trip file's flips are the planner's for its layout.
    Fault("g2-committed-plan-moved", "py-scripts", "a committed round-trip file's footer flip "
          "moved one byte back, onto a CRC32C, with its seed unchanged", CHECK_FAILED,
          "FAIL: test_every_committed_round_trip_files_flips_are_the_planners "
          "(__main__.CommittedPlans", mutate=move_a_committed_footer_flip),
    Fault("oracle-sweep-skips-positions", "py-scripts", "check_oracle.py's footer sweep flips "
          "every other footer byte", CHECK_FAILED,
          "FAIL: test_the_sweep_flips_every_footer_byte_once_and_puts_it_back "
          "(__main__.FooterSweep"),
    Fault("fuzz-host-absolute-path-kept", "py-scripts", "fuzz_host.py writes host.json with "
          "an absolute path in it", CHECK_FAILED,
          "FAIL: test_no_absolute_path_is_left_in_the_file (__main__.FuzzHost"),
    Fault("fuzz-host-name-kept", "py-scripts", "fuzz_host.py writes host.json with the host's "
          "name in it", CHECK_FAILED,
          "FAIL: test_the_hosts_name_is_never_in_the_file (__main__.FuzzHost"),
    # docs/NOTES.md, The diagnostic runs at the freeze: the summary's refusals.
    Fault("diag-no-samples-accepted", "py-scripts", "diag_summary.py takes a perf report with "
          "no samples", CHECK_FAILED,
          "FAIL: test_a_report_with_no_samples_is_refused (__main__.Reports"),
    Fault("diag-commit-unchecked", "py-scripts", "diag_summary.py writes a run made at another "
          "commit under the commit given", CHECK_FAILED,
          "FAIL: test_a_commit_that_is_not_the_one_given_is_refused (__main__.Summaries"),
    Fault("diag-sum-unchecked", "py-scripts", "diag_summary.py takes a report whose self shares "
          "do not sum to 100, one cut at a percent limit, and spreads the missing lines over "
          "the rest", CHECK_FAILED,
          "FAIL: test_a_report_cut_at_a_percent_limit_is_refused (__main__.Reports"),
    Fault("diag-unmatched-spread", "py-scripts", "diag_summary.py spreads the share no pattern "
          "matches over the groups instead of keeping it apart", CHECK_FAILED,
          "FAIL: test_what_no_pattern_matches_is_its_own_share_and_spread_over_none "
          "(__main__.Reports"),
    Fault("diag-absolute-path-kept", "py-scripts", "diag_summary.py writes a summary with an "
          "absolute path in it", CHECK_FAILED,
          "FAIL: test_a_path_the_summary_would_publish_is_refused (__main__.Summaries"),
    Fault("diag-first-answer-timed", "py-scripts", "diag_summary.py counts each query file's "
          "untimed first answer among D3's engine times", CHECK_FAILED,
          "FAIL: test_the_counters_summary_reads_engine_times_from_the_answers "
          "(__main__.Summaries"),
    # Review DX-6, DX-7, DX-9, DX-12: a profile tied to its recording, the SIMD ratio to what
    # each level ran, an iteration summary kept out of results/, and what is not established
    # never stated by the essay.
    Fault("diag-recording-count-unchecked", "py-scripts", "diag_summary.py takes a self report "
          "that holds fewer samples than perf record says it wrote", CHECK_FAILED,
          "FAIL: test_a_report_short_of_its_recording_is_refused (__main__.Recording"),
    Fault("diag-lost-samples-accepted", "py-scripts", "diag_summary.py takes a recording that "
          "lost more samples than it kept", CHECK_FAILED,
          "FAIL: test_samples_lost_beyond_the_bound_are_refused (__main__.Recording"),
    Fault("diag-event-line-optional", "py-scripts", "diag_summary.py takes a self report that "
          "names no event as a report of any event", CHECK_FAILED,
          "FAIL: test_a_report_that_names_no_event_is_refused (__main__.Recording"),
    Fault("diag-other-recording-accepted", "py-scripts", "diag_summary.py takes inclusive "
          "shares from a children report of another event and sample count", CHECK_FAILED,
          "FAIL: test_a_children_or_source_file_report_of_another_recording_is_refused "
          "(__main__.Recording"),
    Fault("diag-inclusive-added-up", "py-scripts", "diag_summary.py adds the inclusive shares "
          "of two entry points that may nest", CHECK_FAILED,
          "FAIL: test_two_entries_of_one_share_are_refused (__main__.Inclusive"),
    Fault("diag-avx2-without-avx2-accepted", "py-scripts", "diag_summary.py publishes the "
          "AVX2-over-scalar ratio of AVX2 runs that ran no AVX2 kernel", CHECK_FAILED,
          "FAIL: test_an_avx2_run_that_ran_no_avx2_kernel_is_refused (__main__.SimdChange"),
    Fault("diag-answers-differ-accepted", "py-scripts", "diag_summary.py compares the engine "
          "times of levels whose answers differ", CHECK_FAILED,
          "FAIL: test_levels_or_rounds_that_answer_differently_are_refused "
          "(__main__.SimdChange"),
    Fault("diag-ratio-not-established-published", "py-scripts", "diag_summary.py publishes a "
          "ratio it lists as not established, for answers that give no kernel runs",
          CHECK_FAILED,
          "FAIL: test_answers_with_no_kernel_runs_leave_the_ratio_not_established "
          "(__main__.SimdChange"),
    Fault("diag-store-unchecked", "py-scripts", "diag_summary.py takes query runs that opened "
          "different stores", CHECK_FAILED,
          "FAIL: test_query_runs_on_other_stores_are_refused (__main__.Refusals"),
    Fault("diag-multiplexed-accepted", "py-scripts", "diag_summary.py takes a counter perf "
          "multiplexed, its total an estimate", CHECK_FAILED,
          "FAIL: test_a_counter_missing_not_counted_or_multiplexed_is_refused "
          "(__main__.Refusals"),
    Fault("diag-iteration-under-results", "py-scripts", "diag_summary.py writes an iteration "
          "summary anywhere under results/ but results/diag/", CHECK_FAILED,
          "FAIL: test_a_smoke_is_iteration_only (__main__.Summaries"),
    Fault("writeup-not-established-read", "py-scripts", "render_writeup.py states a figure its "
          "results file lists as not established", CHECK_FAILED,
          "FAIL: test_a_path_its_file_lists_as_not_established (__main__.Refuses"),
    # E-10's leftover: the comparison reads boost before its first system and after its last.
    Fault("compare-turbo-read-once-accepted", "py-scripts", "check_compare.py takes a protocol "
          "2 Linux document whose boost was read once, after the run, with no reading before "
          "the first system", CHECK_FAILED,
          "FAIL: test_a_linux_turbo_read_only_after_the_run (__main__.Protocol2"),
    Fault("compare-turbo-read-again-on-restart", "py-compare", "compare/run.py started again "
          "after the wrapper's exit 75 reads boost again before its systems, so the first "
          "reading of the document is lost", CHECK_FAILED,
          "FAIL: test_a_restarted_run_keeps_its_first_turbo_reading (__main__.TurboReadings"),
    Fault("compare-full-day-clipped-window", "py-scripts", "check_compare.py stops comparing a "
          "full-day file's query windows with BENCHMARKS.md's, so an hour clipped to the slice "
          "passes as a full day", CHECK_FAILED,
          "FAIL: test_a_clipped_hour_passing_as_a_full_day (__main__.FullDay"),
    # Review F-15: a day's variant source is itself a reported, valid slice file.
    # Review RV-9: a full day named as the variant source is refused once, not recursed into.
    Fault("compare-variant-source-day-recursed", "py-scripts", "check_compare.py validates a "
          "full day named as a day's variant source, which names its source again, until "
          "Python's stack runs out", CHECK_FAILED,
          "FAIL: test_a_variant_source_that_is_a_full_day_is_refused_without_a_traceback "
          "(__main__.FullDay"),
    # Review RP-4: the source is a slice by check_document's own test of a day.
    Fault("compare-variant-source-day-and-slice-recursed", "py-scripts", "check_compare.py "
          "validates a variant source that holds a slice, though its day or its name makes "
          "it a full day, so a day naming itself recurses until Python's stack runs out",
          CHECK_FAILED, "FAIL: test_a_day_that_also_holds_a_slice_naming_itself_is_refused_"
          "without_a_traceback (__main__.FullDay"),
    # Review RP-5: a day's variant source states the day's protocol.
    Fault("compare-variant-source-other-protocol", "py-scripts", "check_compare.py ranks a "
          "protocol 2 day's variants from a protocol 1 slice file", CHECK_FAILED,
          "FAIL: test_a_protocol_2_day_ranked_from_a_protocol_1_slice "
          "(__main__.Protocol2FullDay"),
    Fault("compare-variant-source-unvalidated", "py-scripts", "check_compare.py ranks a full "
          "day's variants from a slice file it never validates, an iteration-only one included",
          CHECK_FAILED, "FAIL: test_a_slice_file_measured_iteration_only (__main__.FullDay"),
    Fault("guard-rate-over-nominal-interval", "py-scripts", "compare/guard.py divides swap "
          "pages by the nominal 2 s, not the time between its samples, and overstates the rate",
          CHECK_FAILED, "FAIL: test_the_rate_is_over_the_time_between_the_samples "
          "(__main__.Breach"),
    # Review F-9: the guard's stop path, and run.py's refusal of a run the guard stopped.
    Fault("guard-stop-signals-nothing", "py-scripts", "compare/guard.py records a breach and "
          "sends the session nothing, so the command runs on", CHECK_FAILED,
          "FAIL: test_a_breach_stops_the_session_and_the_guard_exits_70 (__main__.StopPath"),
    Fault("run-one-guard-fired-ignored", "py-compare", "run.py takes a system the guard stopped "
          "as finished when its command exits 0", CHECK_FAILED,
          "FAIL: test_a_run_the_guard_stopped_is_a_failure_and_leaves_no_load_lines "
          "(__main__.GuardedRun"),
    # Review F-12: QuestDB's version is the running server's, not a literal.
    Fault("questdb-version-a-literal", "py-compare", "QuestDB's version is written as 10.1.0 "
          "whatever the running server's SELECT build says", CHECK_FAILED,
          "FAIL: test_the_version_is_the_one_each_running_server_gave (__main__.QuestDBVersion"),
    # Review RP-6: the version is parsed when the server's facts are first read.
    Fault("questdb-version-read-after-the-part", "py-compare", "QuestDB's build string is "
          "parsed only once every variant and class has run, so a build it cannot read costs "
          "the whole part", CHECK_FAILED,
          "FAIL: test_an_unexpected_build_fails_the_first_class_when_its_facts_are_read "
          "(__main__.QuestDBVersion"),
    # Review F-6: a class A claim needs its proof for every system, not tick-store's alone.
    Fault("compare-class-a-parquet-flush-unchecked", "py-scripts", "check_compare.py takes "
          "Parquet's class A as proven with the harness's flush made", CHECK_FAILED,
          "FAIL: test_a_parquet_class_a_proven_with_the_harness_flush_made (__main__.Faults"),
    Fault("compare-class-a-dirty-pages-unchecked", "py-scripts", "check_compare.py takes "
          "ArcticDB's or QuestDB's class A as proven with no dirty page in its probe, when "
          "class B is not proven and the control rule stays silent", CHECK_FAILED,
          "FAIL: test_a_class_a_proven_with_no_dirty_page_in_its_probe (__main__.Faults"),
    # Review F-5: tick-store alone on a Linux day keeps the four-system day's Linux proofs.
    Fault("compare-tickstore-only-linux-warm-by-residency", "py-scripts", "check_compare.py "
          "takes a Linux tick-store-only day's warm figures on residency alone, the proof of a "
          "host without /proc, instead of each process's storage reads", CHECK_FAILED,
          "FAIL: test_a_linux_tickstore_only_day_warm_by_residency (__main__.TickstoreOnly"),
    Fault("compare-tickstore-only-linux-guard-memory-unchecked", "py-scripts",
          "check_compare.py stops requiring the guard's peak memory of a Linux tick-store-only "
          "day, so one that ran without the guard passes", CHECK_FAILED,
          "FAIL: test_a_linux_tickstore_only_day_without_the_guards_memory "
          "(__main__.TickstoreOnly"),
    # Review RV-4: a protocol 2 day of tick-store alone passes on each host.
    Fault("compare-tickstore-alone-protocol-2-refused", "py-scripts", "check_compare.py "
          "wants the second sibling read for all four systems, so a protocol 2 Linux day of "
          "tick-store alone is refused", CHECK_FAILED,
          "FAIL: test_a_tickstore_alone_day_passes (__main__.Protocol2FullDay"),
    # The comparison protocol 2 rules (docs/REVIEW.md, F-4 to F-11 and E-3).
    Fault("compare-protocol-unknown-accepted", "py-scripts", "check_compare.py takes a file of "
          "a protocol it does not know, protocol 3, as if it knew its rules", CHECK_FAILED,
          "FAIL: test_an_unknown_protocol (__main__.Protocol2"),
    # Review RF-1: protocol 1 belongs to the committed files measured before protocol 2.
    Fault("compare-protocol-1-unpinned", "py-scripts", "check_compare.py takes any file that "
          "states no protocol, so a document of the current harness escapes every protocol 2 "
          "rule by leaving its protocol out", CHECK_FAILED,
          "FAIL: test_a_current_harness_document_without_its_protocol (__main__.Protocol2"),
    # Review RV-6: the committed protocol 1 files are known by their bytes, not their header.
    Fault("compare-protocol-1-by-commit", "py-scripts", "check_compare.py exempts a file from "
          "protocol 2 by the commit its header states, which a new document can copy",
          CHECK_FAILED, "FAIL: test_a_current_harness_document_stating_a_committed_files_commit "
          "(__main__.Protocol2"),
    # A protocol 1 pin is a name and the SHA-256 of its bytes together, and the pins file holds
    # none since the freeze replaced the protocol 1 files.
    Fault("compare-protocol-1-bytes-alone", "py-scripts", "check_compare.py takes a file that "
          "states no protocol by the SHA-256 of its bytes under any name", CHECK_FAILED,
          "FAIL: test_a_protocol_1_pin_holds_only_under_its_own_name (__main__.Protocol2"),
    Fault("pins-protocol-1-line-restored", "py-scripts", "the protocol 1 pin of the kept Linux "
          "day put back in scripts/data_pins.json, so check_compare.py would take it again",
          CHECK_FAILED, "FAIL: test_the_pins_file_pins_no_protocol_1_file (__main__.Protocol2",
          protocol_1_pin_restored),
    Fault("compare-protocol-1-declared", "py-scripts", "check_compare.py takes a file that "
          "declares protocol 1, which no harness ever wrote", CHECK_FAILED,
          "FAIL: test_a_document_declaring_protocol_1 (__main__.Protocol2"),
    Fault("compare-protocol-not-an-integer", "py-scripts", "check_compare.py takes a protocol "
          "equal to 2 that is not the integer 2, such as 2.0", CHECK_FAILED,
          "FAIL: test_a_protocol_that_is_not_the_integer_2 (__main__.Protocol2"),
    Fault("compare-tail-name-unchecked", "py-scripts", "check_compare.py stops checking what a "
          "protocol 2 tail figure is called, so the slowest of 30 could pass as a 99th "
          "percentile", CHECK_FAILED,
          "FAIL: test_the_slowest_of_n_called_a_99th_percentile (__main__.Protocol2"),
    Fault("compare-repetitions-unchecked", "py-scripts", "check_compare.py stops recomputing a "
          "latency block's figures from its repetitions", CHECK_FAILED,
          "FAIL: test_a_tail_that_does_not_follow_from_the_repetitions (__main__.Protocol2"),
    Fault("compare-second-sibling-unchecked", "py-scripts", "check_compare.py stops holding "
          "cpu6's sibling to the 5% rule, so a busy cpu7 passes", CHECK_FAILED,
          "FAIL: test_a_busy_second_sibling (__main__.Protocol2"),
    Fault("compare-write-device-reads-unchecked", "py-scripts", "check_compare.py stops "
          "requiring a write's device reads", CHECK_FAILED,
          "FAIL: test_a_write_with_no_device_reads (__main__.Protocol2"),
    # Review RP-10: the validator's fault tests fail on a crash, which proves no rule.
    Fault("compare-tests-traceback-accepted", "py-scripts", "the comparison validator's tests "
          "take a run that crashed as a refusal, so a fault is proven by a traceback lacking "
          "its message", CHECK_FAILED,
          "FAIL: test_a_validator_that_crashes_fails_the_test_that_ran_it (__main__.Valid"),
    Fault("compare-repetition-counts-unchecked", "py-scripts", "check_compare.py takes a "
          "reported file whose queries ran once warm and once cold, the harness's 30 and 7 "
          "narrowed (review F-5)", CHECK_FAILED,
          "FAIL: test_a_reported_file_with_one_warm_and_one_cold_repetition (__main__.Faults"),
    Fault("compare-write-count-unchecked", "py-scripts", "check_compare.py takes a reported "
          "slice whose classes were written once, not the harness's three (review F-5)",
          CHECK_FAILED, "FAIL: test_a_slice_written_once (__main__.Faults"),
    Fault("compare-macos-write-counter-unchecked", "py-scripts", "check_compare.py takes a "
          "macOS write whose device reads are iostat's disk0 total, which holds the write's own "
          "output, as its count of reads (review RF-5)", CHECK_FAILED,
          "FAIL: test_a_macos_write_counting_disk0s_reads_and_writes (__main__.Protocol2"),
    # Review RV-7: the counter by its statement, and the reads of each process that did it.
    Fault("compare-macos-write-reads-by-process-unchecked", "py-scripts", "check_compare.py "
          "takes a macOS write whose reads by process leave out the worker or do not add up to "
          "its device reads", CHECK_FAILED,
          "FAIL: test_a_macos_write_whose_reads_by_process_are_not_its_reads (__main__.Protocol2"),
    Fault("run-system-write-reads-by-process-dropped", "py-compare", "run_system.py keeps a "
          "macOS write's reads by process for its last repetition alone", CHECK_FAILED,
          "ERROR: test_a_macos_write_keeps_each_repetitions_reads_by_process "
          "(__main__.WriteRecord"),
    Fault("compare-engine-only-not-its-own", "py-scripts", "check_compare.py stops tying a "
          "write repetition's engine-only seconds to its own total less its Python seconds, so "
          "any figure within the total passes (review RF-2)", CHECK_FAILED,
          "FAIL: test_an_engine_only_time_that_is_not_the_total_less_python (__main__.Protocol2"),
    Fault("compare-engine-past-its-query", "py-scripts", "check_compare.py stops holding each "
          "tick-store repetition's engine time within its round trip, cold within its query's, "
          "so an engine ten times its cold query passes behind the open (review RF-7)",
          CHECK_FAILED,
          "FAIL: test_a_cold_engine_time_longer_than_its_query (__main__.Protocol2"),
    Fault("compare-cold-open-resident-unchecked", "py-scripts", "check_compare.py stops "
          "requiring 0 store bytes resident before a cold clock that holds the whole open",
          CHECK_FAILED,
          "FAIL: test_pages_resident_before_a_clock_that_holds_the_whole_open (__main__.Protocol2"),
    Fault("compare-tickstore-cold-open-left-out", "py-scripts", "check_compare.py stops "
          "requiring a tick-store cold figure to be its open plus its query", CHECK_FAILED,
          "FAIL: test_a_tickstore_cold_figure_without_its_open (__main__.Protocol2"),
    Fault("compare-slice-warm-up-unchecked", "py-scripts", "check_compare.py takes a slice warm "
          "proof that records no untimed run before the repetitions", CHECK_FAILED,
          "FAIL: test_a_slice_warm_figure_with_no_warm_up (__main__.Protocol2"),
    Fault("compare-slice-input-not-resident", "py-scripts", "check_compare.py takes a slice "
          "write whose export was partly resident when its clock started", CHECK_FAILED,
          "FAIL: test_a_slice_write_from_an_export_not_resident (__main__.Protocol2"),
    Fault("targets-at-most-reversed", "py-scripts", "render_results.py judges an at-most "
          "target the wrong way round, so a latency above its target would read met in the "
          "README's targets block", CHECK_FAILED,
          "FAIL: test_each_verdict_is_computed_from_the_value_and_the_target "
          "(__main__.RenderTargetsTest"),
    Fault("guard-swap-in-stop-disabled", "py-scripts", "compare/guard.py stops firing on "
          "sustained swap-in, so a host thrashing through swap is no longer stopped",
          CHECK_FAILED, "FAIL: test_sustained_swap_in_stops (__main__.Breach"),
    # Review F-7: a setting the engine does not have is not the value requested.
    Fault("arcticdb-flags-read-as-requested", "py-compare", "ArcticDB's LMDB flags read back "
          "as 0 when the engine has no such key, which is class B's request, so the check "
          "passes on the harness's own assumption", CHECK_FAILED,
          "FAIL: test_class_b_flags_the_engine_does_not_have_are_not_read_as_0 "
          "(__main__.ArcticDBSettings"),
    # Review RF-10: the slice's untimed runs, counted as they ran and borne out by their seconds.
    Fault("compare-warm-up-seconds-unchecked", "py-scripts", "check_compare.py takes a protocol "
          "2 slice's stated untimed runs without the seconds of those runs beside them",
          CHECK_FAILED, "FAIL: test_untimed_runs_stated_with_other_seconds_beside_them "
          "(__main__.Protocol2"),
    # Review RV-8: a day's per-process warm proof, held to its untimed runs' seconds too.
    Fault("compare-day-warm-up-seconds-unchecked", "py-scripts", "check_compare.py takes a "
          "protocol 2 day's stated untimed runs without the seconds of those runs beside them",
          CHECK_FAILED, "FAIL: test_a_day_warm_up_stated_with_other_seconds_beside_it "
          "(__main__.Protocol2FullDay"),
    Fault("warm-up-runs-stated-not-counted", "py-compare", "the warm proof states the constant "
          "number of untimed runs, whatever ran", CHECK_FAILED,
          "FAIL: test_the_proof_states_the_untimed_runs_that_ran (__main__.ResidencyWarmOnADay"),
    # Review RV-5: the two hooks that make a macOS write count the processes that did it.
    Fault("run-system-write-service-uncounted", "py-compare", "run_class leaves the service's "
          "processes out of the write's request, so a macOS write does not count the server's "
          "reads", CHECK_FAILED, "FAIL: test_the_ingest_request_names_the_service_processes "
          "(__main__.WriteCountsItsServer"),
    Fault("tickstore-write-reads-uncounted", "py-compare", "tick-store's write runs "
          "tick_roundtrip through subprocess.run, so on macOS its reads are lost when it is "
          "reaped", CHECK_FAILED, "FAIL: test_the_write_runs_through_the_counter_of_its_childs_"
          "reads (__main__.Ingest"),
    # Review RF-9: each protocol 2 rule and assembly step that had a test and no fault.
    Fault("compare-write-medians-unchecked", "py-scripts", "check_compare.py stops recomputing "
          "a protocol 2 write's medians from its repetitions", CHECK_FAILED,
          "FAIL: test_a_write_median_that_does_not_follow_from_the_repetitions "
          "(__main__.Protocol2"),
    Fault("compare-engine-only-past-its-total", "py-scripts", "check_compare.py takes a write "
          "repetition's engine-only seconds longer than its total", CHECK_FAILED,
          "FAIL: test_an_engine_only_time_longer_than_the_total (__main__.Protocol2"),
    Fault("compare-engine-only-median-unchecked", "py-scripts", "check_compare.py stops "
          "recomputing a write's engine-only median from its repetitions", CHECK_FAILED,
          "FAIL: test_an_engine_only_median_that_does_not_follow (__main__.Protocol2"),
    Fault("compare-write-times-unstated", "py-scripts", "check_compare.py takes a protocol 2 "
          "write that does not say what its two times hold", CHECK_FAILED,
          "FAIL: test_a_write_not_saying_what_its_times_hold (__main__.Protocol2"),
    Fault("compare-tickstore-day-source-not-resident", "py-scripts", "check_compare.py takes "
          "tick-store's write on a day from a source not wholly resident", CHECK_FAILED,
          "FAIL: test_a_tickstore_day_write_from_a_source_not_resident "
          "(__main__.Protocol2FullDay"),
    Fault("compare-cold-open-inside-unchecked", "py-scripts", "check_compare.py takes a cold "
          "proof's open_inside_clock without matching it to the system", CHECK_FAILED,
          "FAIL: test_a_system_claiming_its_open_is_where_it_is_not (__main__.Protocol2"),
    Fault("compare-tickstore-cold-parts-unchecked", "py-scripts", "check_compare.py takes a "
          "tick-store cold block without its open and query kept per repetition", CHECK_FAILED,
          "FAIL: test_a_tickstore_cold_block_without_its_parts (__main__.Protocol2"),
    Fault("compare-warm-worker-open-untimed", "py-scripts", "check_compare.py takes a variant "
          "whose warm worker's open is not timed", CHECK_FAILED,
          "FAIL: test_a_variant_with_no_open_time_for_its_warm_worker (__main__.Protocol2"),
    Fault("compare-tail-rank-unchecked", "py-scripts", "check_compare.py takes a tail figure "
          "without its nearest rank", CHECK_FAILED,
          "FAIL: test_a_tail_figure_with_no_rank (__main__.Protocol2"),
    Fault("compare-tail-slowest-unchecked", "py-scripts", "check_compare.py takes a "
          "slowest-of-n tail figure not flagged as the slowest", CHECK_FAILED,
          "FAIL: test_the_slowest_of_n_not_called_the_slowest (__main__.Protocol2"),
    Fault("compare-macos-second-sibling-claimed", "py-scripts", "check_compare.py takes a "
          "macOS file claiming a reading of a second sibling", CHECK_FAILED,
          "FAIL: test_a_macos_file_claiming_a_second_sibling (__main__.Protocol2"),
    Fault("compare-protocol-1-rule-on-protocol-2", "py-scripts", "check_compare.py takes the "
          "variant rule as protocol 1 stated it in a protocol 2 full day", CHECK_FAILED,
          "FAIL: test_the_rule_as_protocol_1_stated_it (__main__.Protocol2FullDay"),
    Fault("run-system-warm-up-seconds-dropped", "py-compare", "run_system.py drops each "
          "query's warm-up seconds from the part", CHECK_FAILED,
          "ERROR: test_a_query_keeps_its_warm_up_and_its_cold_parts (__main__.Assembly"),
    Fault("run-system-cold-parts-dropped", "py-compare", "run_system.py drops tick-store's "
          "cold open and query parts from the part", CHECK_FAILED,
          "ERROR: test_a_query_keeps_its_warm_up_and_its_cold_parts (__main__.Assembly"),
    Fault("run-system-warm-worker-open-dropped", "py-compare", "run_system.py drops the warm "
          "worker's open from each variant", CHECK_FAILED,
          "FAIL: test_each_variant_keeps_its_warm_workers_open (__main__.Assembly"),
    Fault("run-one-second-sibling-unrequired", "py-compare", "compare/run.py reuses a part "
          "with no reading of cpu6's sibling", CHECK_FAILED,
          "FAIL: test_the_second_siblings_reading_is_kept (__main__.SecondSibling"),
    # Review RF-11: a write's input whole where it must be, refused in the worker.
    Fault("write-input-not-whole-accepted", "py-compare", "a write whose input is not wholly "
          "resident when its clock starts, tick-store's source or any input on the slice, runs "
          "on and is refused only when the file is validated", CHECK_FAILED,
          "FAIL: test_an_input_not_whole_where_it_must_be_stops_the_write_before_its_clock "
          "(__main__.WriteRecord"),
    Fault("device-stat-missing-unnamed", "py-compare", "an input on a file system with no "
          "block device of its own fails with a bare FileNotFoundError, not an error naming it",
          CHECK_FAILED, "FAIL: test_an_input_on_no_block_device_of_its_own_is_named "
          "(__main__.WriteRecord"),
    # Review RF-8: a cold proof's residency is read where the figure's clock starts.
    Fault("cold-clock-start-before-the-open", "py-compare", "a cold proof records the store "
          "resident before the worker's open for a system whose clock starts at the query, so "
          "pages its open made resident go unseen", CHECK_FAILED,
          "FAIL: test_an_open_outside_the_figure_is_read_before_the_query "
          "(__main__.ColdClockStart"),
    # Review F-8: the one-symbol store's rows are held to the rows read back.
    Fault("arcticdb-rows-against-themselves", "py-compare", "ArcticDB's one-symbol stored "
          "settings compare the description's row count with itself, a check that cannot "
          "fail", CHECK_FAILED,
          "FAIL: test_a_description_short_of_the_rows_read_back (__main__.ArcticDBStoredRows"),
    # Review F-5: a measured run is the whole protocol, and a resumed one never mixes parts.
    Fault("compare-measured-run-narrowed", "py-compare", "compare/run.py takes repetition "
          "overrides, --variants or fewer --systems in a measured run, so a reported file can "
          "hold less than the protocol", CHECK_FAILED,
          "FAIL: test_a_measured_run_refuses_to_narrow_anything (__main__.MeasuredRun"),
    # Review RV-11: run.py's own refusals and proofs for tick-store alone.
    Fault("run-alone-linux-proven-as-macos", "py-compare", "run.py writes a Linux tick-store-"
          "alone day's method as macOS's, warm by residency and with no guard", CHECK_FAILED,
          "FAIL: test_each_host_ends_with_a_tickstore_alone_document "
          "(__main__.TickstoreAloneDay"),
    Fault("run-alone-narrowed", "py-compare", "run.py lets tick-store alone run beside other "
          "--systems, from a --variant-source or with --variants", CHECK_FAILED,
          "FAIL: test_tickstore_alone_is_a_day_of_tickstore_and_every_variant "
          "(__main__.TickstoreAloneDay"),
    # Review RV-10: the parts directory does not name the slice, so the arguments do.
    Fault("compare-part-reused-across-slices", "py-compare", "run.py reuses a system's part made "
          "from another slice", CHECK_FAILED,
          "FAIL: test_a_part_made_from_another_slice_runs_again (__main__.ResumedRun"),
    Fault("compare-part-reused-across-arguments", "py-compare", "compare/run.py reuses a "
          "system's part made with other repetition arguments when a run is resumed",
          CHECK_FAILED, "FAIL: test_a_part_made_with_other_repetitions_runs_again "
          "(__main__.ResumedRun"),
    Fault("coldcache-refill-unchecked", "py-compare", "an eviction counts as proven without "
          "checking that the store stays empty, so a store something else is reading passes",
          CHECK_FAILED, "FAIL: test_a_store_refilling_after_eviction_is_not_proven "
          "(__main__.Evict"),
    Fault("cold-refilled-run-counted", "py-compare", "a cold repetition after which the store "
          "refilled, with nothing of ours running, is counted", CHECK_FAILED,
          "FAIL: test_a_refilled_store_runs_the_repetition_again (__main__.ColdRuns"),
    Fault("compare-tests-settings", "py-compare", "the QuestDB commit mode check stops checking",
          CHECK_FAILED, "FAIL: test_the_parser_outranks_a_matching_echo"),
    Fault("tickstore-settings-unchecked", "py-compare", "tick-store's read-back stops comparing "
          "each setting with the one requested, so a store of another segment size passes "
          "silently", CHECK_FAILED, "FAIL: test_a_segment_size_mismatch_stops_the_run"),
    Fault("tickstore-source-warm-no-eviction", "py-compare", "tick-store's write on a day "
          "reads its source through again after a short warm without evicting the day's "
          "export, which on debian left it short again, as at ca96b36", CHECK_FAILED,
          "FAIL: test_a_short_warm_on_a_day_evicts_the_export_and_reads_the_source_again"),
    Fault("results-no-load-lines", "check-results", "a macOS results file with no load lines",
          CHECK_FAILED, "no load lines", drop_load_lines),
    Fault("results-macos-pinned", "check-results", "a macOS results file claiming pinned",
          CHECK_FAILED, "macOS cannot be pinned", claim_pinned_on_macos),
    Fault("results-linux-turbo-unmeasured", "check-results", "the pinned host's Linux results "
          "file with turbo unavailable, as a hosted runner records it", CHECK_FAILED,
          "header.turbo: Linux must record it", unmeasured_turbo_on_linux),
    # Review RA-5: a benchmark file says how its build established its commit.
    Fault("results-commit-source-missing", "check-results", "a macOS results file whose header "
          "does not say how its build established its commit", CHECK_FAILED,
          "header: missing field 'commit_source'", drop_commit_source),
    Fault("results-commit-source-any-value", "py-scripts", "check_results.py takes any "
          "commit_source but an empty one, so a build that named its commit some other way "
          "passes", CHECK_FAILED,
          "FAIL: test_a_commit_source_that_is_neither_git_nor_export (__main__.CheckResultsTest"),
    Fault("results-empty-folder", "check-results", "no results files at all, which must not "
          "pass as nothing wrong", CHECK_FAILED, "no results files in", empty_the_results_folder),
    Fault("guard-sub-resolution-data", "check-results", "a percentile below the timer's floor "
          "in a results file", CHECK_FAILED, "the timer can resolve", sub_resolution_percentile),
    Fault("guard-cpp-removed", "dev-test", "ResolutionGuard stops refusing a value below its "
          "floor", EXIT_CTEST, "ResolutionGuard.RefusesAValueBelowTheFloor"),
    Fault("compare-commit-mode", "check-compare", "QuestDB class B reads back commit mode nosync",
          CHECK_FAILED, "requested 'sync', in force 'nosync'", mismatched_commit_mode),
    Fault("compare-read-back", "check-compare", "a store failed its read-back", CHECK_FAILED,
          "the oracle said mismatch", failed_read_back),
    Fault("compare-query-digest", "check-compare", "a query's digest differs from the ground "
          "truth", CHECK_FAILED, "ground truth", query_digest_mismatch),
    Fault("compare-swap-unflagged", "check-compare", "a cold query had a page swapped in while "
          "timed, and is not flagged", CHECK_FAILED, "Q1.cold.swap_timed: flagged is False with",
          unflagged_swap_in),
    Fault("fuzz-g3-target-missing", "render-check", "G-3's results file without one target's "
          "Mac run, its summary recounted to match, so the Targets block would judge G-3 on "
          "twelve of thirteen targets", EXIT_RENDER_REFUSED,
          "no run of fuzz_columns on macOS at neon: G-3 needs every target",
          fuzz_g3_target_missing),
    Fault("fuzz-g3-crc-target-missing", "render-check", "G-3's results file without the "
          "container target's Linux run, its summary recounted to match, so the SSE4.2 CRC32C "
          "kernel would go unfuzzed for 30 minutes with G-3 still met", EXIT_RENDER_REFUSED,
          "no run of fuzz_container on Linux at avx2: G-3 needs every target",
          fuzz_g3_crc_target_missing),
    Fault("fuzz-g3-short-run-accepted", "py-scripts", "check_fuzz.py stops refusing a clean run "
          "shorter than 1,800 s, so a 20-second run could stand for G-3's 30 minutes",
          CHECK_FAILED, "FAIL: test_a_clean_run_under_1800_seconds_is_refused "
          "(__main__.CheckFuzzTest"),
    # G-3's file is tied to the code it judged.
    Fault("fuzz-g3-tie-ignored", "py-scripts", "check_fuzz.py stops refusing a G-3 file whose "
          "commit is older than a change to the fuzzed code, so a file from before a codec "
          "change could still stand for G-3", CHECK_FAILED,
          "FAIL: test_a_file_older_than_a_fuzzed_code_change_is_refused (__main__.TieTest"),
    Fault("fuzz-g3-unjudged-passes", "py-scripts", "check_fuzz.py passes a file it could not "
          "tie to the code for want of git history, as in an exported tree or a shallow clone",
          CHECK_FAILED, "FAIL: test_no_history_is_not_judged (__main__.TieTest"),
    Fault("check-sh-untied-g3-passes", "py-scripts", "check.sh's render-check passes when G-3's "
          "file could not be tied to the code, rather than skipping, which --no-skips fails",
          CHECK_FAILED, "FAIL: test_a_g3_file_with_no_history_to_tie_it_skips_and_fails_under_"
          "no_skips (__main__.CheckScript"),
    # G-3's run is a replay of every input, a merge, then the timed part on the merged set.
    Fault("fuzz-g3-replay-unchecked", "py-scripts", "check_fuzz.py stops checking a clean run's "
          "replay, merge and timed split, so a run that never replayed its corpus and seeds "
          "could pass", CHECK_FAILED,
          "FAIL: test_a_run_with_no_replay_record_is_refused (__main__.CheckFuzzTest"),
    Fault("fuzz-g3-merge-crash-accepted", "py-scripts", "check_fuzz.py takes a merge libFuzzer "
          "had to start again, which it does only past an input that crashed it", CHECK_FAILED,
          "FAIL: test_a_merge_that_crashed_is_refused (__main__.CheckFuzzTest"),
    Fault("fuzz-g3-timed-short-accepted", "py-scripts", "check_fuzz.py stops requiring 1,800 s "
          "of the timed part before and after its first new input", CHECK_FAILED,
          "FAIL: test_a_timed_part_under_1800_seconds_is_refused (__main__.CheckFuzzTest"),
    Fault("fuzz-g3-old-schema-accepted", "py-scripts", "check_fuzz.py takes the schema from "
          "before the launcher again, which records no replay or merge, so a new file could "
          "record none", CHECK_FAILED,
          "FAIL: test_the_schema_from_before_the_launcher_is_refused_in_any_file "
          "(__main__.TieTest"),
    # Review RG-1: a run stopped by its replay or merge carries that part's command.
    Fault("fuzz-g3-failed-part-caps-unchecked", "py-scripts", "check_fuzz.py holds a run "
          "stopped by its replay or merge, which carries that part's command, to no caps at "
          "all", CHECK_FAILED, "FAIL: test_a_failed_parts_command_still_needs_the_caps "
          "(__main__.CheckFuzzTest"),
    Fault("fuzz-g3-budget-waived-for-any-unclean-run", "py-scripts", "check_fuzz.py waives the "
          "time budget for any run that was not clean, not only one its replay or merge "
          "stopped", CHECK_FAILED,
          "FAIL: test_a_run_whose_timed_part_ran_still_needs_its_budget (__main__.CheckFuzzTest"),
    # Review RG-2: the assembler relabels runs only across commits the tie says do not matter.
    Fault("fuzz-results-relabel-across-launcher-change", "py-scripts", "fuzz_results.py writes "
          "runs made at one commit as a later one's when only scripts changed in between, the "
          "G-3 launcher among them", CHECK_FAILED,
          "FAIL: test_runs_are_not_written_as_a_commit_whose_launcher_changed "
          "(__main__.AssembleTest"),
    # Reviews RG-7, RG-11 and RG-12: the parts' folders, a commit no full clone holds, and a
    # count that is a boolean.
    Fault("fuzz-g3-part-folders-unchecked", "py-scripts", "check_fuzz.py takes a merge of other "
          "folders than the replay's, or a timed run not on the merged set alone", CHECK_FAILED,
          "FAIL: test_a_merge_of_other_folders_than_the_replays_is_refused "
          "(__main__.CheckFuzzTest"),
    Fault("fuzz-g3-missing-commit-unjudged", "py-scripts", "check_fuzz.py leaves a file whose "
          "commit no full clone holds unjudged, as if the clone were shallow, where it is "
          "refused", CHECK_FAILED,
          "FAIL: test_a_commit_in_no_history_of_a_full_clone_is_refused (__main__.TieTest"),
    Fault("fuzz-g3-boolean-count-accepted", "py-scripts", "check_fuzz.py takes true for a merge's "
          "attempts, a replay's inputs or a merge's kept inputs, as Python counts it 1",
          CHECK_FAILED, "FAIL: test_a_count_that_is_a_boolean_is_refused (__main__.CheckFuzzTest"),
    Fault("fuzz-g3-launch-replay-finding-ignored", "py-scripts", "fuzz_g3.py goes on to the "
          "merge and the timed run after a finding in the replay", CHECK_FAILED,
          "FAIL: test_a_finding_in_the_replay_stops_the_target_before_its_timed_run "
          "(__main__.LauncherTest"),
    Fault("fuzz-g3-launch-merge-restart-ignored", "py-scripts", "fuzz_g3.py takes a merge "
          "libFuzzer restarted past a crashing input", CHECK_FAILED,
          "FAIL: test_a_merge_libfuzzer_had_to_restart_fails_the_target (__main__.LauncherTest"),
    # Review RG-9: a target the launcher drops is failed, and an earlier launch's files are refused.
    Fault("fuzz-g3-launch-missing-target-passed", "py-scripts", "fuzz_g3.py counts only the "
          "targets its workers recorded, so a listed target with no result passes", CHECK_FAILED,
          "FAIL: test_a_listed_target_missing_from_the_results_counts_as_failed "
          "(__main__.LauncherTest"),
    Fault("fuzz-g3-launch-worker-raise-uncaught", "py-scripts", "fuzz_g3.py lets a target that "
          "raises end its worker, so the targets queued behind it never run", CHECK_FAILED,
          "FAIL: test_a_target_whose_worker_raises_is_failed_and_the_next_target_still_runs "
          "(__main__.LauncherTest"),
    Fault("fuzz-g3-launch-earlier-files-taken", "py-scripts", "fuzz_g3.py launches into a logs "
          "folder that holds an earlier launch's files for its targets", CHECK_FAILED,
          "FAIL: test_an_earlier_launchs_files_in_the_logs_folder_are_refused "
          "(__main__.LauncherTest"),
    # Review P1-6: G-3's bound-seed gate has a results file, checked by check_gate.py, which
    # also ties the disturbance records' gate run entries to its runs.
    Fault("gate-seed-missing-accepted", "py-scripts", "check_gate.py takes a gate file with no "
          "run of a seed of fuzz/bound_seeds.txt on a host", CHECK_FAILED,
          "FAIL: test_a_seed_missing_on_a_host_is_refused (__main__.CoverageTest"),
    Fault("gate-few-runs-accepted", "py-scripts", "check_gate.py takes a gate file that ran each "
          "seed four times, where the gate requires five", CHECK_FAILED,
          "FAIL: test_fewer_runs_than_the_gate_requires_are_refused (__main__.CoverageTest"),
    Fault("gate-time-per-execution-unchecked", "py-scripts", "check_gate.py takes a run whose "
          "time an execution is not its libFuzzer time over its executed units", CHECK_FAILED,
          "FAIL: test_a_time_that_does_not_follow_from_the_runs_fields_is_refused "
          "(__main__.RunTest"),
    Fault("gate-over-bar-pass-accepted", "py-scripts", "check_gate.py takes a run over the bar "
          "marked pass", CHECK_FAILED,
          "FAIL: test_a_run_over_the_bar_marked_pass_is_refused (__main__.RunTest"),
    Fault("gate-pass-rule-unchecked", "py-scripts", "check_gate.py takes a run marked pass that "
          "reported a finding", CHECK_FAILED,
          "FAIL: test_a_run_with_a_finding_marked_pass_is_refused (__main__.RunTest"),
    Fault("gate-summary-unchecked", "py-scripts", "check_gate.py takes a summary whose worst run "
          "is not the rows' worst", CHECK_FAILED,
          "FAIL: test_another_worst_run_is_refused (__main__.SummaryTest"),
    Fault("gate-bar-unchecked", "py-scripts", "check_gate.py takes a gate file whose bar is not "
          "half of fuzz/launch.txt's -timeout", CHECK_FAILED,
          "FAIL: test_a_bar_not_half_the_timeout_is_refused (__main__.BarTest"),
    Fault("gate-name-unchecked", "py-scripts", "check_gate.py takes a gate file named for "
          "another commit than its own", CHECK_FAILED,
          "FAIL: test_a_file_named_for_another_commit_is_refused (__main__.ShapeTest"),
    Fault("gate-leak-accepted", "py-scripts", "check_gate.py takes a gate file that holds a "
          "home path", CHECK_FAILED,
          "FAIL: test_a_home_path_in_the_file_is_refused (__main__.LeakTest"),
    Fault("gate-entry-run-unchecked", "py-scripts", "check_gate.py ties a gate run entry to any "
          "Linux run of its number, whatever seed the entry names", CHECK_FAILED,
          "FAIL: test_an_entry_naming_a_run_the_file_does_not_hold_is_refused "
          "(__main__.EntryTest"),
    Fault("gate-entry-overlap-unchecked", "py-scripts", "check_gate.py takes a gate run entry "
          "whose span does not overlap the run it names, as the backup's entry first named "
          "the wrong run", CHECK_FAILED,
          "FAIL: test_an_entry_outside_the_run_it_names_is_refused (__main__.EntryTest"),
    Fault("gate-entry-without-file-accepted", "py-scripts", "check_gate.py takes a gate run "
          "entry with no gate file of its commit", CHECK_FAILED,
          "FAIL: test_an_entry_with_no_gate_file_is_refused (__main__.EntryTest"),
    # A gate run entry's span is two real UTC times, start first.
    Fault("gate-entry-span-form-unchecked", "py-scripts", "check_gate.py takes a gate run entry "
          "whose span is \"\" to \"Z\", which overlaps every run as a string", CHECK_FAILED,
          "FAIL: test_an_entry_whose_span_is_no_time_is_refused (__main__.EntryTest"),
    Fault("gate-entry-span-backwards-accepted", "py-scripts", "check_gate.py takes a gate run "
          "entry that ends before it starts", CHECK_FAILED,
          "FAIL: test_an_entry_that_ends_before_it_starts_is_refused (__main__.EntryTest"),
    # Every other refusal of check_gate.py: the file's shape, the pass rule's parts, the list and
    # launch file as the assembler reads them, an entry's case and count, and its exits.
    Fault("gate-file-parts-unchecked", "py-scripts",
          "check_gate.py takes a gate file with a part beside its header, runs and summary",
          CHECK_FAILED, "FAIL: test_a_file_with_another_part_is_refused (__main__.ShapeFormTest"),
    Fault("gate-header-object-unchecked", "py-scripts", "check_gate.py reads a header that is a "
          "list as an object, and raises", CHECK_FAILED,
          "ERROR: test_a_header_that_is_not_an_object_is_refused (__main__.ShapeFormTest"),
    Fault("gate-header-keys-unchecked", "py-scripts", "check_gate.py takes a header with a key "
          "the assembler never writes", CHECK_FAILED,
          "FAIL: test_a_header_with_another_key_is_refused (__main__.ShapeFormTest"),
    Fault("gate-commit-form-unchecked", "py-scripts", "check_gate.py takes a gate file whose "
          "commit is the 12 characters of its name, not a full commit", CHECK_FAILED,
          "FAIL: test_a_short_commit_its_name_gives_is_refused (__main__.ShapeFormTest"),
    Fault("gate-bar-whole-unchecked", "py-scripts", "check_gate.py takes a bar that is not a "
          "whole number of ms", CHECK_FAILED, "FAIL: test_a_bar_that_is_not_whole_is_refused "
          "(__main__.ShapeFormTest"),
    Fault("gate-runs-a-seed-whole-unchecked", "py-scripts", "check_gate.py takes runs a seed "
          "given as a string, and raises", CHECK_FAILED,
          "ERROR: test_runs_a_seed_as_a_string_are_refused (__main__.ShapeFormTest"),
    Fault("gate-no-runs-accepted", "py-scripts", "check_gate.py takes a gate file of no runs",
          CHECK_FAILED, "FAIL: test_a_file_of_no_runs_is_refused (__main__.ShapeFormTest"),
    Fault("gate-runs-not-a-list-accepted", "py-scripts", "check_gate.py reads runs that are a "
          "string as a list of runs", CHECK_FAILED,
          "FAIL: test_runs_that_are_not_a_list_are_refused (__main__.ShapeFormTest"),
    Fault("gate-run-host-unchecked", "py-scripts", "check_gate.py takes a run on a host the gate "
          "did not run on", CHECK_FAILED, "FAIL: test_a_run_on_another_host_is_refused "
          "(__main__.ShapeFormTest"),
    Fault("gate-run-fields-unchecked", "py-scripts", "check_gate.py takes a run with a field "
          "missing, and raises", CHECK_FAILED,
          "ERROR: test_a_run_with_a_field_missing_is_refused (__main__.ShapeTest"),
    Fault("gate-pass-rule-exit-ignored", "py-scripts", "check_gate.py's pass rule leaves out the "
          "exit code, so a run that exited 77 with no finding passes", CHECK_FAILED,
          "FAIL: test_a_run_that_exited_otherwise_marked_pass_is_refused (__main__.RunTest"),
    Fault("gate-pass-rule-bar-ignored", "py-scripts", "check_gate.py's pass rule leaves out the "
          "bar, so it refuses an assembled run over the bar marked fail", CHECK_FAILED,
          "FAIL: test_an_assembled_run_over_the_bar_marked_fail_passes (__main__.RunTest"),
    Fault("gate-pass-rule-sibling-start-ignored", "py-scripts",
          "check_gate.py's pass rule leaves out the sibling at a Linux run's start", CHECK_FAILED,
          "FAIL: test_a_linux_run_without_its_sibling_at_its_start_marked_pass_is_refused "
          "(__main__.RunTest"),
    Fault("gate-pass-rule-sibling-end-ignored", "py-scripts", "check_gate.py's pass rule leaves "
          "out the sibling at a Linux run's end", CHECK_FAILED,
          "FAIL: test_a_linux_run_without_its_sibling_busy_marked_pass_is_refused "
          "(__main__.RunTest"),
    Fault("gate-marked-fail-accepted", "py-scripts",
          "check_gate.py takes a run that met the pass rule marked fail", CHECK_FAILED,
          "FAIL: test_a_clean_run_marked_fail_is_refused (__main__.RunTest"),
    Fault("gate-seed-not-listed-accepted", "py-scripts", "check_gate.py takes a host's runs of a "
          "seed fuzz/bound_seeds.txt does not hold", CHECK_FAILED,
          "FAIL: test_a_seed_not_in_the_list_is_refused (__main__.CoverageTest"),
    Fault("gate-run-numbering-unchecked", "py-scripts", "check_gate.py takes a seed whose runs "
          "are not numbered 1 to its runs a seed, one missing", CHECK_FAILED,
          "FAIL: test_a_seed_with_a_run_missing_is_refused (__main__.CoverageTest"),
    Fault("gate-no-timeout-raises", "py-scripts", "check_gate.py takes no -timeout in "
          "fuzz/launch.txt as a number, and raises", CHECK_FAILED,
          "ERROR: test_a_launch_file_with_no_timeout_is_refused (__main__.ListTest"),
    Fault("gate-entry-case-unchecked", "py-scripts", "check_gate.py reads a gate run entry whose "
          "case is not \"<seed> run <K> of <N>\", and raises", CHECK_FAILED,
          "ERROR: test_an_entry_whose_case_is_not_a_run_is_refused (__main__.EntryTest"),
    Fault("gate-entry-of-n-unchecked", "py-scripts", "check_gate.py takes a gate run entry that "
          "gives another count of runs a seed than the gate file's", CHECK_FAILED,
          "FAIL: test_an_entry_naming_another_count_of_runs_is_refused (__main__.EntryTest"),
    Fault("gate-entry-any-host", "py-scripts", "check_gate.py looks for an entry's run on both "
          "hosts, so it finds two and refuses every entry", CHECK_FAILED,
          "FAIL: test_an_entry_inside_the_run_it_names_passes (__main__.EntryTest"),
    Fault("gate-file-not-json-raises", "py-scripts",
          "check_gate.py raises on a gate file that is not JSON", CHECK_FAILED,
          "ERROR: test_a_file_that_is_not_json_is_refused (__main__.ShapeFormTest"),
    Fault("gate-main-exits-0", "py-scripts", "check_gate.py exits 0 whatever it refuses",
          CHECK_FAILED, "FAIL: test_a_refused_file_exits_1 (__main__.MainTest"),
    Fault("gate-main-entries-ignored", "py-scripts", "check_gate.py exits 0 when only a "
          "disturbance entry is refused", CHECK_FAILED,
          "FAIL: test_a_refused_entry_alone_exits_1 (__main__.MainTest"),
    Fault("gate-list-comment-a-seed", "py-scripts", "check_gate.py reads a comment line of "
          "fuzz/bound_seeds.txt as a seed", CHECK_FAILED,
          "FAIL: test_a_comment_in_the_list_is_no_seed (__main__.ListTest"),
    Fault("gate-list-target-unprefixed", "py-scripts",
          "check_gate.py reads a list's target named without fuzz_ as another target than the "
          "assembler's", CHECK_FAILED,
          "FAIL: test_a_target_named_without_its_prefix_is_its_fuzz_target (__main__.ListTest"),
    Fault("gate-list-line-form-unchecked", "py-scripts", "check_gate.py gives a seed a target "
          "from a list line of three words", CHECK_FAILED,
          "FAIL: test_a_list_line_that_is_not_target_and_seed_gives_no_target (__main__.ListTest"),
    Fault("gate-timeout-any-row", "py-scripts", "check_gate.py reads -timeout from any row of "
          "fuzz/launch.txt, not its flags row", CHECK_FAILED,
          "FAIL: test_a_timeout_outside_the_flags_row_is_not_the_bar (__main__.ListTest"),
    Fault("gate-timeout-first-of-two", "py-scripts", "check_gate.py takes the first of two "
          "-timeout flags as the bar's", CHECK_FAILED,
          "FAIL: test_a_launch_file_with_two_timeouts_is_refused (__main__.ListTest"),
    Fault("gate-block-refusal-ignored", "py-scripts", "render_results.py renders fuzz/README.md's "
          "gate block from a gate file check_gate.py refuses", CHECK_FAILED,
          "FAIL: test_a_refused_file_renders_no_block (__main__.RenderTest"),
    Fault("gate-block-journal-end-said", "py-scripts", "render_results.py gives an event the "
          "host's journal shows as ending at its last line, which the journal does not show",
          CHECK_FAILED, "FAIL: test_a_journal_event_is_given_by_its_last_line_in_the_journal "
          "(__main__.EventEndTest"),
    Fault("gate-block-several-files-taken", "py-scripts", "render_results.py renders the gate's "
          "block from the first of two gate files", CHECK_FAILED,
          "FAIL: test_more_than_one_gate_file_is_refused (__main__.PageTest"),
    Fault("gate-block-markers-optional", "py-scripts", "render_results.py takes fuzz/README.md "
          "with no gate markers beside a gate file, or the markers with no gate file",
          CHECK_FAILED, "FAIL: test_a_gate_file_with_no_markers_is_refused (__main__.PageTest"),
    Fault("gate-block-hand-edit", "render-check", "a hand edit of the Linux worst run's time "
          "in fuzz/README.md's gate block", CHECK_FAILED,
          "the table in fuzz/README.md is stale or was edited by hand"),
    Fault("ties-fuzz-gate-no-kind", "py-scripts", "check_ties.py with no kind for the gate "
          "file", CHECK_FAILED,
          "FAIL: test_the_gate_kind_is_the_fuzz_tie_but_the_launcher_and_its_own_assembler "
          "(__main__.Repository"),
    # Each run field of the gate file holds only what scripts/gate_results.py writes there.
    Fault("gate-run-seed-form-unchecked", "py-scripts", "check_gate.py takes a run whose seed is "
          "a number, not a name", CHECK_FAILED, "FAIL: test_a_seed_that_is_not_a_name_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-target-form-unchecked", "py-scripts",
          "check_gate.py takes a run whose target is null, not a name", CHECK_FAILED,
          "FAIL: test_a_target_that_is_not_a_name_is_refused (__main__.FormTest"),
    Fault("gate-run-number-form-unchecked", "py-scripts", "check_gate.py takes a run numbered 0",
          CHECK_FAILED, "FAIL: test_a_run_numbered_zero_is_refused (__main__.FormTest"),
    Fault("gate-run-start-form-unchecked", "py-scripts", "check_gate.py takes a run whose start "
          "is not a UTC time", CHECK_FAILED, "FAIL: test_a_start_that_is_no_time_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-end-form-unchecked", "py-scripts", "check_gate.py takes a run whose end is "
          "not a UTC time", CHECK_FAILED, "FAIL: test_an_end_that_is_no_time_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-exit-code-form-unchecked", "py-scripts", "check_gate.py takes a run whose "
          "exit code is negative", CHECK_FAILED, "FAIL: test_a_negative_exit_code_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-findings-form-unchecked", "py-scripts", "check_gate.py takes a run whose "
          "findings are a string", CHECK_FAILED, "FAIL: test_findings_as_a_string_are_refused "
          "(__main__.FormTest"),
    Fault("gate-run-units-form-unchecked", "py-scripts", "check_gate.py takes a run of no "
          "executed unit, and divides by it", CHECK_FAILED,
          "ERROR: test_no_executed_unit_is_refused (__main__.FormTest"),
    Fault("gate-run-file-ms-form-unchecked", "py-scripts", "check_gate.py takes a run whose "
          "libFuzzer time is not a whole number of ms", CHECK_FAILED,
          "FAIL: test_a_libfuzzer_time_that_is_not_whole_is_refused (__main__.FormTest"),
    Fault("gate-run-time-form-unchecked", "py-scripts", "check_gate.py takes a run whose time an "
          "execution is a whole number, which the assembler never writes", CHECK_FAILED,
          "FAIL: test_a_time_an_execution_that_is_not_a_float_is_refused (__main__.FormTest"),
    Fault("gate-run-wall-form-unchecked", "py-scripts", "check_gate.py takes a run whose wall "
          "time is negative", CHECK_FAILED, "FAIL: test_a_negative_wall_time_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-load-form-unchecked", "py-scripts", "check_gate.py takes a run whose load is "
          "NaN, which the page prints", CHECK_FAILED,
          "FAIL: test_a_load_that_is_not_a_number_is_refused (__main__.FormTest"),
    Fault("gate-run-pass-form-unchecked", "py-scripts", "check_gate.py takes a run whose pass is "
          "1, not true or false", CHECK_FAILED,
          "FAIL: test_a_pass_that_is_not_true_or_false_is_refused (__main__.FormTest"),
    Fault("gate-run-shares-form-unchecked", "py-scripts", "check_gate.py takes a Linux run whose "
          "busy shares hold a string", CHECK_FAILED,
          "FAIL: test_a_busy_share_that_is_not_a_number_is_refused (__main__.FormTest"),
    Fault("gate-run-processes-form-unchecked", "py-scripts", "check_gate.py takes a Linux run "
          "whose processes started are negative, which the page prints", CHECK_FAILED,
          "FAIL: test_negative_processes_started_are_refused (__main__.FormTest"),
    Fault("gate-run-sibling-start-form-unchecked", "py-scripts",
          "check_gate.py takes a Linux run whose sibling at its start is 1, not true or false",
          CHECK_FAILED, "FAIL: test_a_sibling_at_its_start_that_is_not_true_or_false_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-sibling-end-form-unchecked", "py-scripts", "check_gate.py takes a Linux run "
          "whose sibling at its end is a string, not true or false", CHECK_FAILED,
          "FAIL: test_a_sibling_at_its_end_that_is_not_true_or_false_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-end-before-start-accepted", "py-scripts", "check_gate.py takes a run that "
          "ends before it starts", CHECK_FAILED, "FAIL: test_an_end_before_the_start_is_refused "
          "(__main__.FormTest"),
    Fault("gate-run-target-not-its-own-accepted", "py-scripts", "check_gate.py takes a run whose "
          "target is not its seed's in fuzz/bound_seeds.txt", CHECK_FAILED,
          "FAIL: test_a_target_not_its_seeds_own_is_refused (__main__.FormTest"),
    Fault("gate-infinite-number-accepted", "py-scripts",
          "check_gate.py takes an infinite load or time an execution", CHECK_FAILED,
          "FAIL: test_an_infinite_load_is_refused (__main__.FormTest"),
    Fault("gate-share-past-a-hundred-accepted", "py-scripts", "check_gate.py takes a busy share "
          "over 100", CHECK_FAILED, "FAIL: test_a_busy_share_over_a_hundred_is_refused "
          "(__main__.FormTest"),
    Fault("gate-share-decimals-unchecked", "py-scripts",
          "check_gate.py takes a busy share to two decimals, which the assembler rounds to one",
          CHECK_FAILED, "FAIL: test_a_busy_share_past_one_decimal_is_refused (__main__.FormTest"),
    Fault("gate-share-order-unchecked", "py-scripts",
          "check_gate.py takes busy shares out of the cpus' order", CHECK_FAILED,
          "FAIL: test_busy_shares_out_of_the_cpus_order_are_refused (__main__.FormTest"),
    Fault("gate-share-sibling-missing-accepted", "py-scripts", "check_gate.py takes busy shares "
          "with no share of cpu7, the sibling the gate kept busy", CHECK_FAILED,
          "FAIL: test_busy_shares_without_the_sibling_are_refused (__main__.FormTest"),
    Fault("gate-share-names-unchecked", "py-scripts",
          "check_gate.py takes a busy share named for no cpu", CHECK_FAILED,
          "FAIL: test_busy_shares_of_no_cpu_name_are_refused (__main__.FormTest"),
    Fault("gate-utc-not-real-accepted", "py-scripts", "check_gate.py takes a run that starts on "
          "31 November, which its time pattern matches", CHECK_FAILED,
          "FAIL: test_a_start_on_no_real_day_is_refused (__main__.FormTest"),
    # The gate file's header gives the assembler's words, the tree's list by its SHA-256 and
    # exactly the gate's runs a seed.
    Fault("gate-header-check-unchecked", "py-scripts", "check_gate.py takes a gate file whose "
          "check is another's", CHECK_FAILED, "FAIL: test_another_check_is_refused "
          "(__main__.HeaderTest"),
    Fault("gate-seeds-file-unchecked", "py-scripts", "check_gate.py takes a gate file that names "
          "another list of seeds, which the page prints", CHECK_FAILED,
          "FAIL: test_another_list_named_is_refused (__main__.HeaderTest"),
    Fault("gate-seeds-digest-unchecked", "py-scripts", "check_gate.py takes a gate file whose "
          "list's SHA-256 is not the tree's fuzz/bound_seeds.txt's", CHECK_FAILED,
          "FAIL: test_a_list_digest_not_the_trees_is_refused (__main__.HeaderTest"),
    Fault("gate-bar-text-unchecked", "py-scripts", "check_gate.py takes a gate file whose bar is "
          "given in other words", CHECK_FAILED, "FAIL: test_a_bar_in_other_words_is_refused "
          "(__main__.HeaderTest"),
    Fault("gate-statement-unchecked", "py-scripts", "check_gate.py takes a gate file whose "
          "statement drops the finding from the pass rule", CHECK_FAILED,
          "FAIL: test_another_statement_is_refused (__main__.HeaderTest"),
    Fault("gate-assembled-form-unchecked", "py-scripts", "check_gate.py takes a gate file that "
          "says its runs were timed again", CHECK_FAILED,
          "FAIL: test_an_assembly_in_other_words_is_refused (__main__.HeaderTest"),
    Fault("gate-assembled-days-unchecked", "py-scripts", "check_gate.py takes a gate file that "
          "gives its runs' days otherwise than its runs do", CHECK_FAILED,
          "FAIL: test_an_assembly_from_runs_of_other_days_is_refused (__main__.HeaderTest"),
    Fault("gate-assembled-day-unchecked", "py-scripts", "check_gate.py takes a gate file "
          "assembled on the 45th of a 13th month", CHECK_FAILED,
          "FAIL: test_an_assembly_on_no_day_is_refused (__main__.HeaderTest"),
    Fault("gate-hosts-unchecked", "py-scripts", "check_gate.py takes a gate file that gives no "
          "host's facts", CHECK_FAILED, "FAIL: test_no_hosts_facts_are_refused "
          "(__main__.HeaderTest"),
    Fault("gate-hosts-order-unchecked", "py-scripts", "check_gate.py takes a gate file whose "
          "hosts are not in the assembler's order", CHECK_FAILED,
          "FAIL: test_hosts_in_another_order_are_refused (__main__.HeaderTest"),
    Fault("gate-more-runs-accepted", "py-scripts",
          "check_gate.py takes a gate file that ran each seed six times, where the gate requires "
          "five", CHECK_FAILED, "FAIL: test_more_runs_than_the_gate_requires_are_refused "
          "(__main__.CoverageTest"),
    # The summary is compared with its rows by type as well as value.
    Fault("gate-summary-compared-by-value", "py-scripts", "check_gate.py compares the summary "
          "with its rows by value alone, so 1 stands for true and 2.0 for 2", CHECK_FAILED,
          "FAIL: test_an_all_pass_of_one_for_true_is_refused (__main__.SummaryTest"),
    Fault("gate-summary-missing-field-accepted", "py-scripts", "check_gate.py takes a summary "
          "that leaves out a field its rows give", CHECK_FAILED,
          "FAIL: test_a_summary_missing_a_field_is_refused (__main__.SummaryTest"),
    Fault("gate-summary-list-length-unchecked", "py-scripts", "check_gate.py takes a worst run "
          "tied with a run that is not, as it compares a list only as far as the shorter goes",
          CHECK_FAILED, "FAIL: test_a_worst_run_tied_with_a_run_that_is_not_is_refused "
          "(__main__.SummaryTest"),
    # scripts/gate_results.py, which wrote the gate file from the runs' raw records: each refusal of
    # its records and arguments, and the pass rule's parts.
    Fault("gate-results-list-line-unchecked", "py-scripts",
          "gate_results.py takes a list line of three words", CHECK_FAILED,
          "FAIL: test_a_list_line_that_is_not_target_and_seed_is_refused (__main__.RefusedTest"),
    Fault("gate-results-list-comment-a-seed", "py-scripts",
          "gate_results.py reads a comment line of fuzz/bound_seeds.txt as a seed, and refuses it",
          CHECK_FAILED, "ERROR: test_a_comment_in_the_list_is_no_seed (__main__.AssembleTest"),
    Fault("gate-results-list-target-unprefixed", "py-scripts", "gate_results.py reads a list's "
          "target named without fuzz_ as another target than the gate's", CHECK_FAILED,
          "ERROR: test_a_target_named_without_its_prefix_is_its_fuzz_target "
          "(__main__.AssembleTest"),
    Fault("gate-results-no-timeout-assumed", "py-scripts", "gate_results.py takes a launch file "
          "with no -timeout as -timeout=10", CHECK_FAILED,
          "FAIL: test_a_launch_file_with_no_timeout_is_refused (__main__.RefusedTest"),
    Fault("gate-results-timeout-first-of-two", "py-scripts", "gate_results.py takes the first of "
          "two -timeout flags as the bar's", CHECK_FAILED,
          "FAIL: test_a_launch_file_with_two_timeouts_is_refused (__main__.RefusedTest"),
    Fault("gate-results-counters-processes-unchecked", "py-scripts", "gate_results.py takes a "
          "copy of the counters with no processes line, and raises", CHECK_FAILED,
          "ERROR: test_counters_without_a_processes_line_are_refused (__main__.RefusedTest"),
    Fault("gate-results-iowait-busy", "py-scripts",
          "gate_results.py counts iowait as busy, which the gate did not", CHECK_FAILED,
          "ERROR: test_iowait_counts_as_idle (__main__.AssembleTest"),
    Fault("gate-results-counters-cpus-unchecked", "py-scripts",
          "gate_results.py takes two copies of the counters that name other cpus, and raises",
          CHECK_FAILED, "ERROR: test_counters_naming_other_cpus_are_refused (__main__.RefusedTest"),
    Fault("gate-results-counters-no-time-accepted", "py-scripts",
          "gate_results.py takes counters over which no time passed on a cpu, and divides by it",
          CHECK_FAILED, "ERROR: test_counters_over_which_no_time_passed_are_refused "
          "(__main__.RefusedTest"),
    Fault("gate-results-executed-twice-accepted", "py-scripts",
          "gate_results.py takes a log with two Executed lines", CHECK_FAILED,
          "FAIL: test_a_log_with_two_executed_lines_is_refused (__main__.RefusedTest"),
    Fault("gate-results-executed-other-input-accepted", "py-scripts", "gate_results.py takes a "
          "log that executed another input than its seed", CHECK_FAILED,
          "FAIL: test_a_log_that_executed_another_input_is_refused (__main__.RefusedTest"),
    Fault("gate-results-units-twice-accepted", "py-scripts", "gate_results.py takes a log with "
          "two unit counts", CHECK_FAILED, "FAIL: test_a_log_with_two_unit_counts_is_refused "
          "(__main__.RefusedTest"),
    Fault("gate-results-no-unit-accepted", "py-scripts", "gate_results.py takes a log of no "
          "executed unit, and divides by it", CHECK_FAILED,
          "ERROR: test_a_log_with_no_executed_unit_is_refused (__main__.RefusedTest"),
    Fault("gate-results-findings-not-read", "py-scripts", "gate_results.py reads no finding in a "
          "run's log, so a run that crashed with one gives none", CHECK_FAILED,
          "FAIL: test_a_run_that_crashed_keeps_its_findings (__main__.AssembleTest"),
    Fault("gate-results-table-columns-unchecked", "py-scripts", "gate_results.py takes the "
          "runner's table with a column named otherwise", CHECK_FAILED,
          "FAIL: test_a_runners_table_of_other_columns_is_refused (__main__.RefusedTest"),
    Fault("gate-results-first-of-two-steps", "py-scripts", "gate_results.py takes the first of "
          "two gate folders in a host's records", CHECK_FAILED,
          "FAIL: test_two_gate_folders_are_refused (__main__.RefusedTest"),
    Fault("gate-results-first-of-two-bars", "py-scripts",
          "gate_results.py takes the first of two lines of the gate's list and bar", CHECK_FAILED,
          "FAIL: test_a_step_log_with_two_bar_lines_is_refused (__main__.RefusedTest"),
    Fault("gate-results-other-label-taken", "py-scripts", "gate_results.py takes a start line of "
          "another run label as the run's", CHECK_FAILED,
          "FAIL: test_a_start_line_of_another_runs_label_is_not_the_runs (__main__.AssembleTest"),
    Fault("gate-results-other-step-start-taken", "py-scripts", "gate_results.py takes a start "
          "line of another step as the run's", CHECK_FAILED,
          "FAIL: test_a_start_line_of_another_step_is_not_the_runs (__main__.AssembleTest"),
    Fault("gate-results-other-step-figures-taken", "py-scripts",
          "gate_results.py takes a line of figures of another step as the run's", CHECK_FAILED,
          "ERROR: test_a_line_of_figures_of_another_step_is_not_the_runs (__main__.AssembleTest"),
    Fault("gate-results-no-start-raises", "py-scripts", "gate_results.py reads a run with no "
          "start line, and raises", CHECK_FAILED, "ERROR: test_a_run_with_no_start_is_refused "
          "(__main__.RefusedTest"),
    Fault("gate-results-no-end-raises", "py-scripts", "gate_results.py reads a run with no end "
          "line, and raises", CHECK_FAILED, "ERROR: test_a_missing_run_is_refused "
          "(__main__.RefusedTest"),
    Fault("gate-results-no-figures-raises", "py-scripts", "gate_results.py reads a run with no "
          "line of figures, and raises", CHECK_FAILED,
          "ERROR: test_a_run_with_no_line_of_figures_is_refused (__main__.RefusedTest"),
    Fault("gate-results-no-table-row-raises", "py-scripts", "gate_results.py reads a run with no "
          "row in the runner's table, and raises", CHECK_FAILED,
          "ERROR: test_a_run_with_no_table_row_is_refused (__main__.RefusedTest"),
    Fault("gate-results-other-seed-taken", "py-scripts", "gate_results.py takes a step log or "
          "table row that names another seed for a run", CHECK_FAILED,
          "FAIL: test_a_step_log_naming_another_seed_is_refused (__main__.RefusedTest"),
    Fault("gate-results-wall-unchecked", "py-scripts", "gate_results.py takes a Linux run's wall "
          "time that is not its step log's", CHECK_FAILED,
          "FAIL: test_a_wall_time_not_the_step_logs_is_refused (__main__.RefusedTest"),
    Fault("gate-results-shares-unchecked", "py-scripts", "gate_results.py takes busy shares, a "
          "busy process or a wall time that are not the step log's", CHECK_FAILED,
          "FAIL: test_counters_that_disagree_with_the_step_log_are_refused (__main__.RefusedTest"),
    Fault("gate-results-figures-unchecked", "py-scripts",
          "gate_results.py takes a step log whose figures are not the libFuzzer output's",
          CHECK_FAILED, "FAIL: test_a_step_log_that_disagrees_with_a_log_is_refused "
          "(__main__.RefusedTest"),
    Fault("gate-results-table-unchecked", "py-scripts", "gate_results.py takes a runner's table "
          "that disagrees with the records", CHECK_FAILED,
          "FAIL: test_a_runners_table_that_disagrees_with_a_log_is_refused (__main__.RefusedTest"),
    Fault("gate-results-outcome-unchecked", "py-scripts", "gate_results.py takes a runner's "
          "outcome its records do not give", CHECK_FAILED,
          "FAIL: test_a_runners_outcome_its_records_do_not_give_is_refused (__main__.RefusedTest"),
    Fault("gate-results-pass-rule-findings-ignored", "py-scripts", "gate_results.py's pass rule "
          "leaves out findings", CHECK_FAILED, "ERROR: test_a_run_with_a_finding_does_not_pass "
          "(__main__.AssembleTest"),
    Fault("gate-results-pass-rule-exit-ignored", "py-scripts", "gate_results.py's pass rule "
          "leaves out the exit code", CHECK_FAILED,
          "ERROR: test_a_run_that_exited_otherwise_does_not_pass (__main__.AssembleTest"),
    Fault("gate-results-pass-rule-sibling-ignored", "py-scripts", "gate_results.py's pass rule "
          "leaves out the sibling's busy process", CHECK_FAILED,
          "ERROR: test_a_linux_run_whose_sibling_stopped_does_not_pass (__main__.AssembleTest"),
    Fault("gate-results-pass-rule-bar-ignored", "py-scripts",
          "gate_results.py's pass rule leaves out the bar", CHECK_FAILED,
          "ERROR: test_a_run_over_the_bar_does_not_pass (__main__.AssembleTest"),
    Fault("gate-results-table-count-unchecked", "py-scripts", "gate_results.py takes a runner's "
          "table with a run its records do not hold", CHECK_FAILED,
          "FAIL: test_a_runners_table_with_a_run_too_many_is_refused (__main__.RefusedTest"),
    Fault("gate-results-gate-verdicts-unread", "py-scripts", "gate_results.py does not hold the "
          "runs to the gate's own verdicts", CHECK_FAILED,
          "FAIL: test_a_gate_verdict_its_runs_do_not_give_is_refused (__main__.RefusedTest"),
    Fault("gate-results-gate-verdict-missing-raises", "py-scripts",
          "gate_results.py reads a gate table without a seed's verdict, and raises", CHECK_FAILED,
          "ERROR: test_a_gate_table_without_a_seed_is_refused (__main__.RefusedTest"),
    Fault("gate-results-gate-median-unchecked", "py-scripts", "gate_results.py takes a gate "
          "median its runs do not give", CHECK_FAILED,
          "FAIL: test_a_gate_median_its_runs_do_not_give_is_refused (__main__.RefusedTest"),
    Fault("gate-results-gate-limit-unchecked", "py-scripts", "gate_results.py takes a gate "
          "verdict judged against another limit than the bar", CHECK_FAILED,
          "FAIL: test_a_gate_limit_not_the_bar_is_refused (__main__.RefusedTest"),
    Fault("gate-results-gate-verdict-unchecked", "py-scripts", "gate_results.py takes a gate "
          "verdict that fails a seed whose runs passed", CHECK_FAILED,
          "FAIL: test_a_gate_failing_a_seed_whose_runs_passed_is_refused (__main__.RefusedTest"),
    Fault("gate-results-list-count-unchecked", "py-scripts", "gate_results.py takes a gate that "
          "ran a list of another count of seeds", CHECK_FAILED,
          "FAIL: test_a_list_of_another_length_is_refused (__main__.RefusedTest"),
    Fault("gate-results-list-digest-unchecked", "py-scripts", "gate_results.py takes a gate that "
          "ran a list whose SHA-256 is not the tree's", CHECK_FAILED,
          "FAIL: test_a_list_not_the_trees_is_refused (__main__.RefusedTest"),
    Fault("gate-results-bar-timeout-unchecked", "py-scripts",
          "gate_results.py takes a gate whose bar came from another -timeout than the tree's",
          CHECK_FAILED, "FAIL: test_a_bar_from_another_timeout_is_refused (__main__.RefusedTest"),
    Fault("gate-results-bar-half-unchecked", "py-scripts", "gate_results.py takes a gate whose "
          "bar is not half its -timeout", CHECK_FAILED,
          "FAIL: test_a_bar_not_half_its_timeout_is_refused (__main__.RefusedTest"),
    Fault("gate-results-hosts-runs-unchecked", "py-scripts", "gate_results.py takes hosts that "
          "ran each seed a different number of times", CHECK_FAILED,
          "FAIL: test_hosts_that_ran_each_seed_a_different_number_of_times_are_refused "
          "(__main__.RefusedTest"),
    Fault("gate-results-commit-unchecked", "py-scripts", "gate_results.py takes records of "
          "another commit than the one it is given", CHECK_FAILED,
          "FAIL: test_another_commit_is_refused (__main__.RefusedTest"),
    Fault("gate-results-list-order-unchecked", "py-scripts",
          "gate_results.py takes a gate's list in another order than fuzz/bound_seeds.txt",
          CHECK_FAILED, "FAIL: test_seeds_in_another_order_are_refused (__main__.RefusedTest"),
    Fault("gate-results-commit-argument-unchecked", "py-scripts", "gate_results.py takes a "
          "--commit that is not a full commit", CHECK_FAILED,
          "FAIL: test_a_commit_that_is_not_a_full_hash_is_a_usage_error (__main__.MainTest"),
    Fault("gate-results-day-argument-unchecked", "py-scripts", "gate_results.py takes an "
          "--assembled-on that is not YYYY-MM-DD", CHECK_FAILED,
          "FAIL: test_a_day_that_is_not_a_date_is_a_usage_error (__main__.MainTest"),
    Fault("gate-results-host-name-unchecked", "py-scripts", "gate_results.py takes a --host of "
          "another name than macOS or Linux", CHECK_FAILED,
          "FAIL: test_a_host_of_another_name_is_a_usage_error (__main__.MainTest"),
    Fault("gate-results-host-folder-unchecked", "py-scripts", "gate_results.py takes a --host "
          "with no folder", CHECK_FAILED, "FAIL: test_a_host_with_no_folder_is_a_usage_error "
          "(__main__.MainTest"),
    Fault("gate-results-host-twice-accepted", "py-scripts", "gate_results.py takes the same "
          "--host twice", CHECK_FAILED, "FAIL: test_a_host_given_twice_is_a_usage_error "
          "(__main__.MainTest"),
    Fault("gate-results-refusal-exits-0", "py-scripts", "gate_results.py exits 0 when it refuses "
          "the records", CHECK_FAILED, "FAIL: test_records_that_disagree_make_no_file "
          "(__main__.MainTest"),
    Fault("ci-shallow-checkout", "py-scripts", "CI checks out the single commit it was pushed "
          "at, so G-3's file, which judges an older one, could never be tied there",
          CHECK_FAILED, "FAIL: test_the_workflow_checks_out_the_history_g3s_tie_needs "
          "(__main__.CheckScript"),
    # Review E-1: G-2's exit codes are the ones its runs exited with.
    Fault("store-results-corrupt-exit-unchecked", "py-scripts", "store_results.py writes G-2's "
          "corrupt exit code as the constant 1 and takes a corrupt run that exited 2 or 0",
          CHECK_FAILED, "FAIL: test_a_run_that_exited_with_the_wrong_code_is_refused "
          "(__main__.StoreResults"),
    Fault("roundtrip-corrupt-summary-unchecked", "py-scripts", "check_roundtrip.py stops "
          "holding G-2's exit_code_corrupt to each corrupt run's exit code", CHECK_FAILED,
          "FAIL: test_a_summary_no_run_bears_out_is_refused (__main__.CheckRoundtrip"),
    Fault("roundtrip-exit-codes-typed", "render-check", "12302019's round-trip file with "
          "G-2's two exit codes typed and none observed on its runs, as the assembler wrote "
          "them before review E-1", EXIT_RENDER_REFUSED,
          "g2.data_crc: no exit_code observed", roundtrip_exit_codes_typed),
    # Review RG-5: the files from before the exit codes were replaced at the freeze, and no
    # name takes a file past its runs' exit codes.
    Fault("pins-roundtrip-line-restored", "py-scripts", "a pin of 12302019's round-trip file "
          "from before the exit codes put back in scripts/data_pins.json", CHECK_FAILED,
          "FAIL: test_the_pins_file_holds_no_round_trip_pin (__main__.CheckRoundtrip",
          roundtrip_pin_restored),
    Fault("roundtrip-old-form-by-name-accepted", "py-scripts", "check_roundtrip.py takes a "
          "file under one of the three names the files from before the exit codes had, "
          "whatever it holds", CHECK_FAILED,
          "FAIL: test_the_old_form_is_refused_under_a_committed_name (__main__.CheckRoundtrip"),
    # Review F-4 and RF-4: the tail figure is named for what it is.
    Fault("render-tail-called-p99", "py-scripts", "render_results.py heads the warm tail "
          "column \"Warm p99 ms\" again, the slowest of 30 presented as a 99th percentile",
          CHECK_FAILED, "FAIL: test_the_tail_figure_is_named_for_what_it_is_with_its_n "
          "(__main__.RenderCompareTest"),
    Fault("render-tail-name-unchecked", "py-scripts", "render_results.py calls a protocol 1 "
          "tail figure the slowest of n without holding it to the block's slowest run",
          CHECK_FAILED, "FAIL: test_a_block_whose_figure_is_not_what_its_n_makes_it_is_refused "
          "(__main__.TailNameTest"),
    Fault("render-ql4-p99-unexplained", "py-scripts", "render_results.py gives QL-4's verdict "
          "with no word that its p99 was measured as the slowest of 30 and of 7", CHECK_FAILED,
          "FAIL: test_each_pair_names_its_tail_figure_and_the_verdict_says_what_p99_was "
          "(__main__.RenderLowestTest"),
    # Review RF-3: write targets judged on both clocks.
    Fault("render-write-engine-only-guessed", "py-scripts", "render_results.py takes a write's "
          "median seconds less its median Python seconds as its engine-only seconds over several "
          "repetitions, a guess where the file keeps no engine-only figure", CHECK_FAILED,
          "FAIL: test_a_write_with_no_engine_only_figure_is_not_judged_on_it "
          "(__main__.RenderWriteTest"),
    Fault("targets-write-total-only", "py-scripts", "targets.json judges WT-4 against Parquet "
          "on the total seconds twice, the engine-only condition dropped", CHECK_FAILED,
          "FAIL: test_the_repos_own_write_targets_are_judged_on_both_clocks "
          "(__main__.RenderWriteTest"),
    # Review RV-1: a tick-store-alone day and the four-system day never stand in for each other.
    Fault("render-alone-day-in-four-system-glob", "py-scripts", "render_results.py lets the "
          "four-system day's pattern match a tick-store-alone day, so Linux-<day>-full-*.json "
          "reads tick-store alone, or two files", CHECK_FAILED,
          "FAIL: test_a_linux_alone_day_beside_the_four_system_day_is_not_read_for_it "
          "(__main__.RenderDayKindsTest"),
    Fault("render-alone-scope-unchecked", "py-scripts", "render_results.py reads a file named "
          "as tick-store alone without holding it to tick-store alone's scope", CHECK_FAILED,
          "FAIL: test_a_four_system_day_never_stands_in_for_tickstore_alone "
          "(__main__.RenderDayKindsTest"),
    Fault("render-linux-alone-time-memory", "py-scripts", "render_results.py reads BSD time "
          "-l's memory for a Linux tick-store-alone day, whose memory is the guard's",
          CHECK_FAILED, "FAIL: test_a_linux_tickstore_alone_day_renders_with_the_guards_memory "
          "(__main__.RenderCompareTest"),
    # Review E-2: load lines that are not a list hold no lock proof.
    Fault("load-lines-not-a-list-accepted", "py-scripts", "check_results.py takes load_lines "
          "that are not a list as no problem, for a bench or a comparison file", CHECK_FAILED,
          "FAIL: test_load_lines_that_are_not_a_list (__main__.Faults"),
    Fault("compare-load-lines-a-string", "check-compare", "a comparison file whose load_lines "
          "is one string, with no list of the lock's start and end lines", CHECK_FAILED,
          "header.load_lines: not a list of measure.sh load lines", load_lines_as_a_string),
    # Review E-4: only an exact codec meets a compression target.
    Fault("render-best-codec-lossy", "py-scripts", "render_results.py takes the fewest bits of "
          "any codec on a column, a lossy one included, as its best", CHECK_FAILED,
          "FAIL: test_the_best_codec_is_the_best_exact_one (__main__.RenderTargetsTest"),
    # Review E-7: a condition only shown never hides a miss.
    Fault("render-shown-condition-hides-miss", "py-scripts", "render_results.py judges a part "
          "on every condition, one with no target included, so a shown figure turns a miss "
          "beside it into not judged", CHECK_FAILED,
          "FAIL: test_a_shown_condition_beside_a_judged_one_hides_no_verdict "
          "(__main__.RenderTargetsTest"),
    # Review E-8: the figures QL-4 shows are the ones its verdict compares with.
    Fault("render-lowest-shows-one-system", "py-scripts", "render_results.py shows every field "
          "of the system with the lowest median, so a tail figure that decides a miss is "
          "another system's and is not shown", CHECK_FAILED,
          "FAIL: test_each_figure_shown_is_the_lowest_of_the_others_and_names_its_system "
          "(__main__.RenderLowestTest"),
    # Review E-9: the README's pitch figures are checked against the results.
    Fault("readme-pitch-figure-edited", "render-check", "a hand edit of a figure in the "
          "README's pitch: the store's size raised by one GB", EXIT_RENDER_REFUSED,
          "the README's pitch does not state \"a day's 268,744,780 messages in",
          hand_edit_pitch_figure),
    Fault("readme-pitch-unchecked-figure", "render-check", "a figure typed into the README's "
          "pitch that nothing checks", EXIT_RENDER_REFUSED,
          "a figure in the README's pitch that nothing checks: '42'",
          type_an_unchecked_pitch_figure),
    # Reviews RG-4 and RG-13: a pitch figure is bound to its words, and a ranking to the results.
    Fault("readme-pitch-store-figures-swapped", "render-check", "the README's pitch with the "
          "source's size and the store's traded, both still figures the results give",
          EXIT_RENDER_REFUSED, "the README's pitch does not state \"a day's 268,744,780 messages in",
          swap_pitch_store_sizes),
    Fault("render-pitch-shared-format-accepted", "py-scripts", "render_results.py takes two "
          "pitch figures of one format, which could trade places in the text unseen",
          CHECK_FAILED,
          "FAIL: test_two_figures_of_one_format_are_refused (__main__.RenderPitchTest"),
    Fault("readme-pitch-claim-reworded", "render-check", "the pitch's query sentence "
          "reworded to rank tick-store first on short market-wide windows, its Q1 and Q4 "
          "clause, which holds no figure", EXIT_RENDER_REFUSED,
          "the README's pitch no longer says \"On the Ryzen it answers one stock's hour",
          reword_a_pitch_claim),
    Fault("render-pitch-claim-unchecked", "py-scripts", "render_results.py finds each claim's "
          "words in the pitch and never judges its condition, so a ranking the results reverse "
          "still passes", CHECK_FAILED,
          "FAIL: test_a_claim_the_results_no_longer_bear_out_stops_the_check "
          "(__main__.RenderPitchTest"),
    # The first screen's rules: a ranking keeps its tie, the prose stays short, the table names
    # no id and types no figure, and the line under it hides no miss.
    Fault("readme-pitch-claim-tie-dropped", "render-check", "the claim tying the pitch's "
          "one-thread ingest, below Nasdaq's peak, to its verdict taken out of targets.json",
          EXIT_RENDER_REFUSED, "the README's pitch ranks or judges with 'below' in the sentence",
          drop_a_pitch_claims_tie),
    Fault("render-pitch-word-limit-removed", "py-scripts", "render_results.py stops holding "
          "the pitch to its max_words, so the prose before the table can grow unseen",
          CHECK_FAILED, "FAIL: test_a_pitch_over_its_word_limit_stops_the_check "
          "(__main__.RenderPitchTest"),
    # Review DX-2: a claim is its whole sentence, holds its target's words, and every sentence
    # of the pitch is a claim, holds a figure or is listed as stating no result.
    Fault("render-pitch-claim-a-phrase", "py-scripts", "render_results.py finds a claim's text "
          "anywhere in the pitch, so words around a tied ranking widen, move or negate it "
          "unseen", CHECK_FAILED,
          "FAIL: test_a_claim_is_its_whole_sentence (__main__.RenderPitchTest"),
    Fault("render-pitch-sentence-unaccounted", "py-scripts", "render_results.py takes a pitch "
          "sentence that is no claim, holds no figure and is not listed as stating no result",
          CHECK_FAILED, "FAIL: test_each_sentence_is_a_claim_holds_a_figure_or_is_listed_as_"
          "stating_no_result (__main__.RenderPitchTest"),
    Fault("render-pitch-claim-other-target", "py-scripts", "render_results.py takes a claim "
          "tied to a target whose bar its sentence does not name, so it could rest on another "
          "target of the same verdict", CHECK_FAILED,
          "FAIL: test_a_claim_holds_the_words_its_target_lists_for_its_bar "
          "(__main__.RenderPitchTest"),
    # Review DX-14: a claim names its one machine, and fastest is judged on every field named.
    Fault("render-pitch-claim-machine-unnamed", "py-scripts", "render_results.py takes a claim "
          "judged on one machine whose sentence does not name it", CHECK_FAILED,
          "FAIL: test_a_claim_judged_on_one_machine_names_it_and_no_other "
          "(__main__.RenderPitchTest"),
    Fault("render-pitch-fastest-median-only", "py-scripts", "render_results.py judges a fastest "
          "claim on its first field alone, the median, under a bar of median and tail",
          CHECK_FAILED, "FAIL: test_a_fastest_claim_is_judged_on_every_field_it_names "
          "(__main__.RenderPitchTest"),
    # Review DX-3: headings are on the first screen, under its rules and its word limit.
    Fault("render-pitch-headings-skipped", "py-scripts", "render_results.py leaves the pitch's "
          "headings out of its ranking rule", CHECK_FAILED,
          "FAIL: test_headings_and_hash_lines_are_held_to_the_ranking_rule "
          "(__main__.RenderPitchTest"),
    Fault("render-pitch-word-limit-skips-a-heading", "py-scripts", "render_results.py leaves "
          "the pitch's first heading out of its word count", CHECK_FAILED,
          "FAIL: test_the_word_limit_counts_headings_with_the_prose (__main__.RenderPitchTest"),
    # Reviews DX-10 and DX-11: the ranking words, and what the figure check read past.
    Fault("render-pitch-ranking-words-dropped", "py-scripts", "render_results.py's ranking "
          "words without the ones that passed false sentences, less, ahead, keeps up and the "
          "rest", CHECK_FAILED,
          "FAIL: test_the_ranking_words_include_the_ones_that_passed_false_sentences "
          "(__main__.RenderPitchTest"),
    Fault("render-pitch-not-result-reused", "py-scripts", "render_results.py keeps a not_results "
          "text out of the figure check wherever it is found, so its digits pass in any "
          "sentence", CHECK_FAILED,
          "FAIL: test_a_text_kept_out_of_the_check_is_kept_out_once (__main__.RenderPitchTest"),
    Fault("render-screen-invisible-accepted", "py-scripts", "render_results.py takes a "
          "zero-width character on the first screen, which joins words for the count",
          CHECK_FAILED, "FAIL: test_an_invisible_or_a_numeric_character_is_refused "
          "(__main__.RenderPitchTest"),
    Fault("render-pitch-numeric-character-accepted", "py-scripts", "render_results.py takes a "
          "figure written as a vulgar fraction, which the digit pattern misses", CHECK_FAILED,
          "FAIL: test_an_invisible_or_a_numeric_character_is_refused (__main__.RenderPitchTest"),
    Fault("render-screen-after-the-headline-unread", "py-scripts", "render_results.py reads "
          "nothing between the headline's end marker and the next heading, still on the first "
          "screen", CHECK_FAILED,
          "FAIL: test_text_after_the_headline_before_a_heading_is_refused "
          "(__main__.RenderHeadlineTest"),
    # Review DX-4: the line under the table and the sentence above it are checked apart from
    # their writer.
    Fault("headline-left-out-writer-class-shared", "py-scripts", "render_results.py's check of "
          "the line under the table counts the other rows by the writer's own verdict classes, "
          "so a fault there passes both", CHECK_FAILED,
          "FAIL: test_a_writer_whose_classes_count_a_part_met_row_as_met_is_refused "
          "(__main__.LeftOutLineTest"),
    Fault("headline-left-out-machine-unchecked", "py-scripts", "render_results.py's check of "
          "the line under the table finds each name under any machine", CHECK_FAILED,
          "FAIL: test_a_writer_that_names_the_wrong_machine_is_refused "
          "(__main__.LeftOutLineTest"),
    Fault("headline-left-out-named-count-unchecked", "py-scripts", "render_results.py's check "
          "of the line under the table drops the targets named among the counts", CHECK_FAILED,
          "FAIL: test_a_writer_that_names_a_target_under_another_count_is_refused "
          "(__main__.LeftOutLineTest"),
    Fault("headline-left-out-extra-name-accepted", "py-scripts", "render_results.py's check of "
          "the line under the table takes a name the targets do not leave out", CHECK_FAILED,
          "FAIL: test_a_name_the_targets_do_not_leave_out_is_refused (__main__.LeftOutLineTest"),
    Fault("headline-under-verdict-unchecked", "py-scripts", "render_results.py does not check "
          "the verdict of the sentence under the table apart from its writer", CHECK_FAILED,
          "FAIL: test_a_sentence_under_the_table_that_always_says_met_is_refused "
          "(__main__.LeftOutLineTest"),
    # Review DX-5: "vs" the best of the others is the best of every other system.
    Fault("headline-lowest-of-a-subset", "py-scripts", "render_results.py shows the lowest of "
          "some of the other systems as the best of them", CHECK_FAILED,
          "FAIL: test_the_lowest_of_the_others_takes_every_other_system "
          "(__main__.RenderHeadlineTest"),
    # Review DX-13: the line is read back name by name.
    Fault("headline-plain-name-shared", "py-scripts", "render_results.py takes one plain name "
          "for two targets, or two parts, on one machine", CHECK_FAILED,
          "FAIL: test_a_plain_name_is_whole_and_one_parts_on_a_machine "
          "(__main__.RenderHeadlineTest"),
    Fault("compare-replaced-favourable-counted", "py-scripts", "render_results.py counting the "
          "favourable of a part's outcome on this run and on the replaced run", CHECK_FAILED,
          "FAIL: test_a_changed_outcome_shows_both_and_counts_the_less_favourable "
          "(__main__.ReplacedRunTest"),
    Fault("compare-replaced-figures-hidden", "py-scripts", "render_results.py showing this run's "
          "figures alone for a part whose outcome the replaced run's figures change", CHECK_FAILED,
          "FAIL: test_a_changed_outcome_shows_both_and_counts_the_less_favourable "
          "(__main__.ReplacedRunTest"),
    Fault("compare-replaced-unpinned-accepted", "py-scripts", "render_results.py reading a "
          "replaced comparison file that no pin holds to its bytes", CHECK_FAILED,
          "FAIL: test_a_replaced_file_must_be_pinned_by_its_bytes "
          "(__main__.ReplacedRunTest"),
    Fault("compare-replaced-change-unmarked", "py-scripts", "render_results.py giving a changed "
          "part's counted verdict with no word beside it of both outcomes and the one that "
          "favours tick-store", CHECK_FAILED,
          "FAIL: test_a_changed_outcome_shows_both_and_counts_the_less_favourable "
          "(__main__.ReplacedRunTest"),
    Fault("compare-replaced-clock-unsaid", "py-scripts", "render_results.py judging a replaced "
          "file on the clock it keeps without saying which", CHECK_FAILED,
          "FAIL: test_an_older_file_is_judged_on_the_clock_it_keeps_and_says_so "
          "(__main__.ReplacedRunTest"),
    Fault("compare-counted-trusts-the-writer", "py-scripts", "render_results.py's checks taking "
          "the writer's verdict for a part's counted outcome instead of the less favourable of "
          "both runs", CHECK_FAILED,
          "FAIL: test_counts_and_claims_take_the_less_favourable_worked_out_from_both "
          "(__main__.ReplacedRunTest"),
    Fault("compare-claim-takes-this-run", "py-scripts", "render_results.py judging a pitch claim "
          "on this run's outcome where the replaced run's is less favourable", CHECK_FAILED,
          "FAIL: test_counts_and_claims_take_the_less_favourable_worked_out_from_both "
          "(__main__.ReplacedRunTest"),
    Fault("compare-disturbance-unmarked", "py-scripts", "render_results.py giving a verdict that "
          "rests on a disturbed timed phase with nothing beside it", CHECK_FAILED,
          "FAIL: test_a_disturbed_timed_phase_is_named_with_the_replaced_figure "
          "(__main__.ReplacedRunTest"),
    Fault("compare-disturbance-variant-ignored", "py-scripts", "render_results.py naming a "
          "disturbance of another variant beside a verdict", CHECK_FAILED,
          "FAIL: test_an_untimed_phase_or_another_variant_is_not_named "
          "(__main__.ReplacedRunTest"),
    Fault("compare-disturbance-commit-unchecked", "py-scripts", "render_results.py taking a "
          "disturbance file whose entry falls in a document measured at another commit", CHECK_FAILED,
          "FAIL: test_a_disturbance_file_is_refused_unless_whole "
          "(__main__.ReplacedRunTest"),
    Fault("compare-disturbance-timed-unchecked", "py-scripts", "render_results.py taking a "
          "disturbance entry that calls a timed phase untimed", CHECK_FAILED,
          "FAIL: test_a_disturbance_file_is_refused_unless_whole "
          "(__main__.ReplacedRunTest"),
    Fault("compare-disturbance-span-unchecked", "py-scripts", "render_results.py taking a "
          "disturbance entry that ends before it starts", CHECK_FAILED,
          "FAIL: test_a_disturbance_file_is_refused_unless_whole "
          "(__main__.ReplacedRunTest"),
    Fault("compare-write-inputs-dropped", "py-scripts", "render_results.py giving a write verdict "
          "against other systems with no word of whose input was where", CHECK_FAILED,
          "FAIL: test_a_write_says_whose_input_was_where_from_its_record "
          "(__main__.ReplacedRunTest"),
    Fault("compare-write-asymmetry-unsaid", "py-scripts", "render_results.py saying where each "
          "write's input was and not that engine-only leaves the difference in", CHECK_FAILED,
          "FAIL: test_a_write_says_whose_input_was_where_from_its_record "
          "(__main__.ReplacedRunTest"),
    Fault("compare-faster-write-accepted", "py-scripts", "render_results.py letting a sentence "
          "say tick-store writes faster than a system whose clock holds its disk reads", CHECK_FAILED,
          "FAIL: test_a_sentence_saying_a_write_is_faster_than_a_disk_read_clock_is_refused "
          "(__main__.ReplacedRunTest"),
    Fault("compare-faster-write-readme-unread", "py-scripts", "render_results.py reading the "
          "generated blocks and the other results pages for a faster write and not the README",
          CHECK_FAILED, "FAIL: test_the_render_refuses_a_page_saying_so "
          "(__main__.ReplacedRunTest"),
    Fault("compare-faster-write-pages-unread", "py-scripts", "render_results.py reading only the "
          "generated blocks for a faster write and not the whole prose of the README, RESULTS.md, "
          "BENCHMARKS.md, compare/README.md and fuzz/README.md", CHECK_FAILED,
          "FAIL: test_every_results_page_is_read_whole_and_named_with_its_line "
          "(__main__.ReplacedRunTest"),
    # Review C1: the four conditions on publishing a comparison verdict.
    Fault("compare-changed-part-under-counted-word", "py-scripts", "render_results.py listing a "
          "part the replaced run changes under its counted word, with the rule's outcome in a "
          "later clause", CHECK_FAILED,
          "FAIL: test_a_changed_outcome_shows_both_and_counts_the_less_favourable "
          "(__main__.ReplacedRunTest"),
    Fault("compare-disturbance-replaced-figure-on-comparisons-only", "py-scripts",
          "render_results.py giving the replaced run's figure beside a disturbed phase only on "
          "a row judged against another system", CHECK_FAILED,
          "FAIL: test_a_disturbed_phase_of_a_row_against_no_system_gives_the_replaced_figure "
          "(__main__.ConditionsTest"),
    Fault("compare-swap-in-unmarked", "py-scripts", "render_results.py giving a verdict that "
          "rests on a figure the record flags as timed with a page swapped in with nothing "
          "beside it", CHECK_FAILED,
          "FAIL: test_a_verdict_on_a_figure_timed_with_a_page_swapped_in_says_so "
          "(__main__.ConditionsTest"),
    Fault("pitch-swap-mark-unchecked", "py-scripts", "render_results.py passing a README pitch "
          "claim that rests on a rival's figure timed with pages swapped in and does not say so",
          CHECK_FAILED,
          "FAIL: test_a_pitch_claim_on_a_swapped_in_figure_says_so_in_its_short_form "
          "(__main__.ConditionsTest"),
    Fault("pitch-swap-mark-own-figure-accepted", "py-scripts", "render_results.py letting the "
          "pitch's short swap-in mark, which speaks of rivals, stand for tick-store's own "
          "flagged figure", CHECK_FAILED,
          "FAIL: test_the_pitchs_short_form_speaks_of_rivals_and_stands_nowhere_else "
          "(__main__.ConditionsTest"),
    Fault("move-cause-unknown-dropped", "py-scripts", "render_results.py stating the moves between "
          "the runs without saying their cause is not known", CHECK_FAILED,
          "FAIL: test_a_move_between_the_runs_says_its_cause_is_not_known "
          "(__main__.ConditionsTest"),
    Fault("move-joined-to-the-inputs", "py-scripts", "render_results.py joining the moves between "
          "the runs to the sentence on the inputs' difference", CHECK_FAILED,
          "FAIL: test_a_move_between_the_runs_says_its_cause_is_not_known "
          "(__main__.ConditionsTest"),
    Fault("move-unknown-unchecked", "py-scripts", "render_results.py passing a page's move between "
          "the runs that does not say its cause is not known", CHECK_FAILED,
          "FAIL: test_a_move_between_the_runs_says_its_cause_is_not_known "
          "(__main__.ConditionsTest"),
    Fault("move-cause-unchecked", "py-scripts", "render_results.py passing a page's move between "
          "the runs to a joined cause", CHECK_FAILED,
          "FAIL: test_a_move_joined_to_a_cause_is_refused "
          "(__main__.ConditionsTest"),
    Fault("keeps-sentence-unchecked", "py-scripts", "render_results.py passing a page that says "
          "more than the replaced record keeps", CHECK_FAILED,
          "FAIL: test_what_the_replaced_record_keeps_is_said_from_its_fields "
          "(__main__.ConditionsTest"),
    Fault("keeps-residency-unread", "py-scripts", "render_results.py reading tick-store's input "
          "residency in the replaced record as absent", CHECK_FAILED,
          "FAIL: test_what_the_replaced_record_keeps_is_said_from_its_fields "
          "(__main__.ConditionsTest"),
    Fault("render-move-checks-unwired", "py-scripts", "render_results.py's render never reading "
          "the pages for moves or the replaced record", CHECK_FAILED,
          "FAIL: test_the_render_refuses_a_page_with_a_move_or_a_record_said_wrong "
          "(__main__.ConditionsTest"),
    Fault("decide-every-compared-figure", "py-scripts", "render_results.py saying a verdict "
          "rests on every figure it compares, a flagged one that did not decide it too",
          CHECK_FAILED, "FAIL: test_a_flagged_figure_that_does_not_decide_the_part_gets_no_rests_on "
          "(__main__.ConditionsTest"),
    Fault("decide-missed-by-every-rival", "py-scripts", "render_results.py taking a rival "
          "tick-store beat as deciding a part it missed", CHECK_FAILED,
          "FAIL: test_a_flagged_figure_that_decides_the_part_keeps_its_mark "
          "(__main__.ConditionsTest"),
    Fault("decide-met-condition-in-a-miss", "py-scripts", "render_results.py taking a condition "
          "met as deciding a part missed on another", CHECK_FAILED,
          "FAIL: test_a_flagged_figure_that_does_not_decide_the_part_gets_no_rests_on "
          "(__main__.ConditionsTest"),
    Fault("decide-standing-ignores-unflagged", "py-scripts", "render_results.py marking a miss "
          "that stands on an unflagged rival figure as resting on a flagged one", CHECK_FAILED,
          "FAIL: test_a_miss_standing_on_an_unflagged_rival_gets_no_rival_mark "
          "(__main__.ConditionsTest"),
    Fault("decide-tainted-unread", "py-scripts", "render_results.py taking no rival figure as "
          "flagged or disturbed when it decides what a miss stands on", CHECK_FAILED,
          "FAIL: test_a_flagged_figure_that_decides_the_part_keeps_its_mark "
          "(__main__.ConditionsTest"),
    Fault("short-form-screen-unmarked", "py-scripts", "render_results.py giving a verdict on "
          "the README's first screen with no short mark beside it", CHECK_FAILED,
          "FAIL: test_the_first_screen_takes_the_short_forms_and_results_md_explains_them "
          "(__main__.ConditionsTest"),
    Fault("short-form-quote-loose", "py-scripts", "render_results.py taking a quote's words "
          "near a short form for the mark", CHECK_FAILED,
          "FAIL: test_a_quote_with_no_mark_or_another_short_form_is_refused "
          "(__main__.ConditionsTest"),
    Fault("short-forms-kinds-unexplained", "py-scripts", "RESULTS.md's note on the short forms "
          "leaving out what each kind of disturbance stands for", CHECK_FAILED,
          "FAIL: test_the_first_screen_takes_the_short_forms_and_results_md_explains_them "
          "(__main__.ConditionsTest"),
    Fault("short-disturbance-unknown-kind", "py-scripts", "render_results.py giving a "
          "disturbance with no defined short form a short mark anyway", CHECK_FAILED,
          "FAIL: test_a_quote_with_no_mark_or_another_short_form_is_refused "
          "(__main__.ConditionsTest"),
    Fault("benchmarks-move-cause-unsaid", "render-check", "BENCHMARKS.md's move of ArcticDB's "
          "write between the runs without its unknown cause", EXIT_RENDER_REFUSED,
          "does not say its cause was not measured and is not known", drop_the_moves_unknown_cause),
    Fault("benchmarks-replaced-record-widened", "render-check", "BENCHMARKS.md saying the "
          "replaced record keeps less than it does, as the review found it", EXIT_RENDER_REFUSED,
          "is not the sentence its fields give", widen_what_the_replaced_record_lacks),
    Fault("render-others-list-unheaded", "py-scripts", "render_results.py taking a first screen "
          "whose list of what the table's targets leave out is gone or above its heading "
          "(review R-A5)", CHECK_FAILED,
          "FAIL: test_the_list_stays_under_its_heading_and_the_count_on_the_screen "
          "(__main__.RenderHeadlineTest"),
    Fault("render-pitch-setup-unsaid", "py-scripts", "render_results.py taking a pitch win "
          "among the chosen setups without the setup that beats it (review R-A1)", CHECK_FAILED,
          "FAIL: test_a_pitch_win_another_setup_beats_names_that_setup (__main__.ConditionsTest"),
    Fault("render-setup-mark-dropped", "py-scripts", "render_results.py giving a part met "
          "among the headline setups no mark for the setup that beats it (review R-A1)",
          CHECK_FAILED, "FAIL: test_a_met_part_says_it_is_among_the_headlines_and_names_a_faster_"
          "variant (__main__.RenderLowestTest"),
    Fault("render-swap-short-per-part", "py-scripts", "render_results.py giving each part's "
          "swap-in short form apart, so one verdict repeats a system (review R-B2)",
          CHECK_FAILED, "FAIL: test_swap_in_marks_name_each_system_once_over_a_verdict "
          "(__main__.ShortFormTest"),
    Fault("render-screen-lean-one-way", "py-scripts", "render_results.py saying on the first "
          "screen that swapping may flatter tick-store when its own runs swapped (review R-A3)",
          CHECK_FAILED, "FAIL: test_the_screen_says_whose_runs_swapped_and_which_way_it_leans "
          "(__main__.ShortFormTest"),
    Fault("render-change-short-lean-unsaid", "py-scripts", "render_results.py's short changed "
          "outcome saying the change favours tick-store whichever way it leans (review R-B1)",
          CHECK_FAILED, "FAIL: test_a_changed_outcome_has_a_short_form_and_a_full_one_true_"
          "anywhere (__main__.ShortFormTest"),
    Fault("render-short-verdict-drops-change", "py-scripts", "render_results.py's short verdict "
          "of several parts giving a changed part's counted word alone (review R-B3)",
          CHECK_FAILED, "FAIL: test_a_verdict_of_several_parts_reads_as_one_sentence "
          "(__main__.ShortFormTest"),
    Fault("disturbance-events-counted-each", "py-scripts", "render_results.py counting one "
          "login listed in three fuzz targets that ran at once as three (review R-D2)",
          CHECK_FAILED, "FAIL: test_a_fuzz_runs_events_mark_its_linux_part_only_each_event_once "
          "(__main__.FuzzDisturbanceTest"),
    Fault("disturbance-fuzz-any-host", "py-scripts", "render_results.py marking G-3's macOS "
          "part with the Linux host's events (review R-D1)", CHECK_FAILED,
          "FAIL: test_a_fuzz_runs_events_mark_its_linux_part_only_each_event_once "
          "(__main__.FuzzDisturbanceTest"),
    Fault("disturbance-fuzz-target-unchecked", "py-scripts", "render_results.py taking a fuzz "
          "run entry whose target the fuzz file has no run of (review R-D1)", CHECK_FAILED,
          "FAIL: test_a_fuzz_record_is_checked_like_any_other (__main__.FuzzDisturbanceTest"),
    Fault("disturbance-kinds-uncounted", "py-scripts", "render_results.py saying a phase of two "
          "logins held a login (review R-D2)", CHECK_FAILED,
          "FAIL: test_several_events_in_one_phase_are_said_by_kind_and_count "
          "(__main__.FuzzDisturbanceTest"),
    Fault("writeup-record-of-ignored", "py-scripts", "render_writeup.py reading a record kind "
          "by its pattern alone, so two records make one token read two files (review R-D1)",
          CHECK_FAILED, "ERROR: test_the_record_of_the_runs_commit_is_read (__main__.RecordOf"),
    Fault("headline-value-mark-dropped", "py-scripts", "render_results.py letting a headline "
          "text show a figure from a disturbed phase with no mark beside it", CHECK_FAILED,
          "FAIL: test_a_headline_figure_from_a_disturbed_phase_must_show_its_mark "
          "(__main__.ConditionsTest"),
    Fault("render-ql4-faster-variant-unmarked", "py-scripts", "render_results.py naming a "
          "faster variant beside a QL-4 part with no word of the disturbance in its phase",
          CHECK_FAILED,
          "FAIL: test_a_faster_variant_shown_beside_a_ranking_carries_its_phases_mark "
          "(__main__.ConditionsTest"),
    Fault("render-disturbed-cells-unmarked", "py-scripts", "render_results.py showing a "
          "comparison table's figure from a disturbed phase unmarked", CHECK_FAILED,
          "FAIL: test_the_tables_mark_a_disturbed_phase_and_say_the_rates_are_not_alike "
          "(__main__.ConditionsTest"),
    Fault("render-ingest-rates-alike", "py-scripts", "render_results.py setting the ingest "
          "rates side by side with no word that the inputs were not alike", CHECK_FAILED,
          "FAIL: test_the_tables_mark_a_disturbed_phase_and_say_the_rates_are_not_alike "
          "(__main__.ConditionsTest"),
    Fault("headline-left-out-mark-dropped", "py-scripts", "render_results.py naming a marked "
          "part on the line under the table without its mark", CHECK_FAILED,
          "FAIL: test_the_line_under_the_table_names_a_marked_part_with_its_mark "
          "(__main__.ConditionsTest"),
    Fault("headline-left-out-mark-unchecked", "py-scripts", "render_results.py's check of the "
          "line under the table taking a marked part's bare name", CHECK_FAILED,
          "FAIL: test_the_line_under_the_table_names_a_marked_part_with_its_mark "
          "(__main__.ConditionsTest"),
    Fault("quote-mark-unchecked", "py-scripts", "render_results.py letting a hand-written row "
          "or sentence quote a marked verdict without its marks", CHECK_FAILED,
          "FAIL: test_a_hand_written_quote_of_a_marked_verdict_is_refused_without_its_marks "
          "(__main__.ConditionsTest"),
    Fault("quote-sentences-unread", "py-scripts", "render_results.py reading only table rows "
          "for a quoted verdict and no sentence", CHECK_FAILED,
          "FAIL: test_a_hand_written_quote_of_a_marked_verdict_is_refused_without_its_marks "
          "(__main__.ConditionsTest"),
    Fault("quote-tables-unread", "py-scripts", "render_results.py reading only sentences for a "
          "quoted verdict and no table row", CHECK_FAILED,
          "FAIL: test_a_hand_written_quote_of_a_swapped_in_verdict_is_refused_without_it "
          "(__main__.ConditionsTest"),
    Fault("quote-input-clause-dropped", "py-scripts", "render_results.py letting a quoted write "
          "verdict against another system stand without where the inputs were", CHECK_FAILED,
          "FAIL: test_a_hand_written_quote_of_a_marked_verdict_is_refused_without_its_marks "
          "(__main__.ConditionsTest"),
    Fault("method-input-note-unchecked", "py-scripts", "render_results.py letting a comparison "
          "method section say nothing of where the write inputs were", CHECK_FAILED,
          "FAIL: test_the_methods_sections_say_where_the_write_inputs_were "
          "(__main__.ConditionsTest"),
    Fault("benchmarks-wt4-old-word", "render-check", "BENCHMARKS.md's WT-4 row with the ArcticDB "
          "part under missed and the rule's outcome in a later clause, as the review found it",
          EXIT_RENDER_REFUSED, "quotes the verdict of WT-4 on Linux without a mark",
          quote_wt4_with_the_old_word),
    Fault("readme-comparison-input-unsaid", "render-check", "the README's comparison section "
          "with no word of where each system's write input was", EXIT_RENDER_REFUSED,
          "README.md's '## Comparison' does not say where the writes' inputs were",
          drop_the_readme_input_note),
    Fault("compare-disk-reads-unseen", "py-scripts", "render_results.py finding no system whose "
          "write read its input from disk inside the clock", CHECK_FAILED,
          "FAIL: test_a_system_whose_input_was_not_resident_is_one_whose_clock_holds_its_reads "
          "(__main__.ReplacedRunTest"),
    Fault("writeup-faster-write-unchecked", "py-scripts", "render_writeup.py rendering an essay "
          "that says tick-store writes faster than a system whose clock holds its disk reads", CHECK_FAILED,
          "FAIL: test_a_sentence_saying_a_write_is_faster_than_a_disk_read_clock "
          "(__main__.Refuses"),
    Fault("ties-disturbances-no-kind", "py-scripts", "check_ties.py with no kind for the "
          "disturbance file", CHECK_FAILED,
          "FAIL: test_a_disturbance_file_and_a_replaced_comparison_file_have_kinds "
          "(__main__.Repository"),
    Fault("headline-fuzz-row-names-its-id", "render-check", "the fuzz target added to the "
          "headline's table and its id named on the first screen, in the pitch, where the id is "
          "declared no result",
          EXIT_RENDER_REFUSED, "the README's first screen names 'G-3'", show_the_fuzz_row),
    Fault("readme-first-screen-host-by-system", "render-check", "the pitch calling the Linux "
          "host Linux again past its introduction", EXIT_RENDER_REFUSED,
          "the README's first screen calls a host 'Linux' past its introduction",
          call_a_host_by_its_system),
    Fault("headline-template-typed-figure", "render-check", "the disk row's measured template "
          "with Parquet's size typed in where the results file holds it", EXIT_RENDER_REFUSED,
          "types the figure '3.19' by hand", type_a_figure_into_a_template),
    Fault("headline-template-unnamed-value", "render-check", "the one-thread ingest row given "
          "a value its template never names", EXIT_RENDER_REFUSED,
          "takes the value 'flushed' and names it nowhere",
          give_a_template_a_value_it_does_not_name),
    Fault("headline-row-without-a-source", "render-check", "the one-thread ingest row without "
          "the source of its bar", EXIT_RENDER_REFUSED,
          "no source: each row says what it is, then its bar and where the bar comes from",
          drop_a_rows_source),
    Fault("headline-left-out-drops-a-miss", "render-check", "render_results.py leaves the "
          "missed parts of the targets on the screen out of the line under the table",
          EXIT_RENDER_REFUSED, "the line under the table does not name"),
    Fault("headline-left-out-miscounted", "render-check", "render_results.py counts the other "
          "targets missed in part as met in the line under the table", EXIT_RENDER_REFUSED,
          "the line under the table counts the other rows as"),
    # D-10: a per-locate figure says it leaves out the permutation to file order.
    Fault("render-permutation-caveat-dropped", "py-scripts", "render_results.py shows a "
          "per-locate column's bits against a compression target with no word that they leave "
          "out the locate sequence that rebuilds file order", CHECK_FAILED,
          "FAIL: test_a_per_locate_figure_says_it_leaves_out_the_permutation "
          "(__main__.RenderTargetsTest"),
    # Review RG-3: so does a ratio of per-locate figures, as CR-5's are.
    Fault("render-ratio-permutation-caveat-dropped", "py-scripts", "render_results.py adds the "
          "per-locate caveat only to a best-codec figure, so a ratio of per-locate figures, "
          "CR-5's, is shown met with no word that it leaves out the permutation", CHECK_FAILED,
          "FAIL: test_a_ratio_of_per_locate_figures_says_it_leaves_out_the_permutation "
          "(__main__.RenderTargetsTest"),
    # Review F's note for RESULTS: QL-4 compares headline variants, and says so where it is met.
    Fault("render-ql4-met-unqualified", "py-scripts", "render_results.py calls a QL-4 part met "
          "with no word that it compares headline variants, nor of another system's faster "
          "variant", CHECK_FAILED,
          "FAIL: test_a_met_part_says_it_is_among_the_headlines_and_names_a_faster_variant "
          "(__main__.RenderLowestTest"),
    # The Phase 2 st_blocks item: APFS allocates past a slowly grown file's end.
    Fault("render-macos-allocation-unqualified", "py-scripts", "render_results.py shows macOS "
          "stored bytes with no word that APFS can allocate past a file's end", CHECK_FAILED,
          "FAIL: test_macos_stored_bytes_say_they_can_exceed_the_files_own "
          "(__main__.RenderCompareTest"),
    Fault("results-md-hand-edit", "render-check", "a hand edit inside RESULTS.md's benchmark "
          "block, a host's heading changed", CHECK_FAILED,
          "the table in RESULTS.md is stale or was edited by hand", hand_edit_results_md),
    Fault("results-md-targets-hand-edit", "render-check", "a hand edit of a value in RESULTS.md's "
          "Targets block: WT-1 on Linux shown as 9.000 M records/s and met", CHECK_FAILED,
          "the table in RESULTS.md is stale or was edited by hand", hand_edit_targets_value),
    Fault("results-md-compare-hand-edit", "render-check", "a hand edit of a value in "
          "RESULTS.md's comparison block: tick-store's full-day Q2 warm median shown as 1.00 ms",
          CHECK_FAILED, "the table in RESULTS.md is stale or was edited by hand",
          hand_edit_compare_value),
    Fault("readme-headline-hand-edit", "render-check", "a hand edit of a figure in the "
          "README's headline: one-thread ingest typed as 9.000 M messages/s and met", CHECK_FAILED,
          "the table in README.md is stale or was edited by hand", hand_edit_headline_value),
    # The README's proofs paragraph is counted from results/check-proofs.json.
    Fault("readme-proofs-hand-edit", "render-check", "a hand edit inside the README's proofs "
          "paragraph", CHECK_FAILED, "the table in README.md is stale or was edited by hand",
          hand_edit_proofs_paragraph),
    Fault("render-proofs-verdicts-unread", "py-scripts", "render_results.py counts every fault "
          "of the proofs file as PROVEN and every control as UNDETECTED, whatever their "
          "verdicts", CHECK_FAILED,
          "FAIL: test_the_paragraph_counts_what_the_proof_file_holds (__main__.ProofsBlockTest"),
    Fault("render-proofs-markers-optional", "py-scripts", "render_results.py lets the README "
          "leave out the proofs paragraph's markers while the proofs file exists, so a typed "
          "count could stand in its place", CHECK_FAILED,
          "FAIL: test_the_markers_and_the_file_go_together (__main__.ProofsBlockTest"),
    # Review P1-4: the faults the prover lists are counted beside its record.
    Fault("render-proofs-list-unread", "py-scripts", "render_results.py gives the proofs "
          "paragraph without the faults the prover lists now", CHECK_FAILED,
          "FAIL: test_the_listed_faults_are_counted_beside_the_record (__main__.ProofsBlockTest"),
    Fault("render-proofs-unproven-counted-proven", "py-scripts", "render_results.py counts a "
          "listed fault whose run did not prove it among the proven ones still listed",
          CHECK_FAILED,
          "FAIL: test_the_listed_faults_are_counted_beside_the_record (__main__.ProofsBlockTest"),
    Fault("render-proofs-prover-optional", "py-scripts", "render_results.py renders the proofs "
          "paragraph with no prover to count the listed faults from", CHECK_FAILED,
          "FAIL: test_the_markers_need_the_prover_and_its_list (__main__.ProofsBlockTest"),
    # The ruling on review P1-1: which side of a verdict a swap or a disturbance fell on.
    Fault("sides-unconfirmed-counted", "py-scripts", "render_results.py counts a met part whose mark on a rival's side both runs carry", CHECK_FAILED,
          "FAIL: test_a_rival_mark_both_runs_carry_is_not_counted (__main__.SidesTest"),
    Fault("sides-pitch-claim-unconfirmed", "py-scripts", "render_results.py lets a pitch claim rest on a met part no run confirms", CHECK_FAILED,
          "FAIL: test_no_ranking_sentence_rests_on_a_part_not_counted (__main__.SidesTest"),
    Fault("sides-headline-row-unconfirmed", "py-scripts", "render_results.py shows a met part no run confirms as a headline row's result", CHECK_FAILED,
          "FAIL: test_no_first_screen_shows_a_part_not_counted (__main__.SidesTest"),
    Fault("sides-confirmed-without-replaced-figures", "py-scripts", "render_results.py marks a confirmed part without the replaced run's figures", CHECK_FAILED,
          "FAIL: test_a_rival_mark_the_replaced_run_does_not_carry_is_confirmed (__main__.SidesTest"),
    Fault("sides-quote-not-counted-dropped", "py-scripts", "render_results.py takes a quote of a verdict without its not-counted words", CHECK_FAILED,
          "FAIL: test_a_quote_without_the_not_counted_words_is_refused (__main__.SidesTest"),
    Fault("sides-not-alike-counted", "py-scripts", "render_results.py counts a met part its row labels not like-for-like", CHECK_FAILED,
          "FAIL: test_a_part_not_like_for_like_is_never_counted (__main__.SidesTest"),
    Fault("sides-own-side-uncounted", "py-scripts", "render_results.py takes a mark on tick-store's own side as a rival's and leaves the part uncounted", CHECK_FAILED,
          "FAIL: test_a_mark_on_tick_stores_side_alone_stays_counted (__main__.SidesTest"),
    Fault("sides-unrecorded-run-confirms", "py-scripts", "render_results.py lets a replaced run that no disturbance record covers confirm a rival's disturbed side", CHECK_FAILED,
          "FAIL: test_a_rival_disturbance_no_record_covers_on_the_replaced_run_is_not_counted (__main__.SidesTest"),
    # Review R1-1: a row's class is its counted parts', so a part met and not counted makes no
    # row met or missed in part; the writer and the check of the line under the table each.
    Fault("class-taken-from-an-uncounted-part", "render-check", "render_results.py's "
          "verdict_class makes a row of missed parts and one met and not counted missed in part, "
          "as WT-4 on Linux was", EXIT_RENDER_REFUSED,
          # The writer's count, one row more missed in part than the targets give (review R1-23).
          "the line under the table counts the other rows as {'met': 5, 'missed': 15, "
          "'missed in part': 7, 'not judged': 5}"),
    Fault("class-check-takes-an-uncounted-part", "render-check", "the check of the line under "
          "the table classes a row of missed parts and one met and not counted missed in part, "
          "apart from the writer, which does not", EXIT_RENDER_REFUSED,
          # The check's own count, one row more missed in part than the writer's (review R1-23).
          "and the targets give {'met': 5, 'missed in part': 7, 'missed': 15, 'not judged': 5}"),
    # Review R1-2: the not-counted mark names every side either run flags, with its pages in
    # each run, read from both records.
    Fault("sides-also-flagged-this-run-only", "py-scripts", "render_results.py's not-counted "
          "mark names only the sides this run flags, and leaves out a side the replaced run "
          "alone flags", CHECK_FAILED,
          "FAIL: test_the_not_counted_mark_names_every_side_either_run_flags (__main__.SidesTest"),
    # Review R1-3: a write side's swapping in both runs' write phases, read from each record.
    Fault("sides-write-swap-misread", "py-scripts", "render_results.py gives a write side's "
          "pages swapped out over its write phase as the pages swapped in", CHECK_FAILED,
          "FAIL: test_a_write_side_gives_both_runs_write_phase_swap_from_the_records "
          "(__main__.SidesTest"),
    # Review R1-4: a pitch figure from a marked timed phase stands with its mark, and a claim
    # held on both runs is judged on the replaced run's file too.
    Fault("pitch-figure-without-its-mark", "render-check", "the pitch quoting ArcticDB's warm "
          "hour with a table per stock bare, a figure from a phase in which the records list "
          "two remote logins", EXIT_RENDER_REFUSED,
          "a figure from a timed phase the records mark, without its mark",
          give_a_pitch_figure_without_its_mark),
    Fault("pitch-claim-judged-on-this-run-only", "py-scripts", "render_results.py judges a "
          "pitch claim held on both runs on this run's file alone", CHECK_FAILED,
          "FAIL: test_a_pitch_claim_held_on_both_runs_is_judged_on_the_replaced_file_too "
          "(__main__.ConditionsTest"),
    # Review R1-5: the line under the table groups parts of the same marks, its not-counted
    # class glossed for its parts' reasons, and the not-counted mark stands in for the swap
    # mark of the same systems on the first screen.
    Fault("left-out-groups-merged", "render-check", "render_results.py's line under the table "
          "puts each setup's parts in one group under the first group's marks",
          EXIT_RENDER_REFUSED, "the line under the table does not name"),
    Fault("left-out-gloss-not-its-parts", "py-scripts", "render_results.py glosses the "
          "not-counted class as swapping alone when some of its parts are not counted for "
          "another reason", CHECK_FAILED,
          "FAIL: test_the_line_groups_parts_of_the_same_marks_and_glosses_the_class "
          "(__main__.SidesTest"),
    Fault("screen-swap-mark-beside-not-counted", "py-scripts", "render_results.py gives the "
          "first screen's swap mark of the systems its not-counted mark already names",
          CHECK_FAILED,
          "FAIL: test_the_not_counted_mark_stands_in_for_the_swap_mark_of_the_same_systems "
          "(__main__.SidesTest"),
    # Review R1-6: RESULTS.md's generated prose in a reader's words, each gloss said once.
    Fault("results-label-raw-enum", "py-scripts", "render_results.py gives a comparison file's "
          "label as the harness words it, \"the targets whose host is Both\"", CHECK_FAILED,
          "FAIL: test_a_label_naming_the_host_column_says_both_machines (__main__.PlainLabelTest"),
    Fault("results-watcher-gloss-in-each-cell", "py-scripts", "render_results.py gives what a "
          "span computed from a watcher's period means in each cell that names such a login",
          CHECK_FAILED,
          "FAIL: test_a_cell_names_a_watchers_login_without_what_its_span_means "
          "(__main__.FuzzDisturbanceTest"),
    Fault("results-ingest-columns-glossed-per-class", "py-scripts", "render_results.py glosses "
          "the ingest tables' columns again under class B", CHECK_FAILED,
          "FAIL: test_macos_stored_bytes_say_they_can_exceed_the_files_own "
          "(__main__.RenderCompareTest"),
    # Review R1-9: what guards a timed figure, said by step as the records give it.
    Fault("results-guards-claimed-for-every-figure", "py-scripts", "RESULTS.md's sentence under "
          "the targets table says the per-CPU record and the load rule guard every timed "
          "figure, the gate's, the fuzzing's and the capacity check's included", CHECK_FAILED,
          "FAIL: test_the_guards_are_said_by_step_as_the_records_give_them (__main__.GuardsTest"),
    # Review R1-14: a case's mark in words a reader can parse.
    Fault("results-case-mark-unparsed", "py-scripts", "render_results.py gives a case's mark as "
          "\"one of the 65 cases its median takes\" again", CHECK_FAILED,
          "FAIL: test_a_case_of_a_median_says_how_many_cases_the_median_takes "
          "(__main__.FuzzDisturbanceTest"),
    # Review R1-15: the sentence under the table claims a mark only where the records tie one.
    Fault("under-sentence-claims-every-event-marked", "py-scripts", "the sentence under the "
          "README's table says every job or login in a timed step is marked, though the records "
          "list routine runs with no mark", CHECK_FAILED,
          "FAIL: test_the_sentence_under_the_table_says_what_the_records_leave_unmarked "
          "(__main__.UnderSentenceTest", claim_every_event_is_marked),
    # Review R1-26: nor that the unmarked runs could not be tied to a figure, as R1-15 had it.
    Fault("under-sentence-says-runs-cannot-be-tied", "py-scripts", "the sentence under the "
          "README's table says the routine runs cannot be tied to one figure, though the gate's "
          "record ties each to a step and a target", CHECK_FAILED,
          "FAIL: test_the_sentence_under_the_table_says_what_the_records_leave_unmarked "
          "(__main__.UnderSentenceTest"),
    # Review R1-27: no rule holds the load's value, only its lines.
    Fault("results-guards-load-value-claimed", "py-scripts", "render_results.py's guard "
          "sentence says the validators hold the load to a rule, where they require its lines "
          "alone", CHECK_FAILED,
          "FAIL: test_the_guards_are_said_by_step_as_the_records_give_them (__main__.GuardsTest"),
    # Review R1-29: beside a side the earlier run alone flags, one this run alone flags says so.
    Fault("screen-this-run-side-unsaid", "py-scripts", "render_results.py's first screen gives "
          "a side this run alone flags with no word of it, beside a side the earlier run alone "
          "flags", CHECK_FAILED,
          "FAIL: test_a_side_only_the_earlier_run_flags_is_named_on_the_first_screen "
          "(__main__.SidesTest"),
    # Review R1-16: the first screen names every side either run flags, as RESULTS.md does.
    Fault("screen-earlier-run-side-dropped", "py-scripts", "render_results.py leaves off the "
          "first screen a side only the earlier run flags, which RESULTS.md names",
          CHECK_FAILED,
          "FAIL: test_a_side_only_the_earlier_run_flags_is_named_on_the_first_screen "
          "(__main__.SidesTest"),
    # Review R1-17: the met parts the screen leaves out are named, not counted.
    Fault("left-out-met-part-unnamed", "render-check", "render_results.py's line under the table "
          "leaves out the met parts, as \"Met: 1 more of their parts\" did", EXIT_RENDER_REFUSED,
          "the line under the table does not name 'flushed one-thread ingest', which is met"),
    # Review R1-19: no pitch claim of figures compares what a part met and not counted compares.
    Fault("pitch-claim-of-figures-on-uncounted-part", "py-scripts", "render_results.py refuses "
          "only a pitch claim on a target for resting on a part met and not counted, so a pair "
          "or a fastest claim on the same figures raises the ranking sentence the part may not",
          CHECK_FAILED, "FAIL: test_no_claim_of_figures_compares_what_a_part_not_counted_compares "
          "(__main__.SidesTest"),
    # Review R1-20: a claim of figures is judged on the replaced run's file wherever one is kept.
    Fault("pitch-claim-judged-on-this-run-alone", "py-scripts", "render_results.py judges a "
          "pitch claim of figures on the replaced run's file only where the claim asks for it, "
          "so a pair or a fastest claim the replaced run reverses passes", CHECK_FAILED,
          "FAIL: test_every_claim_of_figures_is_judged_on_the_replaced_file_where_one_is_kept "
          "(__main__.ConditionsTest"),
    Fault("pitch-swap-below-on-this-run-alone", "py-scripts", "render_results.py reads the "
          "swap flags a pair of figures rests on from the replaced run's file only where the "
          "claim asks for it, though the pair is judged there", CHECK_FAILED,
          "FAIL: test_every_claim_of_figures_is_judged_on_the_replaced_file_where_one_is_kept "
          "(__main__.ConditionsTest"),
    Fault("pitch-swap-fastest-on-this-run-alone", "py-scripts", "render_results.py reads the "
          "swap flags a fastest claim rests on from this run's file alone, though the claim is "
          "judged on the replaced run's too", CHECK_FAILED,
          "FAIL: test_every_claim_of_figures_is_judged_on_the_replaced_file_where_one_is_kept "
          "(__main__.ConditionsTest"),
    # Review R1-21: what a watcher's computed span means is said of that kind of login alone.
    Fault("legend-period-said-of-another-kind", "py-scripts", "render_results.py's kinds under "
          "RESULTS.md's targets table say of a backup run what a span computed from a watcher's "
          "period means, and the login that span belongs to loses it", CHECK_FAILED,
          "FAIL: test_the_first_screen_takes_the_short_forms_and_results_md_explains_them "
          "(__main__.ConditionsTest"),
    # Review R1-22: the write-swap reason is given only for a replaced run no record covers.
    Fault("write-swap-reason-on-a-covered-run", "py-scripts", "render_results.py gives a rival's "
          "write side the write-swap reason, which says no disturbance record covers the "
          "replaced run, for a replaced run a record covers", CHECK_FAILED,
          "FAIL: test_a_write_side_gives_both_runs_write_phase_swap_from_the_records "
          "(__main__.SidesTest"),
    # Review R1-23: a row of no parts is not judged in the writer's class, as in the check's.
    Fault("verdict-class-empty-row-not-counted", "py-scripts", "render_results.py's "
          "verdict_class calls a row of no parts met and not counted, where class_of calls it "
          "not judged", CHECK_FAILED,
          "FAIL: test_a_row_whose_only_met_part_is_not_counted_takes_the_class_of_the_rest "
          "(__main__.SidesTest"),
    # Review R1-10: the check names each page by its path.
    Fault("render-check-pages-by-name", "py-scripts", "render_results.py names the pages it "
          "checked by their file names alone, so README.md and fuzz/README.md read the same",
          CHECK_FAILED, "FAIL: test_each_page_is_named_by_its_path (__main__.PageNameTest"),
    Fault("sides-left-out-counts-uncounted", "py-scripts", "render_results.py counts a part no run confirms among the first screen's met parts", CHECK_FAILED,
          "FAIL: test_no_count_takes_a_part_not_counted (__main__.SidesTest"),
    # The ruling on review P1-5: G-3's runs beside its verdict, from its file.
    Fault("fuzz-runs-table-hand-edit", "render-check", "a run's executions typed by hand in "
          "RESULTS.md's table of G-3's runs", CHECK_FAILED,
          "the table in RESULTS.md is stale or was edited by hand", hand_edit_fuzz_runs_table),
    Fault("fuzz-runs-table-rate-as-executions", "py-scripts", "render_results.py gives a run's "
          "executions a second as its executions in G-3's table", CHECK_FAILED,
          "FAIL: test_the_table_gives_each_run_of_the_file (__main__.FuzzRunsTableTest"),
    Fault("fuzz-runs-table-unmarked", "py-scripts", "render_results.py leaves out the "
          "disturbances recorded in G-3's runs", CHECK_FAILED,
          "FAIL: test_the_table_gives_each_run_of_the_file (__main__.FuzzRunsTableTest"),
    Fault("fuzz-cell-executions-dropped", "py-scripts", "render_results.py gives G-3's cell "
          "without its executions", CHECK_FAILED,
          "FAIL: test_the_cell_gives_each_hosts_smallest_and_total_executions (__main__.FuzzRunsTableTest"),
    # Review P1-17: where each write's input was, beside its durability.
    Fault("ingest-input-column-inverted", "py-scripts", "render_results.py says a write's input "
          "read from disk was in memory, and the reverse", CHECK_FAILED,
          "FAIL: test_each_write_says_where_its_input_was (__main__.InputColumnTest"),
    # docs/checks.md, moved out of the README, is read as a results page.
    Fault("render-checks-page-unread", "py-scripts", "render_results.py leaves docs/checks.md "
          "out of the pages whose whole prose the faster-write and quote rules read",
          CHECK_FAILED,
          "FAIL: test_every_results_page_is_read_whole_and_named_with_its_line "
          "(__main__.ReplacedRunTest"),
    # Review P1-2: a quote of a verdict whose part is not like for like says so.
    Fault("render-quote-not-alike-unasked", "py-scripts", "render_results.py takes a quote of a "
          "verdict whose part its row labels not like-for-like without those words",
          CHECK_FAILED,
          "FAIL: test_a_quote_without_the_words_is_refused (__main__.NotAlikeQuoteTest"),
    # Review P1-3: the round trip's times and store size are read from its results file.
    Fault("readme-roundtrip-hand-edit", "render-check", "a hand edit inside the README's "
          "sentence on the round trip's times", CHECK_FAILED,
          "the table in README.md is stale or was edited by hand",
          hand_edit_roundtrip_sentence),
    Fault("render-roundtrip-unrendered", "py-scripts", "render_results.py leaves the README's "
          "round-trip sentence as it finds it, so a typed figure could drift from the file",
          CHECK_FAILED,
          "FAIL: test_the_sentence_gives_the_file_figures (__main__.RoundtripBlockTest"),
    Fault("readme-roundtrip-markers-removed", "py-scripts", "the README without the round-trip "
          "sentence's markers, so its figures would be typed and go unchecked", CHECK_FAILED,
          "ERROR: test_the_readme_gives_the_committed_file (__main__.RoundtripBlockTest"),
    Fault("readme-headline-markers-removed", "render-check", "the README without its headline "
          "markers, so no headline would be rendered or checked", EXIT_RENDER_REFUSED,
          "README.md has the headline markers and targets.json no headline, or the reverse",
          drop_headline_markers),
    Fault("readme-keeps-a-results-block", "render-check", "the README holding a copy of the "
          "Targets block, which --check would otherwise never compare", EXIT_RENDER_REFUSED,
          "README.md holds the targets block's markers, and that block belongs in RESULTS.md "
          "alone", readme_keeps_the_targets_block),
    Fault("headline-without-a-miss", "render-check", "the headline cut to targets that were "
          "met, so the README's top would show no missed target", EXIT_RENDER_REFUSED,
          "no row is a missed target", headline_without_a_miss),
    Fault("headline-verdict-all-parts", "py-scripts", "render_results.py judges a headline row "
          "that shows some of its parts on all of them, so its verdict is not the shown parts'",
          CHECK_FAILED, "FAIL: test_a_row_of_some_parts_is_judged_on_them_and_names_the_rest_"
          "below (__main__.RenderHeadlineTest"),
    Fault("actionlint-broken-workflow", "actionlint", "the workflow names a matrix key that does "
          "not exist", CHECK_FAILED, "no_such_key", break_workflow),
    Fault("hygiene-home-path", "hygiene", "a tracked file holds a home path", CHECK_FAILED,
          "notes-injected.txt:1: absolute home path", inject_home_path),
    Fault("hygiene-data-file", "hygiene", "a tracked market data file", CHECK_FAILED,
          "12302019.NASDAQ_ITCH50: data file is tracked", inject_data_file),
    Fault("hygiene-allowed-kind-no-limit", "py-scripts", "check_hygiene.py letting a file of a "
          "kind allowed to be large through at any size, past its kind's 2,000,000-byte limit",
          CHECK_FAILED, "FAIL: test_an_allowed_kind_over_its_limit_is_refused "
          "(__main__.Hygiene"),
    Fault("hygiene-large-result-not-json", "py-scripts", "check_hygiene.py letting a file of a "
          "kind allowed to be large through when it is not a JSON object, as a data file under "
          "a result's name", CHECK_FAILED,
          "FAIL: test_an_allowed_name_that_is_not_a_json_object_is_refused (__main__.Hygiene"),
    Fault("check-sh-failing-step", "hygiene", "check.sh with a failing step, then a passing "
          "step: non-zero exit, the step named, and the next step still run", CHECK_FAILED,
          "notes-injected.txt:1: absolute home path", inject_home_path,
          later_step="render-check"),
    Fault("check-sh-no-tests", "dev-test", "a test filter that matches nothing must fail, not "
          "pass with no tests run", EXIT_CTEST, "No tests were found",
          arguments=("--test-regex", "OracleExhaustiveFlip"),
          fault_arguments=("--test-regex", "NoSuchTestAnywhere")),
    # Review E-6: CI runs every step it names, py-compare included.
    Fault("check-sh-no-skips-ignored", "py-scripts", "check.sh takes --no-skips and still "
          "passes a step that skips", CHECK_FAILED,
          "FAIL: test_no_skips_turns_a_skip_into_a_failure (__main__.CheckScript"),
    Fault("ci-compare-venv-not-made", "py-scripts", "the workflow no longer makes the .venv, so "
          "py-compare cannot run on CI", CHECK_FAILED,
          "FAIL: test_the_workflow_makes_the_environment_py_compare_needs (__main__.CheckScript"),
    Fault("ci-skips-allowed", "py-scripts", "the workflow runs check.sh without --no-skips, so a "
          "step it names may skip and the job still pass", CHECK_FAILED,
          "FAIL: test_the_workflow_runs_every_step_it_names (__main__.CheckScript"),
    # Review RP-15: check.sh is found however a step calls it.
    Fault("ci-check-sh-run-block-skips-allowed", "py-scripts", "the workflow calls check.sh a "
          "second time, as `bash scripts/check.sh` in a run block, without --no-skips",
          CHECK_FAILED, "FAIL: test_the_workflow_runs_every_step_it_names (__main__.CheckScript"),
    # Review RP-16: the comparison's environment is made of hashed wheels only.
    Fault("ci-compare-sources-allowed", "py-scripts", "the workflow lets pip build a locked "
          "package from source, its build requirements fetched unhashed", CHECK_FAILED,
          "FAIL: test_the_workflow_makes_the_environment_py_compare_needs (__main__.CheckScript"),
    # Review E-5: a proof's key holds the code and prose that made its verdict.
    Fault("prover-key-names-only", "py-scripts", "the prover's key for a proof holds a "
          "mutation's name and no code or prose, so an edited mutation keeps its old proof",
          CHECK_FAILED, "FAIL: test_a_mutation_with_another_body_has_another_fingerprint "
          "(__main__.Fingerprint"),
    # Reviews RP-11 and RP-12: the key holds how a fault is judged and the gate on stored ones.
    Fault("prover-key-judging-left-out", "py-scripts", "the prover's key leaves out whether a "
          "fault is tolerated or a control, so a fault judged otherwise keeps its old verdict",
          CHECK_FAILED, "FAIL: test_a_fault_judged_otherwise_has_a_changed_fingerprint "
          "(__main__.FaultList"),
    Fault("prover-key-gate-left-out", "py-scripts", "the prover's key leaves out the gate that "
          "keeps a stored verdict, so widening it passes every stored failure unproven",
          CHECK_FAILED, "FAIL: test_an_edited_gate_on_stored_verdicts_changes_every_fingerprint "
          "(__main__.Fingerprint"),
    # Every listed fault breaks what it says on the committed tree.
    Fault("prover-patch-stale", "py-scripts", "a listed patch as it was before the code it "
          "edits moved, so it no longer applies", CHECK_FAILED,
          "FAIL: test_every_listed_patch_applies_to_the_committed_tree "
          "(__main__.EveryFaultApplies"),
    Fault("prover-mutation-changes-nothing", "py-scripts", "a listed mutation that writes back "
          "what it read, so its fault changes nothing", CHECK_FAILED,
          "FAIL: test_every_listed_mutation_changes_the_committed_tree "
          "(__main__.EveryFaultApplies"),
    # Review RP-13: a mutation that changes nothing writes nothing, so the test sees it.
    Fault("prover-mutation-matches-nothing", "py-scripts", "a listed mutation that looks for a "
          "setting no file has, so it changes nothing but the files it writes back",
          CHECK_FAILED, "FAIL: test_every_listed_mutation_changes_the_committed_tree "
          "(__main__.EveryFaultApplies"),
    # The coverage summary says which kernel sets its gate requires.
    Fault("coverage-kernel-statement-stale", "py-scripts", "coverage_report.py writes that no "
          "SIMD kernel exists, though its gate requires the scalar set and this processor's",
          CHECK_FAILED, "FAIL: test_the_kernel_statement_is_the_kernel_sets_required "
          "(__main__.RequiredList"),
    Fault("coverage-required-removed", "coverage", "bench_support_test.cpp dropped from the "
          "tests, so the guard and the round-trip check lose their only callers", CHECK_FAILED,
          "REQUIRED FUNCTION NOT REACHED: verify_round_trip"),
    # check_ties.py: every results file against the code it was measured at.
    Fault("ties-kind-dropped", "py-scripts", "check_ties.py's census kind is renamed so that "
          "its pattern matches no file, and every census file is refused as of no kind (review "
          "TD-9)", CHECK_FAILED,
          "FAIL: test_unchanged_code_passes_for_every_kind (__main__.SameContent"),
    Fault("ties-path-dropped", "py-scripts", "check_ties.py's codec-eval kind loses tools/, "
          "where tick_codec_eval is built", CHECK_FAILED,
          "FAIL: test_a_change_in_each_measured_path_fails_every_kind_that_measures_it "
          "(__main__.MeasuredChange"),
    Fault("ties-difference-ignored", "py-scripts", "check_ties.py takes a committed difference "
          "in a measured path as equal, seeing new files alone", CHECK_FAILED,
          "FAIL: test_a_change_in_each_measured_path_fails_every_kind_that_measures_it "
          "(__main__.MeasuredChange"),
    Fault("ties-missing-commit-passes", "py-scripts", "check_ties.py passes a file whose commit "
          "a full clone does not hold", CHECK_FAILED,
          "FAIL: test_a_commit_a_full_clone_lacks_fails (__main__.MissingCommit"),
    Fault("ties-no-history-passes", "py-scripts", "check_ties.py passes every file of a tree "
          "with no history to compare with", CHECK_FAILED,
          "FAIL: test_an_exported_tree_is_not_judged_and_says_so (__main__.NoHistory"),
    Fault("ties-assembled-ignored", "py-scripts", "check_ties.py never compares what assembled "
          "a file since the commit that wrote it", CHECK_FAILED,
          "FAIL: test_a_change_to_what_assembled_a_file_after_it_was_written_fails_it "
          "(__main__.AssembledChange"),
    # The capacity file ties the files that copy from it through the fields read of it.
    Fault("ties-read-field-moved-ignored", "py-scripts", "check_ties.py compares only the "
          "capacity file's day, so a moved stored-bytes figure leaves the store-size files and "
          "the estimate that copy it tied", CHECK_FAILED,
          "FAIL: test_each_read_field_that_moves_unties_the_files_that_read_it "
          "(__main__.ReadFields"),
    Fault("ties-reads-list-misses-a-field", "py-scripts", "check_ties.py's list of what "
          "store_results.py reads of the capacity file leaves out QuestDB's outcome, which it "
          "copies, so a moved outcome would leave the store-size file tied", CHECK_FAILED,
          "FAIL: test_store_results_reads_the_fields_the_store_size_kind_lists "
          "(__main__.ReadsAreWhatTheAssemblersRead"),
    Fault("ties-pin-by-name", "py-scripts", "check_ties.py takes a file as pinned by its name, "
          "whatever its bytes", CHECK_FAILED,
          "FAIL: test_a_pin_of_other_bytes_does_not_hold (__main__.Pinned"),
    # Review TD-1 and TD-7: a git command that fails is a refusal naming it.
    Fault("ties-git-error-passes", "py-scripts", "check_ties.py takes what a failed git command "
          "printed as its answer, so an object missing from the clone reads as nothing changed",
          CHECK_FAILED, "FAIL: test_a_missing_object_is_a_refusal_naming_the_command "
          "(__main__.GitErrors"),
    Fault("ties-results-outside-passes", "py-scripts", "check_ties.py no longer refuses a "
          "--results folder outside the checkout, and stops on a traceback where it should "
          "refuse", CHECK_FAILED, "ERROR: test_a_results_folder_outside_the_checkout_is_refused "
          "(__main__.GitErrors"),
    Fault("ties-unreadable-git-dir-unjudged", "py-scripts", "check_ties.py takes a .git that "
          "git cannot read as a tree with no history, a skip", CHECK_FAILED,
          "FAIL: test_a_git_dir_git_cannot_read_is_refused (__main__.GitErrors"),
    Fault("ties-subfolder-unjudged", "py-scripts", "check_ties.py takes a --repo that is a "
          "folder inside a checkout as a tree with no history, a skip", CHECK_FAILED,
          "FAIL: test_a_folder_inside_a_checkout_is_refused (__main__.GitErrors"),
    # Review TD-2: a file not committed as it is is not judged against what assembled it.
    Fault("ties-uncommitted-assembled-judged", "py-scripts", "check_ties.py judges a results "
          "file that is not committed, or differs from its blob, against the last commit that "
          "wrote it", CHECK_FAILED,
          "FAIL: test_a_file_not_committed_as_it_is_is_not_judged_against_what_assembled_it "
          "(__main__.AssembledChange"),
    # Review TD-3: every file under results/ is examined.
    Fault("ties-non-json-unseen", "py-scripts", "check_ties.py examines only the .json files "
          "under results/", CHECK_FAILED,
          "FAIL: test_every_file_under_results_is_examined (__main__.NoCommit"),
    # Review TD-4: a pin names its file exactly, from a set fixed in code.
    Fault("ties-pin-any-listed-name", "py-scripts", "check_ties.py takes as pinned any file "
          "whose name has a line under its validator's key, at any path", CHECK_FAILED,
          "FAIL: test_another_file_is_no_pin_and_its_line_is_refused "
          "(__main__.Pinned"),
    Fault("ties-protocol-1-pin-allowed", "py-scripts", "check_ties.py lets the comparison "
          "validator's key pin the kept Linux day's name again, though the freeze replaced every "
          "protocol 1 file", CHECK_FAILED,
          "FAIL: test_no_file_may_be_pinned_as_protocol_1 (__main__.Pinned"),
    Fault("ties-stray-pin-accepted", "py-scripts", "check_ties.py lets a line in the pins file "
          "for a name PIN_NAMES does not hold pass unremarked", CHECK_FAILED,
          "FAIL: test_another_file_is_no_pin_and_its_line_is_refused "
          "(__main__.Pinned"),
    Fault("ties-replaced-pin-outside-folder", "py-scripts", "check_ties.py takes a file kept "
          "for the record as pinned outside a replaced/ folder", CHECK_FAILED,
          "FAIL: test_a_replaced_file_outside_a_replaced_folder_is_refused (__main__.Pinned"),
    # Review TD-5 and TD-6: a commit must be in HEAD's history; a tracked link is refused.
    Fault("ties-non-ancestor-passes", "py-scripts", "check_ties.py judges a file whose commit "
          "this clone holds on another branch, which a fresh clone would not", CHECK_FAILED,
          "FAIL: test_a_commit_not_in_heads_history_fails (__main__.MissingCommit"),
    Fault("ties-symlink-passes", "py-scripts", "check_ties.py compares a symbolic link git "
          "tracks in a measured path as if it were the file it points at", CHECK_FAILED,
          "FAIL: test_a_symbolic_link_in_a_measured_path_is_refused (__main__.GitErrors"),
    # Review TD-8: a refused file fails the step.
    Fault("ties-refused-exits-0", "py-scripts", "check_ties.py counts a refused file as no "
          "error, so a run whose only verdicts are refusals exits 0", CHECK_FAILED,
          "FAIL: test_a_commit_git_cannot_read_fails (__main__.NoCommit"),
    # Review TD-10: fuzz_results.py reads the preset through build_preset.py.
    Fault("ties-fuzz-build-preset-dropped", "py-scripts", "check_ties.py's fuzz kind loses "
          "scripts/build_preset.py from what assembles a fuzz file", CHECK_FAILED,
          "FAIL: test_a_change_to_what_assembled_a_file_after_it_was_written_fails_it "
          "(__main__.AssembledChange.test_a_change_to_what_assembled_a_file_after_it_was_written_"
          "fails_it) (path='scripts/build_preset.py')"),
    Fault("check-sh-untied-results-pass", "py-scripts", "check.sh passes check-ties where no "
          "history let it compare anything", CHECK_FAILED,
          "FAIL: test_results_files_with_no_history_to_tie_them_skip_and_fail_under_no_skips "
          "(__main__.CheckScript"),
    # The step itself on a measured file changed after every results file's commit. Its
    # baseline passes only once every results file no pin declares is measured at the code
    # HEAD holds (docs/NOTES.md, The results files tied to their code).
    Fault("ties-measured-code-changed", "check-ties", "a library source changed after the "
          "commit every results file names", CHECK_FAILED,
          "what it measures changed since: src/itch/frame.cpp"),
    # The essay's render, scripts/render_writeup.py, and its step.
    Fault("writeup-typed-number-unchecked", "py-scripts", "render_writeup.py lets a number typed "
          "in the essay's template through", CHECK_FAILED,
          "FAIL: test_a_typed_number_in_the_template (__main__.Refuses"),
    Fault("writeup-two-files-accepted", "py-scripts", "render_writeup.py reads the first of two "
          "results files a token's kind matches", CHECK_FAILED,
          "FAIL: test_a_kind_that_matches_two_files (__main__.Refuses"),
    Fault("writeup-page-not-compared", "py-scripts", "render_writeup.py --check passes a "
          "WRITEUP.md it never compares with a fresh render", CHECK_FAILED,
          "FAIL: test_a_hand_edited_number_in_the_page_fails_check (__main__.Renders"),
    Fault("writeup-not-judged-passes", "py-scripts", "render_writeup.py --check exits 0 while "
          "tokens still wait for their results files", CHECK_FAILED,
          "FAIL: test_a_token_whose_kind_is_still_to_come_is_not_judged (__main__.NotJudged"),
    Fault("check-sh-writeup-not-judged-passes", "py-scripts", "check.sh passes writeup-check "
          "when the essay could not be judged", CHECK_FAILED,
          "FAIL: test_check_sh_reports_not_judged_as_a_skip_and_a_failure_under_no_skips "
          "(__main__.NotJudged"),
    Fault("check-sh-asan-leaves-out-the-oracle-reruns", "py-scripts", "check.sh's asan-ubsan "
          "step leaves out the fixed grid and the named seeds' tests, as the tsan step does",
          CHECK_FAILED, "FAIL: test_only_the_tsan_step_leaves_out_the_oracle_reruns "
          "(__main__.CheckScript"),
    Fault("writeup-verdict-own-code", "py-scripts", "render_writeup.py words a verdict by its "
          "own rule, apart from render_results.py's verdict_class", CHECK_FAILED,
          "FAIL: test_the_verdict_is_render_results_own_code (__main__.Renders"),
    Fault("writeup-constant-found-twice-accepted", "py-scripts", "render_writeup.py reads a code "
          "constant from the first of two lines its pattern matches", CHECK_FAILED,
          "FAIL: test_a_constant_found_twice (__main__.Refuses"),
    # Review WD-1.
    Fault("writeup-commits-mixed-accepted", "py-scripts", "render_writeup.py reads results files "
          "that name two measured commits", CHECK_FAILED,
          "FAIL: test_a_file_at_another_commit_is_refused (__main__.Commits"),
    # A kind measured at another commit holds only where what the other figures measure is
    # unchanged between the two, by check_ties.py's kinds.
    Fault("writeup-measured-change-ignored", "py-scripts", "render_writeup.py takes a kind "
          "measured at another commit though the library code the other figures measure "
          "differs between the two", CHECK_FAILED,
          "FAIL: test_a_library_change_between_the_commits_refuses (__main__.Commits"),
    Fault("writeup-every-path-compared", "py-scripts", "render_writeup.py compares every path "
          "of the tree between the two commits, so a docs commit, which no figure is measured "
          "from, refuses the essay", CHECK_FAILED,
          "FAIL: test_a_change_only_to_prose_and_docs_passes (__main__.Commits"),
    Fault("writeup-exception-kind-measured", "py-scripts", "render_writeup.py also compares "
          "what the later kind itself measures, so the fuzz harness's own change, the reason "
          "it was measured again, refuses the essay", CHECK_FAILED,
          "FAIL: test_a_change_only_the_later_kind_measures_passes (__main__.Commits"),
    # Review WD-2 and WD-3.
    Fault("writeup-judging-word-unclaimed", "py-scripts", "render_writeup.py lets a sentence "
          "judge the results in words no claim or token carries", CHECK_FAILED,
          "FAIL: test_a_judging_word_with_no_claim (__main__.Claims"),
    Fault("writeup-number-word-unseen", "py-scripts", "render_writeup.py lets a number written "
          "in words through", CHECK_FAILED, "FAIL: test_a_number_in_words (__main__.Refuses"),
    Fault("writeup-claim-not-held", "py-scripts", "render_writeup.py keeps a claim whose "
          "condition the results answer false", CHECK_FAILED,
          "FAIL: test_a_claim_the_results_do_not_bear_out (__main__.Claims"),
    # Review WD-6.
    Fault("writeup-census-missing-row", "py-scripts", "render_writeup.py lets the Missed targets "
          "section leave out a missed row", CHECK_FAILED,
          "FAIL: test_a_missed_target_left_out (__main__.Census"),
    Fault("writeup-census-met-row-kept", "py-scripts", "render_writeup.py lets the Missed "
          "targets section keep a met row", CHECK_FAILED,
          "FAIL: test_a_met_target_kept_in (__main__.Census"),
    Fault("writeup-verdict-marks-dropped", "py-scripts", "render_writeup.py prints a verdict "
          "without the marks RESULTS.md sets beside it", CHECK_FAILED,
          "FAIL: test_a_verdict_carries_the_marks_results_gives_it (__main__.Marks"),
    Fault("writeup-verdict-mark-unchecked", "py-scripts", "render_writeup.py lets a verdict "
          "that drops a mark RESULTS.md gives it through", CHECK_FAILED,
          "FAIL: test_a_verdict_that_drops_a_mark_is_refused (__main__.Marks"),
    Fault("writeup-phase-mark-unchecked", "py-scripts", "render_writeup.py lets a verdict "
          "that gives a disturbed or swapped-in phase's mark in neither its full nor its short "
          "form through", CHECK_FAILED,
          "FAIL: test_a_phase_mark_in_neither_form_is_refused (__main__.Marks"),
    Fault("writeup-marks-in-full", "py-scripts", "render_writeup.py gives a phase's mark beside "
          "an essay verdict in full where RESULTS.md defines a short form", CHECK_FAILED,
          "FAIL: test_a_phase_mark_is_given_in_its_short_form (__main__.Marks"),
    Fault("writeup-move-unchecked", "py-scripts", "render_writeup.py lets the essay state a move "
          "between the replaced run and this one without saying its cause was not measured and "
          "is not known", CHECK_FAILED,
          "FAIL: test_a_move_between_the_runs_with_no_unknown_cause (__main__.Marks"),
    Fault("writeup-flag-never-set", "py-scripts", "render_writeup.py never sets the dagger "
          "of a figure from a disturbed or swapped-in phase", CHECK_FAILED,
          "FAIL: test_a_swapped_in_phase_is_flagged (__main__.Marks"),
    Fault("writeup-met-parts-unnamed", "py-scripts", "render_writeup.py names only the parts "
          "another setup beats beside a setup mark, leaving the other parts met unnamed",
          CHECK_FAILED, "FAIL: test_every_part_met_is_named_beside_the_setup_mark "
          "(__main__.SetupMark"),
    Fault("writeup-flag-variant-ignored", "py-scripts", "render_writeup.py reads a dagger's "
          "phase in the headline setup when the read names another", CHECK_FAILED,
          "FAIL: test_a_read_that_names_its_variant_reads_that_variant (__main__.Marks"),
    Fault("writeup-not-counted-part-unnamed", "py-scripts", "render_writeup.py leaves a part "
          "met by the rule and not counted out of the parts met it names", CHECK_FAILED,
          "FAIL: test_a_part_met_and_not_counted_is_named_too (__main__.SetupMark"),
    Fault("writeup-keeps-unchecked", "py-scripts", "render_writeup.py lets the essay say what the "
          "replaced run's record keeps other than as its fields give it", CHECK_FAILED,
          "FAIL: test_what_the_replaced_record_keeps_said_otherwise (__main__.Marks"),
    Fault("writeup-claim-on-changed-outcome", "py-scripts", "render_writeup.py lets a claim "
          "rest on a verdict the replaced run judged otherwise", CHECK_FAILED,
          "FAIL: test_a_claim_that_rests_on_a_changed_outcome (__main__.Marks"),
    # Review WD-4.
    Fault("writeup-literal-value-accepted", "py-scripts", "render_writeup.py takes a number "
          "written into the figure list that is no declared factor", CHECK_FAILED,
          "FAIL: test_a_literal_value (__main__.Literals"),
    # Review WD-5.
    Fault("writeup-host-word-unchecked", "py-scripts", "render_writeup.py lets an entry read on "
          "one host be described as the other host's", CHECK_FAILED,
          "FAIL: test_a_figure_described_as_the_other_host_s (__main__.Hosts"),
    # Review WD-7.
    Fault("writeup-bar-rounded", "py-scripts", "render_writeup.py shows a bar with fewer digits "
          "than it has", CHECK_FAILED,
          "FAIL: test_a_bar_shown_with_fewer_digits_than_it_has (__main__.Gaps"),
    Fault("writeup-figure-across-its-bar", "py-scripts", "render_writeup.py prints a figure "
          "rounded to the other side of its bar from its verdict", CHECK_FAILED,
          "FAIL: test_a_figure_that_rounds_to_the_other_side_of_its_bar (__main__.Gaps"),
    Fault("writeup-gap-shown-as-none", "py-scripts", "render_writeup.py prints a gap as a ratio "
          "of 1 or a difference of 0", CHECK_FAILED,
          "FAIL: test_a_gap_printed_as_none (__main__.Gaps"),
    # Review WD-8.
    Fault("writeup-prover-counts-control", "py-scripts", "render_writeup.py counts the prover's "
          "control among its faults", CHECK_FAILED, "FAIL: test_the_counts (__main__.Prover"),
    # Review WD-9.
    Fault("writeup-pattern-glob-accepted", "py-scripts", "render_writeup.py takes a file pattern "
          "with a commit prefix beside its *", CHECK_FAILED,
          "FAIL: test_a_kind_named_by_a_commit_prefix_or_folder_or_leaving_results "
          "(__main__.Refuses"),
    # Review WD-10.
    Fault("writeup-list-names-the-page", "py-scripts", "render_writeup.py lets the figure list "
          "name its own page, template or targets", CHECK_FAILED,
          "FAIL: test_the_list_naming_its_page_template_or_targets (__main__.Refuses"),
    # Review WD-12.
    Fault("verdict-class-met-on-any-part", "py-scripts", "render_results.py's verdict_class "
          "calls a row met when any part is met", CHECK_FAILED,
          "FAIL: test_verdict_class (__main__.Verdicts"),
    # Review WD-11.
    Fault("writeup-sub-operands-swapped", "py-scripts", "render_writeup.py's sub form subtracts "
          "its operands the wrong way round", CHECK_FAILED,
          "FAIL: test_each_form (__main__.Forms"),
    # Review WD-13.
    Fault("writeup-constant-from-a-comment", "py-scripts", "render_writeup.py reads a code "
          "constant from a comment", CHECK_FAILED,
          "FAIL: test_a_constant_read_from_a_comment (__main__.Refuses"),
    # Review WD-14.
    Fault("writeup-non-result-anywhere", "py-scripts", "render_writeup.py lets a non-result's "
          "text stand outside the places it is declared for", CHECK_FAILED,
          "FAIL: test_a_non_result_used_outside_its_place (__main__.Refuses"),
    # Review WD-15.
    Fault("writeup-pin-unsaid", "py-scripts", "render_writeup.py lets a paragraph show a pinned "
          "kind's figures without saying so", CHECK_FAILED,
          "FAIL: test_a_declared_pin_said_in_each_paragraph (__main__.Pins"),
    Fault("writeup-pinned-file-undeclared", "py-scripts", "render_writeup.py reads a pinned file "
          "through a kind that does not declare the pin", CHECK_FAILED,
          "FAIL: test_a_pinned_file_read_by_a_kind_that_does_not_declare_it (__main__.Pins"),
    # Review WD-16.
    Fault("writeup-opening-any-length", "py-scripts", "render_writeup.py takes an opening of "
          "other than three sentences", CHECK_FAILED,
          "FAIL: test_an_opening_that_is_not_three_sentences_each_with_a_token (__main__.Refuses"),
    # Review WD-17.
    Fault("writeup-tokens-glued-accepted", "py-scripts", "render_writeup.py takes tokens side by "
          "side or against a digit", CHECK_FAILED,
          "FAIL: test_tokens_side_by_side_or_against_a_digit (__main__.Refuses"),
    Fault("writeup-numeral-forms-accepted", "py-scripts", "render_writeup.py takes a multiplier "
          "glued to its number, or a numeral other than an ASCII digit", CHECK_FAILED,
          "FAIL: test_a_multiplier_or_a_numeral_that_is_not_a_digit (__main__.Refuses"),
    Fault("writeup-pitch-condition-skipped", "py-scripts", "render_writeup.py judges an essay "
          "claim on the first condition of its pitch sentence alone", CHECK_FAILED,
          "FAIL: test_a_pitch_sentence_reused_on_every_condition (__main__.Claims"),
    # The step itself on the committed page. Its baseline passes only once WRITEUP.md is
    # rendered, when every results file it reads is committed (docs/TASKS.md).
    Fault("writeup-template-not-rendered", "writeup-check", "the essay's template reworded "
          "and WRITEUP.md not rendered again", CHECK_FAILED,
          "WRITEUP.md differs from a fresh render", mutate=reword_the_writeup_template),
    Fault("control-comment-only", "render-check", "a comment, which no check should notice",
          0, "", control=True),
)


def fingerprint(fault, blob_of=None):
    """Changes when the fault does, so an edited fault cannot keep an old verdict: its fields,
    its patch, the code of its mutation and of this script's verdict path, and the committed
    content of each file it edits. Prose and this script are outside source_commit, so a
    fault's own key must hold what of them made its verdict (review E-5): its judging,
    whether it is tolerated or a control (review RP-11), and the gate in main that keeps a
    stored verdict rather than proving it again (review RP-12)."""
    blob_of = blob_of or head_blobs().get
    patch = patch_of(fault)
    texts, strings = code_of(prove)[0] + code_of(main)[0], set()
    if fault.mutate is not None:
        mutation, strings = code_of(fault.mutate)
        texts += mutation
    if patch is not None:
        strings = strings | patched_paths(patch.read_text())
    edited = sorted((path, blob) for path in strings if (blob := blob_of(path)) is not None)
    described = repr((fault.check, fault.arguments, fault.fault_arguments, fault.expected_exit,
                      fault.expected_string, fault.later_step, fault.tolerated, fault.control,
                      getattr(fault.mutate, "__name__", None),
                      patch.read_text() if patch else None, texts, edited))
    return hashlib.sha256(described.encode()).hexdigest()[:12]


def code_of(function):
    """The text of a function and of every function and class of its module it reaches, with
    the plain values it reads there, and the string constants among them that may name a file.
    Source text where Python has it, which line numbers do not change, else the bytecode."""
    texts, strings, seen = [], set(), set()
    pending = [function]
    while pending:
        current = pending.pop()
        if id(current) in seen:
            continue
        seen.add(id(current))
        texts.append(source_of(current))
        if inspect.isclass(current):
            continue
        codes = [current.__code__]
        while codes:
            code = codes.pop()
            for constant in code.co_consts:
                if isinstance(constant, types.CodeType):
                    codes.append(constant)
                elif isinstance(constant, str) and " " not in constant:
                    strings.add(constant)
            for name in code.co_names:
                value = current.__globals__.get(name)
                if inspect.isfunction(value) and value.__globals__ is current.__globals__:
                    pending.append(value)
                elif inspect.isclass(value) and value.__module__ == current.__module__:
                    pending.append(value)
                elif plain(value):
                    texts.append(f"{name} = {value!r}")
    return sorted(texts), strings


# This script's text as it was loaded, so a key is of the code that runs: a commit to this
# file during a run moves its lines on disk, where inspect would read them.
LOADED_LINES = Path(__file__).read_text(encoding="utf-8").splitlines(keepends=True)


def source_of(item):
    """A function's or class's source text: from this script as loaded, from its file for
    another module's, and its bytecode where Python has no source."""
    if getattr(item, "__module__", None) == __name__:
        first = (item.__code__.co_firstlineno if inspect.isfunction(item)
                 else item.__firstlineno__)
        return "".join(inspect.getblock(LOADED_LINES[first - 1:]))
    try:
        return inspect.getsource(item)
    except (OSError, TypeError):
        return bytecode_of(item.__code__)


def bytecode_of(code):
    """A function's code where Python has no source text for it: instructions, constants and
    names, nested functions included."""
    constants = tuple(bytecode_of(constant) if isinstance(constant, types.CodeType)
                      else repr(constant) for constant in code.co_consts)
    return repr((code.co_code, constants, code.co_names))


def plain(value):
    """A value whose repr is the same on every machine: no paths, objects or modules."""
    if isinstance(value, (str, int, float, bool)):
        return True
    return isinstance(value, tuple) and all(plain(item) for item in value)


def patched_paths(patch_text):
    """The files a patch edits, from its headers."""
    return {line.split(" b/", 1)[1] for line in patch_text.splitlines()
            if line.startswith("diff --git a/") and " b/" in line}


@functools.cache
def head_blobs():
    """Every committed file's blob id at HEAD."""
    listing = git_in(REPO_ROOT, "ls-tree", "-r", "-z", "HEAD")
    blobs = {}
    for entry in listing.split("\0"):
        if "\t" in entry:
            meta, path = entry.split("\t", 1)
            blobs[path] = meta.split()[2]
    return blobs


def patch_of(fault):
    path = FAULTS_DIR / f"{fault.id}.patch"
    return path if path.exists() else None


def run(command, **options):
    return subprocess.run(command, check=False, capture_output=True, text=True, **options)


def git_in(root, *arguments):
    done = run(["git", "-C", str(root), *arguments])
    if done.returncode != 0:
        raise RuntimeError(f"git {' '.join(arguments)}: {done.stderr.strip()}")
    return done.stdout.strip()


def source_commit():
    """The last commit that changed code, so prose and these results do not undo a proof."""
    return git_in(REPO_ROOT, "rev-list", "-1", "HEAD", "--", ".", *NOT_CODE)[:12]


def require_clean_tree():
    changed = [line for line in git_in(REPO_ROOT, "status", "--porcelain").splitlines()
               if not line.endswith(("results/check-proofs.json", ".md"))]
    if changed:
        raise SystemExit("prove_checks: the proof runs on what is committed, and these files "
                         "differ:\n" + "\n".join(changed))


def compiler_name():
    cxx = os.environ.get("CXX")
    if not cxx:
        raise SystemExit("prove_checks: CXX is not set")
    return run([cxx, "--version"]).stdout.splitlines()[0].partition(" (")[0]


def prepare_tree():
    """A clone at HEAD, with its own copy of the fetched sources, so a fault can touch them."""
    WORK.mkdir(parents=True, exist_ok=True)
    if not TREE.exists():
        git_in(REPO_ROOT, "clone", "--quiet", "--no-hardlinks", str(REPO_ROOT), str(TREE))
    git_in(TREE, "fetch", "--quiet", "origin", "HEAD")
    git_in(TREE, "reset", "--quiet", "--hard", "FETCH_HEAD")
    # A link, which the .gitignore rule for a folder misses, so a clean would remove it.
    venv = REPO_ROOT / ".venv"
    if venv.exists() and not (TREE / ".venv").exists():
        (TREE / ".venv").symlink_to(venv)
    with (TREE / ".git" / "info" / "exclude").open("a") as exclude:
        exclude.write("/.venv\n")
    for name in ("googletest", "hdrhistogram", "benchmark", "zstd"):
        source = REPO_ROOT / "build" / "_deps" / f"{name}-src"
        target = TREE / "build" / "_deps" / f"{name}-src"
        if not source.exists():
            raise SystemExit(f"prove_checks: {source.name} is not fetched. Run "
                             "scripts/check.sh --only clang-tidy once first.")
        if not target.exists():
            shutil.copytree(source, target, symlinks=True)


def clean_tree():
    git_in(TREE, "reset", "--quiet", "--hard")
    git_in(TREE, "clean", "--quiet", "-fd")


def check_environment():
    tools = REPO_ROOT / ".tools"
    env = dict(os.environ, CHECK_CMAKE_ARGS="-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
               PATH=f"{tools}{os.pathsep}{os.environ['PATH']}")
    if (tools / "actionlint").exists():
        env["ACTIONLINT"] = str(tools / "actionlint")
    return env


@dataclass
class StepRun:
    script_exit: int
    status: str
    step_exit: int
    log: str
    later_status: str


def read_summary(folder):
    rows = {}
    summary = folder / "summary.tsv"
    if summary.exists():
        for line in summary.read_text().splitlines():
            step, status, code, *_ = line.split("\t")
            rows[step] = (status, code)
    return rows


def run_check(fault, arguments, label, raw_folder):
    logs = WORK / "check-logs"
    only = fault.check + (f",{fault.later_step}" if fault.later_step else "")
    command = [str(TREE / "scripts" / "check.sh"), "--log-dir", str(logs), "--only", only,
               *arguments]
    done = run(command, cwd=TREE, env=check_environment())
    rows = read_summary(logs)
    status, code = rows.get(fault.check, ("NOT RUN", "-"))
    log_path = logs / f"{fault.check}.log"
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raw_folder.mkdir(parents=True, exist_ok=True)
    (raw_folder / f"{label}.log").write_text(done.stdout + done.stderr + "\n=== step log\n" + log)
    return StepRun(done.returncode, status, int(code) if code.lstrip("-").isdigit() else -1, log,
                   rows.get(fault.later_step, ("", ""))[0])


def apply_fault(fault):
    patch = patch_of(fault)
    if patch is not None:
        done = run(["git", "-C", str(TREE), "apply", "--whitespace=nowarn", str(patch)])
        if done.returncode != 0:
            raise RuntimeError(f"{patch.name} does not apply: {done.stderr.strip()}")
    elif fault.mutate is not None:
        fault.mutate()
    elif not fault.fault_arguments:
        raise RuntimeError(f"{fault.id} changes nothing: no patch, mutation or arguments")


def revert_fault(fault):
    patch = patch_of(fault)
    # A patch to a fetched source sits in an ignored folder, which a reset does not restore.
    if patch is not None and "build/_deps/" in patch.read_text():
        run(["git", "-C", str(TREE), "apply", "--reverse", "--whitespace=nowarn", str(patch)])
    clean_tree()


def judge(fault, result):
    """The verdict for a faulted run."""
    if fault.control:
        return "UNDETECTED" if result.script_exit == 0 else "CONTROL FAILED"
    if fault.tolerated:
        passed = result.script_exit == 0 and result.status == "PASS"
        return "PROVEN" if passed and fault.expected_string in result.log else "NOT TOLERATED"
    if result.script_exit == 0:
        return "UNDETECTED"
    if result.status != "FAIL":
        return f"WRONG REASON: the step ended {result.status}"
    if result.script_exit != CHECK_FAILED:
        return f"WRONG REASON: check.sh exited {result.script_exit}"
    if result.step_exit != fault.expected_exit:
        return f"WRONG REASON: the step exited {result.step_exit}"
    if fault.expected_string not in result.log:
        return "WRONG REASON: the expected string is not in the log"
    if fault.later_step and result.later_status != "PASS":
        return f"WRONG REASON: {fault.later_step} ended {result.later_status!r}"
    return "PROVEN"


def prove(fault, raw_root):
    raw_folder = raw_root / fault.id
    entry = {
        "fault_id": fault.id,
        "check": fault.check,
        "what": fault.what,
        "kind": "patch" if patch_of(fault) else "mutation",
        "fingerprint": fingerprint(fault),
        "expected_exit": fault.expected_exit,
        "expected_string": fault.expected_string,
        "actual_exit": None,
        "matched_string": None,
        "baseline_passed": False,
        "verdict": "NOT RUN",
    }
    clean_tree()
    baseline = run_check(fault, fault.arguments, "baseline", raw_folder)
    entry["baseline_passed"] = baseline.script_exit == 0 and baseline.status == "PASS"
    if not entry["baseline_passed"]:
        entry["verdict"] = "BASELINE FAILED"
        return entry

    apply_fault(fault)
    try:
        result = run_check(fault, fault.arguments + fault.fault_arguments, "fault", raw_folder)
    finally:
        revert_fault(fault)
    entry["actual_exit"] = result.step_exit
    if fault.expected_string and fault.expected_string in result.log:
        entry["matched_string"] = fault.expected_string
    entry["verdict"] = judge(fault, result)
    return entry


def is_proven(entry):
    return entry.get("verdict") == "PROVEN" or (entry.get("control") is True and
                                                entry.get("verdict") == "UNDETECTED")


def load_results(commit):
    if OUTPUT.exists():
        document = json.loads(OUTPUT.read_text())
        if document.get("source_commit") == commit:
            return {entry["fault_id"]: entry for entry in document["faults"]}
    return {}


def write_results(commit, compiler, entries):
    ordered = [entries[fault.id] for fault in FAULTS if fault.id in entries]
    document = {
        "schema": SCHEMA,
        "source_commit": commit,
        "platform": f"{platform.system()} {platform.machine()}",
        "compiler": compiler,
        "statement": "Each fault ran in a throwaway clone. Baseline passed, the fault applied, "
                     "the same step failed with this exit code and string, the fault reverted.",
        "faults": ordered,
    }
    partial = OUTPUT.with_suffix(".partial")
    partial.write_text(json.dumps(document, indent=2) + "\n")
    partial.replace(OUTPUT)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Show every check failing on a fault.")
    parser.add_argument("--force", action="store_true", help="redo faults already proven")
    parser.add_argument("--only", help="comma-separated fault ids")
    parser.add_argument("--raw-logs", type=Path,
                        default=os.environ.get("CHECK_PROOF_LOGS") or WORK / "logs")
    parser.add_argument("--list", action="store_true", help="print the fault ids")
    args = parser.parse_args(argv)

    if args.list:
        print("\n".join(fault.id for fault in FAULTS))
        return 0
    chosen = list(FAULTS)
    if args.only:
        wanted = set(args.only.split(","))
        unknown = wanted - {fault.id for fault in FAULTS}
        if unknown:
            raise SystemExit(f"prove_checks: unknown faults: {sorted(unknown)}")
        chosen = [fault for fault in FAULTS if fault.id in wanted]

    require_clean_tree()
    compiler = compiler_name()
    commit = source_commit()
    entries = load_results(commit)
    prepare_tree()
    args.raw_logs.mkdir(parents=True, exist_ok=True)

    failed = 0
    for fault in chosen:
        known = entries.get(fault.id)
        current = known is not None and known.get("fingerprint") == fingerprint(fault)
        if current and is_proven(known) and not args.force:
            print(f"{fault.id}: already {known['verdict']} at {commit}", flush=True)
            continue
        print(f"{fault.id}: {fault.check} ...", flush=True)
        entry = prove(fault, args.raw_logs)
        if fault.control:
            entry["control"] = True
        entries[fault.id] = entry
        write_results(commit, compiler, entries)
        print(f"  baseline {entry['baseline_passed']}, exit {entry['actual_exit']}, "
              f"{entry['verdict']}", flush=True)
        if not is_proven(entry):
            failed += 1
    print(f"{len(chosen) - failed} of {len(chosen)} faults as expected", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
