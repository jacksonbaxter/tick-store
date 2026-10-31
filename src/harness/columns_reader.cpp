#include "tick_store/harness/columns_reader.hpp"

#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace tick_store::harness {

ColumnsReader::ColumnsReader(Files files, const store::ColumnsParser& parser) noexcept
    : files_(std::move(files)), parser_(parser) {}

std::expected<std::unique_ptr<ColumnsReader>, store::StoreError> ColumnsReader::open(
    const std::filesystem::path& directory) {
    const auto manifest_file = store::MappedFile::open(directory / store::kColumnsManifestName);
    if (!manifest_file.has_value()) {
        return std::unexpected(manifest_file.error());
    }
    const auto manifest = store::parse_columns_manifest(manifest_file->bytes());
    if (!manifest.has_value()) {
        return std::unexpected(manifest.error());
    }

    Files files;
    files.mapped.reserve(store::kColumnFileCount);
    for (std::size_t i = 0; i < store::kColumnFileCount; ++i) {
        auto mapped = store::MappedFile::open(directory / store::kColumnFileNames[i]);
        if (!mapped.has_value()) {
            return std::unexpected(mapped.error());
        }
        files.bytes[i] = mapped->bytes();
        files.mapped.push_back(std::move(*mapped));
    }

    // Spans stay valid across the move of the MappedFiles, as mapped_file.hpp promises.
    const auto parser = store::ColumnsParser::open(*manifest, files.bytes);
    if (!parser.has_value()) {
        return std::unexpected(parser.error());
    }
    return std::unique_ptr<ColumnsReader>{new ColumnsReader{std::move(files), *parser}};
}

std::expected<RecordReader::Record, RecordReadError> ColumnsReader::next() {
    const auto frame = parser_.next();
    if (!frame.has_value()) [[unlikely]] {
        return std::unexpected(RecordReadError{.message = to_string(frame.error()),
                                               .store_offset = frame.error().offset});
    }
    return *frame;
}

std::expected<std::span<const std::byte>, RecordReadError> ColumnsReader::tail() {
    return parser_.tail();
}

namespace {

// Refuses every record, so the oracle reports an unopenable directory as it would any
// store that cannot produce its records.
class RefusedColumns final : public RecordReader {
public:
    explicit RefusedColumns(const store::StoreError& error) noexcept : error_(error) {}

    [[nodiscard]] std::expected<Record, RecordReadError> next() override {
        return std::unexpected(
            RecordReadError{.message = to_string(error_), .store_offset = error_.offset});
    }

    [[nodiscard]] std::expected<std::span<const std::byte>, RecordReadError> tail() override {
        return std::span<const std::byte>{};
    }

private:
    store::StoreError error_;
};

}  // namespace

VerifyReport verify_columns(std::span<const std::byte> source,
                            const std::filesystem::path& directory, const VerifyOptions& options) {
    auto reader = ColumnsReader::open(directory);
    if (!reader.has_value()) {
        RefusedColumns refused{reader.error()};
        return verify(source, refused, options);
    }
    return verify(source, **reader, options);
}

}  // namespace tick_store::harness
