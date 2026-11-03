#include "tick_store/codec/gorilla_xor.hpp"

#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace tick_store::codec {

namespace {

// A window: X's set bits lie in the `bits` bits above `trailing` zeros, below
// `leading` zeros. leading + bits + trailing = 64, and bits is at least 1.
struct Window {
    unsigned leading = 0;
    unsigned trailing = 0;

    [[nodiscard]] constexpr unsigned bits() const noexcept {
        return kWordBits - leading - trailing;
    }

    // True if X, which is not zero, has its set bits inside this window.
    [[nodiscard]] constexpr bool holds(std::uint64_t x) const noexcept {
        return std::cmp_greater_equal(std::countl_zero(x), leading) &&
               std::cmp_greater_equal(std::countr_zero(x), trailing);
    }
};

}  // namespace

std::expected<void, CodecError> encode_xor_block(std::span<const std::uint64_t> words,
                                                 std::vector<std::byte>& out, XorStats* stats) {
    if (words.empty() || words.size() > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "block values", 0,
                                           kMaxBlockValues, words.size()));
    }
    const std::size_t start = out.size();
    const std::size_t header = begin_block(out, static_cast<std::uint32_t>(words.size()));
    BitWriter writer{out};
    writer.write(words.front(), kWordBits);
    std::uint64_t previous = words.front();
    bool have_window = false;
    Window window;
    XorStats counted;
    for (const std::uint64_t word : words.subspan(1)) {
        const std::uint64_t x = word ^ previous;
        previous = word;
        if (x == 0) {
            writer.write_bit(false);
            ++counted.zero;
            continue;
        }
        if (have_window && window.holds(x)) {
            writer.write(0b10U, 2);
            // trailing <= 63, since the window holds at least one bit.
            writer.write(x >> window.trailing, window.bits());
            ++counted.reuse;
            counted.data_bits += window.bits();
            continue;
        }
        window.leading = std::min(static_cast<unsigned>(std::countl_zero(x)), kXorMaxLeading);
        window.trailing = static_cast<unsigned>(std::countr_zero(x));
        have_window = true;
        writer.write(0b11U, 2);
        writer.write(window.leading, kXorLeadingBits);
        writer.write(window.bits() - 1U, kXorLengthBits);
        writer.write(x >> window.trailing, window.bits());
        ++counted.new_window;
        counted.data_bits += window.bits();
    }
    const auto payload = writer.finish();
    if (!payload.has_value()) {
        out.resize(start);
        return std::unexpected(payload.error());
    }
    // At most 64 + (2^20 - 1) * 77 bits, under 10 MB, so the size fits 32 bits.
    patch_block_header(out, header, static_cast<std::uint32_t>(*payload));
    if (stats != nullptr) {
        stats->zero += counted.zero;
        stats->reuse += counted.reuse;
        stats->new_window += counted.new_window;
        stats->data_bits += counted.data_bits;
    }
    return {};
}

std::expected<std::size_t, CodecError> decode_xor_block(std::span<const std::byte> block,
                                                        std::vector<std::uint64_t>& out) {
    const auto header = read_block_header(block, kXorFirstValueBits, kXorMinBitsPerValue);
    if (!header.has_value()) {
        return std::unexpected(header.error());
    }
    const std::size_t start = out.size();
    const auto fail = [&](const CodecError& error) {
        out.resize(start);
        return std::unexpected(error);
    };
    // The count is at most kMaxBlockValues and within what the payload can hold.
    out.reserve(start + header->count);
    constexpr std::uint64_t kPayloadStart = kBlockHeaderSize * 8U;
    BitReader reader{block.subspan(kBlockHeaderSize, header->payload_bytes), kPayloadStart};

    const auto first = reader.read(kWordBits, "first word");
    if (!first.has_value()) {
        return fail(first.error());
    }
    out.push_back(*first);
    std::uint64_t previous = *first;
    bool have_window = false;
    Window window;
    for (std::uint32_t i = 1; i < header->count; ++i) {
        const std::uint64_t at = kPayloadStart + reader.position();
        const auto changed = reader.read_bit("xor control");
        if (!changed.has_value()) {
            return fail(changed.error());
        }
        if (!*changed) {
            out.push_back(previous);
            continue;
        }
        const auto opens = reader.read_bit("xor control");
        if (!opens.has_value()) {
            return fail(opens.error());
        }
        std::uint64_t x = 0;
        if (!*opens) {
            if (!have_window) {
                return fail(codec_error(CodecErrorKind::bad_window, "xor window reuse", at, 1, 0));
            }
            const auto data = reader.read(window.bits(), "xor data bits");
            if (!data.has_value()) {
                return fail(data.error());
            }
            // trailing <= 63, since the window holds at least one bit.
            x = *data << window.trailing;
            if (x == 0) {
                return fail(codec_error(CodecErrorKind::non_canonical, "xor reused data bits",
                                        at, 0, 0b10U));
            }
        } else {
            const auto leading = reader.read(kXorLeadingBits, "xor leading zeros");
            if (!leading.has_value()) {
                return fail(leading.error());
            }
            const auto length = reader.read(kXorLengthBits, "xor data length");
            if (!length.has_value()) {
                return fail(length.error());
            }
            // Both fields are at most 6 bits wide, so neither cast nor sum can wrap.
            const auto lead = static_cast<unsigned>(*leading);
            const unsigned bits = static_cast<unsigned>(*length) + 1U;
            // D4: the window must fit the word before its trailing zeros are computed.
            if (lead + bits > kWordBits) {
                return fail(codec_error(CodecErrorKind::bad_window, "xor window", at, kWordBits,
                                        lead + bits));
            }
            const unsigned trailing = kWordBits - lead - bits;
            const auto data = reader.read(bits, "xor data bits");
            if (!data.has_value()) {
                return fail(data.error());
            }
            // The encoder's window ends at X's lowest set bit, and starts at its highest
            // unless the leading count was capped.
            x = *data << trailing;
            const unsigned exact_trailing =
                x == 0 ? kWordBits : static_cast<unsigned>(std::countr_zero(x));
            if (exact_trailing != trailing) {
                return fail(codec_error(CodecErrorKind::non_canonical, "xor trailing zeros", at,
                                        exact_trailing, trailing));
            }
            const unsigned exact_leading =
                std::min(static_cast<unsigned>(std::countl_zero(x)), kXorMaxLeading);
            if (exact_leading != lead) {
                return fail(codec_error(CodecErrorKind::non_canonical, "xor leading zeros", at,
                                        exact_leading, lead));
            }
            if (have_window && window.holds(x)) {
                return fail(codec_error(CodecErrorKind::non_canonical, "xor new window", at, 0b10U,
                                        0b11U));
            }
            window = Window{.leading = lead, .trailing = trailing};
            have_window = true;
        }
        previous ^= x;
        out.push_back(previous);
    }
    if (const auto end = reader.finish("block padding"); !end.has_value()) {
        return fail(end.error());
    }
    return kBlockHeaderSize + header->payload_bytes;
}

std::expected<void, CodecError> encode_xor(std::span<const std::uint64_t> words,
                                           std::size_t block_values, std::vector<std::byte>& out,
                                           XorStats* stats) {
    if (const auto checked = check_block_values(block_values); !checked.has_value()) {
        return checked;
    }
    const std::size_t start = out.size();
    for (std::size_t at = 0; at < words.size(); at += block_values) {
        const std::size_t size = std::min(words.size() - at, block_values);
        if (const auto block = encode_xor_block(words.subspan(at, size), out, stats);
            !block.has_value()) {
            out.resize(start);
            return block;
        }
    }
    return {};
}

std::expected<void, CodecError> decode_xor(std::span<const std::byte> column, std::uint64_t count,
                                           std::vector<std::uint64_t>& out) {
    return decode_column(column, count, out, &decode_xor_block);
}

std::expected<void, CodecError> encode_xor_doubles(std::span<const double> values,
                                                   std::size_t block_values,
                                                   std::vector<std::byte>& out, XorStats* stats) {
    std::vector<std::uint64_t> words;
    words.reserve(values.size());
    for (const double value : values) {
        words.push_back(std::bit_cast<std::uint64_t>(value));
    }
    return encode_xor(words, block_values, out, stats);
}

std::expected<void, CodecError> decode_xor_doubles(std::span<const std::byte> column,
                                                   std::uint64_t count, std::vector<double>& out) {
    std::vector<std::uint64_t> words;
    if (const auto done = decode_xor(column, count, words); !done.has_value()) {
        return done;
    }
    out.reserve(out.size() + words.size());
    for (const std::uint64_t word : words) {
        out.push_back(std::bit_cast<double>(word));
    }
    return {};
}

}  // namespace tick_store::codec
