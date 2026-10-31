#pragma once

// A small source stream for store and oracle tests: one frame of each of the 23
// types, then raw-path records, then optionally a tail. Bytes come from the
// hand-built fixtures, so nothing here depends on the encoder under test.

#include "itch_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/stub_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <stdexcept>
#include <vector>

namespace tick_store::test {

using itch::test::Bytes;

// Not a type code, so these take the raw path as an unknown type.
inline constexpr char kUnknownType = 'z';

// A type code with one byte too many, so it takes the raw path as a wrong length.
inline constexpr char kWrongLengthType = 'S';

// A frame whose prefix promises more than is left, which a cursor stops at.
inline Bytes truncated_tail() {
    return itch::test::bytes({0x00, 0x7F, 0xAB, 0xCD});
}

// 23 types, an unknown type, a wrong length, then a one-byte unknown record, whose
// prefix one flip can turn into a zero length.
inline Bytes source_records() {
    Bytes out;
    const auto add = [&](const Bytes& payload) {
        const Bytes frame = itch::test::framed(static_cast<std::uint16_t>(payload.size()), payload);
        out.insert(out.end(), frame.begin(), frame.end());
    };
    for (const auto& fixture : itch::test::fixtures()) {
        add(fixture.bytes);
    }
    add(itch::test::bytes({static_cast<unsigned char>(kUnknownType), 0xA1, 0xA2, 0xA3}));
    Bytes long_event = itch::test::fixture(kWrongLengthType).bytes;
    long_event.push_back(std::byte{0xEE});
    add(long_event);
    add(itch::test::bytes({static_cast<unsigned char>(kUnknownType)}));
    return out;
}

inline constexpr std::size_t kSourceRecordCount = 26;

inline Bytes source_with_tail() {
    Bytes out = source_records();
    const Bytes tail = truncated_tail();
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

struct SourceFrames {
    std::vector<itch::Frame> frames;
    std::span<const std::byte> tail;
};

// The frames of `source`, by the real cursor. They borrow from `source`.
inline SourceFrames frames_of(std::span<const std::byte> source) {
    SourceFrames out;
    itch::FrameCursor cursor{source};
    for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
        out.frames.push_back(*frame);
    }
    out.tail = cursor.remaining();
    return out;
}

// The decoded records of `source`. RawMessages borrow from `source`.
inline std::vector<itch::Message> messages_of(std::span<const std::byte> source) {
    std::vector<itch::Message> out;
    for (const itch::Frame& frame : frames_of(source).frames) {
        out.push_back(itch::decode(frame.payload).value());
    }
    return out;
}

inline Bytes read_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    const std::vector<char> chars{std::istreambuf_iterator<char>{file},
                                  std::istreambuf_iterator<char>{}};
    const auto bytes = std::as_bytes(std::span{chars});
    return {bytes.begin(), bytes.end()};
}

inline void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    const auto chars = std::span{reinterpret_cast<const char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): writing bytes to a stream
                                 bytes.size()};
    file.write(chars.data(), static_cast<std::streamsize>(chars.size()));
    if (!file) {
        throw std::runtime_error("cannot write a test file");
    }
}

// Writes `messages` and `tail` through the real writer, and returns the file.
inline Bytes store_bytes(const std::vector<itch::Message>& messages,
                         std::span<const std::byte> tail, const std::filesystem::path& path) {
    auto writer = store::StubWriter::create(path);
    if (!writer.has_value()) {
        throw std::runtime_error(to_string(writer.error()));
    }
    for (const itch::Message& message : messages) {
        if (const auto appended = writer->append(message); !appended.has_value()) {
            throw std::runtime_error(to_string(appended.error()));
        }
    }
    if (const auto finished = writer->finish(tail); !finished.has_value()) {
        throw std::runtime_error(to_string(finished.error()));
    }
    return read_file(path);
}

}  // namespace tick_store::test
