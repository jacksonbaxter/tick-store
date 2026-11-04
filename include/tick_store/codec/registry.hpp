#pragma once

// The one table of codec ids (F4). A block in a store file names its codec by an id
// from this table, and nothing else assigns one. The container format checks every id
// it reads against it, and the store decodes through the decoder it names, so the
// format and the codecs cannot disagree about what an id means.
//
// The rules for changing the table:
//
//   - An id is never reused. Once a file could have been written with it, it means
//     that codec for ever.
//   - active: writers may use it, readers decode it.
//   - read_only: readers still decode it, so files written with it stay readable
//     (F5), and writers refuse it. A codec leaves service this way.
//   - reserved: no file may hold it, and the reader refuses it as an unknown codec.
//     The entry stays in the table with its reason, so the id is never handed out
//     again. An id becomes reserved only when no supported format version can have
//     been written with it.
//   - A new codec takes the next unused id and changes no format version. A reader
//     older than the id refuses a block that names it, as an unknown codec, rather
//     than misread it (D11). A change to an existing codec's bytes is a new id, never
//     an edit to an old one.
//
// A codec's payload is one block in the codec's own frame (block.hpp), which carries
// its value count. Composite codecs, such as zstd over another codec's block, are new
// ids when the store needs them.

#include "tick_store/codec/block.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/stream_vbyte.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tick_store::codec {

// NOLINTNEXTLINE(performance-enum-size): the id is a 16-bit field in the file format.
enum class CodecId : std::uint16_t {
    none = 0,
    raw = 1,
    gorilla_timestamps = 2,
    gorilla_xor = 3,
    pfor = 4,
    stream_vbyte = 5,
    dictionary = 6,
    run_length = 7,
};

enum class CodecStatus : std::uint8_t {
    active,
    read_only,
    reserved,
};

struct CodecInfo {
    CodecId id = CodecId::none;
    std::string_view name;
    CodecStatus status = CodecStatus::reserved;
    // Decodes one block of this codec, or nullptr where the payload is not a codec
    // block: raw bytes, and every reserved id.
    BlockDecoder decoder = nullptr;
};

// Every id ever assigned, in id order, so an id is its index.
inline constexpr std::array kCodecs{
    // Never assigned, so a zeroed directory entry names no codec.
    CodecInfo{.id = CodecId::none, .name = "none", .status = CodecStatus::reserved},
    // Bytes the store keeps as they are, such as a source file's trailing bytes.
    CodecInfo{.id = CodecId::raw, .name = "raw", .status = CodecStatus::active},
    CodecInfo{.id = CodecId::gorilla_timestamps,
              .name = "gorilla_timestamps",
              .status = CodecStatus::active,
              .decoder = &decode_timestamp_block},
    CodecInfo{.id = CodecId::gorilla_xor,
              .name = "gorilla_xor",
              .status = CodecStatus::active,
              .decoder = &decode_xor_block},
    CodecInfo{.id = CodecId::pfor,
              .name = "pfor",
              .status = CodecStatus::active,
              .decoder = &decode_pfor_block},
    CodecInfo{.id = CodecId::stream_vbyte,
              .name = "stream_vbyte",
              .status = CodecStatus::active,
              .decoder = &decode_svb_block},
    CodecInfo{.id = CodecId::dictionary,
              .name = "dictionary",
              .status = CodecStatus::active,
              .decoder = &decode_dict_block},
    CodecInfo{.id = CodecId::run_length,
              .name = "run_length",
              .status = CodecStatus::active,
              .decoder = &decode_rle_block},
};

static_assert(
    [] {
        for (std::size_t i = 0; i < kCodecs.size(); ++i) {
            if (static_cast<std::size_t>(kCodecs[i].id) != i) {
                return false;
            }
        }
        return true;
    }(),
    "kCodecs must list every id in order, so that an id is its index");

// The entry for `id`, or nullptr if the id was never assigned.
[[nodiscard]] constexpr const CodecInfo* find_codec(std::uint16_t id) noexcept {
    return id < kCodecs.size() ? &kCodecs[id] : nullptr;
}

// True if a file may hold `id`: active or read_only.
[[nodiscard]] constexpr bool codec_readable(std::uint16_t id) noexcept {
    const CodecInfo* info = find_codec(id);
    return info != nullptr && info->status != CodecStatus::reserved;
}

// True if a writer may use `id`: active only.
[[nodiscard]] constexpr bool codec_writable(std::uint16_t id) noexcept {
    const CodecInfo* info = find_codec(id);
    return info != nullptr && info->status == CodecStatus::active;
}

}  // namespace tick_store::codec
