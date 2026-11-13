#pragma once

// Holds the process's file-size limit at a few bytes, with SIGXFSZ ignored, so a write past
// it fails with EFBIG, as a full disk would, on every host (macOS has no /dev/full). The
// limit and the handler are put back when it goes out of scope.

#include <gtest/gtest.h>

#include <sys/resource.h>

#include <csignal>

namespace tick_store::test {

class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t bytes) {
        previous_handler_ = std::signal(SIGXFSZ, SIG_IGN);
        EXPECT_EQ(::getrlimit(RLIMIT_FSIZE, &previous_), 0);
        rlimit limit = previous_;
        limit.rlim_cur = bytes;
        EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &limit), 0);
    }

    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;
    FileSizeLimit(FileSizeLimit&&) = delete;
    FileSizeLimit& operator=(FileSizeLimit&&) = delete;

    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous_);
        std::signal(SIGXFSZ, previous_handler_);
    }

private:
    rlimit previous_{};
    void (*previous_handler_)(int) = nullptr;
};

}  // namespace tick_store::test
