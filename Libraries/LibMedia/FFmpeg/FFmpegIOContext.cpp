/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <LibMedia/FFmpeg/FFmpegIOContext.h>

extern "C" {
#include <libavformat/avformat.h>
}

namespace Media::FFmpeg {

FFmpegIOContext::FFmpegIOContext(NonnullRefPtr<IncrementallyPopulatedStream::Seekable> stream, AVIOContext* avio_context)
    : m_stream(stream)
    , m_avio_context(avio_context)
{
}

FFmpegIOContext::~FFmpegIOContext()
{
    // NOTE: free the buffer inside the AVIO context, since it might be changed since its initial allocation
    av_free(m_avio_context->buffer);
    avio_context_free(&m_avio_context);
}

ErrorOr<NonnullOwnPtr<FFmpegIOContext>> FFmpegIOContext::create(NonnullRefPtr<IncrementallyPopulatedStream::Seekable> stream)
{
    auto* avio_buffer = av_malloc(PAGE_SIZE);
    if (avio_buffer == nullptr)
        return Error::from_string_literal("Failed to allocate AVIO buffer");

    // This AVIOContext explains to avformat how to interact with our stream
    auto* avio_context = avio_alloc_context(
        static_cast<unsigned char*>(avio_buffer),
        PAGE_SIZE,
        0,
        stream.ptr(),
        [](void* opaque, u8* buffer, int size) -> int {
            auto& stream = *static_cast<IncrementallyPopulatedStream::Seekable*>(opaque);

            // AK::Bytes buffer_bytes { buffer, AK::min<size_t>(size, PAGE_SIZE) };
            // auto read_bytes_or_error = stream.read_bytes(buffer_bytes);
            auto buffer_bytes_or_error = stream.read_bytes(AK::min<size_t>(size, PAGE_SIZE));
            if (buffer_bytes_or_error.is_error()) {
                if (buffer_bytes_or_error.error().category() == DecoderErrorCategory::EndOfStream)
                    return AVERROR_EOF;
                if (buffer_bytes_or_error.error().category() == DecoderErrorCategory::NeedsMoreInput)
                    return AVERROR(EAGAIN);
                return AVERROR_UNKNOWN;
            }
            VERIFY(buffer_bytes_or_error.value().size() > 0);
            memcpy(buffer, buffer_bytes_or_error.value().data(), buffer_bytes_or_error.value().size());
            if (buffer_bytes_or_error.value().size() == 0)
                return AVERROR_EOF;
            return static_cast<int>(buffer_bytes_or_error.value().size());
        },
        nullptr,
        [](void* opaque, int64_t offset, int whence) -> int64_t {
            whence &= ~AVSEEK_FORCE;

            auto& stream = *static_cast<IncrementallyPopulatedStream::Seekable*>(opaque);
            if (whence == AVSEEK_SIZE)
                return stream.size();

            auto seek_mode_from_whence = [](int origin) -> IncrementallyPopulatedStream::Seekable::SeekMode {
                if (origin == SEEK_CUR)
                    return IncrementallyPopulatedStream::Seekable::SeekMode::FromCurrentPosition;
                if (origin == SEEK_END)
                    return IncrementallyPopulatedStream::Seekable::SeekMode::FromEndPosition;
                return IncrementallyPopulatedStream::Seekable::SeekMode::SetPosition;
            };

            stream.seek(offset, seek_mode_from_whence(whence));
            return 0;
        });
    if (avio_context == nullptr) {
        av_free(avio_buffer);
        return Error::from_string_literal("Failed to allocate AVIO context");
    }

    return make<FFmpegIOContext>(stream, avio_context);
}

}
