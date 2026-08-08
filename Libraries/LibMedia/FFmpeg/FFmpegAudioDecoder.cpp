/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibCore/System.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>

#include "FFmpegAudioDecoder.h"

namespace Media::FFmpeg {

static bool aac_audio_object_type_is_supported(ParsedCodec const& codec)
{
    auto parameters = codec.aac_parameters();
    if (!parameters.has_value() || !parameters->audio_object_type.has_value())
        return true;
    // We can't ask FFmpeg which variants of AAC are supported, so we use a set here that is likely supported instead.
    return first_is_one_of(*parameters->audio_object_type, 2u, 5u, 29u);
}

Optional<DecoderCapabilities> FFmpegAudioDecoder::capabilities(ParsedCodec const& codec)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Audio)
        return {};
    if (codec.codec_id() == CodecID::AAC && !aac_audio_object_type_is_supported(codec))
        return {};
    if (!avcodec_find_decoder(ffmpeg_codec_id_from_media_codec_id(codec.codec_id())))
        return {};
    return DecoderCapabilities { .smooth = true, .power_efficient = false };
}

DecoderErrorOr<NonnullOwnPtr<FFmpegAudioDecoder>> FFmpegAudioDecoder::try_create(CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            avcodec_free_context(&codec_context);
            av_packet_free(&packet);
            av_frame_free(&frame);
        }
    };

    auto ff_codec_id = ffmpeg_codec_id_from_media_codec_id(codec_id);
    auto const* codec = avcodec_find_decoder(ff_codec_id);
    if (!codec)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find FFmpeg decoder for codec {}", codec_id);

    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    codec_context->time_base = { 1, 1'000'000 };
    codec_context->thread_count = static_cast<int>(min(Core::System::hardware_concurrency(), 4));

    if (sample_specification.sample_rate() > NumericLimits<int>::max())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Sample rate is too large"sv);
    codec_context->sample_rate = static_cast<int>(sample_specification.sample_rate());

    if (sample_specification.channel_map().is_valid()) {
        auto channel_layout_result = channel_map_to_av_channel_layout(sample_specification.channel_map());
        if (channel_layout_result.is_error())
            return DecoderError::format(DecoderErrorCategory::Invalid, channel_layout_result.error().string_literal());
        codec_context->ch_layout = channel_layout_result.release_value();
    }

    if (!codec_initialization_data.is_empty()) {
        if (codec_initialization_data.size() > NumericLimits<int>::max())
            return DecoderError::corrupted("Codec initialization data is too large"sv);

        codec_context->extradata = static_cast<u8*>(av_malloc(codec_initialization_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codec_context->extradata)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate codec initialization data buffer for FFmpeg codec"sv);

        memcpy(codec_context->extradata, codec_initialization_data.data(), codec_initialization_data.size());
        codec_context->extradata_size = static_cast<int>(codec_initialization_data.size());
    }

    if (avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", codec_id);

    packet = av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    memory_guard.disarm();
    return DECODER_TRY_ALLOC(try_make<FFmpegAudioDecoder>(codec_context, packet, frame));
}

FFmpegAudioDecoder::FFmpegAudioDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame)
    : m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
{
}

FFmpegAudioDecoder::~FFmpegAudioDecoder()
{
    av_packet_free(&m_packet);
    av_frame_free(&m_frame);
    avcodec_free_context(&m_codec_context);
}

DecoderErrorOr<void> FFmpegAudioDecoder::receive_coded_data(CodedFrame const& coded_frame)
{
    auto coded_data = coded_frame.data();
    VERIFY(coded_data.size() < NumericLimits<int>::max());

    m_packet->data = const_cast<u8*>(coded_data.data());
    m_packet->size = static_cast<int>(coded_data.size());
    m_packet->pts = coded_frame.presentation_timestamp().to_microseconds();
    m_packet->dts = coded_frame.decode_timestamp().to_microseconds();

    ScopeGuard clear_packet_side_data { [&] { av_packet_free_side_data(m_packet); } };
    auto new_codec_configuration = coded_frame.new_codec_configuration();
    if (new_codec_configuration.has_value() && !new_codec_configuration->is_empty())
        TRY(add_new_extradata_to_packet(*m_packet, *new_codec_configuration));

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    switch (result) {
    case 0:
        return {};
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder cannot decode any more data until frames have been retrieved"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
    case AVERROR(ENOMEM):
        return DecoderError::with_description(DecoderErrorCategory::Memory, "FFmpeg codec ran out of internal memory"sv);
    default:
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "FFmpeg codec reports that the data is corrupted"sv);
    }
}

void FFmpegAudioDecoder::signal_end_of_stream()
{
    m_packet->data = nullptr;
    m_packet->size = 0;
    m_packet->pts = 0;
    m_packet->dts = 0;

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    VERIFY(result == 0 || result == AVERROR_EOF);
}

template<typename T>
static float float_sample_from_frame_data(u8** data, size_t plane, size_t index);

template<>
float float_sample_from_frame_data<u8>(u8** data, size_t plane, size_t index)
{
    return static_cast<float>(data[plane][index] - 128) / 128;
}

template<typename T>
requires(IsSigned<T>)
static float float_sample_from_frame_data(u8** data, size_t plane, size_t index)
{
    auto* pointer = reinterpret_cast<T*>(data[plane]);
    constexpr float inverse_peak = 1.0f / (static_cast<float>(NumericLimits<T>::max()) + 1.0f);
    return static_cast<float>(pointer[index]) * inverse_peak;
}

template<typename T>
requires(IsFloatingPoint<T>)
static float float_sample_from_frame_data(u8** data, size_t plane, size_t index)
{
    auto* pointer = reinterpret_cast<T*>(data[plane]);
    return pointer[index];
}

DecoderErrorOr<void> FFmpegAudioDecoder::write_next_block(AudioBlock& block)
{
    VERIFY(m_frame->nb_samples >= 0);
    if (m_frame_read_offset >= static_cast<size_t>(m_frame->nb_samples)) {
        auto result = avcodec_receive_frame(m_codec_context, m_frame);

        switch (result) {
        case 0:
            break;
        case AVERROR(EAGAIN):
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder has no frames available, send more input"sv);
        case AVERROR_EOF:
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
        case AVERROR(EINVAL):
            return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
        default:
            return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg codec encountered an unexpected error retrieving frames with code {:x}", result);
        }

        m_frame_read_offset = 0;

        if (m_frame->sample_rate <= 0)
            return DecoderError::corrupted("FFmpeg decoder created a packet with an invalid sample rate"sv);
        VERIFY(m_frame->nb_samples >= 0);
    }

    auto channel_map_result = av_channel_layout_to_channel_map(m_frame->ch_layout);
    if (channel_map_result.is_error())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, channel_map_result.error().string_literal());
    auto channel_map = channel_map_result.release_value();
    auto sample_specification = Audio::SampleSpecification(m_frame->sample_rate, channel_map);

    auto format = static_cast<AVSampleFormat>(m_frame->format);
    auto is_planar = av_sample_fmt_is_planar(format) != 0;
    auto planar_format = av_get_planar_sample_fmt(format);

    auto total_frame_count = static_cast<size_t>(m_frame->nb_samples);
    auto channel_count = static_cast<size_t>(m_frame->ch_layout.nb_channels);
    auto frame_count = min(total_frame_count - m_frame_read_offset, AudioBlock::max_frame_count(channel_count));

    auto timestamp = AK::Duration::from_microseconds(m_frame->pts);
    timestamp += AK::Duration::from_time_units(AK::clamp_to<i64>(m_frame_read_offset), 1, sample_specification.sample_rate());
    block.initialize(sample_specification, timestamp, frame_count);

    auto sample_size = [&] {
        switch (planar_format) {
        case AV_SAMPLE_FMT_U8P:
            return sizeof(u8);
        case AV_SAMPLE_FMT_S16P:
            return sizeof(i16);
        case AV_SAMPLE_FMT_S32P:
            return sizeof(i32);
        case AV_SAMPLE_FMT_FLTP:
            return sizeof(float);
        case AV_SAMPLE_FMT_DBLP:
            return sizeof(double);
        case AV_SAMPLE_FMT_S64P:
            return sizeof(i64);
        default:
            VERIFY_NOT_REACHED();
        }
    }();

    VERIFY(m_frame->linesize[0] > 0);
    if (is_planar)
        VERIFY(static_cast<size_t>(m_frame->linesize[0]) >= total_frame_count * sample_size);
    else
        VERIFY(static_cast<size_t>(m_frame->linesize[0]) >= total_frame_count * channel_count * sample_size);

    for (size_t channel = 0; channel < channel_count; ++channel) {
        auto channel_data = block.channel_data(channel);
        for (size_t frame = 0; frame < frame_count; ++frame) {
            auto source_frame = m_frame_read_offset + frame;
            size_t plane = 0;
            size_t index_in_plane = (source_frame * channel_count) + channel;
            if (is_planar) {
                plane = channel;
                index_in_plane = source_frame;
            }

            auto float_sample = [&] {
                switch (planar_format) {
                case AV_SAMPLE_FMT_U8P:
                    return float_sample_from_frame_data<u8>(m_frame->extended_data, plane, index_in_plane);
                case AV_SAMPLE_FMT_S16P:
                    return float_sample_from_frame_data<i16>(m_frame->extended_data, plane, index_in_plane);
                case AV_SAMPLE_FMT_S32P:
                    return float_sample_from_frame_data<i32>(m_frame->extended_data, plane, index_in_plane);
                case AV_SAMPLE_FMT_FLTP:
                    return float_sample_from_frame_data<float>(m_frame->extended_data, plane, index_in_plane);
                case AV_SAMPLE_FMT_DBLP:
                    return float_sample_from_frame_data<double>(m_frame->extended_data, plane, index_in_plane);
                case AV_SAMPLE_FMT_S64P:
                    return float_sample_from_frame_data<i64>(m_frame->extended_data, plane, index_in_plane);
                default:
                    VERIFY_NOT_REACHED();
                }
            }();
            channel_data[frame] = float_sample;
        }
    }

    m_frame_read_offset += frame_count;
    return {};
}

void FFmpegAudioDecoder::flush()
{
    avcodec_flush_buffers(m_codec_context);
    av_frame_unref(m_frame);
    m_frame_read_offset = 0;
}

}
