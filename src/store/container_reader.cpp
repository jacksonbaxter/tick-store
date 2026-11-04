#include "tick_store/store/container_reader.hpp"

#include "tick_store/store/container.hpp"
#include "tick_store/store/mapped_file.hpp"
#include "tick_store/store/store_error.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace tick_store::store {

std::expected<ContainerReader, StoreError> ContainerReader::open(
    const std::filesystem::path& path, ReadOptions options) {
    auto mapped = MappedFile::open(path, Access::normal);
    if (!mapped.has_value()) {
        return std::unexpected(mapped.error());
    }
    auto mapping = std::make_shared<const MappedFile>(std::move(*mapped));
    // The parser borrows the mapping's bytes, which do not move while the shared
    // MappedFile lives, and the reader holds it for as long as the parser does.
    auto parser = ContainerParser::open(mapping->bytes(), options);
    if (!parser.has_value()) {
        return std::unexpected(parser.error());
    }
    return ContainerReader{std::move(mapping), std::move(*parser)};
}

std::expected<BlockView, StoreError> ContainerReader::block(std::uint64_t index) const {
    auto view = parser_.block(index);
    if (view.has_value()) {
        view->owner = mapping_;
    }
    return view;
}

std::expected<SectionView, StoreError> ContainerReader::section(std::string_view name) const {
    auto view = parser_.section(name);
    if (view.has_value()) {
        view->owner = mapping_;
    }
    return view;
}

}  // namespace tick_store::store
