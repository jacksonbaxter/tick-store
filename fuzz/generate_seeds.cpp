// Writes the seed corpora, generated at build time and never committed:
//
//   <dir>/message/  payloads, for fuzz_itch_message
//   <dir>/frame/    framed streams, for fuzz_itch_frame
//   <dir>/store/    stub store files, for fuzz_stub_store
//   <dir>/columns/  column directories as sections, for fuzz_columns
//   <dir>/gorilla_timestamps/  timestamp columns, for fuzz_gorilla_timestamps
//   <dir>/gorilla_xor/         XOR columns, for fuzz_gorilla_xor
//   <dir>/pfor/                PFOR columns in every option set, for fuzz_pfor
//   <dir>/stream_vbyte/        Stream VByte columns, for fuzz_stream_vbyte
//
// Every valid seed comes from the real encoder or the real store writer, so the
// seeds follow the code (Z4). Corrupt seeds are those same bytes cut, extended or
// relabelled, one for each way the decoders and the parser can refuse an input.

#include "codec_oracles.hpp"
#include "columns_oracles.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"
#include "tick_store/store/stub_store.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace tick_store::itch;
using Bytes = std::vector<std::byte>;

template <std::size_t N>
consteval Alpha<N> alpha(std::string_view text) {
    Alpha<N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

constexpr Stock kStock = alpha<8>("ZVZZT   ");
constexpr Mpid kMpid = alpha<4>("NSDQ");
constexpr MessageHeader kHeader{0x0102, 0x0304, 34'200'000'000'123ULL};

constexpr std::byte ch(char c) { return static_cast<std::byte>(c); }

// One ordinary message of each of the 23 types.
std::vector<Message> ordinary_messages() {
    return {
        SystemEvent{kHeader, ch('O')},
        StockDirectory{kHeader, kStock, ch('Q'), ch('N'), 100, ch('N'), ch('C'), alpha<2>("Z "),
                       ch('P'), ch('N'), ch(' '), ch('1'), ch('N'), 0, ch('N')},
        StockTradingAction{kHeader, kStock, ch('T'), ch(' '), alpha<4>("    ")},
        RegShoRestriction{kHeader, kStock, ch('0')},
        MarketParticipantPosition{kHeader, kMpid, kStock, ch('Y'), ch('N'), ch('A')},
        MwcbDeclineLevel{kHeader, Price8{320'000'000'000}, Price8{300'000'000'000},
                         Price8{280'000'000'000}},
        MwcbStatus{kHeader, ch('1')},
        IpoQuotingPeriodUpdate{kHeader, kStock, 34'200, ch('A'), Price4{120'000}},
        LuldAuctionCollar{kHeader, kStock, Price4{100'000}, Price4{105'000}, Price4{95'000}, 1},
        OperationalHalt{kHeader, kStock, ch('Q'), ch('H')},
        AddOrder{kHeader, 1'000, ch('B'), 100, kStock, Price4{123'400}},
        AddOrderWithMpid{kHeader, 1'001, ch('S'), 200, kStock, Price4{123'500}, kMpid},
        OrderExecuted{kHeader, 1'000, 50, 9'000},
        OrderExecutedWithPrice{kHeader, 1'000, 50, 9'001, ch('Y'), Price4{123'450}},
        OrderCancel{kHeader, 1'001, 100},
        OrderDelete{kHeader, 1'001},
        OrderReplace{kHeader, 1'000, 1'002, 100, Price4{123'600}},
        Trade{kHeader, 0, ch('B'), 300, kStock, Price4{123'400}, 9'002},
        CrossTrade{kHeader, 1'000'000, kStock, Price4{123'400}, 9'003, ch('O')},
        BrokenTrade{kHeader, 9'003},
        NetOrderImbalance{kHeader, 1'000, 500, ch('B'), kStock, Price4{123'000}, Price4{123'100},
                          Price4{123'200}, ch('O'), ch('L')},
        RetailInterest{kHeader, kStock, ch('B')},
        DirectListingPriceDiscovery{kHeader, kStock, ch('Y'), Price4{100'000}, Price4{200'000},
                                    Price4{150'000}, 1'000, Price4{135'000}, Price4{165'000}},
    };
}

Bytes encoded(const Message& message) {
    Bytes out(kMaxPayloadSize);
    const auto written = encode(message, out);
    if (!written.has_value()) {
        throw std::runtime_error(to_string(written.error()));
    }
    out.resize(*written);
    return out;
}

Bytes filled(std::byte type, std::size_t size, std::byte fill) {
    Bytes out(size, fill);
    out.front() = type;
    return out;
}

Bytes frame(const Bytes& payload) {
    Bytes out{static_cast<std::byte>(payload.size() >> 8U),
              static_cast<std::byte>(payload.size() & 0xFFU)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void write_seed(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    for (const std::byte b : bytes) {
        file.put(static_cast<char>(b));
    }
    if (!file) {
        throw std::runtime_error(std::format("cannot write {}", path.string()));
    }
}

class SeedDir {
public:
    explicit SeedDir(std::filesystem::path dir) : dir_(std::move(dir)) {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void add(std::span<const std::byte> bytes) {
        write_seed(dir_ / std::format("seed-{:05}", next_++), bytes);
    }

    [[nodiscard]] std::size_t count() const noexcept { return next_; }

private:
    std::filesystem::path dir_;
    std::size_t next_ = 0;
};

// Valid payloads and each way a payload can be refused.
void write_message_seeds(SeedDir& seeds) {
    for (const Message& message : ordinary_messages()) {
        const Bytes valid = encoded(message);
        seeds.add(valid);

        // Field extremes, on the wire as all-zero and all-ones bodies.
        for (const std::byte fill : {std::byte{0x00}, std::byte{0xFF}}) {
            seeds.add(filled(valid.front(), valid.size(), fill));
        }

        // A cut at every length is a truncated error for each field in turn.
        for (std::size_t length = 1; length < valid.size(); ++length) {
            seeds.add(std::span{valid}.first(length));
        }

        // Trailing bytes.
        Bytes longer = valid;
        longer.push_back(std::byte{0x5A});
        seeds.add(longer);
    }

    // Each unused byte value as a type, so the whitelist's edges are in the corpus.
    for (unsigned type = 0; type <= std::numeric_limits<std::uint8_t>::max(); ++type) {
        const auto byte = static_cast<std::byte>(type);
        if (!wire_size_of(byte).has_value()) {
            seeds.add(filled(byte, 12, std::byte{0x20}));
        }
    }

    // The timestamp's top value, and the empty payload.
    OrderDelete top;
    top.header.timestamp_ns = kMaxTimestampNs;
    top.order_reference_number = std::numeric_limits<std::uint64_t>::max();
    seeds.add(encoded(top));
    seeds.add(Bytes{});
}

// Streams: whole days in miniature, and each way a cursor can stop.
void write_frame_seeds(SeedDir& seeds) {
    Bytes day;
    for (const Message& message : ordinary_messages()) {
        const Bytes one = frame(encoded(message));
        seeds.add(one);
        day.insert(day.end(), one.begin(), one.end());
    }
    seeds.add(day);

    // Cut at the start of a prefix, inside a prefix, and inside a body.
    const std::size_t first_frame = frame(encoded(ordinary_messages().front())).size();
    for (const std::size_t cut : {first_frame, first_frame + 1, first_frame + 2, first_frame + 5}) {
        seeds.add(std::span{day}.first(cut));
    }

    // Zero-length frames: alone, ending a stream, and with bytes after them.
    const Bytes zero{std::byte{0}, std::byte{0}};
    seeds.add(zero);
    Bytes ended = day;
    ended.insert(ended.end(), zero.begin(), zero.end());
    seeds.add(ended);
    ended.push_back(std::byte{0x41});
    seeds.add(ended);

    // Frames through the raw path, inside a stream.
    Bytes mixed = frame(filled(std::byte{'z'}, 9, std::byte{0xA5}));
    const Bytes short_add = frame(filled(AddOrder::type_code, 20, std::byte{0}));
    mixed.insert(mixed.end(), short_add.begin(), short_add.end());
    seeds.add(mixed);

    // The largest frame, and a prefix promising more than the input holds.
    seeds.add(frame(filled(std::byte{'z'}, kMaxPayloadSize, std::byte{0x5A})));
    seeds.add(Bytes{std::byte{0xFF}, std::byte{0xFF}, std::byte{0x41}});

    // A single byte, which is less than a prefix, and nothing at all.
    seeds.add(Bytes{std::byte{0x00}});
    seeds.add(Bytes{});
}

// A file from the real writer: the 23 types, two raw-path records and `tail`.
Bytes written_store(const std::filesystem::path& scratch, const std::vector<Message>& messages,
                    std::span<const std::byte> tail) {
    auto writer = tick_store::store::StubWriter::create(scratch);
    if (!writer.has_value()) {
        throw std::runtime_error(to_string(writer.error()));
    }
    for (const Message& message : messages) {
        if (const auto appended = writer->append(message); !appended.has_value()) {
            throw std::runtime_error(to_string(appended.error()));
        }
    }
    if (const auto finished = writer->finish(tail); !finished.has_value()) {
        throw std::runtime_error(to_string(finished.error()));
    }
    std::ifstream file{scratch, std::ios::binary};
    Bytes bytes;
    for (auto it = std::istreambuf_iterator<char>{file}; it != std::istreambuf_iterator<char>{};
         ++it) {
        bytes.push_back(static_cast<std::byte>(*it));
    }
    std::filesystem::remove(scratch);
    return bytes;
}

// `file` with one header field overwritten.
template <std::size_t Width>
Bytes with_field(Bytes file, std::size_t offset, std::uint64_t value) {
    if (!tick_store::store::write_le<Width>(file, offset, value)) {
        throw std::runtime_error("a header field is outside the file");
    }
    return file;
}

// Valid stores, and one corrupt seed for each check in the parser.
void write_store_seeds(SeedDir& seeds, const std::filesystem::path& scratch) {
    const Bytes unknown = filled(std::byte{'z'}, 9, std::byte{0xA5});
    const Bytes short_add = filled(AddOrder::type_code, 20, std::byte{0});

    std::vector<Message> messages = ordinary_messages();
    messages.emplace_back(RawMessage{.bytes = unknown, .reason = RawReason::unknown_type});
    messages.emplace_back(RawMessage{.bytes = short_add, .reason = RawReason::wrong_length});

    const std::array tail{std::byte{0x00}, std::byte{0x7F}, std::byte{0xAB}};
    const Bytes store = written_store(scratch, messages, tail);
    seeds.add(store);
    seeds.add(written_store(scratch, messages, {}));
    seeds.add(written_store(scratch, {}, {}));
    seeds.add(written_store(scratch, {}, tail));

    // Each header check, in the order open() runs them. Offsets are in stub_store.hpp.
    const std::uint64_t records_bytes =
        store.size() - tick_store::store::kStubHeaderSize - tail.size();
    seeds.add(with_field<1>(store, 0, 'X'));
    seeds.add(with_field<4>(store, 8, 2));
    seeds.add(with_field<4>(store, 12, 41));
    seeds.add(with_field<8>(store, 16, messages.size() + 1));
    seeds.add(with_field<8>(store, 16, 0));
    seeds.add(with_field<8>(store, 24, records_bytes + tail.size() + 1));
    seeds.add(with_field<8>(store, 24, records_bytes - 1));
    seeds.add(with_field<8>(store, 32, tail.size() + 1));
    seeds.add(with_field<8>(store, 16, std::numeric_limits<std::uint64_t>::max()));

    // Sections that fill the file but hold a half prefix, a cut body, a zero length.
    const std::array half_prefix{std::byte{0x00}};
    const std::array cut_body{std::byte{0x00}, std::byte{0x05}, std::byte{'S'}};
    const std::array zero_length{std::byte{0x00}, std::byte{0x00}};
    for (const std::span<const std::byte> records :
         {std::span<const std::byte>{half_prefix}, std::span<const std::byte>{cut_body},
          std::span<const std::byte>{zero_length}}) {
        const auto header = tick_store::store::encode_stub_header(
            {.record_count = 1, .records_bytes = records.size(), .tail_bytes = 0});
        Bytes bad(header.begin(), header.end());
        bad.insert(bad.end(), records.begin(), records.end());
        seeds.add(bad);
    }

    // Cut inside the header, at its end, in the records and in the tail.
    for (const std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{39}, std::size_t{40},
                                  std::size_t{41}, std::size_t{55}, store.size() - 1}) {
        seeds.add(std::span{store}.first(cut));
    }
}

// A directory from the real encoder holding `payloads`, as fuzz_columns' sections.
Bytes columns_input(const std::vector<Bytes>& payloads, std::span<const std::byte> tail) {
    tick_store::store::ColumnsEncoder encoder;
    for (const Bytes& payload : payloads) {
        if (const auto added = encoder.append(payload); !added.has_value()) {
            throw std::runtime_error(to_string(added.error()));
        }
    }
    auto buffers = std::move(encoder.buffers());
    buffers[tick_store::store::kColumnFileCount - 1].assign(tail.begin(), tail.end());
    return tick_store::store::oracle::pack_columns(
        tick_store::store::format_columns_manifest(encoder.manifest(tail.size())), buffers);
}

// Valid directories, and one corrupt seed for each kind of check in the parser. The
// hand-picked corruptions stand in for what mutation is slow to find: anything that has
// to stay consistent across two sections.
// A rare row, then two hot rows, with the text offset between the hot rows moved past the end
// of rest.dat. Hot rows hold no text, so the first of them starts exactly at the file's end, and
// the parser's bound on the offset is all that stops it reading on. A mutation is unlikely to
// find this, because every other seed with two rows ends in text.
Bytes text_offset_past_its_file(const std::vector<Bytes>& payloads) {
    const auto is_type = [](std::byte type) {
        return [type](const Bytes& payload) { return payload.front() == type; };
    };
    const auto rare = std::ranges::find_if(payloads, is_type(SystemEvent::type_code));
    const auto hot = std::ranges::find_if(payloads, is_type(AddOrder::type_code));
    if (rare == payloads.end() || hot == payloads.end()) {
        throw std::runtime_error("a seed payload is missing");
    }
    Bytes seed = columns_input({*rare, *hot, *hot}, {});
    const auto sections = tick_store::store::oracle::split_columns(seed);
    if (!sections.has_value()) {
        throw std::runtime_error("a seed did not split");
    }
    constexpr std::size_t kRestOffsets =
        1 + static_cast<std::size_t>(tick_store::store::ColumnFile::rest_off);
    constexpr std::size_t kRestBytes =
        1 + static_cast<std::size_t>(tick_store::store::ColumnFile::rest_dat);
    constexpr std::size_t kSecondHotRow = 2;
    const auto at = static_cast<std::size_t>((*sections)[kRestOffsets].data() - seed.data());
    const std::uint64_t past_the_end = (*sections)[kRestBytes].size() + 8;
    if (!tick_store::store::write_le<8>(seed, at + (kSecondHotRow * 8), past_the_end)) {
        throw std::runtime_error("a seed was too short");
    }
    return seed;
}

void write_columns_seeds(SeedDir& seeds) {
    std::vector<Bytes> payloads;
    for (const Message& message : ordinary_messages()) {
        payloads.push_back(encoded(message));
    }
    payloads.push_back(filled(std::byte{'z'}, 9, std::byte{0xA5}));
    payloads.push_back(filled(AddOrder::type_code, 20, std::byte{0}));
    payloads.push_back(filled(AddOrder::type_code, AddOrder::wire_size, std::byte{0xFF}));

    const std::array tail{std::byte{0x00}, std::byte{0x7F}, std::byte{0xAB}};
    const Bytes whole = columns_input(payloads, tail);
    seeds.add(whole);
    seeds.add(columns_input(payloads, {}));
    seeds.add(columns_input({}, {}));
    seeds.add(columns_input({}, tail));
    for (const Bytes& payload : payloads) {
        seeds.add(columns_input({payload}, {}));
    }

    // Cuts through a section header, a manifest and a file, then nothing at all.
    for (const std::size_t cut : {std::size_t{0}, std::size_t{3}, std::size_t{4}, std::size_t{40},
                                  std::size_t{120}, whole.size() - 1}) {
        seeds.add(std::span{whole}.first(cut));
    }

    seeds.add(text_offset_past_its_file(payloads));

    // One byte flipped in each section, starting at its first byte.
    auto sections = tick_store::store::oracle::split_columns(whole);
    if (!sections.has_value()) {
        throw std::runtime_error("a seed did not split");
    }
    std::size_t section_start = 0;
    for (const std::span<const std::byte> section : *sections) {
        section_start += 4;
        if (!section.empty()) {
            Bytes flipped = whole;
            flipped[section_start] ^= std::byte{0x01};
            seeds.add(flipped);
        }
        section_start += section.size();
    }
}


// SplitMix64, so the generated values are the same on every build and host.
class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t operator()() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

private:
    std::uint64_t state_;
};

using Words = std::vector<std::uint64_t>;
using ColumnEncoder = std::expected<void, tick_store::codec::CodecError> (*)(
    std::span<const std::uint64_t>, std::size_t, std::vector<std::byte>&);

Bytes column_of(ColumnEncoder encode, const Words& values, std::size_t block_values) {
    Bytes out;
    if (const auto done = encode(values, block_values, out); !done.has_value()) {
        throw std::runtime_error(tick_store::codec::to_string(done.error()));
    }
    return out;
}

std::expected<void, tick_store::codec::CodecError> timestamps_column(
    std::span<const std::uint64_t> values, std::size_t block_values, std::vector<std::byte>& out) {
    return tick_store::codec::encode_timestamps(values, block_values, out);
}

std::expected<void, tick_store::codec::CodecError> xor_column(
    std::span<const std::uint64_t> values, std::size_t block_values, std::vector<std::byte>& out) {
    return tick_store::codec::encode_xor(values, block_values, out);
}

// Each column at a few block sizes, then cut through its first header, its first payload
// and its last byte.
void add_columns(SeedDir& seeds, ColumnEncoder encode, const std::vector<Words>& columns) {
    for (const Words& values : columns) {
        for (const std::size_t block_values : {std::size_t{1}, std::size_t{3}, std::size_t{4096}}) {
            const Bytes column = column_of(encode, values, block_values);
            seeds.add(column);
            if (block_values == 3 && !column.empty()) {
                for (const std::size_t cut : {std::size_t{1}, std::size_t{7}, std::size_t{9},
                                              column.size() / 2, column.size() - 1}) {
                    seeds.add(std::span{column}.first(std::min(cut, column.size())));
                }
            }
        }
    }
}

// A block: a count, a payload size that fits what follows, a first word, then `bits` as
// a string of 0 and 1, zero-padded to a whole byte.
Bytes block_from_bits(std::uint32_t count, std::uint64_t first, std::string_view bits) {
    Bytes payload;
    for (unsigned i = 8; i > 0; --i) {
        payload.push_back(static_cast<std::byte>((first >> (8U * (i - 1U))) & 0xFFU));
    }
    unsigned filled = 0;
    for (const char bit : bits) {
        if (filled % 8 == 0) {
            payload.push_back(std::byte{0});
        }
        if (bit == '1') {
            payload.back() |= static_cast<std::byte>(0x80U >> (filled % 8U));
        }
        ++filled;
    }
    Bytes block;
    const std::size_t at = tick_store::codec::begin_block(block, count);
    tick_store::codec::patch_block_header(block, at, static_cast<std::uint32_t>(payload.size()));
    block.insert(block.end(), payload.begin(), payload.end());
    return block;
}

std::string bits_of(std::uint64_t value, unsigned width) {
    std::string out;
    for (unsigned i = width; i > 0; --i) {
        out += ((value >> (i - 1U)) & 1U) != 0 ? '1' : '0';
    }
    return out;
}

// A valid block with one header field changed: the count to zero, past the cap and past
// what the payload holds, and the payload size one past the bytes there.
void add_header_corruptions(SeedDir& seeds, const Bytes& block) {
    const auto set = [&](std::size_t offset, std::uint64_t value) {
        Bytes changed = block;
        if (!tick_store::store::write_le<4>(changed, offset, value)) {
            throw std::runtime_error("a seed block is shorter than its header");
        }
        seeds.add(changed);
    };
    set(0, 0);
    set(0, tick_store::codec::kMaxBlockValues + 1U);
    set(0, tick_store::codec::kMaxBlockValues);
    set(4, block.size() - tick_store::codec::kBlockHeaderSize + 1U);
    set(4, 0xFFFF'FFFFU);
    Bytes spare = block;
    spare.push_back(std::byte{0});
    tick_store::codec::patch_block_header(spare, 0, static_cast<std::uint32_t>(
                                                        spare.size() -
                                                        tick_store::codec::kBlockHeaderSize));
    seeds.add(spare);
}

// Timestamps whose delta-of-deltas are `dods`.
Words from_dods(std::uint64_t first, const Words& dods) {
    Words values{first};
    std::uint64_t delta = 0;
    for (const std::uint64_t dod : dods) {
        delta += dod;
        values.push_back(values.back() + delta);
    }
    return values;
}

void write_timestamp_seeds(SeedDir& seeds) {
    using tick_store::codec::kDodBucketBits;
    Words edges;
    for (std::size_t b = 0; b + 1 < kDodBucketBits.size(); ++b) {
        const std::uint64_t half = std::uint64_t{1} << (kDodBucketBits[b] - 1U);
        for (const std::uint64_t dod : {half - 2, half - 1, half, half + 1}) {
            edges.push_back(dod);
            edges.push_back(std::uint64_t{0} - dod);
        }
    }
    edges.push_back(std::uint64_t{1} << 63U);
    edges.push_back((std::uint64_t{1} << 63U) - 1U);

    SplitMix64 random{2019};
    Words itch_like{34'200'000'000'000ULL};
    for (int i = 0; i < 3000; ++i) {
        const std::uint64_t pick = random() % 100;
        // Mostly gaps of tens of microseconds, some repeats, some long jumps.
        std::uint64_t gap = random() % 60'000;
        if (pick < 2) {
            gap = 0;
        } else if (pick < 4) {
            gap = random() % (1ULL << 30U);
        }
        itch_like.push_back(itch_like.back() + gap);
    }
    const Words extremes{0, ~std::uint64_t{0}, 0, 1, std::uint64_t{1} << 63U, 7, 7, 7};
    add_columns(seeds, &timestamps_column,
                {from_dods(0x123456789ABCULL, {5, 0, 40'000, 0, 3'000'000, 1ULL << 40U,
                                               ~std::uint64_t{0}}),
                 from_dods(1'000'000, edges), itch_like, extremes, {0xFFFF'FFFF'FFFFULL}});

    const Bytes block = column_of(&timestamps_column, from_dods(100, {1, 2, 3}), 4096);
    add_header_corruptions(seeds, block);

    // Non-canonical buckets, a cut control code and field, and set padding.
    for (const std::string& bits :
         {"10" + bits_of(0, 16), "110" + bits_of(5, 19), "1110" + bits_of(262'143, 23),
          "1111" + bits_of(4'194'303, 64), "1111" + bits_of(0, 64), std::string{"10000000"},
          std::string{"00000001"}, std::string{"000000000"}}) {
        seeds.add(block_from_bits(2, 34'200'000'000'000ULL, bits));
    }
    seeds.add(block_from_bits(9, 0, "00000001"));
    seeds.add(Bytes{});
}

std::string new_window(unsigned leading, unsigned bits, std::uint64_t data) {
    return "11" + bits_of(leading, 5) + bits_of(bits - 1U, 6) + bits_of(data, bits);
}

void write_xor_seeds(SeedDir& seeds) {
    using tick_store::codec::price4_to_dollars;
    using tick_store::codec::scaled_to_double;
    SplitMix64 random{2020};
    Words dollars;
    Words integers;
    Words as_doubles;
    std::uint32_t price = 500'000;
    for (int i = 0; i < 3000; ++i) {
        const std::uint64_t step = random() % 11;
        price = price + (static_cast<std::uint32_t>(step) * 100U) - 500U;
        // Now and then a sub-dollar price or a stub quote, as ITCH has.
        std::uint32_t value = price;
        if (i % 97 == 0) {
            value = 1'999'999'900U;
        } else if (i % 89 == 0) {
            value = 4'321U;
        }
        dollars.push_back(std::bit_cast<std::uint64_t>(price4_to_dollars(value)));
        integers.push_back(value);
        as_doubles.push_back(std::bit_cast<std::uint64_t>(scaled_to_double(value, 1)));
    }
    const Words specials{
        std::bit_cast<std::uint64_t>(0.0),      std::bit_cast<std::uint64_t>(-0.0),
        0x7FF8000000000001ULL,                  0xFFF0000000000001ULL,
        0x7FF0000000000000ULL,                  1,
        std::bit_cast<std::uint64_t>(3200.0),   std::bit_cast<std::uint64_t>(0.0001),
    };
    const Words golden{0x4059000000000000ULL, 0x4059000000000000ULL, 0x4059400000000000ULL,
                       0x4059000000000000ULL, 0x4059000000000001ULL, 0xBFA6FFFFFFFFFFFEULL,
                       0x3FA6FFFFFFFFFFFFULL};
    const Words windows{0, ~std::uint64_t{0}, 0, std::uint64_t{1} << 63U, 0, 1, 0,
                        std::uint64_t{1} << 32U, 0, std::uint64_t{1} << 31U, 0,
                        0x00F0000000000000ULL, 0x0060000000000000ULL, 0x01E0000000000000ULL};
    add_columns(seeds, &xor_column, {golden, windows, specials, dollars, integers, as_doubles});

    const Bytes block = column_of(&xor_column, golden, 4096);
    add_header_corruptions(seeds, block);

    // Each refusal of the decoder: D4's two window checks, the four canonical checks, a
    // field cut short, and set padding.
    for (const auto& [count, bits] : std::vector<std::pair<std::uint32_t, std::string>>{
             {2, "10" + bits_of(1, 6)},
             {2, "11" + bits_of(31, 5) + bits_of(63, 6)},
             {2, new_window(20, 4, 0b1010)},
             {2, new_window(20, 4, 0b0101)},
             {3, new_window(24, 8, 0xFF) + new_window(25, 7, 0x41)},
             {3, new_window(28, 4, 0b1001) + "10" + "0000"},
             {6, "000011"},
             {2, "11" + bits_of(0, 5) + bits_of(9, 6)},
             {2, "00000001"},
         }) {
        seeds.add(block_from_bits(count, 0, bits));
    }
    seeds.add(Bytes{});
}

// Values for the integer codecs: timestamps as ITCH has them, whole-cent prices with stub
// quotes, a regular clock, multiples of a scale with values off it, the 64-bit extremes,
// and each width edge over a full vector and a tail.
std::vector<Words> integer_value_sets() {
    SplitMix64 random{2021};
    Words itch_like{34'200'000'000'000ULL};
    for (int i = 0; i < 2500; ++i) {
        const std::uint64_t pick = random() % 100;
        std::uint64_t gap = random() % 60'000;
        if (pick < 3) {
            gap = 0;
        } else if (pick < 5) {
            gap = random() % (1ULL << 30U);
        }
        itch_like.push_back(itch_like.back() + gap);
    }
    Words prices;
    std::uint64_t price = 1'234'500;
    for (int i = 0; i < 2500; ++i) {
        price = price + ((random() % 11) * 100U) - 500U;
        std::uint64_t value = price;
        if (i % 97 == 0) {
            value = 1'999'999'900U;
        } else if (i % 89 == 0) {
            value = 4'321U;
        }
        prices.push_back(value);
    }
    Words regular;
    for (std::uint64_t i = 0; i < 1'100; ++i) {
        regular.push_back(34'200'000'000'000ULL + (i * 25'000));
    }
    Words sevens;
    for (std::uint64_t i = 0; i < 300; ++i) {
        sevens.push_back((i % 41 == 0) ? (i * 7) + 3 : (500 + (random() % 50)) * 7);
    }
    constexpr std::uint64_t kTop = ~std::uint64_t{0};
    Words threes;
    for (std::uint64_t k = 1; k <= 8; ++k) {
        threes.push_back(kTop - (3 * k));
    }
    threes.insert(threes.end(), {kTop - 1, kTop});
    std::vector<Words> sets{itch_like, prices, regular, sevens, threes,
                            {0, kTop, 0, 1, std::uint64_t{1} << 63U, 7, 7, 7},
                            {5, 300, 300, 70'000, (std::uint64_t{1} << 40U) + 70'000},
                            {0, 255, 256, 65'535, 65'536, (1ULL << 24U) - 1, 1ULL << 24U,
                             (1ULL << 32U) - 1, 1ULL << 32U, kTop}};
    for (const unsigned width : {0U, 1U, 7U, 8U, 31U, 32U, 33U, 63U, 64U}) {
        const std::uint64_t top = width == 64 ? kTop : (std::uint64_t{1} << width) - 1U;
        Words edge(1'025);
        for (std::size_t i = 0; i < edge.size(); ++i) {
            edge[i] = i % 2 == 0 ? 0 : top;
        }
        // One exception in the full vector, so patching has something to patch.
        edge[777] = kTop;
        sets.push_back(edge);
    }
    return sets;
}

// A small valid block with each byte from the payload's start set to 0, to 0xFF and to
// one more, so every field's refusal has a seed one change from the encoder's bytes.
void add_byte_corruptions(SeedDir& seeds, const Bytes& block) {
    constexpr std::size_t kFirstBytes = 48;
    for (std::size_t at = tick_store::codec::kBlockHeaderSize;
         at < std::min(block.size(), tick_store::codec::kBlockHeaderSize + kFirstBytes); ++at) {
        const auto one_more = static_cast<std::byte>(std::to_integer<unsigned>(block[at]) + 1U);
        for (const std::byte value : {std::byte{0}, std::byte{0xFF}, one_more}) {
            Bytes changed = block;
            changed[at] = value;
            if (changed != block) {
                seeds.add(changed);
            }
        }
    }
}

// A Stream VByte block of deltas 5, 295, 0, 69,700 and 2^40, built by hand, and one
// change to it for each refusal the decoder makes, as in stream_vbyte_test.cpp.
void add_svb_refusals(SeedDir& seeds) {
    const std::vector<std::uint8_t> golden{0x01, 0x84, 0x00, 0x05, 0x27, 0x01, 0x00, 0x44, 0x10,
                                           0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x09, 0x03, 0x04,
                                           0x00, 0x01};
    const auto block = [&seeds](std::vector<std::uint8_t> payload) {
        Bytes bytes(payload.size());
        std::ranges::transform(payload, bytes.begin(),
                               [](std::uint8_t b) { return static_cast<std::byte>(b); });
        Bytes framed;
        const std::size_t at = tick_store::codec::begin_block(framed, 5);
        tick_store::codec::patch_block_header(framed, at, static_cast<std::uint32_t>(bytes.size()));
        framed.insert(framed.end(), bytes.begin(), bytes.end());
        seeds.add(framed);
    };
    block(golden);
    const auto with = [&](std::size_t at, std::uint8_t value) {
        std::vector<std::uint8_t> changed = golden;
        changed[at] = value;
        block(changed);
    };
    with(0, 5);       // an unknown flag
    with(2, 0x04);    // a code past the last value
    with(11, 6);      // more highs than values
    with(15, 0);      // a high width of 0
    with(15, 33);     // and of 33
    with(15, 10);     // and wider than the highest needs
    with(16, 4);      // a gap width past what 5 positions can need
    with(17, 5);      // a position past the last value
    with(19, 0);      // a high part of 0
    // 5 in two bytes, a length longer than its value needs.
    block({0x01, 0x85, 0x00, 0x05, 0x00, 0x27, 0x01, 0x00, 0x44, 0x10, 0x01, 0x00, 0x01, 0x00,
           0x00, 0x00, 0x09, 0x03, 0x04, 0x00, 0x01});
    // Zigzag set on values plain codes no larger: the encoder takes plain.
    block({0x03, 0x84, 0x00, 0x0A, 0x4E, 0x02, 0x00, 0x88, 0x20, 0x02, 0x00, 0x01, 0x00,
           0x00, 0x00, 0x0A, 0x03, 0x04, 0x00, 0x02});
}

void write_integer_seeds(SeedDir& seeds, std::span<const tick_store::codec::oracle::Encoder> encoders) {
    const std::vector<Words> sets = integer_value_sets();
    const Words small{1000, 1001, 1003, 1002, 999'999, 1004, 1005, 1006,
                      50'000, 50'300, 49'700, 51'000, 50'500, 49'937, 50'100, 50'200};
    for (const auto& encoder : encoders) {
        add_columns(seeds, encoder.encode_column, sets);
        const Bytes block = column_of(encoder.encode_column, small, 4096);
        add_header_corruptions(seeds, block);
        add_byte_corruptions(seeds, block);
    }
    seeds.add(Bytes{});
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): a failed build step should terminate and say why.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: generate_seeds <output directory>");
        return 2;
    }
    const std::span args{argv, static_cast<std::size_t>(argc)};
    const std::filesystem::path root{args[1]};
    SeedDir message{root / "message"};
    SeedDir stream{root / "frame"};
    SeedDir store{root / "store"};
    SeedDir columns{root / "columns"};
    SeedDir timestamps{root / "gorilla_timestamps"};
    SeedDir xor_words{root / "gorilla_xor"};
    SeedDir pfor{root / "pfor"};
    SeedDir svb{root / "stream_vbyte"};
    write_message_seeds(message);
    write_frame_seeds(stream);
    write_store_seeds(store, root / "store-scratch");
    write_columns_seeds(columns);
    write_timestamp_seeds(timestamps);
    write_xor_seeds(xor_words);
    write_integer_seeds(pfor, tick_store::codec::oracle::detail::kPforEncoders);
    write_integer_seeds(svb, tick_store::codec::oracle::detail::kSvbEncoders);
    add_svb_refusals(svb);
    std::println("wrote {} message, {} frame, {} store, {} columns, {} gorilla_timestamps, "
                 "{} gorilla_xor, {} pfor and {} stream_vbyte seeds",
                 message.count(), stream.count(), store.count(), columns.count(),
                 timestamps.count(), xor_words.count(), pfor.count(), svb.count());
    return 0;
}
