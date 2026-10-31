#pragma once

// A column directory from the real encoder, held in memory, and a RecordReader over one,
// so tests can corrupt it byte by byte without touching a file.

#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/harness/record_reader.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tick_store::test {

constexpr std::size_t index_of(store::ColumnFile file) {
    return static_cast<std::size_t>(file);
}

struct ColumnDirectory {
    std::string manifest;
    store::ColumnBuffers files;

    [[nodiscard]] store::ColumnBytes bytes() const {
        store::ColumnBytes out{};
        for (std::size_t i = 0; i < files.size(); ++i) {
            out[i] = files[i];
        }
        return out;
    }

    [[nodiscard]] std::span<const std::byte> manifest_bytes() const {
        return std::as_bytes(std::span{manifest});
    }

    void write_to(const std::filesystem::path& directory) const {
        std::filesystem::create_directories(directory);
        write_file(directory / store::kColumnsManifestName, manifest_bytes());
        for (std::size_t i = 0; i < files.size(); ++i) {
            write_file(directory / store::kColumnFileNames[i], files[i]);
        }
    }
};

// Every frame of `source` through the encoder, and its tail.
inline ColumnDirectory export_columns(std::span<const std::byte> source) {
    store::ColumnsEncoder encoder;
    const auto [frames, tail] = frames_of(source);
    for (const itch::Frame& frame : frames) {
        if (const auto added = encoder.append(frame.payload); !added.has_value()) {
            throw std::runtime_error(to_string(added.error()));
        }
    }
    ColumnDirectory out;
    out.manifest = store::format_columns_manifest(encoder.manifest(tail.size()));
    out.files = std::move(encoder.buffers());
    auto& tail_file = out.files[index_of(store::ColumnFile::tail)];
    tail_file.assign(tail.begin(), tail.end());
    return out;
}

// A directory rebuilding one payload.
inline ColumnDirectory export_payload(std::span<const std::byte> payload) {
    store::ColumnsEncoder encoder;
    if (const auto added = encoder.append(payload); !added.has_value()) {
        throw std::runtime_error(to_string(added.error()));
    }
    ColumnDirectory out;
    out.manifest = store::format_columns_manifest(encoder.manifest(0));
    out.files = std::move(encoder.buffers());
    return out;
}

// The directory's records for the oracle, in memory.
class BufferReader final : public harness::RecordReader {
public:
    explicit BufferReader(store::ColumnsParser parser) : parser_(std::move(parser)) {}

    [[nodiscard]] std::expected<Record, harness::RecordReadError> next() override {
        const auto frame = parser_.next();
        if (!frame.has_value()) {
            return std::unexpected(harness::RecordReadError{.message = to_string(frame.error()),
                                                            .store_offset = frame.error().offset});
        }
        return *frame;
    }

    [[nodiscard]] std::expected<std::span<const std::byte>, harness::RecordReadError> tail()
        override {
        return parser_.tail();
    }

private:
    store::ColumnsParser parser_;
};

}  // namespace tick_store::test
