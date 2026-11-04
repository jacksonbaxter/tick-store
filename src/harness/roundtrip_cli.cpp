#include "tick_store/harness/roundtrip_cli.hpp"

#include "tick_store/codec/registry.hpp"
#include "tick_store/harness/columns_reader.hpp"
#include "tick_store/harness/flip_bit.hpp"
#include "tick_store/harness/itch_store_record_reader.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/itch/raw_counters.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/itch_store_writer.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"
#include "tick_store/store/stub_store.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace tick_store::harness {
namespace {

constexpr std::string_view kUsage =
    "usage: tick_roundtrip ingest <source> <store> --durability page_cache|device_flush\n"
    "                             [--segment-records N]\n"
    "       tick_roundtrip ingest-stub <source> <store>\n"
    "       tick_roundtrip verify <source> <store> [--expect-sha256 HEX] [--no-checksums]\n"
    "       tick_roundtrip verify-columns <source> <columns-dir> [--expect-sha256 HEX]\n"
    "       tick_roundtrip layout <store> [--blocks]\n"
    "       tick_roundtrip flip-bit <file> <byte_offset> <bit>\n";

// What the process has held at most, in bytes: getrusage's ru_maxrss, which on Linux is the
// kernel's VmHWM in KiB and on macOS bytes.
[[nodiscard]] std::uint64_t peak_resident_bytes() noexcept {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    const auto peak = static_cast<std::uint64_t>(usage.ru_maxrss);
#ifdef __APPLE__
    return peak;
#else
    return peak * 1024U;
#endif
}

int usage(CliOutput& output) {
    output.err += kUsage;
    return kExitUsageOrIo;
}

int io_failure(CliOutput& output, std::string_view what, const store::StoreError& error) {
    output.err += std::format("{}: {}\n", what, to_string(error));
    return kExitUsageOrIo;
}

template <class T>
[[nodiscard]] std::optional<T> parse_number(std::string_view text) noexcept {
    T value{};
    const char* const start = text.data();
    const char* const end = text.data() + text.size();
    const auto [stopped, status] = std::from_chars(start, end, value);
    if (status != std::errc{} || stopped != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> hex_or_none(const std::optional<Sha256Digest>& digest) {
    return digest.has_value() ? std::optional{to_hex(*digest)} : std::nullopt;
}

void write_difference(JsonWriter& json, const Difference& difference) {
    json.begin_object();
    json.field("kind", to_string(difference.kind));
    json.field("record_index", difference.record_index);
    json.field("source_byte_offset", difference.source_offset);
    json.field("bit", difference.bit);
    json.field("expected_byte", difference.expected_byte);
    json.field("actual_byte", difference.actual_byte);
    json.key("message_type");
    if (difference.message_type.has_value()) {
        json.value(std::string(1, static_cast<char>(*difference.message_type)));
    } else {
        json.null();
    }
    json.field("expected_size", difference.expected_size);
    json.field("actual_size", difference.actual_size);
    json.end_object();
}

std::string summary_json(std::string_view command, std::string_view format,
                         std::string_view checksums, const VerifyReport& report) {
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_roundtrip"});
    json.field("command", command);
    json.field("store_format", format);
    json.field("checksums", checksums);
    json.field("verdict", std::string_view{report.passed() ? "pass" : "mismatch"});
    json.field("records", report.source_records);
    json.field("stored_records", report.stored_records);
    json.field("bytes", report.source_bytes);
    json.field("stored_bytes", report.stored_bytes);
    json.field("tail_bytes", report.source_tail_bytes);
    json.field("stored_tail_bytes", report.stored_tail_bytes);
    json.field("tail_matches", report.tail_matches);
    json.field("mismatched_records", report.mismatched_records());
    json.field("differing_records", report.differing_records);
    json.field("missing_records", report.missing_records);
    json.field("extra_records", report.extra_records);
    json.field("aliased_records", report.aliased_records);
    json.field("source_sha256", to_hex(report.source_sha256));
    json.field("stored_sha256", hex_or_none(report.stored_sha256));
    json.field("expected_sha256", hex_or_none(report.expected_sha256));
    json.field("hashes_match", report.hashes_match());
    json.field("source_matches_expected", report.source_matches_pin());
    json.field("stored_matches_expected", report.stored_matches_pin());
    json.key("first_difference");
    if (report.first_difference.has_value()) {
        write_difference(json, *report.first_difference);
    } else {
        json.null();
    }
    json.key("store_error");
    if (report.store_error.has_value()) {
        json.value(std::string_view{report.store_error->message});
    } else {
        json.null();
    }
    json.field("wall_seconds", report.wall_seconds);
    json.field("peak_rss_bytes", peak_resident_bytes());
    json.end_object();
    return json.finish();
}

std::string describe_failure(const VerifyReport& report) {
    std::string text;
    if (report.first_difference.has_value()) {
        const Difference& d = *report.first_difference;
        text += std::format("first difference: {} at record {}, source byte {}", to_string(d.kind),
                            d.record_index, d.source_offset);
        if (d.bit.has_value()) {
            text += std::format(", bit {}, expected 0x{:02x}, actual 0x{:02x}", *d.bit,
                                d.expected_byte.value_or(0), d.actual_byte.value_or(0));
        }
        if (d.message_type.has_value()) {
            text += std::format(", type 0x{:02x}", *d.message_type);
        }
        text += '\n';
    }
    if (report.store_error.has_value()) {
        text += std::format("store error: {}, store byte {}\n", report.store_error->message,
                            report.store_error->store_offset);
    }
    text += std::format("{} records mismatched\n", report.mismatched_records());
    if (!report.hashes_match()) {
        text += "the source and the re-serialised stream do not hash the same\n";
    }
    if (!report.source_matches_pin()) {
        text += "the source does not match the expected SHA-256\n";
    }
    if (!report.stored_matches_pin()) {
        text += "the re-serialised stream does not match the expected SHA-256\n";
    }
    return text;
}

int report_verdict(std::string_view command, std::string_view format, std::string_view checksums,
                   const VerifyReport& report, CliOutput& output) {
    output.out += summary_json(command, format, checksums, report);
    if (report.passed()) {
        return kExitPass;
    }
    output.err += describe_failure(report);
    return kExitMismatch;
}

bool starts_with(std::span<const std::byte> bytes, std::span<const std::byte> magic) {
    return bytes.size() >= magic.size() && std::ranges::equal(bytes.first(magic.size()), magic);
}

int run_verify(std::string_view source_path, std::string_view store_path,
               const VerifyOptions& options, store::Checksums checksums, CliOutput& output) {
    const auto source = store::MappedFile::open(source_path);
    if (!source.has_value()) {
        return io_failure(output, "cannot read the source", source.error());
    }
    const auto store_file = store::MappedFile::open(store_path);
    if (!store_file.has_value()) {
        return io_failure(output, "cannot read the store", store_file.error());
    }
    // The stub store's magic picks the stub's reader; anything else is read as the ITCH
    // store, whose container refuses a file that is not one.
    if (starts_with(store_file->bytes(), store::kStubMagic)) {
        if (checksums == store::Checksums::skip) {
            output.err += "the stub store has no checksums to skip\n";
            return kExitUsageOrIo;
        }
        return report_verdict("verify", "stub", "none",
                              verify_stub_store(source->bytes(), store_file->bytes(), options),
                              output);
    }
    const bool skip = checksums == store::Checksums::skip;
    return report_verdict(
        "verify", "itch-store-v1", skip ? "skip" : "verify",
        verify_itch_store(source->bytes(), store_file->bytes(), options, {.checksums = checksums}),
        output);
}

int run_verify_columns(std::string_view source_path, std::string_view directory,
                       const VerifyOptions& options, CliOutput& output) {
    const auto source = store::MappedFile::open(source_path);
    if (!source.has_value()) {
        return io_failure(output, "cannot read the source", source.error());
    }
    return report_verdict("verify-columns", "columns", "none",
                          verify_columns(source->bytes(), directory, options), output);
}

struct IngestCounts {
    std::uint64_t records = 0;
    std::uint64_t tail_bytes = 0;
    itch::RawPathCounters raw;
};

// Walks every frame of the source through the lossless decoder into `append`, then hands
// the tail to `finish`. Returns an exit code, or nullopt on success.
template <class Append, class Finish>
std::optional<int> walk_source(std::span<const std::byte> source, IngestCounts& counts,
                               Append&& append, Finish&& finish, CliOutput& output) {
    itch::FrameCursor cursor{source};
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            output.err += std::format("record {}: {}\n", counts.records, to_string(message.error()));
            return kExitUsageOrIo;
        }
        counts.raw.observe(*message);
        if (auto failed = append(*message)) {
            return failed;
        }
        ++counts.records;
    }
    const auto tail = cursor.remaining();
    counts.tail_bytes = tail.size();
    return finish(tail);
}

void write_ingest_fields(JsonWriter& json, const IngestCounts& counts, std::uint64_t source_bytes,
                         std::uint64_t stored_bytes) {
    json.field("records", counts.records);
    json.field("bytes", source_bytes);
    json.field("tail_bytes", counts.tail_bytes);
    json.field("raw_unknown_type", counts.raw.count(itch::RawReason::unknown_type));
    json.field("raw_wrong_length", counts.raw.count(itch::RawReason::wrong_length));
    json.field("stored_file_bytes", stored_bytes);
}

int run_ingest_stub(std::string_view source_path, std::string_view store_path, CliOutput& output) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();

    const auto source = store::MappedFile::open(source_path);
    if (!source.has_value()) {
        return io_failure(output, "cannot read the source", source.error());
    }
    auto writer = store::StubWriter::create(store_path);
    if (!writer.has_value()) {
        return io_failure(output, "cannot create the store", writer.error());
    }
    IngestCounts counts;
    const auto failed = walk_source(
        source->bytes(), counts,
        [&](const itch::Message& message) -> std::optional<int> {
            if (const auto appended = writer->append(message); !appended.has_value()) {
                return io_failure(output, "cannot write the store", appended.error());
            }
            return std::nullopt;
        },
        [&](std::span<const std::byte> tail) -> std::optional<int> {
            if (const auto finished = writer->finish(tail); !finished.has_value()) {
                return io_failure(output, "cannot finish the store", finished.error());
            }
            return std::nullopt;
        },
        output);
    if (failed.has_value()) {
        return *failed;
    }
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_roundtrip"});
    json.field("command", std::string_view{"ingest-stub"});
    write_ingest_fields(json, counts, source->bytes().size(), std::filesystem::file_size(store_path));
    json.field("wall_seconds", std::chrono::duration<double>{Clock::now() - start}.count());
    json.field("peak_rss_bytes", peak_resident_bytes());
    json.end_object();
    output.out += json.finish();
    return kExitPass;
}

int store_failure(CliOutput& output, std::string_view what, const store::ItchStoreError& error) {
    output.err += std::format("{}: {}\n", what, to_string(error));
    return kExitUsageOrIo;
}

int run_ingest(std::string_view source_path, std::string_view store_path,
               store::Durability durability, store::ItchStoreOptions options, CliOutput& output) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();

    const auto source = store::MappedFile::open(source_path);
    if (!source.has_value()) {
        return io_failure(output, "cannot read the source", source.error());
    }
    auto writer = store::ItchStoreWriter::create(store_path, durability, options);
    if (!writer.has_value()) {
        return store_failure(output, "cannot create the store", writer.error());
    }
    IngestCounts counts;
    const auto failed = walk_source(
        source->bytes(), counts,
        [&](const itch::Message& message) -> std::optional<int> {
            if (const auto appended = writer->append(message); !appended.has_value()) {
                return store_failure(output, "cannot write the store", appended.error());
            }
            return std::nullopt;
        },
        [&](std::span<const std::byte> tail) -> std::optional<int> {
            if (const auto finished = writer->finish(tail); !finished.has_value()) {
                return store_failure(output, "cannot finish the store", finished.error());
            }
            return std::nullopt;
        },
        output);
    if (failed.has_value()) {
        return *failed;
    }
    const std::uint64_t stored = std::filesystem::file_size(store_path);
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_roundtrip"});
    json.field("command", std::string_view{"ingest"});
    json.field("store_format", std::string_view{"itch-store-v1"});
    json.field("durability", to_string(durability));
    json.field("segment_records", std::uint64_t{options.segment_records});
    write_ingest_fields(json, counts, source->bytes().size(), stored);
    json.field("ratio", stored == 0 ? 0.0
                                    : static_cast<double>(source->bytes().size()) /
                                          static_cast<double>(stored));
    json.field("wall_seconds", std::chrono::duration<double>{Clock::now() - start}.count());
    json.field("peak_rss_bytes", peak_resident_bytes());
    json.end_object();
    output.out += json.finish();
    return kExitPass;
}

int run_layout(std::string_view store_path, bool blocks, CliOutput& output) {
    auto reader = store::ItchStoreReader::open(store_path);
    if (!reader.has_value()) {
        output.err += std::format("cannot open the store: {}\n", to_string(reader.error()));
        return kExitMismatch;
    }
    const store::StoreSizes sizes = reader->sizes();
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_roundtrip"});
    json.field("command", std::string_view{"layout"});
    json.field("store_format", std::string_view{"itch-store-v1"});
    json.field("records", reader->header().record_count);
    json.field("segment_records", std::uint64_t{reader->header().segment_records});
    json.field("segments", reader->segment_count());
    json.field("symbols", std::uint64_t{reader->symbols().size()});
    json.field("blocks", reader->container().block_count());
    json.field("file_bytes", sizes.file_bytes);
    json.key("container");
    json.begin_object();
    json.field("header_bytes", sizes.header_bytes);
    json.field("trailer_bytes", sizes.trailer_bytes);
    json.field("footer_fixed_bytes", sizes.footer_fixed_bytes);
    json.field("block_entry_bytes", sizes.block_entry_bytes);
    json.field("section_entry_bytes", sizes.section_entry_bytes);
    json.field("block_metadata_bytes", sizes.metadata_bytes);
    json.field("footer_offset", reader->container().footer_offset());
    json.end_object();
    json.key("sections");
    json.begin_object();
    json.field("store", sizes.store_section_bytes);
    json.field("symbols", sizes.symbols_section_bytes);
    json.field("tail", sizes.tail_section_bytes);
    json.end_object();
    json.key("columns");
    json.begin_array();
    for (std::size_t c = 0; c < store::kColumnCount; ++c) {
        json.begin_object();
        json.field("column", store::kColumns[c].name);
        json.field("payload_bytes", sizes.column_payload_bytes[c]);
        json.field("metadata_bytes", sizes.column_metadata_bytes[c]);
        json.field("blocks", sizes.column_blocks[c]);
        json.field("zstd_blocks", sizes.column_zstd_blocks[c]);
        json.field("values", sizes.column_values[c]);
        json.end_object();
    }
    json.end_array();
    if (blocks) {
        json.key("block_list");
        json.begin_array();
        for (std::uint64_t s = 0; s < reader->segment_count(); ++s) {
            const store::SegmentEntry& entry = reader->segment(s);
            for (std::size_t c = 0; c < store::kColumnCount; ++c) {
                if (entry.block[c] == store::kNoIndex) {
                    continue;
                }
                const auto info = reader->container().block_info(entry.block[c]);
                if (!info.has_value()) {
                    continue;
                }
                json.begin_object();
                json.field("block", entry.block[c]);
                json.field("segment", s);
                json.field("column", store::kColumns[c].name);
                json.field("codec", codec::find_codec(static_cast<std::uint16_t>(info->codec))->name);
                json.field("payload_offset", info->payload_offset);
                json.field("payload_bytes", std::uint64_t{info->payload_size});
                json.end_object();
            }
        }
        json.end_array();
    }
    json.end_object();
    output.out += json.finish();
    return kExitPass;
}

int run_flip_bit(std::span<const std::string_view> arguments, CliOutput& output) {
    const auto offset = parse_number<std::uint64_t>(arguments[1]);
    const auto bit = parse_number<unsigned>(arguments[2]);
    if (!offset.has_value() || !bit.has_value() || *bit > 7U) {
        return usage(output);
    }
    const auto flipped = flip_bit(arguments[0], *offset, *bit);
    if (!flipped.has_value()) {
        output.err += std::format("cannot flip the bit: {}\n", flipped.error());
        return kExitUsageOrIo;
    }
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_roundtrip"});
    json.field("command", std::string_view{"flip-bit"});
    json.field("byte_offset", *offset);
    json.field("bit", *bit);
    json.field("before", flipped->before);
    json.field("after", flipped->after);
    json.end_object();
    output.out += json.finish();
    return kExitPass;
}

}  // namespace

int run_roundtrip_cli(std::span<const std::string_view> arguments, CliOutput& output) {
    if (arguments.empty()) {
        return usage(output);
    }
    const std::string_view command = arguments.front();
    const auto rest = arguments.subspan(1);

    if (command == "ingest" && rest.size() >= 2) {
        std::optional<store::Durability> durability;
        store::ItchStoreOptions options;
        for (std::size_t i = 2; i < rest.size(); i += 2) {
            if (i + 1 >= rest.size()) {
                return usage(output);
            }
            if (rest[i] == "--durability" && rest[i + 1] == "page_cache") {
                durability = store::Durability::page_cache;
            } else if (rest[i] == "--durability" && rest[i + 1] == "device_flush") {
                durability = store::Durability::device_flush;
            } else if (rest[i] == "--segment-records") {
                const auto value = parse_number<std::uint32_t>(rest[i + 1]);
                if (!value.has_value() || *value == 0 || *value > store::kMaxSegmentRecords) {
                    return usage(output);
                }
                options.segment_records = *value;
            } else {
                return usage(output);
            }
        }
        // The durability class is part of what a write means, so it is never a default.
        if (!durability.has_value()) {
            return usage(output);
        }
        return run_ingest(rest[0], rest[1], *durability, options, output);
    }
    if (command == "ingest-stub" && rest.size() == 2) {
        return run_ingest_stub(rest[0], rest[1], output);
    }
    if (command == "verify" && rest.size() >= 2) {
        VerifyOptions options;
        store::Checksums checksums = store::Checksums::verify;
        for (std::size_t i = 2; i < rest.size(); ++i) {
            if (rest[i] == "--no-checksums" && checksums == store::Checksums::verify) {
                checksums = store::Checksums::skip;
            } else if (rest[i] == "--expect-sha256" && i + 1 < rest.size() &&
                       !options.expected_sha256.has_value()) {
                options.expected_sha256 = parse_sha256_hex(rest[++i]);
                if (!options.expected_sha256.has_value()) {
                    return usage(output);
                }
            } else {
                return usage(output);
            }
        }
        return run_verify(rest[0], rest[1], options, checksums, output);
    }
    if (command == "verify-columns" && rest.size() == 2) {
        return run_verify_columns(rest[0], rest[1], {}, output);
    }
    if (command == "verify-columns" && rest.size() == 4 && rest[2] == "--expect-sha256") {
        const auto pin = parse_sha256_hex(rest[3]);
        if (!pin.has_value()) {
            return usage(output);
        }
        return run_verify_columns(rest[0], rest[1], {.expected_sha256 = pin}, output);
    }
    if (command == "layout" && rest.size() == 1) {
        return run_layout(rest[0], false, output);
    }
    if (command == "layout" && rest.size() == 2 && rest[1] == "--blocks") {
        return run_layout(rest[0], true, output);
    }
    if (command == "flip-bit" && rest.size() == 3) {
        return run_flip_bit(rest, output);
    }
    return usage(output);
}

}  // namespace tick_store::harness
