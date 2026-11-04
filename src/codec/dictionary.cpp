#include "tick_store/codec/dictionary.hpp"

#include "tick_store/codec/bit_stream.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/pfor.hpp"

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

constexpr unsigned kEntryCountBytes = 4;
constexpr unsigned kLongestBits = 8;
constexpr unsigned kLastMode = static_cast<unsigned>(DictMode::sorted_move_to_front);
constexpr std::uint64_t kPayloadStart = kBlockHeaderSize * 8U;
// Code prefixes the Huffman decoder looks up in one step. Longer codes finish by the
// canonical rule from there.
constexpr unsigned kHuffmanTableBits = 10;

constexpr PforOptions kRankedEntries{.transform = IntTransform::none, .adaptive_scale = true,
                                     .patch = true};
constexpr PforOptions kSortedEntries{.transform = IntTransform::delta, .adaptive_scale = true,
                                     .patch = true};
constexpr PforOptions kIndices{.transform = IntTransform::none, .adaptive_scale = false,
                               .patch = true};

// Bits each per-length count takes for a dictionary of `entries`: enough to hold K.
[[nodiscard]] constexpr unsigned count_bits(std::uint64_t entries) noexcept {
    return static_cast<unsigned>(std::bit_width(entries));
}

struct Entry {
    std::uint64_t value = 0;
    std::uint64_t count = 0;
};

// Huffman code lengths for `counts`, which are non-increasing, returned non-decreasing so
// that entry i takes the i-th: entries used equally often may swap lengths at no cost. The
// classic two-queue construction, since the leaves arrive sorted.
[[nodiscard]] std::vector<unsigned> huffman_lengths(std::span<const std::uint64_t> counts) {
    const std::size_t leaves = counts.size();
    if (leaves == 1) {
        return {0};
    }
    // Nodes 0 .. leaves - 1 are the leaves, lightest first; internal nodes follow in the
    // order they are made, which is also non-decreasing weight.
    std::vector<std::uint64_t> weight((2 * leaves) - 1);
    std::vector<std::size_t> parent((2 * leaves) - 1, 0);
    for (std::size_t i = 0; i < leaves; ++i) {
        weight[i] = counts[leaves - 1 - i];
    }
    std::size_t next_leaf = 0;
    std::size_t next_internal = leaves;
    std::size_t made = leaves;
    const auto take = [&] {
        if (next_leaf < leaves && (next_internal >= made || weight[next_leaf] <= weight[next_internal])) {
            return next_leaf++;
        }
        return next_internal++;
    };
    while (made < weight.size()) {
        const std::size_t a = take();
        const std::size_t b = take();
        weight[made] = weight[a] + weight[b];
        parent[a] = made;
        parent[b] = made;
        ++made;
    }
    std::vector<unsigned> depth(weight.size(), 0);
    for (std::size_t node = weight.size() - 1; node-- > 0;) {
        depth[node] = depth[parent[node]] + 1;
    }
    std::vector<unsigned> lengths(depth.begin(), depth.begin() + static_cast<std::ptrdiff_t>(leaves));
    std::ranges::sort(lengths);
    return lengths;
}

// The canonical code of each entry, in entry order, from non-decreasing lengths.
[[nodiscard]] std::vector<std::uint64_t> canonical_codes(std::span<const unsigned> lengths) {
    std::vector<std::uint64_t> codes(lengths.size(), 0);
    std::uint64_t code = 0;
    unsigned length = lengths.empty() ? 0 : lengths.front();
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (i > 0) {
            ++code;
        }
        code <<= (lengths[i] - length);
        length = lengths[i];
        codes[i] = code;
    }
    return codes;
}

// What one block's encoder works from: the distinct values both ways, and each value's
// index into each.
struct Prepared {
    std::vector<Entry> sorted;
    std::vector<Entry> ranked;
    std::vector<std::uint64_t> sorted_index;
    std::vector<std::uint64_t> ranked_index;
};

[[nodiscard]] Prepared prepare(std::span<const std::uint64_t> values) {
    Prepared prepared;
    std::vector<std::uint64_t> order(values.begin(), values.end());
    std::ranges::sort(order);
    for (const std::uint64_t value : order) {
        if (prepared.sorted.empty() || prepared.sorted.back().value != value) {
            prepared.sorted.push_back(Entry{.value = value, .count = 0});
        }
        ++prepared.sorted.back().count;
    }
    prepared.ranked = prepared.sorted;
    // Stable, so entries used equally often stay in ascending order.
    std::ranges::stable_sort(prepared.ranked, [](const Entry& a, const Entry& b) {
        return a.count > b.count;
    });
    std::vector<std::uint64_t> rank_of(prepared.sorted.size());
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> by_value(prepared.ranked.size());
        for (std::size_t i = 0; i < prepared.ranked.size(); ++i) {
            by_value[i] = {prepared.ranked[i].value, i};
        }
        std::ranges::sort(by_value);
        for (std::size_t i = 0; i < by_value.size(); ++i) {
            rank_of[i] = by_value[i].second;
        }
    }
    prepared.sorted_index.resize(values.size());
    prepared.ranked_index.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto found = std::ranges::lower_bound(prepared.sorted, values[i], {}, &Entry::value);
        const auto index = static_cast<std::uint64_t>(found - prepared.sorted.begin());
        prepared.sorted_index[i] = index;
        prepared.ranked_index[i] = rank_of[index];
    }
    return prepared;
}

[[nodiscard]] std::vector<std::uint64_t> values_of(std::span<const Entry> entries) {
    std::vector<std::uint64_t> out(entries.size());
    std::ranges::transform(entries, out.begin(), &Entry::value);
    return out;
}

using Encoded = std::expected<void, CodecError>;

// The payload's fixed fields and its dictionary. The nested encoders refuse nothing a
// checked block holds, but their results are passed on all the same.
[[nodiscard]] Encoded begin_payload(DictMode mode, std::span<const std::uint64_t> dictionary,
                                    const PforOptions& options, std::vector<std::byte>& out) {
    out.push_back(static_cast<std::byte>(mode));
    append_le(out, dictionary.size(), kEntryCountBytes);
    return encode_pfor_block(dictionary, options, out);
}

[[nodiscard]] Encoded encode_ranked_pfor(const Prepared& prepared, std::vector<std::byte>& out) {
    if (auto begun = begin_payload(DictMode::ranked_pfor, values_of(prepared.ranked),
                                   kRankedEntries, out);
        !begun.has_value()) {
        return begun;
    }
    return encode_pfor_block(prepared.ranked_index, kIndices, out);
}

[[nodiscard]] Encoded encode_ranked_huffman(const Prepared& prepared, std::vector<std::byte>& out) {
    if (auto begun = begin_payload(DictMode::ranked_huffman, values_of(prepared.ranked),
                                   kRankedEntries, out);
        !begun.has_value()) {
        return begun;
    }
    std::vector<std::uint64_t> counts(prepared.ranked.size());
    std::ranges::transform(prepared.ranked, counts.begin(), &Entry::count);
    const std::vector<unsigned> lengths = huffman_lengths(counts);
    const std::vector<std::uint64_t> codes = canonical_codes(lengths);
    const unsigned longest = lengths.back();
    BitWriter writer{out};
    writer.write(longest, kLongestBits);
    const unsigned width = count_bits(counts.size());
    for (unsigned length = 1; length <= longest; ++length) {
        writer.write(static_cast<std::uint64_t>(std::ranges::count(lengths, length)), width);
    }
    for (const std::uint64_t index : prepared.ranked_index) {
        writer.write(codes[index], lengths[index]);
    }
    if (const auto finished = writer.finish(); !finished.has_value()) {
        return std::unexpected(finished.error());
    }
    return {};
}

[[nodiscard]] Encoded encode_sorted_mtf(const Prepared& prepared, std::vector<std::byte>& out) {
    if (auto begun = begin_payload(DictMode::sorted_move_to_front, values_of(prepared.sorted),
                                   kSortedEntries, out);
        !begun.has_value()) {
        return begun;
    }
    std::vector<std::uint64_t> list(prepared.sorted.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        list[i] = i;
    }
    std::vector<std::uint64_t> ranks(prepared.sorted_index.size());
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        const auto found = std::ranges::find(list, prepared.sorted_index[i]);
        const auto rank = static_cast<std::size_t>(found - list.begin());
        ranks[i] = rank;
        std::move_backward(list.begin(), found, found + 1);
        list.front() = prepared.sorted_index[i];
    }
    return encode_pfor_block(ranks, kIndices, out);
}

// Rebases an error from a nested block onto the outer block's offsets.
[[nodiscard]] CodecError rebased(CodecError error, std::uint64_t base_bits) noexcept {
    error.offset += base_bits;
    return error;
}

// Decodes a canonical Huffman stream of `count` indices into `indices`.
[[nodiscard]] std::expected<void, CodecError> read_huffman(std::span<const std::byte> bytes,
                                                           std::uint64_t base_bits,
                                                           std::uint64_t entries,
                                                           std::size_t count,
                                                           std::vector<std::uint64_t>& indices) {
    BitReader reader{bytes, base_bits};
    const std::uint64_t longest_at = base_bits + reader.position();
    const auto longest = reader.read(kLongestBits, "dict longest code");
    if (!longest.has_value()) {
        return std::unexpected(longest.error());
    }
    const std::uint64_t most = entries == 1 ? 0 : kDictMaxCodeBits;
    if ((entries == 1 && *longest != 0) || (entries > 1 && (*longest == 0 || *longest > most))) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_field, "dict longest code", longest_at, most, *longest));
    }
    const auto lengths = static_cast<unsigned>(*longest);
    std::vector<std::uint64_t> per_length(lengths + 1U, 0);
    const std::uint64_t counts_at = base_bits + reader.position();
    std::uint64_t total = 0;
    std::uint64_t space = 0;
    for (unsigned length = 1; length <= lengths; ++length) {
        const auto counted = reader.read(count_bits(entries), "dict code counts");
        if (!counted.has_value()) {
            return std::unexpected(counted.error());
        }
        per_length[length] = *counted;
        // At most 32 counts of at most 21 bits, each weighed by at most 2^31: no wrap.
        total += *counted;
        space += *counted << (lengths - length);
    }
    if (entries > 1) {
        if (total != entries) {
            return std::unexpected(
                codec_error(CodecErrorKind::bad_field, "dict code counts", counts_at, entries, total));
        }
        if (per_length[lengths] == 0) {
            return std::unexpected(codec_error(CodecErrorKind::non_canonical, "dict longest code",
                                               longest_at, lengths - 1U, lengths));
        }
        if (space != (std::uint64_t{1} << lengths)) {
            return std::unexpected(codec_error(CodecErrorKind::bad_field, "dict code lengths",
                                               counts_at, std::uint64_t{1} << lengths, space));
        }
    }
    indices.assign(count, 0);
    if (entries > 1) {
        // The fast path: while a whole longest code is left, peek that many bits and find
        // the code's length from a table of its first `table_bits`, then by the canonical
        // rule for a longer one. The code is complete, so every peek holds one whole code,
        // and nothing here can fail; the last codes, closer to the end than the longest
        // code, take the bit-by-bit loop below, which reports a truncated stream.
        const unsigned table_bits = std::min(lengths, kHuffmanTableBits);
        std::vector<std::uint64_t> first_code(lengths + 1U, 0);
        std::vector<std::uint64_t> first_index(lengths + 1U, 0);
        {
            std::uint64_t first = 0;
            std::uint64_t index = 0;
            for (unsigned length = 1; length <= lengths; ++length) {
                first_code[length] = first;
                first_index[length] = index;
                index += per_length[length];
                first = (first + per_length[length]) << 1U;
            }
        }
        // Each slot: the code's length in the low 8 bits and its entry above them, or 0
        // where every code with that prefix is longer than table_bits.
        std::vector<std::uint64_t> table(std::size_t{1} << table_bits, 0);
        for (unsigned length = 1; length <= table_bits; ++length) {
            const unsigned spread = table_bits - length;
            for (std::uint64_t k = 0; k < per_length[length]; ++k) {
                const std::uint64_t slot = (first_code[length] + k) << spread;
                const std::uint64_t value = ((first_index[length] + k) << 8U) | length;
                std::fill_n(table.begin() + static_cast<std::ptrdiff_t>(slot),
                            std::size_t{1} << spread, value);
            }
        }
        std::size_t done = 0;
        while (done < count && reader.remaining() >= lengths) {
            // remaining() >= lengths and lengths <= 32, so the peek holds real bits.
            const std::uint64_t window = reader.peek_within(lengths);
            const std::uint64_t slot = table[window >> (lengths - table_bits)];
            auto length = static_cast<unsigned>(slot & 0xFFU);
            std::uint64_t index = slot >> 8U;
            if (length == 0) {
                for (length = table_bits + 1U; length <= lengths; ++length) {
                    const std::uint64_t code = window >> (lengths - length);
                    if (code - first_code[length] < per_length[length]) {
                        index = first_index[length] + (code - first_code[length]);
                        break;
                    }
                }
            }
            reader.advance_within(length);
            indices[done++] = index;
        }
        for (std::size_t i = done; i < count; ++i) {
            // The canonical decode: `first` is the first code of each length, `index` the
            // first entry with it. The code is complete, so a code ends by `lengths`.
            std::uint64_t code = 0;
            std::uint64_t first = 0;
            std::uint64_t index = 0;
            for (unsigned length = 1; length <= lengths; ++length) {
                const auto bit = reader.read(1, "dict codes");
                if (!bit.has_value()) {
                    return std::unexpected(bit.error());
                }
                code |= *bit;
                if (code - first < per_length[length]) {
                    indices[i] = index + (code - first);
                    break;
                }
                index += per_length[length];
                first = (first + per_length[length]) << 1U;
                code <<= 1U;
            }
        }
    }
    return reader.finish("dict codes");
}

// One block's counts, added to a column's.
void add_block(DictStats& column, const DictStats& block) noexcept {
    for (std::size_t mode = 0; mode < kDictModes; ++mode) {
        column.blocks[mode] += block.blocks[mode];
    }
    column.entries += block.entries;
}

}  // namespace

std::expected<void, CodecError> encode_dict_block(std::span<const std::uint64_t> values,
                                                  std::vector<std::byte>& out, DictStats* stats) {
    if (values.empty() || values.size() > kMaxBlockValues) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "block values", 0,
                                           kMaxBlockValues, values.size()));
    }
    const Prepared prepared = prepare(values);
    std::vector<std::byte> best;
    std::vector<std::byte> trial;
    DictMode chosen = DictMode::ranked_pfor;
    if (auto done = encode_ranked_pfor(prepared, best); !done.has_value()) {
        return done;
    }
    if (auto done = encode_ranked_huffman(prepared, trial); !done.has_value()) {
        return done;
    }
    if (trial.size() < best.size()) {
        std::swap(best, trial);
        chosen = DictMode::ranked_huffman;
    }
    if (prepared.sorted.size() <= kDictMtfMaxEntries) {
        trial.clear();
        if (auto done = encode_sorted_mtf(prepared, trial); !done.has_value()) {
            return done;
        }
        if (trial.size() < best.size()) {
            std::swap(best, trial);
            chosen = DictMode::sorted_move_to_front;
        }
    }
    const std::size_t header = begin_block(out, static_cast<std::uint32_t>(values.size()));
    out.insert(out.end(), best.begin(), best.end());
    // Two PFOR blocks of at most 2^20 values, each under 21 MB, so this fits 32 bits.
    patch_block_header(out, header,
                       static_cast<std::uint32_t>(out.size() - header - kBlockHeaderSize));
    if (stats != nullptr) {
        ++stats->blocks[static_cast<std::size_t>(chosen)];
        stats->entries += prepared.sorted.size();
    }
    return {};
}

std::expected<std::size_t, CodecError> decode_dict_block(std::span<const std::byte> block,
                                                         std::vector<std::uint64_t>& out) {
    const auto header = read_block_header(block, kDictMinPayloadBytes * 8U, 0);
    if (!header.has_value()) {
        return std::unexpected(header.error());
    }
    const std::size_t count = header->count;
    const std::span<const std::byte> payload = block.subspan(kBlockHeaderSize, header->payload_bytes);
    ByteCursor cursor{payload, kPayloadStart};

    const std::uint64_t mode_at = cursor.bit_offset();
    const auto mode = cursor.read_le(1, "dict mode");
    if (!mode.has_value()) {
        return std::unexpected(mode.error());
    }
    if (*mode > kLastMode) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_field, "dict mode", mode_at, kLastMode, *mode));
    }
    const auto as_mode = static_cast<DictMode>(*mode);
    const std::uint64_t entries_at = cursor.bit_offset();
    const auto entries = cursor.read_le(kEntryCountBytes, "dict entries");
    if (!entries.has_value()) {
        return std::unexpected(entries.error());
    }
    const std::uint64_t most_entries =
        as_mode == DictMode::sorted_move_to_front ? std::min<std::uint64_t>(count, kDictMtfMaxEntries)
                                                  : count;
    if (*entries == 0 || *entries > most_entries) {
        return std::unexpected(codec_error(CodecErrorKind::bad_count, "dict entries", entries_at,
                                           most_entries, *entries));
    }
    const auto entry_count = static_cast<std::size_t>(*entries);

    // The nested PFOR blocks: positions in `payload`, offsets from the block's first byte.
    std::size_t at = kEntryCountBytes + 1U;
    std::vector<std::uint64_t> dictionary;
    const std::uint64_t dictionary_at = kPayloadStart + (at * 8U);
    {
        const auto taken = decode_pfor_block(payload.subspan(at), dictionary);
        if (!taken.has_value()) {
            return std::unexpected(rebased(taken.error(), dictionary_at));
        }
        if (dictionary.size() != entry_count) {
            return std::unexpected(codec_error(CodecErrorKind::bad_count, "dict dictionary count",
                                               dictionary_at, entry_count, dictionary.size()));
        }
        at += *taken;
    }

    std::vector<std::uint64_t> indices;
    const std::uint64_t indices_at = kPayloadStart + (at * 8U);
    if (as_mode == DictMode::ranked_huffman) {
        if (const auto read = read_huffman(payload.subspan(at), indices_at, *entries, count, indices);
            !read.has_value()) {
            return std::unexpected(read.error());
        }
    } else {
        const auto taken = decode_pfor_block(payload.subspan(at), indices);
        if (!taken.has_value()) {
            return std::unexpected(rebased(taken.error(), indices_at));
        }
        if (indices.size() != count) {
            return std::unexpected(codec_error(CodecErrorKind::bad_count, "dict index count",
                                               indices_at, count, indices.size()));
        }
        if (const std::uint64_t highest = std::ranges::max(indices); highest >= *entries) {
            return std::unexpected(codec_error(CodecErrorKind::bad_field, "dict index", indices_at,
                                               *entries - 1U, highest));
        }
        at += *taken;
        if (at != payload.size()) {
            return std::unexpected(codec_error(CodecErrorKind::trailing_bits, "dict block",
                                               kPayloadStart + (at * 8U), 0,
                                               std::uint64_t{payload.size() - at} * 8U));
        }
    }

    if (as_mode == DictMode::sorted_move_to_front) {
        // Ranks to indices. Every rank is below K, and K is at most kDictMtfMaxEntries,
        // so an index fits 16 bits and a move shifts a quarter of the bytes 64 would.
        static_assert(kDictMtfMaxEntries <= (std::uint64_t{1} << 16U));
        static_assert(kDictMaxCodeBits <= BitReader::kMaxPeekBits);
        std::vector<std::uint16_t> list(entry_count);
        for (std::size_t i = 0; i < entry_count; ++i) {
            list[i] = static_cast<std::uint16_t>(i);
        }
        for (std::uint64_t& value : indices) {
            const auto rank = static_cast<std::ptrdiff_t>(value);
            const std::uint16_t index = list[static_cast<std::size_t>(rank)];
            std::move_backward(list.begin(), list.begin() + rank, list.begin() + rank + 1);
            list.front() = index;
            value = index;
        }
    }

    std::vector<std::uint64_t> used(entry_count, 0);
    for (const std::uint64_t index : indices) {
        ++used[index];
    }
    for (std::size_t i = 0; i < entry_count; ++i) {
        if (used[i] == 0) {
            return std::unexpected(
                codec_error(CodecErrorKind::bad_field, "dict entry unused", dictionary_at, i, 0));
        }
    }
    if (as_mode == DictMode::sorted_move_to_front) {
        for (std::size_t i = 1; i < entry_count; ++i) {
            if (dictionary[i] <= dictionary[i - 1]) {
                return std::unexpected(codec_error(CodecErrorKind::bad_field, "dict entry order",
                                                   dictionary_at, i - 1, i));
            }
        }
    } else {
        for (std::size_t i = 1; i < entry_count; ++i) {
            const bool commoner = used[i] > used[i - 1];
            const bool unsorted_tie = used[i] == used[i - 1] && dictionary[i] <= dictionary[i - 1];
            if (commoner || unsorted_tie) {
                return std::unexpected(codec_error(CodecErrorKind::bad_field, "dict entry order",
                                                   dictionary_at, i - 1, i));
            }
        }
        std::vector<std::uint64_t> distinct = dictionary;
        std::ranges::sort(distinct);
        if (std::ranges::adjacent_find(distinct) != distinct.end()) {
            return std::unexpected(
                codec_error(CodecErrorKind::bad_field, "dict entry repeated", dictionary_at, 0, 1));
        }
    }

    reserve_for_block(out, header->count);
    for (const std::uint64_t index : indices) {
        out.push_back(dictionary[index]);
    }
    return kBlockHeaderSize + header->payload_bytes;
}

std::expected<void, CodecError> encode_dict(std::span<const std::uint64_t> values,
                                            std::size_t block_values, std::vector<std::byte>& out,
                                            DictStats* stats) {
    if (const auto checked = check_block_values(block_values); !checked.has_value()) {
        return checked;
    }
    const std::size_t start = out.size();
    std::span<const std::uint64_t> previous;
    std::size_t previous_at = start;
    DictStats block_stats;
    for (std::size_t at = 0; at < values.size(); at += block_values) {
        const std::size_t size = std::min(values.size() - at, block_values);
        const std::span<const std::uint64_t> block = values.subspan(at, size);
        const std::size_t block_at = out.size();
        if (!previous.empty() && std::ranges::equal(block, previous)) {
            // Copied by position: inserting a vector's own range into it is undefined.
            const std::size_t bytes = block_at - previous_at;
            out.resize(block_at + bytes);
            std::copy_n(out.begin() + static_cast<std::ptrdiff_t>(previous_at), bytes,
                        out.begin() + static_cast<std::ptrdiff_t>(block_at));
            if (stats != nullptr) {
                ++stats->repeated;
            }
        } else {
            block_stats = DictStats{};
            if (auto encoded = encode_dict_block(block, out, &block_stats); !encoded.has_value()) {
                out.resize(start);
                return encoded;
            }
        }
        if (stats != nullptr) {
            add_block(*stats, block_stats);
        }
        previous = block;
        previous_at = block_at;
    }
    return {};
}

std::expected<void, CodecError> decode_dict(std::span<const std::byte> column, std::uint64_t count,
                                            std::vector<std::uint64_t>& out) {
    return decode_column(column, count, out, &decode_dict_block);
}

}  // namespace tick_store::codec
