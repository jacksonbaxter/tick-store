#include "tick_store/harness/hash_worker.hpp"

#include "tick_store/harness/sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <utility>

namespace tick_store::harness {

ChunkWorker::ChunkWorker(Consume consume, ChunkWorkerOptions options)
    : consume_(std::move(consume)), options_(options), queue_(options.queue_slots) {
    chunk_.reserve(options_.chunk_bytes);
    thread_ = std::thread{[this] {
        while (const auto chunk = queue_.pop()) {
            consume_(*chunk);
        }
    }};
}

ChunkWorker::~ChunkWorker() {
    cancel();
    join();
}

void ChunkWorker::update(std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const std::size_t take = std::min(options_.chunk_bytes - chunk_.size(), bytes.size());
        const auto piece = bytes.first(take);
        chunk_.insert(chunk_.end(), piece.begin(), piece.end());
        bytes = bytes.subspan(take);
        if (chunk_.size() == options_.chunk_bytes) {
            push_chunk();
        }
    }
}

void ChunkWorker::push_chunk() {
    // A refused push means the worker was cancelled, so the bytes have no reader.
    std::ignore = queue_.push(std::exchange(chunk_, {}));
    chunk_.reserve(options_.chunk_bytes);
}

bool ChunkWorker::finish() {
    if (!chunk_.empty()) {
        push_chunk();
    }
    queue_.close();
    join();
    return !queue_.cancelled();
}

void ChunkWorker::cancel() { queue_.cancel(); }

void ChunkWorker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

HashWorker::HashWorker(ChunkWorkerOptions options)
    : worker_([this](std::span<const std::byte> chunk) { hash_.update(chunk); }, options) {}

std::optional<Sha256Digest> HashWorker::finish() {
    if (!worker_.finish()) {
        return std::nullopt;
    }
    return hash_.finish();
}

}  // namespace tick_store::harness
