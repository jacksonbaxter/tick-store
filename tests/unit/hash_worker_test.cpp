// The project's concurrent code. These tests run under TSan in the tsan preset,
// and each end-of-life path has its own test: a clean finish, a full queue, a
// cancel with a producer blocked on it, and destruction part way through.

#include "tick_store/harness/bounded_queue.hpp"
#include "tick_store/harness/hash_worker.hpp"
#include "tick_store/harness/sha256.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace tick_store::harness {
namespace {

constexpr std::chrono::milliseconds kMoment{2};

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 31U) ^ (i >> 8U));
    }
    return bytes;
}

// Polls, because "blocked" has no event to wait on. It can only fail by reporting
// a blocked producer as still blocked, never the reverse.
template <class Predicate>
bool stays_false_for_a_moment(const Predicate& predicate) {
    for (int i = 0; i < 20; ++i) {
        if (predicate()) {
            return false;
        }
        std::this_thread::sleep_for(kMoment);
    }
    return true;
}

TEST(BoundedQueue, KeepsOrderAndDrainsAfterClose) {
    BoundedQueue<int> queue{4};
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    queue.close();
    EXPECT_FALSE(queue.push(3));
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), std::nullopt);
    EXPECT_EQ(queue.pop(), std::nullopt);
    EXPECT_FALSE(queue.cancelled());
}

TEST(BoundedQueue, CancelDropsWhatIsQueued) {
    BoundedQueue<int> queue{4};
    EXPECT_TRUE(queue.push(1));
    queue.cancel();
    EXPECT_FALSE(queue.push(2));
    EXPECT_EQ(queue.pop(), std::nullopt);
    EXPECT_TRUE(queue.cancelled());
}

TEST(BoundedQueue, FullQueueBlocksTheProducerUntilAPop) {
    BoundedQueue<int> queue{1};
    ASSERT_TRUE(queue.push(1));

    std::atomic<bool> pushed{false};
    std::thread producer{[&] {
        EXPECT_TRUE(queue.push(2));
        pushed = true;
    }};
    EXPECT_TRUE(stays_false_for_a_moment([&] { return pushed.load(); }));

    EXPECT_EQ(queue.pop(), 1);
    producer.join();
    EXPECT_TRUE(pushed);
    EXPECT_EQ(queue.pop(), 2);
}

TEST(BoundedQueue, CancelReleasesABlockedProducer) {
    BoundedQueue<int> queue{1};
    ASSERT_TRUE(queue.push(1));

    std::atomic<bool> refused{false};
    std::thread producer{[&] { refused = !queue.push(2); }};
    EXPECT_TRUE(stays_false_for_a_moment([&] { return refused.load(); }));

    queue.cancel();
    producer.join();
    EXPECT_TRUE(refused);
}

TEST(BoundedQueue, CloseReleasesABlockedConsumer) {
    BoundedQueue<int> queue{1};
    std::thread consumer{[&] { EXPECT_EQ(queue.pop(), std::nullopt); }};
    queue.close();
    consumer.join();
}

TEST(BoundedQueue, ManyProducersAndConsumersLoseNothing) {
    constexpr int kProducers = 4;
    constexpr int kEach = 2000;
    BoundedQueue<int> queue{3};
    std::atomic<long> sum{0};
    std::atomic<int> count{0};

    std::vector<std::thread> consumers;
    consumers.reserve(3);
    for (int c = 0; c < 3; ++c) {
        consumers.emplace_back([&] {
            while (const auto value = queue.pop()) {
                sum += *value;
                ++count;
            }
        });
    }
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 1; i <= kEach; ++i) {
                EXPECT_TRUE(queue.push(i));
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    queue.close();
    for (std::thread& consumer : consumers) {
        consumer.join();
    }
    EXPECT_EQ(count, kProducers * kEach);
    EXPECT_EQ(sum, static_cast<long>(kProducers) * kEach * (kEach + 1) / 2);
}

TEST(ChunkWorker, DeliversEveryByteInOrderInChunksOfTheStatedSize) {
    const auto input = pattern(1000);
    std::vector<std::byte> seen;
    std::vector<std::size_t> sizes;
    ChunkWorker worker{[&](std::span<const std::byte> chunk) {
                           seen.insert(seen.end(), chunk.begin(), chunk.end());
                           sizes.push_back(chunk.size());
                       },
                       {.chunk_bytes = 64, .queue_slots = 2}};
    for (std::size_t offset = 0; offset < input.size(); offset += 7) {
        const std::size_t take = std::min<std::size_t>(7, input.size() - offset);
        worker.update(std::span{input}.subspan(offset, take));
    }
    EXPECT_TRUE(worker.finish());
    EXPECT_EQ(seen, input);
    ASSERT_EQ(sizes.size(), 16U);
    for (std::size_t i = 0; i + 1 < sizes.size(); ++i) {
        EXPECT_EQ(sizes[i], 64U);
    }
    EXPECT_EQ(sizes.back(), 40U);
}

TEST(ChunkWorker, FinishWithNothingGivenDeliversNothing) {
    int chunks = 0;
    ChunkWorker worker{[&](std::span<const std::byte>) { ++chunks; }, {}};
    EXPECT_TRUE(worker.finish());
    EXPECT_EQ(chunks, 0);
}

// One chunk stuck in the consumer, and one in the queue, make the next chunk block.
TEST(ChunkWorker, FullQueueHoldsTheProducerBackUntilTheConsumerMoves) {
    std::latch release{1};
    std::latch consuming{1};
    std::atomic<std::size_t> consumed{0};
    ChunkWorker worker{[&](std::span<const std::byte> chunk) {
                           consuming.count_down();
                           release.wait();
                           consumed += chunk.size();
                       },
                       {.chunk_bytes = 8, .queue_slots = 1}};
    const auto bytes = pattern(8);
    worker.update(bytes);
    consuming.wait();  // the first chunk is in the consumer and has left the queue
    worker.update(bytes);

    std::atomic<bool> done{false};
    std::thread producer{[&] {
        worker.update(bytes);
        done = true;
    }};
    EXPECT_TRUE(stays_false_for_a_moment([&] { return done.load(); }));

    release.count_down();
    producer.join();
    EXPECT_TRUE(done);
    EXPECT_TRUE(worker.finish());
    EXPECT_EQ(consumed, 24U);
}

// The shutdown an error takes: the consumer is stuck, the queue is full, and the
// producer is blocked. cancel() must free the producer without the consumer moving.
TEST(ChunkWorker, CancelFreesABlockedProducerAndDropsTheQueue) {
    std::latch release{1};
    std::latch consuming{1};
    std::atomic<int> chunks{0};
    ChunkWorker worker{[&](std::span<const std::byte>) {
                           ++chunks;
                           consuming.count_down();
                           release.wait();
                       },
                       {.chunk_bytes = 8, .queue_slots = 1}};
    const auto bytes = pattern(8);
    worker.update(bytes);
    consuming.wait();
    worker.update(bytes);

    std::thread producer{[&] { worker.update(bytes); }};
    std::this_thread::sleep_for(kMoment);
    worker.cancel();
    producer.join();

    worker.update(bytes);  // discarded, and must not block
    release.count_down();
    EXPECT_FALSE(worker.finish());
    EXPECT_EQ(chunks, 1);
}

TEST(ChunkWorker, DestructorCancelsAndJoinsWithWorkOutstanding) {
    std::latch release{1};
    std::latch consuming{1};
    std::atomic<int> chunks{0};
    std::thread releaser;
    {
        ChunkWorker worker{[&](std::span<const std::byte>) {
                               ++chunks;
                               consuming.count_down();
                               release.wait();
                           },
                           {.chunk_bytes = 8, .queue_slots = 2}};
        const auto bytes = pattern(8);
        worker.update(bytes);
        consuming.wait();
        worker.update(bytes);
        worker.update(bytes);
        // The join in the destructor needs the consumer to move, so another thread
        // lets it go while the destructor runs. Which side gets there first is not
        // fixed, hence the range below: a stuck join or a leaked thread is the defect.
        releaser = std::thread{[&] {
            std::this_thread::sleep_for(kMoment);
            release.count_down();
        }};
    }
    releaser.join();
    EXPECT_GE(chunks, 1);
    EXPECT_LE(chunks, 3);
}

TEST(HashWorker, MatchesTheOneShotHashAcrossChunkAndQueueSizes) {
    const auto input = pattern(100'000);
    const Sha256Digest expected = sha256(input);
    for (const std::size_t chunk_bytes : {std::size_t{1}, std::size_t{63}, std::size_t{64},
                                          std::size_t{65}, std::size_t{4096}, input.size() + 1}) {
        for (const std::size_t queue_slots : {std::size_t{1}, std::size_t{8}}) {
            if (chunk_bytes == 1 && queue_slots == 1) {
                continue;  // a million lock round trips under TSan prove nothing extra
            }
            HashWorker worker{{.chunk_bytes = chunk_bytes, .queue_slots = queue_slots}};
            for (std::size_t offset = 0; offset < input.size(); offset += 997) {
                const std::size_t take = std::min<std::size_t>(997, input.size() - offset);
                worker.update(std::span{input}.subspan(offset, take));
            }
            EXPECT_EQ(worker.finish(), expected)
                << "chunk " << chunk_bytes << " slots " << queue_slots;
        }
    }
}

TEST(HashWorker, CancelledWorkerHasNoDigest) {
    HashWorker worker{{.chunk_bytes = 16, .queue_slots = 1}};
    worker.update(pattern(10'000));
    worker.cancel();
    EXPECT_EQ(worker.finish(), std::nullopt);
}

TEST(HashWorker, CancelFromAnotherThreadWhileUpdating) {
    HashWorker worker{{.chunk_bytes = 16, .queue_slots = 1}};
    const auto bytes = pattern(1'000);
    std::atomic<bool> stop{false};
    std::thread producer{[&] {
        while (!stop) {
            worker.update(bytes);
        }
    }};
    std::this_thread::sleep_for(kMoment);
    worker.cancel();
    stop = true;
    producer.join();
    EXPECT_EQ(worker.finish(), std::nullopt);
}

TEST(HashWorker, DestructionWithoutFinishIsClean) {
    const auto bytes = pattern(100'000);
    HashWorker worker{{.chunk_bytes = 64, .queue_slots = 2}};
    worker.update(bytes);
}

}  // namespace
}  // namespace tick_store::harness
