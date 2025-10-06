/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/Stream.h>
#include <AK/Time.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>

namespace Media::FFmpeg {

FFmpegDemuxer::FFmpegDemuxer(ReadonlyBytes data, NonnullOwnPtr<SeekableStream>&& stream, NonnullOwnPtr<Media::FFmpeg::FFmpegIOContext>&& io_context)
    : m_data(data)
    , m_stream(move(stream))
    , m_io_context(move(io_context))
{
}

FFmpegDemuxer::~FFmpegDemuxer()
{
    if (m_format_context != nullptr)
        avformat_close_input(&m_format_context);
}

static DecoderErrorOr<void> initialize_format_context(AVFormatContext*& format_context, AVIOContext& io_context)
{
    format_context = avformat_alloc_context();
    if (format_context == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate format context"sv);
    format_context->pb = &io_context;
    if (avformat_open_input(&format_context, nullptr, nullptr, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to open input for format parsing"sv);

    // Read stream info; doing this is required for headerless formats like MPEG
    if (avformat_find_stream_info(format_context, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to find stream info"sv);

    return {};
}

DecoderErrorOr<NonnullRefPtr<FFmpegDemuxer>> FFmpegDemuxer::from_data(ReadonlyBytes data)
{
    auto stream = DECODER_TRY_ALLOC(try_make<FixedMemoryStream>(data));
    auto io_context = DECODER_TRY_ALLOC(Media::FFmpeg::FFmpegIOContext::create(*stream));
    auto demuxer = DECODER_TRY_ALLOC(adopt_nonnull_ref_or_enomem(new (nothrow) FFmpegDemuxer(data, move(stream), move(io_context))));

    TRY(initialize_format_context(demuxer->m_format_context, *demuxer->m_io_context->avio_context()));

    return demuxer;
}

FFmpegDemuxer::TrackContext& FFmpegDemuxer::get_track_context(Track const& track)
{
    return *m_track_contexts.ensure(track, [&] {
        auto stream = MUST(try_make<FixedMemoryStream>(m_data));
        auto io_context = MUST(Media::FFmpeg::FFmpegIOContext::create(*stream));

        auto track_context = make<TrackContext>(move(stream), move(io_context));

        // We've already initialized a format context, so the only way this can fail is OOM.
        MUST(initialize_format_context(track_context->format_context, *track_context->io_context->avio_context()));

        track_context->packet = av_packet_alloc();
        VERIFY(track_context->packet != nullptr);
        return track_context;
    });
}

static inline AK::Duration time_units_to_duration(i64 time_units, int numerator, int denominator)
{
    VERIFY(numerator != 0);
    VERIFY(denominator != 0);
    auto seconds = time_units * numerator / denominator;
    auto seconds_in_time_units = seconds * denominator / numerator;
    auto remainder_in_time_units = time_units - seconds_in_time_units;
    auto nanoseconds = ((remainder_in_time_units * 1'000'000'000 * numerator) + (denominator / 2)) / denominator;
    return AK::Duration::from_seconds(seconds) + AK::Duration::from_nanoseconds(nanoseconds);
}

static inline AK::Duration time_units_to_duration(i64 time_units, AVRational const& time_base)
{
    return time_units_to_duration(time_units, time_base.num, time_base.den);
}

static inline i64 duration_to_time_units(AK::Duration duration, int numerator, int denominator)
{
    VERIFY(numerator != 0);
    VERIFY(denominator != 0);
    auto seconds = duration.to_truncated_seconds();
    auto nanoseconds = (duration - AK::Duration::from_seconds(seconds)).to_nanoseconds();

    auto time_units = seconds * denominator / numerator;
    time_units += nanoseconds * denominator / numerator / 1'000'000'000;
    return time_units;
}

static inline i64 duration_to_time_units(AK::Duration duration, AVRational const& time_base)
{
    return duration_to_time_units(duration, time_base.num, time_base.den);
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::total_duration()
{
    if (m_format_context->duration < 0) {
        return DecoderError::format(DecoderErrorCategory::Unknown, "Negative stream duration");
    }

    return time_units_to_duration(m_format_context->duration, 1, AV_TIME_BASE);
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::duration_of_track(Track const& track)
{
    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];

    if (stream->duration >= 0) {
        return time_units_to_duration(stream->duration, stream->time_base);
    }

    // If the stream doesn't specify the duration, fallback to what the container says the duration is.
    return total_duration();
}

DecoderErrorOr<Track> FFmpegDemuxer::get_track_for_stream_index(u32 stream_index)
{
    VERIFY(stream_index < m_format_context->nb_streams);

    auto& stream = *m_format_context->streams[stream_index];
    auto type = track_type_from_ffmpeg_media_type(stream.codecpar->codec_type);
    auto get_string_metadata = [&](char const* key) {
        auto* name_entry = av_dict_get(stream.metadata, key, nullptr, 0);
        if (name_entry == nullptr)
            return Utf16String();
        return Utf16String::from_utf8(StringView(name_entry->value, strlen(name_entry->value)));
    };
    auto name = get_string_metadata("title");
    auto language = get_string_metadata("language");
    Track track(type, stream_index, name, language);

    if (type == TrackType::Video) {
        track.set_video_data({
            .pixel_width = static_cast<u64>(stream.codecpar->width),
            .pixel_height = static_cast<u64>(stream.codecpar->height),
        });
    }

    return track;
}

DecoderErrorOr<Vector<Track>> FFmpegDemuxer::get_tracks_for_type(TrackType type)
{
    auto media_type = ffmpeg_media_type_from_track_type(type);
    Vector<Track> tracks = {};
    for (u32 i = 0; i < m_format_context->nb_streams; i++) {
        auto& stream = *m_format_context->streams[i];
        if (stream.codecpar->codec_type != media_type)
            continue;

        tracks.append(TRY(get_track_for_stream_index(i)));
    }
    return tracks;
}

DecoderErrorOr<Optional<Track>> FFmpegDemuxer::get_preferred_track_for_type(TrackType type)
{
    auto media_type = ffmpeg_media_type_from_track_type(type);
    auto best_stream_index = av_find_best_stream(m_format_context, media_type, -1, -1, nullptr, 0);
    if (best_stream_index < 0)
        return OptionalNone();

    return get_track_for_stream_index(best_stream_index);
}

DecoderErrorOr<DemuxerSeekResult> FFmpegDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions)
{
    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;

    VERIFY(format_context.nb_streams == m_format_context->nb_streams);
    VERIFY(track.identifier() < format_context.nb_streams);
    auto& stream = *format_context.streams[track.identifier()];
    auto av_timestamp = duration_to_time_units(timestamp, stream.time_base);

    auto seek_succeeded = false;
    if (track_context.is_seekable) {
        if (av_seek_frame(&format_context, stream.index, av_timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            seek_succeeded = false;
            track_context.is_seekable = false;
        } else {
            seek_succeeded = true;
        }
    }
    if (!seek_succeeded) {
        track_context.is_seekable = false;
        auto av_base_timestamp = duration_to_time_units(timestamp, AV_TIME_BASE_Q);
        if (av_seek_frame(&format_context, -1, av_base_timestamp, AVSEEK_FLAG_BACKWARD) < 0)
            return DecoderError::format(DecoderErrorCategory::Corrupted, "Failed to seek");
    }

    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<CodecID> FFmpegDemuxer::get_codec_id_for_track(Track const& track)
{
    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];
    return media_codec_id_from_ffmpeg_codec_id(stream->codecpar->codec_id);
}

DecoderErrorOr<ReadonlyBytes> FFmpegDemuxer::get_codec_initialization_data_for_track(Track const& track)
{
    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];
    return ReadonlyBytes { stream->codecpar->extradata, static_cast<size_t>(stream->codecpar->extradata_size) };
}

DecoderErrorOr<CodedFrame> FFmpegDemuxer::get_next_sample_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;
    auto& packet = *track_context.packet;

    VERIFY(format_context.nb_streams == m_format_context->nb_streams);
    VERIFY(track.identifier() < format_context.nb_streams);
    auto& stream = *format_context.streams[track.identifier()];

    for (;;) {
        auto read_frame_error = av_read_frame(&format_context, &packet);
        if (read_frame_error < 0) {
            if (read_frame_error == AVERROR_EOF)
                return DecoderError::format(DecoderErrorCategory::EndOfStream, "End of stream");

            return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to read frame");
        }
        if (packet.stream_index != stream.index) {
            av_packet_unref(&packet);
            continue;
        }

        auto auxiliary_data = [&]() -> CodedFrame::AuxiliaryData {
            if (track.type() == TrackType::Video) {
                auto color_primaries = static_cast<ColorPrimaries>(stream.codecpar->color_primaries);
                auto transfer_characteristics = static_cast<TransferCharacteristics>(stream.codecpar->color_trc);
                auto matrix_coefficients = static_cast<MatrixCoefficients>(stream.codecpar->color_space);
                auto color_range = [stream] {
                    switch (stream.codecpar->color_range) {
                    case AVColorRange::AVCOL_RANGE_MPEG:
                        return VideoFullRangeFlag::Studio;
                    case AVColorRange::AVCOL_RANGE_JPEG:
                        return VideoFullRangeFlag::Full;
                    default:
                        return VideoFullRangeFlag::Unspecified;
                    }
                }();
                return CodedVideoFrameData(CodingIndependentCodePoints(color_primaries, transfer_characteristics, matrix_coefficients, color_range));
            }
            if (track.type() == TrackType::Audio) {
                return CodedAudioFrameData();
            }
            VERIFY_NOT_REACHED();
        }();

        // Copy the packet data so that we have a permanent reference to it whilst the Sample is alive, which allows us
        // to wipe the packet afterwards.
        auto packet_data = DECODER_TRY_ALLOC(ByteBuffer::copy(packet.data, packet.size));

        auto flags = (packet.flags & AV_PKT_FLAG_KEY) != 0 ? FrameFlags::Keyframe : FrameFlags::None;
        auto sample = CodedFrame(
            time_units_to_duration(packet.pts, stream.time_base),
            flags,
            move(packet_data),
            auxiliary_data);

        // Wipe the packet now that the data is safe.
        av_packet_unref(&packet);
        return sample;
    }
}

FFmpegDemuxer::TrackContext::~TrackContext()
{
    av_packet_free(&packet);
    avformat_free_context(format_context);
}

}
