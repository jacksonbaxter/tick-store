#pragma once

// One hand-built message per ITCH 5.0 type. Bytes and field tables are typed in
// from the specification and do not use src/itch/layout.hpp, so they check it.
//
// The bytes are chosen so that no two fields can be confused: the header is the
// same throughout, integers count up from a distinct high nibble, and Alpha
// values all differ within a message. A swapped field or a wrong offset changes
// a decoded value.

#include "test_bytes.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tick_store::itch::test {

struct FieldSpec {
    std::string_view name;
    std::size_t offset = 0;
    std::size_t width = 0;
};

struct Fixture {
    std::string_view label;
    char type = 0;
    std::size_t length = 0;
    Bytes bytes;
    // Every field from offset 0, in wire order, covering every byte.
    std::vector<FieldSpec> fields;
};

// All 23, in the order of the specification's tables.
[[nodiscard]] const std::vector<Fixture>& fixtures();

// The fixture of one type byte.
[[nodiscard]] const Fixture& fixture(char type);

}  // namespace tick_store::itch::test
