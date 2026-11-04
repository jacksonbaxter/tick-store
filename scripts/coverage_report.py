#!/usr/bin/env python3
"""Turn an llvm-cov export into the curated summary, and gate on the required functions.

    python3 -I scripts/coverage_report.py export.json output.json

scripts/coverage.sh is the normal way in. The report gives line and function coverage per file
for src/ and include/. The gate fails if a function below is missing from the binary or was
never called: every decode_as and encode_as for the 23 message types, each parser and validator
entry point, the codecs' decoders and encoders, the container's parser, writer and durability
calls, CRC32C in software and in this processor's hardware kernel, the fuzz oracles, and the
oracle's compare path. A name that stops matching fails too, so the list cannot go stale in
silence.

The list holds no SIMD kernel because none exists yet. The Phase 3 kernels add theirs here.
llvm-cov sees only C++. scripts/check_results.py and scripts/check_compare.py are shown failing
rule by rule in tests/scripts/, not measured here.

Standard library only, apart from llvm-cxxfilt on PATH.
"""

import json
import platform
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MESSAGE_HEADER = "include/tick_store/itch/message.hpp"
MESSAGE_TYPES = 23
SCHEMA = "tick-store-coverage-1"
REPORTED = ("src/", "include/")
SIMD_STATEMENT = "No SIMD kernels exist yet, so none are reported and none are claimed."
PYTHON_STATEMENT = ("llvm-cov measures the C++ only. The Python validators are shown failing "
                    "rule by rule in tests/scripts/ instead.")
RUN_STATEMENT = "tests/unit, with ItchRealDay left out so the report needs no data file."

CODEC = "src/itch/codec.cpp"
FRAME = "include/tick_store/itch/frame.hpp"
BIG_ENDIAN = "include/tick_store/itch/big_endian.hpp"
LITTLE_ENDIAN = "include/tick_store/store/little_endian.hpp"
STUB_STORE = "src/store/stub_store.cpp"
COLUMNS = "src/store/columns.cpp"
MAPPED_FILE = "src/store/mapped_file.cpp"
ROUNDTRIP = "src/harness/roundtrip.cpp"
RESOLUTION_GUARD = "src/bench_support/resolution_guard.cpp"
STREAM_CHECK = "src/bench_support/stream_check.cpp"
STUB_READER = "src/harness/stub_reader.cpp"
COLUMNS_READER = "src/harness/columns_reader.cpp"
MESSAGE_ORACLES = "fuzz/itch_oracles.hpp"
STORE_ORACLES = "fuzz/store_oracles.hpp"
COLUMNS_ORACLES = "fuzz/columns_oracles.hpp"
BIT_STREAM = "include/tick_store/codec/bit_stream.hpp"
BLOCK = "src/codec/block.cpp"
GORILLA_TIMESTAMPS = "src/codec/gorilla_timestamps.cpp"
GORILLA_XOR = "src/codec/gorilla_xor.cpp"
PRICE_DOUBLE = "src/codec/price_double.cpp"
CODEC_ORACLES = "fuzz/codec_oracles.hpp"
LANE_PACK = "src/codec/lane_pack.cpp"
PATCH_LIST = "src/codec/patch_list.cpp"
PFOR = "src/codec/pfor.cpp"
STREAM_VBYTE = "src/codec/stream_vbyte.cpp"
DICTIONARY = "src/codec/dictionary.cpp"
RUN_LENGTH = "src/codec/run_length.cpp"
CONTAINER = "src/store/container.cpp"
CONTAINER_WRITER = "src/store/container_writer.cpp"
CONTAINER_READER = "src/store/container_reader.cpp"
DURABILITY = "src/store/durability.cpp"
CRC32C = "src/store/crc32c.cpp"
CONTAINER_ORACLES = "fuzz/container_oracles.hpp"
ITCH_STORE = "src/store/itch_store.cpp"
ITCH_SEGMENT = "src/store/itch_segment.cpp"
ITCH_STORE_READER = "src/store/itch_store_reader.cpp"
ITCH_STORE_WRITER = "src/store/itch_store_writer.cpp"
ZSTD_FRAME = "src/codec/zstd_frame.cpp"
ITCH_STORE_RECORD_READER = "src/harness/itch_store_record_reader.cpp"
ITCH_STORE_ORACLES = "fuzz/itch_store_oracles.hpp"

# Each is (group, file, what to call it, pattern over the demangled name).
FIXED_REQUIRED = (
    ("parser", CODEC, "decode", r"^tick_store::itch::decode\("),
    ("parser", CODEC, "decode_strict", r"^tick_store::itch::decode_strict\("),
    ("parser", CODEC, "encode", r"^tick_store::itch::encode\("),
    ("parser", CODEC, "encode_frame", r"^tick_store::itch::encode_frame\("),
    ("parser", CODEC, "wire_size_of", r"^tick_store::itch::wire_size_of\("),
    ("parser", CODEC, "encode_as<RawMessage>", r"encode_as\(tick_store::itch::RawMessage"),
    ("parser", FRAME, "FrameCursor::next", r"FrameCursor::next\("),
    ("parser", BIG_ENDIAN, "BigEndianReader::read_uint", r"BigEndianReader::read_uint<"),
    ("parser", BIG_ENDIAN, "BigEndianReader::read_byte", r"BigEndianReader::read_byte\("),
    ("parser", BIG_ENDIAN, "BigEndianReader::read_bytes", r"BigEndianReader::read_bytes<"),
    ("parser", LITTLE_ENDIAN, "read_le", r"read_le<"),
    ("parser", STUB_STORE, "StoreParser::open", r"StoreParser::open\("),
    ("parser", STUB_STORE, "StoreParser::next", r"StoreParser::next\("),
    ("parser", COLUMNS, "parse_columns_manifest", r"parse_columns_manifest\("),
    ("parser", COLUMNS, "ColumnsParser::open", r"ColumnsParser::open\("),
    ("parser", COLUMNS, "ColumnsParser::next", r"ColumnsParser::next\("),
    ("parser", COLUMNS, "ColumnsParser::rebuild_row", r"ColumnsParser::rebuild_row\("),
    ("parser", MAPPED_FILE, "MappedFile::open", r"MappedFile::open\("),
    ("parser", CONTAINER, "ContainerParser::open", r"ContainerParser::open\("),
    ("parser", CONTAINER, "ContainerParser::block_info", r"ContainerParser::block_info\("),
    ("parser", CONTAINER, "ContainerParser::block_with", r"ContainerParser::block_with\("),
    ("parser", CONTAINER, "ContainerParser::section_info", r"ContainerParser::section_info\("),
    ("parser", CONTAINER, "ContainerParser::section_at", r"ContainerParser::section_at\("),
    ("parser", CONTAINER, "ContainerParser::section", r"ContainerParser::section\("),
    ("parser", CONTAINER, "ContainerParser::verify_all", r"ContainerParser::verify_all\("),
    ("parser", CONTAINER_READER, "ContainerReader::open", r"ContainerReader::open\("),
    ("parser", CONTAINER_READER, "ContainerReader::block", r"ContainerReader::block\("),
    ("parser", CONTAINER_READER, "ContainerReader::section", r"ContainerReader::section\("),
    ("writer", CONTAINER, "ContainerLayout::add_block", r"ContainerLayout::add_block\("),
    ("writer", CONTAINER, "ContainerLayout::add_section", r"ContainerLayout::add_section\("),
    ("writer", CONTAINER, "ContainerLayout::encode_footer_and_trailer",
     r"ContainerLayout::encode_footer_and_trailer\("),
    ("writer", CONTAINER, "build_container", r"store::build_container\("),
    ("writer", CONTAINER_WRITER, "ContainerWriter::create", r"ContainerWriter::create\("),
    ("writer", CONTAINER_WRITER, "ContainerWriter::add_block", r"ContainerWriter::add_block\("),
    ("writer", CONTAINER_WRITER, "ContainerWriter::add_section",
     r"ContainerWriter::add_section\("),
    ("writer", CONTAINER_WRITER, "ContainerWriter::publish", r"ContainerWriter::publish\("),
    ("writer", DURABILITY, "sync_file", r"store::sync_file\("),
    ("writer", DURABILITY, "sync_directory", r"store::sync_directory\("),
    ("writer", DURABILITY, "full_fsync_or_fallback", r"full_fsync_or_fallback\("),
    ("writer", DURABILITY, "write_all", r"store::write_all\("),
    ("parser", ITCH_STORE, "parse_store_header", r"store::parse_store_header\("),
    ("parser", ITCH_STORE, "SymbolTable::parse", r"SymbolTable::parse\("),
    ("parser", ITCH_STORE, "parse_column_metadata", r"store::parse_column_metadata\("),
    ("parser", ITCH_STORE, "decode_column", r"^tick_store::store::decode_column\("),
    ("parser", ITCH_STORE_READER, "ItchStoreReader::open", r"ItchStoreReader::open\("),
    ("parser", ITCH_STORE_READER, "ItchStoreReader::open_bytes", r"ItchStoreReader::open_bytes\("),
    ("parser", ITCH_STORE_READER, "ItchStoreReader::read_sections",
     r"ItchStoreReader::read_sections\("),
    ("parser", ITCH_STORE_READER, "ItchStoreReader::read_directory",
     r"ItchStoreReader::read_directory\("),
    ("parser", ITCH_STORE_READER, "ItchStoreReader::read_column", r"ItchStoreReader::read_column\("),
    ("parser", ITCH_STORE_READER, "ItchStoreReader::read_columns",
     r"ItchStoreReader::read_columns\("),
    ("parser", ITCH_STORE_READER, "ItchStoreReader::tail", r"ItchStoreReader::tail\("),
    ("parser", ITCH_STORE_READER, "SegmentReader::seek", r"SegmentReader::seek\("),
    ("parser", ITCH_SEGMENT, "SegmentAssembler::load", r"SegmentAssembler::load\("),
    ("parser", ITCH_SEGMENT, "SegmentAssembler::next", r"SegmentAssembler::next\("),
    ("parser", ZSTD_FRAME, "decode_zstd_payload", r"codec::decode_zstd_payload\("),
    ("writer", ITCH_STORE, "encode_store_header", r"store::encode_store_header\("),
    ("writer", ITCH_STORE, "SymbolTable::encode", r"SymbolTable::encode\("),
    ("writer", ITCH_STORE, "encode_column_metadata", r"store::encode_column_metadata\("),
    ("writer", ITCH_STORE, "encode_column", r"^tick_store::store::encode_column\("),
    ("writer", ITCH_SEGMENT, "SegmentBuilder::add", r"SegmentBuilder::add\("),
    ("writer", ITCH_SEGMENT, "SegmentBuilder::finish", r"SegmentBuilder::finish\("),
    ("writer", ITCH_STORE_WRITER, "ItchStoreWriter::create", r"ItchStoreWriter::create\("),
    ("writer", ITCH_STORE_WRITER, "ItchStoreWriter::in_memory", r"ItchStoreWriter::in_memory\("),
    ("writer", ITCH_STORE_WRITER, "ItchStoreWriter::append", r"ItchStoreWriter::append\("),
    ("writer", ITCH_STORE_WRITER, "ItchStoreWriter::write_segment",
     r"ItchStoreWriter::write_segment\("),
    ("writer", ITCH_STORE_WRITER, "ItchStoreWriter::finish", r"ItchStoreWriter::finish\("),
    ("writer", ZSTD_FRAME, "encode_zstd_payload", r"codec::encode_zstd_payload\("),
    ("codec", CRC32C, "crc32c", r"^tick_store::store::crc32c\("),
    ("codec", CRC32C, "crc32c_with", r"crc32c_with\("),
    ("codec", CRC32C, "crc32c_software", r"detail::crc32c_software\("),
    ("parser", STUB_READER, "StubRecordReader::next", r"StubRecordReader::next\("),
    ("parser", COLUMNS_READER, "ColumnsReader::open", r"ColumnsReader::open\("),
    ("parser", COLUMNS_READER, "ColumnsReader::next", r"ColumnsReader::next\("),
    ("codec", BIT_STREAM, "BitReader::read", r"BitReader::read\("),
    ("codec", BIT_STREAM, "BitReader::finish", r"BitReader::finish\("),
    ("codec", BLOCK, "read_block_header", r"read_block_header\("),
    ("codec", BLOCK, "decode_column", r"codec::decode_column\("),
    ("codec", GORILLA_TIMESTAMPS, "decode_timestamp_block", r"decode_timestamp_block\("),
    ("codec", GORILLA_TIMESTAMPS, "decode_timestamps", r"codec::decode_timestamps\("),
    ("codec", GORILLA_XOR, "decode_xor_block", r"decode_xor_block\("),
    ("codec", GORILLA_XOR, "decode_xor", r"codec::decode_xor\("),
    ("codec", GORILLA_XOR, "decode_xor_doubles", r"decode_xor_doubles\("),
    ("codec", PRICE_DOUBLE, "double_to_scaled", r"double_to_scaled\("),
    ("codec", BIT_STREAM, "BitWriter::write", r"BitWriter::write\("),
    ("codec", BIT_STREAM, "BitWriter::finish", r"BitWriter::finish\("),
    ("codec", GORILLA_TIMESTAMPS, "encode_timestamp_block", r"encode_timestamp_block\("),
    ("codec", GORILLA_XOR, "encode_xor_block", r"encode_xor_block\("),
    ("codec", LANE_PACK, "unpack_vector", r"codec::unpack_vector\("),
    ("codec", LANE_PACK, "unpack_linear", r"codec::unpack_linear\("),
    ("codec", LANE_PACK, "pack_vector", r"codec::pack_vector\("),
    ("codec", LANE_PACK, "pack_linear", r"codec::pack_linear\("),
    ("codec", PATCH_LIST, "read_patches", r"codec::read_patches\("),
    ("codec", PATCH_LIST, "append_patches", r"codec::append_patches\("),
    ("codec", PFOR, "decode_pfor_block", r"decode_pfor_block\("),
    ("codec", PFOR, "decode_pfor", r"codec::decode_pfor\("),
    ("codec", PFOR, "encode_pfor_block", r"encode_pfor_block\("),
    ("codec", PFOR, "encode_pfor", r"codec::encode_pfor\("),
    ("codec", STREAM_VBYTE, "decode_svb_block", r"decode_svb_block\("),
    ("codec", STREAM_VBYTE, "decode_svb", r"codec::decode_svb\("),
    ("codec", STREAM_VBYTE, "encode_svb_block", r"encode_svb_block\("),
    ("codec", STREAM_VBYTE, "encode_svb", r"codec::encode_svb\("),
    ("codec", DICTIONARY, "decode_dict_block", r"decode_dict_block\("),
    ("codec", DICTIONARY, "decode_dict", r"codec::decode_dict\("),
    ("codec", DICTIONARY, "encode_dict_block", r"encode_dict_block\("),
    ("codec", DICTIONARY, "encode_dict", r"codec::encode_dict\("),
    ("codec", DICTIONARY, "read_huffman", r"read_huffman\("),
    ("codec", DICTIONARY, "huffman_lengths", r"huffman_lengths\("),
    ("codec", RUN_LENGTH, "decode_rle_block", r"decode_rle_block\("),
    ("codec", RUN_LENGTH, "decode_rle", r"codec::decode_rle\("),
    ("codec", RUN_LENGTH, "encode_rle_block", r"encode_rle_block\("),
    ("codec", RUN_LENGTH, "encode_rle", r"codec::encode_rle\("),
    ("validator", COLUMNS, "require_size", r"require_size\("),
    ("validator", COLUMNS, "check_text_ends", r"check_text_ends\("),
    ("validator", COLUMNS, "check_raw_row_empty", r"check_raw_row_empty\("),
    ("validator", RESOLUTION_GUARD, "ResolutionGuard::publish", r"ResolutionGuard::publish\("),
    ("validator", STREAM_CHECK, "verify_round_trip", r"verify_round_trip\("),
    ("validator", MESSAGE_ORACLES, "check_message", r"oracle::check_message\("),
    ("validator", MESSAGE_ORACLES, "check_stream", r"oracle::check_stream\("),
    ("validator", STORE_ORACLES, "check_store", r"oracle::check_store\("),
    ("validator", COLUMNS_ORACLES, "check_columns", r"oracle::check_columns\("),
    ("validator", CONTAINER_ORACLES, "check_container", r"oracle::check_container\("),
    ("validator", CONTAINER_ORACLES, "check_parse", r"store::oracle::check_parse\("),
    ("validator", CONTAINER_ORACLES, "fixed_checksums", r"oracle::fixed_checksums\("),
    ("validator", ITCH_STORE_ORACLES, "check_itch_store", r"oracle::check_itch_store\("),
    ("validator", ITCH_STORE_ORACLES, "read_store", r"oracle::detail::read_store\("),
    ("validator", ITCH_STORE_ORACLES, "rewrite", r"oracle::detail::rewrite\("),
    ("validator", ITCH_STORE_ORACLES, "check_random_access", r"oracle::detail::check_random_access\("),
    ("validator", CODEC_ORACLES, "check_timestamp_column", r"oracle::check_timestamp_column\("),
    ("validator", CODEC_ORACLES, "check_xor_column", r"oracle::check_xor_column\("),
    ("validator", CODEC_ORACLES, "check_one_block", r"oracle::check_one_block\("),
    ("validator", CODEC_ORACLES, "check_as_values", r"oracle::check_as_values\("),
    ("validator", CODEC_ORACLES, "check_price_mappings", r"oracle::check_price_mappings\("),
    ("validator", CODEC_ORACLES, "check_pfor_column", r"oracle::check_pfor_column\("),
    ("validator", CODEC_ORACLES, "check_svb_column", r"oracle::check_svb_column\("),
    ("validator", CODEC_ORACLES, "check_svb_block", r"oracle::detail::check_svb_block\("),
    ("validator", CODEC_ORACLES, "pfor_block_encoders", r"oracle::detail::pfor_block_encoders\("),
    ("validator", CODEC_ORACLES, "check_dict_column", r"oracle::check_dict_column\("),
    ("validator", CODEC_ORACLES, "check_dict_block", r"oracle::detail::check_dict_block\("),
    ("validator", CODEC_ORACLES, "check_rle_column", r"oracle::check_rle_column\("),
    ("validator", CODEC_ORACLES, "check_rle_block", r"oracle::detail::check_rle_block\("),
    ("oracle", ROUNDTRIP, "verify", r"^tick_store::harness::verify\("),
    ("oracle", ROUNDTRIP, "verify_stub_store", r"^tick_store::harness::verify_stub_store\("),
    ("oracle", COLUMNS_READER, "verify_columns", r"^tick_store::harness::verify_columns\("),
    ("oracle", ITCH_STORE_RECORD_READER, "verify_itch_store",
     r"^tick_store::harness::verify_itch_store\("),
    ("oracle", ITCH_STORE_RECORD_READER, "ItchStoreRecordReader::next",
     r"ItchStoreRecordReader::next\("),
    ("oracle", ITCH_STORE_RECORD_READER, "ItchStoreRecordReader::tail",
     r"ItchStoreRecordReader::tail\("),
    ("oracle", ITCH_STORE_RECORD_READER, "RefusedItchStore::next", r"RefusedItchStore::next\("),
    ("oracle", ROUNDTRIP, "Walk::run", r"Walk::run\("),
    ("oracle", ROUNDTRIP, "Walk::step", r"Walk::step\("),
    ("oracle", ROUNDTRIP, "Walk::take", r"Walk::take\("),
    ("oracle", ROUNDTRIP, "Walk::stop_reading", r"Walk::stop_reading\("),
    ("oracle", ROUNDTRIP, "Walk::note_missing", r"Walk::note_missing\("),
    ("oracle", ROUNDTRIP, "Walk::note_difference", r"Walk::note_difference\("),
    ("oracle", ROUNDTRIP, "Walk::locate", r"Walk::locate\("),
    ("oracle", ROUNDTRIP, "Walk::drain_extra_records", r"Walk::drain_extra_records\("),
    ("oracle", ROUNDTRIP, "Walk::compare_tail", r"Walk::compare_tail\("),
    ("oracle", ROUNDTRIP, "RefusedStore::next", r"RefusedStore::next\("),
)


def message_types(root):
    text = (Path(root) / MESSAGE_HEADER).read_text()
    names = re.findall(r"struct (\w+) \{\n    static constexpr std::byte type_code", text)
    if len(names) != MESSAGE_TYPES:
        raise SystemExit(f"{MESSAGE_HEADER}: found {len(names)} message types, "
                         f"expected {MESSAGE_TYPES}")
    return names


# The hardware CRC32C kernel this processor builds, which the tests must reach: each
# processor builds only its own, so the other's is not in the binary to require.
HARDWARE_CRC32C = {
    "arm64": ("codec", "src/store/crc32c_armv8.cpp", "crc32c_armv8", r"detail::crc32c_armv8\("),
    "aarch64": ("codec", "src/store/crc32c_armv8.cpp", "crc32c_armv8",
                r"detail::crc32c_armv8\("),
    "x86_64": ("codec", "src/store/crc32c_sse42.cpp", "crc32c_sse42", r"detail::crc32c_sse42\("),
    "AMD64": ("codec", "src/store/crc32c_sse42.cpp", "crc32c_sse42", r"detail::crc32c_sse42\("),
}


def required_functions(types, machine=None):
    required = []
    for name in types:
        required.append(("decoder", CODEC, f"decode_as<{name}>",
                         rf"decode_as<tick_store::itch::{name}>\("))
        required.append(("encoder", CODEC, f"encode_as<{name}>",
                         rf"encode_as<tick_store::itch::{name}>\("))
    required.extend(FIXED_REQUIRED)
    machine = platform.machine() if machine is None else machine
    if machine in HARDWARE_CRC32C:
        required.append(HARDWARE_CRC32C[machine])
    return required


def demangle(names):
    """Local functions carry their file as a prefix. Everything else is a mangled name."""
    bare = [name.rpartition(":")[2] for name in names]
    done = subprocess.run(["llvm-cxxfilt"], input="\n".join(bare), capture_output=True, text=True,
                          check=True)
    return done.stdout.splitlines()


def short_name(demangled):
    """A name without its signature, or None for a lambda, which has no name worth keeping."""
    if demangled.startswith("_Z") or "$_" in demangled:
        return None
    plain = demangled.replace("(anonymous namespace)::", "")
    return plain.partition("(")[0].removeprefix("tick_store::")


def relative_to_repo(path, root):
    prefix = f"{root}/"
    return path[len(prefix):] if path.startswith(prefix) else None


def functions_of(export, root):
    """Every function under the repo, as (repo-relative file, demangled name, calls)."""
    entries = export["data"][0]["functions"]
    demangled = demangle([entry["name"] for entry in entries])
    found = []
    for entry, name in zip(entries, demangled, strict=True):
        file = relative_to_repo(entry["filenames"][0], root)
        if file is not None:
            found.append((file, name, entry["count"]))
    return found


def evaluate(required, functions):
    """Each required function's calls. calls None means no function matched its pattern."""
    results = []
    for group, file, name, pattern in required:
        regex = re.compile(pattern)
        matches = [calls for at, demangled, calls in functions
                   if at == file and regex.search(demangled)]
        results.append({"group": group, "file": file, "name": name,
                        "calls": sum(matches) if matches else None})
    return results


def percent(covered, total):
    return round(100 * covered / total, 2) if total else 100.0


def file_rows(export, root):
    rows = []
    for entry in export["data"][0]["files"]:
        file = relative_to_repo(entry["filename"], root)
        if file is None or not file.startswith(REPORTED):
            continue
        lines = entry["summary"]["lines"]
        functions = entry["summary"]["functions"]
        rows.append({"file": file,
                     "lines_covered": lines["covered"], "lines_total": lines["count"],
                     "lines_percent": percent(lines["covered"], lines["count"]),
                     "functions_covered": functions["covered"],
                     "functions_total": functions["count"],
                     "functions_percent": percent(functions["covered"], functions["count"])})
    return sorted(rows, key=lambda row: row["file"])


def totals_of(rows):
    lines = sum(row["lines_covered"] for row in rows), sum(row["lines_total"] for row in rows)
    functions = (sum(row["functions_covered"] for row in rows),
                 sum(row["functions_total"] for row in rows))
    return {"lines_covered": lines[0], "lines_total": lines[1],
            "lines_percent": percent(*lines),
            "functions_covered": functions[0], "functions_total": functions[1],
            "functions_percent": percent(*functions)}


def tracked_sources(root):
    listing = subprocess.run(["git", "-C", str(root), "ls-files", "src/*.cpp"], check=True,
                             capture_output=True, text=True).stdout
    return listing.split()


def commit_of(root):
    return subprocess.run(["git", "-C", str(root), "rev-parse", "--short=12", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()


def unreached(results):
    return [item for item in results if not item["calls"]]


def describe(item):
    state = "no such function in the binary" if item["calls"] is None else "0 calls"
    return f"REQUIRED FUNCTION NOT REACHED: {item['name']} in {item['file']}: {state}"


def never_called(functions):
    """What no test calls, by name, so the gap stays visible beside the percentages."""
    named = {(file, short_name(name)) for file, name, calls in functions
             if calls == 0 and file.startswith(REPORTED) and short_name(name)}
    return [{"file": file, "function": name} for file, name in sorted(named)]


def report_of(export, root):
    rows = file_rows(export, root)
    required = required_functions(message_types(root))
    functions = functions_of(export, root)
    results = evaluate(required, functions)
    measured = {row["file"] for row in rows}
    return {
        "schema": SCHEMA,
        "commit": commit_of(root),
        "platform": f"{platform.system()} {platform.machine()}",
        "run": RUN_STATEMENT,
        "simd_kernels": SIMD_STATEMENT,
        "python_validators": PYTHON_STATEMENT,
        "totals": totals_of(rows),
        "required": {"total": len(results), "reached": len(results) - len(unreached(results)),
                     "functions": results},
        "named_functions_never_called": never_called(functions),
        "sources_not_built_on_this_platform": [
            file for file in tracked_sources(root) if file not in measured],
        "files": rows,
    }


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    export = json.loads(Path(argv[0]).read_text())
    report = report_of(export, REPO_ROOT)
    output = Path(argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")

    totals = report["totals"]
    required = report["required"]
    print(f"coverage summary: lines {totals['lines_percent']}%, "
          f"functions {totals['functions_percent']}%, "
          f"required {required['reached']}/{required['total']} reached")
    failures = unreached(required["functions"])
    for item in failures:
        print(describe(item), file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
