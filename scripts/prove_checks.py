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

TIDY_ONE_FILE = ("--tidy-files", "src/itch/frame.cpp")


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


def macos_results(folder, root=TREE):
    """The one macOS results file in results/<folder>, by pattern, so a re-measure cannot
    strand the faults that edit it."""
    found = sorted((root / "results" / folder).glob("macOS-*.json"))
    if len(found) != 1:
        raise SystemExit(f"prove_checks: results/{folder} needs exactly one macOS-*.json, "
                         f"and has {len(found)}: {[path.name for path in found]}")
    return found[0]


def linux_results(folder, root=TREE):
    """The one Linux results file in results/<folder>, from the pinned host."""
    found = sorted((root / "results" / folder).glob("Linux-*.json"))
    if len(found) != 1:
        raise SystemExit(f"prove_checks: results/{folder} needs exactly one Linux-*.json, "
                         f"and has {len(found)}: {[path.name for path in found]}")
    return found[0]


def add_file(path, content):
    target = TREE / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    subprocess.run(["git", "-C", str(TREE), "add", "-f", path], check=True)


def drop_load_lines():
    edit_json(macos_results("bench"), lambda d: d["header"].update(load_lines=[]))


def unmeasured_turbo_on_linux():
    """What run_bench.py records on a host with no boost switch, such as a hosted runner."""
    edit_json(linux_results("bench"), lambda d: d["header"].update(
        turbo="unavailable: no cpufreq boost switch, as on a virtual machine"))


def claim_pinned_on_macos():
    edit_json(macos_results("bench"), lambda d: d["header"].update(pinning="pinned"))


def sub_resolution_percentile():
    def change(document):
        document["results"][0]["latency"]["percentiles"][0]["value_ns"] = 0.5
    edit_json(macos_results("bench"), change)


def questdb_variant(document):
    return next(iter(document["systems"]["questdb"]["variants"].values()))


def mismatched_commit_mode():
    def change(document):
        for setting in questdb_variant(document)["classes"]["B"]["settings"]:
            if setting["name"] == "cairo.commit.mode (conf file)":
                setting["in_force"] = "nosync"
    edit_json(macos_results("compare"), change)


def failed_read_back():
    def change(document):
        oracle = questdb_variant(document)["classes"]["B"]["read_back"]["oracle"]
        oracle["verdict"] = "mismatch"
        oracle["exit_code"] = 1
    edit_json(macos_results("compare"), change)


def query_digest_mismatch():
    def change(document):
        questdb_variant(document)["queries"]["Q1"]["digest"] = "0" * 64
    edit_json(macos_results("compare"), change)


def hand_edit_readme():
    edit_text("README.md", "**macOS.**", "**macOS, hand edited.**")


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
    Fault("fuzz-pfor-head-delta-escape", "fuzz-pfor", "the head-delta inverse puts an escaped "
          "value over the front head instead of pushing the heads down", EXIT_LIBFUZZER,
          "check_pfor_column:", arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-dictionary-repeated-entry", "fuzz-dictionary", "the dictionary decoder stops "
          "refusing a ranked dictionary that holds one value twice", EXIT_LIBFUZZER,
          "check_dict_column:", arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-run-length-maximal-runs", "fuzz-run-length", "the run-length decoder stops "
          "refusing two neighbouring runs of one value", EXIT_LIBFUZZER, "check_rle_column:",
          arguments=FUZZ_ARGUMENTS),
    Fault("fuzz-container-metadata-start", "fuzz-container", "the container parser starts the "
          "footer's metadata area one byte past the section entries", EXIT_LIBFUZZER,
          "check_container:", arguments=FUZZ_ARGUMENTS),
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
    Fault("g1-store-alters-a-byte", "oracle", "the ITCH store's writer flips the low bit of every "
          "add order's shares, so record 2 re-serialises one bit off", CHECK_FAILED,
          "first difference: bytes at record 2, source byte"),
    Fault("itch-store-import-not-appended", "dev-test", "the ITCH store's writer stops appending "
          "an import to its locate's list, so the distances after it in the segment are off",
          EXIT_CTEST, "ItchStoreReferences.AnOrderFromAnEarlierSegmentIsImportedOnceThenADistance",
          arguments=("--test-regex", "ItchStore")),
    Fault("fuzz-itch-store-distance-bound", "fuzz-itch-store", "the ITCH store's segment decoder "
          "stops checking a reference distance against its locate's list", EXIT_LIBFUZZER,
          # ASan or UBSan, by whether the list is empty; either trace names the function.
          "in tick_store::store::SegmentAssembler::State::named", arguments=FUZZ_ARGUMENTS),
    Fault("g2-weakened-compare", "dev-test", "the oracle's compare ignores bit 0 and stops "
          "requiring the hashes to match", EXIT_CTEST, "Value of: report.passed()",
          arguments=("--test-regex", "OracleExhaustiveFlip")),
    Fault("data-sha256-removed", "py-scripts", "fetch_data.py stops comparing SHA-256 digests",
          CHECK_FAILED, "FAIL: test_one_flipped_byte (__main__.CompressedHash"),
    Fault("taskset-removed", "py-scripts", "run_bench.py drops the taskset -c 4 pin from a "
          "Linux case's command", CHECK_FAILED,
          "FAIL: test_a_linux_case_records_its_siblings_busy_share (__main__.RunBenchTest"),
    Fault("compare-tests-settings", "py-compare", "the QuestDB commit mode check stops checking",
          CHECK_FAILED, "FAIL: test_the_parser_outranks_a_matching_echo"),
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
    Fault("readme-hand-edit", "render-check", "a hand edit inside the README's table markers",
          CHECK_FAILED, "stale or was edited by hand", hand_edit_readme),
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
