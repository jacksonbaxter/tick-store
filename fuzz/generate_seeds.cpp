// Writes the seed corpora, generated at build time and never committed:
//
//   <dir>/message/  payloads, for fuzz_itch_message
//   <dir>/frame/    framed streams, for fuzz_itch_frame
//   <dir>/store/    stub store files, for fuzz_stub_store
//
// Every valid seed comes from the real encoder or the real store writer, so the
// seeds follow the code (Z4). Corrupt seeds are those same bytes cut, extended or
// relabelled, one for each way the decoders and the parser can refuse an input.

#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/little_endian.hpp"
#include "tick_store/store/store_error.hpp"
#include "tick_store/store/stub_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <print>
#include <span>
#include <stdexcept>
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
    write_message_seeds(message);
    write_frame_seeds(stream);
    write_store_seeds(store, root / "store-scratch");
    std::println("wrote {} message, {} frame and {} store seeds", message.count(), stream.count(),
                 store.count());
    return 0;
}
