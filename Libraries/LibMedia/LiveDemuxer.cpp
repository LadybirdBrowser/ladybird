/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibMedia/LiveDemuxer.h>

namespace Media {

static constexpr AK::Duration AUDIO_BLOCK_DURATION = AK::Duration::from_milliseconds(10);
static constexpr AK::Duration DEFAULT_VIDEO_FRAME_DURATION = AK::Duration::from_milliseconds(33);
static constexpr AK::Duration MAXIMUM_VIDEO_FRAME_DURATION = AK::Duration::from_milliseconds(200);

NonnullRefPtr<LiveDemuxer> LiveDemuxer::create_for_audio(u64 track_identifier, Audio::SampleSpecification sample_specification, Utf16String const& label)
{
    VERIFY(sample_specification.is_valid());
    Track track { TrackType::Audio, track_identifier, Track::Kind::Main, label, {} };
    track.set_audio_data({ .sample_specification = sample_specification });
    return adopt_ref(*new LiveDemuxer(move(track)));
}

NonnullRefPtr<LiveDemuxer> LiveDemuxer::create_for_video(u64 track_identifier, Utf16String const& label)
{
    Track track { TrackType::Video, track_identifier, Track::Kind::Main, label, {} };
    return adopt_ref(*new LiveDemuxer(move(track)));
}

LiveDemuxer::LiveDemuxer(Track track)
    : m_track(move(track))
    , m_event_loop(Core::EventLoop::current_weak())
{
    // A live track's duration is unknown until it ends.
    m_scan_state.duration = AK::Duration::max();
    m_scan_state.track_buffered_ranges.append({ .track_identifier = m_track.identifier(), .time_ranges = {}, .reached_end_of_stream = false });
}

LiveDemuxer::~LiveDemuxer() = default;

void LiveDemuxer::set_time_reader(MediaTimeReader time_reader)
{
    MutexLocker locker { m_mutex };
    m_time_reader = move(time_reader);
    m_data_changed.broadcast();
}

AK::Duration LiveDemuxer::current_time_while_locked() const
{
    if (!m_time_reader.has_value())
        return AK::Duration::zero();
    return m_time_reader->current_time();
}

i64 LiveDemuxer::frame_index_at(AK::Duration time) const
{
    return time.to_time_units(1, m_track.audio_data().sample_specification.sample_rate());
}

AK::Duration LiveDemuxer::time_at_frame_index(i64 frame_index) const
{
    return AK::Duration::from_time_units(frame_index, 1, m_track.audio_data().sample_specification.sample_rate());
}

size_t LiveDemuxer::pending_audio_frame_count_while_locked() const
{
    return (m_pending_audio.size() - m_pending_audio_offset) / m_track.audio_data().sample_specification.channel_count();
}

AK::Duration LiveDemuxer::audio_end_time_while_locked() const
{
    auto next_frame_index = max(m_next_audio_frame_index, frame_index_at(current_time_while_locked()));
    return time_at_frame_index(next_frame_index + static_cast<i64>(pending_audio_frame_count_while_locked()));
}

void LiveDemuxer::push_audio(ReadonlyBytes interleaved_samples)
{
    VERIFY(m_track.type() == TrackType::Audio);
    auto channel_count = m_track.audio_data().sample_specification.channel_count();
    auto frame_count = interleaved_samples.size() / (sizeof(float) * channel_count);
    if (frame_count == 0)
        return;

    MutexLocker locker { m_mutex };
    if (m_ended)
        return;

    if (audio_end_time_while_locked() - current_time_while_locked() > AUDIO_MAXIMUM_LATENCY)
        return;

    // Compact consumed audio to keep alternating reads and writes from growing the buffer without bound.
    if (m_pending_audio_offset > 0 && m_pending_audio_offset >= m_pending_audio.size() - m_pending_audio_offset) {
        m_pending_audio.remove(0, m_pending_audio_offset);
        m_pending_audio_offset = 0;
    }
    auto sample_count = frame_count * channel_count;
    auto old_size = m_pending_audio.size();
    m_pending_audio.resize(old_size + sample_count);
    __builtin_memcpy(m_pending_audio.data() + old_size, interleaved_samples.data(), sample_count * sizeof(float));
    m_data_changed.broadcast();
}

AK::Duration LiveDemuxer::audio_end_time() const
{
    VERIFY(m_track.type() == TrackType::Audio);
    MutexLocker locker { m_mutex };
    return audio_end_time_while_locked();
}

void LiveDemuxer::request_key_frame_while_locked()
{
    if (m_key_frame_request_is_pending)
        return;
    auto event_loop = m_event_loop->take();
    if (!event_loop)
        return;
    m_key_frame_request_is_pending = true;
    event_loop->deferred_invoke([self = NonnullRefPtr { *this }] {
        {
            MutexLocker locker { self->m_mutex };
            self->m_key_frame_request_is_pending = false;
            if (!self->m_waiting_for_key_frame)
                return;
        }
        if (self->on_key_frame_needed)
            self->on_key_frame_needed();
    });
}

void LiveDemuxer::push_video_frame(CodecID codec_id, FrameFlags flags, AK::Duration source_timestamp, FixedArray<u8> data, AK::Duration live_edge, VideoPacing pacing)
{
    VERIFY(m_track.type() == TrackType::Video);
    VERIFY(track_type_from_codec_id(codec_id) == TrackType::Video);
    MutexLocker locker { m_mutex };
    if (m_ended)
        return;

    auto is_key_frame = has_flag(flags, FrameFlags::Keyframe);
    if (m_waiting_for_key_frame) {
        if (!is_key_frame) {
            request_key_frame_while_locked();
            return;
        }
        m_waiting_for_key_frame = false;
    }

    if (m_video_frames.size() >= MAXIMUM_QUEUED_VIDEO_FRAMES) {
        // Nothing is reading, so only the frames from here on are worth keeping.
        if (!is_key_frame) {
            m_video_frames.clear();
            m_waiting_for_key_frame = true;
            request_key_frame_while_locked();
            return;
        }
        m_video_frames.clear();
    }

    auto duration = DEFAULT_VIDEO_FRAME_DURATION;
    if (m_last_video_source_timestamp.has_value() && source_timestamp > *m_last_video_source_timestamp)
        duration = min(source_timestamp - *m_last_video_source_timestamp, MAXIMUM_VIDEO_FRAME_DURATION);

    // Use capture timing to smooth arrival jitter. Reanchor frames that would play late or too far ahead.
    auto timestamp = live_edge;
    if (pacing == VideoPacing::FromSource && m_last_video_source_timestamp.has_value()) {
        auto paced_timestamp = m_video_anchor_timestamp + (source_timestamp - m_video_anchor_source_timestamp);
        if (paced_timestamp >= live_edge && paced_timestamp - live_edge <= VIDEO_MAXIMUM_SMOOTHING_DELAY)
            timestamp = paced_timestamp;
    }
    if (timestamp == live_edge) {
        m_video_anchor_timestamp = live_edge;
        m_video_anchor_source_timestamp = source_timestamp;
    }
    m_last_video_source_timestamp = source_timestamp;

    // Frames are decoded in the order that they arrive, so their timestamps have to say the same.
    if (m_last_video_timestamp.has_value() && timestamp <= *m_last_video_timestamp)
        timestamp = *m_last_video_timestamp + AK::Duration::from_microseconds(1);
    m_last_video_timestamp = timestamp;

    m_video_frames.empend(codec_id, timestamp, timestamp, duration, flags, move(data));
    m_data_changed.broadcast();
}

void LiveDemuxer::end()
{
    {
        MutexLocker locker { m_mutex };
        if (m_ended)
            return;
        m_ended = true;
        m_data_changed.broadcast();
    }

    m_scan_state.reached_end_of_stream = true;
    m_scan_state.track_buffered_ranges[0].reached_end_of_stream = true;
    if (m_scan_state_change_handler)
        m_scan_state_change_handler(TimeRanges {});
}

DecoderErrorOr<void> LiveDemuxer::create_context_for_track(Track const& track)
{
    VERIFY(track == m_track);
    return {};
}

DecoderErrorOr<Vector<Track>> LiveDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    if (type == m_track.type())
        tracks.append(m_track);
    return tracks;
}

DecoderErrorOr<Optional<Track>> LiveDemuxer::get_preferred_track_for_type(TrackType type)
{
    if (type != m_track.type())
        return Optional<Track> {};
    return Optional<Track> { m_track };
}

DecoderErrorOr<CodedFrame> LiveDemuxer::get_next_sample_for_track(Track const& track)
{
    VERIFY(track == m_track);
    if (m_track.type() == TrackType::Audio)
        return get_next_audio_block();
    return get_next_video_frame();
}

CodedFrame LiveDemuxer::take_audio_block_while_locked(size_t frame_count, bool silent)
{
    auto channel_count = m_track.audio_data().sample_specification.channel_count();
    auto sample_count = frame_count * channel_count;
    auto data = MUST(FixedArray<u8>::create(sample_count * sizeof(float)));
    if (silent) {
        data.fill_with(0);
    } else {
        __builtin_memcpy(data.data(), m_pending_audio.data() + m_pending_audio_offset, data.size());
        m_pending_audio_offset += sample_count;
    }

    // PCM needs no configuration data, but the first frame must still start a decode sequence.
    Optional<FixedArray<u8>> codec_configuration;
    if (m_needs_codec_configuration) {
        m_needs_codec_configuration = false;
        codec_configuration = FixedArray<u8> {};
    }

    auto timestamp = time_at_frame_index(m_next_audio_frame_index);
    m_next_audio_frame_index += static_cast<i64>(frame_count);
    auto duration = time_at_frame_index(m_next_audio_frame_index) - timestamp;
    return CodedFrame(CodecID::F32LE, timestamp, timestamp, duration, FrameFlags::Keyframe, move(data), move(codec_configuration));
}

DecoderErrorOr<CodedFrame> LiveDemuxer::get_next_audio_block()
{
    auto block_frame_count = static_cast<size_t>(max<i64>(1, frame_index_at(AUDIO_BLOCK_DURATION)));

    MutexLocker locker { m_mutex };
    auto clock_is_stalled = false;
    while (true) {
        if (m_aborted.load())
            return DecoderError::with_description(DecoderErrorCategory::Aborted, "Read aborted"sv);

        auto now = current_time_while_locked();

        m_next_audio_frame_index = max(m_next_audio_frame_index, frame_index_at(now));

        auto pending_frame_count = pending_audio_frame_count_while_locked();
        if (pending_frame_count > 0)
            return take_audio_block_while_locked(min(pending_frame_count, block_frame_count), false);

        if (m_ended)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);

        // Silence keeps the output and other tracks playing, and starts a clock still waiting for output.
        if (time_at_frame_index(m_next_audio_frame_index) - now < AUDIO_TARGET_LATENCY || clock_is_stalled)
            return take_audio_block_while_locked(block_frame_count, true);

        m_data_changed.wait_for(AUDIO_BLOCK_DURATION);
        clock_is_stalled = current_time_while_locked() == now;
    }
}

DecoderErrorOr<CodedFrame> LiveDemuxer::get_next_video_frame()
{
    MutexLocker locker { m_mutex };
    while (true) {
        if (m_aborted.load())
            return DecoderError::with_description(DecoderErrorCategory::Aborted, "Read aborted"sv);
        if (!m_video_frames.is_empty()) {
            auto frame = m_video_frames.take_first();
            if (m_needs_codec_configuration) {
                m_needs_codec_configuration = false;
                frame.set_new_codec_configuration({});
            }
            m_reader_depends_on_earlier_frames = true;
            return frame;
        }
        if (m_ended)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);
        m_data_changed.wait();
    }
}

AK::Duration LiveDemuxer::select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode)
{
    return target;
}

DecoderErrorOr<DemuxerSeekResult> LiveDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions options)
{
    VERIFY(track == m_track);
    MutexLocker locker { m_mutex };

    if (has_flag(options, DemuxerSeekOptions::NeedCodecConfiguration))
        m_needs_codec_configuration = true;

    if (m_track.type() == TrackType::Audio) {
        // What has arrived plays from wherever the reader wants to continue.
        m_next_audio_frame_index = frame_index_at(timestamp);
        return DemuxerSeekResult::MovedPosition;
    }

    if (!has_flag(options, DemuxerSeekOptions::Force))
        return DemuxerSeekResult::KeptCurrentPosition;

    // The reader starts decoding anew, which it can only do from a key frame.
    Optional<size_t> last_key_frame_index;
    for (size_t i = m_video_frames.size(); i > 0; i--) {
        if (m_video_frames[i - 1].is_keyframe()) {
            last_key_frame_index = i - 1;
            break;
        }
    }
    if (last_key_frame_index.has_value()) {
        m_video_frames.remove(0, *last_key_frame_index);
    } else if (m_reader_depends_on_earlier_frames || !m_video_frames.is_empty()) {
        m_video_frames.clear();
        m_waiting_for_key_frame = true;
        request_key_frame_while_locked();
    }
    m_reader_depends_on_earlier_frames = false;
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<AK::Duration> LiveDemuxer::duration_of_track(Track const&)
{
    return total_duration();
}

DecoderErrorOr<AK::Duration> LiveDemuxer::total_duration()
{
    return AK::Duration::max();
}

DemuxerScanState const& LiveDemuxer::scan_state() const
{
    return m_scan_state;
}

void LiveDemuxer::set_scan_state_change_handler(ScanStateChangeHandler handler)
{
    m_scan_state_change_handler = move(handler);
}

void LiveDemuxer::set_blocking_reads_aborted_for_track(Track const&)
{
    MutexLocker locker { m_mutex };
    m_aborted.store(true);
    m_data_changed.broadcast();
}

void LiveDemuxer::reset_blocking_reads_aborted_for_track(Track const&)
{
    m_aborted.store(false);
}

void LiveDemuxer::set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler)
{
    // Waiting for live data to be produced must not block playback of other tracks.
}

}
