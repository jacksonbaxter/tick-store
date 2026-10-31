#pragma once

// A directory for one test, under the build tree, removed when the test ends. The
// name comes from the running test and the process, so tests can run in parallel.

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <format>
#include <system_error>

namespace tick_store::test {

class ScratchDir {
public:
    ScratchDir() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = std::filesystem::path{TICK_STORE_TEST_SCRATCH_DIR} /
                std::format("{}-{}-{}", info->test_suite_name(), info->name(), ::getpid());
        std::filesystem::create_directories(path_);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path operator/(const char* name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

}  // namespace tick_store::test
