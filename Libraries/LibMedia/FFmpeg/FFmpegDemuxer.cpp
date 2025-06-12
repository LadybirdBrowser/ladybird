/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Stream.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>

namespace Media::FFmpeg {

FFmpegDemuxer::FFmpegDemuxer(NonnullOwnPtr<SeekableStream> stream, NonnullOwnPtr<Media::FFmpeg::FFmpegIOContext> io_context)
    : m_stream(move(stream))
    , m_io_context(move(io_context))
{
}

FFmpegDemuxer::~FFmpegDemuxer()
{
    if (m_packet != nullptr)
        av_packet_free(&m_packet);
    if (m_codec_context != nullptr)
        avcodec_free_context(&m_codec_context);
    if (m_format_context != nullptr)
        avformat_close_input(&m_format_context);
}

ErrorOr<NonnullOwnPtr<FFmpegDemuxer>> FFmpegDemuxer::create(NonnullOwnPtr<SeekableStream> stream)
{
    auto io_context = TRY(Media::FFmpeg::FFmpegIOContext::create(*stream));
    auto demuxer = make<FFmpegDemuxer>(move(stream), move(io_context));

    // Open the container
    demuxer->m_format_context = avformat_alloc_context();
    if (demuxer->m_format_context == nullptr)
        return Error::from_string_literal("Failed to allocate format context");
    demuxer->m_format_context->pb = demuxer->m_io_context->avio_context();
    if (avformat_open_input(&demuxer->m_format_context, nullptr, nullptr, nullptr) < 0)
        return Error::from_string_literal("Failed to open input for format parsing");

    // Read stream info; doing this is required for headerless formats like MPEG
    if (avformat_find_stream_info(demuxer->m_format_context, nullptr) < 0)
        return Error::from_string_literal("Failed to find stream info");

    demuxer->m_packet = av_packet_alloc();
    if (demuxer->m_packet == nullptr)
        return Error::from_string_literal("Failed to allocate packet");

    return demuxer;
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::duration_of_track_in_milliseconds(Track const& track)
{
    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];

    if (stream->duration >= 0) {
        auto time_base = av_q2d(stream->time_base);
        double duration_in_milliseconds = static_cast<double>(stream->duration) * time_base * 1000.0;
        return AK::Duration::from_milliseconds(AK::round_to<int64_t>(duration_in_milliseconds));
    }

    // If the stream doesn't specify the duration, fallback to what the container says the duration is.
    // If the container doesn't know the duration, then we're out of luck. Return an error.
    if (m_format_context->duration < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Negative stream duration");

    double duration_in_milliseconds = (static_cast<double>(m_format_context->duration) / AV_TIME_BASE) * 1000.0;
    return AK::Duration::from_milliseconds(AK::round_to<int64_t>(duration_in_milliseconds));
}

DecoderErrorOr<Vector<Track>> FFmpegDemuxer::get_tracks_for_type(TrackType type)
{
    AVMediaType media_type;

    switch (type) {
    case TrackType::Video:
        media_type = AVMediaType::AVMEDIA_TYPE_VIDEO;
        break;
    case TrackType::Audio:
        media_type = AVMediaType::AVMEDIA_TYPE_AUDIO;
        break;
    case TrackType::Subtitles:
        media_type = AVMediaType::AVMEDIA_TYPE_SUBTITLE;
        break;
    }

    // Find the best stream to play within the container
    int best_stream_index = av_find_best_stream(m_format_context, media_type, -1, -1, nullptr, 0);
    if (best_stream_index == AVERROR_STREAM_NOT_FOUND)
        return DecoderError::format(DecoderErrorCategory::Unknown, "No stream for given type found in container");
    if (best_stream_index == AVERROR_DECODER_NOT_FOUND)
        return DecoderError::format(DecoderErrorCategory::Unknown, "No suitable decoder found for stream");
    if (best_stream_index < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to find a stream for the given type");

    auto* stream = m_format_context->streams[best_stream_index];

    Track track(type, best_stream_index);

    if (type == TrackType::Video) {
        track.set_video_data({
            .duration = TRY(duration_of_track_in_milliseconds(track)),
            .pixel_width = static_cast<u64>(stream->codecpar->width),
            .pixel_height = static_cast<u64>(stream->codecpar->height),
        });
    }

    Vector<Track> tracks;
    tracks.append(move(track));
    return tracks;
}

DecoderErrorOr<Optional<AK::Duration>> FFmpegDemuxer::seek_to_most_recent_keyframe(Track track, AK::Duration timestamp, Optional<AK::Duration> earliest_available_sample)
{
    // FIXME: What do we do with this here?
    (void)earliest_available_sample;

    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];
    auto time_base = av_q2d(stream->time_base);
    auto time_in_seconds = static_cast<double>(timestamp.to_milliseconds()) / 1000.0 / time_base;
    auto sample_timestamp = AK::round_to<int64_t>(time_in_seconds);

    if (av_seek_frame(m_format_context, stream->index, sample_timestamp, AVSEEK_FLAG_BACKWARD) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to seek");

    return timestamp;
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::duration(Track track)
{
    return duration_of_track_in_milliseconds(track);
}

DecoderErrorOr<CodecID> FFmpegDemuxer::get_codec_id_for_track(Track track)
{
    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];
    return media_codec_id_from_ffmpeg_codec_id(stream->codecpar->codec_id);
}

DecoderErrorOr<ReadonlyBytes> FFmpegDemuxer::get_codec_initialization_data_for_track(Track track)
{
    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];
    return ReadonlyBytes { stream->codecpar->extradata, static_cast<size_t>(stream->codecpar->extradata_size) };
}

DecoderErrorOr<Sample> FFmpegDemuxer::get_next_sample_for_track(Track track)
{
    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];

    for (;;) {
        auto read_frame_error = av_read_frame(m_format_context, m_packet);
        if (read_frame_error < 0) {
            if (read_frame_error == AVERROR_EOF)
                return DecoderError::format(DecoderErrorCategory::EndOfStream, "End of stream");

            return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to read frame");
        }
        if (m_packet->stream_index != stream->index) {
            av_packet_unref(m_packet);
            continue;
        }

        auto color_primaries = static_cast<ColorPrimaries>(stream->codecpar->color_primaries);
        auto transfer_characteristics = static_cast<TransferCharacteristics>(stream->codecpar->color_trc);
        auto matrix_coefficients = static_cast<MatrixCoefficients>(stream->codecpar->color_space);
        auto color_range = [stream] {
            switch (stream->codecpar->color_range) {
            case AVColorRange::AVCOL_RANGE_MPEG:
                return VideoFullRangeFlag::Studio;
            case AVColorRange::AVCOL_RANGE_JPEG:
                return VideoFullRangeFlag::Full;
            default:
                return VideoFullRangeFlag::Unspecified;
            }
        }();

        auto time_base = av_q2d(stream->time_base);
        double timestamp_in_milliseconds = static_cast<double>(m_packet->pts) * time_base * 1000.0;

        // Copy the packet data so that we have a permanent reference to it whilst the Sample is alive, which allows us
        // to wipe the packet afterwards.
        auto packet_data = DECODER_TRY_ALLOC(ByteBuffer::copy(m_packet->data, m_packet->size));

        auto sample = Sample(
            AK::Duration::from_milliseconds(AK::round_to<int64_t>(timestamp_in_milliseconds)),
            move(packet_data),
            VideoSampleData(CodingIndependentCodePoints(color_primaries, transfer_characteristics, matrix_coefficients, color_range)));

        // Wipe the packet now that the data is safe.
        av_packet_unref(m_packet);
        return sample;
    }
}

}
