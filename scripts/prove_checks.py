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
commit that changed code, not prose or the summaries, plus a fingerprint of the fault itself.
Raw logs go to --raw-logs or CHECK_PROOF_LOGS, by default build/fault-proofs/logs. The
toolchain comes from the environment.

Standard library only.
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
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


def host_results(host, folder, root=TREE, benchmark=None):
    """The one results file of a host in results/<folder>, by pattern, so a re-measure cannot
    strand the faults that edit it. In a folder of several benchmarks, name the benchmark."""
    pattern = f"{host}-{benchmark}-*.json" if benchmark else f"{host}-*.json"
    found = sorted((root / "results" / folder).glob(pattern))
    if len(found) != 1:
        raise SystemExit(f"prove_checks: results/{folder} needs exactly one {pattern}, "
                         f"and has {len(found)}: {[path.name for path in found]}")
    return found[0]


def macos_results(folder, root=TREE, benchmark=None):
    return host_results("macOS", folder, root, benchmark)


def linux_results(folder, root=TREE, benchmark=None):
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


def mismatched_commit_mode():
    def change(document):
        for setting in questdb_variant(document)["classes"]["B"]["settings"]:
            if setting["name"] == "cairo.commit.mode (conf file)":
                setting["in_force"] = "nosync"
    edit_json(macos_results("compare", benchmark=COMPARE_FAULT_SLICE), change)


def failed_read_back():
    def change(document):
        oracle = questdb_variant(document)["classes"]["B"]["read_back"]["oracle"]
        oracle["verdict"] = "mismatch"
        oracle["exit_code"] = 1
    edit_json(macos_results("compare", benchmark=COMPARE_FAULT_SLICE), change)


def query_digest_mismatch():
    def change(document):
        questdb_variant(document)["queries"]["Q1"]["digest"] = "0" * 64
    edit_json(macos_results("compare", benchmark=COMPARE_FAULT_SLICE), change)


def unflagged_swap_in():
    def change(document):
        cold = questdb_variant(document)["queries"]["Q1"]["cold"]["swap_timed"]
        cold["pages_in"] += 1
        cold["pages_in_by_repetition"][0] += 1
        cold["flagged"] = False
    edit_json(macos_results("compare", benchmark=COMPARE_FAULT_SLICE), change)


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
    Fault("compare-full-day-clipped-window", "py-scripts", "check_compare.py stops comparing a "
          "full-day file's query windows with BENCHMARKS.md's, so an hour clipped to the slice "
          "passes as a full day", CHECK_FAILED,
          "FAIL: test_a_clipped_hour_passing_as_a_full_day (__main__.FullDay"),
    Fault("compare-tickstore-only-on-linux", "py-scripts", "check_compare.py stops refusing a "
          "tick-store-only full day from Linux, so a subset of the four-system day could pass "
          "as a full-day result", CHECK_FAILED,
          "FAIL: test_a_tickstore_only_day_on_linux (__main__.TickstoreOnly"),
    # The comparison protocol 2 rules (docs/REVIEW.md, F-4 to F-11 and E-3).
    Fault("compare-protocol-unknown-accepted", "py-scripts", "check_compare.py takes a file of "
          "a protocol it does not know, protocol 3, as if it knew its rules", CHECK_FAILED,
          "FAIL: test_an_unknown_protocol (__main__.Protocol2"),
    # Review RF-1: protocol 1 belongs to the committed files measured before protocol 2.
    Fault("compare-protocol-1-any-commit", "py-scripts", "check_compare.py takes a file that "
          "states no protocol from any commit, so a document of the current harness escapes "
          "every protocol 2 rule by leaving its protocol out", CHECK_FAILED,
          "FAIL: test_a_current_harness_document_without_its_protocol (__main__.Protocol2"),
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
    Fault("compare-macos-write-counter-unchecked", "py-scripts", "check_compare.py takes a "
          "macOS write whose device reads are iostat's disk0 total, which holds the write's own "
          "output, as its count of reads (review RF-5)", CHECK_FAILED,
          "FAIL: test_a_macos_write_counting_disk0s_reads_and_writes (__main__.Protocol2"),
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
    Fault("coverage-required-removed", "coverage", "bench_support_test.cpp dropped from the "
          "tests, so the guard and the round-trip check lose their only callers", CHECK_FAILED,
          "REQUIRED FUNCTION NOT REACHED: verify_round_trip"),
    Fault("control-comment-only", "render-check", "a comment, which no check should notice",
          0, "", control=True),
)


def fingerprint(fault):
    """Changes when the fault does, so an edited fault cannot keep an old verdict."""
    patch = patch_of(fault)
    described = repr((fault.check, fault.arguments, fault.fault_arguments, fault.expected_exit,
                      fault.expected_string, fault.later_step,
                      getattr(fault.mutate, "__name__", None),
                      patch.read_text() if patch else None))
    return hashlib.sha256(described.encode()).hexdigest()[:12]


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
