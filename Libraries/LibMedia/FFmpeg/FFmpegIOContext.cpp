/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <LibMedia/FFmpeg/FFmpegIOContext.h>

namespace Media::FFmpeg {

FFmpegIOContext::FFmpegIOContext(AVIOContext* avio_context)
    : m_avio_context(avio_context)
{
}

FFmpegIOContext::~FFmpegIOContext()
{
    // NOTE: free the buffer inside the AVIO context, since it might be changed since its initial allocation
    av_free(m_avio_context->buffer);
    avio_context_free(&m_avio_context);
}

ErrorOr<NonnullOwnPtr<FFmpegIOContext>> FFmpegIOContext::create(AK::SeekableStream& stream)
{
    auto* avio_buffer = av_malloc(PAGE_SIZE);
    if (avio_buffer == nullptr)
        return Error::from_string_literal("Failed to allocate AVIO buffer");

    // This AVIOContext explains to avformat how to interact with our stream
    auto* avio_context = avio_alloc_context(
        static_cast<unsigned char*>(avio_buffer),
        PAGE_SIZE,
        0,
        &stream,
        [](void* opaque, u8* buffer, int size) -> int {
            auto& stream = *static_cast<SeekableStream*>(opaque);
            AK::Bytes buffer_bytes { buffer, AK::min<size_t>(size, PAGE_SIZE) };
            auto read_bytes_or_error = stream.read_some(buffer_bytes);
            if (read_bytes_or_error.is_error()) {
                if (read_bytes_or_error.error().code() == EOF)
                    return AVERROR_EOF;
                return AVERROR_UNKNOWN;
            }
            int number_of_bytes_read = read_bytes_or_error.value().size();
            if (number_of_bytes_read == 0)
                return AVERROR_EOF;
            return number_of_bytes_read;
        },
        nullptr,
        [](void* opaque, int64_t offset, int whence) -> int64_t {
            whence &= ~AVSEEK_FORCE;

            auto& stream = *static_cast<SeekableStream*>(opaque);
            if (whence == AVSEEK_SIZE)
                return static_cast<int64_t>(stream.size().value());

            auto seek_mode_from_whence = [](int origin) -> SeekMode {
                if (origin == SEEK_CUR)
                    return SeekMode::FromCurrentPosition;
                if (origin == SEEK_END)
                    return SeekMode::FromEndPosition;
                return SeekMode::SetPosition;
            };
            auto offset_or_error = stream.seek(offset, seek_mode_from_whence(whence));
            if (offset_or_error.is_error())
                return -EIO;
            return 0;
        });
    if (avio_context == nullptr) {
        av_free(avio_buffer);
        return Error::from_string_literal("Failed to allocate AVIO context");
    }

    return make<FFmpegIOContext>(avio_context);
}

}
