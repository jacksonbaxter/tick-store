// Usage is in harness/query_cli.hpp.

#include "tick_store/harness/query_cli.hpp"

#include "tick_store/harness/roundtrip_cli.hpp"
#include "tick_store/harness/sha256.hpp"
#include "tick_store/store/itch_query.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tick_store::harness {
namespace {

using store::ItchQuery;
using store::QueryRow;
using store::QueryStats;

constexpr std::string_view kUsage =
    "usage: tick_query <store.tsc> --day <MMDDYYYY> [--path indexed|exact]\n";

// A flat JSON object: strings, integers, booleans and null. Enough for a request line.
using JsonValue = std::variant<std::monostate, std::string, std::int64_t, bool>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<JsonObject> object() {
        JsonObject out;
        if (!eat('{')) {
            return std::nullopt;
        }
        if (eat('}')) {
            return end_of_text() ? std::optional{out} : std::nullopt;
        }
        while (true) {
            auto key = string();
            if (!key.has_value() || !eat(':')) {
                return std::nullopt;
            }
            auto value = scalar();
            if (!value.has_value()) {
                return std::nullopt;
            }
            out.insert_or_assign(std::move(*key), std::move(*value));
            if (eat(',')) {
                continue;
            }
            if (eat('}')) {
                return end_of_text() ? std::optional{out} : std::nullopt;
            }
            return std::nullopt;
        }
    }

private:
    void space() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) {
            ++at_;
        }
    }

    [[nodiscard]] bool eat(char c) {
        space();
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool end_of_text() {
        space();
        return at_ == text_.size();
    }

    [[nodiscard]] bool word(std::string_view w) {
        if (text_.substr(at_, w.size()) == w) {
            at_ += w.size();
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string> string() {
        if (!eat('"')) {
            return std::nullopt;
        }
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) {
                return std::nullopt;
            }
            const char e = text_[at_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    out += ' ';
                    break;
                case 'u':
                    // Kept as '?': no field the worker reads holds anything but ASCII.
                    if (at_ + 4 > text_.size()) {
                        return std::nullopt;
                    }
                    at_ += 4;
                    out += '?';
                    break;
                default:
                    return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> scalar() {
        space();
        if (at_ >= text_.size()) {
            return std::nullopt;
        }
        if (text_[at_] == '"') {
            auto s = string();
            if (!s.has_value()) {
                return std::nullopt;
            }
            return JsonValue{std::move(*s)};
        }
        if (word("true")) {
            return JsonValue{true};
        }
        if (word("false")) {
            return JsonValue{false};
        }
        if (word("null")) {
            return JsonValue{};
        }
        const bool negative = text_[at_] == '-';
        if (negative) {
            ++at_;
        }
        const std::size_t first = at_;
        std::uint64_t value = 0;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            const auto digit = static_cast<std::uint64_t>(text_[at_] - '0');
            if (value > (std::uint64_t{1} << 62U) / 5U) {
                return std::nullopt;
            }
            value = (value * 10U) + digit;
            ++at_;
        }
        if (at_ == first || value > static_cast<std::uint64_t>(INT64_MAX)) {
            return std::nullopt;
        }
        const auto signed_value = static_cast<std::int64_t>(value);
        return JsonValue{negative ? -signed_value : signed_value};
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

[[nodiscard]] std::string json_string(std::string_view text) {
    std::string out(1, '"');
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] std::optional<std::int64_t> integer(const JsonObject& request, std::string_view key) {
    const auto at = request.find(key);
    if (at == request.end()) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::int64_t>(&at->second)) {
        return *value;
    }
    return std::nullopt;
}

[[nodiscard]] std::string text(const JsonObject& request, std::string_view key) {
    const auto at = request.find(key);
    if (at == request.end()) {
        return {};
    }
    if (const auto* value = std::get_if<std::string>(&at->second)) {
        return *value;
    }
    return {};
}

// compare/queries.py's _hash_rows() over ROW_COLUMNS, the rows already in seq order.
[[nodiscard]] std::string rows_digest(const std::vector<QueryRow>& rows, std::int64_t midnight) {
    Sha256 hash;
    std::vector<std::byte> buffer;
    buffer.reserve(1U << 16U);
    const auto flush = [&] {
        hash.update(buffer);
        buffer.clear();
    };
    const auto name = [&](std::string_view column) {
        flush();
        hash.update(std::as_bytes(std::span{column.data(), column.size()}));
    };
    const auto put = [&](std::int64_t value) {
        auto bits = static_cast<std::uint64_t>(value);
        for (int i = 0; i < 8; ++i) {
            buffer.push_back(static_cast<std::byte>(bits & 0xFFU));
            bits >>= 8U;
        }
        if (buffer.size() >= (1U << 16U)) {
            flush();
        }
    };
    const auto column = [&](std::string_view column_name, auto field) {
        name(column_name);
        for (const QueryRow& row : rows) {
            put(field(row));
        }
    };
    column("seq", [](const QueryRow& r) { return static_cast<std::int64_t>(r.seq); });
    column("ts", [&](const QueryRow& r) { return static_cast<std::int64_t>(r.ts_ns) + midnight; });
    column("msg_type", [](const QueryRow& r) { return static_cast<std::int64_t>(r.msg_type); });
    column("locate", [](const QueryRow& r) { return static_cast<std::int64_t>(r.locate); });
    column("order_ref", [](const QueryRow& r) { return static_cast<std::int64_t>(r.order_ref); });
    // The column form's side is int8 and every stored side fits it (columns.hpp).
    column("side", [](const QueryRow& r) { return static_cast<std::int64_t>(static_cast<std::int8_t>(r.side)); });
    column("shares", [](const QueryRow& r) { return static_cast<std::int64_t>(r.shares); });
    column("price", [](const QueryRow& r) { return static_cast<std::int64_t>(r.price); });
    name("stock");
    for (const QueryRow& row : rows) {
        if (row.has_stock) {
            buffer.insert(buffer.end(), row.stock.begin(), row.stock.end());
        }
        buffer.push_back(std::byte{0});
        if (buffer.size() >= (1U << 16U)) {
            flush();
        }
    }
    flush();
    return to_hex(hash.finish());
}

[[nodiscard]] std::string aggregate_digest(const std::string& json) {
    return to_hex(sha256(std::as_bytes(std::span{json.data(), json.size()})));
}

[[nodiscard]] std::string stats_json(const QueryStats& stats) {
    return std::format(
        R"("segments":{},"exact_segments":{},"blocks":{},"bytes_decoded":{},)"
        R"("values_decoded":{})",
        stats.segments, stats.exact_segments, stats.blocks, stats.bytes_decoded,
        stats.values_decoded);
}

[[nodiscard]] std::int64_t since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                start)
        .count();
}

// One request line to one answer line.
[[nodiscard]] std::string answer(ItchQuery& query, std::string_view line, std::int64_t midnight,
                                 bool& quit) {
    JsonReader reader{line};
    const auto request = reader.object();
    if (!request.has_value()) {
        return R"({"error":"request is not a flat JSON object"})";
    }
    const std::string kind = text(*request, "kind");
    const std::string name = text(*request, "name");
    const std::string head =
        std::format(R"({{"name":{},"kind":{})", json_string(name), json_string(kind));
    const auto fail = [&](std::string_view why) {
        return std::format(R"({},"error":{}}})", head, json_string(why));
    };
    if (kind == "quit") {
        quit = true;
        return head + "}";
    }
    const auto start = integer(*request, "start_ns").value_or(0);
    const auto end = integer(*request, "end_ns").value_or(0);
    const auto locate = integer(*request, "locate").value_or(-1);
    if (start < 0 || end < 0) {
        return fail("start_ns and end_ns must not be negative");
    }
    if (locate < -1 || locate > 0xFFFF) {
        return fail("locate must be -1 or 0 to 65535");
    }
    const auto from = static_cast<std::uint64_t>(start);
    const auto to = static_cast<std::uint64_t>(end);
    const std::optional<std::uint16_t> one =
        locate < 0 ? std::nullopt : std::optional{static_cast<std::uint16_t>(locate)};

    if (kind == "rows") {
        const auto clock = std::chrono::steady_clock::now();
        auto rows = query.rows(from, to, one);
        const std::int64_t engine = since(clock);
        if (!rows.has_value()) {
            return fail(store::to_string(rows.error()));
        }
        const auto digest_clock = std::chrono::steady_clock::now();
        const std::string digest = rows_digest(*rows, midnight);
        return std::format(R"({},"rows":{},"sha256":"{}","engine_ns":{},"digest_ns":{},{}}})",
                           head, rows->size(), digest, engine, since(digest_clock),
                           stats_json(query.stats()));
    }
    const auto aggregate = [&](std::int64_t engine, const std::string& json, std::size_t count,
                               std::int64_t digest_ns) {
        return std::format(
            R"({},"rows":{},"sha256":"{}","answer":{},"engine_ns":{},"digest_ns":{},{}}})",
            head, count, aggregate_digest(json), json, engine, digest_ns,
            stats_json(query.stats()));
    };
    if (kind == "minute_aggregate") {
        if (!one.has_value()) {
            return fail("minute_aggregate needs a locate");
        }
        const auto clock = std::chrono::steady_clock::now();
        auto minutes = query.minute_aggregate(from, to, *one);
        const std::int64_t engine = since(clock);
        if (!minutes.has_value()) {
            return fail(store::to_string(minutes.error()));
        }
        const auto digest_clock = std::chrono::steady_clock::now();
        std::string json = "[";
        for (std::size_t i = 0; i < minutes->size(); ++i) {
            const store::MinuteBucket& b = (*minutes)[i];
            json += std::format("{}[{},{},{},{},{}]", i == 0 ? "" : ",",
                                static_cast<std::int64_t>(b.minute_ns) + midnight, b.count,
                                b.shares, b.min_price, b.max_price);
        }
        json += ']';
        return aggregate(engine, json, minutes->size(), since(digest_clock));
    }
    if (kind == "column_sum") {
        const auto clock = std::chrono::steady_clock::now();
        auto sum = query.shares_sum();
        const std::int64_t engine = since(clock);
        if (!sum.has_value()) {
            return fail(store::to_string(sum.error()));
        }
        const auto digest_clock = std::chrono::steady_clock::now();
        const std::string json = std::format("[[{},{}]]", sum->records, sum->sum);
        return aggregate(engine, json, 1, since(digest_clock));
    }
    if (kind == "busiest_locate") {
        const auto clock = std::chrono::steady_clock::now();
        auto counts = query.locate_counts(from, to);
        const std::int64_t engine = since(clock);
        if (!counts.has_value()) {
            return fail(store::to_string(counts.error()));
        }
        // numpy's argmax over the counts with locate 0 zeroed: the first maximum.
        std::size_t best = 0;
        std::uint64_t most = 0;
        for (std::size_t l = 1; l < counts->size(); ++l) {
            if ((*counts)[l] > most) {
                most = (*counts)[l];
                best = l;
            }
        }
        return std::format(R"({},"locate":{},"locate_rows":{},"engine_ns":{},{}}})", head, best,
                           most, engine, stats_json(query.stats()));
    }
    return fail("kind must be rows, minute_aggregate, column_sum, busiest_locate or quit");
}

}  // namespace

std::optional<std::int64_t> midnight_ns_of(std::string_view label) noexcept {
    if (label.size() != 8) {
        return std::nullopt;
    }
    std::array<int, 3> number{0, 0, 0};
    const std::array<std::pair<std::size_t, std::size_t>, 3> parts{{{0, 2}, {2, 2}, {4, 4}}};
    for (std::size_t p = 0; p < parts.size(); ++p) {
        for (std::size_t i = 0; i < parts[p].second; ++i) {
            const char c = label[parts[p].first + i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            number[p] = (number[p] * 10) + (c - '0');
        }
    }
    const std::chrono::year_month_day date{std::chrono::year{number[2]},
                                           std::chrono::month{static_cast<unsigned>(number[0])},
                                           std::chrono::day{static_cast<unsigned>(number[1])}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const std::chrono::sys_days days{date};
    return std::chrono::duration_cast<std::chrono::nanoseconds>(days.time_since_epoch()).count();
}

int run_query_cli(std::span<const std::string_view> arguments, std::istream& in, std::ostream& out,
                  CliOutput& output) {
    std::optional<std::string_view> path;
    std::optional<std::int64_t> midnight;
    store::QueryPath query_path = store::QueryPath::indexed;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view arg = arguments[i];
        if (arg == "--day" && i + 1 < arguments.size()) {
            midnight = midnight_ns_of(arguments[++i]);
            if (!midnight.has_value()) {
                output.err += "tick_query: --day takes a date as MMDDYYYY\n";
                return kExitUsageOrIo;
            }
        } else if (arg == "--path" && i + 1 < arguments.size()) {
            const std::string_view which = arguments[++i];
            if (which == "indexed") {
                query_path = store::QueryPath::indexed;
            } else if (which == "exact") {
                query_path = store::QueryPath::exact;
            } else {
                output.err += kUsage;
                return kExitUsageOrIo;
            }
        } else if (!path.has_value() && !arg.starts_with("--")) {
            path = arg;
        } else {
            output.err += kUsage;
            return kExitUsageOrIo;
        }
    }
    if (!path.has_value() || !midnight.has_value()) {
        output.err += kUsage;
        return kExitUsageOrIo;
    }
    const auto clock = std::chrono::steady_clock::now();
    auto reader = store::ItchStoreReader::open(std::string{*path});
    if (!reader.has_value()) {
        output.err += std::format("tick_query: the store does not open: {}\n",
                                  store::to_string(reader.error()));
        return kExitMismatch;
    }
    ItchQuery query{*reader, query_path};
    out << std::format(
               R"({{"ready":true,"open_ns":{},"segments":{},"records":{},"path":"{}"}})",
               since(clock), reader->segment_count(), reader->header().record_count,
               query_path == store::QueryPath::indexed ? "indexed" : "exact")
        << '\n'
        << std::flush;
    std::string line;
    bool quit = false;
    while (!quit && std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        out << answer(query, line, *midnight, quit) << '\n' << std::flush;
    }
    return kExitPass;
}

}  // namespace tick_store::harness
