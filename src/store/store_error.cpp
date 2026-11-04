#include "tick_store/store/store_error.hpp"

#include <format>
#include <string>
#include <string_view>

namespace tick_store::store {

std::string to_string(const StoreError& error) {
    const auto located = [&](std::string_view what) {
        return std::format("{}: {} at offset {}: expected {}, have {}", what, error.field,
                           error.offset, error.expected, error.actual);
    };
    switch (error.kind) {
        case StoreErrorKind::file_too_small:
            return located("file too small");
        case StoreErrorKind::bad_magic:
            return located("bad magic");
        case StoreErrorKind::unsupported_version:
            return located("unsupported version");
        case StoreErrorKind::bad_header_size:
            return located("bad header size");
        case StoreErrorKind::bad_section_sizes:
            return located("bad section sizes");
        case StoreErrorKind::truncated_record:
            return located("truncated record");
        case StoreErrorKind::zero_length_record:
            return located("zero-length record");
        case StoreErrorKind::record_count_mismatch:
            return located("record count mismatch");
        case StoreErrorKind::unencodable_record:
            return located("unencodable record");
        case StoreErrorKind::bad_manifest:
            return located("bad manifest");
        case StoreErrorKind::bad_column_size:
            return located("bad column size");
        case StoreErrorKind::bad_text_offsets:
            return located("bad text offsets");
        case StoreErrorKind::bad_row:
            return located("bad row");
        case StoreErrorKind::io:
            return std::format("I/O error: {} failed with errno {}", error.field, error.actual);
        case StoreErrorKind::bad_footer:
            return located("bad footer");
        case StoreErrorKind::bad_extent:
            return located("bad extent");
        case StoreErrorKind::bad_checksum:
            return located("bad checksum");
        case StoreErrorKind::unknown_codec:
            return located("unknown codec");
        case StoreErrorKind::bad_field:
            return located("bad field");
        case StoreErrorKind::not_found:
            return located("not found");
        case StoreErrorKind::invalid_argument:
            return located("invalid argument");
    }
    return "invalid store error";
}

}  // namespace tick_store::store
