// CRC32C: the standard check values for every kernel this host runs, the hardware
// kernel against the software one on seeded random bytes at every start offset and
// many lengths, and the kernel the dispatcher chose.

#include "codec_fixtures.hpp"
#include "test_bytes.hpp"
#include "tick_store/store/crc32c.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tick_store::store {
namespace {

using itch::test::bytes;
using itch::test::Bytes;
using itch::test::exact_copy;

constexpr std::array kAllKernels{Crc32cKernel::software, Crc32cKernel::armv8,
                                 Crc32cKernel::sse42};

// The kernel the build for this processor must have, and that both hosts run.
constexpr Crc32cKernel kHardwareKernel =
#ifdef __aarch64__
    Crc32cKernel::armv8;
#elifdef __x86_64__
    Crc32cKernel::sse42;
#else
    Crc32cKernel::software;
#endif

std::vector<Crc32cKernel> available_kernels() {
    std::vector<Crc32cKernel> kernels;
    for (const Crc32cKernel kernel : kAllKernels) {
        if (crc32c_kernel_available(kernel)) {
            kernels.push_back(kernel);
        }
    }
    return kernels;
}

Bytes filled(std::size_t size, auto value_at) {
    Bytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(value_at(i));
    }
    return out;
}

Bytes ascii(std::string_view text) {
    Bytes out;
    itch::test::append_ascii(out, text);
    return out;
}

struct Vector {
    std::string_view name;
    Bytes input;
    std::uint32_t crc;
};

// RFC 3720, appendix B.4, plus the common "123456789" check value.
std::vector<Vector> standard_vectors() {
    return {
        {"32 bytes of zero", Bytes(32), 0x8A9136AAU},
        {"32 bytes of 0xFF", filled(32, [](std::size_t) { return 0xFFU; }), 0x62A8AB43U},
        {"32 ascending bytes", filled(32, [](std::size_t i) { return i; }), 0x46DD794EU},
        {"32 descending bytes", filled(32, [](std::size_t i) { return 31 - i; }), 0x113FDB5CU},
        {"iSCSI SCSI Read (10) command PDU",
         bytes({0x01, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
                0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
         0xD9963A56U},
        {"123456789", ascii("123456789"), 0xE3069283U},
        {"empty", Bytes{}, 0x00000000U},
    };
}

TEST(Crc32c, StandardVectorsOnEveryAvailableKernel) {
    for (const Crc32cKernel kernel : available_kernels()) {
        for (const Vector& vector : standard_vectors()) {
            SCOPED_TRACE(std::string{to_string(kernel)} + ": " + std::string{vector.name});
            const Bytes input = exact_copy(vector.input);
            EXPECT_EQ(crc32c_with(kernel, input), std::optional{vector.crc});
        }
    }
}

TEST(Crc32c, DispatchedFunctionGivesTheStandardValues) {
    for (const Vector& vector : standard_vectors()) {
        SCOPED_TRACE(vector.name);
        EXPECT_EQ(crc32c(exact_copy(vector.input)), vector.crc);
    }
}

// A hardware kernel missing on either reference host fails here rather than leaving
// the software kernel to pass in its place (S6).
TEST(Crc32c, HardwareKernelForThisProcessorIsAvailableAndChosen) {
    EXPECT_TRUE(crc32c_kernel_available(Crc32cKernel::software));
    EXPECT_TRUE(crc32c_kernel_available(kHardwareKernel)) << to_string(kHardwareKernel);
    EXPECT_EQ(crc32c_active_kernel(), kHardwareKernel)
        << "active " << to_string(crc32c_active_kernel()) << ", expected "
        << to_string(kHardwareKernel)
        << ". TICK_STORE_CRC32C=software in the environment forces the software kernel";
}

TEST(Crc32c, KernelsOfOtherProcessorsAreUnavailable) {
    for (const Crc32cKernel kernel : kAllKernels) {
        if (kernel != Crc32cKernel::software && kernel != kHardwareKernel) {
            EXPECT_FALSE(crc32c_kernel_available(kernel)) << to_string(kernel);
            EXPECT_EQ(crc32c_with(kernel, bytes({1, 2, 3})), std::nullopt) << to_string(kernel);
        }
    }
}

TEST(Crc32c, ContinuingEqualsOneCall) {
    const Bytes whole = ascii("123456789");
    for (const Crc32cKernel kernel : available_kernels()) {
        for (std::size_t cut = 0; cut <= whole.size(); ++cut) {
            SCOPED_TRACE(std::string{to_string(kernel)} + " cut at " + std::to_string(cut));
            const std::span<const std::byte> all{whole};
            const auto first = crc32c_with(kernel, all.first(cut));
            ASSERT_TRUE(first.has_value());
            EXPECT_EQ(crc32c_with(kernel, all.subspan(cut), *first), std::optional{0xE3069283U});
        }
    }
}

// Every start offset 0 to 15 and every length 0 to 300, then seeded random lengths up
// to 64 KiB, each in a heap block of exactly its size, so ASan sees a read past the
// end. The values are compared directly with the software kernel's (S5).
TEST(Crc32c, HardwareMatchesSoftwareOnRandomLengthsAndOffsets) {
    ASSERT_TRUE(crc32c_kernel_available(kHardwareKernel));
    codec::test::SplitMix64 random{20261103};

    const auto compare = [&](std::size_t offset, std::size_t length) {
        Bytes buffer(offset + length);
        for (std::byte& value : buffer) {
            value = static_cast<std::byte>(random() & 0xFFU);
        }
        const Bytes exact = exact_copy(buffer);
        const auto input = std::span<const std::byte>{exact}.subspan(offset);
        const auto start = static_cast<std::uint32_t>(random());
        const auto software = crc32c_with(Crc32cKernel::software, input, start);
        const auto hardware = crc32c_with(kHardwareKernel, input, start);
        ASSERT_TRUE(software.has_value());
        ASSERT_EQ(hardware, software) << "offset " << offset << ", length " << length;
    };

    for (std::size_t offset = 0; offset < 16; ++offset) {
        for (std::size_t length = 0; length <= 300; ++length) {
            compare(offset, length);
            if (::testing::Test::HasFatalFailure()) {
                return;
            }
        }
    }
    for (int round = 0; round < 200; ++round) {
        compare(static_cast<std::size_t>(round % 16), 301 + (random() % 65236));
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

TEST(Crc32c, OneFlippedBitChangesTheValue) {
    Bytes input = filled(64, [](std::size_t i) { return i * 7; });
    const std::uint32_t clean = crc32c(input);
    for (std::size_t bit = 0; bit < input.size() * 8; ++bit) {
        input[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        EXPECT_NE(crc32c(input), clean) << "bit " << bit;
        input[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    }
}

}  // namespace
}  // namespace tick_store::store
