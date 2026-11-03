#pragma once

// Shared pieces of the codec tests: a seeded generator, so a run is the same on every
// host and adding a case never changes another case's data.

#include <cstdint>

namespace tick_store::codec::test {

// SplitMix64, from Steele, Lea and Flood, "Fast splittable pseudorandom number
// generators", 2014. Fixed seeds are the point here: these are test inputs.
class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t operator()() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

private:
    std::uint64_t state_;
};

}  // namespace tick_store::codec::test
