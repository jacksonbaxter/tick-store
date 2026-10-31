#include "tick_store/harness/roundtrip.hpp"

#include "tick_store/harness/hash_worker.hpp"
#include "tick_store/harness/record_reader.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/harness/stub_reader.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/store/store_error.hpp"
#include "tick_store/store/stub_store.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace tick_store::harness {
namespace {

[[nodiscard]] std::uint8_t byte_value(std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

// True if the two ranges share any byte. std::less gives a total order on pointers
// into different objects, which the built-in operators do not.
[[nodiscard]] bool overlaps(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
    if (a.empty() || b.empty()) {
        return false;
    }
    const std::less<> before;
    return before(a.data(), b.data() + b.size()) && before(b.data(), a.data() + a.size());
}

// The type byte of a whole frame, or nullopt if it has no payload.
[[nodiscard]] std::optional<std::uint8_t> type_of_frame(std::span<const std::byte> frame) noexcept {
    if (frame.size() <= itch::kFramePrefixSize) {
        return std::nullopt;
    }
    return byte_value(frame[itch::kFramePrefixSize]);
}

[[nodiscard]] Difference at(DifferenceKind kind, std::uint64_t record,
                            std::uint64_t source_offset) noexcept {
    Difference difference;
    difference.kind = kind;
    difference.record_index = record;
    difference.source_offset = source_offset;
    return difference;
}

class Walk {
public:
    Walk(std::span<const std::byte> source, RecordReader& reader) noexcept
        : source_(source), reader_(reader) {}

    void run() {
        itch::FrameCursor cursor{source_};
        for (auto frame = cursor.next(); frame.has_value(); frame = cursor.next()) {
            const std::size_t size = itch::kFramePrefixSize + frame->payload.size();
            step(source_.subspan(frame->offset, size), frame->offset);
        }
        const std::span<const std::byte> tail = cursor.remaining();
        report_.source_tail_bytes = tail.size();
        source_hash_.update(tail);
        report_.source_bytes = source_.size();

        if (reader_live_) {
            drain_extra_records(cursor.offset());
        }
        if (reader_live_) {
            compare_tail(tail, cursor.offset());
        }

        report_.source_sha256 = source_hash_.finish().value_or(Sha256Digest{});
        report_.stored_sha256 = stored_hash_.finish();
    }

    [[nodiscard]] VerifyReport& report() noexcept { return report_; }

private:
    void step(std::span<const std::byte> expected, std::uint64_t offset) {
        ++report_.source_records;
        source_hash_.update(expected);
        if (!reader_live_ || store_done_) [[unlikely]] {
            note_missing(expected, offset);
            return;
        }

        const auto record = reader_.next();
        if (!record.has_value()) [[unlikely]] {
            stop_reading(record.error(), expected, offset);
            return;
        }
        if (!record->has_value()) [[unlikely]] {
            // The store is done, and the tail check still has to run.
            store_done_ = true;
            note_missing(expected, offset);
            return;
        }
        take(**record);
        if (std::ranges::equal(expected, **record)) [[likely]] {
            return;
        }
        ++report_.differing_records;
        note_difference(expected, **record, offset);
    }

    // What every stored record goes through, wherever in the walk it turns up.
    void take(std::span<const std::byte> record) {
        ++report_.stored_records;
        report_.stored_bytes += record.size();
        stored_hash_.update(record);
        if (overlaps(record, source_)) [[unlikely]] {
            ++report_.aliased_records;
        }
    }

    void stop_reading(RecordReadError error, std::span<const std::byte> expected,
                      std::uint64_t offset) {
        // The rest of the stored stream will never come, so its digest could only
        // mislead. The source still gets walked, for the pin and the totals.
        stored_hash_.cancel();
        reader_live_ = false;
        report_.store_error = std::move(error);
        ++report_.missing_records;
        note_source_record(DifferenceKind::store_error, expected, offset);
    }

    void note_missing(std::span<const std::byte> expected, std::uint64_t offset) {
        ++report_.missing_records;
        note_source_record(DifferenceKind::missing_record, expected, offset);
    }

    // A difference that has a source record and no stored one to compare it with.
    void note_source_record(DifferenceKind kind, std::span<const std::byte> expected,
                            std::uint64_t offset) {
        Difference difference = at(kind, report_.source_records - 1, offset);
        difference.message_type = type_of_frame(expected);
        difference.expected_size = expected.size();
        note_first(difference);
    }

    void note_difference(std::span<const std::byte> expected, std::span<const std::byte> actual,
                         std::uint64_t offset) {
        Difference difference = at(DifferenceKind::length, report_.source_records - 1, offset);
        difference.message_type = type_of_frame(expected);
        difference.expected_size = expected.size();
        difference.actual_size = actual.size();
        locate(expected, actual, DifferenceKind::bytes, difference);
        note_first(difference);
    }

    // Fills in the first differing byte, or the end of the shorter range.
    static void locate(std::span<const std::byte> expected, std::span<const std::byte> actual,
                       DifferenceKind kind_if_bytes, Difference& difference) {
        const auto [in_expected, in_actual] = std::ranges::mismatch(expected, actual);
        const auto index = static_cast<std::uint64_t>(in_expected - expected.begin());
        difference.source_offset += index;
        if (in_expected == expected.end() || in_actual == actual.end()) {
            return;
        }
        difference.kind = kind_if_bytes;
        const std::uint8_t differing = byte_value(*in_expected ^ *in_actual);
        difference.bit = static_cast<std::uint8_t>(std::countr_zero(differing));
        difference.expected_byte = byte_value(*in_expected);
        difference.actual_byte = byte_value(*in_actual);
    }

    void note_first(const Difference& difference) {
        if (!report_.first_difference.has_value()) {
            report_.first_difference = difference;
        }
    }

    void drain_extra_records(std::uint64_t end_of_records) {
        while (!store_done_) {
            const auto record = reader_.next();
            if (!record.has_value()) [[unlikely]] {
                stored_hash_.cancel();
                reader_live_ = false;
                report_.store_error = record.error();
                note_first(at(DifferenceKind::store_error,
                              report_.source_records + report_.extra_records, end_of_records));
                return;
            }
            if (!record->has_value()) {
                return;
            }
            take(**record);
            Difference difference = at(DifferenceKind::extra_record,
                                       report_.source_records + report_.extra_records,
                                       end_of_records);
            difference.message_type = type_of_frame(**record);
            difference.actual_size = (*record)->size();
            note_first(difference);
            ++report_.extra_records;
        }
    }

    void compare_tail(std::span<const std::byte> expected, std::uint64_t offset) {
        const auto stored = reader_.tail();
        if (!stored.has_value()) [[unlikely]] {
            stored_hash_.cancel();
            report_.store_error = stored.error();
            note_first(at(DifferenceKind::store_error, report_.source_records, offset));
            return;
        }
        report_.stored_tail_bytes = stored->size();
        report_.stored_bytes += stored->size();
        stored_hash_.update(*stored);
        if (overlaps(*stored, source_)) [[unlikely]] {
            ++report_.aliased_records;
        }
        report_.tail_matches = std::ranges::equal(expected, *stored);
        if (report_.tail_matches) {
            return;
        }
        Difference difference = at(DifferenceKind::tail_length, report_.source_records, offset);
        difference.expected_size = expected.size();
        difference.actual_size = stored->size();
        locate(expected, *stored, DifferenceKind::tail_bytes, difference);
        note_first(difference);
    }

    std::span<const std::byte> source_;
    RecordReader& reader_;
    bool reader_live_ = true;
    bool store_done_ = false;
    VerifyReport report_;
    HashWorker source_hash_;
    HashWorker stored_hash_;
};

using Clock = std::chrono::steady_clock;

[[nodiscard]] double seconds_since(Clock::time_point start) noexcept {
    return std::chrono::duration<double>{Clock::now() - start}.count();
}

// The reader for a file the parser refused at open: nothing to give but the error.
class RefusedStore final : public RecordReader {
public:
    explicit RefusedStore(RecordReadError error) noexcept : error_(std::move(error)) {}

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

std::string_view to_string(DifferenceKind kind) noexcept {
    switch (kind) {
        case DifferenceKind::bytes:
            return "bytes";
        case DifferenceKind::length:
            return "length";
        case DifferenceKind::missing_record:
            return "missing_record";
        case DifferenceKind::extra_record:
            return "extra_record";
        case DifferenceKind::tail_bytes:
            return "tail_bytes";
        case DifferenceKind::tail_length:
            return "tail_length";
        case DifferenceKind::store_error:
            return "store_error";
    }
    return "invalid";
}

VerifyReport verify(std::span<const std::byte> source, RecordReader& reader,
                    const VerifyOptions& options) {
    const Clock::time_point start = Clock::now();
    Walk walk{source, reader};
    walk.run();
    VerifyReport report = std::move(walk.report());
    report.expected_sha256 = options.expected_sha256;
    report.wall_seconds = seconds_since(start);
    return report;
}

VerifyReport verify_stub_store(std::span<const std::byte> source,
                               std::span<const std::byte> store_file,
                               const VerifyOptions& options) {
    auto parser = store::StoreParser::open(store_file);
    if (!parser.has_value()) {
        RefusedStore refused{{.message = to_string(parser.error()),
                              .store_offset = parser.error().offset}};
        return verify(source, refused, options);
    }
    StubRecordReader reader{*parser};
    return verify(source, reader, options);
}

}  // namespace tick_store::harness
