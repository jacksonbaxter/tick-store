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
//
// That cap leaves the work unbounded: 759 bytes, zstd standing in for the raw bytes, hold
// a segment of 64 records of 65,535 raw bytes, 4,194,240 values, and every value is
// decoded five times and encoded twice here. So the fuzz target also passes kStoreFuzzBounds, which caps the values a
// file's segments state in all, from the directory alone. A file stating more, up to a
// compare limit, is only read whole twice, by the chosen kernel set and by the scalar set,
// and the two reads compared; one stating more than that is opened and nothing more. Called
// without bounds, as the unit suite calls it, the check decodes every file within the
// segment cap (fuzz/README.md, "Bounds").

#include "container_oracles.hpp"
#include "tick_store/codec/block.hpp"
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
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::store::oracle {

inline constexpr std::uint32_t kFuzzMaxSegmentRecords = 64;

// How much of one file the check decodes. The default decodes every file within the
// segment cap.
struct StoreBounds {
    // The values a file's segments may state in all, over every column and piece
    // (SegmentEntry::values). A column is decoded only when its stated count is the one its
    // records need, and to exactly that many values, so this bounds every decode and
    // re-encode the check makes of the file.
    std::uint64_t stated_values = std::numeric_limits<std::uint64_t>::max();
    // The compare limit: a file past the cap that states at most this many is still read
    // whole by the chosen kernel set and with the scalar set forced, and the two reads must
    // give the same verdict and records. Nothing else is checked on it.
    std::uint64_t compared_values = std::numeric_limits<std::uint64_t>::max();
};

// The fuzz target's cap: seven eighths of one largest codec block's values, 917,504. The
// dearest file built within it, 3,840 add orders beside a segment of 64 raw records of 13,785
// bytes (bound_seeds.hpp), takes 466 ms an execution through fuzz_itch_store on the Mac; at
// one largest block's values, 2^20, the same file with raw records of 15,831 bytes takes 516,
// and at 3 x 2^18 one of 11,737 bytes 424. Typed records cost the most a value, but -max_len
// holds them to about 36,000 values: the densest built, 70,000 bytes of add orders in
// segments of 64, state 35,624, and files of small segments cost 65 to 83 ms whole.
inline constexpr std::uint64_t kFuzzMaxStoreValues = std::uint64_t{7} * (codec::kMaxBlockValues / 8);
// The compare limit: four largest blocks' values, 4,194,304. The two whole reads cost about
// 84 ns a stated value in the one-off tool on the Mac: 351 ms for 16 segments of 64 raw
// records of 4,096 bytes (4,197,424 stated), 1,391 ms for 16,780,336 stated, 5,564 ms for
// 67,110,960, which a file of 7,884 bytes states. The dearest file within it, the cap's add
// orders beside raw bytes to the limit, takes 462 ms through fuzz_itch_store.
inline constexpr std::uint64_t kFuzzComparedStoreValues = std::uint64_t{4} * codec::kMaxBlockValues;
inline constexpr StoreBounds kStoreFuzzBounds{.stated_values = kFuzzMaxStoreValues,
                                              .compared_values = kFuzzComparedStoreValues};

namespace detail {

// A store's content: each record's payload, the tail, and the segment size.
struct StoreContent {
    std::vector<Bytes> records;
    Bytes tail;
    std::uint32_t segment_records = 0;

    friend bool operator==(const StoreContent&, const StoreContent&) = default;
};

// What reading a file gave: its content, a refusal, or nothing decoded because the
// segment size or the values stated are over their cap.
struct ReadResult {
    std::optional<StoreContent> content;
    std::optional<ItchStoreError> error;
    bool over_cap = false;
    // The values the file's segments state and its segment size, if it opened.
    std::uint64_t stated = 0;
    std::uint32_t segment_records = 0;
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

// The values a file's segments state in all, from the directory, saturating.
[[nodiscard]] inline std::uint64_t stated_values(const ItchStoreReader& reader) {
    std::uint64_t total = 0;
    for (std::uint64_t s = 0; s < reader.segment_count(); ++s) {
        for (const std::uint32_t values : reader.segment(s).values) {
            total = values > std::numeric_limits<std::uint64_t>::max() - total
                        ? std::numeric_limits<std::uint64_t>::max()
                        : total + values;
        }
    }
    return total;
}

[[nodiscard]] inline ReadResult read_store(std::span<const std::byte> file, Checksums checksums,
                                           const StoreBounds& bounds, const char*& failure) {
    ReadResult result;
    auto reader = ItchStoreReader::open_bytes(file, {.checksums = checksums});
    if (!reader.has_value()) {
        result.error = reader.error();
        return result;
    }
    result.stated = stated_values(*reader);
    result.segment_records = reader->header().segment_records;
    if (reader->header().segment_records > kFuzzMaxSegmentRecords ||
        result.stated > bounds.stated_values) {
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

[[nodiscard]] inline const char* check_itch_store(std::span<const std::byte> file,
                                                 const StoreBounds& bounds) {
    const char* failure = nullptr;
    const detail::ReadResult skipped = detail::read_store(file, Checksums::skip, bounds, failure);
    if (failure != nullptr) {
        return failure;
    }
    {
        // The kernel-against-scalar oracle: the same verdict and records with the scalar
        // set forced.
        const codec::simd::ScalarKernelsOnThisThread scalar;
        const detail::ReadResult by_scalar =
            detail::read_store(file, Checksums::skip, bounds, failure);
        if (failure != nullptr) {
            return failure;
        }
        if (by_scalar.content != skipped.content || by_scalar.error != skipped.error ||
            by_scalar.over_cap != skipped.over_cap) {
            return "the kernel set and the scalar set read the file differently";
        }
    }
    const detail::Bytes fixed = fixed_checksums(file);
    const detail::ReadResult repaired = detail::read_store(fixed, Checksums::verify, bounds, failure);
    if (failure != nullptr) {
        return failure;
    }
    if (skipped.content.has_value() != repaired.content.has_value() ||
        skipped.over_cap != repaired.over_cap || skipped.error != repaired.error) {
        return "skipping CRCs and recomputing them gave different verdicts";
    }
    if (skipped.error.has_value()) {
        const detail::ReadResult again = detail::read_store(file, Checksums::skip, bounds, failure);
        return again.error != skipped.error ? "a refusal was not repeatable" : nullptr;
    }
    if (skipped.over_cap) {
        // Past the cap, up to the compare limit: read whole, as within it, by the chosen
        // kernel set and with the scalar set forced, and the two reads compared. A file over
        // the segment cap is not read: one segment may then hold more than
        // -malloc_limit_mb allows.
        if (skipped.segment_records > kFuzzMaxSegmentRecords ||
            skipped.stated > bounds.compared_values) {
            return nullptr;
        }
        const detail::ReadResult kernels = detail::read_store(file, Checksums::skip, StoreBounds{}, failure);
        if (failure != nullptr) {
            return failure;
        }
        const codec::simd::ScalarKernelsOnThisThread scalar;
        const detail::ReadResult by_scalar = detail::read_store(file, Checksums::skip, StoreBounds{}, failure);
        if (failure != nullptr) {
            return failure;
        }
        if (by_scalar.content != kernels.content || by_scalar.error != kernels.error) {
            return "past the cap, the kernel set and the scalar set read the file differently";
        }
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
    // The writer's file holds what the reader gave, so it states the same values.
    const detail::ReadResult reread =
        detail::read_store(*written, Checksums::verify, StoreBounds{}, failure);
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

// The whole check, as the unit suite runs it.
[[nodiscard]] inline const char* check_itch_store(std::span<const std::byte> file) {
    return check_itch_store(file, StoreBounds{});
}

}  // namespace tick_store::store::oracle
