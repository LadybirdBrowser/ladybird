/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>

namespace Web::MediaSourceExtensions {

TrackBufferDemuxer::TrackBufferDemuxer(Media::Track const& track)
    : m_track(track)
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

void TrackBufferDemuxer::extend_run_bounds_for_frame(FrameRun& run, Media::CodedFrame const& frame)
{
    auto start = frame.presentation_timestamp();
    auto end = start + frame.duration();
    if (run.frames.is_empty()) {
        run.presentation_start = start;
        run.presentation_end = end;
    } else {
        run.presentation_start = min(run.presentation_start, start);
        run.presentation_end = max(run.presentation_end, end);
    }
}

void TrackBufferDemuxer::recalculate_run_bounds(FrameRun& run)
{
    VERIFY(!run.frames.is_empty());
    run.presentation_start = run.frames.first().presentation_timestamp();
    run.presentation_end = run.presentation_start;
    for (auto const& frame : run.frames)
        extend_run_bounds_for_frame(run, frame);
}

// A run's first frame carries the codec configuration to decode it from, so a run never has to be searched
// past to find one.
Optional<ReadonlyBytes> TrackBufferDemuxer::codec_configuration_after_frame_prefix_while_locked(FrameRun const& run, size_t frame_count)
{
    VERIFY(frame_count <= run.frames.size());
    for (auto index = frame_count; index-- > 0;) {
        auto configuration = run.frames[index].new_codec_configuration();
        if (configuration.has_value())
            return configuration;
    }
    return {};
}

void TrackBufferDemuxer::add_coded_frame(Media::CodedFrame frame)
{
    Sync::MutexLocker locker { m_mutex };

    auto update_last_appended_codec_configuration = [&] {
        auto configuration = frame.new_codec_configuration();
        if (configuration.has_value())
            m_last_appended_codec_configuration = MUST(FixedArray<u8>::create(configuration.value()));
    };

    // A frame continues a run when it decodes after every frame already in it, without a gap that
    // would make the two sides separately decodable.
    auto continues_run = [&](FrameRun const& run) {
        auto last_decode_timestamp = run.frames.last().decode_timestamp();
        auto delta = frame.decode_timestamp() - last_decode_timestamp;
        return delta > AK::Duration::zero() && delta <= maximum_time_range_gap();
    };

    Optional<size_t> run_to_continue;
    for (size_t index = m_runs.size(); index-- > 0;) {
        if (!continues_run(m_runs[index]))
            continue;
        run_to_continue = index;
        break;
    }

    // AD-HOC: Coded frame eviction can remove the random access point that a frame decodes from after its
    //         coded frame group began, leaving the frame with nothing to be decoded from.
    if (!run_to_continue.has_value() && !frame.is_keyframe()) {
        update_last_appended_codec_configuration();
        return;
    }

    count_frame_duration_while_locked(frame.duration());
    m_track_buffer_ranges.add_range(frame.presentation_timestamp(), frame.presentation_timestamp() + frame.duration());
    m_total_bytes += frame.data().size();
    m_last_appended_decode_timestamp = frame.decode_timestamp();

    if (run_to_continue.has_value()) {
        auto& run = m_runs[run_to_continue.value()];
        extend_run_bounds_for_frame(run, frame);
        update_last_appended_codec_configuration();
        run.frames.append(move(frame));
        m_data_changed.broadcast();
        queue_scan_state_change_dispatch_while_locked();
        return;
    }

    FrameRun run;
    if (!frame.new_codec_configuration().has_value() && m_last_appended_codec_configuration.has_value())
        frame.set_new_codec_configuration(MUST(m_last_appended_codec_configuration->clone()));
    extend_run_bounds_for_frame(run, frame);
    update_last_appended_codec_configuration();
    run.frames.append(move(frame));

    size_t insert_index = 0;
    while (insert_index < m_runs.size() && m_runs[insert_index].presentation_start < run.presentation_start)
        insert_index++;
    m_runs.insert(insert_index, move(run));
    verify_runs_are_ordered_around_index_while_locked(insert_index);
    if (insert_index <= m_current_run && !m_runs.is_empty())
        m_current_run = min(m_current_run + 1, m_runs.size() - 1);

    m_data_changed.broadcast();
    queue_scan_state_change_dispatch_while_locked();
}

void TrackBufferDemuxer::note_cursor_jumped_while_locked()
{
    if (m_cursor_continuity == CursorContinuity::Continuous)
        m_cursor_continuity = CursorContinuity::Jumped;
}

void TrackBufferDemuxer::verify_runs_are_ordered_around_index_while_locked(size_t run_index) const
{
    if (run_index > 0)
        VERIFY(m_runs[run_index - 1].presentation_start <= m_runs[run_index].presentation_start);
    if (run_index + 1 < m_runs.size())
        VERIFY(m_runs[run_index].presentation_start <= m_runs[run_index + 1].presentation_start);
}

void TrackBufferDemuxer::split_run_while_locked(size_t run_index, size_t split_at, FixedArray<u8> codec_configuration_before_tail)
{
    auto& run = m_runs[run_index];
    VERIFY(split_at > 0 && split_at < run.frames.size());

    FrameRun tail;
    for (size_t index = split_at; index < run.frames.size(); index++)
        tail.frames.append(move(run.frames[index]));
    run.frames.remove(split_at, run.frames.size() - split_at);
    if (!tail.frames.first().new_codec_configuration().has_value())
        tail.frames.first().set_new_codec_configuration(move(codec_configuration_before_tail));
    recalculate_run_bounds(run);
    recalculate_run_bounds(tail);

    auto insert_index = run_index + 1;
    while (insert_index < m_runs.size() && m_runs[insert_index].presentation_start < tail.presentation_start)
        insert_index++;
    m_runs.insert(insert_index, move(tail));
    verify_runs_are_ordered_around_index_while_locked(run_index);
    verify_runs_are_ordered_around_index_while_locked(insert_index);
    if (m_current_run == run_index && m_current_frame >= split_at) {
        m_current_run = insert_index;
        m_current_frame -= split_at;
    } else if (m_current_run >= insert_index) {
        m_current_run++;
    }
}

size_t TrackBufferDemuxer::erase_frames_and_dependants_while_locked(size_t run_index, size_t first_frame, size_t minimum_frame_count)
{
    auto& run = m_runs[run_index];
    VERIFY(minimum_frame_count > 0);
    VERIFY(first_frame + minimum_frame_count <= run.frames.size());

    size_t bytes = 0;
    size_t frame_count = 0;
    size_t removed_frames_with_the_maximum_duration = 0;
    for (size_t index = first_frame; index < run.frames.size(); index++) {
        auto const& frame = run.frames[index];
        if (frame_count >= minimum_frame_count && frame.is_keyframe())
            break;
        m_track_buffer_ranges.remove_range(frame.presentation_timestamp(), frame.presentation_timestamp() + frame.duration());
        bytes += frame.data().size();
        if (frame.duration() == m_maximum_frame_duration)
            removed_frames_with_the_maximum_duration++;
        frame_count++;
    }

    FixedArray<u8> codec_configuration_before_remaining_frames;
    if (first_frame + frame_count < run.frames.size()) {
        auto configuration = codec_configuration_after_frame_prefix_while_locked(run, first_frame + frame_count);
        if (configuration.has_value())
            codec_configuration_before_remaining_frames = MUST(FixedArray<u8>::create(*configuration));
    }

    m_total_bytes -= bytes;
    run.frames.remove(first_frame, frame_count);
    decrement_frames_with_maximum_duration_while_locked(removed_frames_with_the_maximum_duration);

    if (m_current_run == run_index) {
        if (m_current_frame >= first_frame + frame_count) {
            m_current_frame -= frame_count;
        } else if (m_current_frame >= first_frame) {
            m_current_frame = first_frame;
            note_cursor_jumped_while_locked();
        }
    }

    if (run.frames.is_empty()) {
        m_runs.remove(run_index);
        if (m_current_run > run_index)
            m_current_run--;
        else if (m_current_run == run_index)
            m_cursor_continuity = CursorContinuity::NeedsReanchoring;
        return bytes;
    }

    if (first_frame == 0) {
        if (!run.frames.first().new_codec_configuration().has_value())
            run.frames.first().set_new_codec_configuration(move(codec_configuration_before_remaining_frames));
    } else if (first_frame < run.frames.size()) {
        VERIFY(run.frames[first_frame].is_keyframe());
        split_run_while_locked(run_index, first_frame, move(codec_configuration_before_remaining_frames));
        return bytes;
    }
    recalculate_run_bounds(run);
    return bytes;
}

void TrackBufferDemuxer::remove_coded_frames_and_dependants_in_range(AK::Duration start, AK::Duration end)
{
    (void)remove_coded_frames_and_dependants_in_range_returning_presentation_timestamp_at(start, end, {});
}

Optional<AK::Duration> TrackBufferDemuxer::remove_coded_frames_and_dependants_in_range_returning_presentation_timestamp_at(AK::Duration start, AK::Duration end, Optional<AK::Duration> last_decode_timestamp)
{
    Sync::MutexLocker locker { m_mutex };

    Optional<AK::Duration> removed_frame_presentation_timestamp;

    for (size_t run_index = m_runs.size(); run_index-- > 0;) {
        auto& run = m_runs[run_index];
        if (run.presentation_end <= start || run.presentation_start >= end)
            continue;

        Optional<size_t> first_index;
        size_t last_index = 0;
        for (size_t index = 0; index < run.frames.size(); index++) {
            auto const& frame = run.frames[index];
            auto timestamp = frame.presentation_timestamp();
            if (timestamp < start || timestamp >= end)
                continue;
            if (!first_index.has_value())
                first_index = index;
            last_index = index;
        }
        if (!first_index.has_value())
            continue;

        for (size_t index = first_index.value(); index < run.frames.size(); index++) {
            auto const& frame = run.frames[index];
            if (index > last_index && frame.is_keyframe())
                break;
            if (last_decode_timestamp == frame.decode_timestamp())
                removed_frame_presentation_timestamp = frame.presentation_timestamp();
            if (m_last_appended_decode_timestamp == frame.decode_timestamp())
                m_last_appended_decode_timestamp.clear();
        }

        erase_frames_and_dependants_while_locked(run_index, first_index.value(), last_index - first_index.value() + 1);
    }

    m_data_changed.broadcast();
    queue_scan_state_change_dispatch_while_locked();

    return removed_frame_presentation_timestamp;
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
    if (m_cursor_presentation_timestamp.has_value()) {
        time_range_start = min(time_range_start, m_cursor_presentation_timestamp.value());
        time_range_end = max(time_range_end, m_cursor_presentation_timestamp.value());
    }
    return frame.presentation_timestamp() < time_range_start || frame.presentation_timestamp() > time_range_end;
}

bool TrackBufferDemuxer::run_ends_at_last_appended_frame_while_locked(FrameRun const& run) const
{
    return m_last_appended_decode_timestamp.has_value()
        && run.frames.last().decode_timestamp() == m_last_appended_decode_timestamp.value();
}

Optional<AK::Duration> TrackBufferDemuxer::earliest_evictable_frame_timestamp(AK::Duration current_time) const
{
    Sync::MutexLocker locker { m_mutex };
    if (m_runs.is_empty())
        return {};

    auto const& frames = m_runs.first().frames;
    size_t frame_count = 0;
    for (auto const& frame : frames) {
        if (frame_count > 0 && frame.is_keyframe())
            break;
        if (!is_frame_evictable_while_locked(frame, current_time))
            return {};
        frame_count++;
    }

    if (frame_count == frames.size() && run_ends_at_last_appended_frame_while_locked(m_runs.first()))
        return {};
    return frames.first().presentation_timestamp();
}

size_t TrackBufferDemuxer::take_earliest_frame_and_dependants()
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(!m_reached_end_of_stream);
    VERIFY(!m_runs.is_empty());

    auto bytes = erase_frames_and_dependants_while_locked(0, 0, 1);
    queue_scan_state_change_dispatch_while_locked();
    return bytes;
}

Optional<AK::Duration> TrackBufferDemuxer::latest_evictable_frame_timestamp(AK::Duration current_time) const
{
    Sync::MutexLocker locker { m_mutex };
    if (m_runs.is_empty())
        return {};

    if (run_ends_at_last_appended_frame_while_locked(m_runs.last()))
        return {};

    auto const& frame = m_runs.last().frames.last();
    if (!is_frame_evictable_while_locked(frame, current_time))
        return {};
    return frame.presentation_timestamp();
}

TrackBufferDemuxer::RemovedFrame TrackBufferDemuxer::take_latest_frame()
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(!m_reached_end_of_stream);

    auto& run = m_runs.last();
    auto const& frame = run.frames.last();
    RemovedFrame removed { .byte_size = frame.data().size(), .decode_timestamp = frame.decode_timestamp() };

    erase_frames_and_dependants_while_locked(m_runs.size() - 1, run.frames.size() - 1, 1);
    queue_scan_state_change_dispatch_while_locked();
    return removed;
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

void TrackBufferDemuxer::count_frame_duration_while_locked(AK::Duration duration)
{
    if (duration > m_maximum_frame_duration) {
        m_maximum_frame_duration = duration;
        m_frames_at_maximum_duration = 1;
        return;
    }
    if (duration == m_maximum_frame_duration)
        m_frames_at_maximum_duration++;
}

void TrackBufferDemuxer::decrement_frames_with_maximum_duration_while_locked(size_t count)
{
    VERIFY(m_frames_at_maximum_duration >= count);
    m_frames_at_maximum_duration -= count;

    if (m_frames_at_maximum_duration == 0) {
        m_maximum_frame_duration = AK::Duration::zero();
        for (auto const& run : m_runs) {
            for (auto const& frame : run.frames)
                count_frame_duration_while_locked(frame.duration());
        }
    }
}

Optional<size_t> TrackBufferDemuxer::find_run_to_play_from_while_locked(AK::Duration timestamp) const
{
    Optional<size_t> earliest_run_after_timestamp;
    for (size_t index = m_runs.size(); index-- > 0;) {
        auto const& run = m_runs[index];
        if (run.presentation_start <= timestamp && timestamp < run.presentation_end)
            return index;
        if (run.presentation_start >= timestamp)
            earliest_run_after_timestamp = index;
    }

    if (!earliest_run_after_timestamp.has_value())
        return {};
    if (m_runs[*earliest_run_after_timestamp].presentation_start - timestamp > maximum_time_range_gap())
        return {};
    return earliest_run_after_timestamp;
}

Optional<ReadonlyBytes> TrackBufferDemuxer::codec_configuration_at_position_while_locked(size_t run_index, size_t frame_index) const
{
    auto const& run = m_runs[run_index];
    VERIFY(frame_index < run.frames.size());
    return codec_configuration_after_frame_prefix_while_locked(run, frame_index + 1);
}

Media::DecoderErrorOr<Media::CodedFrame> TrackBufferDemuxer::get_next_sample_for_track(Media::Track const&)
{
    Sync::MutexLocker locker { m_mutex };

    auto continue_into_next_run_while_locked = [&] {
        if (m_current_run + 1 >= m_runs.size())
            return false;
        auto const& current_run = m_runs[m_current_run];
        auto const& next_run = m_runs[m_current_run + 1];
        auto gap = next_run.presentation_start - current_run.presentation_end;
        if (gap > maximum_time_range_gap())
            return false;

        m_current_run++;
        m_current_frame = 0;
        note_cursor_jumped_while_locked();

        return true;
    };

    bool notified_blocked = false;
    Optional<Media::DecoderError> error;
    while (m_cursor_continuity == CursorContinuity::NeedsReanchoring || m_current_run >= m_runs.size() || m_current_frame >= m_runs[m_current_run].frames.size()) {
        if (m_cursor_continuity == CursorContinuity::NeedsReanchoring) {
            auto anchor = m_cursor_presentation_timestamp.value_or(AK::Duration::zero());
            if (move_cursor_to_presentation_time_while_locked(anchor)) {
                continue;
            }
        } else if (continue_into_next_run_while_locked()) {
            continue;
        }
        if (m_aborted.load()) {
            error = Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Read aborted"sv);
            break;
        }
        if (m_reached_end_of_stream && m_current_run + 1 >= m_runs.size()) {
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

    auto const& stored_frame = m_runs[m_current_run].frames[m_current_frame];

    if (m_cursor_continuity == CursorContinuity::Jumped) {
        m_cursor_continuity = CursorContinuity::Continuous;
        auto configuration = codec_configuration_at_position_while_locked(m_current_run, m_current_frame);
        auto configuration_is_new = configuration.has_value() && (!m_last_delivered_codec_configuration.has_value() || *configuration != m_last_delivered_codec_configuration->span());
        if (configuration_is_new)
            m_last_delivered_codec_configuration = MUST(FixedArray<u8>::create(*configuration));

        // The first frame of a run always includes a codec configuration, but we only want to emit it if it differs
        // from the config at the end of the last run.
        if (configuration_is_new || stored_frame.new_codec_configuration().has_value()) {
            Optional<FixedArray<u8>> new_codec_configuration;
            if (configuration_is_new)
                new_codec_configuration = MUST(m_last_delivered_codec_configuration->clone());
            Media::CodedFrame frame {
                stored_frame.codec_id(),
                stored_frame.presentation_timestamp(),
                stored_frame.decode_timestamp(),
                stored_frame.duration(),
                stored_frame.flags(),
                MUST(FixedArray<u8>::create(stored_frame.data())),
                move(new_codec_configuration),
            };
            m_cursor_presentation_timestamp = frame.presentation_timestamp();
            m_current_frame++;
            return frame;
        }
    } else if (auto configuration = stored_frame.new_codec_configuration(); configuration.has_value()) {
        m_last_delivered_codec_configuration = MUST(FixedArray<u8>::create(*configuration));
    }

    m_cursor_presentation_timestamp = stored_frame.presentation_timestamp();
    m_current_frame++;
    return stored_frame;
}

AK::Duration TrackBufferDemuxer::select_fast_seek_target_for_track(Media::Track const&, AK::Duration target, Media::SeekMode mode)
{
    Sync::MutexLocker locker { m_mutex };

    Optional<AK::Duration> best_timestamp;
    for (auto const& run : m_runs) {
        for (auto const& frame : run.frames) {
            if (!frame.is_keyframe())
                continue;
            auto timestamp = frame.presentation_timestamp();
            if (mode == Media::SeekMode::FastBefore) {
                if (timestamp <= target && (!best_timestamp.has_value() || timestamp > best_timestamp.value()))
                    best_timestamp = timestamp;
            } else {
                VERIFY(mode == Media::SeekMode::FastAfter);
                if (timestamp >= target && (!best_timestamp.has_value() || timestamp < best_timestamp.value()))
                    best_timestamp = timestamp;
            }
        }
    }
    return best_timestamp.value_or(target);
}

bool TrackBufferDemuxer::move_cursor_to_presentation_time_while_locked(AK::Duration timestamp)
{
    auto run_index = find_run_to_play_from_while_locked(timestamp);
    if (!run_index.has_value())
        return false;

    auto const& run = m_runs[run_index.value()];
    VERIFY(run.frames.first().is_keyframe());
    size_t keyframe_index = 0;
    for (size_t index = 1; index < run.frames.size(); index++) {
        auto const& frame = run.frames[index];
        if (!frame.is_keyframe() || frame.presentation_timestamp() > timestamp)
            continue;
        if (frame.presentation_timestamp() >= run.frames[keyframe_index].presentation_timestamp())
            keyframe_index = index;
    }

    m_current_run = run_index.value();
    m_current_frame = keyframe_index;
    m_cursor_presentation_timestamp = run.frames[m_current_frame].presentation_timestamp();
    m_cursor_continuity = CursorContinuity::Jumped;
    return true;
}

Media::DecoderErrorOr<Media::DemuxerSeekResult> TrackBufferDemuxer::seek_to_most_recent_keyframe(Media::Track const&, AK::Duration timestamp, Media::DemuxerSeekOptions options)
{
    Sync::MutexLocker locker { m_mutex };

    // Forget what the consumer was sent, so that the configuration in effect is delivered again.
    if (has_flag(options, Media::DemuxerSeekOptions::NeedCodecConfiguration))
        m_last_delivered_codec_configuration = {};

    while (true) {
        if (m_aborted.load())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Seek aborted"sv);

        if (move_cursor_to_presentation_time_while_locked(timestamp)) {

            m_data_changed.broadcast();
            return Media::DemuxerSeekResult::MovedPosition;
        }

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
