#include "tick_store/harness/roundtrip_cli.hpp"

#include "tick_store/harness/columns_reader.hpp"
#include "tick_store/harness/flip_bit.hpp"
#include "tick_store/harness/json_writer.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/itch/raw_counters.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"
#include "tick_store/store/stub_store.hpp"

#include <charconv>
#include <chrono>
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
    "usage: tick_roundtrip ingest <source> <store>\n"
    "       tick_roundtrip verify <source> <store> [--expect-sha256 HEX]\n"
    "       tick_roundtrip verify-columns <source> <columns-dir> [--expect-sha256 HEX]\n"
    "       tick_roundtrip flip-bit <file> <byte_offset> <bit>\n";

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

std::string summary_json(std::string_view command, const VerifyReport& report) {
    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_roundtrip"});
    json.field("command", command);
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

int report_verdict(std::string_view command, const VerifyReport& report, CliOutput& output) {
    output.out += summary_json(command, report);
    if (report.passed()) {
        return kExitPass;
    }
    output.err += describe_failure(report);
    return kExitMismatch;
}

int run_verify(std::string_view source_path, std::string_view store_path,
               const VerifyOptions& options, CliOutput& output) {
    const auto source = store::MappedFile::open(source_path);
    if (!source.has_value()) {
        return io_failure(output, "cannot read the source", source.error());
    }
    const auto store_file = store::MappedFile::open(store_path);
    if (!store_file.has_value()) {
        return io_failure(output, "cannot read the store", store_file.error());
    }
    return report_verdict("verify", verify_stub_store(source->bytes(), store_file->bytes(), options),
                          output);
}

int run_verify_columns(std::string_view source_path, std::string_view directory,
                       const VerifyOptions& options, CliOutput& output) {
    const auto source = store::MappedFile::open(source_path);
    if (!source.has_value()) {
        return io_failure(output, "cannot read the source", source.error());
    }
    return report_verdict("verify-columns", verify_columns(source->bytes(), directory, options),
                          output);
}

int run_ingest(std::string_view source_path, std::string_view store_path, CliOutput& output) {
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

    itch::FrameCursor cursor{source->bytes()};
    itch::RawPathCounters raw;
    std::uint64_t records = 0;
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        const auto message = itch::decode(frame->payload);
        if (!message.has_value()) {
            output.err += std::format("record {}: {}\n", records, to_string(message.error()));
            return kExitUsageOrIo;
        }
        raw.observe(*message);
        if (const auto appended = writer->append(*message); !appended.has_value()) {
            return io_failure(output, "cannot write the store", appended.error());
        }
        ++records;
    }
    const auto tail = cursor.remaining();
    if (const auto finished = writer->finish(tail); !finished.has_value()) {
        return io_failure(output, "cannot finish the store", finished.error());
    }

    JsonWriter json;
    json.begin_object();
    json.field("tool", std::string_view{"tick_roundtrip"});
    json.field("command", std::string_view{"ingest"});
    json.field("records", records);
    json.field("bytes", source->bytes().size());
    json.field("tail_bytes", tail.size());
    json.field("raw_unknown_type", raw.count(itch::RawReason::unknown_type));
    json.field("raw_wrong_length", raw.count(itch::RawReason::wrong_length));
    json.field("stored_file_bytes", std::filesystem::file_size(store_path));
    json.field("wall_seconds", std::chrono::duration<double>{Clock::now() - start}.count());
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

    if (command == "ingest" && rest.size() == 2) {
        return run_ingest(rest[0], rest[1], output);
    }
    if (command == "verify" && rest.size() == 2) {
        return run_verify(rest[0], rest[1], {}, output);
    }
    if (command == "verify" && rest.size() == 4 && rest[2] == "--expect-sha256") {
        const auto pin = parse_sha256_hex(rest[3]);
        if (!pin.has_value()) {
            return usage(output);
        }
        return run_verify(rest[0], rest[1], {.expected_sha256 = pin}, output);
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
    if (command == "flip-bit" && rest.size() == 3) {
        return run_flip_bit(rest, output);
    }
    return usage(output);
}

}  // namespace tick_store::harness
