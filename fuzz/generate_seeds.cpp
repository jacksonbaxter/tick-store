// Writes the seed corpora, generated at build time and never committed:
//
//   <dir>/message/  payloads, for fuzz_itch_message
//   <dir>/frame/    framed streams, for fuzz_itch_frame
//   <dir>/store/    stub store files, for fuzz_stub_store
//   <dir>/container/  container files from the real writer, for fuzz_container
//   <dir>/itch_store/  ITCH store files from the real writer, for fuzz_itch_store
//   <dir>/itch_query/  ITCH store files over several seek groups, for fuzz_itch_query
//   <dir>/columns/  column directories as sections, for fuzz_columns
//   <dir>/gorilla_timestamps/  timestamp columns, for fuzz_gorilla_timestamps
//   <dir>/gorilla_xor/         XOR columns, for fuzz_gorilla_xor
//   <dir>/pfor/                PFOR columns in every option set, for fuzz_pfor
//   <dir>/stream_vbyte/        Stream VByte columns, for fuzz_stream_vbyte
//   <dir>/dictionary/          dictionary columns in each mode, for fuzz_dictionary
//   <dir>/run_length/          run-length columns, for fuzz_run_length
//
// Every valid seed comes from the real encoder or the real store writer, so the
// seeds follow the code (Z4). Corrupt seeds are those same bytes cut, extended or
// relabelled, one for each way the decoders and the parser can refuse an input.

#include "codec_oracles.hpp"
#include "columns_oracles.hpp"
#include "container_oracles.hpp"
#include "itch_query_oracles.hpp"
#include "itch_store_oracles.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/codec/registry.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/zstd_frame.hpp"
#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/columns.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/container_writer.hpp"
#include "tick_store/store/crc32c.hpp"
#include "tick_store/store/durability.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_writer.hpp"
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
#include <functional>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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

// An ITCH store from the real writer, in memory.
Bytes written_itch_store(std::span<const Message> messages, std::span<const std::byte> tail,
                         std::uint32_t segment_records) {
    auto writer = tick_store::store::ItchStoreWriter::in_memory({.segment_records = segment_records});
    if (!writer.has_value()) {
        throw std::runtime_error(to_string(writer.error()));
    }
    for (const Message& message : messages) {
        if (auto added = writer->append(message); !added.has_value()) {
            throw std::runtime_error(to_string(added.error()));
        }
    }
    if (auto finished = writer->finish(tail); !finished.has_value()) {
        throw std::runtime_error(to_string(finished.error()));
    }
    return writer->take_bytes();
}

// Orders created, named, replaced and deleted on a few locates, with a stock exception
// and an order named under the wrong locate, so segments hold creations, distances and
// imports.
std::vector<Message> book_messages() {
    std::vector<Message> out;
    std::uint64_t state = 7;
    std::vector<std::uint64_t> live;
    std::uint64_t next = 5'000;
    for (std::uint64_t t = 1; t <= 200; ++t) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto locate = static_cast<std::uint16_t>(1 + ((state >> 40U) % 3));
        const MessageHeader h{locate, static_cast<std::uint16_t>(t % 5), 34'200'000'000'000ULL + (t * 997)};
        const std::uint64_t pick = (state >> 20U) % 8;
        if (live.empty() || pick < 3) {
            const Stock stock = t == 50 ? alpha<8>("OTHER   ") : kStock;
            out.emplace_back(AddOrder{h, next, (state & 1U) != 0 ? ch('B') : ch('S'),
                                      static_cast<std::uint32_t>(100 * (1 + (state >> 60U))), stock,
                                      Price4{static_cast<std::uint32_t>(100'000 + ((state >> 48U) % 500))}});
            live.push_back(next++);
            continue;
        }
        const std::size_t at = (state >> 8U) % live.size();
        if (pick < 5) {
            out.emplace_back(OrderExecuted{h, live[at], 10, 90'000 + t});
        } else if (pick < 6) {
            out.emplace_back(OrderReplace{h, live[at], next, 100, Price4{100'100}});
            live[at] = next++;
        } else if (pick < 7) {
            out.emplace_back(OrderCancel{h, live[at], 5});
        } else {
            out.emplace_back(OrderDelete{h, live[at]});
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
    return out;
}

// A store file taken apart, one part changed, and rebuilt through build_container, so its
// CRCs hold and the store's own check is what refuses it.
Bytes with_itch_store_part(const Bytes& file,
                           const std::function<void(std::vector<tick_store::store::ContainerPart>&,
                                                    std::vector<tick_store::store::ContainerPart>&)>& edit) {
    auto parser = tick_store::store::ContainerParser::open(file);
    if (!parser.has_value()) {
        throw std::runtime_error(to_string(parser.error()));
    }
    std::vector<tick_store::store::ContainerPart> blocks;
    std::vector<tick_store::store::ContainerPart> sections;
    for (std::uint64_t b = 0; b < parser->block_count(); ++b) {
        const auto view = parser->block(b);
        blocks.push_back({.section_name = "", .codec = view->codec,
                          .metadata = Bytes(view->metadata.begin(), view->metadata.end()),
                          .bytes = Bytes(view->payload.begin(), view->payload.end())});
    }
    for (std::uint64_t i = 0; i < parser->section_count(); ++i) {
        const auto info = parser->section_info(i);
        const auto view = parser->section(info->name);
        sections.push_back({.section_name = std::string{info->name},
                            .codec = tick_store::codec::CodecId::raw, .metadata = {},
                            .bytes = Bytes(view->bytes.begin(), view->bytes.end())});
    }
    edit(blocks, sections);
    std::vector<tick_store::store::ContainerPart> parts = blocks;
    parts.insert(parts.end(), sections.begin(), sections.end());
    auto rebuilt = tick_store::store::build_container(parts);
    if (!rebuilt.has_value()) {
        throw std::runtime_error(to_string(rebuilt.error()));
    }
    return std::move(*rebuilt);
}

void put_le_field(Bytes& bytes, std::size_t at, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width && at + i < bytes.size(); ++i) {
        bytes[at + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

// Stores of every type, raw records and tails at several segment sizes up to the fuzzing
// cap, a book of orders across segments, then one corrupt copy for each field of the
// store's own encoding, rebuilt with valid CRCs, and cuts.
void write_itch_store_seeds(SeedDir& seeds) {
    namespace st = tick_store::store;
    const Bytes unknown = filled(std::byte{'z'}, 9, std::byte{0xA5});
    const Bytes short_add = filled(AddOrder::type_code, 20, std::byte{0});
    std::vector<Message> messages = ordinary_messages();
    messages.emplace_back(RawMessage{.bytes = unknown, .reason = RawReason::unknown_type});
    messages.emplace_back(RawMessage{.bytes = short_add, .reason = RawReason::wrong_length});
    const std::array tail{std::byte{0x00}, std::byte{0x7F}, std::byte{0xAB}};

    for (const std::uint32_t n : {1U, 2U, 5U, 8U, 25U, st::oracle::kFuzzMaxSegmentRecords}) {
        seeds.add(written_itch_store(messages, tail, n));
    }
    seeds.add(written_itch_store({}, {}, 4));
    seeds.add(written_itch_store({}, tail, 4));
    const std::vector<Message> book = book_messages();
    for (const std::uint32_t n : {7U, 32U, st::oracle::kFuzzMaxSegmentRecords}) {
        seeds.add(written_itch_store(book, {}, n));
    }
    // Over the cap: opened, not decoded.
    seeds.add(written_itch_store(messages, tail, st::oracle::kFuzzMaxSegmentRecords + 1));

    using Parts = std::vector<st::ContainerPart>;
    const Bytes base = written_itch_store(messages, tail, 8);
    const auto section = [](Parts& sections, std::string_view name) -> Bytes& {
        for (st::ContainerPart& part : sections) {
            if (part.section_name == name) {
                return part.bytes;
            }
        }
        throw std::runtime_error("no such section");
    };
    // The store section's fields.
    for (const auto& [at, width, value] :
         std::vector<std::tuple<std::size_t, std::size_t, std::uint64_t>>{
             {st::kStoreVersionOffset, 4, 2},
             {st::kStoreSegmentRecordsOffset, 4, 0},
             {st::kStoreSegmentRecordsOffset, 4, 7},
             {st::kStoreRecordCountOffset, 8, 24},
             {st::kStoreRecordCountOffset, 8, ~std::uint64_t{0}},
             {st::kStoreSegmentCountOffset, 8, 3}}) {
        seeds.add(with_itch_store_part(base, [&](Parts&, Parts& sections) {
            put_le_field(section(sections, st::kStoreSection), at, width, value);
        }));
    }
    // The symbols section: a count off by one, and locates out of order.
    seeds.add(with_itch_store_part(base, [&](Parts&, Parts& sections) {
        put_le_field(section(sections, st::kSymbolsSection), 0, 4, 2);
    }));
    seeds.add(with_itch_store_part(written_itch_store(book, {}, 32), [&](Parts&, Parts& sections) {
        put_le_field(section(sections, st::kSymbolsSection), 14, 2, 1);
    }));
    // Each metadata field of the first segment's type block, then of a column block.
    for (const auto& [at, width, value] :
         std::vector<std::tuple<std::size_t, std::size_t, std::uint64_t>>{
             {st::kMetaSegmentOffset, 4, 1},
             {st::kMetaColumnOffset, 2, st::kColumnCount},
             {st::kMetaReservedOffset, 2, 1},
             {st::kMetaValueCountOffset, 4, 0},
             {st::kMetaValueCountOffset, 4, 7},
             {st::kMetaFirstRecordOffset, 8, 1},
             {st::kMetaFirstTimeOffset, 8, 1},
             {st::kMetaLastTimeOffset, 8, 1},
             {st::kMetaMinTimeOffset, 8, 0},
             {st::kMetaMaxTimeOffset, 8, ~std::uint64_t{0}},
             {st::kMetaTypedOffset, 4, 9},
             {st::kMetaTypedOffset, 4, 7},
             {st::kMetaBitmapSizeOffset, 2, 1},
             {st::kMetaBitmapOffset, 1, 1}}) {
        seeds.add(with_itch_store_part(base, [&](Parts& blocks, Parts&) {
            put_le_field(blocks[0].metadata, at, width, value);
        }));
        if (at < st::kColumnMetadataSize) {
            seeds.add(with_itch_store_part(base, [&](Parts& blocks, Parts&) {
                put_le_field(blocks[2].metadata, at, width, value);
            }));
        }
    }
    // A block under another codec, a block dropped, a block repeated, two swapped.
    seeds.add(with_itch_store_part(base, [](Parts& blocks, Parts&) {
        blocks[2].codec = tick_store::codec::CodecId::dictionary;
    }));
    seeds.add(with_itch_store_part(base, [](Parts& blocks, Parts&) { blocks.erase(blocks.begin() + 2); }));
    seeds.add(with_itch_store_part(base, [](Parts& blocks, Parts&) {
        blocks.insert(blocks.begin() + 2, blocks[2]);
    }));
    seeds.add(with_itch_store_part(base, [](Parts& blocks, Parts&) { std::swap(blocks[0], blocks[1]); }));
    // A column's chunk wrapped in zstd by hand, then each zstd header field changed.
    const Bytes wrapped = with_itch_store_part(base, [](Parts& blocks, Parts&) {
        Bytes outer;
        if (tick_store::codec::encode_zstd_payload(static_cast<std::uint16_t>(blocks[1].codec),
                                                   blocks[1].bytes, outer)
                .has_value()) {
            blocks[1].codec = tick_store::codec::CodecId::zstd;
            blocks[1].bytes = std::move(outer);
        }
    });
    seeds.add(wrapped);
    for (const auto& [at, width, value] :
         std::vector<std::tuple<std::size_t, std::size_t, std::uint64_t>>{
             {0, 2, 0}, {0, 2, 8}, {2, 2, 1}, {4, 4, 0}, {4, 4, 0xFFFF'FFFFU}, {8, 1, 0}}) {
        seeds.add(with_itch_store_part(wrapped, [&](Parts& blocks, Parts&) {
            put_le_field(blocks[1].bytes, at, width, value);
        }));
    }
    // Each block's payload with its first byte changed: a codec header field, a value.
    for (std::size_t b = 0; b < 12; ++b) {
        seeds.add(with_itch_store_part(base, [&](Parts& blocks, Parts&) {
            if (b < blocks.size() && !blocks[b].bytes.empty()) {
                blocks[b].bytes[blocks[b].bytes.size() / 2] ^= std::byte{0x11};
            }
        }));
    }
    // Cuts at the header, inside the data and inside the footer.
    for (const std::size_t cut : {std::size_t{0}, std::size_t{16}, base.size() / 2, base.size() - 1}) {
        seeds.add(std::span{base}.first(cut));
    }
}

// A container from the real writer, committed to `scratch` in the page_cache mode and
// read back.
Bytes written_container(const std::filesystem::path& scratch,
                        const std::vector<tick_store::store::ContainerPart>& parts) {
    using tick_store::store::ContainerWriter;
    auto writer = ContainerWriter::create(scratch, tick_store::store::Durability::page_cache);
    if (!writer.has_value()) {
        throw std::runtime_error(to_string(writer.error()));
    }
    for (const auto& part : parts) {
        if (part.section_name.empty()) {
            if (const auto added = writer->add_block(part.codec, part.metadata, part.bytes);
                !added.has_value()) {
                throw std::runtime_error(to_string(added.error()));
            }
        } else if (const auto added = writer->add_section(part.section_name, part.bytes);
                   !added.has_value()) {
            throw std::runtime_error(to_string(added.error()));
        }
    }
    if (const auto committed = writer->commit(); !committed.has_value()) {
        throw std::runtime_error(to_string(committed.error()));
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

// `file` with a field overwritten and every CRC recomputed, so the seed reaches the
// structural check behind them.
template <std::size_t Width>
Bytes with_container_field(const Bytes& file, std::size_t offset, std::uint64_t value) {
    return tick_store::store::oracle::fixed_checksums(with_field<Width>(file, offset, value));
}

// Valid containers of every shape the format allows, and one corrupt seed for each check
// in the parser, at the offsets container.hpp gives.
void write_container_seeds(SeedDir& seeds, const std::filesystem::path& scratch) {
    namespace store = tick_store::store;
    using tick_store::codec::CodecId;
    using Part = store::ContainerPart;
    const auto block = [](CodecId codec, Bytes metadata, Bytes payload) {
        return Part{.section_name = "", .codec = codec, .metadata = std::move(metadata),
                    .bytes = std::move(payload)};
    };
    const auto section = [](std::string name, Bytes bytes) {
        return Part{.section_name = std::move(name), .codec = CodecId::raw, .metadata = {},
                    .bytes = std::move(bytes)};
    };
    // A real codec block, so the seeds carry what a store writes.
    Bytes timestamps;
    const std::array<std::uint64_t, 5> times{34'200'000'000'000ULL, 34'200'000'000'100ULL,
                                             34'200'000'000'100ULL, 34'200'000'009'000ULL,
                                             34'200'000'009'001ULL};
    if (!tick_store::codec::encode_timestamp_block(times, timestamps).has_value()) {
        throw std::runtime_error("encode_timestamp_block failed");
    }

    const std::vector<Part> typical{
        block(CodecId::gorilla_timestamps, Bytes(16, std::byte{0x11}), timestamps),
        section("dict.stock", Bytes(24, std::byte{'Z'})),
        block(CodecId::raw, {std::byte{1}, std::byte{2}}, {std::byte{0xAB}, std::byte{0xCD}}),
        section("empty", {}),
        block(CodecId::pfor, {}, Bytes(40, std::byte{0x5A})),
        section("index.time", Bytes(48, std::byte{0x07})),
    };
    const Bytes file = written_container(scratch, typical);
    seeds.add(file);
    seeds.add(written_container(scratch, {}));
    seeds.add(written_container(scratch, {block(CodecId::raw, {}, {})}));
    seeds.add(written_container(scratch, {section("only", Bytes(5, std::byte{3}))}));
    seeds.add(written_container(scratch, {section("a", {}), block(CodecId::raw, {}, {}),
                                          section("b", {}), block(CodecId::stream_vbyte, {},
                                                                  Bytes(3, std::byte{9})),
                                          section("c", {})}));
    std::vector<Part> many;
    for (int i = 0; i < 40; ++i) {
        many.push_back(block(static_cast<CodecId>(1 + (i % 7)), Bytes(static_cast<std::size_t>(i % 5),
                                                                      std::byte{0x42}),
                             Bytes(static_cast<std::size_t>(i % 11), static_cast<std::byte>(i))));
        if (i % 4 == 0) {
            many.push_back(section("s" + std::to_string((i * 13) % 40), Bytes(3, std::byte{0x21})));
        }
    }
    seeds.add(written_container(scratch, many));

    // Offsets in `file`, which holds three blocks and three sections.
    const auto parser = store::ContainerParser::open(file);
    if (!parser.has_value()) {
        throw std::runtime_error(to_string(parser.error()));
    }
    const std::size_t size = file.size();
    const std::size_t trailer = size - store::kTrailerSize;
    const auto footer = static_cast<std::size_t>(parser->footer_offset());
    const std::size_t blocks_at = footer + store::kFooterFixedSize;
    const std::size_t sections_at = blocks_at + (3 * store::kBlockEntrySize);
    const std::size_t block1 = blocks_at + store::kBlockEntrySize;
    const std::size_t section1 = sections_at + store::kSectionEntrySize;

    // The header, the trailer and the footer CRC, which nothing recomputes.
    seeds.add(with_field<1>(file, 0, 'X'));
    seeds.add(with_field<4>(file, store::kHeaderVersionOffset, 2));
    seeds.add(with_field<4>(file, store::kHeaderSizeOffset, 17));
    seeds.add(with_field<1>(file, size - 1, 'X'));
    seeds.add(with_field<4>(file, trailer + store::kTrailerReserved, 1));
    seeds.add(with_field<8>(file, trailer, 15));
    seeds.add(with_field<8>(file, trailer, trailer));
    seeds.add(with_field<4>(file, trailer + store::kTrailerFooterCrc, 0));
    // Each footer field, with every CRC made to match.
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{4}, max}) {
        seeds.add(with_container_field<8>(file, footer + store::kFooterBlockCount, value));
        seeds.add(with_container_field<8>(file, footer + store::kFooterSectionCount, value));
        seeds.add(with_container_field<8>(file, footer + store::kFooterMetadataSize, value));
    }
    seeds.add(with_container_field<8>(file, block1 + store::kBlockPayloadOffset, 17));
    seeds.add(with_container_field<8>(file, block1 + store::kBlockMetadataOffset, 1));
    seeds.add(with_container_field<4>(file, block1 + store::kBlockPayloadSize, 3));
    seeds.add(with_container_field<4>(file, block1 + store::kBlockPayloadSize, 1));
    seeds.add(with_container_field<4>(file, block1 + store::kBlockMetadataSize, 3));
    seeds.add(with_container_field<4>(file, block1 + store::kBlockMetadataSize, 1));
    seeds.add(with_container_field<2>(file, block1 + store::kBlockCodec, 0));
    seeds.add(with_container_field<2>(file, block1 + store::kBlockCodec,
                                      tick_store::codec::kCodecs.size()));
    seeds.add(with_container_field<2>(file, block1 + store::kBlockReserved, 1));
    seeds.add(with_container_field<8>(file, section1 + store::kSectionOffset, 1));
    seeds.add(with_container_field<8>(file, section1 + store::kSectionSize, 1));
    seeds.add(with_container_field<1>(file, section1 + store::kSectionNameLength, 0));
    seeds.add(with_container_field<1>(file, section1 + store::kSectionNameLength, 44));
    seeds.add(with_container_field<1>(file, section1 + store::kSectionName, 'A'));
    seeds.add(with_container_field<1>(file, section1 + store::kSectionName + 40, 'a'));
    // A repeated name: the second section renamed to the first's.
    Bytes repeated = file;
    std::ranges::copy(std::span{file}.subspan(sections_at + store::kSectionNameLength,
                                              1 + store::kMaxSectionName),
                      repeated.begin() + static_cast<std::ptrdiff_t>(section1 +
                                                                     store::kSectionNameLength));
    seeds.add(tick_store::store::oracle::fixed_checksums(repeated));
    // A block's and a section's CRC stale, which only a lookup sees.
    for (const std::size_t stale : {block1 + store::kBlockCrc, section1 + store::kSectionCrc}) {
        Bytes stale_crc = with_field<4>(file, stale, 0x5A5A5A5A);
        const std::uint32_t footer_crc = tick_store::store::crc32c(
            std::span<const std::byte>{stale_crc}.subspan(footer, trailer - footer));
        seeds.add(with_field<4>(stale_crc, trailer + store::kTrailerFooterCrc, footer_crc));
    }
    Bytes stale_payload = file;
    stale_payload[store::kContainerHeaderSize] ^= std::byte{0x01};
    seeds.add(stale_payload);

    // Cut at every boundary and inside the trailer.
    for (const std::size_t cut : {std::size_t{0}, std::size_t{15}, std::size_t{16},
                                  std::size_t{63}, footer, blocks_at, sections_at, trailer,
                                  size - 8, size - 1}) {
        seeds.add(std::span{file}.first(cut));
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

// A day-like stream for the query engine: adds, executions, cancels, deletes, replaces and
// trades over a few locates, system events on locate 0, ties in time and orders named
// across segments, enough records for several seek groups.
std::vector<Message> query_messages(std::size_t records) {
    std::vector<Message> out;
    out.reserve(records);
    std::uint64_t state = 11;
    std::vector<std::vector<std::uint64_t>> live(6);
    std::uint64_t next = 9'000;
    std::uint64_t time = 34'199'000'000'000ULL;
    for (std::size_t i = 0; i < records; ++i) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        time += (state >> 61U) == 0 ? 0 : (state >> 40U) % 2'000'000;
        const auto locate = static_cast<std::uint16_t>(1 + ((state >> 33U) % 5));
        const MessageHeader h{locate, static_cast<std::uint16_t>(i % 3), time};
        std::vector<std::uint64_t>& orders = live[locate];
        const std::uint64_t pick = (state >> 20U) % 16;
        const auto price = Price4{static_cast<std::uint32_t>(100'000 + ((state >> 44U) % 900))};
        if (pick == 15) {
            out.emplace_back(SystemEvent{MessageHeader{0, 0, time}, ch('Q')});
            continue;
        }
        if (orders.empty() || pick < 6) {
            out.emplace_back(AddOrder{h, next, (state & 1U) != 0 ? ch('B') : ch('S'),
                                      static_cast<std::uint32_t>(100 * (1 + (state >> 58U))),
                                      i % 1'000 == 999 ? alpha<8>("OTHER   ") : kStock, price});
            orders.push_back(next++);
            continue;
        }
        const std::size_t at = (state >> 8U) % orders.size();
        const std::uint64_t named = (state >> 7U) % 23 == 0 ? next + 77 : orders[at];
        if (pick < 9) {
            out.emplace_back(OrderExecuted{h, named, 10, 90'000 + i});
        } else if (pick < 10) {
            out.emplace_back(OrderExecutedWithPrice{h, named, 5, 90'000 + i, ch('Y'), price});
        } else if (pick < 11) {
            out.emplace_back(OrderReplace{h, named, next, 100, price});
            orders[at] = next++;
        } else if (pick < 12) {
            out.emplace_back(OrderCancel{h, named, 5});
        } else if (pick < 14) {
            out.emplace_back(OrderDelete{h, named});
            orders.erase(orders.begin() + static_cast<std::ptrdiff_t>(at));
        } else {
            out.emplace_back(Trade{h, named, ch('B'), 100, kStock, price, 70'000 + i});
        }
    }
    return out;
}

// Stores for the query engine at segment sizes up to its fuzzing cap, one holding records
// the column form keeps raw, then copies with a byte of each index column changed and
// rebuilt with valid CRCs, and the ITCH store seeds.
void write_itch_query_seeds(SeedDir& seeds) {
    namespace st = tick_store::store;
    using Parts = std::vector<st::ContainerPart>;
    const std::vector<Message> day = query_messages(9'000);
    for (const std::uint32_t n : {4'500U, 9'000U, st::oracle::kQueryFuzzMaxSegmentRecords}) {
        seeds.add(written_itch_store(day, {}, n));
    }
    std::vector<Message> mixed = day;
    const Bytes unknown = filled(std::byte{'z'}, 9, std::byte{0xA5});
    mixed[0] = RawMessage{.bytes = unknown, .reason = RawReason::unknown_type};
    mixed[4'500] = RawMessage{.bytes = unknown, .reason = RawReason::unknown_type};
    mixed[6'000] = AddOrder{MessageHeader{2, 0, 34'200'500'000'000ULL}, 77, ch('B'), 0x8000'0001U,
                            kStock, Price4{100}};
    seeds.add(written_itch_store(mixed, {}, 4'500));
    const Bytes base = written_itch_store(day, {}, 4'500);
    const auto block_of = [](const Parts& blocks, st::Column column) -> std::size_t {
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const auto metadata = st::parse_column_metadata(blocks[b].metadata, 0);
            if (metadata.has_value() && metadata->segment == 0 && metadata->column == column) {
                return b;
            }
        }
        throw std::runtime_error("no such block");
    };
    for (const st::Column column : {st::Column::seek, st::Column::locate_run,
                                    st::Column::locate_run_records, st::Column::locate_run_adds,
                                    st::Column::locate, st::Column::type, st::Column::timestamp}) {
        for (const std::size_t at : {std::size_t{0}, std::size_t{9}, std::size_t{40}, std::size_t{200}}) {
            seeds.add(with_itch_store_part(base, [&](Parts& blocks, Parts&) {
                Bytes& payload = blocks[block_of(blocks, column)].bytes;
                if (at < payload.size()) {
                    payload[at] ^= std::byte{0x10};
                }
            }));
        }
    }
    // The plain flag cleared on a plain segment, and set on one that is not.
    seeds.add(with_itch_store_part(base, [&](Parts& blocks, Parts&) {
        put_le_field(blocks[block_of(blocks, st::Column::type)].metadata, st::kMetaPlainOffset, 1, 0);
    }));
    seeds.add(with_itch_store_part(written_itch_store(mixed, {}, 4'500), [&](Parts& blocks, Parts&) {
        put_le_field(blocks[block_of(blocks, st::Column::type)].metadata, st::kMetaPlainOffset, 1, 1);
    }));
    write_itch_store_seeds(seeds);
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

// Columns with few distinct values, as the dictionary and run-length codecs are for:
// share-like sizes with a few common values, which take Huffman codes; a locate-like
// column of 40 values drawn with recency, which takes move-to-front; runs of a few values;
// one value; and two values alternating.
std::vector<Words> low_cardinality_sets() {
    SplitMix64 random{2026};
    constexpr std::array<std::uint64_t, 9> kShares{100, 200, 300, 500, 1000, 50, 1, 37, 2500};
    Words shares;
    for (int i = 0; i < 600; ++i) {
        // 100 most often, then 200 and 300, then the rest.
        const std::uint64_t pick = random() % 64;
        std::uint64_t at = 3 + (pick % 6);
        if (pick < 40) {
            at = 0;
        } else if (pick < 52) {
            at = 1 + (pick % 2);
        }
        shares.push_back(kShares[at]);
    }
    Words locates;
    Words recent{8'000};
    for (int i = 0; i < 600; ++i) {
        if (random() % 4 == 0 || recent.size() < 40) {
            recent.push_back(1 + (random() % 9'000));
        }
        locates.push_back(recent[recent.size() - 1 - (random() % std::min<std::size_t>(recent.size(), 6))]);
    }
    Words runs;
    while (runs.size() < 700) {
        const std::uint64_t value = random() % 6;
        const std::uint64_t length = 1 + (random() % 30);
        runs.insert(runs.end(), length, value);
    }
    Words alternating;
    for (std::uint64_t i = 0; i < 64; ++i) {
        alternating.push_back(i % 2 == 0 ? 7 : 1ULL << 40U);
    }
    return {shares, locates, runs, Words(50, 42), alternating};
}

// The integer seeds, then the low-cardinality columns, and one block of each through
// every header and byte corruption, so a refusal in each mode is one change away.
void write_low_cardinality_seeds(SeedDir& seeds,
                                 std::span<const tick_store::codec::oracle::Encoder> encoders) {
    write_integer_seeds(seeds, encoders);
    const std::vector<Words> sets = low_cardinality_sets();
    for (const auto& encoder : encoders) {
        add_columns(seeds, encoder.encode_column, sets);
        for (const Words& values : sets) {
            const Words first(values.begin(),
                              values.begin() + static_cast<std::ptrdiff_t>(
                                                   std::min<std::size_t>(values.size(), 64)));
            const Bytes block = column_of(encoder.encode_column, first, 4096);
            add_header_corruptions(seeds, block);
            add_byte_corruptions(seeds, block);
        }
        // The most values one block holds, all equal: a few dozen bytes the oracle decodes to
        // 2^20 values and re-encodes in blocks of 3. A dictionary block of this, one entry
        // with codes of no bits, timed out the CI fuzz run before encode_dict copied a block
        // that repeats the one before.
        seeds.add(column_of(encoder.encode_column, Words(tick_store::codec::kMaxBlockValues, 42),
                            tick_store::codec::kMaxBlockValues));
    }
}
// A block of `count` values whose payload is the given fixed bytes followed by nested PFOR
// blocks, each with its options, as the dictionary and run-length payloads are.
struct Nested {
    Words values;
    tick_store::codec::PforOptions options;
};

Bytes nested_block(std::uint32_t count, const Bytes& fixed, const std::vector<Nested>& nested) {
    Bytes payload = fixed;
    for (const Nested& part : nested) {
        if (!tick_store::codec::encode_pfor_block(part.values, part.options, payload).has_value()) {
            throw std::runtime_error("a hand-built seed's nested block was refused");
        }
    }
    Bytes block;
    const std::size_t at = tick_store::codec::begin_block(block, count);
    tick_store::codec::patch_block_header(block, at, static_cast<std::uint32_t>(payload.size()));
    block.insert(block.end(), payload.begin(), payload.end());
    return block;
}

// An optional leading byte, then a u32 little-endian.
Bytes fixed_fields(std::optional<std::uint8_t> first, std::uint32_t value) {
    Bytes out;
    if (first.has_value()) {
        out.push_back(static_cast<std::byte>(*first));
    }
    for (unsigned i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
    }
    return out;
}

// Adds a hand-built block. One meant to be valid must decode, so a valid seed cannot drift
// into a refusal. Refusals are not checked here: the unit tests' corrupt fixtures check each
// one, and a decoder that stops refusing one must reach the fuzz oracle, not stop the build.
void add_checked(SeedDir& seeds, tick_store::codec::BlockDecoder decode, const Bytes& block,
                 std::string_view refused_field) {
    if (refused_field.empty()) {
        Words ignored;
        if (!decode(block, ignored).has_value()) {
            throw std::runtime_error("a hand-built seed meant to be valid was refused");
        }
    }
    seeds.add(block);
}

// Each refusal of the dictionary decoder's own order checks, built by hand with the
// encoder's nested options: a ranked dictionary with an entry repeated, one out of
// frequency order, one with entries tied in use but not ascending, an unused entry, and a
// sorted dictionary with an entry repeated and one descending.
void add_dict_refusals(SeedDir& seeds) {
    using tick_store::codec::IntTransform;
    using tick_store::codec::PforOptions;
    constexpr PforOptions kRanked{.transform = IntTransform::none, .adaptive_scale = true,
                                  .patch = true};
    constexpr PforOptions kSorted{.transform = IntTransform::delta, .adaptive_scale = true,
                                  .patch = true};
    constexpr PforOptions kIndices{.transform = IntTransform::none, .adaptive_scale = false,
                                   .patch = true};
    const auto add = [&](std::uint8_t mode, const PforOptions& options, const Words& dictionary,
                         const Words& indices, std::string_view refused_field) {
        add_checked(seeds, &tick_store::codec::decode_dict_block,
                    nested_block(static_cast<std::uint32_t>(indices.size()),
                                 fixed_fields(mode, static_cast<std::uint32_t>(dictionary.size())),
                                 {{dictionary, options}, {indices, kIndices}}),
                    refused_field);
    };
    add(0, kRanked, {100, 200}, {0, 0, 1}, "");
    add(0, kRanked, {100, 100}, {0, 0, 1}, "dict entry repeated");
    add(0, kRanked, {100, 200}, {0, 1, 1}, "dict entry order");
    add(0, kRanked, {200, 100}, {0, 1}, "dict entry order");
    add(0, kRanked, {100, 200, 300}, {0, 0, 1}, "dict entry unused");
    add(2, kSorted, {100, 200, 300}, {0, 1, 2, 0}, "");
    add(2, kSorted, {100, 100, 300}, {0, 1, 2}, "dict entry order");
    add(2, kSorted, {300, 200, 100}, {0, 1, 2}, "dict entry order");
}

// Each refusal of the run-length decoder's own checks: runs not maximal, and lengths one
// short of the count and one past it.
void add_rle_refusals(SeedDir& seeds) {
    using tick_store::codec::IntTransform;
    using tick_store::codec::PforOptions;
    constexpr PforOptions kPlain{.transform = IntTransform::none, .adaptive_scale = true,
                                 .patch = true};
    constexpr PforOptions kLengths{.transform = IntTransform::none, .adaptive_scale = false,
                                   .patch = true};
    const auto add = [&](std::uint32_t count, const Words& values, const Words& lengths,
                         std::string_view refused_field) {
        add_checked(seeds, &tick_store::codec::decode_rle_block,
                    nested_block(count,
                                 fixed_fields(std::nullopt,
                                              static_cast<std::uint32_t>(values.size())),
                                 {{values, kPlain}, {lengths, kLengths}}),
                    refused_field);
    };
    add(5, {3, 4}, {1, 2}, "");
    add(5, {3, 3}, {1, 2}, "rle run value");
    add(6, {3, 4}, {1, 2}, "rle run lengths");
    add(4, {3, 4}, {1, 2}, "rle run length");
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
    SeedDir container{root / "container"};
    SeedDir itch_store{root / "itch_store"};
    SeedDir itch_query{root / "itch_query"};
    SeedDir columns{root / "columns"};
    SeedDir timestamps{root / "gorilla_timestamps"};
    SeedDir xor_words{root / "gorilla_xor"};
    SeedDir pfor{root / "pfor"};
    SeedDir svb{root / "stream_vbyte"};
    SeedDir dict{root / "dictionary"};
    SeedDir rle{root / "run_length"};
    write_message_seeds(message);
    write_frame_seeds(stream);
    write_store_seeds(store, root / "store-scratch");
    write_container_seeds(container, root / "container-scratch.tsc");
    write_itch_store_seeds(itch_store);
    write_itch_query_seeds(itch_query);
    write_columns_seeds(columns);
    write_timestamp_seeds(timestamps);
    write_xor_seeds(xor_words);
    write_integer_seeds(pfor, tick_store::codec::oracle::detail::kPforEncoders);
    write_integer_seeds(svb, tick_store::codec::oracle::detail::kSvbEncoders);
    add_svb_refusals(svb);
    write_low_cardinality_seeds(dict, tick_store::codec::oracle::detail::kDictEncoders);
    add_dict_refusals(dict);
    write_low_cardinality_seeds(rle, tick_store::codec::oracle::detail::kRleEncoders);
    add_rle_refusals(rle);
    std::println("wrote {} message, {} frame, {} store, {} container, {} itch_store, {} itch_query, {} columns, "
                 "{} gorilla_timestamps, {} gorilla_xor, {} pfor, {} stream_vbyte, "
                 "{} dictionary and {} run_length seeds",
                 message.count(), stream.count(), store.count(), container.count(),
                 itch_store.count(), itch_query.count(), columns.count(),
                 timestamps.count(), xor_words.count(), pfor.count(), svb.count(), dict.count(),
                 rle.count());
    return 0;
}
