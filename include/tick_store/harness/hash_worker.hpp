#pragma once

// Hashing 8 GB alongside the record compare is the project's one concurrent job.
// ChunkWorker is the machinery: the producer's bytes are copied into chunks, the
// chunks go through a bounded queue, and one worker thread consumes them in order.
// HashWorker is ChunkWorker feeding a Sha256.
//
// Threads: update() and finish() belong to one producer thread. cancel() is safe
// from any thread, and is how an error shuts the worker down: it lets go of a
// producer blocked on a full queue, drops what is queued, and the thread ends after
// the chunk it is on. The destructor cancels and joins, so unwinding past a worker
// never leaves a thread running.

#include "tick_store/harness/bounded_queue.hpp"
#include "tick_store/harness/sha256.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace tick_store::harness {

struct ChunkWorkerOptions {
    // Bytes in one chunk.
    std::size_t chunk_bytes = std::size_t{1} << 20U;
    // Chunks that can wait in the queue, so the memory bound is about their product.
    std::size_t queue_slots = 8;
};

class ChunkWorker {
public:
    // Called on the worker thread with each chunk, in order. It must not throw.
    using Consume = std::function<void(std::span<const std::byte>)>;

    ChunkWorker(Consume consume, ChunkWorkerOptions options);
    ~ChunkWorker();

    ChunkWorker(const ChunkWorker&) = delete;
    ChunkWorker& operator=(const ChunkWorker&) = delete;
    ChunkWorker(ChunkWorker&&) = delete;
    ChunkWorker& operator=(ChunkWorker&&) = delete;

    // Blocks while the queue is full. After a cancel it discards `bytes`.
    void update(std::span<const std::byte> bytes);

    // Hands over what is left and waits for the thread to end. Returns true if
    // every byte reached the consumer, and false if the worker was cancelled.
    [[nodiscard]] bool finish();

    void cancel();

private:
    void push_chunk();
    void join();

    Consume consume_;
    ChunkWorkerOptions options_;
    BoundedQueue<std::vector<std::byte>> queue_;
    std::vector<std::byte> chunk_;
    // Last, so the thread starts after everything it uses exists.
    std::thread thread_;
};

// Hashes the bytes given to update() on its own thread.
class HashWorker {
public:
    explicit HashWorker(ChunkWorkerOptions options = {});

    void update(std::span<const std::byte> bytes) { worker_.update(bytes); }

    // The digest of every byte given to update(), or nullopt if cancelled.
    [[nodiscard]] std::optional<Sha256Digest> finish();

    void cancel() { worker_.cancel(); }

private:
    Sha256 hash_;
    // After hash_, so the thread is joined before hash_ goes away.
    ChunkWorker worker_;
};

}  // namespace tick_store::harness
