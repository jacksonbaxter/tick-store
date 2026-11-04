#pragma once

// Plugs the ITCH store into the oracle, which is unchanged: each record comes out of the
// store's segment decoder as a Message and goes through the real ITCH encoder, segment by
// segment, so what the oracle compares is the output of the store's whole read path.

#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/roundtrip.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace tick_store::harness {

class ItchStoreRecordReader final : public RecordReader {
public:
    // `store` must outlive this reader.
    explicit ItchStoreRecordReader(const store::ItchStoreReader& store) noexcept
        : store_(store) {}

    [[nodiscard]] std::expected<Record, RecordReadError> next() override;
    [[nodiscard]] std::expected<std::span<const std::byte>, RecordReadError> tail() override;

private:
    const store::ItchStoreReader& store_;
    store::SegmentReader segment_;
    std::uint64_t next_segment_ = 0;
    bool loaded_ = false;
    std::array<std::byte, itch::kFramePrefixSize + itch::kMaxPayloadSize> frame_{};
};

// The oracle over a store file held in memory or mapped. A file the reader refuses at open
// is a failed report with store_error set, and its source hash is still computed.
[[nodiscard]] VerifyReport verify_itch_store(std::span<const std::byte> source,
                                             std::span<const std::byte> store_file,
                                             const VerifyOptions& options = {},
                                             store::ReadOptions read = {});

}  // namespace tick_store::harness
