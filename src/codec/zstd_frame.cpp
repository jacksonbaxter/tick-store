#include "tick_store/codec/zstd_frame.hpp"

#include "tick_store/codec/byte_cursor.hpp"
#include "tick_store/codec/codec_error.hpp"
#include "tick_store/codec/registry.hpp"

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace tick_store::codec {
namespace {

constexpr std::uint64_t kInnerCodecOffset = 0;
constexpr std::uint64_t kReservedOffset = 2;
constexpr std::uint64_t kInnerSizeOffset = 4;
constexpr std::uint64_t kMaxInnerBytes = 0xFFFF'FFFFULL;

void put_le(std::vector<std::byte>& out, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
    }
}

[[nodiscard]] bool valid_inner(std::uint64_t id) noexcept {
    return id <= 0xFFFFU && id != static_cast<std::uint64_t>(CodecId::none) &&
           id != static_cast<std::uint64_t>(CodecId::zstd) &&
           codec_readable(static_cast<std::uint16_t>(id));
}

struct FreeDecompress {
    void operator()(ZSTD_DCtx* context) const noexcept { ZSTD_freeDCtx(context); }
};

struct FreeCompress {
    void operator()(ZSTD_CCtx* context) const noexcept { ZSTD_freeCCtx(context); }
};

}  // namespace

std::expected<void, CodecError> encode_zstd_payload(std::uint16_t inner_codec,
                                                    std::span<const std::byte> inner,
                                                    std::vector<std::byte>& out) {
    if (!valid_inner(inner_codec)) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_parameter, "zstd inner codec", 0, 0, inner_codec));
    }
    if (inner.empty() || inner.size() > kMaxInnerBytes) {
        return std::unexpected(codec_error(CodecErrorKind::bad_parameter, "zstd inner size", 0,
                                           kMaxInnerBytes, inner.size()));
    }
    // One context per thread, kept for the thread's life, so a writer calling this once a
    // column does not allocate zstd's tables every time.
    thread_local const std::unique_ptr<ZSTD_CCtx, FreeCompress> context{ZSTD_createCCtx()};
    const std::size_t start = out.size();
    put_le(out, inner_codec, 2);
    put_le(out, 0, 2);
    put_le(out, inner.size(), 4);
    const std::size_t bound = ZSTD_compressBound(inner.size());
    out.resize(start + kZstdHeaderSize + bound);
    const std::size_t written =
        ZSTD_compressCCtx(context.get(), out.data() + start + kZstdHeaderSize, bound, inner.data(),
                          inner.size(), kZstdLevel);
    if (ZSTD_isError(written) != 0U) {
        out.resize(start);
        return std::unexpected(
            codec_error(CodecErrorKind::bad_parameter, "zstd frame", 0, 0, written));
    }
    out.resize(start + kZstdHeaderSize + written);
    return {};
}

std::expected<ZstdPayload, CodecError> decode_zstd_payload(std::span<const std::byte> payload,
                                                           std::uint64_t max_inner_bytes) {
    ByteCursor cursor{payload};
    const auto inner = cursor.read_le(2, "zstd header");
    const auto reserved = cursor.read_le(2, "zstd header");
    const auto size = cursor.read_le(4, "zstd header");
    if (!size.has_value()) {
        return std::unexpected(!inner.has_value()      ? inner.error()
                               : !reserved.has_value() ? reserved.error()
                                                       : size.error());
    }
    if (!valid_inner(*inner)) {
        return std::unexpected(codec_error(CodecErrorKind::bad_field, "zstd inner codec",
                                           kInnerCodecOffset * 8U, 0, *inner));
    }
    if (*reserved != 0) {
        return std::unexpected(codec_error(CodecErrorKind::bad_field, "zstd reserved",
                                           kReservedOffset * 8U, 0, *reserved));
    }
    const std::uint64_t limit = max_inner_bytes < kMaxInnerBytes ? max_inner_bytes : kMaxInnerBytes;
    if (*size == 0 || *size > limit) {
        return std::unexpected(codec_error(CodecErrorKind::bad_count, "zstd inner size",
                                           kInnerSizeOffset * 8U, limit, *size));
    }
    const std::span<const std::byte> frame = payload.subspan(kZstdHeaderSize);
    constexpr std::uint64_t frame_bits = kZstdHeaderSize * 8U;
    const std::size_t frame_size = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    if (ZSTD_isError(frame_size) != 0U) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_field, "zstd frame", frame_bits, 0, frame_size));
    }
    if (frame_size != frame.size()) {
        return std::unexpected(codec_error(CodecErrorKind::bad_field, "zstd frame", frame_bits,
                                           frame.size(), frame_size));
    }
    const unsigned long long content = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (content != *size) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_field, "zstd frame", frame_bits, *size, content));
    }
    thread_local const std::unique_ptr<ZSTD_DCtx, FreeDecompress> context{ZSTD_createDCtx()};
    ZstdPayload out;
    out.inner_codec = static_cast<std::uint16_t>(*inner);
    out.bytes.resize(static_cast<std::size_t>(*size));
    const std::size_t got = ZSTD_decompressDCtx(context.get(), out.bytes.data(), out.bytes.size(),
                                                frame.data(), frame.size());
    if (ZSTD_isError(got) != 0U) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_field, "zstd frame", frame_bits, 0, got));
    }
    if (got != out.bytes.size()) {
        return std::unexpected(
            codec_error(CodecErrorKind::bad_field, "zstd frame", frame_bits, *size, got));
    }
    return out;
}

}  // namespace tick_store::codec
