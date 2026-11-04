#include "tick_store/codec/pfor.hpp"

#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/int_transform.hpp"
#include "tick_store/codec/lane_pack.hpp"
#include "tick_store/codec/patch_list.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace tick_store::codec {

namespace {

using WidthHistogram = std::array<std::uint32_t, kWordBits + 1>;

constexpr std::uint64_t kMaxWord = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kSignBit = std::uint64_t{1} << 63U;
constexpr unsigned kLastTransform = static_cast<unsigned>(IntTransform::head_delta);
constexpr std::size_t kVectorHeaderBytes = 3;
constexpr unsigned kRemainderCountBytes = 4;
constexpr unsigned kExceptionCountBytes = 2;
constexpr std::uint64_t kExceptionCountBits = std::uint64_t{kExceptionCountBytes} * 8U;
constexpr std::uint64_t kHighWidthBits = 8;
constexpr std::uint64_t kGapWidthBits = 8;
// The adaptivity yardstick in PforStats: groups of this many values, each paying a
// width byte.
constexpr std::size_t kGroupValues = 128;
constexpr std::uint64_t kGroupWidthBits = 8;
// Scale candidates come from runs of this many values, and this many of the commonest.
constexpr std::size_t kScaleRun = 8;
constexpr std::size_t kCommonDivisors = 3;

constexpr PatchFields kRemainderFields{.gap_width = "pfor remainder gap width",
                                       .positions = "pfor remainder position",
                                       .values = "pfor remainder"};
constexpr PatchFields kExceptionFields{.gap_width = "pfor exception gap width",
                                       .positions = "pfor exception position",
                                       .values = "pfor exception high bits"};

[[nodiscard]] constexpr unsigned width_of(std::uint64_t value) noexcept {
    return static_cast<unsigned>(std::bit_width(value));
}

[[nodiscard]] constexpr std::uint64_t round_up_to_byte(std::uint64_t bits) noexcept {
    return (bits + 7U) / 8U * 8U;
}

// Bits `count` values take packed at `width` with `exceptions` of `high` bits each, their
// gaps `gap_bits` wide, the exception count, high width and gap width included. A full
// vector is whole words; anything shorter is linear and padded to a byte.
[[nodiscard]] constexpr std::uint64_t body_bits(std::size_t count, unsigned width,
                                                std::size_t exceptions, unsigned high,
                                                unsigned gap_bits) noexcept {
    const std::uint64_t raw = std::uint64_t{count} * width;
    std::uint64_t bits = (count == kVectorValues ? raw : round_up_to_byte(raw)) + kExceptionCountBits;
    if (exceptions > 0) {
        bits += kHighWidthBits + kGapWidthBits +
                round_up_to_byte(std::uint64_t{exceptions} * gap_bits) +
                round_up_to_byte(std::uint64_t{exceptions} * high);
    }
    return bits;
}

struct WidthChoice {
    unsigned width = 0;
    unsigned high = 0;
    std::size_t exceptions = 0;
    std::uint64_t bits = 0;
};

// The width that makes values of these bit widths smallest. Ties keep the wider width,
// which has fewer exceptions to patch. Widths that would leave more than half the values
// as exceptions are not tried: there each exception costs a gap and high bits on top of
// the width, so the next width up is nearly always smaller.
[[nodiscard]] WidthChoice choose_width(std::span<const std::uint8_t> widths, bool patch) noexcept {
    const std::size_t count = widths.size();
    WidthHistogram histogram{};
    for (const std::uint8_t width : widths) {
        ++histogram[width];
    }
    unsigned widest = 0;
    for (unsigned w = kWordBits; w > 0; --w) {
        if (histogram[w] != 0) {
            widest = w;
            break;
        }
    }
    WidthChoice best{.width = widest, .high = 0, .exceptions = 0,
                     .bits = body_bits(count, widest, 0, 0, 0)};
    if (!patch || widest == 0) {
        return best;
    }
    // The narrowest width to try, and the exceptions each width from there leaves.
    std::array<std::size_t, kWordBits + 1> exceptions{};
    unsigned lowest = widest;
    std::size_t above = 0;
    while (lowest > 0 && above + histogram[lowest] <= count / 2) {
        above += histogram[lowest];
        --lowest;
        exceptions[lowest] = above;
    }
    if (lowest == widest) {
        return best;
    }
    // The widest gap between the exceptions of each width tried, in one pass: a value w
    // bits wide is an exception at every width below w.
    std::array<std::size_t, kWordBits + 1> next{};
    std::array<std::size_t, kWordBits + 1> widest_gap{};
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned top = widths[i];
        for (unsigned b = lowest; b < top; ++b) {
            widest_gap[b] = std::max(widest_gap[b], i - next[b]);
            next[b] = i + 1;
        }
    }
    for (unsigned width = widest; width-- > lowest;) {
        const unsigned high = widest - width;
        const std::uint64_t bits = body_bits(count, width, exceptions[width], high,
                                             width_of(widest_gap[width]));
        if (bits < best.bits) {
            best = {.width = width, .high = high, .exceptions = exceptions[width], .bits = bits};
        }
    }
    return best;
}

[[nodiscard]] constexpr std::uint64_t mode_value(std::uint64_t y, bool zigzag) noexcept {
    return zigzag ? zigzag_encode(y) : y;
}

struct VectorPlan {
    bool zigzag = false;
    std::uint64_t base = 0;
    WidthChoice width;
    std::uint64_t total_bits = 0;
};

[[nodiscard]] std::uint64_t base_of(std::span<const std::uint64_t> y, bool zigzag) noexcept {
    if (zigzag) {
        std::uint64_t least = kMaxWord;
        for (const std::uint64_t value : y) {
            least = std::min(least, zigzag_encode(value));
        }
        return least;
    }
    // The smallest as signed: flipping the sign bit makes unsigned order signed order.
    std::uint64_t least = kMaxWord;
    for (const std::uint64_t value : y) {
        least = std::min(least, value ^ kSignBit);
    }
    return least ^ kSignBit;
}

using VectorWidths = std::array<std::uint8_t, kVectorValues>;

// The bit width of each value's offset from `base` in this mode, into `widths`.
[[nodiscard]] std::span<const std::uint8_t> widths_of(std::span<const std::uint64_t> y, bool zigzag,
                                                      std::uint64_t base,
                                                      VectorWidths& widths) noexcept {
    for (std::size_t i = 0; i < y.size(); ++i) {
        widths[i] = static_cast<std::uint8_t>(width_of(mode_value(y[i], zigzag) - base));
    }
    return std::span<const std::uint8_t>{widths}.first(y.size());
}

[[nodiscard]] VectorPlan plan_vector(std::span<const std::uint64_t> y, bool patch) noexcept {
    VectorPlan best;
    VectorWidths widths{};
    for (const bool zigzag : {false, true}) {
        const std::uint64_t base = base_of(y, zigzag);
        const WidthChoice width = choose_width(widths_of(y, zigzag, base, widths), patch);
        const std::uint64_t total = width.bits + (8U * (kVectorHeaderBytes + bytes_for(base)));
        if (!zigzag || total < best.total_bits) {
            best = {.zigzag = zigzag, .base = base, .width = width, .total_bits = total};
        }
    }
    return best;
}

// What a full vector would take with a width per group of 128, in the plan's mode and base.
[[nodiscard]] std::uint64_t bits_per_group(std::span<const std::uint64_t> y,
                                           const VectorPlan& plan, bool patch) noexcept {
    VectorWidths storage{};
    const std::span<const std::uint8_t> widths = widths_of(y, plan.zigzag, plan.base, storage);
    std::uint64_t bits = 0;
    for (std::size_t at = 0; at < widths.size(); at += kGroupValues) {
        bits += choose_width(widths.subspan(at, std::min(kGroupValues, widths.size() - at)), patch)
                    .bits +
                kGroupWidthBits;
    }
    return bits;
}

// Buffers an encoder reuses from block to block and scale to scale, so a column of small
// blocks does not allocate for each.
struct Scratch {
    std::vector<std::uint64_t> q;
    std::vector<std::uint64_t> y;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> words;
    Patches patches;
    Patches remainders;
    std::vector<std::uint64_t> candidates;
    std::vector<std::uint64_t> runs;
    std::vector<std::pair<std::size_t, std::uint64_t>> counted;
    std::vector<std::byte> best;
    std::vector<std::byte> trial;
};

void emit_vector(std::span<const std::uint64_t> y, const VectorPlan& plan,
                 std::vector<std::byte>& out, Scratch& scratch) {
    const unsigned width = plan.width.width;
    out.push_back(std::byte{plan.zigzag ? std::uint8_t{1} : std::uint8_t{0}});
    out.push_back(static_cast<std::byte>(width));
    const unsigned base_bytes = bytes_for(plan.base);
    out.push_back(static_cast<std::byte>(base_bytes));
    append_le(out, plan.base, base_bytes);

    scratch.offsets.resize(y.size());
    scratch.patches.positions.clear();
    scratch.patches.values.clear();
    for (std::size_t i = 0; i < y.size(); ++i) {
        const std::uint64_t offset = mode_value(y[i], plan.zigzag) - plan.base;
        scratch.offsets[i] = offset;
        // Wider than `width`, so width < 64 and the shift is in range.
        if (width_of(offset) > width) {
            scratch.patches.positions.push_back(static_cast<std::uint32_t>(i));
            scratch.patches.values.push_back(offset >> width);
        }
    }
    if (y.size() == kVectorValues) {
        scratch.words.resize(vector_words(width));
        pack_vector(std::span<const std::uint64_t>{scratch.offsets}.first<kVectorValues>(), width,
                    scratch.words);
        append_words_le(scratch.words, out);
    } else {
        pack_linear(scratch.offsets, width, out);
    }
    append_le(out, scratch.patches.positions.size(), kExceptionCountBytes);
    if (!scratch.patches.positions.empty()) {
        out.push_back(static_cast<std::byte>(plan.width.high));
        append_patches(scratch.patches, plan.width.high, out);
    }
}

void accumulate(PforStats& into, const PforStats& from) noexcept {
    into.scaled_blocks += from.scaled_blocks;
    into.remainders += from.remainders;
    into.full_vectors += from.full_vectors;
    into.tail_values += from.tail_values;
    into.zigzag_vectors += from.zigzag_vectors;
    into.exceptions += from.exceptions;
    into.full_vector_bits += from.full_vector_bits;
    into.full_vector_bits_per_128 += from.full_vector_bits_per_128;
    into.tail_padding_bits += from.tail_padding_bits;
    into.head_delta_fallbacks += from.head_delta_fallbacks;
}

// One block at scale `scale`, appended to `out`.
void encode_scaled(std::span<const std::uint64_t> values, std::uint64_t scale,
                   const PforOptions& options, std::vector<std::byte>& out, PforStats& stats,
                   Scratch& scratch) {
    const std::size_t count = values.size();
    const std::size_t header = begin_block(out, static_cast<std::uint32_t>(count));
    const std::size_t transform_at = out.size();
    out.push_back(static_cast<std::byte>(options.transform));
    const unsigned scale_bytes = bytes_for(scale);
    out.push_back(static_cast<std::byte>(scale_bytes));
    append_le(out, scale, scale_bytes);

    scratch.q.assign(values.begin(), values.end());
    if (scale > 1) {
        Patches& remainders = scratch.remainders;
        remainders.positions.clear();
        remainders.values.clear();
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t remainder = values[i] % scale;
            scratch.q[i] = values[i] / scale;
            if (remainder != 0) {
                remainders.positions.push_back(static_cast<std::uint32_t>(i));
                remainders.values.push_back(remainder);
            }
        }
        append_le(out, remainders.positions.size(), kRemainderCountBytes);
        append_patches(remainders, width_of(scale - 1), out);
        ++stats.scaled_blocks;
        stats.remainders += remainders.positions.size();
    }

    scratch.y.resize(count);
    switch (options.transform) {
        case IntTransform::none:
            std::ranges::copy(scratch.q, scratch.y.begin());
            break;
        case IntTransform::delta:
            delta_encode(scratch.q, scratch.y);
            break;
        case IntTransform::delta_of_delta:
            delta_of_delta_encode(scratch.q, scratch.y);
            break;
        case IntTransform::head_delta:
            if (!head_delta_encode(scratch.q, scratch.y)) {
                delta_encode(scratch.q, scratch.y);
                out[transform_at] = static_cast<std::byte>(IntTransform::delta);
                ++stats.head_delta_fallbacks;
            }
            break;
    }

    const std::span<const std::uint64_t> y{scratch.y};
    for (std::size_t at = 0; at < count; at += kVectorValues) {
        const std::span<const std::uint64_t> vector = y.subspan(at, std::min(kVectorValues, count - at));
        const VectorPlan plan = plan_vector(vector, options.patch);
        if (vector.size() == kVectorValues) {
            ++stats.full_vectors;
            stats.full_vector_bits += plan.width.bits;
            stats.full_vector_bits_per_128 += bits_per_group(vector, plan, options.patch);
        } else {
            stats.tail_values += vector.size();
            const std::uint64_t width = plan.width.width;
            stats.tail_padding_bits +=
                (kVectorValues * width) - round_up_to_byte(vector.size() * width);
        }
        stats.zigzag_vectors += plan.zigzag ? 1U : 0U;
        stats.exceptions += plan.width.exceptions;
        emit_vector(vector, plan, out, scratch);
    }
    // At most 2^20 values of under 160 bits each, remainders included, so the payload is
    // under 21 MB and fits 32 bits.
    patch_block_header(out, header,
                       static_cast<std::uint32_t>(out.size() - header - kBlockHeaderSize));
}

// Candidate scales, all from the block: 1, the divisor common to every value, and the
// commonest divisors of runs of kScaleRun values with what those share.
void scale_candidates(std::span<const std::uint64_t> values, Scratch& scratch) {
    std::vector<std::uint64_t>& candidates = scratch.candidates;
    candidates.assign(1, 1);
    const auto add = [&candidates](std::uint64_t scale) {
        if (scale > 1 && std::ranges::find(candidates, scale) == candidates.end()) {
            candidates.push_back(scale);
        }
    };
    std::uint64_t all = 0;
    for (const std::uint64_t value : values) {
        all = std::gcd(all, value);
    }
    add(all);

    std::vector<std::uint64_t>& runs = scratch.runs;
    runs.clear();
    for (std::size_t at = 0; at + kScaleRun <= values.size(); at += kScaleRun) {
        std::uint64_t divisor = 0;
        for (const std::uint64_t value : values.subspan(at, kScaleRun)) {
            divisor = std::gcd(divisor, value);
        }
        if (divisor > 1) {
            runs.push_back(divisor);
        }
    }
    std::ranges::sort(runs);
    std::vector<std::pair<std::size_t, std::uint64_t>>& counted = scratch.counted;
    counted.clear();
    for (std::size_t at = 0; at < runs.size();) {
        std::size_t end = at;
        while (end < runs.size() && runs[end] == runs[at]) {
            ++end;
        }
        counted.emplace_back(end - at, runs[at]);
        at = end;
    }
    // Commonest first, and the smaller divisor first among equals.
    std::ranges::sort(counted, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::uint64_t shared = 0;
    for (std::size_t i = 0; i < std::min(kCommonDivisors, counted.size()); ++i) {
        add(counted[i].second);
        shared = std::gcd(shared, counted[i].second);
        if (i > 0) {
            add(shared);
        }
    }
}

// One block, its values already checked, with the caller's buffers.
void encode_block_with(std::span<const std::uint64_t> values, const PforOptions& options,
                       std::vector<std::byte>& out, PforStats* stats, Scratch& scratch) {
    PforStats chosen;
    if (!options.adaptive_scale) {
        encode_scaled(values, 1, options, out, chosen, scratch);
    } else {
        scale_candidates(values, scratch);
        bool have_best = false;
        for (const std::uint64_t scale : scratch.candidates) {
            scratch.trial.clear();
            PforStats counted;
            encode_scaled(values, scale, options, scratch.trial, counted, scratch);
            if (!have_best || scratch.trial.size() < scratch.best.size()) {
                std::swap(scratch.best, scratch.trial);
                chosen = counted;
                have_best = true;
            }
        }
        out.insert(out.end(), scratch.best.begin(), scratch.best.end());
    }
    if (stats != nullptr) {
        accumulate(*stats, chosen);
    }
}

[[nodiscard]] std::expected<void, CodecError> check_options(std::size_t count,
                                                            const PforOptions& options) {
    if (count == 0 || count > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "block values", 0,
                                           kMaxBlockValues, count));
    }
    if (static_cast<unsigned>(options.transform) > kLastTransform) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "pfor transform", 0,
                                           kLastTransform,
                                           static_cast<unsigned>(options.transform)));
    }
    return {};
}

}  // namespace

std::expected<void, CodecError> encode_pfor_block(std::span<const std::uint64_t> values,
                                                  const PforOptions& options,
                                                  std::vector<std::byte>& out, PforStats* stats) {
    if (const auto checked = check_options(values.size(), options); !checked.has_value()) {
        return checked;
    }
    Scratch scratch;
    encode_block_with(values, options, out, stats, scratch);
    return {};
}

std::expected<std::size_t, CodecError> decode_pfor_block(std::span<const std::byte> block,
                                                         std::vector<std::uint64_t>& out) {
    const auto header = read_block_header(block, kPforMinPayloadBytes * 8U, 0);
    if (!header.has_value()) {
        return std::unexpected(header.error());
    }
    const std::size_t count = header->count;
    const std::size_t vectors = (count + kVectorValues - 1) / kVectorValues;
    // Every vector takes at least its header and exception count, so a count the payload
    // cannot hold is refused before anything is sized by it (D8). read_block_header has
    // already seen at least kPforMinPayloadBytes.
    if (header->payload_bytes < kPforMinPayloadBytes + ((vectors - 1) * kPforMinVectorBytes)) {
        const std::size_t most =
            (1U + ((header->payload_bytes - kPforMinPayloadBytes) / kPforMinVectorBytes)) *
            kVectorValues;
        return std::unexpected(
            codec_error(CodecErrorKind::bad_count, "block count", 0, most, count));
    }
    constexpr std::uint64_t kPayloadStart = kBlockHeaderSize * 8U;
    ByteCursor cursor{block.subspan(kBlockHeaderSize, header->payload_bytes), kPayloadStart};
    const std::size_t start = out.size();
    const auto fail = [&](const CodecError& error) {
        out.resize(start);
        return std::unexpected(error);
    };
    // Reads a one-byte field. The payload holds the fixed fields of a vector, but earlier
    // vectors may have used them up, so every read is still checked.
    const auto byte_field = [&cursor](std::string_view field) { return cursor.read_le(1, field); };

    const std::uint64_t transform_at = cursor.bit_offset();
    const auto transform = byte_field("pfor transform");
    if (!transform.has_value()) {
        return fail(transform.error());
    }
    if (*transform > kLastTransform) {
        return fail(codec_error(CodecErrorKind::bad_field, "pfor transform", transform_at,
                                kLastTransform, *transform));
    }
    const std::uint64_t scale_bytes_at = cursor.bit_offset();
    const auto scale_bytes = byte_field("pfor scale bytes");
    if (!scale_bytes.has_value()) {
        return fail(scale_bytes.error());
    }
    if (*scale_bytes == 0 || *scale_bytes > 8) {
        return fail(
            codec_error(CodecErrorKind::bad_field, "pfor scale bytes", scale_bytes_at, 8, *scale_bytes));
    }
    const std::uint64_t scale_at = cursor.bit_offset();
    const auto scale = cursor.read_le(static_cast<unsigned>(*scale_bytes), "pfor scale");
    if (!scale.has_value()) {
        return fail(scale.error());
    }
    // A scale of 0 has no top byte that is not zero, so this refuses it too.
    if (bytes_for(*scale) != *scale_bytes) {
        return fail(codec_error(CodecErrorKind::non_canonical, "pfor scale", scale_at,
                                bytes_for(*scale), *scale_bytes));
    }
    // Kept per thread, so a column of blocks allocates its lists once.
    thread_local Patches remainders;
    remainders.positions.clear();
    remainders.values.clear();
    if (*scale > 1) {
        const std::uint64_t count_at = cursor.bit_offset();
        const auto listed = cursor.read_le(kRemainderCountBytes, "pfor remainder count");
        if (!listed.has_value()) {
            return fail(listed.error());
        }
        if (*listed > count) {
            return fail(codec_error(CodecErrorKind::bad_count, "pfor remainder count", count_at,
                                    count, *listed));
        }
        const std::uint64_t list_at = cursor.bit_offset();
        if (const auto read = read_patches(cursor, static_cast<std::size_t>(*listed), count,
                                           width_of(*scale - 1), kRemainderFields, remainders);
            !read.has_value()) {
            return fail(read.error());
        }
        for (const std::uint64_t remainder : remainders.values) {
            if (remainder >= *scale) {
                return fail(codec_error(CodecErrorKind::bad_field, "pfor remainder", list_at,
                                        *scale - 1, remainder));
            }
        }
    }

    reserve_for_block(out, header->count);
    out.resize(start + count);
    const std::span<std::uint64_t> y = std::span{out}.subspan(start);
    // Only the words a vector's width uses are loaded, and only those are read.
    std::array<std::uint64_t, kVectorValues> words;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    thread_local Patches exceptions;
    for (std::size_t at = 0; at < count; at += kVectorValues) {
        const std::span<std::uint64_t> vector = y.subspan(at, std::min(kVectorValues, count - at));
        const std::uint64_t mode_at = cursor.bit_offset();
        const auto mode = byte_field("pfor mode");
        if (!mode.has_value()) {
            return fail(mode.error());
        }
        if (*mode > 1) {
            return fail(codec_error(CodecErrorKind::bad_field, "pfor mode", mode_at, 1, *mode));
        }
        const std::uint64_t width_at = cursor.bit_offset();
        const auto width_field = byte_field("pfor width");
        if (!width_field.has_value()) {
            return fail(width_field.error());
        }
        if (*width_field > kWordBits) {
            return fail(
                codec_error(CodecErrorKind::bad_width, "pfor width", width_at, kWordBits, *width_field));
        }
        const auto width = static_cast<unsigned>(*width_field);
        const std::uint64_t base_bytes_at = cursor.bit_offset();
        const auto base_bytes = byte_field("pfor base bytes");
        if (!base_bytes.has_value()) {
            return fail(base_bytes.error());
        }
        if (*base_bytes > 8) {
            return fail(codec_error(CodecErrorKind::bad_field, "pfor base bytes", base_bytes_at,
                                    8, *base_bytes));
        }
        const std::uint64_t base_at = cursor.bit_offset();
        const auto base = cursor.read_le(static_cast<unsigned>(*base_bytes), "pfor base");
        if (!base.has_value()) {
            return fail(base.error());
        }
        if (bytes_for(*base) != *base_bytes) {
            return fail(codec_error(CodecErrorKind::non_canonical, "pfor base", base_at,
                                    bytes_for(*base), *base_bytes));
        }

        if (vector.size() == kVectorValues) {
            const auto packed = cursor.take(vector_bytes(width), "pfor packed vector");
            if (!packed.has_value()) {
                return fail(packed.error());
            }
            const std::span<std::uint64_t> used = std::span{words}.first(vector_words(width));
            load_words_le(*packed, used);
            unpack_vector(used, width, vector.first<kVectorValues>());
        } else {
            const auto packed = cursor.take(linear_bytes(vector.size(), width), "pfor packed tail");
            if (!packed.has_value()) {
                return fail(packed.error());
            }
            if (!unpack_linear(*packed, width, vector)) {
                // The last byte holds padding that is not zero.
                const std::uint64_t used = vector.size() * width;
                return fail(codec_error(CodecErrorKind::trailing_bits, "pfor tail padding",
                                        cursor.bit_offset() - 8U + (used % 8U), 0,
                                        std::to_integer<std::uint64_t>(packed->back()) >> (used % 8U)));
            }
        }

        const std::uint64_t exceptions_at = cursor.bit_offset();
        const auto listed = cursor.read_le(kExceptionCountBytes, "pfor exception count");
        if (!listed.has_value()) {
            return fail(listed.error());
        }
        if (*listed > vector.size()) {
            return fail(codec_error(CodecErrorKind::bad_count, "pfor exception count",
                                    exceptions_at, vector.size(), *listed));
        }
        if (*listed > 0) {
            const std::uint64_t high_at = cursor.bit_offset();
            const auto high = byte_field("pfor high width");
            if (!high.has_value()) {
                return fail(high.error());
            }
            if (*high == 0 || *high > kWordBits - width) {
                return fail(codec_error(CodecErrorKind::bad_width, "pfor high width", high_at,
                                        kWordBits - width, *high));
            }
            if (const auto read = read_patches(cursor, static_cast<std::size_t>(*listed),
                                               vector.size(), static_cast<unsigned>(*high),
                                               kExceptionFields, exceptions);
                !read.has_value()) {
                return fail(read.error());
            }
            const std::uint64_t highest = std::ranges::max(exceptions.values);
            if (width_of(highest) != *high) {
                return fail(codec_error(CodecErrorKind::non_canonical, "pfor high width", high_at,
                                        width_of(highest), *high));
            }
            // width <= 63 here, since the high width is at least 1.
            for (std::size_t i = 0; i < exceptions.positions.size(); ++i) {
                vector[exceptions.positions[i]] |= exceptions.values[i] << width;
            }
        }
        const std::uint64_t offset = *base;
        if (*mode == 1) {
            for (std::uint64_t& value : vector) {
                value = zigzag_decode(value + offset);
            }
        } else {
            for (std::uint64_t& value : vector) {
                value += offset;
            }
        }
    }

    switch (static_cast<IntTransform>(*transform)) {
        case IntTransform::none:
            break;
        case IntTransform::delta:
            delta_decode(y);
            break;
        case IntTransform::delta_of_delta:
            delta_of_delta_decode(y);
            break;
        case IntTransform::head_delta:
            head_delta_decode(y);
            break;
    }

    if (*scale > 1) {
        // Each value is q * s, plus its remainder, and neither step may pass 2^64 - 1.
        const std::uint64_t most = kMaxWord / *scale;
        if (const std::uint64_t highest = std::ranges::max(y); highest > most) {
            return fail(codec_error(CodecErrorKind::bad_field, "pfor scaled value",
                                    cursor.bit_offset(), most, highest));
        }
        for (std::uint64_t& value : y) {
            value *= *scale;
        }
        for (std::size_t i = 0; i < remainders.positions.size(); ++i) {
            std::uint64_t& value = y[remainders.positions[i]];
            const std::uint64_t remainder = remainders.values[i];
            if (value > kMaxWord - remainder) {
                return fail(codec_error(CodecErrorKind::bad_field, "pfor scaled value",
                                        cursor.bit_offset(), kMaxWord - remainder, value));
            }
            value += remainder;
        }
    }
    if (cursor.remaining() != 0) {
        return fail(codec_error(CodecErrorKind::trailing_bits, "pfor block", cursor.bit_offset(), 0,
                                std::uint64_t{cursor.remaining()} * 8U));
    }
    return kBlockHeaderSize + header->payload_bytes;
}

std::expected<void, CodecError> encode_pfor(std::span<const std::uint64_t> values,
                                            std::size_t block_values, const PforOptions& options,
                                            std::vector<std::byte>& out, PforStats* stats) {
    if (const auto checked = check_block_values(block_values); !checked.has_value()) {
        return checked;
    }
    if (values.empty()) {
        return {};
    }
    // Every block is 1 to block_values values, so the options are all that can be wrong.
    if (const auto checked = check_options(1, options); !checked.has_value()) {
        return checked;
    }
    Scratch scratch;
    for (std::size_t at = 0; at < values.size(); at += block_values) {
        const std::size_t size = std::min(values.size() - at, block_values);
        encode_block_with(values.subspan(at, size), options, out, stats, scratch);
    }
    return {};
}

std::expected<void, CodecError> decode_pfor(std::span<const std::byte> column, std::uint64_t count,
                                            std::vector<std::uint64_t>& out) {
    return decode_column(column, count, out, &decode_pfor_block);
}

}  // namespace tick_store::codec
