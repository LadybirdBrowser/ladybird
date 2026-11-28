/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegHelpers.h>

#include "FFmpegVideoEncoder.h"

namespace Media::FFmpeg {

// Every video codec is carried over RTP with this clock rate, so the encoder counts time in it too.
static constexpr AVRational TIME_BASE { 1, 90'000 };

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoEncoder>> FFmpegVideoEncoder::try_create(CodecID codec_id, u32 width, u32 height, int bitrate)
{
    // The encoder fills in and reads the bundled FFmpeg's structs, so it only runs against that FFmpeg.
    auto const& functions = FFmpegFunctions::bundled();

    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            functions.avcodec_free_context(&codec_context);
            functions.av_packet_free(&packet);
            functions.av_frame_free(&frame);
        }
    };

    auto const* codec = functions.avcodec_find_encoder(functions.ffmpeg_codec_id(codec_id));
    if (!codec)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find FFmpeg encoder for codec {}", codec_id);

    codec_context = functions.avcodec_alloc_context3(codec);
    if (!codec_context)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    if (width == 0 || height == 0 || width > NumericLimits<i32>::max() || height > NumericLimits<i32>::max())
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Video has an unusable size"sv);
    codec_context->width = static_cast<int>(width);
    codec_context->height = static_cast<int>(height);
    codec_context->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_context->time_base = TIME_BASE;
    codec_context->bit_rate = bitrate;
    // Generate key frames on request: their larger size adds delay to a live stream.
    codec_context->gop_size = NumericLimits<int>::max();
    codec_context->thread_count = 1;

    // libvpx: give up quality for speed as far as needed to keep up, hold no pictures back to look ahead, and keep
    // the frames decodable when some of their packets are lost.
    if (codec_id == CodecID::VP8) {
        static constexpr i64 REALTIME_DEADLINE = 1;
        static constexpr i64 FASTEST_REALTIME_SPEED = 8;
        (void)functions.av_opt_set_int(codec_context->priv_data, "deadline", REALTIME_DEADLINE, 0);
        (void)functions.av_opt_set_int(codec_context->priv_data, "cpu-used", FASTEST_REALTIME_SPEED, 0);
        (void)functions.av_opt_set_int(codec_context->priv_data, "lag-in-frames", 0, 0);
        (void)functions.av_opt_set_int(codec_context->priv_data, "error-resilient", 1, 0);
    }

    if (functions.avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg encoder {}", codec_id);

    packet = functions.av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = functions.av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    auto encoder = DECODER_TRY_ALLOC(try_make<FFmpegVideoEncoder>(functions, codec_context, packet, frame));
    memory_guard.disarm();
    return encoder;
}

FFmpegVideoEncoder::FFmpegVideoEncoder(FFmpegFunctions const& functions, AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame)
    : m_functions(functions)
    , m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
{
}

FFmpegVideoEncoder::~FFmpegVideoEncoder()
{
    m_functions.av_packet_free(&m_packet);
    m_functions.av_frame_free(&m_frame);
    m_functions.avcodec_free_context(&m_codec_context);
}

u32 FFmpegVideoEncoder::width() const
{
    return static_cast<u32>(m_codec_context->width);
}

u32 FFmpegVideoEncoder::height() const
{
    return static_cast<u32>(m_codec_context->height);
}

static void copy_plane(u8* destination, int destination_bytes_per_row, ReadonlyBytes source, u32 width, u32 height)
{
    for (u32 row = 0; row < height; ++row)
        __builtin_memcpy(destination + static_cast<size_t>(row) * destination_bytes_per_row, source.data() + static_cast<size_t>(row) * width, width);
}

DecoderErrorOr<Optional<FFmpegVideoEncoder::Frame>> FFmpegVideoEncoder::encode(I420Picture const& picture, AK::Duration timestamp, bool as_key_frame)
{
    if (picture.width() != width() || picture.height() != height())
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Picture is not the size the encoder was made for"sv);

    m_functions.av_frame_unref(m_frame);
    m_frame->width = m_codec_context->width;
    m_frame->height = m_codec_context->height;
    m_frame->format = AV_PIX_FMT_YUV420P;
    m_frame->pts = m_functions.av_rescale_q(timestamp.to_microseconds(), AVRational { 1, 1'000'000 }, TIME_BASE);
    m_frame->pict_type = as_key_frame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (m_functions.av_frame_get_buffer(m_frame, 0) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate encoder frame buffer"sv);

    copy_plane(m_frame->data[0], m_frame->linesize[0], picture.y(), picture.width(), picture.height());
    copy_plane(m_frame->data[1], m_frame->linesize[1], picture.u(), picture.chroma_width(), picture.chroma_height());
    copy_plane(m_frame->data[2], m_frame->linesize[2], picture.v(), picture.chroma_width(), picture.chroma_height());

    if (auto result = m_functions.avcodec_send_frame(m_codec_context, m_frame); result < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg encoder reported error code {:x}", result);

    m_functions.av_packet_unref(m_packet);
    auto result = m_functions.avcodec_receive_packet(m_codec_context, m_packet);
    if (result == AVERROR(EAGAIN))
        return OptionalNone {};
    if (result < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg encoder encountered an unexpected error retrieving a frame with code {:x}", result);

    auto data = ByteBuffer::copy(m_packet->data, static_cast<size_t>(m_packet->size));
    if (data.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to copy encoded frame bytes"sv);
    return Frame { .data = data.release_value(), .is_key_frame = (m_packet->flags & AV_PKT_FLAG_KEY) != 0 };
}

}
