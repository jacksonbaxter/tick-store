#pragma once

// The check fuzz_itch_store runs, kept apart from libFuzzer so the unit suite replays
// crashers through the same code. It returns nullptr if the input passed, or a
// description of the broken property.
//
// The input is a whole store file. It reaches the store's own checks below the CRCs two
// ways (Z1): read with Checksums::skip, and read with CRCs verified after every CRC its
// container structure points at is recomputed (fixed_checksums from
// container_oracles.hpp). Both must give the same verdict, and a refusal must repeat.
//
// The store's encoding leaves the writer choices (zstd or not, each PFOR block's
// options), so an accepted file is not compared with itself byte for byte. The oracle is
// decode, re-encode, compare (Z2): every record and the tail an accepted file gives are
// written again through the real writer at the file's segment size; that file must read
// back to the same records and tail, and writing those again must give the same bytes,
// so the writer's output is a fixed point. Every segment is also read on its own, last to
// first, and must give the records the sequential read gave. The file read with the
// scalar kernel set forced on this thread must give the same verdict and records.
//
// The segment size is capped at kFuzzMaxSegmentRecords: a file declaring more is opened
// and its directory checked, but its segments are not decoded. A segment's raw bytes can
// reach 65,535 a record, held as one 64-bit value a byte, so the cap keeps one input
// within libFuzzer's allocation limit.

#include "container_oracles.hpp"
#include "tick_store/codec/simd.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"
#include "tick_store/store/itch_store_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::store::oracle {

inline constexpr std::uint32_t kFuzzMaxSegmentRecords = 64;

namespace detail {

// A store's content: each record's payload, the tail, and the segment size.
struct StoreContent {
    std::vector<Bytes> records;
    Bytes tail;
    std::uint32_t segment_records = 0;

    friend bool operator==(const StoreContent&, const StoreContent&) = default;
};

// What reading a file gave: its content, a refusal, or nothing decoded because the
// segment size is over the cap.
struct ReadResult {
    std::optional<StoreContent> content;
    std::optional<ItchStoreError> error;
    bool over_cap = false;
};

[[nodiscard]] inline std::optional<Bytes> payload_of(const itch::Message& message) {
    std::array<std::byte, itch::kMaxPayloadSize> scratch{};
    const auto written = itch::encode(message, scratch);
    if (!written.has_value()) {
        return std::nullopt;
    }
    return Bytes(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(*written));
}

// The records of one segment, appended to `out`. Returns false with `error` set on a
// refusal; a message the encoder refuses is reported through `failure`.
[[nodiscard]] inline bool read_segment(const ItchStoreReader& reader, std::uint64_t segment,
                                       SegmentReader& records, std::vector<Bytes>& out,
                                       std::optional<ItchStoreError>& error,
                                       const char*& failure) {
    if (auto sought = records.seek(reader, segment); !sought.has_value()) {
        error = sought.error();
        return false;
    }
    for (;;) {
        auto message = records.next();
        if (!message.has_value()) {
            error = message.error();
            return false;
        }
        if (!message->has_value()) {
            return true;
        }
        auto payload = payload_of(**message);
        if (!payload.has_value()) {
            failure = "the reader gave a message the encoder refuses";
            return false;
        }
        out.push_back(std::move(*payload));
    }
}

[[nodiscard]] inline ReadResult read_store(std::span<const std::byte> file, Checksums checksums,
                                           const char*& failure) {
    ReadResult result;
    auto reader = ItchStoreReader::open_bytes(file, {.checksums = checksums});
    if (!reader.has_value()) {
        result.error = reader.error();
        return result;
    }
    if (reader->header().segment_records > kFuzzMaxSegmentRecords) {
        result.over_cap = true;
        return result;
    }
    StoreContent content;
    content.segment_records = reader->header().segment_records;
    SegmentReader records;
    for (std::uint64_t s = 0; s < reader->segment_count(); ++s) {
        if (!read_segment(*reader, s, records, content.records, result.error, failure)) {
            return result;
        }
    }
    auto tail = reader->tail();
    if (!tail.has_value()) {
        result.error = tail.error();
        return result;
    }
    content.tail.assign(tail->begin(), tail->end());
    if (content.records.size() != reader->header().record_count) {
        failure = "an accepted file gave a record count its header does not hold";
        return result;
    }
    result.content = std::move(content);
    return result;
}

// The content written again through the real writer, or nullopt if it refused.
[[nodiscard]] inline std::optional<Bytes> rewrite(const StoreContent& content) {
    auto writer = ItchStoreWriter::in_memory({.segment_records = content.segment_records});
    if (!writer.has_value()) {
        return std::nullopt;
    }
    for (const Bytes& payload : content.records) {
        const auto message = itch::decode(payload);
        if (!message.has_value() || !writer->append(*message).has_value()) {
            return std::nullopt;
        }
    }
    if (!writer->finish(content.tail).has_value()) {
        return std::nullopt;
    }
    return writer->take_bytes();
}

// Every segment read on its own, last to first, against the sequential read.
[[nodiscard]] inline const char* check_random_access(std::span<const std::byte> file,
                                                     const StoreContent& content) {
    auto reader = ItchStoreReader::open_bytes(file);
    if (!reader.has_value()) {
        return "a file accepted once was refused on a second open";
    }
    SegmentReader segment;
    for (std::uint64_t s = reader->segment_count(); s-- > 0;) {
        std::vector<Bytes> records;
        std::optional<ItchStoreError> error;
        const char* failure = nullptr;
        if (!read_segment(*reader, s, segment, records, error, failure)) {
            return failure != nullptr ? failure : "a segment read on its own was refused";
        }
        const std::uint64_t first = reader->segment(s).summary.first_record;
        if (first + records.size() > content.records.size()) {
            return "a segment read on its own gave records past the file's";
        }
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (records[i] != content.records[first + i]) {
                return "a segment read on its own differs from the sequential read";
            }
        }
    }
    return nullptr;
}

}  // namespace detail

[[nodiscard]] inline const char* check_itch_store(std::span<const std::byte> file) {
    const char* failure = nullptr;
    const detail::ReadResult skipped = detail::read_store(file, Checksums::skip, failure);
    if (failure != nullptr) {
        return failure;
    }
    {
        // The kernel-against-scalar oracle: the same verdict and records with the scalar
        // set forced.
        const codec::simd::ScalarKernelsOnThisThread scalar;
        const detail::ReadResult by_scalar = detail::read_store(file, Checksums::skip, failure);
        if (failure != nullptr) {
            return failure;
        }
        if (by_scalar.content != skipped.content || by_scalar.error != skipped.error ||
            by_scalar.over_cap != skipped.over_cap) {
            return "the kernel set and the scalar set read the file differently";
        }
    }
    const detail::Bytes fixed = fixed_checksums(file);
    const detail::ReadResult repaired = detail::read_store(fixed, Checksums::verify, failure);
    if (failure != nullptr) {
        return failure;
    }
    if (skipped.content.has_value() != repaired.content.has_value() ||
        skipped.over_cap != repaired.over_cap || skipped.error != repaired.error) {
        return "skipping CRCs and recomputing them gave different verdicts";
    }
    if (skipped.error.has_value()) {
        const detail::ReadResult again = detail::read_store(file, Checksums::skip, failure);
        return again.error != skipped.error ? "a refusal was not repeatable" : nullptr;
    }
    if (skipped.over_cap) {
        return nullptr;
    }
    if (!skipped.content.has_value() || !repaired.content.has_value()) {
        return "a file neither refused nor over the cap gave no records";
    }
    const detail::StoreContent& content = *skipped.content;
    if (*repaired.content != content) {
        return "the file read differently with CRCs skipped and recomputed";
    }
    if (const char* random = detail::check_random_access(fixed, content)) {
        return random;
    }

    const auto written = detail::rewrite(content);
    if (!written.has_value()) {
        return "the writer refused what the reader gave";
    }
    const detail::ReadResult reread = detail::read_store(*written, Checksums::verify, failure);
    if (failure != nullptr) {
        return failure;
    }
    if (!reread.content.has_value()) {
        return "the reader refused what the writer wrote";
    }
    if (*reread.content != content) {
        return "the rewritten file reads back differently";
    }
    const auto again = detail::rewrite(*reread.content);
    if (!again.has_value() || *again != *written) {
        return "the writer's output is not a fixed point";
    }
    return nullptr;
}

}  // namespace tick_store::store::oracle
