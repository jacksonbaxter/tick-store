#pragma once

// The typed error of the store's file layer: what went wrong and where.

#include <cstdint>
#include <string>
#include <string_view>

namespace tick_store::store {

enum class StoreErrorKind : std::uint8_t {
    // The file is shorter than a header.
    file_too_small,
    bad_magic,
    unsupported_version,
    bad_header_size,
    // Header, records and tail do not add up to the file size.
    bad_section_sizes,
    // A frame in the records section runs past the end of that section.
    truncated_record,
    // A frame declares no payload, which no stored record can.
    zero_length_record,
    // The records section does not hold the number of records the header states.
    record_count_mismatch,
    // The writer was given something it cannot store.
    unencodable_record,
    // A column directory's manifest is not in the exact form columns.hpp gives.
    bad_manifest,
    // A column file does not have the size the manifest gives it.
    bad_column_size,
    // A text column's offsets are not a valid walk of its bytes.
    bad_text_offsets,
    // A row of a column directory cannot be rebuilt into a record.
    bad_row,
    // A system call failed. `field` names it and `actual` is errno.
    io,
    // Container: the trailer's footer offset, or the footer's counts, do not fit the
    // file.
    bad_footer,
    // Container: a block, a section or a block's metadata does not sit exactly where
    // the ones before it end, or runs past the region that holds it.
    bad_extent,
    // Container: a stored CRC32C differs from the one computed over the bytes.
    bad_checksum,
    // Container: a block names a codec id that is not in the registry, or is reserved.
    unknown_codec,
    // Container: a field with a fixed set of values holds another, such as a reserved
    // field that is not zero or a section name with a character outside its alphabet.
    bad_field,
    // Container: no block at that index, or no section of that name. Not corruption.
    not_found,
    // A writer was given something the format cannot hold.
    invalid_argument,
};

// Offsets are from the start of the file, and `expected` and `actual` are the
// values in conflict:
//
//   file_too_small         expected = header size, actual = file size
//   bad_magic              expected, actual = the 8 magic bytes as a little-endian value
//   unsupported_version    expected, actual = version
//   bad_header_size        expected, actual = header size
//   bad_section_sizes      `field` is the first section that does not fit. expected =
//                          bytes it needs, actual = bytes left for it
//   truncated_record       offset = the frame; expected = bytes the frame needs,
//                          actual = bytes left in the section
//   zero_length_record     offset = the frame; expected = at least 1, actual = 0
//   record_count_mismatch  expected = count in the header, actual = frames present
//   unencodable_record     `field` is what the encoder objected to
//   bad_manifest           `field` is the line refused; offset = its byte offset
//   bad_column_size        `field` is the file; expected, actual = its size
//   bad_text_offsets       `field` is the file; offset = the row; expected = the offset
//                          the row needs at least, actual = the one it has
//   bad_row                `field` is the column; offset = the row; expected = what
//                          the row's form needs, actual = the value found
//   io                     actual = errno
//   bad_footer             `field` is the footer offset or the count refused;
//                          expected = the largest value that fits (for a footer offset
//                          below the header, the smallest), actual = the value
//   bad_extent             `field` names the offset or size refused; offset = where
//                          that field is in the file; expected = where the extent had
//                          to start (for a size, the most that fits), actual = the value
//   bad_checksum           `field` is what the CRC covers; offset = where those bytes
//                          start; expected = the stored CRC, actual = the computed one
//   unknown_codec          offset = the field; actual = the codec id
//   bad_field              offset = the field; expected = the value or bound it must
//                          meet, actual = the value found. For a section name
//                          character outside the alphabet, expected = 0; for a
//                          repeated name, expected and actual are the two entries'
//                          indexes
//   not_found              `field` is "block index" (expected = the block count,
//                          actual = the index) or "section" (the name is not kept)
//   invalid_argument       `field` is what the writer refused; expected, actual = the
//                          limit and the value, where there is one
//
// `field` points at static storage, so an error outlives the file.
struct StoreError {
    StoreErrorKind kind = StoreErrorKind::io;
    std::string_view field;
    std::uint64_t offset = 0;
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;

    friend bool operator==(const StoreError&, const StoreError&) = default;
};

[[nodiscard]] std::string to_string(const StoreError& error);

}  // namespace tick_store::store
