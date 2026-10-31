#pragma once

// The correctness oracle for G-1 and G-2. It walks the source's frames and a store's
// records in lock step and compares bytes for each record, then the tails. Separately
// it hashes the whole source and the whole re-serialised stream, each on its own
// worker thread, and compares those with each other and with a pin if one is given.
//
// The comparison is positional. A dropped record therefore also shows as a difference
// at every later record: the first difference locates the fault, and the totals say
// how far it reached.

#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tick_store::harness {

enum class DifferenceKind : std::uint8_t {
    // Bytes differ inside a record. `bit`, `expected_byte` and `actual_byte` are set.
    bytes,
    // One record is a prefix of the other, so only the lengths differ.
    length,
    // The store ran out with this source record left.
    missing_record,
    // The source ran out, and the store has this record left over.
    extra_record,
    // Bytes differ inside the tail. `bit`, `expected_byte` and `actual_byte` are set.
    tail_bytes,
    // One tail is a prefix of the other.
    tail_length,
    // The store refused to give this record.
    store_error,
};

[[nodiscard]] std::string_view to_string(DifferenceKind kind) noexcept;

// The first thing that differed. Bit 0 is the least significant bit of the byte, the
// same numbering flip-bit takes.
struct Difference {
    DifferenceKind kind = DifferenceKind::bytes;
    // Index of the record, from 0. For the tail, the number of source records.
    std::uint64_t record_index = 0;
    // Where in the source the first differing byte is. For an extra record, the end
    // of the source's records.
    std::uint64_t source_offset = 0;
    std::optional<std::uint8_t> bit;
    std::optional<std::uint8_t> expected_byte;
    std::optional<std::uint8_t> actual_byte;
    // The source record's type byte, or the stored record's for an extra record.
    std::optional<std::uint8_t> message_type;
    // Whole sizes, prefix included: source, then stored. 0 where that side has none.
    std::uint64_t expected_size = 0;
    std::uint64_t actual_size = 0;
};

struct VerifyOptions {
    // The pin. If set, both streams must hash to it.
    std::optional<Sha256Digest> expected_sha256;
};

struct VerifyReport {
    std::uint64_t source_records = 0;
    std::uint64_t stored_records = 0;
    // Records and tail, on each side.
    std::uint64_t source_bytes = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t source_tail_bytes = 0;
    std::uint64_t stored_tail_bytes = 0;

    // Positional comparison. mismatched_records() is the sum of the first three.
    std::uint64_t differing_records = 0;
    std::uint64_t missing_records = 0;
    std::uint64_t extra_records = 0;
    // Stored records returned as a view of the source itself.
    std::uint64_t aliased_records = 0;
    // False too if a store error meant the tail was never reached.
    bool tail_matches = false;

    std::optional<Difference> first_difference;
    std::optional<RecordReadError> store_error;

    Sha256Digest source_sha256{};
    // Absent if a store error cut the stream short, because a digest of part of a
    // stream means nothing.
    std::optional<Sha256Digest> stored_sha256;
    std::optional<Sha256Digest> expected_sha256;

    double wall_seconds = 0.0;

    [[nodiscard]] std::uint64_t mismatched_records() const noexcept {
        return differing_records + missing_records + extra_records;
    }

    [[nodiscard]] bool hashes_match() const noexcept { return stored_sha256 == source_sha256; }

    [[nodiscard]] bool source_matches_pin() const noexcept {
        return !expected_sha256.has_value() || source_sha256 == *expected_sha256;
    }

    [[nodiscard]] bool stored_matches_pin() const noexcept {
        return !expected_sha256.has_value() || stored_sha256 == expected_sha256;
    }

    [[nodiscard]] bool passed() const noexcept {
        return !store_error.has_value() && mismatched_records() == 0 && aliased_records == 0 &&
               tail_matches && source_bytes == stored_bytes && hashes_match() &&
               source_matches_pin() && stored_matches_pin();
    }
};

// Runs the oracle. `source` is the whole source file, frames and tail.
[[nodiscard]] VerifyReport verify(std::span<const std::byte> source, RecordReader& reader,
                                  const VerifyOptions& options = {});

// Runs the oracle on a stub store held in memory. A file the parser refuses is a
// failed report with store_error set, and its source hash is still computed.
[[nodiscard]] VerifyReport verify_stub_store(std::span<const std::byte> source,
                                             std::span<const std::byte> store_file,
                                             const VerifyOptions& options = {});

}  // namespace tick_store::harness
