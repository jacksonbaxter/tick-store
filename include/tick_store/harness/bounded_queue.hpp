#pragma once

// A blocking multi-producer, multi-consumer queue with a fixed capacity. The
// capacity is what stops a fast producer from buffering a whole day in memory.
//
// Two ways to end it:
//
//   close()   no more pushes. pop() still returns what is queued, then nullopt.
//   cancel()  no more pushes, and what is queued is dropped. This is the one to
//             use on an error: a consumer that has stopped can no longer drain the
//             queue, so a producer blocked on a full queue must be let go.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace tick_store::harness {

template <class T>
class BoundedQueue {
public:
    // `capacity` must be at least 1.
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // Blocks while full. Returns false, dropping `value`, if the queue has ended.
    [[nodiscard]] bool push(T value) {
        std::unique_lock lock{mutex_};
        not_full_.wait(lock, [this] { return ended() || items_.size() < capacity_; });
        if (ended()) {
            return false;
        }
        items_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Blocks while empty and open. nullopt means nothing more will come.
    [[nodiscard]] std::optional<T> pop() {
        std::unique_lock lock{mutex_};
        not_empty_.wait(lock, [this] { return ended() || !items_.empty(); });
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> value{std::move(items_.front())};
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    void close() { end([this] { closed_ = true; }); }

    void cancel() {
        end([this] {
            cancelled_ = true;
            items_.clear();
        });
    }

    [[nodiscard]] bool cancelled() const {
        const std::scoped_lock lock{mutex_};
        return cancelled_;
    }

private:
    [[nodiscard]] bool ended() const noexcept { return closed_ || cancelled_; }

    template <class Change>
    void end(const Change& change) {
        {
            const std::scoped_lock lock{mutex_};
            change();
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    bool closed_ = false;
    bool cancelled_ = false;
};

}  // namespace tick_store::harness
