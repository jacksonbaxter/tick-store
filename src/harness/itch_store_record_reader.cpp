#include "tick_store/harness/itch_store_record_reader.hpp"

#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace tick_store::harness {
namespace {

[[nodiscard]] RecordReadError read_error(const store::ItchStoreError& error) {
    return RecordReadError{.message = store::to_string(error),
                           .store_offset = error.offset == store::kNoIndex ? 0 : error.offset};
}

// The reader for a file refused at open: nothing to give but the error.
class RefusedItchStore final : public RecordReader {
public:
    explicit RefusedItchStore(RecordReadError error) noexcept : error_(std::move(error)) {}

    [[nodiscard]] std::expected<Record, RecordReadError> next() override {
        return std::unexpected(error_);
    }
    [[nodiscard]] std::expected<std::span<const std::byte>, RecordReadError> tail() override {
        return std::unexpected(error_);
    }

private:
    RecordReadError error_;
};

}  // namespace

std::expected<RecordReader::Record, RecordReadError> ItchStoreRecordReader::next() {
    for (;;) {
        if (!loaded_) {
            if (next_segment_ == store_.segment_count()) {
                return Record{std::nullopt};
            }
            if (auto sought = segment_.seek(store_, next_segment_); !sought.has_value()) {
                return std::unexpected(read_error(sought.error()));
            }
            ++next_segment_;
            loaded_ = true;
        }
        auto message = segment_.next();
        if (!message.has_value()) [[unlikely]] {
            return std::unexpected(read_error(message.error()));
        }
        if (!message->has_value()) {
            loaded_ = false;
            continue;
        }
        const auto written = itch::encode_frame(**message, frame_);
        if (!written.has_value()) [[unlikely]] {
            return std::unexpected(RecordReadError{.message = to_string(written.error())});
        }
        return Record{std::span<const std::byte>{frame_}.first(*written)};
    }
}

std::expected<std::span<const std::byte>, RecordReadError> ItchStoreRecordReader::tail() {
    auto tail = store_.tail();
    if (!tail.has_value()) {
        return std::unexpected(read_error(tail.error()));
    }
    return *tail;
}

VerifyReport verify_itch_store(std::span<const std::byte> source,
                               std::span<const std::byte> store_file,
                               const VerifyOptions& options, store::ReadOptions read) {
    auto store = store::ItchStoreReader::open_bytes(store_file, read);
    if (!store.has_value()) {
        RefusedItchStore refused{read_error(store.error())};
        return verify(source, refused, options);
    }
    ItchStoreRecordReader reader{*store};
    return verify(source, reader, options);
}

}  // namespace tick_store::harness
