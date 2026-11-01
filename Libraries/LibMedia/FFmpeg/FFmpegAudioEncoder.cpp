/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibMedia/FFmpeg/FFmpegChannelLayout.h>
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegHelpers.h>

#include "FFmpegAudioEncoder.h"

namespace Media::FFmpeg {

DecoderErrorOr<NonnullOwnPtr<FFmpegAudioEncoder>> FFmpegAudioEncoder::try_create(CodecID codec_id, Audio::SampleSpecification const& sample_specification, int bitrate)
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

    if (sample_specification.sample_rate() > NumericLimits<int>::max())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Sample rate is too large"sv);
    codec_context->sample_rate = static_cast<int>(sample_specification.sample_rate());
    codec_context->time_base = AVRational { 1, codec_context->sample_rate };
    codec_context->bit_rate = bitrate;
    codec_context->sample_fmt = AV_SAMPLE_FMT_FLT;

    if (sample_specification.channel_map().is_valid()) {
        auto channel_layout_result = channel_map_to_av_channel_layout(functions, sample_specification.channel_map());
        if (channel_layout_result.is_error())
            return DecoderError::format(DecoderErrorCategory::Invalid, channel_layout_result.error().string_literal());
        codec_context->ch_layout = channel_layout_result.release_value();
    }

    if (functions.avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg encoder {}", codec_id);

    packet = functions.av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = functions.av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    memory_guard.disarm();
    return DECODER_TRY_ALLOC(try_make<FFmpegAudioEncoder>(functions, codec_context, packet, frame));
}

FFmpegAudioEncoder::FFmpegAudioEncoder(FFmpegFunctions const& functions, AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame)
    : m_functions(functions)
    , m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
{
}

FFmpegAudioEncoder::~FFmpegAudioEncoder()
{
    m_functions.av_packet_free(&m_packet);
    m_functions.av_frame_free(&m_frame);
    m_functions.avcodec_free_context(&m_codec_context);
}

int FFmpegAudioEncoder::frame_size() const
{
    return m_codec_context->frame_size;
}

DecoderErrorOr<void> FFmpegAudioEncoder::receive_pcm_data(AK::Duration timestamp, AudioBlock const& block)
{
    auto channel_count = block.channel_count();
    if (channel_count == 0)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "AudioBlock has no channels"sv);

    auto frame_count = block.frame_count();
    if (frame_count == 0)
        return {};

    m_functions.av_frame_unref(m_frame);
    m_frame->nb_samples = static_cast<int>(frame_count);
    m_frame->format = AV_SAMPLE_FMT_FLT;
    m_frame->sample_rate = m_codec_context->sample_rate;
    if (m_functions.av_channel_layout_copy(&m_frame->ch_layout, &m_codec_context->ch_layout) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to copy AVChannelLayout to encoder frame"sv);
    m_frame->pts = m_functions.av_rescale_q(timestamp.to_microseconds(), AVRational { 1, 1'000'000 }, m_codec_context->time_base);

    if (m_functions.av_frame_get_buffer(m_frame, 0) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate encoder frame buffer"sv);

    // AV_SAMPLE_FMT_FLT is packed/interleaved, so the whole frame lives in data[0].
    auto sample_count = block.sample_count();
    auto destination = Span<float> { reinterpret_cast<float*>(m_frame->data[0]), sample_count };
    if (block.copy_to_interleaved(destination) < sample_count)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "AudioBlock produced fewer samples than expected"sv);

    auto result = m_functions.avcodec_send_frame(m_codec_context, m_frame);
    switch (result) {
    case 0:
        return {};
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg encoder cannot accept more frames until packets have been retrieved"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg encoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg encoder is in an invalid state"sv);
    case AVERROR(ENOMEM):
        return DecoderError::with_description(DecoderErrorCategory::Memory, "FFmpeg encoder ran out of internal memory"sv);
    default:
        return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg encoder reported error code {:x}", result);
    }
}

DecoderErrorOr<bool> FFmpegAudioEncoder::write_next_packet(Packet& out)
{
    m_functions.av_packet_unref(m_packet);
    auto result = m_functions.avcodec_receive_packet(m_codec_context, m_packet);
    switch (result) {
    case 0: {
        auto buffer_or_err = ByteBuffer::copy(m_packet->data, static_cast<size_t>(m_packet->size));
        if (buffer_or_err.is_error())
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to copy encoded packet bytes"sv);
        out.data = buffer_or_err.release_value();
        // The encoder reports timestamps in its time_base (1 / sample_rate), as it was fed them.
        out.timestamp = AK::Duration::from_microseconds(m_functions.av_rescale_q(m_packet->pts, m_codec_context->time_base, AVRational { 1, 1'000'000 }));
        out.duration = AK::Duration::from_microseconds(m_functions.av_rescale_q(m_packet->duration, m_codec_context->time_base, AVRational { 1, 1'000'000 }));
        return true;
    }
    case AVERROR(EAGAIN):
        return false;
    case AVERROR_EOF:
        return false;
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg encoder has not been opened"sv);
    default:
        return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg encoder encountered an unexpected error retrieving packets with code {:x}", result);
    }
}

void FFmpegAudioEncoder::signal_end_of_stream()
{
    auto result = m_functions.avcodec_send_frame(m_codec_context, nullptr);
    VERIFY(result == 0 || result == AVERROR_EOF);
}

void FFmpegAudioEncoder::flush()
{
    m_functions.avcodec_flush_buffers(m_codec_context);
}

}
