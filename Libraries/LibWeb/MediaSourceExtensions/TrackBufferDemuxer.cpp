/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>

namespace Web::MediaSourceExtensions {

TrackBufferDemuxer::TrackBufferDemuxer(Media::Track const& track, Media::CodecID codec_id, ByteBuffer codec_initialization_data)
    : m_track(track)
    , m_codec_id(codec_id)
    , m_codec_initialization_data(move(codec_initialization_data))
{
}

TrackBufferDemuxer::~TrackBufferDemuxer() = default;

Media::TimeRanges TrackBufferDemuxer::track_buffer_ranges() const
{
    Threading::MutexLocker locker { m_mutex };
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
    Threading::MutexLocker locker { m_mutex };
    auto start = frame.timestamp();
    auto end = frame.timestamp() + frame.duration();
    m_last_frame_duration = frame.duration();
    m_track_buffer_ranges.add_range(start, end);

    // Insert in sorted order by presentation timestamp using binary search.
    // Overlapping frames should have been removed by remove_coded_frames_and_dependants_in_range()
    // before this call.
    auto timestamp = frame.timestamp();
    size_t insert_index = 0;
    auto* existing_coded_frame = binary_search(m_coded_frames, timestamp, &insert_index, [](AK::Duration needle, Media::CodedFrame const& frame) {
        return needle <=> frame.timestamp();
    });
    VERIFY(!existing_coded_frame);
    if (insert_index < m_coded_frames.size() && m_coded_frames[insert_index].timestamp() < timestamp)
        insert_index++;

    m_coded_frames.insert(insert_index, move(frame));

    if (insert_index <= m_read_position && (!m_last_returned_timestamp.has_value() || m_last_returned_timestamp.value() > timestamp))
        m_read_position++;

    m_data_changed.broadcast();
}

void TrackBufferDemuxer::remove_coded_frames_and_dependants_in_range(AK::Duration start, AK::Duration end)
{
    Threading::MutexLocker locker { m_mutex };

    // https://w3c.github.io/media-source/#sourcebuffer-coded-frame-processing
    // 1.13. Remove all coded frames from track buffer that have a presentation timestamp greater than
    //       or equal to presentation timestamp and less than frame end timestamp.
    // 1.14. Remove all possible decoding dependencies on the coded frames removed in the previous step
    //       by removing all coded frames from track buffer between those frames removed in the previous
    //       step and the next random access point after those removed frames.

    // Find the first frame at or after the start of the range.
    size_t remove_start = 0;
    while (remove_start < m_coded_frames.size() && m_coded_frames[remove_start].timestamp() < start)
        remove_start++;

    // Find all overlapping frames plus subsequent frames up to the next keyframe.
    size_t remove_end = remove_start;
    while (remove_end < m_coded_frames.size() && m_coded_frames[remove_end].timestamp() < end)
        remove_end++;
    if (remove_end <= remove_start)
        return;
    while (remove_end < m_coded_frames.size() && !m_coded_frames[remove_end].is_keyframe())
        remove_end++;

    for (size_t i = remove_start; i < remove_end; i++) {
        auto const& removed_frame = m_coded_frames[i];
        auto removed_start = removed_frame.timestamp();
        auto removed_end = (removed_frame.timestamp() + removed_frame.duration());
        m_track_buffer_ranges.remove_range(removed_start, removed_end);
    }

    m_coded_frames.remove(remove_start, remove_end - remove_start);

    // Adjust read position if it was in or past the removed range.
    if (m_read_position >= remove_end) {
        m_read_position -= (remove_end - remove_start);
    } else if (m_read_position > remove_start) {
        m_read_position = remove_start > 0 ? remove_start - 1 : 0;

        while (m_read_position > 0 && !m_coded_frames[m_read_position].is_keyframe())
            m_read_position--;

        m_last_returned_timestamp.clear();
    }
}

void TrackBufferDemuxer::set_reached_end_of_stream()
{
    Threading::MutexLocker locker { m_mutex };
    m_reached_end_of_stream = true;
    m_data_changed.broadcast();
}

void TrackBufferDemuxer::clear_reached_end_of_stream()
{
    Threading::MutexLocker locker { m_mutex };
    m_reached_end_of_stream = false;
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
    return m_last_frame_duration + m_last_frame_duration;
}

bool TrackBufferDemuxer::next_frame_is_in_gap_while_locked() const
{
    auto max_gap = maximum_time_range_gap();
    if (!m_last_returned_timestamp.has_value() || max_gap.is_zero())
        return false;
    if (m_read_position >= m_coded_frames.size())
        return false;
    auto delta = m_coded_frames[m_read_position].timestamp() - m_last_returned_timestamp.value();
    return delta > max_gap;
}

Media::DecoderErrorOr<Media::CodedFrame> TrackBufferDemuxer::get_next_sample_for_track(Media::Track const&)
{
    Threading::MutexLocker locker { m_mutex };

    while (m_read_position >= m_coded_frames.size() || next_frame_is_in_gap_while_locked()) {
        if (m_aborted.load())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Read aborted"sv);
        if (m_read_position >= m_coded_frames.size() && m_reached_end_of_stream)
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "End of stream"sv);
        m_data_changed.wait();
    }

    m_last_returned_timestamp = m_coded_frames[m_read_position].timestamp();
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

Media::DecoderErrorOr<Media::DemuxerSeekResult> TrackBufferDemuxer::seek_to_most_recent_keyframe(Media::Track const&, AK::Duration timestamp, Media::DemuxerSeekOptions)
{
    Threading::MutexLocker locker { m_mutex };

    size_t best_position = 0;
    AK::Duration best_timestamp;

    while (true) {
        if (m_aborted.load())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Seek aborted"sv);

        best_timestamp = AK::Duration::max();

        for (size_t i = 0; i < m_coded_frames.size(); i++) {
            auto const& frame = m_coded_frames[i];
            if (frame.timestamp() >= timestamp)
                break;
            if (frame.is_keyframe()) {
                best_position = i;
                best_timestamp = frame.timestamp();
            }
        }

        auto max_gap = maximum_time_range_gap();
        if (timestamp >= best_timestamp) {
            auto has_gap = [&] {
                auto prior_timestamp = m_coded_frames[best_position].timestamp();
                for (size_t i = best_position + 1; i < m_coded_frames.size(); i++) {
                    auto const& frame = m_coded_frames[i];
                    auto delta = frame.timestamp() - prior_timestamp;
                    if (delta > max_gap)
                        return true;
                    if (frame.timestamp() >= timestamp)
                        break;
                    prior_timestamp = frame.timestamp();
                }
                return timestamp - prior_timestamp > max_gap;
            }();
            if (!has_gap)
                break;
        } else if (best_position < m_coded_frames.size()) {
            auto& future_frame = m_coded_frames[best_position];
            if (future_frame.is_keyframe() && future_frame.timestamp() - timestamp <= max_gap) {
                best_timestamp = timestamp;
                break;
            }
        }

        m_data_changed.wait();
    }

    m_read_position = best_position;
    m_last_returned_timestamp = best_timestamp;
    m_data_changed.broadcast();
    return Media::DemuxerSeekResult::MovedPosition;
}

Media::DecoderErrorOr<AK::Duration> TrackBufferDemuxer::duration_of_track(Media::Track const&)
{
    return AK::Duration::zero();
}

Media::DecoderErrorOr<AK::Duration> TrackBufferDemuxer::total_duration()
{
    return AK::Duration::zero();
}

Media::TimeRanges TrackBufferDemuxer::buffered_time_ranges() const
{
    return track_buffer_ranges();
}

void TrackBufferDemuxer::set_blocking_reads_aborted_for_track(Media::Track const&)
{
    m_aborted.store(true);
    Threading::MutexLocker locker { m_mutex };
    m_data_changed.broadcast();
}

void TrackBufferDemuxer::reset_blocking_reads_aborted_for_track(Media::Track const&)
{
    m_aborted.store(false);
}

bool TrackBufferDemuxer::is_read_blocked_for_track(Media::Track const&)
{
    Threading::MutexLocker locker { m_mutex };
    if (m_aborted.load())
        return false;
    return m_read_position >= m_coded_frames.size() || next_frame_is_in_gap_while_locked();
}

}
