/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>

namespace Web::MediaSourceExtensions {

static AK::Duration decode_timestamp(Media::CodedFrame const& frame)
{
    return frame.auxiliary_data().visit(
        [&](Media::CodedVideoFrameData const& video_data) { return video_data.decode_timestamp().value_or(frame.timestamp()); },
        [&](Media::CodedAudioFrameData const&) { return frame.timestamp(); });
}

enum class KeyframeSide {
    AtOrBefore,
    AtOrAfter,
};

struct SelectedKeyframe {
    size_t position { 0 };
    AK::Duration timestamp;
};

static Optional<SelectedKeyframe> find_nearest_keyframe(Vector<Media::CodedFrame> const& frames, AK::Duration target, KeyframeSide side)
{
    Optional<SelectedKeyframe> selected_keyframe;
    for (size_t i = 0; i < frames.size(); i++) {
        auto const& frame = frames[i];
        if (!frame.is_keyframe())
            continue;

        auto timestamp = frame.timestamp();
        if (side == KeyframeSide::AtOrBefore && timestamp > target)
            continue;
        if (side == KeyframeSide::AtOrAfter && timestamp < target)
            continue;

        if (selected_keyframe.has_value()) {
            if (side == KeyframeSide::AtOrBefore && timestamp <= selected_keyframe->timestamp)
                continue;
            if (side == KeyframeSide::AtOrAfter && timestamp >= selected_keyframe->timestamp)
                continue;
        }

        selected_keyframe = SelectedKeyframe { i, timestamp };
    }
    return selected_keyframe;
}

TrackBufferDemuxer::TrackBufferDemuxer(Media::Track const& track, Media::CodecID codec_id, ByteBuffer codec_initialization_data)
    : m_track(track)
    , m_codec_id(codec_id)
    , m_codec_initialization_data(move(codec_initialization_data))
{
}

TrackBufferDemuxer::~TrackBufferDemuxer() = default;

Media::TimeRanges TrackBufferDemuxer::track_buffer_ranges() const
{
    Sync::MutexLocker locker { m_mutex };
    // https://w3c.github.io/media-source/#track-buffer-ranges
    // NOTE: Implementations MAY coalesce adjacent ranges separated by a gap smaller than 2 times the
    //       maximum frame duration buffered so far in this track buffer.
    auto max_gap = maximum_time_range_gap();
    if (max_gap.is_zero())
        return m_track_buffer_ranges;
    return m_track_buffer_ranges.coalesced(max_gap);
}

void TrackBufferDemuxer::add_coded_frame(Media::CodedFrame frame)
{
    Sync::MutexLocker locker { m_mutex };
    auto start = frame.timestamp();
    auto end = frame.timestamp() + frame.duration();
    m_maximum_frame_duration = max(m_maximum_frame_duration, frame.duration());
    m_track_buffer_ranges.add_range(start, end);

    // Coded frames are decoded in decode timestamp order. In codecs with frame reordering, such as H.264,
    // this can differ from presentation timestamp order.
    // Overlapping frames should have been removed by remove_coded_frames_and_dependants_in_range()
    // before this call.
    auto timestamp = decode_timestamp(frame);
    size_t insert_index = 0;
    auto* existing_coded_frame = binary_search(m_coded_frames, timestamp, &insert_index, [](AK::Duration needle, Media::CodedFrame const& frame) {
        return needle <=> decode_timestamp(frame);
    });
    if (existing_coded_frame) {
        insert_index = existing_coded_frame - m_coded_frames.data();
        while (insert_index < m_coded_frames.size() && decode_timestamp(m_coded_frames[insert_index]) == timestamp)
            insert_index++;
    } else if (insert_index < m_coded_frames.size() && decode_timestamp(m_coded_frames[insert_index]) < timestamp) {
        insert_index++;
    }

    m_total_bytes += frame.data().size();
    m_coded_frames.insert(insert_index, move(frame));

    if (insert_index <= m_read_position && (!m_last_returned_decode_timestamp.has_value() || m_last_returned_decode_timestamp.value() > timestamp))
        m_read_position++;

    m_data_changed.broadcast();
    queue_scan_state_change_dispatch_while_locked();
}

void TrackBufferDemuxer::remove_coded_frames_and_dependants_in_range(AK::Duration start, AK::Duration end)
{
    Sync::MutexLocker locker { m_mutex };

    // https://w3c.github.io/media-source/#sourcebuffer-coded-frame-processing
    // 1.13. Remove all coded frames from track buffer that have a presentation timestamp greater than
    //       or equal to presentation timestamp and less than frame end timestamp.
    // 1.14. Remove all possible decoding dependencies on the coded frames removed in the previous step
    //       by removing all coded frames from track buffer between those frames removed in the previous
    //       step and the next random access point after those removed frames.

    Optional<size_t> first_frame_in_range;
    size_t remove_end = 0;
    for (size_t i = 0; i < m_coded_frames.size(); i++) {
        auto presentation_timestamp = m_coded_frames[i].timestamp();
        if (presentation_timestamp >= start && presentation_timestamp < end) {
            if (!first_frame_in_range.has_value())
                first_frame_in_range = i;
            remove_end = i + 1;
        }
    }
    if (!first_frame_in_range.has_value())
        return;
    auto remove_start = first_frame_in_range.value();

    // Remove all matching frames and their decode dependencies up to the next random access point.
    while (remove_end < m_coded_frames.size() && !m_coded_frames[remove_end].is_keyframe())
        remove_end++;

    for (size_t i = remove_start; i < remove_end; i++) {
        auto const& removed_frame = m_coded_frames[i];
        auto removed_start = removed_frame.timestamp();
        auto removed_end = (removed_frame.timestamp() + removed_frame.duration());
        m_track_buffer_ranges.remove_range(removed_start, removed_end);
        m_total_bytes -= removed_frame.data().size();
    }

    m_coded_frames.remove(remove_start, remove_end - remove_start);

    // Adjust read position if it was in or past the removed range.
    if (m_read_position >= remove_end) {
        m_read_position -= (remove_end - remove_start);
    } else if (m_read_position > remove_start) {
        m_read_position = remove_start > 0 ? remove_start - 1 : 0;

        while (m_read_position > 0 && !m_coded_frames[m_read_position].is_keyframe())
            m_read_position--;

        m_last_returned_presentation_timestamp.clear();
        m_last_returned_decode_timestamp.clear();
    }

    queue_scan_state_change_dispatch_while_locked();
}

size_t TrackBufferDemuxer::total_bytes() const
{
    Sync::MutexLocker locker { m_mutex };
    return m_total_bytes;
}

bool TrackBufferDemuxer::is_frame_evictable_while_locked(Media::CodedFrame const& frame, AK::Duration current_time) const
{
    auto time_range_start = current_time;
    auto time_range_end = current_time;
    if (m_last_returned_presentation_timestamp.has_value()) {
        time_range_start = min(time_range_start, m_last_returned_presentation_timestamp.value());
        time_range_end = max(time_range_end, m_last_returned_presentation_timestamp.value());
    }
    return frame.timestamp() < time_range_start || frame.timestamp() > time_range_end;
}

Optional<AK::Duration> TrackBufferDemuxer::earliest_evictable_frame_timestamp(AK::Duration current_time) const
{
    Sync::MutexLocker locker { m_mutex };
    if (m_coded_frames.is_empty())
        return {};
    auto const& frame = m_coded_frames[0];
    if (!is_frame_evictable_while_locked(frame, current_time))
        return {};
    return frame.timestamp();
}

size_t TrackBufferDemuxer::take_earliest_frame()
{
    Sync::MutexLocker locker { m_mutex };
    auto frame = m_coded_frames.take_first();
    m_track_buffer_ranges.remove_range(frame.timestamp(), frame.timestamp() + frame.duration());
    auto bytes = frame.data().size();
    m_total_bytes -= bytes;
    if (m_read_position > 0)
        m_read_position--;
    queue_scan_state_change_dispatch_while_locked();
    return bytes;
}

Optional<AK::Duration> TrackBufferDemuxer::latest_evictable_frame_timestamp(AK::Duration current_time) const
{
    Sync::MutexLocker locker { m_mutex };
    if (m_coded_frames.is_empty())
        return {};
    auto const& frame = m_coded_frames.last();
    if (!is_frame_evictable_while_locked(frame, current_time))
        return {};
    return frame.timestamp();
}

TrackBufferDemuxer::EvictedFrame TrackBufferDemuxer::take_latest_frame()
{
    Sync::MutexLocker locker { m_mutex };
    auto frame = m_coded_frames.take_last();
    m_track_buffer_ranges.remove_range(frame.timestamp(), frame.timestamp() + frame.duration());
    auto bytes = frame.data().size();
    m_total_bytes -= bytes;
    queue_scan_state_change_dispatch_while_locked();
    return {
        .byte_count = bytes,
        .presentation_timestamp = frame.timestamp(),
        .decode_timestamp = decode_timestamp(frame),
    };
}

void TrackBufferDemuxer::set_reached_end_of_stream()
{
    Sync::MutexLocker locker { m_mutex };
    m_reached_end_of_stream = true;
    m_data_changed.broadcast();
    queue_scan_state_change_dispatch_while_locked();
}

void TrackBufferDemuxer::clear_reached_end_of_stream()
{
    Sync::MutexLocker locker { m_mutex };
    m_reached_end_of_stream = false;
    queue_scan_state_change_dispatch_while_locked();
}

Media::DecoderErrorOr<void> TrackBufferDemuxer::create_context_for_track(Media::Track const&)
{
    return {};
}

Media::DecoderErrorOr<Vector<Media::Track>> TrackBufferDemuxer::get_tracks_for_type(Media::TrackType type)
{
    if (m_track.type() == type)
        return Vector { m_track };
    return Vector<Media::Track> {};
}

Media::DecoderErrorOr<Optional<Media::Track>> TrackBufferDemuxer::get_preferred_track_for_type(Media::TrackType type)
{
    if (m_track.type() == type)
        return Optional<Media::Track> { m_track };
    return Optional<Media::Track> {};
}

AK::Duration TrackBufferDemuxer::maximum_time_range_gap() const
{
    return m_maximum_frame_duration + m_maximum_frame_duration;
}

bool TrackBufferDemuxer::next_frame_is_in_gap_while_locked() const
{
    auto max_gap = maximum_time_range_gap();
    if (!m_last_returned_decode_timestamp.has_value() || max_gap.is_zero())
        return false;
    if (m_read_position >= m_coded_frames.size())
        return false;
    auto delta = decode_timestamp(m_coded_frames[m_read_position]) - m_last_returned_decode_timestamp.value();
    return delta > max_gap;
}

Media::DecoderErrorOr<Media::CodedFrame> TrackBufferDemuxer::get_next_sample_for_track(Media::Track const&)
{
    Sync::MutexLocker locker { m_mutex };

    bool notified_blocked = false;
    Optional<Media::DecoderError> error;
    while (m_read_position >= m_coded_frames.size() || next_frame_is_in_gap_while_locked()) {
        if (m_aborted.load()) {
            error = Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Read aborted"sv);
            break;
        }
        if (m_read_position >= m_coded_frames.size() && m_reached_end_of_stream) {
            error = Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "End of stream"sv);
            break;
        }
        if (!notified_blocked) {
            notified_blocked = true;
            if (m_read_blocked_change_handler)
                m_read_blocked_change_handler(Media::ReadBlocked::Yes);
        }
        m_data_changed.wait();
    }

    if (notified_blocked && m_read_blocked_change_handler)
        m_read_blocked_change_handler(Media::ReadBlocked::No);

    if (error.has_value())
        return error.release_value();

    m_last_returned_presentation_timestamp = m_coded_frames[m_read_position].timestamp();
    m_last_returned_decode_timestamp = decode_timestamp(m_coded_frames[m_read_position]);
    return m_coded_frames[m_read_position++];
}

Media::DecoderErrorOr<Media::CodecID> TrackBufferDemuxer::get_codec_id_for_track(Media::Track const&)
{
    return m_codec_id;
}

Media::DecoderErrorOr<ReadonlyBytes> TrackBufferDemuxer::get_codec_initialization_data_for_track(Media::Track const&)
{
    return m_codec_initialization_data.bytes();
}

AK::Duration TrackBufferDemuxer::select_fast_seek_target_for_track(Media::Track const&, AK::Duration target, Media::SeekMode mode)
{
    Sync::MutexLocker locker { m_mutex };
    if (m_coded_frames.is_empty())
        return target;

    VERIFY(mode == Media::SeekMode::FastBefore || mode == Media::SeekMode::FastAfter);
    auto side = mode == Media::SeekMode::FastBefore ? KeyframeSide::AtOrBefore : KeyframeSide::AtOrAfter;
    auto keyframe = find_nearest_keyframe(m_coded_frames, target, side);
    return keyframe.has_value() ? keyframe->timestamp : target;
}

Media::DecoderErrorOr<Media::DemuxerSeekResult> TrackBufferDemuxer::seek_to_most_recent_keyframe(Media::Track const&, AK::Duration timestamp, Media::DemuxerSeekOptions)
{
    Sync::MutexLocker locker { m_mutex };

    auto end_seek = [&]() -> Media::DecoderError {
        m_read_position = m_coded_frames.size();
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Seek target is not buffered"sv);
    };

    while (true) {
        if (m_aborted.load())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Seek aborted"sv);

        auto keyframe = find_nearest_keyframe(m_coded_frames, timestamp, KeyframeSide::AtOrBefore);
        bool selected_future_keyframe = false;
        if (!keyframe.has_value()) {
            keyframe = find_nearest_keyframe(m_coded_frames, timestamp, KeyframeSide::AtOrAfter);
            selected_future_keyframe = keyframe.has_value();
        }

        if (keyframe.has_value()) {
            static constexpr auto MAXIMUM_EARLY_SEEK_GAP = AK::Duration::from_milliseconds(100);

            auto maximum_gap = maximum_time_range_gap();
            auto ranges = m_track_buffer_ranges.coalesced(maximum_gap);
            auto range = ranges.range_at_or_after(timestamp);
            auto target_is_buffered = range.has_value() && range->start <= timestamp && range->end >= timestamp;
            auto future_keyframe_is_nearby = selected_future_keyframe && range.has_value()
                && keyframe->timestamp - timestamp <= maximum_gap;
            auto target_precedes_buffered_data = selected_future_keyframe && range.has_value()
                && timestamp < range->start && range->start - timestamp <= MAXIMUM_EARLY_SEEK_GAP
                && keyframe->timestamp <= range->end;
            auto target_follows_buffered_data = !ranges.is_empty() && timestamp >= ranges.highest_end_time()
                && (m_reached_end_of_stream || timestamp - ranges.highest_end_time() <= maximum_gap);

            if (!target_is_buffered && !future_keyframe_is_nearby && !target_precedes_buffered_data && !target_follows_buffered_data) {
                if (m_reached_end_of_stream)
                    return end_seek();
                m_data_changed.wait();
                continue;
            }

            m_read_position = keyframe->position;
            m_last_returned_presentation_timestamp = m_coded_frames[m_read_position].timestamp();
            m_last_returned_decode_timestamp = decode_timestamp(m_coded_frames[m_read_position]);
            m_data_changed.broadcast();

            return Media::DemuxerSeekResult::MovedPosition;
        }

        if (m_reached_end_of_stream)
            return end_seek();
        m_data_changed.wait();
    }
}

Media::DecoderErrorOr<AK::Duration> TrackBufferDemuxer::duration_of_track(Media::Track const&)
{
    return AK::Duration::zero();
}

Media::DecoderErrorOr<AK::Duration> TrackBufferDemuxer::total_duration()
{
    return AK::Duration::zero();
}

Media::DemuxerScanState const& TrackBufferDemuxer::scan_state() const
{
    return m_scan_state;
}

void TrackBufferDemuxer::set_scan_state_change_handler(Function<void()> handler)
{
    m_scan_state_change_handler = move(handler);
    Sync::MutexLocker locker { m_mutex };
    m_scan_state_change_handler_event_loop = &Core::EventLoop::current();
    // Deliver any state that was built up before a home event loop existed.
    queue_scan_state_change_dispatch_while_locked();
}

void TrackBufferDemuxer::queue_scan_state_change_dispatch_while_locked()
{
    if (m_scan_state_change_dispatch_pending)
        return;
    if (m_scan_state_change_handler_event_loop == nullptr)
        return;
    m_scan_state_change_dispatch_pending = true;
    m_scan_state_change_handler_event_loop->deferred_invoke([self = NonnullRefPtr(*this)] {
        bool reached_end_of_stream;
        {
            Sync::MutexLocker locker { self->m_mutex };
            self->m_scan_state_change_dispatch_pending = false;
            reached_end_of_stream = self->m_reached_end_of_stream;
        }
        Vector<Media::DemuxerTrackScanState> tracks;
        tracks.empend(self->m_track, self->track_buffer_ranges(), reached_end_of_stream);
        self->m_scan_state = { move(tracks), AK::Duration::zero() };
        if (self->m_scan_state_change_handler)
            self->m_scan_state_change_handler();
    });
}

void TrackBufferDemuxer::set_blocking_reads_aborted_for_track(Media::Track const&)
{
    m_aborted.store(true);
    Sync::MutexLocker locker { m_mutex };
    m_data_changed.broadcast();
}

void TrackBufferDemuxer::reset_blocking_reads_aborted_for_track(Media::Track const&)
{
    m_aborted.store(false);
}

void TrackBufferDemuxer::set_read_blocked_change_handler_for_track(Media::Track const&, Media::ReadBlockedChangeHandler handler)
{
    Sync::MutexLocker locker { m_mutex };
    m_read_blocked_change_handler = move(handler);
}

}
