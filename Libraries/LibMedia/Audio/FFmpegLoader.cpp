/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FFmpegLoader.h"
#include <AK/NumericLimits.h>
#include <LibCore/System.h>

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
#    define USE_FFMPEG_CH_LAYOUT
#endif
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 100)
#    define USE_CONSTIFIED_POINTERS
#endif

namespace Audio {

static constexpr int BUFFER_MAX_PROBE_SIZE = 64 * KiB;

FFmpegLoaderPlugin::FFmpegLoaderPlugin(NonnullOwnPtr<SeekableStream> stream, NonnullOwnPtr<Media::FFmpeg::FFmpegIOContext> io_context)
    : LoaderPlugin(move(stream))
    , m_io_context(move(io_context))
{
}

FFmpegLoaderPlugin::~FFmpegLoaderPlugin()
{
    if (m_frame != nullptr)
        av_frame_free(&m_frame);
    if (m_packet != nullptr)
        av_packet_free(&m_packet);
    if (m_codec_context != nullptr)
        avcodec_free_context(&m_codec_context);
    if (m_format_context != nullptr)
        avformat_close_input(&m_format_context);
}

ErrorOr<NonnullOwnPtr<LoaderPlugin>> FFmpegLoaderPlugin::create(NonnullOwnPtr<SeekableStream> stream)
{
    auto io_context = TRY(Media::FFmpeg::FFmpegIOContext::create(*stream));
    auto loader = make<FFmpegLoaderPlugin>(move(stream), move(io_context));
    TRY(loader->initialize());
    return loader;
}

ErrorOr<void> FFmpegLoaderPlugin::initialize()
{
    // Open the container
    m_format_context = avformat_alloc_context();
    if (m_format_context == nullptr)
        return Error::from_string_literal("Failed to allocate format context");
    m_format_context->pb = m_io_context->avio_context();
    if (avformat_open_input(&m_format_context, nullptr, nullptr, nullptr) < 0)
        return Error::from_string_literal("Failed to open input for format parsing");

    // Read stream info; doing this is required for headerless formats like MPEG
    if (avformat_find_stream_info(m_format_context, nullptr) < 0)
        return Error::from_string_literal("Failed to find stream info");

#ifdef USE_CONSTIFIED_POINTERS
    AVCodec const* codec {};
#else
    AVCodec* codec {};
#endif
    // Find the best stream to play within the container
    int best_stream_index = av_find_best_stream(m_format_context, AVMediaType::AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (best_stream_index == AVERROR_STREAM_NOT_FOUND)
        return Error::from_string_literal("No audio stream found in container");
    if (best_stream_index == AVERROR_DECODER_NOT_FOUND)
        return Error::from_string_literal("No suitable decoder found for stream");
    if (best_stream_index < 0)
        return Error::from_string_literal("Failed to find an audio stream");
    m_audio_stream = m_format_context->streams[best_stream_index];

    // Set up the context to decode the audio stream
    m_codec_context = avcodec_alloc_context3(codec);
    if (m_codec_context == nullptr)
        return Error::from_string_literal("Failed to allocate the codec context");

    if (avcodec_parameters_to_context(m_codec_context, m_audio_stream->codecpar) < 0)
        return Error::from_string_literal("Failed to copy codec parameters");

    m_codec_context->pkt_timebase = m_audio_stream->time_base;
    m_codec_context->thread_count = AK::min(static_cast<int>(Core::System::hardware_concurrency()), 4);

    if (avcodec_open2(m_codec_context, codec, nullptr) < 0)
        return Error::from_string_literal("Failed to open input for decoding");

    // This is an initial estimate of the total number of samples in the stream.
    // During decoding, we might need to increase the number as more frames come in.
    double duration_in_seconds = static_cast<double>(m_audio_stream->duration) * time_base();
    if (duration_in_seconds < 0)
        return Error::from_string_literal("Negative stream duration");
    m_total_samples = AK::round_to<decltype(m_total_samples)>(sample_rate() * duration_in_seconds);

    // Allocate packet (logical chunk of data) and frame (video / audio frame) buffers
    m_packet = av_packet_alloc();
    if (m_packet == nullptr)
        return Error::from_string_literal("Failed to allocate packet");

    m_frame = av_frame_alloc();
    if (m_frame == nullptr)
        return Error::from_string_literal("Failed to allocate frame");

    return {};
}

double FFmpegLoaderPlugin::time_base() const
{
    return av_q2d(m_audio_stream->time_base);
}

bool FFmpegLoaderPlugin::sniff(SeekableStream& stream)
{
    auto io_context = MUST(Media::FFmpeg::FFmpegIOContext::create(stream));
#ifdef USE_CONSTIFIED_POINTERS
    AVInputFormat const* detected_format {};
#else
    AVInputFormat* detected_format {};
#endif
    auto score = av_probe_input_buffer2(io_context->avio_context(), &detected_format, nullptr, nullptr, 0, BUFFER_MAX_PROBE_SIZE);
    return score > 0;
}

static ErrorOr<FixedArray<Sample>> extract_samples_from_frame(AVFrame& frame)
{
    size_t number_of_samples = frame.nb_samples;
    VERIFY(number_of_samples > 0);

#ifdef USE_FFMPEG_CH_LAYOUT
    size_t number_of_channels = frame.ch_layout.nb_channels;
#else
    size_t number_of_channels = frame.channels;
#endif
    auto format = static_cast<AVSampleFormat>(frame.format);
    auto packed_format = av_get_packed_sample_fmt(format);
    auto is_planar = av_sample_fmt_is_planar(format) == 1;

    // FIXME: handle number_of_channels > 2
    if (number_of_channels != 1 && number_of_channels != 2)
        return Error::from_string_view("Unsupported number of channels"sv);

    switch (format) {
    case AV_SAMPLE_FMT_FLTP:
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S32:
        break;
    default:
        // FIXME: handle other formats
        return Error::from_string_view("Unsupported sample format"sv);
    }

    auto get_plane_pointer = [&](size_t channel_index) -> uint8_t* {
        return is_planar ? frame.extended_data[channel_index] : frame.extended_data[0];
    };
    auto index_in_plane = [&](size_t sample_index, size_t channel_index) {
        if (is_planar)
            return sample_index;
        return sample_index * number_of_channels + channel_index;
    };
    auto read_sample = [&](uint8_t* data, size_t index) -> float {
        switch (packed_format) {
        case AV_SAMPLE_FMT_FLT:
            return reinterpret_cast<float*>(data)[index];
        case AV_SAMPLE_FMT_S16:
            return reinterpret_cast<i16*>(data)[index] / static_cast<float>(NumericLimits<i16>::max());
        case AV_SAMPLE_FMT_S32:
            return reinterpret_cast<i32*>(data)[index] / static_cast<float>(NumericLimits<i32>::max());
        default:
            VERIFY_NOT_REACHED();
        }
    };

    auto samples = TRY(FixedArray<Sample>::create(number_of_samples));
    for (size_t sample = 0; sample < number_of_samples; ++sample) {
        if (number_of_channels == 1) {
            samples.unchecked_at(sample) = Sample { read_sample(get_plane_pointer(0), index_in_plane(sample, 0)) };
        } else {
            samples.unchecked_at(sample) = Sample {
                read_sample(get_plane_pointer(0), index_in_plane(sample, 0)),
                read_sample(get_plane_pointer(1), index_in_plane(sample, 1)),
            };
        }
    }
    return samples;
}

ErrorOr<Vector<FixedArray<Sample>>> FFmpegLoaderPlugin::load_chunks(size_t samples_to_read_from_input)
{
    Vector<FixedArray<Sample>> chunks {};

    do {
        // Obtain a packet
        auto read_frame_error = av_read_frame(m_format_context, m_packet);
        if (read_frame_error < 0) {
            if (read_frame_error == AVERROR_EOF)
                break;
            return Error::from_string_literal("Failed to read frame");
        }
        if (m_packet->stream_index != m_audio_stream->index) {
            av_packet_unref(m_packet);
            continue;
        }

        // Send the packet to the decoder
        if (avcodec_send_packet(m_codec_context, m_packet) < 0)
            return Error::from_string_literal("Failed to send packet");
        av_packet_unref(m_packet);

        // Ask the decoder for a new frame. We might not have sent enough data yet
        auto receive_frame_error = avcodec_receive_frame(m_codec_context, m_frame);
        if (receive_frame_error != 0) {
            if (receive_frame_error == AVERROR(EAGAIN))
                continue;
            if (receive_frame_error == AVERROR_EOF)
                break;
            return Error::from_string_literal("Failed to receive frame");
        }

        chunks.append(TRY(extract_samples_from_frame(*m_frame)));

        // Use the frame's presentation timestamp to set the number of loaded samples
        m_loaded_samples = static_cast<int>(m_frame->pts * sample_rate() * time_base());
        if (m_loaded_samples > m_total_samples) [[unlikely]]
            m_total_samples = m_loaded_samples;

        samples_to_read_from_input -= AK::min(samples_to_read_from_input, m_frame->nb_samples);
    } while (samples_to_read_from_input > 0);

    return chunks;
}

ErrorOr<void> FFmpegLoaderPlugin::reset()
{
    return seek(0);
}

ErrorOr<void> FFmpegLoaderPlugin::seek(int sample_index)
{
    auto sample_position_in_seconds = static_cast<double>(sample_index) / sample_rate();
    auto sample_timestamp = AK::round_to<int64_t>(sample_position_in_seconds / time_base());

    if (av_seek_frame(m_format_context, m_audio_stream->index, sample_timestamp, AVSEEK_FLAG_ANY) < 0)
        return Error::from_string_literal("Failed to seek");
    avcodec_flush_buffers(m_codec_context);

    m_loaded_samples = sample_index;
    return {};
}

u32 FFmpegLoaderPlugin::sample_rate()
{
    VERIFY(m_codec_context != nullptr);
    return m_codec_context->sample_rate;
}

u16 FFmpegLoaderPlugin::num_channels()
{
    VERIFY(m_codec_context != nullptr);
#ifdef USE_FFMPEG_CH_LAYOUT
    return m_codec_context->ch_layout.nb_channels;
#else
    return m_codec_context->channels;
#endif
}

PcmSampleFormat FFmpegLoaderPlugin::pcm_format()
{
    // FIXME: pcm_format() is unused, always return Float for now
    return PcmSampleFormat::Float32;
}

ByteString FFmpegLoaderPlugin::format_name()
{
    if (!m_format_context)
        return "unknown";
    return m_format_context->iformat->name;
}

}
