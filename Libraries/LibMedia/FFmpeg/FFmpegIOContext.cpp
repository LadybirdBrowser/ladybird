/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <LibMedia/FFmpeg/FFmpegIOContext.h>
#include <LibMedia/MediaStream.h>

extern "C" {
#include <libavformat/avformat.h>
}

namespace Media::FFmpeg {

FFmpegIOContext::FFmpegIOContext(NonnullRefPtr<MediaStreamCursor> stream_cursor, AVIOContext* avio_context)
    : m_stream_cursor(move(stream_cursor))
    , m_avio_context(avio_context)
{
}

FFmpegIOContext::~FFmpegIOContext()
{
    // NOTE: free the buffer inside the AVIO context, since it might be changed since its initial allocation
    av_free(m_avio_context->buffer);
    avio_context_free(&m_avio_context);
}

ErrorOr<NonnullOwnPtr<FFmpegIOContext>> FFmpegIOContext::create(NonnullRefPtr<MediaStreamCursor> stream_cursor)
{
    auto* avio_buffer = av_malloc(PAGE_SIZE);
    if (avio_buffer == nullptr)
        return Error::from_string_literal("Failed to allocate AVIO buffer");

    // This AVIOContext explains to avformat how to interact with our stream
    auto* avio_context = avio_alloc_context(
        static_cast<unsigned char*>(avio_buffer),
        PAGE_SIZE,
        0,
        stream_cursor.ptr(),
        [](void* opaque, u8* buffer, int size) -> int {
            auto& stream_cursor = *static_cast<MediaStreamCursor*>(opaque);
            Bytes buffer_bytes { buffer, AK::min<size_t>(size, PAGE_SIZE) };
            auto buffer_bytes_or_error = stream_cursor.read_into(buffer_bytes);
            if (buffer_bytes_or_error.is_error()) {
                if (buffer_bytes_or_error.error().category() == DecoderErrorCategory::Aborted)
                    return AVERROR_EXIT;
                if (buffer_bytes_or_error.error().category() == DecoderErrorCategory::EndOfStream)
                    return AVERROR_EOF;
                return AVERROR_UNKNOWN;
            }
            if (buffer_bytes_or_error.value() == 0)
                return AVERROR_EOF;
            return static_cast<int>(buffer_bytes_or_error.value());
        },
        nullptr,
        [](void* opaque, int64_t offset, int whence) -> int64_t {
            whence &= ~AVSEEK_FORCE;

            auto& stream_cursor = *static_cast<MediaStreamCursor*>(opaque);
            if (whence == AVSEEK_SIZE)
                return stream_cursor.size();

            auto seek_mode_from_whence = [](int origin) -> AK::SeekMode {
                if (origin == SEEK_CUR)
                    return AK::SeekMode::FromCurrentPosition;
                if (origin == SEEK_END)
                    return AK::SeekMode::FromEndPosition;
                return AK::SeekMode::SetPosition;
            };

            auto maybe_seek_error = stream_cursor.seek(offset, seek_mode_from_whence(whence));
            if (maybe_seek_error.is_error()) {
                if (maybe_seek_error.error().category() == DecoderErrorCategory::Aborted)
                    return AVERROR_EXIT;
                if (maybe_seek_error.error().category() == DecoderErrorCategory::EndOfStream)
                    return AVERROR_EOF;
                return AVERROR_UNKNOWN;
            }
            return stream_cursor.position();
        });
    if (avio_context == nullptr) {
        av_free(avio_buffer);
        return Error::from_string_literal("Failed to allocate AVIO context");
    }

    return make<FFmpegIOContext>(move(stream_cursor), avio_context);
}

}
