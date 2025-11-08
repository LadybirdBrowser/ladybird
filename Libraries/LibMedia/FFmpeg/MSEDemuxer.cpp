/*
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>
#include <LibMedia/FFmpeg/FFmpegIOContext.h>
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

    dbgln("MSE: avio_read_callback() called - buf_size={}, read_pos={}, buffer_size={}",
          buf_size, demuxer->m_read_position, demuxer->m_buffer.size());

    // Calculate how much data is available from current read position
    size_t available = demuxer->m_buffer.size() - demuxer->m_read_position;
    if (available == 0) {
        // Track that we returned EOF so we know to reset FFmpeg state when data arrives
        demuxer->m_returned_eof_from_read = true;
        dbgln("MSE: avio_read_callback() returning EOF (no data available)");
        return AVERROR_EOF;
    }

    // If we previously returned EOF but now have data, reset FFmpeg's EOF state
    if (demuxer->m_returned_eof_from_read && demuxer->m_avio_context) {
        demuxer->m_avio_context->eof_reached = 0;
        demuxer->m_avio_context->error = 0;
        demuxer->m_returned_eof_from_read = false;
        dbgln("MSE: Reset EOF flags in read callback - new data available ({} bytes at position {})",
              available, demuxer->m_read_position);
    }

    // Read as much as requested or available
    size_t to_read = min(static_cast<size_t>(buf_size), available);
    memcpy(buf, demuxer->m_buffer.data() + demuxer->m_read_position, to_read);
    demuxer->m_read_position += to_read;
    dbgln("MSE: avio_read_callback() read {} bytes, new read_pos={}", to_read, demuxer->m_read_position);

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

    m_stream_first_timestamps.clear();
    m_stream_first_timestamps.resize(m_format_context->nb_streams);
    m_stream_last_timestamps.clear();
    m_stream_last_timestamps.resize(m_format_context->nb_streams);
    for (auto& entry : m_stream_last_timestamps)
        entry = AK::Duration::zero();
    m_pending_samples.clear();
    m_pending_samples.resize(m_format_context->nb_streams);
    m_buffered_start = m_timestamp_offset;
    m_buffered_end = m_timestamp_offset;

    // Extract duration if available
    if (m_format_context->duration > 0) {
        m_duration = time_units_to_duration(m_format_context->duration, 1, AV_TIME_BASE);
    }

    for (unsigned i = 0; i < m_format_context->nb_streams; ++i) {
        auto* stream = m_format_context->streams[i];
        dbgln("MSE: Stream {} codec_type={} codec_id={} time_base={}/{}",
            i,
            static_cast<int>(stream->codecpar->codec_type),
            static_cast<int>(stream->codecpar->codec_id),
            stream->time_base.num,
            stream->time_base.den);
    }

    if (m_initialized)
        TRY(recalculate_buffered_range());

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
    // On first media segment, initialize the format context with just the init segment
    // (m_initialized gets set to true by initialize_format_context)
    bool is_first_media_segment = !m_initialized;

    if (is_first_media_segment) {
        dbgln("MSE: First media segment received. Initializing format context with init segment.");

        // Reset read position to start so FFmpeg can parse the init segment
        m_read_position = 0;

        // Initialize format context - FFmpeg will parse the init segment
        TRY(initialize_format_context());

        dbgln("MSE: Format context initialized successfully with init segment. Buffer: {}/{}",
              m_read_position, m_buffer.size());
    }

    // Now parse the actual media segment to extract frames
    // Create a temporary buffer with init segment + this media segment
    ByteBuffer segment_buffer;
    DECODER_TRY_ALLOC(segment_buffer.try_append(m_buffer.span().slice(0, m_init_segment_size)));
    DECODER_TRY_ALLOC(segment_buffer.try_append(data));

    dbgln("MSE: Parsing media segment ({} bytes, total with init: {} bytes)",
          data.size(), segment_buffer.size());

    // Create a temporary format context to parse this segment
    auto segment_stream = DECODER_TRY_ALLOC(try_make<FixedMemoryStream>(segment_buffer.bytes()));
    auto segment_io = DECODER_TRY_ALLOC(Media::FFmpeg::FFmpegIOContext::create(*segment_stream));

    AVFormatContext* segment_format = avformat_alloc_context();
    if (!segment_format)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate format context for segment parsing"sv);

    ScopeGuard format_guard([&] {
        if (segment_format)
            avformat_close_input(&segment_format);
    });

    segment_format->pb = segment_io->avio_context();

    if (avformat_open_input(&segment_format, nullptr, nullptr, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to open segment for parsing"sv);

    if (avformat_find_stream_info(segment_format, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to find stream info in segment"sv);

    // Read all packets from this segment and store them
    AVPacket* packet = av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate packet for segment parsing"sv);

    ScopeGuard packet_guard([&] {
        av_packet_free(&packet);
    });

    u32 packet_count = 0;
    while (av_read_frame(segment_format, packet) >= 0) {
        auto stream_index = packet->stream_index;
        if (stream_index >= 0 && static_cast<u32>(stream_index) < segment_format->nb_streams) {
            TRY(store_packet_for_stream(*packet, static_cast<u32>(stream_index)));
            packet_count++;
        }
        av_packet_unref(packet);
    }

    // Also append to the main buffer (for potential future use)
    DECODER_TRY_ALLOC(m_buffer.try_append(data));

    // Calculate buffered range from pending samples
    // Find the earliest and latest NORMALIZED timestamps across all streams
    Optional<AK::Duration> min_time;
    Optional<AK::Duration> max_time;

    // First pass: establish first timestamps for normalization if not already set
    for (u32 stream_index = 0; stream_index < m_pending_samples.size(); ++stream_index) {
        auto& queue = m_pending_samples[stream_index];
        if (queue.is_empty())
            continue;

        if (stream_index >= m_format_context->nb_streams)
            continue;
        auto* stream = m_format_context->streams[stream_index];

        ensure_stream_capacity(stream_index);

        // Find the earliest timestamp in this stream to use as the base
        for (auto& sample : queue) {
            auto pts = sample.pts != AV_NOPTS_VALUE ? sample.pts : sample.dts;
            if (pts == AV_NOPTS_VALUE)
                continue;

            auto timestamp = time_units_to_duration(pts, stream->time_base);

            // Set first timestamp if not already set
            auto& first_timestamp = m_stream_first_timestamps[stream_index];
            if (!first_timestamp.has_value() || timestamp < first_timestamp.value()) {
                first_timestamp = timestamp;
            }
            break; // Only need the first one
        }
    }

    // Second pass: calculate normalized range
    for (u32 stream_index = 0; stream_index < m_pending_samples.size(); ++stream_index) {
        auto& queue = m_pending_samples[stream_index];
        if (queue.is_empty())
            continue;

        if (stream_index >= m_format_context->nb_streams)
            continue;
        auto* stream = m_format_context->streams[stream_index];

        auto& first_timestamp = m_stream_first_timestamps[stream_index];
        if (!first_timestamp.has_value())
            continue;

        // Find earliest and latest normalized timestamps in this stream's queue
        for (auto& sample : queue) {
            auto pts = sample.pts != AV_NOPTS_VALUE ? sample.pts : sample.dts;
            if (pts == AV_NOPTS_VALUE)
                continue;

            auto timestamp = time_units_to_duration(pts, stream->time_base);
            auto normalized = timestamp - first_timestamp.value();
            if (normalized < AK::Duration::zero())
                normalized = AK::Duration::zero();
            normalized += m_timestamp_offset;

            auto end_time = normalized;
            if (sample.duration > 0) {
                auto duration = time_units_to_duration(sample.duration, stream->time_base);
                end_time = normalized + duration;
            }

            if (!min_time.has_value() || normalized < min_time.value())
                min_time = normalized;
            if (!max_time.has_value() || end_time > max_time.value())
                max_time = end_time;
        }
    }

    // Set buffered range from actual timestamps, or keep previous range if no samples
    if (min_time.has_value() && max_time.has_value()) {
        m_buffered_start = min_time.value();
        m_buffered_end = max_time.value();
    }

    dbgln("MSE: Parsed media segment - extracted {} packets, buffer now {} bytes, buffered range: {}-{}s",
          packet_count, m_buffer.size(),
          m_buffered_start.to_seconds(), m_buffered_end.to_seconds());

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

void MSEDemuxer::set_timestamp_offset(AK::Duration offset)
{
    if (m_timestamp_offset == offset)
        return;
    m_timestamp_offset = offset;
    if (!m_initialized)
        return;
    if (auto result = recalculate_buffered_range(); result.is_error()) {
        dbgln("MSE: Failed to recalculate buffered range after timestamp offset change: {}", result.error().string_literal());
    }
}

void MSEDemuxer::ensure_stream_capacity(u32 stream_index)
{
    if (m_stream_first_timestamps.size() <= stream_index) {
        m_stream_first_timestamps.resize(stream_index + 1);
        auto previous_last_size = m_stream_last_timestamps.size();
        m_stream_last_timestamps.resize(stream_index + 1);
        for (size_t i = previous_last_size; i < m_stream_last_timestamps.size(); ++i)
            m_stream_last_timestamps[i] = AK::Duration::zero();
    }

    if (m_pending_samples.size() <= stream_index)
        m_pending_samples.resize(stream_index + 1);
}

void MSEDemuxer::clear_pending_samples()
{
    for (auto& queue : m_pending_samples)
        queue.clear();
}

DecoderErrorOr<void> MSEDemuxer::store_packet_for_stream(AVPacket& packet, u32 stream_index)
{
    ensure_stream_capacity(stream_index);

    dbgln("MSE: Storing packet for stream {} (pts={}, dts={}, size={})",
        stream_index,
        packet.pts,
        packet.dts,
        packet.size);

    PendingSample sample;
    sample.pts = packet.pts;
    sample.dts = packet.dts;
    sample.duration = packet.duration;
    sample.flags = packet.flags;
    sample.data = DECODER_TRY_ALLOC(ByteBuffer::copy(packet.data, packet.size));

    auto append_result = m_pending_samples[stream_index].try_append(move(sample));
    if (append_result.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to store pending packet"sv);
    av_packet_unref(&packet);
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

AK::Duration MSEDemuxer::normalize_timestamp(u32 stream_index, AK::Duration timestamp)
{
    ensure_stream_capacity(stream_index);

    auto& first_timestamp = m_stream_first_timestamps[stream_index];
    if (!first_timestamp.has_value() || timestamp < first_timestamp.value())
        first_timestamp = timestamp;

    auto normalized = timestamp - first_timestamp.value();
    if (normalized < AK::Duration::zero())
        normalized = AK::Duration::zero();

    normalized += m_timestamp_offset;
    return normalized;
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

DecoderErrorOr<void> MSEDemuxer::recalculate_buffered_range()
{
    if (!m_initialized)
        return {};
    if (m_buffer.is_empty())
        return {};

    auto data_view = ReadonlyBytes { m_buffer.data(), m_buffer.size() };
    auto stream = DECODER_TRY_ALLOC(try_make<FixedMemoryStream>(data_view));
    auto io_context = DECODER_TRY_ALLOC(Media::FFmpeg::FFmpegIOContext::create(*stream));

    AVFormatContext* format_context = avformat_alloc_context();
    if (!format_context)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate format context for buffered range calculation"sv);

    ScopeGuard format_context_guard([&] {
        if (format_context != nullptr)
            avformat_close_input(&format_context);
    });

    format_context->pb = io_context->avio_context();

    if (avformat_open_input(&format_context, nullptr, nullptr, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to open input while calculating buffered range"sv);

    if (avformat_find_stream_info(format_context, nullptr) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to find stream info while calculating buffered range"sv);

    Vector<Optional<AK::Duration>> first_timestamps;
    first_timestamps.resize(format_context->nb_streams);
    Vector<AK::Duration> last_timestamps;
    last_timestamps.resize(format_context->nb_streams);
    for (auto& entry : last_timestamps)
        entry = AK::Duration::zero();

    auto* packet = av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate packet for buffered range calculation"sv);

    ScopeGuard packet_guard([&] {
        av_packet_free(&packet);
    });

    while (av_read_frame(format_context, packet) >= 0) {
        auto* stream_info = format_context->streams[packet->stream_index];
        auto codec_type = stream_info->codecpar->codec_type;
        if (codec_type != AVMEDIA_TYPE_VIDEO && codec_type != AVMEDIA_TYPE_AUDIO) {
            av_packet_unref(packet);
            continue;
        }

        auto pts_value = packet->pts;
        if (pts_value == AV_NOPTS_VALUE)
            pts_value = packet->dts;
        if (pts_value == AV_NOPTS_VALUE) {
            av_packet_unref(packet);
            continue;
        }

        auto absolute_timestamp = time_units_to_duration(pts_value, stream_info->time_base);
        auto& first_timestamp = first_timestamps[packet->stream_index];
        if (!first_timestamp.has_value())
            first_timestamp = absolute_timestamp;

        auto normalized_timestamp = absolute_timestamp - first_timestamp.value();
        if (normalized_timestamp < AK::Duration::zero())
            normalized_timestamp = AK::Duration::zero();

        auto packet_duration = AK::Duration::zero();
        if (packet->duration > 0)
            packet_duration = time_units_to_duration(packet->duration, stream_info->time_base);

        auto end_timestamp = normalized_timestamp + packet_duration + m_timestamp_offset;
        last_timestamps[packet->stream_index] = max(last_timestamps[packet->stream_index], end_timestamp);

        av_packet_unref(packet);
    }

    if (m_stream_first_timestamps.size() < format_context->nb_streams) {
        m_stream_first_timestamps.resize(format_context->nb_streams);
        auto previous_last_size = m_stream_last_timestamps.size();
        m_stream_last_timestamps.resize(format_context->nb_streams);
        for (size_t i = previous_last_size; i < m_stream_last_timestamps.size(); ++i)
            m_stream_last_timestamps[i] = AK::Duration::zero();
        m_pending_samples.resize(format_context->nb_streams);
    }

    AK::Duration global_end = m_timestamp_offset;
    for (size_t i = 0; i < format_context->nb_streams; ++i) {
        if (!first_timestamps[i].has_value())
            continue;

        if (!m_stream_first_timestamps[i].has_value())
            m_stream_first_timestamps[i] = first_timestamps[i];

        m_stream_last_timestamps[i] = last_timestamps[i];
        global_end = max(global_end, last_timestamps[i]);
    }

    m_buffered_start = m_timestamp_offset;
    m_buffered_end = global_end;
    return {};
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

    clear_pending_samples();

    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<CodedFrame> MSEDemuxer::get_next_sample_for_track(Track const& track)
{
    if (!m_initialized)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Not initialized"sv);

    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;

    VERIFY(format_context.nb_streams == m_format_context->nb_streams);
    VERIFY(track.identifier() < format_context.nb_streams);
    auto& stream = *format_context.streams[track.identifier()];

    ensure_stream_capacity(stream.index);

    dbgln("MSE: get_next_sample_for_track() called for track {} (buffer: {}/{} bytes)",
          track.identifier(), m_read_position, m_buffer.size());

    auto auxiliary_data_for_stream = [&]() -> CodedFrame::AuxiliaryData {
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
    };

    auto produce_frame = [&](PendingSample& sample, bool from_pending_queue) -> DecoderErrorOr<CodedFrame> {
        auto pts_value = sample.pts != AV_NOPTS_VALUE ? sample.pts : sample.dts;
        if (pts_value == AV_NOPTS_VALUE)
            pts_value = 0;

        auto absolute_timestamp = time_units_to_duration(pts_value, stream.time_base);
        auto normalized_timestamp = normalize_timestamp(stream.index, absolute_timestamp);
        if (normalized_timestamp < AK::Duration::zero())
            normalized_timestamp = AK::Duration::zero();

        auto sample_duration = AK::Duration::zero();
        if (sample.duration > 0)
            sample_duration = time_units_to_duration(sample.duration, stream.time_base);

        m_stream_last_timestamps[stream.index] = max(m_stream_last_timestamps[stream.index], normalized_timestamp + sample_duration);
        m_buffered_end = max(m_buffered_end, m_stream_last_timestamps[stream.index]);

        dbgln("MSE: Returning packet for stream {} -> normalized_ts={}ms duration={}ms flags={} (from_pending={})",
            stream.index,
            normalized_timestamp.to_milliseconds(),
            sample_duration.to_milliseconds(),
            sample.flags,
            from_pending_queue);

        auto flags = (sample.flags & AV_PKT_FLAG_KEY) != 0 ? FrameFlags::Keyframe : FrameFlags::None;

        auto packet_data = move(sample.data);
        return CodedFrame(
            normalized_timestamp,
            flags,
            move(packet_data),
            auxiliary_data_for_stream());
    };

    // All packets are now pre-parsed and stored in m_pending_samples during append_media_segment()
    // So we just need to check if we have a packet available for this stream
    if (static_cast<size_t>(stream.index) < m_pending_samples.size() && !m_pending_samples[stream.index].is_empty()) {
        auto sample = m_pending_samples[stream.index].take_first();
        return produce_frame(sample, true);
    }

    // No more packets available for this stream
    dbgln("MSE: No more packets available for stream {} (End of buffered data)", stream.index);
    return DecoderError::format(DecoderErrorCategory::EndOfStream, "End of buffered data"sv);
}

}
