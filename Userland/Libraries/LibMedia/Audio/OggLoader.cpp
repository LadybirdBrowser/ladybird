/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OggLoader.h"
#include <AK/BitStream.h>
#include <AK/ScopeGuard.h>
#include <LibCore/System.h>

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
#    define USE_FFMPEG_CH_LAYOUT
#endif

namespace Audio {

OggLoaderPlugin::OggLoaderPlugin(NonnullOwnPtr<SeekableStream> stream)
    : LoaderPlugin(move(stream))
{
}

OggLoaderPlugin::~OggLoaderPlugin()
{
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    avcodec_free_context(&m_codec_context);
    avformat_close_input(&m_format_context);
    avio_context_free(&m_avio_context);
    av_free(m_avio_buffer);
}

ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> OggLoaderPlugin::create(NonnullOwnPtr<SeekableStream> stream)
{
    auto loader = make<OggLoaderPlugin>(move(stream));
    TRY(loader->initialize());
    return loader;
}

MaybeLoaderError OggLoaderPlugin::initialize()
{
    m_format_context = avformat_alloc_context();
    if (m_format_context == nullptr)
        return LoaderError { LoaderError::Category::IO, "Failed to allocate format context" };

    m_avio_buffer = av_malloc(PAGE_SIZE);
    if (m_avio_buffer == nullptr)
        return LoaderError { LoaderError::Category::IO, "Failed to allocate AVIO buffer" };

    // This AVIOContext explains to avformat how to interact with our stream
    m_avio_context = avio_alloc_context(
        static_cast<unsigned char*>(m_avio_buffer),
        PAGE_SIZE,
        0,
        m_stream.ptr(),
        [](void* opaque, u8* buffer, int size) -> int {
            auto& stream = *static_cast<SeekableStream*>(opaque);
            AK::Bytes buffer_bytes { buffer, static_cast<size_t>(size) };
            auto read_bytes_or_error = stream.read_some(buffer_bytes);
            if (read_bytes_or_error.is_error()) {
                if (read_bytes_or_error.error().code() == EOF)
                    return AVERROR_EOF;
                return AVERROR_UNKNOWN;
            }
            return static_cast<int>(read_bytes_or_error.value().size());
        },
        nullptr,
        [](void* opaque, int64_t offset, int origin) -> int64_t {
            auto& stream = *static_cast<SeekableStream*>(opaque);
            auto seek_mode_from_whence = [](int origin) -> SeekMode {
                if (origin == SEEK_CUR)
                    return SeekMode::FromCurrentPosition;
                if (origin == SEEK_END)
                    return SeekMode::FromEndPosition;
                return SeekMode::SetPosition;
            };
            auto offset_or_error = stream.seek(offset, seek_mode_from_whence(origin));
            if (offset_or_error.is_error())
                return -EIO;
            return 0;
        });

    m_format_context->pb = m_avio_context;

    // Open the stream as an ogg container
    auto* av_input_format = av_find_input_format("ogg");
    if (av_input_format == nullptr)
        return LoaderError { LoaderError::Category::Internal, "Failed to obtain input format" };

    if (avformat_open_input(&m_format_context, nullptr, av_input_format, nullptr) < 0)
        return LoaderError { LoaderError::Category::IO, "Failed to open input for format parsing" };

    // Find the best stream to play within the container
    int best_stream_index = av_find_best_stream(m_format_context, AVMediaType::AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (best_stream_index < 0)
        return LoaderError { LoaderError::Category::Format, "Failed to find an audio stream" };
    m_audio_stream = m_format_context->streams[best_stream_index];

    // Set up the codec to decode the audio stream
    AVCodec const* codec = avcodec_find_decoder(m_audio_stream->codecpar->codec_id);
    if (codec == nullptr)
        return LoaderError { LoaderError::Category::IO, "Failed to find a suitable decoder" };

    m_codec_context = avcodec_alloc_context3(codec);
    if (m_codec_context == nullptr)
        return LoaderError { LoaderError::Category::IO, "Failed to allocate the codec context" };

    if (avcodec_parameters_to_context(m_codec_context, m_audio_stream->codecpar) < 0)
        return LoaderError { LoaderError::Category::IO, "Failed to copy codec parameters" };

    m_codec_context->thread_count = AK::min(static_cast<int>(Core::System::hardware_concurrency()), 4);

    if (avcodec_open2(m_codec_context, codec, nullptr) < 0)
        return LoaderError { LoaderError::Category::IO, "Failed to open input for decoding" };

    double duration_in_seconds = m_audio_stream->duration * time_base();
    m_total_samples = AK::round_to<decltype(m_total_samples)>(m_codec_context->sample_rate * duration_in_seconds);

    // Prepare packet and frame buffers
    m_packet = av_packet_alloc();
    if (m_packet == nullptr)
        return LoaderError { LoaderError::Category::IO, "Failed to allocate packet" };

    m_frame = av_frame_alloc();
    if (m_frame == nullptr)
        return LoaderError { LoaderError::Category::IO, "Failed to allocate frame" };

    return {};
}

double OggLoaderPlugin::time_base() const
{
    return static_cast<double>(m_audio_stream->time_base.num) / m_audio_stream->time_base.den;
}

bool OggLoaderPlugin::sniff(SeekableStream& stream)
{
    LittleEndianInputBitStream bit_input { MaybeOwned<Stream>(stream) };
    auto maybe_ogg = bit_input.read_bits<u32>(32);
    return !maybe_ogg.is_error() && maybe_ogg.value() == 0x5367674F; // "OggS"
}

static ErrorOr<FixedArray<Sample>> extract_samples_from_frame(AVFrame& frame)
{
    size_t number_of_samples = frame.nb_samples;
#ifdef USE_FFMPEG_CH_LAYOUT
    size_t number_of_channels = frame.ch_layout.nb_channels;
#else
    size_t number_of_channels = frame.channels;
#endif
    AVSampleFormat format = static_cast<AVSampleFormat>(frame.format);

    VERIFY(number_of_samples > 0);

    // FIXME: handle number_of_channels > 2
    if (number_of_channels != 1 && number_of_channels != 2)
        return Error::from_string_view("Unsupported number of channels"sv);

    // FIXME: handle other formats
    if (format != AV_SAMPLE_FMT_FLTP)
        return Error::from_string_view("Unsupported sample format"sv);

    // FIXME: handle non-planar data (this is also implied by *P format(s) above)
    if (av_sample_fmt_is_planar(format) != 1)
        return Error::from_string_view("Non-planar sample data is not supported yet"sv);

    auto read_sample = [&](uint8_t* plane, size_t sample) -> float {
        switch (format) {
        case AV_SAMPLE_FMT_FLTP:
            return reinterpret_cast<float*>(plane)[sample];
        default:
            VERIFY_NOT_REACHED();
        }
    };

    auto samples = TRY(FixedArray<Sample>::create(number_of_samples));
    for (size_t sample = 0; sample < number_of_samples; ++sample) {
        if (number_of_channels == 1) {
            samples.unchecked_at(sample) = Sample { read_sample(frame.extended_data[0], sample) };
        } else {
            samples.unchecked_at(sample) = Sample {
                read_sample(frame.extended_data[0], sample),
                read_sample(frame.extended_data[1], sample),
            };
        }
    }
    return samples;
}

ErrorOr<Vector<FixedArray<Sample>>, LoaderError> OggLoaderPlugin::load_chunks(size_t samples_to_read_from_input)
{
    Vector<FixedArray<Sample>> chunks {};

    for (;;) {
        // Obtain a packet and send it to the decoder
        if (av_read_frame(m_format_context, m_packet) < 0)
            return LoaderError { LoaderError::Category::IO, "Failed to read frame" };
        if (avcodec_send_packet(m_codec_context, m_packet) < 0)
            return LoaderError { LoaderError::Category::IO, "Failed to send packet" };
        av_packet_unref(m_packet);

        // Ask the decoder for a new frame. We might not have sent enough data yet
        auto receive_frame_error = avcodec_receive_frame(m_codec_context, m_frame);
        if (receive_frame_error == 0) {
            chunks.append(TRY(extract_samples_from_frame(*m_frame)));
            m_loaded_samples += m_frame->nb_samples;

            samples_to_read_from_input -= AK::min(samples_to_read_from_input, m_frame->nb_samples);
            if (samples_to_read_from_input == 0)
                break;
            continue;
        }

        if (receive_frame_error == AVERROR(EAGAIN))
            continue;
        if (receive_frame_error == AVERROR_EOF)
            return Error::from_errno(EOF);

        return LoaderError { LoaderError::Category::IO, "Failed to receive frame" };
    }

    av_frame_unref(m_frame);

    return chunks;
}

MaybeLoaderError OggLoaderPlugin::reset()
{
    return seek(0);
}

MaybeLoaderError OggLoaderPlugin::seek(int sample_index)
{
    auto sample_position_in_seconds = static_cast<double>(sample_index) / m_codec_context->sample_rate;
    auto sample_timestamp = AK::round_to<int64_t>(sample_position_in_seconds / time_base());

    if (av_seek_frame(m_format_context, m_audio_stream->index, sample_timestamp, 0) < 0)
        return LoaderError { LoaderError::Category::IO, "Failed to seek" };

    m_loaded_samples = sample_index;
    return {};
}

u32 OggLoaderPlugin::sample_rate()
{
    VERIFY(m_codec_context != nullptr);
    return m_codec_context->sample_rate;
}

u16 OggLoaderPlugin::num_channels()
{
    VERIFY(m_codec_context != nullptr);
#ifdef USE_FFMPEG_CH_LAYOUT
    return m_codec_context->ch_layout.nb_channels;
#else
    return m_codec_context->channels;
#endif
}

PcmSampleFormat OggLoaderPlugin::pcm_format()
{
    // FIXME: pcm_format() is unused, always return Float for now
    return PcmSampleFormat::Float32;
}

}
