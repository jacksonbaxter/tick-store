// The decode kernels against the scalar set, for every set this build and processor run.
//
// How the SIMD items of the code-quality bar are met here:
//
//   S1  The scalar set is always built and always tested. ctest runs the codec tests a
//       second time with TICK_STORE_SIMD=scalar (tests/unit/CMakeLists.txt).
//   S3  Every input is an exact-size heap buffer that ends where the kernel's span ends,
//       so under ASan a read one byte past it fails.
//   S4  Every width 0 to 64, inputs at every byte offset 0 to 15 so no load is aligned by
//       luck, and lengths across every tail from 0 to past two whole steps.
//   S5  Each kernel's output is compared with the scalar set's by memcmp, not value by
//       value through a tolerance.
//   S6  The set this processor should run is required, not skipped when missing, and the
//       set the environment asks for is required to be the one that ran.
//   S7  Packed bytes are read at odd offsets, so a typed load of a misaligned word in a
//       scalar tail fails under UBSan's alignment check; the vector loads take byte
//       pointers through memcpy or unaligned intrinsics.

#include "tick_store/codec/lane_pack.hpp"
#include "tick_store/codec/pfor.hpp"
#include "tick_store/codec/simd.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::codec::simd {
namespace {

using Values = std::vector<std::uint64_t>;

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kOffsets = 16;

#if defined(__aarch64__) || defined(__arm64__)
constexpr KernelLevel kProcessorLevel = KernelLevel::neon;
#elifdef __x86_64__
constexpr KernelLevel kProcessorLevel = KernelLevel::avx2;
#else
constexpr KernelLevel kProcessorLevel = KernelLevel::scalar;
#endif

// Every test's generator starts from a fixed seed of its own, so a failure reproduces.
std::mt19937_64 seeded(std::uint64_t seed) { return std::mt19937_64{seed}; }

// The bytes Stream VByte stores a 32-bit value in.
unsigned svb_length(std::uint32_t value) {
    unsigned length = 1;
    while (length < 4U && (value >> (8U * length)) != 0U) {
        ++length;
    }
    return length;
}

// A buffer of exactly `size` bytes on the heap, so ASan sees its end.
class ExactBytes {
public:
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): an exact size, for ASan
    explicit ExactBytes(std::size_t size) : bytes_(std::make_unique<std::byte[]>(size)), size_(size) {}
    [[nodiscard]] std::span<std::byte> span() noexcept { return {bytes_.get(), size_}; }

private:
    std::unique_ptr<std::byte[]> bytes_;  // NOLINT(modernize-avoid-c-arrays): an exact size, for ASan
    std::size_t size_;
};

// `content` copied to the end of an exact buffer `offset` bytes longer, returned with
// the buffer that owns it.
struct Placed {
    ExactBytes owner;
    std::span<const std::byte> bytes;
};

Placed place(std::span<const std::byte> content, std::size_t offset) {
    Placed placed{.owner = ExactBytes{offset + content.size()}, .bytes = {}};
    const std::span<std::byte> all = placed.owner.span();
    if (!content.empty()) {
        std::memcpy(all.data() + offset, content.data(), content.size());
    }
    placed.bytes = std::span<const std::byte>{all}.subspan(offset);
    return placed;
}

// The sets to test against the scalar one: every one available, the scalar set included,
// so a broken driver tail shows even there.
std::vector<const Kernels*> sets() {
    std::vector<const Kernels*> out;
    for (const KernelLevel level : kAllKernelLevels) {
        if (const Kernels* set = kernels_for(level); set != nullptr) {
            out.push_back(set);
        }
    }
    return out;
}

const Kernels& scalar() { return *kernels_for(KernelLevel::scalar); }

bool same(std::span<const std::uint64_t> a, std::span<const std::uint64_t> b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

std::vector<std::byte> packed_vector(const std::array<std::uint64_t, kVectorValues>& values,
                                     unsigned width) {
    std::vector<std::uint64_t> words(vector_words(width));
    pack_vector(values, width, words);
    std::vector<std::byte> bytes;
    append_words_le(words, bytes);
    return bytes;
}

std::array<std::uint64_t, kVectorValues> random_offsets(std::mt19937_64& rng, std::uint64_t mask) {
    std::array<std::uint64_t, kVectorValues> values{};
    for (std::uint64_t& value : values) {
        value = rng() & mask;
    }
    // The widest offset at least once, and a run of zeros, at the vector's two ends.
    values[kVectorValues - 1] = mask;
    values[0] = 0;
    return values;
}

std::uint64_t mask_of(unsigned width) {
    return width == 64 ? kMax : (std::uint64_t{1} << width) - 1U;
}

// --- Dispatch --------------------------------------------------------------------------

TEST(SimdDispatch, TheSetThisProcessorShouldRunIsAvailable) {
    EXPECT_TRUE(kernel_level_available(KernelLevel::scalar));
    EXPECT_TRUE(kernel_level_available(kProcessorLevel))
        << to_string(kProcessorLevel) << " is missing on a processor that should have it";
    EXPECT_EQ(best_kernel_level(), kProcessorLevel);
}

TEST(SimdDispatch, SetsOfOtherProcessorsAreUnavailable) {
    for (const KernelLevel level : kAllKernelLevels) {
        if (level != KernelLevel::scalar && level != kProcessorLevel) {
            EXPECT_FALSE(kernel_level_available(level)) << to_string(level);
            EXPECT_EQ(kernels_for(level), nullptr) << to_string(level);
            EXPECT_EQ(prefix_sum_forms(level).vector, nullptr) << to_string(level);
        }
    }
}

TEST(SimdDispatch, LevelNamesRoundTrip) {
    for (const KernelLevel level : kAllKernelLevels) {
        KernelLevel parsed = KernelLevel::scalar;
        EXPECT_TRUE(parse_kernel_level(to_string(level), parsed));
        EXPECT_EQ(parsed, level);
        EXPECT_EQ(kernels_for(level) == nullptr ? level : kernels_for(level)->level, level);
    }
    KernelLevel parsed = KernelLevel::avx2;
    EXPECT_FALSE(parse_kernel_level("sse2", parsed));
    EXPECT_FALSE(parse_kernel_level("", parsed));
    EXPECT_FALSE(parse_kernel_level("NEON", parsed));
}

// The level the environment asked for, or this processor's, is the one in use, and a
// decode through the public codec runs that set's kernels and no other's (S1, S6).
TEST(SimdDispatch, TheRequestedSetIsTheOneThatRuns) {
    KernelLevel expected = kProcessorLevel;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    if (const char* cap = std::getenv("TICK_STORE_SIMD"); cap != nullptr) {
        ASSERT_TRUE(parse_kernel_level(cap, expected)) << cap;
    }
    ASSERT_EQ(active_kernels().level, expected)
        << "active " << to_string(active_kernels().level) << ", expected " << to_string(expected);

    std::mt19937_64 rng = seeded(0x51D0'0001);
    Values values(5000);
    std::uint64_t sum = 0;
    for (std::uint64_t& value : values) {
        sum += rng() % 1000;
        value = sum;
    }
    std::vector<std::byte> column;
    ASSERT_TRUE(encode_pfor(values, 4096,
                            {.transform = IntTransform::delta, .adaptive_scale = false, .patch = true},
                            column)
                    .has_value());
    reset_kernel_runs();
    Values decoded;
    ASSERT_TRUE(decode_pfor(column, values.size(), decoded).has_value());
    EXPECT_EQ(decoded, values);
    const auto runs = kernel_runs();
    for (const KernelLevel level : kAllKernelLevels) {
        const std::uint64_t ran = runs[static_cast<std::size_t>(level)];
        if (level == expected) {
            EXPECT_GT(ran, 0U) << to_string(level) << " was asked for and never ran";
        } else {
            EXPECT_EQ(ran, 0U) << to_string(level) << " ran in place of " << to_string(expected);
        }
    }
}

// The fuzz oracles' switch: inside the scope the codecs run only scalar kernels, nested
// scopes keep it, and leaving the outer one gives back the chosen set.
TEST(SimdDispatch, AScalarScopeRunsOnlyTheScalarSetAndRestoresTheChoice) {
    const KernelLevel chosen = active_kernels().level;
    Values values(3000);
    std::ranges::iota(values, std::uint64_t{7});
    std::vector<std::byte> column;
    ASSERT_TRUE(encode_pfor(values, 4096,
                            {.transform = IntTransform::delta, .adaptive_scale = false, .patch = true},
                            column)
                    .has_value());
    const auto decode_counting = [&] {
        reset_kernel_runs();
        Values decoded;
        EXPECT_TRUE(decode_pfor(column, values.size(), decoded).has_value());
        EXPECT_EQ(decoded, values);
        return kernel_runs();
    };
    {
        const ScalarKernelsOnThisThread outer;
        {
            const ScalarKernelsOnThisThread inner;
            EXPECT_EQ(active_kernels().level, KernelLevel::scalar);
        }
        EXPECT_EQ(active_kernels().level, KernelLevel::scalar);
        const auto runs = decode_counting();
        for (const KernelLevel level : kAllKernelLevels) {
            const std::uint64_t ran = runs[static_cast<std::size_t>(level)];
            if (level == KernelLevel::scalar) {
                EXPECT_GT(ran, 0U);
            } else {
                EXPECT_EQ(ran, 0U) << to_string(level) << " ran inside a scalar scope";
            }
        }
    }
    EXPECT_EQ(active_kernels().level, chosen);
    EXPECT_GT(decode_counting()[static_cast<std::size_t>(chosen)], 0U);
}

// --- Unpack ------------------------------------------------------------------------------

TEST(SimdKernels, UnpackEveryWidthAndOffsetMatchesScalar) {
    std::mt19937_64 rng = seeded(0x51D0'0002);
    for (unsigned width = 0; width <= 64; ++width) {
        const auto values = random_offsets(rng, mask_of(width));
        const std::vector<std::byte> bytes = packed_vector(values, width);
        ASSERT_EQ(bytes.size(), 128U * width);
        std::array<std::uint64_t, kVectorValues> reference{};
        scalar().unpack_vector(bytes, width, reference);
        ASSERT_TRUE(same(reference, values)) << "the scalar unpack is wrong at width " << width;
        for (const Kernels* set : sets()) {
            for (std::size_t offset = 0; offset < kOffsets; ++offset) {
                const Placed placed = place(bytes, offset);
                auto out = std::make_unique<std::array<std::uint64_t, kVectorValues>>();
                out->fill(0xA5A5A5A5A5A5A5A5ULL);
                set->unpack_vector(placed.bytes, width, *out);
                ASSERT_TRUE(same(*out, reference))
                    << to_string(set->level) << " width " << width << " offset " << offset;
            }
        }
    }
}

// A size that is not the width's leaves every value 0, in every set alike.
TEST(SimdKernels, UnpackRefusesAWrongSizeTheSameWay) {
    const std::vector<std::byte> bytes(std::size_t{128} * 3U, std::byte{0xFF});
    for (const Kernels* set : sets()) {
        auto out = std::make_unique<std::array<std::uint64_t, kVectorValues>>();
        out->fill(7);
        set->unpack_vector(std::span{bytes}.first(std::size_t{128} * 2U), 3, *out);
        EXPECT_TRUE(std::ranges::all_of(*out, [](std::uint64_t v) { return v == 0; }))
            << to_string(set->level);
        out->fill(7);
        set->unpack_vector(bytes, 65, *out);
        EXPECT_TRUE(std::ranges::all_of(*out, [](std::uint64_t v) { return v == 0; }))
            << to_string(set->level);
    }
}

// The linear layout at every width, every count across the eight-value step and its
// tail, at every offset, against the scalar set and against the values packed.
TEST(SimdKernels, LinearUnpackEveryWidthTailAndOffsetMatchesScalar) {
    std::mt19937_64 rng = seeded(0x51D0'000B);
    std::vector<std::size_t> counts;
    for (std::size_t n = 0; n <= 33; ++n) {
        counts.push_back(n);
    }
    counts.push_back(1000);
    for (unsigned width = 1; width <= 64; ++width) {
        for (const std::size_t n : counts) {
            Values values(n);
            for (std::uint64_t& value : values) {
                value = rng() & mask_of(width);
            }
            std::vector<std::byte> bytes;
            pack_linear(values, width, bytes);
            ASSERT_EQ(bytes.size(), linear_bytes(n, width));
            for (const Kernels* set : sets()) {
                for (std::size_t offset = 0; offset < kOffsets; offset += (n % 2 == 0 ? 1 : 7)) {
                    const Placed placed = place(bytes, offset);
                    const auto owner = std::make_unique<std::uint64_t[]>(n);  // NOLINT(modernize-avoid-c-arrays): exact, for ASan
                    const std::span<std::uint64_t> out{owner.get(), n};
                    set->unpack_linear(placed.bytes, width, out);
                    ASSERT_TRUE(same(out, values))
                        << to_string(set->level) << " width " << width << " count " << n << " offset "
                        << offset;
                }
            }
        }
    }
}

// --- Frame of reference and the inverse delta ---------------------------------------------

constexpr std::array<std::uint64_t, 6> kBases{0, 1, 0x7FFF'FFFF'FFFF'FFFFULL, kMax, 1U << 21U,
                                              0x0123'4567'89AB'CDEFULL};

// Lengths over every tail of the widest step, twice over, then some long ones.
std::vector<std::size_t> lengths() {
    std::vector<std::size_t> out;
    for (std::size_t n = 0; n <= 40; ++n) {
        out.push_back(n);
    }
    for (const std::size_t n : {std::size_t{1023}, std::size_t{1024}, std::size_t{1025},
                                std::size_t{4099}}) {
        out.push_back(n);
    }
    return out;
}

Values random_values(std::mt19937_64& rng, std::size_t n, unsigned width) {
    Values values(n);
    for (std::uint64_t& value : values) {
        value = rng() & mask_of(width);
    }
    return values;
}

// Runs `kernel` on a copy of `input` placed at each start within an exact buffer, and
// compares the values and the returned carry with the scalar kernel's.
template <class Run>
void each_placement(const Values& input, std::string_view what, Run run) {
    Values reference = input;
    const std::uint64_t reference_result = run(scalar(), std::span<std::uint64_t>{reference});
    for (const Kernels* set : sets()) {
        for (std::size_t start = 0; start < 8; ++start) {
            const auto owner = std::make_unique<std::uint64_t[]>(start + input.size());  // NOLINT(modernize-avoid-c-arrays): exact, for ASan
            const std::span<std::uint64_t> values{owner.get() + start, input.size()};
            std::ranges::copy(input, values.begin());
            const std::uint64_t result = run(*set, values);
            ASSERT_TRUE(same(values, reference))
                << what << ": " << to_string(set->level) << ", " << input.size() << " values at "
                << start;
            ASSERT_EQ(result, reference_result)
                << what << ": " << to_string(set->level) << ", " << input.size() << " values";
        }
    }
}

TEST(SimdKernels, AddBaseMatchesScalarOnEveryTail) {
    std::mt19937_64 rng = seeded(0x51D0'0003);
    for (const std::size_t n : lengths()) {
        for (const std::uint64_t base : kBases) {
            for (const bool zigzag : {false, true}) {
                const Values input = random_values(rng, n, n % 2 == 0 ? 64U : 20U);
                each_placement(input, "add_base", [&](const Kernels& set, std::span<std::uint64_t> v) {
                    set.add_base(v, base, zigzag);
                    return std::uint64_t{0};
                });
            }
        }
    }
}

TEST(SimdKernels, PrefixSumMatchesScalarOnEveryTail) {
    std::mt19937_64 rng = seeded(0x51D0'0004);
    for (const std::size_t n : lengths()) {
        for (const std::uint64_t carry : kBases) {
            const Values input = random_values(rng, n, n % 3 == 0 ? 64U : 30U);
            each_placement(input, "prefix_sum", [&](const Kernels& set, std::span<std::uint64_t> v) {
                return set.prefix_sum(v, carry);
            });
            // Both measured forms of every set, whichever the table took.
            for (const KernelLevel level : kAllKernelLevels) {
                const PrefixSumForms forms = prefix_sum_forms(level);
                if (forms.vector == nullptr) {
                    continue;
                }
                for (const auto form : {forms.vector, forms.tree}) {
                    each_placement(input, "prefix_sum form",
                                   [&](const Kernels&, std::span<std::uint64_t> v) { return form(v, carry); });
                }
            }
        }
    }
}

TEST(SimdKernels, DeltaPassMatchesScalarOnEveryTail) {
    std::mt19937_64 rng = seeded(0x51D0'0005);
    for (const std::size_t n : lengths()) {
        for (const std::uint64_t base : kBases) {
            for (const bool zigzag : {false, true}) {
                const Values input = random_values(rng, n, 64);
                const std::uint64_t carry = rng();
                each_placement(input, "delta_pass", [&](const Kernels& set, std::span<std::uint64_t> v) {
                    return set.delta_pass(v, base, zigzag, carry);
                });
            }
        }
    }
}

// --- The fused kernels ------------------------------------------------------------------

using FusedFn = std::uint64_t (*)(std::span<const std::byte>, unsigned, std::uint64_t, bool,
                                  std::uint64_t, std::span<std::uint64_t, kVectorValues>) noexcept;

void check_fused(FusedFn Kernels::*kernel, std::string_view name, std::uint64_t seed) {
    std::mt19937_64 rng{seed};
    for (unsigned width = 0; width <= 64; ++width) {
        const auto offsets = random_offsets(rng, mask_of(width));
        const std::vector<std::byte> bytes = packed_vector(offsets, width);
        // Bases that let the narrow form run at narrow widths, and ones that do not.
        const std::uint64_t mask = mask_of(width);
        const std::array<std::uint64_t, 5> bases{
            0, kNarrowDeltaLimit > mask ? kNarrowDeltaLimit - 1U - mask : 0, 3, rng(), kMax};
        for (const std::uint64_t base : bases) {
            for (const bool zigzag : {false, true}) {
                const std::uint64_t carry = rng();
                // The reference: unpack, then the scalar delta pass.
                std::array<std::uint64_t, kVectorValues> reference = offsets;
                const std::uint64_t reference_last =
                    scalar().delta_pass(reference, base, zigzag, carry);
                for (const Kernels* set : sets()) {
                    for (std::size_t offset = 0; offset < kOffsets; offset += (width % 2 == 0 ? 1 : 5)) {
                        const Placed placed = place(bytes, offset);
                        auto out = std::make_unique<std::array<std::uint64_t, kVectorValues>>();
                        const std::uint64_t last =
                            (set->*kernel)(placed.bytes, width, base, zigzag, carry, *out);
                        ASSERT_TRUE(same(*out, reference))
                            << name << ": " << to_string(set->level) << " width " << width << " base "
                            << base << " zigzag " << zigzag << " offset " << offset;
                        ASSERT_EQ(last, reference_last) << name << ": " << to_string(set->level);
                    }
                }
            }
        }
    }
}

TEST(SimdKernels, FusedUnpackDeltaMatchesScalarAtEveryWidth) {
    check_fused(&Kernels::unpack_delta, "unpack_delta", 0x51D0'0006);
}

TEST(SimdKernels, NarrowFusedUnpackDeltaMatchesScalarAtEveryWidth) {
    check_fused(&Kernels::unpack_delta_narrow, "unpack_delta_narrow", 0x51D0'0007);
}

// --- Stream VByte -------------------------------------------------------------------------

struct SvbInput {
    std::vector<std::byte> control;
    std::vector<std::byte> data;
};

// Values of mixed lengths, each in its shortest form, unless `stretch` names one to give
// a byte more than it needs.
SvbInput svb_input(const Values& values, std::size_t stretch) {
    SvbInput in;
    in.control.assign((values.size() + 3U) / 4U, std::byte{0});
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto low = static_cast<std::uint32_t>(values[i]);
        unsigned length = svb_length(low);
        if (i == stretch && length < 4) {
            ++length;
        }
        in.control[i / 4U] |= static_cast<std::byte>((length - 1U) << (2U * (i % 4U)));
        for (unsigned b = 0; b < length; ++b) {
            in.data.push_back(static_cast<std::byte>((low >> (8U * b)) & 0xFFU));
        }
    }
    return in;
}

Values svb_values(std::mt19937_64& rng, std::size_t n) {
    Values values(n);
    for (std::uint64_t& value : values) {
        const unsigned bits = 1U + static_cast<unsigned>(rng() % 32U);
        value = rng() & ((std::uint64_t{1} << bits) - 1U);
    }
    return values;
}

TEST(SimdKernels, StreamVbyteMatchesScalarOnEveryTailAndOffset) {
    std::mt19937_64 rng = seeded(0x51D0'0008);
    for (const std::size_t n : lengths()) {
        const Values values = svb_values(rng, n);
        const SvbInput in = svb_input(values, n);
        for (const Kernels* set : sets()) {
            for (std::size_t offset = 0; offset < kOffsets; offset += 3) {
                const Placed control = place(in.control, offset);
                const Placed data = place(in.data, offset);
                const auto owner = std::make_unique<std::uint64_t[]>(n);  // NOLINT(modernize-avoid-c-arrays): exact, for ASan
                const std::span<std::uint64_t> out{owner.get(), n};
                ASSERT_TRUE(set->svb_decode(control.bytes, data.bytes, out))
                    << to_string(set->level) << " " << n;
                ASSERT_TRUE(same(out, values)) << to_string(set->level) << " " << n << " at " << offset;
            }
        }
    }
}

// A value one byte longer than it needs is refused wherever it falls: in a whole step
// or in the tail, by every set.
TEST(SimdKernels, StreamVbyteRefusesALongValueAnywhere) {
    std::mt19937_64 rng = seeded(0x51D0'0009);
    for (const std::size_t n : {std::size_t{1}, std::size_t{5}, std::size_t{17}, std::size_t{64}}) {
        Values values = svb_values(rng, n);
        for (std::size_t stretch = 0; stretch < n; ++stretch) {
            values[stretch] &= 0xFFFFU;  // room to stretch
            const SvbInput in = svb_input(values, stretch);
            for (const Kernels* set : sets()) {
                const Placed control = place(in.control, 1);
                const Placed data = place(in.data, 1);
                Values out(n);
                EXPECT_FALSE(set->svb_decode(control.bytes, data.bytes, out))
                    << to_string(set->level) << " " << n << " long at " << stretch;
            }
        }
    }
}

TEST(SimdKernels, StreamVbyteDataBytesFollowTheCodes) {
    std::mt19937_64 rng = seeded(0x51D0'000A);
    for (const std::size_t n : lengths()) {
        const SvbInput in = svb_input(svb_values(rng, n), n);
        EXPECT_EQ(svb_data_bytes(in.control, n), in.data.size()) << n;
    }
}

}  // namespace
}  // namespace tick_store::codec::simd
