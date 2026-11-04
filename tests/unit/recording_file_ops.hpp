#pragma once

// A FileOps that records every call and can make any one of them fail, or run a hook
// in its place, such as ending the process to stand for a crash. By default each call
// goes on to the real POSIX one, so files really are written.

#include "tick_store/store/durability.hpp"

#include <cerrno>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tick_store::store::test {

class RecordingFileOps final : public FileOps {
public:
    // Every call in order. A flush or close names what it was given, "file" or
    // "directory", as in "full_fsync file" or "close directory".
    std::vector<std::string> calls;

    // When false, a flush is recorded and succeeds without reaching the kernel, so a
    // test can drive the other platform's policy, whose calls this OS may not have.
    bool forward_syncs = true;

    // Each write takes at most this many bytes, to exercise short writes. 0: no limit.
    std::size_t write_limit = 0;

    // The nth call (from 1) named `call` returns -1 with errno `error`.
    void fail_on(const std::string& call, int error, int nth = 1) {
        failures_[{call, nth}] = error;
    }

    // The nth call named `call` runs `hook` first, then goes on as usual.
    void on(const std::string& call, std::function<void()> hook, int nth = 1) {
        hooks_[{call, nth}] = std::move(hook);
    }

    // Calls other than writes, which a test usually wants to see without the data.
    [[nodiscard]] std::vector<std::string> calls_without_writes() const {
        std::vector<std::string> out;
        for (const std::string& call : calls) {
            if (call != "write") {
                out.push_back(call);
            }
        }
        return out;
    }

    [[nodiscard]] int count(const std::string& call) const {
        int n = 0;
        for (const std::string& seen : calls) {
            n += seen == call ? 1 : 0;
        }
        return n;
    }

    int create_file(const char* path) override {
        if (!begin("create_file")) {
            return -1;
        }
        return real().create_file(path);
    }

    int open_directory(const char* path) override {
        if (!begin("open_directory")) {
            return -1;
        }
        const int fd = real().open_directory(path);
        if (fd >= 0) {
            directories_.insert(fd);
        }
        return fd;
    }

    std::ptrdiff_t write(int fd, const std::byte* data, std::size_t size) override {
        if (!begin("write")) {
            return -1;
        }
        const std::size_t taken = write_limit == 0 || size < write_limit ? size : write_limit;
        return real().write(fd, data, taken);
    }

    int full_fsync(int fd) override {
        if (!begin(std::string{"full_fsync "}.append(kind(fd)))) {
            return -1;
        }
        return forward_syncs ? real().full_fsync(fd) : 0;
    }

    int fsync(int fd) override {
        if (!begin(std::string{"fsync "}.append(kind(fd)))) {
            return -1;
        }
        return forward_syncs ? real().fsync(fd) : 0;
    }

    int fdatasync(int fd) override {
        if (!begin(std::string{"fdatasync "}.append(kind(fd)))) {
            return -1;
        }
        return forward_syncs ? real().fdatasync(fd) : 0;
    }

    int close(int fd) override {
        const std::string name = std::string{"close "}.append(kind(fd));
        directories_.erase(fd);
        if (!begin(name)) {
            // The descriptor is released anyway, as POSIX close does on most errors.
            static_cast<void>(real().close(fd));
            errno = failed_errno_;
            return -1;
        }
        return real().close(fd);
    }

    int rename(const char* from, const char* to) override {
        if (!begin("rename")) {
            return -1;
        }
        return real().rename(from, to);
    }

    int unlink(const char* path) override {
        if (!begin("unlink")) {
            return -1;
        }
        return real().unlink(path);
    }

private:
    static FileOps& real() { return posix_file_ops(); }

    [[nodiscard]] std::string_view kind(int fd) const {
        return directories_.contains(fd) ? "directory" : "file";
    }

    // Records the call, runs its hook, and returns false with errno set if it is to fail.
    bool begin(const std::string& call) {
        calls.push_back(call);
        const int nth = ++seen_[call];
        if (const auto hook = hooks_.find({call, nth}); hook != hooks_.end()) {
            hook->second();
        }
        if (const auto failure = failures_.find({call, nth}); failure != failures_.end()) {
            failed_errno_ = failure->second;
            errno = failure->second;
            return false;
        }
        return true;
    }

    std::map<std::pair<std::string, int>, int> failures_;
    std::map<std::pair<std::string, int>, std::function<void()>> hooks_;
    std::map<std::string, int> seen_;
    std::set<int> directories_;
    int failed_errno_ = 0;
};

}  // namespace tick_store::store::test
