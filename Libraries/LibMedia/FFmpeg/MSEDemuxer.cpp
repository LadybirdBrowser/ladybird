/*
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>
#include <LibMedia/FFmpeg/MSEDemuxer.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

namespace Media::FFmpeg {

// Helper functions for time conversion (copied from FFmpegDemuxer)
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

[[maybe_unused]] static inline i64 duration_to_time_units(AK::Duration duration, int numerator, int denominator)
{
    VERIFY(numerator != 0);
    VERIFY(denominator != 0);
    auto seconds = duration.to_truncated_seconds();
    auto nanoseconds = (duration - AK::Duration::from_seconds(seconds)).to_nanoseconds();
    auto time_units = seconds * denominator / numerator;
    time_units += nanoseconds * denominator / numerator / 1'000'000'000;
    return time_units;
}

[[maybe_unused]] static inline i64 duration_to_time_units(AK::Duration duration, AVRational const& time_base)
{
    return duration_to_time_units(duration, time_base.num, time_base.den);
}

// Custom AVIO read callback that reads from our growing buffer
int MSEDemuxer::avio_read_callback(void* opaque, uint8_t* buf, int buf_size)
{
    auto* demuxer = static_cast<MSEDemuxer*>(opaque);

    // Calculate how much data is available from current read position
    size_t available = demuxer->m_buffer.size() - demuxer->m_read_position;
    if (available == 0)
        return AVERROR_EOF;

    // Read as much as requested or available
    size_t to_read = min(static_cast<size_t>(buf_size), available);
    memcpy(buf, demuxer->m_buffer.data() + demuxer->m_read_position, to_read);
    demuxer->m_read_position += to_read;

    return static_cast<int>(to_read);
}

// Custom AVIO seek callback
int64_t MSEDemuxer::avio_seek_callback(void* opaque, int64_t offset, int whence)
{
    auto* demuxer = static_cast<MSEDemuxer*>(opaque);

    int64_t new_position = 0;
    switch (whence) {
    case SEEK_SET:
        new_position = offset;
        break;
    case SEEK_CUR:
        new_position = demuxer->m_read_position + offset;
        break;
    case SEEK_END:
        new_position = demuxer->m_buffer.size() + offset;
        break;
    case AVSEEK_SIZE:
        return demuxer->m_buffer.size();
    default:
        return AVERROR(EINVAL);
    }

    if (new_position < 0 || new_position > static_cast<int64_t>(demuxer->m_buffer.size()))
        return AVERROR(EINVAL);

    demuxer->m_read_position = new_position;
    dbgln("MSE: AVIO seek to {} (whence: {}) -> new position: {}", offset, whence, new_position);
    return new_position;
}

MSEDemuxer::MSEDemuxer()
{
}

MSEDemuxer::~MSEDemuxer()
{
    if (m_format_context != nullptr)
        avformat_close_input(&m_format_context);
    if (m_avio_context != nullptr) {
        // Free the buffer allocated by avio_alloc_context
        if (m_avio_context->buffer)
            av_free(m_avio_context->buffer);
        avio_context_free(&m_avio_context);
    }
}

MSEDemuxer::TrackContext::TrackContext()
{
}

MSEDemuxer::TrackContext::~TrackContext()
{
    if (format_context != nullptr)
        avformat_close_input(&format_context);
    if (packet != nullptr)
        av_packet_free(&packet);
}

DecoderErrorOr<NonnullRefPtr<MSEDemuxer>> MSEDemuxer::create()
{
    auto demuxer = DECODER_TRY_ALLOC(adopt_nonnull_ref_or_enomem(new (nothrow) MSEDemuxer()));
    return demuxer;
}

DecoderErrorOr<void> MSEDemuxer::initialize_format_context()
{
    VERIFY(!m_initialized);

    // Allocate custom AVIOContext with our callbacks
    constexpr size_t avio_buffer_size = 4096;
    auto* avio_buffer = static_cast<unsigned char*>(av_malloc(avio_buffer_size));
    if (!avio_buffer)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate AVIO buffer"sv);

    m_avio_context = avio_alloc_context(
        avio_buffer,
        avio_buffer_size,
        0, // write flag (0 = read only)
        this, // opaque pointer passed to callbacks
        avio_read_callback,
        nullptr, // no write callback
        avio_seek_callback);

    if (!m_avio_context) {
        av_free(avio_buffer);
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate AVIO context"sv);
    }

    // Allocate format context
    m_format_context = avformat_alloc_context();
    if (!m_format_context)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate format context"sv);

    m_format_context->pb = m_avio_context;

    // Increase probe size and analyze duration for fragmented MP4
    // This helps FFmpeg properly detect codec parameters from init segment
    m_format_context->probesize = 10000000; // 10MB
    m_format_context->max_analyze_duration = 10000000; // 10 seconds in AV_TIME_BASE units

    // Open input - this will parse the initialization segment
    if (avformat_open_input(&m_format_context, nullptr, nullptr, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to open input for format parsing"sv);

    // Find stream info with extended analysis
    if (avformat_find_stream_info(m_format_context, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to find stream info"sv);

    // For H.264 in fragmented MP4, FFmpeg often can't determine pixel format from init segment alone
    // Manually set it to yuv420p (most common for H.264 Baseline/Main Profile)
    for (unsigned i = 0; i < m_format_context->nb_streams; i++) {
        auto* stream = m_format_context->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (stream->codecpar->format == AV_PIX_FMT_NONE) {
                dbgln("MSE: Video stream {} has unspecified pixel format, setting to yuv420p", i);
                stream->codecpar->format = AV_PIX_FMT_YUV420P;
            }
        }
    }

    m_initialized = true;

    // Extract duration if available
    if (m_format_context->duration > 0) {
        m_duration = time_units_to_duration(m_format_context->duration, 1, AV_TIME_BASE);
    }

    return {};
}

DecoderErrorOr<void> MSEDemuxer::append_initialization_segment(ReadonlyBytes data)
{
    if (m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Initialization segment already appended"sv);

    // Append initialization segment data to buffer
    DECODER_TRY_ALLOC(m_buffer.try_append(data));

    // Store the size of the init segment for later use
    m_init_segment_size = data.size();

    dbgln("MSE: Initialization segment appended ({} bytes). Deferring format context initialization until first media segment.",
          m_init_segment_size);

    // NOTE: We intentionally do NOT call initialize_format_context() here!
    // If we open the format context now, FFmpeg will try to probe for streams
    // and hit EOF since there's no media data yet. This EOF state gets cached.
    // Instead, we'll initialize the format context after the first media segment
    // is appended, ensuring FFmpeg always has frame data available.

    return {};
}

DecoderErrorOr<void> MSEDemuxer::append_media_segment(ReadonlyBytes data)
{
    // On first media segment, initialize the format context
    // (m_initialized gets set to true by initialize_format_context)
    bool is_first_media_segment = !m_initialized;

    auto old_size = m_buffer.size();

    // Append media segment data to growing buffer
    DECODER_TRY_ALLOC(m_buffer.try_append(data));

    dbgln("MSE: append_media_segment() - appended {} bytes (buffer: {} -> {} bytes)",
          data.size(), old_size, m_buffer.size());

    // If this is the first media segment, NOW initialize the format context
    // At this point we have both init segment and media data in the buffer
    if (is_first_media_segment) {
        dbgln("MSE: First media segment received. Initializing format context now that we have frame data.");

        // Reset read position to start so FFmpeg can parse from the beginning
        m_read_position = 0;

        // Initialize format context - FFmpeg will parse init + media segments
        TRY(initialize_format_context());

        dbgln("MSE: Format context initialized successfully. Read position: {}/{}", m_read_position, m_buffer.size());
    }

    // Note: av_read_frame() will read from our growing buffer via custom AVIO callbacks
    // The AVIO context will automatically see the new data when it tries to read

    return {};
}

DecoderErrorOr<void> MSEDemuxer::remove(AK::Duration start, AK::Duration end)
{
    // FIXME: Implement segment removal for MSE
    // This is complex as it requires:
    // 1. Tracking which buffer ranges contain which time ranges
    // 2. Possibly re-creating the buffer without removed segments
    // 3. Updating track contexts
    //
    // For now, we'll just return success as this is primarily used for
    // buffer management optimization rather than core functionality.
    (void)start;
    (void)end;
    return {};
}

MSEDemuxer::TrackContext& MSEDemuxer::get_track_context(Track const& track)
{
    return *m_track_contexts.ensure(track, [&] {
        auto track_context = make<TrackContext>();

        // For MSE, we use the shared format context, not per-track contexts
        // Just allocate a packet for this track
        track_context->format_context = m_format_context;
        track_context->packet = av_packet_alloc();
        VERIFY(track_context->packet != nullptr);

        return track_context;
    });
}

DecoderErrorOr<Track> MSEDemuxer::get_track_for_stream_index(u32 stream_index)
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

DecoderErrorOr<Vector<Track>> MSEDemuxer::get_tracks_for_type(TrackType type)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

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

DecoderErrorOr<Optional<Track>> MSEDemuxer::get_preferred_track_for_type(TrackType type)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    auto media_type = ffmpeg_media_type_from_track_type(type);
    auto best_stream_index = av_find_best_stream(m_format_context, media_type, -1, -1, nullptr, 0);
    if (best_stream_index < 0)
        return OptionalNone();

    return get_track_for_stream_index(best_stream_index);
}

DecoderErrorOr<CodecID> MSEDemuxer::get_codec_id_for_track(Track const& track)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];
    return media_codec_id_from_ffmpeg_codec_id(stream->codecpar->codec_id);
}

DecoderErrorOr<ReadonlyBytes> MSEDemuxer::get_codec_initialization_data_for_track(Track const& track)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];
    return ReadonlyBytes { stream->codecpar->extradata, static_cast<size_t>(stream->codecpar->extradata_size) };
}

DecoderErrorOr<AK::Duration> MSEDemuxer::duration_of_track(Track const& track)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    VERIFY(track.identifier() < m_format_context->nb_streams);
    auto* stream = m_format_context->streams[track.identifier()];

    if (stream->duration >= 0) {
        return time_units_to_duration(stream->duration, stream->time_base);
    }

    // Fallback to container duration
    return total_duration();
}

DecoderErrorOr<AK::Duration> MSEDemuxer::total_duration()
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    // For MSE, duration may not be known upfront or may update as we append segments
    if (m_format_context->duration > 0) {
        return time_units_to_duration(m_format_context->duration, 1, AV_TIME_BASE);
    }

    // Return stored duration from initialization
    return m_duration;
}

DecoderErrorOr<DemuxerSeekResult> MSEDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;

    VERIFY(format_context.nb_streams == m_format_context->nb_streams);
    VERIFY(track.identifier() < format_context.nb_streams);

    dbgln("MSE: seek_to_most_recent_keyframe() called for track {} to timestamp {} (current read pos: {}/{})",
          track.identifier(), timestamp.to_seconds(), m_read_position, m_buffer.size());

    // For MSE with fragmented MP4, seeking to position 0 would read the init segment (moov),
    // which has no frames. Instead, we need to flush FFmpeg's internal state and let it
    // continue reading from the current AVIO position (after init segment, into media segments).

    // Flush the format context to reset internal buffering state
    avformat_flush(&format_context);
    dbgln("MSE: Flushed format context, read position: {}/{}", m_read_position, m_buffer.size());

    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<CodedFrame> MSEDemuxer::get_next_sample_for_track(Track const& track)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;
    auto& packet = *track_context.packet;

    VERIFY(format_context.nb_streams == m_format_context->nb_streams);
    VERIFY(track.identifier() < format_context.nb_streams);
    auto& stream = *format_context.streams[track.identifier()];

    dbgln("MSE: get_next_sample_for_track() called for track {} (buffer: {}/{} bytes)",
          track.identifier(), m_read_position, m_buffer.size());

    // Read frames until we get one for our track
    for (;;) {
        auto read_frame_error = av_read_frame(&format_context, &packet);
        if (read_frame_error < 0) {
            if (read_frame_error == AVERROR_EOF) {
                dbgln("MSE: av_read_frame() returned EOF (buffer: {}/{} bytes)", m_read_position, m_buffer.size());
                return DecoderError::format(DecoderErrorCategory::EndOfStream, "End of stream");
            }

            dbgln("MSE: av_read_frame() failed with error: {}", read_frame_error);
            return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to read frame");
        }
        if (packet.stream_index != stream.index) {
            av_packet_unref(&packet);
            continue;
        }

        // Extract auxiliary data
        auto auxiliary_data = [&]() -> CodedFrame::AuxiliaryData {
            if (track.type() == TrackType::Video) {
                auto color_primaries = static_cast<ColorPrimaries>(stream.codecpar->color_primaries);
                auto transfer_characteristics = static_cast<TransferCharacteristics>(stream.codecpar->color_trc);
                auto matrix_coefficients = static_cast<MatrixCoefficients>(stream.codecpar->color_space);
                auto color_range = [stream] {
                    switch (stream.codecpar->color_range) {
                    case AVCOL_RANGE_JPEG:
                        return VideoFullRangeFlag::Full;
                    case AVCOL_RANGE_MPEG:
                        return VideoFullRangeFlag::Studio;
                    default:
                        return VideoFullRangeFlag::Unspecified;
                    }
                }();

                return CodedVideoFrameData {
                    CodingIndependentCodePoints {
                        color_primaries,
                        transfer_characteristics,
                        matrix_coefficients,
                        color_range,
                    },
                };
            }
            return CodedAudioFrameData {};
        }();

        // Create and return the coded frame
        auto timestamp = time_units_to_duration(packet.pts, stream.time_base);
        auto is_key_frame = (packet.flags & AV_PKT_FLAG_KEY) != 0;
        auto flags = is_key_frame ? FrameFlags::Keyframe : FrameFlags::None;

        auto packet_data = DECODER_TRY_ALLOC(ByteBuffer::copy(packet.data, packet.size));
        av_packet_unref(&packet);

        return CodedFrame(
            timestamp,
            flags,
            move(packet_data),
            auxiliary_data);
    }
}

}
