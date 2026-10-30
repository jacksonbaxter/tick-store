// Replays checked-in crashers through the fuzz targets' own oracles (Z6), and runs
// the golden fixtures through them, so the oracles stay exercised on every build
// even when the crasher directories hold nothing.

#include "itch_fixtures.hpp"
#include "itch_oracles.hpp"
#include "test_bytes.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
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

}  // namespace
}  // namespace tick_store::itch
