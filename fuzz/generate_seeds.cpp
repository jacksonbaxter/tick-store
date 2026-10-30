// Writes the seed corpora, generated at build time and never committed:
//
//   <dir>/message/  payloads, for fuzz_itch_message
//   <dir>/frame/    framed streams, for fuzz_itch_frame
//
// Every valid seed comes from the real encoder, so the seeds follow the code (Z4).
// Corrupt seeds are those same bytes cut, extended or relabelled, one for each
// way the decoders can refuse an input.

#include "tick_store/itch/codec.hpp"
#include "tick_store/itch/message.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
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
    write_message_seeds(message);
    write_frame_seeds(stream);
    std::println("wrote {} message seeds and {} frame seeds", message.count(), stream.count());
    return 0;
}
