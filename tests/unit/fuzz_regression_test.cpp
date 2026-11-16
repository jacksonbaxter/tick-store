// Replays checked-in crashers through the fuzz targets' own oracles (Z6), and runs
// the golden fixtures through them, so the oracles stay exercised on every build
// even when the crasher directories hold nothing.

#include "bound_seeds.hpp"
#include "codec_oracles.hpp"
#include "columns_fixture.hpp"
#include "columns_oracles.hpp"
#include "container_fixture.hpp"
#include "container_oracles.hpp"
#include "itch_fixtures.hpp"
#include "itch_oracles.hpp"
#include "itch_query_oracles.hpp"
#include "itch_store_fixture.hpp"
#include "itch_store_oracles.hpp"
#include "scratch_dir.hpp"
#include "store_oracles.hpp"
#include "stub_fixture.hpp"
#include "test_bytes.hpp"
#include "tick_store/codec/block.hpp"
#include "tick_store/codec/dictionary.hpp"
#include "tick_store/codec/gorilla_timestamps.hpp"
#include "tick_store/codec/gorilla_xor.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/price_double.hpp"
#include "tick_store/codec/run_length.hpp"
#include "tick_store/codec/stream_vbyte.hpp"
#include "tick_store/itch/frame.hpp"
#include "tick_store/itch/message.hpp"
#include "tick_store/store/container.hpp"
#include "tick_store/store/itch_store.hpp"
#include "tick_store/store/itch_store_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::itch {
namespace {

using test::Bytes;
using test::exact_copy;
using test::fixtures;

std::vector<std::filesystem::path> crashers(const std::string& target) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry :
         std::filesystem::directory_iterator{std::filesystem::path{TICK_STORE_CRASHER_DIR} / target}) {
        if (entry.is_regular_file() && entry.path().filename() != ".gitkeep") {
            paths.push_back(entry.path());
        }
    }
    return paths;
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    Bytes bytes;
    for (auto it = std::istreambuf_iterator<char>{file}; it != std::istreambuf_iterator<char>{};
         ++it) {
        bytes.push_back(static_cast<std::byte>(*it));
    }
    return exact_copy(bytes);
}

// nullptr turned into an empty string, so a failure prints the broken property.
std::string text(const char* failure) { return failure == nullptr ? std::string{} : failure; }

TEST(FuzzRegression, MessageCrashersStayFixed) {
    for (const auto& path : crashers("itch_message")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(oracle::check_message(read_file(path))), "");
    }
}

TEST(FuzzRegression, FrameCrashersStayFixed) {
    for (const auto& path : crashers("itch_frame")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(oracle::check_stream(read_file(path))), "");
    }
}

TEST(FuzzRegression, StoreCrashersStayFixed) {
    for (const auto& path : crashers("stub_store")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_store(read_file(path))), "");
    }
}

TEST(FuzzRegression, ContainerCrashersStayFixed) {
    for (const auto& path : crashers("container")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_container(read_file(path))), "");
    }
}

TEST(FuzzRegression, ItchStoreCrashersStayFixed) {
    for (const auto& path : crashers("itch_store")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_itch_store(read_file(path))), "");
    }
}

TEST(FuzzRegression, ItchQueryCrashersStayFixed) {
    for (const auto& path : crashers("itch_query")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_itch_query(read_file(path))), "");
    }
}

TEST(FuzzRegression, ColumnsCrashersStayFixed) {
    for (const auto& path : crashers("columns")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(store::oracle::check_columns(read_file(path))), "");
    }
}

TEST(FuzzRegression, GorillaTimestampCrashersStayFixed) {
    for (const auto& path : crashers("gorilla_timestamps")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_timestamp_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, GorillaXorCrashersStayFixed) {
    for (const auto& path : crashers("gorilla_xor")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_xor_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, PforCrashersStayFixed) {
    for (const auto& path : crashers("pfor")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_pfor_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, StreamVByteCrashersStayFixed) {
    for (const auto& path : crashers("stream_vbyte")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_svb_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, DictionaryCrashersStayFixed) {
    for (const auto& path : crashers("dictionary")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_dict_column(read_file(path))), "");
    }
}

TEST(FuzzRegression, RunLengthCrashersStayFixed) {
    for (const auto& path : crashers("run_length")) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_EQ(text(codec::oracle::check_rle_column(read_file(path))), "");
    }
}

// fuzz/bound_seeds.txt, which a runner times the bound seeds from, names the seeds
// fuzz/bound_seeds.hpp builds, in the same order.
TEST(FuzzRegression, BoundSeedsAreTheListedOnes) {
    std::ifstream list{TICK_STORE_BOUND_SEEDS};
    ASSERT_TRUE(list.is_open());
    std::vector<std::string> lines;
    for (std::string line; std::getline(list, line);) {
        lines.push_back(line);
    }
    std::vector<std::string> named;
    named.reserve(bound_seeds::kBoundSeeds.size());
    for (const auto& seed : bound_seeds::kBoundSeeds) {
        named.push_back(std::string{seed.target} + " " + std::string{seed.seeds} + "/" +
                        std::string{seed.name});
    }
    EXPECT_EQ(lines, named);
}

// The tiers a codec input's blocks fall in under `bounds`, walked as check_as_column walks
// them: within the cap while the values before a block and its stated count stay within it,
// then compared, then decoded once, by the same values; a refused block ends the walk, and
// is noted where it is the first, within the cap, or where it states values and is past the
// cap within the compare limit (not the tail's header, which states none).
bound_seeds::CodecTiers tiers_of(const codec::oracle::CodecUnderTest& codec,
                                 std::span<const std::byte> input,
                                 const codec::oracle::OracleBounds& bounds) {
    bound_seeds::CodecTiers tiers;
    std::size_t offset = 0;
    std::size_t seen = 0;
    bool capped = false;
    while (offset < input.size()) {
        const std::span<const std::byte> rest = input.subspan(offset);
        const std::size_t after = seen + codec::stated_block_count(rest).value_or(0);
        std::vector<std::uint64_t> values;
        const auto taken = codec.decode_block(rest, values);
        if (!taken.has_value()) {
            tiers.first_refused_within_cap = offset == 0 && after <= bounds.decoded_values;
            tiers.refused_compared = after > seen && (capped || after > bounds.decoded_values) &&
                                     after <= bounds.compared_values;
            break;
        }
        capped = capped || after > bounds.decoded_values;
        if (!capped) {
            ++tiers.within_cap;
        } else if (after <= bounds.compared_values) {
            ++tiers.compared;
        } else if (after <= bounds.single_decoded_values) {
            ++tiers.decoded_once;
        } else {
            ++tiers.not_examined;
        }
        seen += values.size();
        offset += *taken;
    }
    return tiers;
}

// The costliest input of each bounded and each codec fuzz target, and the inputs the bound's
// proofs replay (fuzz/bound_seeds.hpp), through its oracle with no bounds, so what the fuzz
// targets' caps and budgets leave out of one input is still checked on these. Each must sit
// where it was built to, its codec blocks in the tiers named for it, so it keeps showing the
// bound it stands for. One test a seed, so check.sh's limit for one test holds each alone.
class BoundSeeds : public ::testing::TestWithParam<std::size_t> {};

TEST_P(BoundSeeds, PassTheWholeCheck) {
    using bound_seeds::Side;
    const auto& seed = bound_seeds::kBoundSeeds.at(GetParam());
    const std::vector<std::byte> bytes = exact_copy(seed.make());
    EXPECT_LE(bytes.size(), seed.max_len);
    if (seed.side == Side::codec_tiers) {
        ASSERT_NE(seed.codec, nullptr);
        ASSERT_NE(seed.bounds, nullptr);
        EXPECT_EQ(tiers_of(*seed.codec, bytes, *seed.bounds), seed.tiers);
    } else if (seed.side != Side::unbounded) {
        // Read as the fuzz targets read them, with CRCs recomputed: a query seed's footer CRC
        // field holds the width that steers its last request (bound_seeds::dearest_steering).
        const std::vector<std::byte> fixed = store::oracle::fixed_checksums(bytes);
        const auto reader = store::ItchStoreReader::open_bytes(fixed);
        ASSERT_TRUE(reader.has_value());
        const std::uint64_t stated = store::oracle::detail::stated_values(*reader);
        const std::uint64_t work = store::oracle::query_work(*reader, store::oracle::kWholeCheckWork);
        const std::uint64_t compared = store::oracle::query_work(*reader, store::oracle::kComparedAsksWork);
        switch (seed.side) {
            case Side::within_store_cap:
                EXPECT_LE(stated, store::oracle::kStoreFuzzBounds.stated_values);
                break;
            case Side::store_compared:
                EXPECT_GT(stated, store::oracle::kStoreFuzzBounds.stated_values);
                EXPECT_LE(stated, store::oracle::kStoreFuzzBounds.compared_values);
                break;
            case Side::past_query_budget:
                EXPECT_LE(work, store::oracle::kQueryFuzzBounds.work);
                EXPECT_GT(work, store::oracle::kQueryFuzzBounds.cross_checked_work);
                break;
            case Side::within_query_budget:
                EXPECT_LE(work, store::oracle::kQueryFuzzBounds.cross_checked_work);
                break;
            case Side::query_compared:
                EXPECT_GT(work, store::oracle::kQueryFuzzBounds.work);
                EXPECT_LE(compared, store::oracle::kQueryFuzzBounds.compared_work);
                break;
            case Side::codec_tiers:
            case Side::unbounded:
                break;
        }
    }
    EXPECT_EQ(text(seed.check(bytes)), "");
}

// Named for the target, and for the seed where a target has more than one:
// .../pfor_delta_past_cap, .../itch_query_within_budget.
INSTANTIATE_TEST_SUITE_P(
    FuzzRegression, BoundSeeds,
    ::testing::Range<std::size_t>(0, bound_seeds::kBoundSeeds.size()),
    [](const ::testing::TestParamInfo<std::size_t>& info) {
        const auto& seed = bound_seeds::kBoundSeeds.at(info.param);
        std::string name{seed.target.substr(std::string_view{"fuzz_"}.size())};
        std::string_view rest = seed.name.substr(std::string_view{"bound-"}.size());
        if (rest.starts_with("costliest")) {
            rest.remove_prefix(std::min(rest.size(), std::string_view{"costliest-"}.size()));
        }
        if (!rest.empty()) {
            name += "_" + std::string{rest};
        }
        std::ranges::replace(name, '-', '_');
        return name;
    });

// The fixed grid (fuzz/README.md, "Bounds"): what a codec fuzz target's cap leaves out of
// a block, the unit suite checks here. One block of each size between the cap and
// kMaxBlockValues, just past the cap, each power of two there with one either side, and
// kMaxBlockValues, goes through the target's whole oracle with no bounds: both kernel sets,
// every re-encode, the block checks and the column checks on all its values. PFOR's and
// run-length's caps are one maximal block, so only the dictionary has such sizes. Its blocks
// come in each mode, and each carries every nested PFOR block, its dictionary and, but for
// Huffman codes, its indices, under one transform, with the adaptive scale or without it,
// re-encoded by hand from what the encoder wrote, since a valid block may carry any. Their values are dense, as a
// real column's, so the blocks are far larger than -max_len and no fuzz target reaches
// them. Each point is one test, so check.sh's limit for one test holds each alone. The sizes
// follow the dictionary's cap constant (kDictFuzzDecodedValues); at a cap of one maximal block
// there are none. fuzz_dictionary compares the kernel sets on blocks of these sizes too, since
// its compare limit reaches a maximal block past the cap; the grid runs every other check on
// them.
static_assert(codec::oracle::kPforFuzzDecodedValues >= codec::kMaxBlockValues &&
                  codec::oracle::kRleFuzzDecodedValues >= codec::kMaxBlockValues,
              "a codec cap below one maximal block needs its sizes in the grid");

struct GridPoint {
    codec::DictMode mode;
    codec::IntTransform transform;
    bool scale;
    std::size_t values;
};

[[nodiscard]] std::vector<std::size_t> grid_sizes(std::size_t cap) {
    std::vector<std::size_t> sizes;
    if (cap >= codec::kMaxBlockValues) {
        return sizes;
    }
    sizes.push_back(cap + 1);
    for (std::size_t power = 1; power <= codec::kMaxBlockValues; power *= 2) {
        for (const std::size_t size : {power - 1, power, power + 1}) {
            if (size > cap + 1 && size <= codec::kMaxBlockValues) {
                sizes.push_back(size);
            }
        }
    }
    std::ranges::sort(sizes);
    sizes.erase(std::ranges::unique(sizes).begin(), sizes.end());
    return sizes;
}

[[nodiscard]] std::vector<GridPoint> grid_points() {
    std::vector<GridPoint> points;
    for (const std::size_t values : grid_sizes(codec::oracle::kDictFuzzDecodedValues)) {
        for (const auto mode : {codec::DictMode::ranked_pfor, codec::DictMode::ranked_huffman,
                                codec::DictMode::sorted_move_to_front}) {
            for (const auto transform :
                 {codec::IntTransform::none, codec::IntTransform::delta,
                  codec::IntTransform::delta_of_delta, codec::IntTransform::head_delta}) {
                for (const bool scale : {true, false}) {
                    points.push_back({mode, transform, scale, values});
                }
            }
        }
    }
    return points;
}

[[nodiscard]] std::uint64_t mixed(std::uint64_t x) {
    x += 0x9E37'79B9'7F4A'7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return x ^ (x >> 31U);
}

// Values the mode wins on, each a multiple of 7, so the dictionary's entries take a scale where
// it is allowed. Ranked PFOR, which the encoder is held to: 50,000 values, near uniform. Ranked
// Huffman: a few values, each half as frequent as the one before. Move-to-front: 700 values,
// each repeated before the next.
[[nodiscard]] std::vector<std::uint64_t> grid_values(codec::DictMode mode, std::size_t count) {
    std::vector<std::uint64_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t noise = mixed(i);
        switch (mode) {
            case codec::DictMode::ranked_pfor:
                values[i] = 7 * (noise % 50'000);
                break;
            case codec::DictMode::ranked_huffman:
                values[i] = 7 * (1'000 + static_cast<std::uint64_t>(
                                             std::countr_zero(noise | (std::uint64_t{1} << 40U))));
                break;
            case codec::DictMode::sorted_move_to_front:
                values[i] = 7 * (1'000 + ((i / 3) % 700) + (noise % 2));
                break;
        }
    }
    return values;
}

[[nodiscard]] std::uint64_t u32_at(std::span<const std::byte> bytes, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 4; i > 0; --i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[at + i - 1]);
    }
    return value;
}

// The nested PFOR block at the front of `nested`, decoded and written again under
// `transform`, with the adaptive scale if `scale`, appended to `out`; returns the bytes it
// took, or 0 if refused.
[[nodiscard]] std::size_t rewrite_nested(std::span<const std::byte> nested,
                                         codec::IntTransform transform, bool scale, Bytes& out) {
    std::vector<std::uint64_t> values;
    const auto taken = codec::decode_pfor_block(nested, values);
    if (!taken.has_value() ||
        !codec::encode_pfor_block(
             values, codec::PforOptions{.transform = transform, .adaptive_scale = scale, .patch = true},
             out)
             .has_value()) {
        return 0;
    }
    return *taken;
}

class OracleGrid : public ::testing::TestWithParam<GridPoint> {};

TEST_P(OracleGrid, PassTheWholeCheck) {
    constexpr std::size_t kModeAt = 0;
    constexpr std::size_t kEntriesAt = 1;
    constexpr std::size_t kDictionaryAt = 5;
    const GridPoint& point = GetParam();
    const auto mode = static_cast<unsigned>(point.mode);
    // Ranked PFOR, and that mode with the one wanted: the encoder takes the smaller.
    Bytes written;
    ASSERT_TRUE(codec::encode_dict_block(grid_values(point.mode, point.values), written, nullptr,
                                         static_cast<codec::DictModeSet>(1U | (1U << mode)))
                    .has_value());
    const std::span<const std::byte> payload = std::span{written}.subspan(codec::kBlockHeaderSize);
    ASSERT_EQ(std::to_integer<unsigned>(payload[kModeAt]), mode);
    // The mode byte and the entry count, then each nested block under the transform.
    Bytes rewritten(payload.begin(), payload.begin() + kDictionaryAt);
    std::vector<std::size_t> nested_at{rewritten.size()};
    const std::size_t dictionary = rewrite_nested(payload.subspan(kDictionaryAt), point.transform, point.scale, rewritten);
    ASSERT_NE(dictionary, 0U);
    if (point.mode == codec::DictMode::ranked_huffman) {
        rewritten.insert(rewritten.end(), payload.begin() + static_cast<std::ptrdiff_t>(kDictionaryAt + dictionary),
                         payload.end());
    } else {
        nested_at.push_back(rewritten.size());
        const std::size_t indices =
            rewrite_nested(payload.subspan(kDictionaryAt + dictionary), point.transform, point.scale, rewritten);
        ASSERT_EQ(kDictionaryAt + dictionary + indices, payload.size());
    }
    const Bytes block = bound_seeds::framed(point.values, rewritten);
    // The block's mode byte, its entry count, and each nested block's transform byte.
    ASSERT_EQ(std::to_integer<unsigned>(block.at(codec::kBlockHeaderSize + kModeAt)), mode);
    ASSERT_EQ(u32_at(block, codec::kBlockHeaderSize + kEntriesAt), u32_at(payload, kEntriesAt));
    for (const std::size_t at : nested_at) {
        ASSERT_EQ(std::to_integer<unsigned>(block.at(codec::kBlockHeaderSize + at + codec::kBlockHeaderSize)),
                  static_cast<unsigned>(point.transform));
    }
    ASSERT_EQ(codec::stated_block_count(block), point.values);
    EXPECT_EQ(text(codec::oracle::check_dict_column(exact_copy(block))), "");
}

// The test's name: the mode, the nested blocks' transform and scale, and the block's values.
std::string grid_name(const ::testing::TestParamInfo<GridPoint>& info) {
    static constexpr std::array<std::string_view, 3> kModes{"ranked_pfor", "ranked_huffman",
                                                            "move_to_front"};
    static constexpr std::array<std::string_view, 4> kTransforms{"none", "delta", "delta_of_delta",
                                                                 "head_delta"};
    const GridPoint& point = info.param;
    return std::string{kModes.at(static_cast<std::size_t>(point.mode))} + "_" +
           std::string{kTransforms.at(static_cast<std::size_t>(point.transform))} +
           (point.scale ? "_scaled_" : "_unscaled_") + std::to_string(point.values);
}

// Not among the scalar-capped parts (tests/unit/CMakeLists.txt): there the chosen set is the
// scalar set, so the kernel comparison would compare the scalar set with itself.
INSTANTIATE_TEST_SUITE_P(Grid, OracleGrid, ::testing::ValuesIn(grid_points()), grid_name);
// A cap of one maximal block leaves the grid no sizes.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleGrid);

TEST(FuzzOracles, PassEveryFixtureWholeAndCutAtEveryLength) {
    for (const auto& f : fixtures()) {
        SCOPED_TRACE(f.label);
        for (std::size_t length = 0; length <= f.length; ++length) {
            const Bytes payload = exact_copy(std::span{f.bytes}.first(length));
            EXPECT_EQ(text(oracle::check_message(payload)), "") << "length " << length;
        }
    }
}

TEST(FuzzOracles, PassEveryWayAStreamCanEnd) {
    Bytes stream;
    for (const auto& f : fixtures()) {
        const Bytes one = test::framed(static_cast<std::uint16_t>(f.length), f.bytes);
        stream.insert(stream.end(), one.begin(), one.end());
    }
    for (std::size_t length = 0; length <= stream.size(); ++length) {
        const Bytes cut = exact_copy(std::span{stream}.first(length));
        EXPECT_EQ(text(oracle::check_stream(cut)), "") << "length " << length;
    }

    Bytes ended = stream;
    ended.push_back(std::byte{0});
    ended.push_back(std::byte{0});
    EXPECT_EQ(text(oracle::check_stream(exact_copy(ended))), "");
    ended.push_back(std::byte{0x41});
    EXPECT_EQ(text(oracle::check_stream(exact_copy(ended))), "");
}

// A frame whose prefix has both bytes set, 0x0101, and so both halves of the length
// read (review A-1). The oracle passes the cursor's stops on it; that it refuses a
// zero-length stop there, which would drop a whole frame, is shown by the test below.
TEST(FuzzOracles, PassAFrameOf257Bytes) {
    const Bytes payload(257, std::byte{0x41});
    Bytes stream = test::framed(0x0101, payload);
    EXPECT_EQ(text(oracle::check_stream(exact_copy(stream))), "");
    stream.push_back(std::byte{0x01});
    EXPECT_EQ(text(oracle::check_stream(exact_copy(stream))), "");
}

// That frame alone: the input ends cleanly, and its first byte, 0x01, cuts the shorter input
// to 1 byte, so only the shorter cursor stops early, at a truncated prefix. A cursor that
// misstated that stop is seen here by the shorter input's checks alone (review RA-2, RR-3).
TEST(FuzzOracles, TheFrameOracleChecksTheShorterInputsStop) {
    const Bytes stream = test::framed(0x0101, Bytes(257, std::byte{0x41}));
    ASSERT_EQ(std::to_integer<std::size_t>(stream.front()) % stream.size(), 1U);
    EXPECT_EQ(text(oracle::check_stream(exact_copy(stream))), "");
}

// Stops no correct cursor makes, each over the tail it claims to describe: every one must
// be refused, by the check named, and the right stop over the same tail accepted. The
// cursor is inline and constexpr, so only check_stop can be handed a wrong stop (review
// RA-1): without these, deleting a stop check passed every test.
TEST(FuzzOracles, TheFrameOracleRefusesAStopTheBytesDoNotJustify) {
    using enum FrameStopKind;
    Bytes whole = test::framed(0x0101, Bytes(257, std::byte{0x41}));
    const Bytes short_body = test::bytes({0x00, 0x04, 'x', 'x', 'x'});
    const Bytes whole_body = test::bytes({0x00, 0x04, 'x', 'x', 'x', 'x'});
    const Bytes zero = test::bytes({0x00, 0x00, 'x'});
    const Bytes one = test::bytes({0x01});
    const Bytes two = test::bytes({0x00, 0x04});
    const Bytes none;

    struct Case {
        const char* label;
        FrameStop stop;
        const Bytes& tail;
        std::string refusal;
    };
    const auto cases = std::to_array<Case>({
        {"zero-length on 01 01", {zero_length, 0, 0, 257}, whole,
         "a zero-length stop and a zero prefix do not go together"},
        {"body, wrong expected", {truncated_body, 0, 5, 3}, short_body,
         "a truncated body does not state its prefix's length, or that length is there"},
        {"body, wrong actual", {truncated_body, 0, 4, 2}, short_body,
         "a stop does not state the bytes after its prefix"},
        {"body, whole body there", {truncated_body, 0, 4, 4}, whole_body,
         "the cursor reported a truncated body with the whole body present"},
        {"prefix, 2 bytes left", {truncated_prefix, 0, 2, 1}, two,
         "the cursor reported a truncated prefix with a whole prefix left"},
        {"prefix, wrong numbers", {truncated_prefix, 0, 2, 2}, one,
         "a truncated prefix does not state 2 bytes needed and the 1 left"},
        {"zero-length, states bytes needed", {zero_length, 0, 2, 1}, zero,
         "a zero-length stop states bytes needed"},
        {"zero-length, wrong actual", {zero_length, 0, 0, 0}, zero,
         "a stop does not state the bytes after its prefix"},
        {"body over one byte", {truncated_body, 0, 2, 0}, one,
         "the cursor read a prefix that is not there"},
        {"end with bytes left", {end, 0, 0, 0}, one, "the cursor reported end with bytes left"},
        {"end, states bytes needed", {end, 0, 1, 0}, none,
         "an end stop states bytes needed or bytes left"},
        {"end, states bytes left", {end, 0, 0, 1}, none,
         "an end stop states bytes needed or bytes left"},
        {"fault with no bytes", {zero_length, 0, 0, 0}, none,
         "the cursor reported a fault with no bytes left"},
    });
    for (const Case& c : cases) {
        SCOPED_TRACE(c.label);
        EXPECT_EQ(text(oracle::check_stop(c.stop, c.tail)), c.refusal);
    }

    const auto right = std::to_array<std::pair<FrameStop, const Bytes&>>({
        {{zero_length, 0, 0, 1}, zero},
        {{truncated_body, 0, 4, 3}, short_body},
        {{truncated_body, 0, 4, 0}, two},
        {{truncated_prefix, 0, 2, 1}, one},
        {{end, 0, 0, 0}, none},
    });
    for (const auto& [stop, tail] : right) {
        EXPECT_EQ(text(oracle::check_stop(stop, tail)), "") << to_string(stop);
    }
    whole.pop_back();
    EXPECT_EQ(text(oracle::check_stop({truncated_body, 0, 257, 256}, whole)), "");
}

// A real export, as fuzz_columns' sections, cut at every length of the whole input and
// with every bit flipped in turn.
TEST(FuzzOracles, PassEveryCutAndFlipOfAColumnDirectory) {
    const Bytes source = tick_store::test::source_with_tail();
    const auto directory = tick_store::test::export_columns(source);
    const Bytes input = store::oracle::pack_columns(directory.manifest, directory.files);
    EXPECT_EQ(text(store::oracle::check_columns(exact_copy(input))), "");
    for (std::size_t length = 0; length < input.size(); length += 7) {
        const Bytes cut = exact_copy(std::span{input}.first(length));
        EXPECT_EQ(text(store::oracle::check_columns(cut)), "") << "length " << length;
    }
    Bytes flipped = input;
    for (std::size_t byte = 0; byte < flipped.size(); byte += 3) {
        const unsigned bit = byte % 8U;
        flipped[byte] ^= static_cast<std::byte>(1U << bit);
        EXPECT_EQ(text(store::oracle::check_columns(flipped)), "") << "byte " << byte;
        flipped[byte] ^= static_cast<std::byte>(1U << bit);
    }
}

// The columns oracle's check of a frame from the parser refuses one too short to hold its
// own size without reading past it (B-5 of the final review, docs/REVIEW.md): no input gives
// such a frame today, but the oracle is there for the day the parser does. Empty, and one
// byte in a heap block of its own, so ASan sees a read past it.
TEST(FuzzOracles, TheColumnsFrameCheckRefusesAFrameTooShortToStateItsSize) {
    EXPECT_FALSE(store::oracle::frame_states_its_size(std::span<const std::byte>{}));
    EXPECT_FALSE(store::oracle::frame_states_its_size(exact_copy(Bytes(1, std::byte{0}))));
    EXPECT_FALSE(store::oracle::frame_states_its_size(exact_copy(Bytes(2, std::byte{0}))));
    EXPECT_TRUE(store::oracle::frame_states_its_size(
        exact_copy(Bytes{std::byte{0}, std::byte{1}, std::byte{0x41}})));
    EXPECT_FALSE(store::oracle::frame_states_its_size(
        exact_copy(Bytes{std::byte{0}, std::byte{2}, std::byte{0x41}})));
}

// The golden container, cut at every length and with every bit flipped in turn, and a
// copy with every CRC wrong, which only the CRC checks refuse.
TEST(FuzzOracles, PassEveryCutAndFlipOfAContainer) {
    const Bytes file = exact_copy(store::test::read_bytes(store::test::golden_path()));
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(text(store::oracle::check_container(file)), "");
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_container(cut)), "") << "length " << length;
    }
    for (std::size_t bit = 0; bit < file.size() * 8; ++bit) {
        Bytes flipped = exact_copy(file);
        flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        EXPECT_EQ(text(store::oracle::check_container(flipped)), "") << "bit " << bit;
    }
    Bytes stale = exact_copy(file);
    for (const std::size_t at : {store::test::golden_block_entry(0) + store::kBlockCrc,
                                 store::test::golden_section_entry(0) + store::kSectionCrc,
                                 store::test::kGoldenTrailer + store::kTrailerFooterCrc}) {
        stale[at] ^= std::byte{0xFF};
    }
    EXPECT_EQ(text(store::oracle::check_container(stale)), "");
    EXPECT_EQ(store::oracle::fixed_checksums(stale), file);
}

// The golden ITCH store, cut at every length and with one bit of every byte flipped, the
// bit chosen by the byte's offset; and the same file with segments over the fuzzing cap.
TEST(FuzzOracles, PassEveryCutAndFlipOfAnItchStore) {
    const Bytes file =
        read_file(std::filesystem::path{TICK_STORE_GOLDEN_DIR} / "itch-store-v3.tsc");
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(text(store::oracle::check_itch_store(file)), "");
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_itch_store(cut)), "") << "length " << length;
    }
    for (std::size_t byte = 0; byte < file.size(); ++byte) {
        Bytes flipped = exact_copy(file);
        flipped[byte] ^= static_cast<std::byte>(1U << (byte % 8));
        EXPECT_EQ(text(store::oracle::check_itch_store(flipped)), "") << "byte " << byte;
    }
}

// The query oracle on the golden store cut and flipped throughout, and
// on a store of several seek groups a segment whose index columns each have a byte changed.
TEST(FuzzOracles, PassEveryCutAndFlipThroughTheQueryOracle) {
    const Bytes file =
        read_file(std::filesystem::path{TICK_STORE_GOLDEN_DIR} / "itch-store-v3.tsc");
    ASSERT_FALSE(file.empty());
    EXPECT_EQ(text(store::oracle::check_itch_query(file)), "");
    // Every 11th length and every 5th byte: the whole decode, which most cuts and flips
    // still pass under recomputed CRCs, makes each call cost a full set of queries twice.
    for (std::size_t length = 0; length < file.size(); length += 11) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_itch_query(cut)), "") << "length " << length;
    }
    for (std::size_t byte = 0; byte < file.size(); byte += 5) {
        Bytes flipped = exact_copy(file);
        flipped[byte] ^= static_cast<std::byte>(1U << (byte % 8));
        EXPECT_EQ(text(store::oracle::check_itch_query(flipped)), "") << "byte " << byte;
    }
    std::vector<Message> messages;
    for (std::uint64_t i = 0; i < 9'000; ++i) {
        const auto locate = static_cast<std::uint16_t>(1 + (i % 4));
        const MessageHeader header{locate, 0, 34'200'000'000'000ULL + (i * 1'000)};
        if (i % 3 == 2) {
            messages.emplace_back(OrderDelete{header, 1'000 + i - 2});
        } else {
            messages.emplace_back(AddOrder{header, 1'000 + i, std::byte{'B'}, 100,
                                           store::test::stock_of("QRY"), Price4{100'000}});
        }
    }
    const Bytes store_file = store::test::store_of_messages(messages, 4'500);
    EXPECT_EQ(text(store::oracle::check_itch_query(store_file)), "");
    for (const store::Column column :
         {store::Column::seek, store::Column::locate_run, store::Column::locate_run_start,
          store::Column::locate_run_add_start, store::Column::locate_run_import_start,
          store::Column::locate, store::Column::time_base, store::Column::timestamp}) {
        for (const std::size_t at : {std::size_t{0}, std::size_t{12}, std::size_t{60}}) {
            store::test::StoreParts parts = store::test::parts_of(store_file);
            Bytes& payload = parts.block(0, column).bytes;
            if (at < payload.size()) {
                payload[at] ^= std::byte{0x04};
            }
            EXPECT_EQ(text(store::oracle::check_itch_query(parts.build())), "")
                << store::column_info(column).name << " byte " << at;
        }
    }
}

// A segment of raw records alone, between two typed ones: no typed record of it is in any
// window, so only the row before it, which its raw rows repeat the time of, says whether a
// query must read it. Both query paths choose segments with the same may_hold and seed
// them with the same seed_before, so comparing them cannot see either go wrong (review
// C-5); the oracle's column form from the whole decode can. A fault that skips such a
// segment (may_hold answering from its typed records alone) fails this test.
TEST(FuzzOracles, TheQueryOracleSeesASegmentBothPathsWouldSkip) {
    static const Bytes raw = test::bytes({'z', 1, 2, 3});
    std::vector<Message> messages;
    messages.reserve(12);
    for (std::uint64_t i = 0; i < 4; ++i) {
        messages.emplace_back(AddOrder{MessageHeader{1, 0, 1'000 + i}, 10 + i, std::byte{'B'}, 100,
                                       store::test::stock_of("QRY"), Price4{100'000}});
    }
    for (std::uint64_t i = 0; i < 4; ++i) {
        messages.emplace_back(RawMessage{.bytes = raw, .reason = RawReason::unknown_type});
    }
    for (std::uint64_t i = 0; i < 4; ++i) {
        messages.emplace_back(AddOrder{MessageHeader{2, 0, 2'000 + i}, 20 + i, std::byte{'S'}, 100,
                                       store::test::stock_of("QRY"), Price4{100'000}});
    }
    const Bytes file = store::test::store_of_messages(messages, 4);
    const auto reader = store::ItchStoreReader::open_bytes(file);
    ASSERT_TRUE(reader.has_value());
    ASSERT_EQ(reader->segment_count(), 3U);
    ASSERT_EQ(reader->segment(1).summary.typed_records, 0U);
    EXPECT_EQ(text(store::oracle::check_itch_query(file)), "");
}

// A real store, cut at every length and with every bit flipped in turn.
TEST(FuzzOracles, PassEveryCutAndFlipOfAStore) {
    const tick_store::test::ScratchDir dir;
    const Bytes source = tick_store::test::source_with_tail();
    const Bytes file =
        tick_store::test::store_bytes(tick_store::test::messages_of(source),
                                      tick_store::test::frames_of(source).tail, dir / "store");
    EXPECT_EQ(text(store::oracle::check_store(exact_copy(file))), "");
    for (std::size_t length = 0; length < file.size(); ++length) {
        const Bytes cut = exact_copy(std::span{file}.first(length));
        EXPECT_EQ(text(store::oracle::check_store(cut)), "") << "length " << length;
    }
    for (std::size_t bit = 0; bit < file.size() * 8; ++bit) {
        Bytes flipped = exact_copy(file);
        flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        EXPECT_EQ(text(store::oracle::check_store(flipped)), "") << "bit " << bit;
    }
}

// A column from each Gorilla encoder, whole, cut at every length and with every bit
// flipped in turn, through the fuzz targets' oracles. A flipped column must be refused or
// re-encode to the flipped bytes, which the oracle checks.
TEST(FuzzOracles, PassEveryCutAndFlipOfAGorillaColumn) {
    std::vector<std::uint64_t> timestamps{34'200'000'000'000ULL};
    std::vector<std::uint64_t> dollars;
    std::uint64_t price = 1'234'500;
    for (std::uint64_t i = 1; i < 40; ++i) {
        timestamps.push_back(timestamps.back() + ((i * 7'919U) % 60'000U) + (i % 5 == 0 ? 0 : 1));
        price = price + ((i * 37U) % 11U * 100U) - 500U;
        dollars.push_back(std::bit_cast<std::uint64_t>(
            codec::price4_to_dollars(static_cast<std::uint32_t>(price))));
    }
    Bytes timestamp_column;
    ASSERT_TRUE(codec::encode_timestamps(timestamps, 16, timestamp_column).has_value());
    Bytes xor_column;
    ASSERT_TRUE(codec::encode_xor(dollars, 16, xor_column).has_value());

    using Check = const char* (*)(std::span<const std::byte>);
    for (const auto& [column, check] :
         {std::pair<const Bytes&, Check>{timestamp_column, &codec::oracle::check_timestamp_column},
          std::pair<const Bytes&, Check>{xor_column, &codec::oracle::check_xor_column}}) {
        EXPECT_EQ(text(check(exact_copy(column))), "");
        for (std::size_t length = 0; length < column.size(); ++length) {
            EXPECT_EQ(text(check(exact_copy(std::span{column}.first(length)))), "")
                << "length " << length;
        }
        for (std::size_t bit = 0; bit < column.size() * 8; ++bit) {
            Bytes flipped = exact_copy(column);
            flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            EXPECT_EQ(text(check(flipped)), "") << "bit " << bit;
        }
    }
}

// A column from each integer encoder, whole, cut at every length and with every bit
// flipped in turn, through the fuzz targets' oracles. These formats leave the encoder
// choices, so a flipped column must be refused or decode to values that every encoder
// round-trips, which the oracle checks.
TEST(FuzzOracles, PassEveryCutAndFlipOfAnIntegerColumn) {
    std::vector<std::uint64_t> values{34'200'000'000'000ULL};
    for (std::uint64_t i = 1; i < 40; ++i) {
        values.push_back(values.back() + ((i * 7'919U) % 60'000U) + (i == 20 ? 1ULL << 36U : 0));
    }
    std::vector<std::uint64_t> prices;
    prices.reserve(40);
    for (std::uint64_t i = 0; i < 40; ++i) {
        prices.push_back(i == 17 ? 1'999'999'900U : 1'234'500U + ((i * 37U) % 11U * 100U));
    }
    Bytes timestamp_column;
    ASSERT_TRUE(codec::encode_pfor(values, 16, codec::PforOptions{}, timestamp_column).has_value());
    Bytes price_column;
    ASSERT_TRUE(codec::encode_pfor(prices, 16,
                                   codec::PforOptions{.transform = codec::IntTransform::none,
                                                      .adaptive_scale = true,
                                                      .patch = true},
                                   price_column)
                    .has_value());
    Bytes svb_column;
    ASSERT_TRUE(codec::encode_svb(values, 16, true, svb_column).has_value());
    Bytes head_column;
    ASSERT_TRUE(codec::encode_pfor(values, 16,
                                   codec::PforOptions{.transform = codec::IntTransform::head_delta,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                   head_column)
                    .has_value());
    // Few distinct values with runs, as shares and flags are: one dictionary column in each
    // mode the encoder takes at this size, and one run-length column.
    std::vector<std::uint64_t> shares;
    shares.reserve(40);
    for (std::uint64_t i = 0; i < 40; ++i) {
        std::uint64_t size = 100;
        if (i % 5 == 0) {
            size = 200;
        } else if (i % 13 == 0) {
            size = 37;
        }
        shares.push_back(size);
    }
    Bytes dict_column;
    ASSERT_TRUE(codec::encode_dict(shares, 16, dict_column).has_value());
    std::vector<std::uint64_t> runs;
    runs.reserve(40);
    for (std::uint64_t i = 0; i < 40; ++i) {
        runs.push_back(i / 7 % 3);
    }
    Bytes rle_column;
    ASSERT_TRUE(codec::encode_rle(runs, 16, rle_column).has_value());

    using Check = const char* (*)(std::span<const std::byte>);
    for (const auto& [column, check] :
         {std::pair<const Bytes&, Check>{timestamp_column, &codec::oracle::check_pfor_column},
          std::pair<const Bytes&, Check>{price_column, &codec::oracle::check_pfor_column},
          std::pair<const Bytes&, Check>{head_column, &codec::oracle::check_pfor_column},
          std::pair<const Bytes&, Check>{svb_column, &codec::oracle::check_svb_column},
          std::pair<const Bytes&, Check>{dict_column, &codec::oracle::check_dict_column},
          std::pair<const Bytes&, Check>{rle_column, &codec::oracle::check_rle_column}}) {
        EXPECT_EQ(text(check(exact_copy(column))), "");
        for (std::size_t length = 0; length < column.size(); ++length) {
            EXPECT_EQ(text(check(exact_copy(std::span{column}.first(length)))), "")
                << "length " << length;
        }
        for (std::size_t bit = 0; bit < column.size() * 8; ++bit) {
            Bytes flipped = exact_copy(column);
            flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            EXPECT_EQ(text(check(flipped)), "") << "bit " << bit;
        }
    }

    // One full vector of 1,024 values and a one-value tail, so the checks, kernel against
    // scalar among them, reach the interleaved vector path and AVX2's fused delta, not only
    // the linear tails the blocks of 16 above take (B-7 of the final review,
    // docs/REVIEW.md): timestamps by delta with no exceptions, and prices by frame of
    // reference with one exception in the vector. A cut at each of the first 24 bytes, which
    // hold the block's and the vector's fields, and a flip every 7 bits there, so each byte
    // gets one; then a cut every 97 bytes and a flip every 257 bits through the packed words.
    // Each check re-encodes 1,025 values in every option set, so this samples rather than
    // trying every bit as the columns above do.
    // The first delta is the first value, so it starts small enough to need no exception.
    std::vector<std::uint64_t> times{40};
    for (std::uint64_t i = 1; i < 1'025; ++i) {
        times.push_back(times.back() + 1 + ((i * 7'919U) % 61U));
    }
    std::vector<std::uint64_t> vector_prices;
    vector_prices.reserve(1'025);
    for (std::uint64_t i = 0; i < 1'025; ++i) {
        vector_prices.push_back(i == 500 ? 1'999'999'900U : 1'234'500U + ((i * 37U) % 11U * 100U));
    }
    Bytes time_vector;
    codec::PforStats time_stats;
    ASSERT_TRUE(codec::encode_pfor(times, 1'025,
                                   codec::PforOptions{.transform = codec::IntTransform::delta,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                   time_vector, &time_stats)
                    .has_value());
    Bytes price_vector;
    codec::PforStats price_stats;
    ASSERT_TRUE(codec::encode_pfor(vector_prices, 1'025,
                                   codec::PforOptions{.transform = codec::IntTransform::none,
                                                      .adaptive_scale = false,
                                                      .patch = true},
                                   price_vector, &price_stats)
                    .has_value());
    // A full vector and a one-value tail each, the timestamps' vector with no exceptions,
    // which AVX2 decodes fused, and the prices' with its one.
    for (const codec::PforStats& stats : {time_stats, price_stats}) {
        EXPECT_EQ(stats.full_vectors, 1U);
        EXPECT_EQ(stats.tail_values, 1U);
    }
    EXPECT_EQ(time_stats.exceptions, 0U);
    EXPECT_EQ(price_stats.exceptions, 1U);
    for (const Bytes& column : {time_vector, price_vector}) {
        EXPECT_EQ(text(codec::oracle::check_pfor_column(exact_copy(column))), "");
        for (std::size_t length = 0; length < column.size(); length += length < 24 ? 1 : 97) {
            const Bytes cut = exact_copy(std::span{column}.first(length));
            EXPECT_EQ(text(codec::oracle::check_pfor_column(cut)), "") << "length " << length;
        }
        constexpr std::size_t kFieldBits = std::size_t{24} * 8U;
        for (std::size_t bit = 0; bit < column.size() * 8; bit += bit < kFieldBits ? 7 : 257) {
            Bytes flipped = exact_copy(column);
            flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            EXPECT_EQ(text(codec::oracle::check_pfor_column(flipped)), "") << "bit " << bit;
        }
    }
}

}  // namespace
}  // namespace tick_store::itch
