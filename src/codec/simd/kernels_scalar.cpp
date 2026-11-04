// The scalar kernel set: always built, and the reference every other set is tested
// against. Each kernel goes through the shared driver with no whole step, so the
// driver's tail, the same scalar code every other set finishes with, does all the work.

#include "driver.hpp"

#include "tick_store/codec/lane_pack.hpp"
#include "tick_store/codec/simd.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tick_store::codec::simd::detail {
namespace {

constexpr KernelLevel kLevel = KernelLevel::scalar;
constexpr auto kNeverFits = [](std::size_t) noexcept { return false; };
constexpr auto kNoStep = [](std::size_t) noexcept {};

void unpack_vector_scalar(std::span<const std::byte> packed, unsigned width,
                          std::span<std::uint64_t, kVectorValues> values) noexcept {
    drive<kVectorValues>(kLevel, kVectorValues, kNeverFits, kNoStep, [&](std::size_t) {
        if (width > 64U || packed.size() != vector_bytes(width)) {
            // As codec::unpack_vector leaves a wrong size: every value 0.
            std::ranges::fill(values, std::uint64_t{0});
            return;
        }
        std::array<std::uint64_t, kVectorValues> words;  // NOLINT(cppcoreguidelines-pro-type-member-init): only the words used are written, then read
        const std::span<std::uint64_t> used = std::span{words}.first(vector_words(width));
        load_words_le(packed, used);
        codec::unpack_vector(used, width, values);
    });
}

void unpack_linear_scalar(std::span<const std::byte> bytes, unsigned width,
                          std::span<std::uint64_t> values) noexcept {
    drive<8>(kLevel, values.size(), kNeverFits, kNoStep,
             [&](std::size_t first) { unpack_linear_from(bytes, width, values, first); });
}

void add_base_scalar(std::span<std::uint64_t> values, std::uint64_t base, bool zigzag) noexcept {
    drive<1>(kLevel, values.size(), kNeverFits, kNoStep,
             [&](std::size_t first) { add_base_from(values, first, base, zigzag); });
}

std::uint64_t prefix_sum_scalar(std::span<std::uint64_t> values, std::uint64_t carry) noexcept {
    drive<1>(kLevel, values.size(), kNeverFits, kNoStep,
             [&](std::size_t first) { carry = prefix_sum_from(values, first, carry); });
    return carry;
}

std::uint64_t prefix_sum_scalar_tree(std::span<std::uint64_t> values,
                                    std::uint64_t carry) noexcept {
    drive<1>(kLevel, values.size(), kNeverFits, kNoStep,
             [&](std::size_t first) { carry = prefix_sum_tree_from(values, first, carry); });
    return carry;
}

std::uint64_t delta_pass_scalar(std::span<std::uint64_t> values, std::uint64_t base, bool zigzag,
                                std::uint64_t carry) noexcept {
    drive<1>(kLevel, values.size(), kNeverFits, kNoStep, [&](std::size_t first) {
        carry = delta_pass_from(values, first, base, zigzag, carry);
    });
    return carry;
}

std::uint64_t unpack_delta_scalar(std::span<const std::byte> packed, unsigned width,
                                  std::uint64_t base, bool zigzag, std::uint64_t carry,
                                  std::span<std::uint64_t, kVectorValues> values) noexcept {
    unpack_vector_scalar(packed, width, values);
    return delta_pass_scalar(values, base, zigzag, carry);
}

bool svb_decode_scalar(std::span<const std::byte> control, std::span<const std::byte> data,
                       std::span<std::uint64_t> values) noexcept {
    bool canonical = true;
    drive<1>(kLevel, values.size(), kNeverFits, kNoStep, [&](std::size_t first) {
        canonical = svb_decode_from(control, data, values, first, 0);
    });
    return canonical;
}

constexpr Kernels kScalar{
    .level = kLevel,
    // the scalar fused kernel is the two passes.
    .fuse_delta = false,
    .unpack_vector = &unpack_vector_scalar,
    .unpack_linear = &unpack_linear_scalar,
    .add_base = &add_base_scalar,
    .prefix_sum = &prefix_sum_scalar,
    .delta_pass = &delta_pass_scalar,
    .unpack_delta = &unpack_delta_scalar,
    // The scalar set has no narrow lanes: the 64-bit sums are its only form.
    .unpack_delta_narrow = &unpack_delta_scalar,
    .svb_decode = &svb_decode_scalar,
};

}  // namespace

const Kernels& scalar_kernels() noexcept { return kScalar; }

PrefixSumForms scalar_prefix_sum_forms() noexcept {
    return {.vector = &prefix_sum_scalar, .tree = &prefix_sum_scalar_tree};
}

}  // namespace tick_store::codec::simd::detail
