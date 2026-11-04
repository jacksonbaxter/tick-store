#pragma once

// The golden container: what tests/unit/golden/container-v1.tsc holds, as parts for the
// writer and as values for the reader, plus helpers to corrupt a copy of it one field
// at a time.
//
//   offset  size  what
//   0       16    header
//   16      21    block 0: gorilla_timestamps, 3 values, metadata 16 bytes
//   37      8     section "dict.stock"
//   45      2     block 1: raw, metadata 2 bytes
//   47      0     section "empty"
//   47      24    footer counts: 2 blocks, 2 sections, 18 metadata bytes
//   71      64    block entries
//   135     128   section entries
//   263     18    metadata area
//   281     24    trailer
//   305           end

#include "tick_store/codec/registry.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/crc32c.hpp"
#include "tick_store/store/little_endian.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace tick_store::store::test {

using Bytes = std::vector<std::byte>;

inline constexpr std::array<std::uint64_t, 3> kGoldenTimes{
    34'200'000'000'000ULL, 34'200'000'000'100ULL, 34'200'000'000'250ULL};

// Block 0's payload: the timestamp codec's block for kGoldenTimes, 3 values in 13
// payload bytes after the codec's 8-byte frame.
inline Bytes golden_timestamp_block() {
    constexpr std::array<unsigned, 21> kBytes{0x03, 0x00, 0x00, 0x00, 0x0D, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x1F, 0x1A, 0xCE, 0xD9,
                                              0xF0, 0x00, 0x80, 0x19, 0x20, 0x03, 0x20};
    Bytes out;
    for (const unsigned value : kBytes) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

inline Bytes ascii_bytes(std::string_view text) {
    Bytes out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

inline Bytes le_bytes(std::uint64_t value, std::size_t width) {
    Bytes out;
    for (std::size_t i = 0; i < width; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
    }
    return out;
}

// Block 0's metadata, a zone map as the store might keep one: first and last time.
inline Bytes golden_zone_map() {
    Bytes out = le_bytes(kGoldenTimes.front(), 8);
    const Bytes last = le_bytes(kGoldenTimes.back(), 8);
    out.insert(out.end(), last.begin(), last.end());
    return out;
}

inline std::vector<ContainerPart> golden_parts() {
    return {
        {.section_name = "",
         .codec = codec::CodecId::gorilla_timestamps,
         .metadata = golden_zone_map(),
         .bytes = golden_timestamp_block()},
        {.section_name = "dict.stock", .codec = {}, .metadata = {}, .bytes = ascii_bytes("ZVZZT   ")},
        {.section_name = "",
         .codec = codec::CodecId::raw,
         .metadata = {std::byte{0x01}, std::byte{0x02}},
         .bytes = {std::byte{0xAB}, std::byte{0xCD}}},
        {.section_name = "empty", .codec = {}, .metadata = {}, .bytes = {}},
    };
}

// Offsets in the golden file, from the table above.
inline constexpr std::size_t kGoldenSize = 305;
inline constexpr std::size_t kGoldenFooter = 47;
inline constexpr std::size_t kGoldenBlockEntries = 71;
inline constexpr std::size_t kGoldenSectionEntries = 135;
inline constexpr std::size_t kGoldenMetadata = 263;
inline constexpr std::size_t kGoldenTrailer = 281;

constexpr std::size_t golden_block_entry(std::size_t index) {
    return kGoldenBlockEntries + (index * kBlockEntrySize);
}

constexpr std::size_t golden_section_entry(std::size_t index) {
    return kGoldenSectionEntries + (index * kSectionEntrySize);
}

inline std::filesystem::path golden_path() {
    return std::filesystem::path{TICK_STORE_GOLDEN_DIR} / "container-v1.tsc";
}

inline Bytes read_bytes(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    Bytes out;
    for (auto it = std::istreambuf_iterator<char>{file}; it != std::istreambuf_iterator<char>{};
         ++it) {
        out.push_back(static_cast<std::byte>(*it));
    }
    out.shrink_to_fit();
    return out;
}

inline void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    for (const std::byte value : bytes) {
        file.put(static_cast<char>(value));
    }
}

// Overwrites a field with a little-endian value.
inline void put(Bytes& file, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) {
        file.at(offset + i) = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

// Recomputes the footer CRC from wherever the trailer says the footer starts, so a
// corrupted footer field reaches the structural check behind the CRC. The file must
// hold a trailer whose footer offset lies inside it.
inline void fix_footer_crc(Bytes& file) {
    const std::size_t trailer = file.size() - kTrailerSize;
    const std::uint64_t footer = read_le<8>(file, trailer + kTrailerFooterOffset).value_or(0);
    const auto crc = crc32c(std::span<const std::byte>{file}.subspan(
        static_cast<std::size_t>(footer), trailer - static_cast<std::size_t>(footer)));
    put(file, trailer + kTrailerFooterCrc, 4, crc);
}

// A copy with a field overwritten and the footer CRC made to match.
inline Bytes with_footer_field(Bytes file, std::size_t offset, std::size_t width,
                               std::uint64_t value) {
    put(file, offset, width, value);
    fix_footer_crc(file);
    file.shrink_to_fit();
    return file;
}

}  // namespace tick_store::store::test
