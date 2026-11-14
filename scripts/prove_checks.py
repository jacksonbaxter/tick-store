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
    target = TREE / path
    document = json.loads(target.read_text())
    change(document)
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


def sub_resolution_percentile():
    def change(document):
        document["results"][0]["latency"]["percentiles"][0]["value_ns"] = 0.5
    edit_json(macos_results("bench", benchmark=BENCH_FAULT_BENCHMARK), change)


def questdb_variant(document):
    return next(iter(document["systems"]["questdb"]["variants"].values()))


def edit_compare_slice(change):
    """The macOS slice, edited, with its pin moved to the edited bytes. A protocol 1 file is
    known by its SHA-256 in scripts/data_pins.json, which would refuse any edit first, so a
    fault in its content re-pins it, and only the rule the fault is about can refuse it."""
    path = macos_results("compare", benchmark=COMPARE_FAULT_SLICE)
    edit_json(path, change)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()

    def repin(pins):
        pins["protocol_1_comparison_files"][path.name] = digest
    edit_json("scripts/data_pins.json", repin)


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
    """12302019's round-trip file as the assembler of 1669ae0 wrote it, G-2's two exit codes
    typed and none recorded on a run, from any later commit: check_roundtrip.py takes that
    form only from the three files pinned by their bytes (reviews E-1 and RG-5)."""
    edit_json("results/roundtrip-12302019.json", lambda d: d.update(commit="0123456789ab"))


def edit_a_pinned_roundtrip_file():
    """12302019's round-trip file, its commit kept, with a field added by hand, which nothing
    renders: only its bytes tell it from the file pinned (review RG-5)."""
    edit_json("results/roundtrip-12302019.json",
              lambda d: d["g2"]["data_crc"].update(detail="typed by hand"))


def hand_edit_targets_value():
    """WT-1 on Linux in RESULTS.md's Targets block, its figure raised past the target and the
    verdict turned to met."""
    edit_table_cells("RESULTS.md", "| WT-1 | Linux |", {3: "9.000 M records/s", 4: "met"})


def hand_edit_compare_value():
    """tick-store's Q2 warm median in RESULTS.md's first comparison table, the Linux full day."""
    edit_table_cells("RESULTS.md", "| Q2 | tick-store |", {4: "1.00"})


def hand_edit_headline_value():
    """WT-1 in the README's headline, its figure raised past the target and the verdict met."""
    edit_table_cells("README.md", "| [WT-1](RESULTS.md#targets),", {1: "9.000 M records/s",
                                                                   3: "met"})


def hand_edit_pitch_figure():
    """The README's pitch, its one-thread ingest raised by hand (review E-9)."""
    edit_text("README.md", "reaches 1.376 M records/s", "reaches 2.376 M records/s")


def swap_pitch_store_sizes():
    """The README's pitch with the source's size and the store's in each other's places, both
    still figures the results give (review RG-4)."""
    document = json.loads((TREE / "results" / "store-size-12302019.json").read_text())
    source, stored = (f"{document[key] * 1e-9:.2f} GB" for key in ("source_bytes",
                                                                   "stored_bytes"))
    edit_text("README.md", f"{source}, in {stored}", f"{stored}, in {source}")


def reword_a_pitch_claim():
    """The pitch's Q1 and Q4 sentence reworded to rank tick-store first, with no figure in it
    for the figure check to catch (review RG-13)."""
    edit_text("README.md", "ArcticDB or QuestDB is fastest", "tick-store is fastest")


def type_an_unchecked_pitch_figure():
    """A figure typed into the README's pitch that no row of its check covers (review E-9)."""
    edit_text("README.md", "It misses targets too.", "It is 42 times faster. It misses targets too.")


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
          "source file and in a header our tests include", 0,
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
    Fault("fuzz-dictionary-repeated-entry", "fuzz-dictionary", "the dictionary decoder stops "
          "refusing a ranked dictionary that holds one value twice", EXIT_LIBFUZZER,
          "check_dict_column:", arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-dictionary-mode-set-ignored", "fuzz-dictionary", "the dictionary encoder tries "
          "Huffman whatever its mode set allows, so a column held to ranked PFOR can take "
          "Huffman", EXIT_LIBFUZZER, "check_dict_column:", arguments=FUZZ_ARGUMENTS),
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
    Fault("fuzz-host-absolute-path-kept", "py-scripts", "fuzz_host.py writes host.json with "
          "an absolute path in it", CHECK_FAILED,
          "FAIL: test_no_absolute_path_is_left_in_the_file (__main__.FuzzHost"),
    Fault("fuzz-host-name-kept", "py-scripts", "fuzz_host.py writes host.json with the host's "
          "name in it", CHECK_FAILED,
          "FAIL: test_the_hosts_name_is_never_in_the_file (__main__.FuzzHost"),
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
    Fault("fuzz-g3-pin-any-content", "py-scripts", "check_fuzz.py lets any file named as the "
          "ea1dde4 one through the tie, whatever its bytes, where only the pinned SHA-256 may "
          "pass", CHECK_FAILED,
          "FAIL: test_the_pin_lets_through_no_other_content (__main__.TieTest"),
    Fault("fuzz-g3-pin-other-file", "py-scripts", "check_fuzz.py takes a pin for any file, so a "
          "new G-3 file could be pinned past the tie", CHECK_FAILED,
          "FAIL: test_no_other_file_may_be_pinned (__main__.TieTest"),
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
          "before the launcher in any file, not only in the pinned ea1dde4 one, so a new file "
          "could record no replay", CHECK_FAILED,
          "FAIL: test_only_the_pinned_file_may_keep_the_schema_from_before_the_launcher "
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
    Fault("roundtrip-exit-codes-typed", "render-check", "a round-trip file from a later commit "
          "with G-2's two exit codes typed and none observed on its runs, as the assembler "
          "wrote them before review E-1", EXIT_RENDER_REFUSED,
          "g2.data_crc: no exit_code observed", roundtrip_exit_codes_typed),
    # Review RG-5: the files from before the exit codes are known by their bytes alone.
    Fault("roundtrip-pinned-name-other-bytes", "render-check", "12302019's round-trip file "
          "edited by hand, its commit kept, so only its bytes differ from the pinned file's",
          EXIT_RENDER_REFUSED, "roundtrip-12302019.json: g1.ingest: no exit_code observed",
          edit_a_pinned_roundtrip_file),
    Fault("roundtrip-pin-other-file-accepted", "py-scripts", "check_roundtrip.py takes a pin "
          "for any file, so a new round-trip file could be pinned past its exit codes",
          CHECK_FAILED, "FAIL: test_no_other_file_may_be_pinned (__main__.CheckRoundtrip"),
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
          "README's pitch: tick-store's one-thread ingest shown as 2.376 M records/s",
          EXIT_RENDER_REFUSED, "the README's pitch does not state '1.376 M records/s'",
          hand_edit_pitch_figure),
    Fault("readme-pitch-unchecked-figure", "render-check", "a figure typed into the README's "
          "pitch that nothing checks", EXIT_RENDER_REFUSED,
          "a figure in the README's pitch that nothing checks: '42'",
          type_an_unchecked_pitch_figure),
    # Reviews RG-4 and RG-13: a pitch figure is bound to its words, and a ranking to the results.
    Fault("readme-pitch-store-figures-swapped", "render-check", "the README's pitch with the "
          "source's size and the store's traded, both still figures the results give",
          EXIT_RENDER_REFUSED, "the README's pitch does not state '268,744,780 messages and",
          swap_pitch_store_sizes),
    Fault("render-pitch-shared-format-accepted", "py-scripts", "render_results.py takes two "
          "pitch figures of one format, which could trade places in the text unseen",
          CHECK_FAILED,
          "FAIL: test_two_figures_of_one_format_are_refused (__main__.RenderPitchTest"),
    Fault("readme-pitch-claim-reworded", "render-check", "the pitch's Q1 and Q4 sentence "
          "reworded to rank tick-store first, with no figure in it", EXIT_RENDER_REFUSED,
          "the README's pitch no longer says 'ArcticDB or QuestDB is fastest'",
          reword_a_pitch_claim),
    Fault("render-pitch-claim-unchecked", "py-scripts", "render_results.py finds each claim's "
          "words in the pitch and never judges its condition, so a ranking the results reverse "
          "still passes", CHECK_FAILED,
          "FAIL: test_a_claim_the_results_no_longer_bear_out_stops_the_check "
          "(__main__.RenderPitchTest"),
    # Review RG-6: G-3's file from before the tie is not judged at this code.
    Fault("render-g3-before-the-tie-judged", "py-scripts", "render_results.py judges G-3 met on "
          "the file made before G-3 was tied to the code, which judged an older commit",
          CHECK_FAILED, "FAIL: test_the_file_from_before_the_tie_is_not_judged_at_this_code "
          "(__main__.RenderPreTieTest"),
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
    Fault("readme-headline-hand-edit", "render-check", "a hand edit of a value in the README's "
          "headline: WT-1 shown as 9.000 M records/s and met", CHECK_FAILED,
          "the table in README.md is stale or was edited by hand", hand_edit_headline_value),
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
          CHECK_FAILED, "FAIL: test_a_row_of_some_parts_names_the_rest_below "
          "(__main__.RenderHeadlineTest"),
    Fault("actionlint-broken-workflow", "actionlint", "the workflow names a matrix key that does "
          "not exist", CHECK_FAILED, "no_such_key", break_workflow),
    Fault("hygiene-home-path", "hygiene", "a tracked file holds a home path", CHECK_FAILED,
          "notes-injected.txt:1: absolute home path", inject_home_path),
    Fault("hygiene-data-file", "hygiene", "a tracked market data file", CHECK_FAILED,
          "12302019.NASDAQ_ITCH50: data file is tracked", inject_data_file),
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
    # Review E-5: a proof's key holds the code and prose that made its verdict.
    Fault("prover-key-names-only", "py-scripts", "the prover's key for a proof holds a "
          "mutation's name and no code or prose, so an edited mutation keeps its old proof",
          CHECK_FAILED, "FAIL: test_a_mutation_with_another_body_has_another_fingerprint "
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
    # The coverage summary says which kernel sets its gate requires.
    Fault("coverage-kernel-statement-stale", "py-scripts", "coverage_report.py writes that no "
          "SIMD kernel exists, though its gate requires the scalar set and this processor's",
          CHECK_FAILED, "FAIL: test_the_kernel_statement_is_the_kernel_sets_required "
          "(__main__.RequiredList"),
    Fault("coverage-required-removed", "coverage", "bench_support_test.cpp dropped from the "
          "tests, so the guard and the round-trip check lose their only callers", CHECK_FAILED,
          "REQUIRED FUNCTION NOT REACHED: verify_round_trip"),
    Fault("control-comment-only", "render-check", "a comment, which no check should notice",
          0, "", control=True),
)


def fingerprint(fault, blob_of=None):
    """Changes when the fault does, so an edited fault cannot keep an old verdict: its fields,
    its patch, the code of its mutation and of this script's verdict path, and the committed
    content of each file it edits. Prose and this script are outside source_commit, so a
    fault's own key must hold what of them made its verdict (review E-5)."""
    blob_of = blob_of or head_blobs().get
    patch = patch_of(fault)
    texts, strings = code_of(prove)[0], set()
    if fault.mutate is not None:
        mutation, strings = code_of(fault.mutate)
        texts += mutation
    if patch is not None:
        strings = strings | patched_paths(patch.read_text())
    edited = sorted((path, blob) for path in strings if (blob := blob_of(path)) is not None)
    described = repr((fault.check, fault.arguments, fault.fault_arguments, fault.expected_exit,
                      fault.expected_string, fault.later_step,
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
