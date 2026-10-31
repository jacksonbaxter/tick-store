#pragma once

// Plugs a column directory into the oracle. A reader is built from the directory alone,
// so nothing of the source reaches it, and each record goes through the same decoder
// and encoder as the stub store's. Whatever a comparison system stored comes back here
// as column files, so this is how the oracle reaches a system it knows nothing about.

#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace tick_store::harness {

class ColumnsReader final : public RecordReader {
public:
    // Maps every file and proves their sizes. Any failure is a StoreError.
    [[nodiscard]] static std::expected<std::unique_ptr<ColumnsReader>, store::StoreError> open(
        const std::filesystem::path& directory);

    [[nodiscard]] std::expected<Record, RecordReadError> next() override;
    [[nodiscard]] std::expected<std::span<const std::byte>, RecordReadError> tail() override;

private:
    struct Files {
        std::vector<store::MappedFile> mapped;
        store::ColumnBytes bytes{};
    };

    ColumnsReader(Files files, const store::ColumnsParser& parser) noexcept;

    Files files_;
    store::ColumnsParser parser_;
};

// Runs the oracle on a column directory. One the parser refuses is a failed report with
// store_error set, and its source hash is still computed.
[[nodiscard]] VerifyReport verify_columns(std::span<const std::byte> source,
                                          const std::filesystem::path& directory,
                                          const VerifyOptions& options = {});

}  // namespace tick_store::harness
