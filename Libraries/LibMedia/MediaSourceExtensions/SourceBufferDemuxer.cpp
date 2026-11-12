/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/QuickSort.h>
#include <AK/SaturatingMath.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/MediaSourceExtensions/SourceBufferDemuxer.h>

namespace Media::MediaSourceExtensions {

static_assert(sizeof(Media::CodedFrame) <= SourceBufferDemuxer::buffered_bytes_per_frame);

SourceBufferDemuxer::SourceBufferDemuxer(Vector<Media::Track> const& tracks)
{
    m_scan_state.buffered_ranges_policy = buffered_ranges_policy();
    for (auto const& track : tracks)
        m_tracks.append(make<TrackData>(track));
}

SourceBufferDemuxer::~SourceBufferDemuxer() = default;

SourceBufferDemuxer::ChangeBatch::ChangeBatch(RefPtr<SourceBufferDemuxer> demuxer)
    : m_demuxer(move(demuxer))
{
    if (!m_demuxer)
        return;
    MutexLocker locker { m_demuxer->m_mutex };
    VERIFY(!m_demuxer->m_in_change_batch);
    m_demuxer->m_in_change_batch = true;
}

SourceBufferDemuxer::ChangeBatch::~ChangeBatch()
{
    if (!m_demuxer)
        return;
    MutexLocker locker { m_demuxer->m_mutex };
    VERIFY(m_demuxer->m_in_change_batch);
    m_demuxer->m_in_change_batch = false;
    if (m_demuxer->m_has_unpublished_changes)
        m_demuxer->publish_buffered_data_changes_while_locked();
}

// The tracks are fixed at construction, so looking one up needs no lock.
SourceBufferDemuxer::TrackData& SourceBufferDemuxer::track_data(Media::Track const& track)
{
    for (auto& data : m_tracks) {
        if (data->track == track)
            return *data;
    }
    VERIFY_NOT_REACHED();
}

SourceBufferDemuxer::TrackData const& SourceBufferDemuxer::track_data(Media::Track const& track) const
{
    return const_cast<SourceBufferDemuxer&>(*this).track_data(track);
}

bool SourceBufferDemuxer::contributes_to_buffered_ranges(TrackData const& data)
{
    return data.track.type() == Media::TrackType::Audio || data.track.type() == Media::TrackType::Video;
}

AK::Duration SourceBufferDemuxer::maximum_time_range_gap(TrackData const& data)
{
    return data.maximum_frame_duration + data.maximum_frame_duration;
}

// https://w3c.github.io/media-source/#presentation-start-time
bool SourceBufferDemuxer::is_within_start_gap_allowance(AK::Duration timestamp, AK::Duration first_data_start)
{
    // NOTE: For the purposes of determining if HTMLMediaElement's buffered contains a TimeRanges that includes the
    //       current playback position, implementations MAY choose to allow a current playback position at or after
    //       presentation start time and before the first TimeRanges to play the first TimeRanges if that TimeRanges
    //       starts within a reasonably short time, like 1 second, after presentation start time.
    constexpr auto start_gap_allowance = AK::Duration::from_seconds(1);
    return timestamp >= AK::Duration::zero() && timestamp < first_data_start && first_data_start <= start_gap_allowance;
}

void SourceBufferDemuxer::bring_track_buffer_ranges_up_to_date(TrackData& data)
{
    // https://w3c.github.io/media-source/#track-buffer-ranges
    // NOTE: Implementations MAY coalesce adjacent ranges separated by a gap smaller than 2 times the
    //       maximum frame duration buffered so far in this track buffer.
    auto maximum_gap = maximum_time_range_gap(data);

    if (data.track_buffer_ranges_need_rebuild) {
        data.track_buffer_ranges_need_rebuild = false;
        data.frame_intervals_to_add_to_ranges.clear_with_capacity();
        Vector<Media::TimeRanges::Range> frame_intervals;
        for (auto const& run : data.runs) {
            for (auto const& frame : run.frames)
                frame_intervals.append({ frame.presentation_timestamp(), frame.presentation_timestamp() + frame.duration() });
        }
        data.track_buffer_ranges = {};
        data.track_buffer_ranges.add_ranges(frame_intervals, maximum_gap);
        data.track_buffer_ranges_maximum_gap = maximum_gap;
        return;
    }

    // Only erasing frames can narrow the gap, and that rebuilds the ranges.
    VERIFY(maximum_gap >= data.track_buffer_ranges_maximum_gap);
    if (maximum_gap > data.track_buffer_ranges_maximum_gap) {
        data.track_buffer_ranges = data.track_buffer_ranges.coalesced(maximum_gap);
        data.track_buffer_ranges_maximum_gap = maximum_gap;
    }
    if (!data.frame_intervals_to_add_to_ranges.is_empty()) {
        data.track_buffer_ranges.add_ranges(data.frame_intervals_to_add_to_ranges, maximum_gap);
        data.frame_intervals_to_add_to_ranges.clear();
    }
}

Media::TimeRanges SourceBufferDemuxer::track_buffer_ranges(Media::Track const& track) const
{
    MutexLocker locker { m_mutex };
    return track_data(track).track_buffer_ranges;
}

Media::TimeRanges SourceBufferDemuxer::buffered_ranges() const
{
    MutexLocker locker { m_mutex };
    return m_buffered_ranges;
}

AK::Duration SourceBufferDemuxer::highest_end_time_while_locked() const
{
    AK::Duration highest_end_time;
    for (auto const& data : m_tracks)
        highest_end_time = max(highest_end_time, data->track_buffer_ranges.highest_end_time());
    return highest_end_time;
}

Vector<Media::TrackBufferedRanges> SourceBufferDemuxer::track_buffered_ranges_while_locked() const
{
    Vector<Media::TrackBufferedRanges> track_buffered_ranges;
    for (auto const& data : m_tracks) {
        if (contributes_to_buffered_ranges(*data))
            track_buffered_ranges.append({ data->track.identifier(), data->track_buffer_ranges, m_reached_end_of_stream });
    }
    return track_buffered_ranges;
}

Vector<Media::TrackBufferedRanges> SourceBufferDemuxer::track_buffered_ranges() const
{
    MutexLocker locker { m_mutex };
    return track_buffered_ranges_while_locked();
}

Media::DemuxerScanState SourceBufferDemuxer::scan_state_while_locked() const
{
    Media::DemuxerScanState scan_state;
    scan_state.track_buffered_ranges = track_buffered_ranges_while_locked();
    scan_state.buffered_ranges_policy = buffered_ranges_policy();
    scan_state.reached_end_of_stream = m_reached_end_of_stream;
    scan_state.duration = highest_end_time_while_locked();
    return scan_state;
}

void SourceBufferDemuxer::buffered_data_changed_while_locked()
{
    m_has_unpublished_changes = true;
    if (!m_in_change_batch)
        publish_buffered_data_changes_while_locked();
}

void SourceBufferDemuxer::publish_buffered_data_changes_while_locked()
{
    m_has_unpublished_changes = false;
    for (auto& data : m_tracks)
        bring_track_buffer_ranges_up_to_date(*data);
    if (!m_frame_intervals_to_invalidate.is_empty()) {
        m_invalidated_ranges.add_ranges(m_frame_intervals_to_invalidate);
        m_frame_intervals_to_invalidate.clear();
    }

    m_buffered_ranges = scan_state_while_locked().buffered_ranges();
    m_data_changed.broadcast();
    queue_scan_state_change_dispatch_while_locked();
}

void SourceBufferDemuxer::extend_run_bounds_for_frame(FrameRun& run, Media::CodedFrame const& frame)
{
    auto start = frame.presentation_timestamp();
    auto end = start + frame.duration();
    if (run.frames.is_empty()) {
        run.presentation_start = start;
        run.highest_presentation_start = start;
        run.presentation_end = end;
    } else {
        run.presentation_start = min(run.presentation_start, start);
        run.highest_presentation_start = max(run.highest_presentation_start, start);
        run.presentation_end = max(run.presentation_end, end);
    }
}

void SourceBufferDemuxer::recalculate_run_bounds(FrameRun& run)
{
    VERIFY(!run.frames.is_empty());
    run.presentation_start = run.frames.first().presentation_timestamp();
    run.highest_presentation_start = run.presentation_start;
    run.presentation_end = run.presentation_start;
    for (auto const& frame : run.frames)
        extend_run_bounds_for_frame(run, frame);
}

static size_t buffered_byte_count(Media::CodedFrame const& frame)
{
    auto byte_count = SourceBufferDemuxer::buffered_bytes_per_frame + frame.data().size();
    if (auto configuration = frame.new_codec_configuration(); configuration.has_value())
        byte_count += configuration.value().size();
    return byte_count;
}

// A run's first frame carries the codec configuration to decode it from, so a run never has to be searched
// past to find one.
Optional<ReadonlyBytes> SourceBufferDemuxer::codec_configuration_after_frame_prefix(FrameRun const& run, size_t frame_count)
{
    VERIFY(frame_count <= run.frames.size());
    for (auto index = frame_count; index-- > 0;) {
        auto configuration = run.frames[index].new_codec_configuration();
        if (configuration.has_value())
            return configuration;
    }
    return {};
}

AK::Duration SourceBufferDemuxer::highest_presentation_timestamp(Media::Track const& track) const
{
    MutexLocker locker { m_mutex };
    AK::Duration highest_presentation_timestamp;
    for (auto const& run : track_data(track).runs)
        highest_presentation_timestamp = max(highest_presentation_timestamp, run.highest_presentation_start);
    return highest_presentation_timestamp;
}

void SourceBufferDemuxer::carry_codec_configuration_of_dropped_frame(Media::Track const& track, Media::CodedFrame const& frame)
{
    auto configuration = frame.new_codec_configuration();
    if (!configuration.has_value())
        return;
    MutexLocker locker { m_mutex };
    track_data(track).codec_configuration_of_dropped_frame = MUST(FixedArray<u8>::create(configuration.value()));
}

void SourceBufferDemuxer::add_coded_frame(Media::Track const& track, Media::CodedFrame frame)
{
    MutexLocker locker { m_mutex };
    auto& data = track_data(track);

    if (data.codec_configuration_of_dropped_frame.has_value()) {
        if (!frame.new_codec_configuration().has_value())
            frame.set_new_codec_configuration(data.codec_configuration_of_dropped_frame.release_value());
        data.codec_configuration_of_dropped_frame.clear();
    }

    auto update_last_appended_codec_configuration = [&] {
        auto configuration = frame.new_codec_configuration();
        if (configuration.has_value())
            data.last_appended_codec_configuration = MUST(FixedArray<u8>::create(configuration.value()));
    };

    // A frame continues the run of the last appended frame when it decodes no earlier than that frame, without a gap
    // that would make the two sides separately decodable.
    auto run_to_continue = data.run_ending_with_last_appended_frame;
    if (run_to_continue.has_value()) {
        auto delta = frame.decode_timestamp() - data.runs[run_to_continue.value()].frames.last().decode_timestamp();
        if (delta < AK::Duration::zero() || delta > maximum_time_range_gap(data))
            run_to_continue.clear();
    }

    // AD-HOC: Coded frame eviction can remove the random access point that a frame decodes from after its
    //         coded frame group began, leaving the frame with nothing to be decoded from.
    if (!run_to_continue.has_value() && !frame.is_keyframe()) {
        update_last_appended_codec_configuration();
        return;
    }

    count_frame_duration(data, frame.duration());
    data.frame_intervals_to_add_to_ranges.append({ frame.presentation_timestamp(), frame.presentation_timestamp() + frame.duration() });

    if (run_to_continue.has_value()) {
        auto& run = data.runs[run_to_continue.value()];
        extend_run_bounds_for_frame(run, frame);
        update_last_appended_codec_configuration();
        data.total_bytes += buffered_byte_count(frame);
        run.frames.append(move(frame));
        note_run_start_changed(data, run_to_continue.value());
        restore_run_order(data);
        buffered_data_changed_while_locked();
        return;
    }

    FrameRun run;
    if (!frame.new_codec_configuration().has_value() && data.last_appended_codec_configuration.has_value())
        frame.set_new_codec_configuration(MUST(data.last_appended_codec_configuration->clone()));
    extend_run_bounds_for_frame(run, frame);
    update_last_appended_codec_configuration();
    data.total_bytes += buffered_byte_count(frame);
    run.frames.append(move(frame));

    auto insert_index = first_run_starting_at_or_after(data, run.presentation_start);
    data.runs.insert(insert_index, move(run));
    verify_runs_are_ordered_around_index(data, insert_index);
    if (insert_index <= data.current_run && !data.runs.is_empty())
        data.current_run = min(data.current_run + 1, data.runs.size() - 1);
    data.run_ending_with_last_appended_frame = insert_index;

    buffered_data_changed_while_locked();
}

// Delivery is in decode order, so a reordered frame's presentation can end after that of a later delivered frame.
void SourceBufferDemuxer::note_delivered_frame(TrackData& data, Media::CodedFrame const& frame)
{
    auto end = frame.presentation_timestamp() + frame.duration();
    if (!data.highest_delivered_presentation_end.has_value() || end > data.highest_delivered_presentation_end.value())
        data.highest_delivered_presentation_end = end;
}

void SourceBufferDemuxer::note_cursor_jumped(TrackData& data)
{
    if (data.cursor_continuity == CursorContinuity::Continuous)
        data.cursor_continuity = CursorContinuity::Jumped;
}

size_t SourceBufferDemuxer::first_run_starting_at_or_after(TrackData const& data, AK::Duration presentation_start)
{
    size_t low = 0;
    size_t high = data.runs.size();
    while (low < high) {
        auto middle = low + (high - low) / 2;
        if (data.runs[middle].presentation_start < presentation_start)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

void SourceBufferDemuxer::note_run_start_changed(TrackData& data, size_t run_index)
{
    auto start = data.runs[run_index].presentation_start;
    if (run_index > 0 && data.runs[run_index - 1].presentation_start > start)
        data.runs_need_reordering = true;
    if (run_index + 1 < data.runs.size() && start > data.runs[run_index + 1].presentation_start)
        data.runs_need_reordering = true;
}

// Operations walk the runs by index while changing them, so the order is only restored once they are done.
void SourceBufferDemuxer::restore_run_order(TrackData& data)
{
    if (!data.runs_need_reordering)
        return;
    data.runs_need_reordering = false;

    Vector<size_t> old_indices;
    old_indices.ensure_capacity(data.runs.size());
    for (size_t index = 0; index < data.runs.size(); index++)
        old_indices.unchecked_append(index);
    quick_sort(old_indices, [&](size_t a, size_t b) { return data.runs[a].presentation_start < data.runs[b].presentation_start; });

    Vector<FrameRun> runs;
    runs.ensure_capacity(data.runs.size());
    auto current_run = data.current_run;
    Optional<size_t> run_ending_with_last_appended_frame;
    for (size_t new_index = 0; new_index < old_indices.size(); new_index++) {
        auto old_index = old_indices[new_index];
        if (old_index == data.current_run)
            current_run = new_index;
        if (old_index == data.run_ending_with_last_appended_frame)
            run_ending_with_last_appended_frame = new_index;
        runs.unchecked_append(move(data.runs[old_index]));
    }
    data.runs = move(runs);
    data.current_run = current_run;
    data.run_ending_with_last_appended_frame = run_ending_with_last_appended_frame;
}

void SourceBufferDemuxer::verify_runs_are_ordered_around_index(TrackData const& data, size_t run_index)
{
    if (run_index > 0)
        VERIFY(data.runs[run_index - 1].presentation_start <= data.runs[run_index].presentation_start);
    if (run_index + 1 < data.runs.size())
        VERIFY(data.runs[run_index].presentation_start <= data.runs[run_index + 1].presentation_start);
}

void SourceBufferDemuxer::split_run(TrackData& data, size_t run_index, size_t split_at, FixedArray<u8> codec_configuration_before_tail)
{
    auto& run = data.runs[run_index];
    VERIFY(split_at > 0 && split_at < run.frames.size());

    FrameRun tail;
    for (size_t index = split_at; index < run.frames.size(); index++)
        tail.frames.append(move(run.frames[index]));
    run.frames.remove(split_at, run.frames.size() - split_at);
    if (!tail.frames.first().new_codec_configuration().has_value()) {
        data.total_bytes += codec_configuration_before_tail.size();
        tail.frames.first().set_new_codec_configuration(move(codec_configuration_before_tail));
    }
    recalculate_run_bounds(run);
    recalculate_run_bounds(tail);

    auto insert_index = run_index + 1;
    data.runs.insert(insert_index, move(tail));
    note_run_start_changed(data, run_index);
    note_run_start_changed(data, insert_index);
    if (data.current_run == run_index && data.current_frame >= split_at) {
        data.current_run = insert_index;
        data.current_frame -= split_at;
    } else if (data.current_run >= insert_index) {
        data.current_run++;
    }
    if (data.run_ending_with_last_appended_frame == run_index)
        data.run_ending_with_last_appended_frame = insert_index;
    else if (data.run_ending_with_last_appended_frame.has_value() && data.run_ending_with_last_appended_frame.value() >= insert_index)
        data.run_ending_with_last_appended_frame = data.run_ending_with_last_appended_frame.value() + 1;
}

bool SourceBufferDemuxer::removed_frames_overlap_group_of_pictures_being_read(FrameRun const& cursor_run, size_t cursor_frame_index, size_t first_removed_frame_index, size_t removed_frame_count)
{
    VERIFY(!cursor_run.frames.is_empty());
    auto last_frame_index = cursor_run.frames.size() - 1;
    auto frame_index_in_group = min(cursor_frame_index, last_frame_index);

    auto group_start_index = frame_index_in_group;
    while (group_start_index > 0 && !cursor_run.frames[group_start_index].is_keyframe())
        group_start_index--;
    auto group_end_index = frame_index_in_group + 1;
    while (group_end_index < cursor_run.frames.size() && !cursor_run.frames[group_end_index].is_keyframe())
        group_end_index++;

    return first_removed_frame_index < group_end_index && first_removed_frame_index + removed_frame_count > group_start_index;
}

size_t SourceBufferDemuxer::erase_frames_and_dependants(TrackData& data, size_t run_index, size_t first_frame, size_t minimum_frame_count)
{
    auto& run = data.runs[run_index];
    VERIFY(minimum_frame_count > 0);
    VERIFY(first_frame + minimum_frame_count <= run.frames.size());
    auto total_bytes_before_erasing = data.total_bytes;

    size_t bytes = 0;
    size_t frame_count = 0;
    size_t removed_frames_with_the_maximum_duration = 0;
    for (size_t index = first_frame; index < run.frames.size(); index++) {
        auto const& frame = run.frames[index];
        if (frame_count >= minimum_frame_count && frame.is_keyframe())
            break;
        if (data.highest_delivered_presentation_end.has_value() && frame.presentation_timestamp() < data.highest_delivered_presentation_end.value())
            m_frame_intervals_to_invalidate.append({ frame.presentation_timestamp(), frame.presentation_timestamp() + frame.duration() });
        bytes += buffered_byte_count(frame);
        if (frame.duration() == data.maximum_frame_duration)
            removed_frames_with_the_maximum_duration++;
        frame_count++;
    }

    FixedArray<u8> codec_configuration_before_remaining_frames;
    if (first_frame + frame_count < run.frames.size()) {
        auto configuration = codec_configuration_after_frame_prefix(run, first_frame + frame_count);
        if (configuration.has_value())
            codec_configuration_before_remaining_frames = MUST(FixedArray<u8>::create(*configuration));
    }

    // The frames replacing the group being read may land in another run, so reads must find them by time.
    if (data.current_run == run_index && removed_frames_overlap_group_of_pictures_being_read(run, data.current_frame, first_frame, frame_count))
        data.cursor_continuity = CursorContinuity::NeedsReanchoring;

    if (data.run_ending_with_last_appended_frame == run_index && first_frame + frame_count == run.frames.size())
        data.run_ending_with_last_appended_frame.clear();

    data.total_bytes -= bytes;
    run.frames.remove(first_frame, frame_count);
    data.track_buffer_ranges_need_rebuild = true;
    decrement_frames_with_maximum_duration(data, removed_frames_with_the_maximum_duration);

    if (data.current_run == run_index) {
        if (data.current_frame >= first_frame + frame_count) {
            data.current_frame -= frame_count;
        } else if (data.current_frame >= first_frame) {
            data.current_frame = first_frame;
            note_cursor_jumped(data);
        }
    }

    if (run.frames.is_empty()) {
        data.runs.remove(run_index);
        if (data.run_ending_with_last_appended_frame.has_value() && data.run_ending_with_last_appended_frame.value() > run_index)
            data.run_ending_with_last_appended_frame = data.run_ending_with_last_appended_frame.value() - 1;
        if (data.current_run > run_index)
            data.current_run--;
        else if (data.current_run == run_index)
            data.cursor_continuity = CursorContinuity::NeedsReanchoring;
        return saturating_sub(total_bytes_before_erasing, data.total_bytes);
    }

    if (first_frame == 0) {
        if (!run.frames.first().new_codec_configuration().has_value()) {
            data.total_bytes += codec_configuration_before_remaining_frames.size();
            run.frames.first().set_new_codec_configuration(move(codec_configuration_before_remaining_frames));
        }
    } else if (first_frame < run.frames.size()) {
        VERIFY(run.frames[first_frame].is_keyframe());
        split_run(data, run_index, first_frame, move(codec_configuration_before_remaining_frames));
        return saturating_sub(total_bytes_before_erasing, data.total_bytes);
    }
    recalculate_run_bounds(run);
    note_run_start_changed(data, run_index);
    return saturating_sub(total_bytes_before_erasing, data.total_bytes);
}

void SourceBufferDemuxer::remove_coded_frames_and_dependants_in_range(Media::Track const& track, AK::Duration start, AK::Duration end)
{
    (void)remove_coded_frames_and_dependants_in_range_returning_presentation_timestamp_at(track, start, end, {});
}

Optional<AK::Duration> SourceBufferDemuxer::remove_coded_frames_and_dependants_in_range_returning_presentation_timestamp_at(Media::Track const& track, AK::Duration start, AK::Duration end, Optional<AK::Duration> last_decode_timestamp)
{
    MutexLocker locker { m_mutex };
    auto& data = track_data(track);

    Optional<AK::Duration> removed_frame_presentation_timestamp;

    for (size_t run_index = data.runs.size(); run_index-- > 0;) {
        auto& run = data.runs[run_index];
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
        }

        erase_frames_and_dependants(data, run_index, first_index.value(), last_index - first_index.value() + 1);
    }

    restore_run_order(data);
    buffered_data_changed_while_locked();

    return removed_frame_presentation_timestamp;
}

size_t SourceBufferDemuxer::total_bytes(Media::Track const& track) const
{
    MutexLocker locker { m_mutex };
    return track_data(track).total_bytes;
}

bool SourceBufferDemuxer::is_frame_evictable(TrackData const& data, Media::CodedFrame const& frame, AK::Duration current_time)
{
    auto time_range_start = current_time;
    auto time_range_end = current_time;
    if (data.cursor_presentation_timestamp.has_value()) {
        time_range_start = min(time_range_start, data.cursor_presentation_timestamp.value());
        time_range_end = max(time_range_end, data.cursor_presentation_timestamp.value());
    }
    return frame.presentation_timestamp() < time_range_start || frame.presentation_timestamp() > time_range_end;
}

bool SourceBufferDemuxer::run_ends_at_last_appended_frame(TrackData const& data, size_t run_index)
{
    return data.run_ending_with_last_appended_frame == run_index;
}

template<typename Callback>
void SourceBufferDemuxer::for_each_earliest_evictable_group(TrackData const& data, AK::Duration current_time, Callback callback)
{
    size_t bytes = 0;
    for (size_t run_index = 0; run_index < data.runs.size(); run_index++) {
        auto const& frames = data.runs[run_index].frames;

        // Eviction that stops within a run moves the last evicted configuration onto the first remaining frame.
        size_t last_evicted_configuration_size = 0;

        size_t group_start = 0;
        while (group_start < frames.size()) {
            size_t group_end = group_start + 1;
            while (group_end < frames.size() && !frames[group_end].is_keyframe())
                group_end++;

            size_t group_bytes = 0;
            Optional<size_t> group_configuration_size;
            for (size_t index = group_start; index < group_end; index++) {
                if (!is_frame_evictable(data, frames[index], current_time))
                    return;
                group_bytes += buffered_byte_count(frames[index]);
                if (auto configuration = frames[index].new_codec_configuration(); configuration.has_value())
                    group_configuration_size = configuration.value().size();
            }
            if (group_end == frames.size() && run_ends_at_last_appended_frame(data, run_index))
                return;

            bytes += group_bytes;
            if (group_configuration_size.has_value())
                last_evicted_configuration_size = group_configuration_size.value();

            auto freed_bytes = bytes;
            if (group_end < frames.size() && !frames[group_end].new_codec_configuration().has_value())
                freed_bytes -= last_evicted_configuration_size;
            if (callback(frames[group_start].presentation_timestamp(), freed_bytes) == IterationDecision::Break)
                return;
            group_start = group_end;
        }
    }
}

size_t SourceBufferDemuxer::evictable_bytes_when_taking_all_earliest_frames(Media::Track const& track, AK::Duration current_time) const
{
    MutexLocker locker { m_mutex };
    size_t bytes = 0;
    for_each_earliest_evictable_group(track_data(track), current_time, [&](AK::Duration, size_t freed_bytes) {
        bytes = freed_bytes;
        return IterationDecision::Continue;
    });
    return bytes;
}

Vector<SourceBufferDemuxer::EarliestEvictableGroup> SourceBufferDemuxer::earliest_evictable_groups(Media::Track const& track, AK::Duration current_time, size_t wanted_bytes) const
{
    MutexLocker locker { m_mutex };
    Vector<EarliestEvictableGroup> groups;
    for_each_earliest_evictable_group(track_data(track), current_time, [&](AK::Duration presentation_timestamp, size_t freed_bytes) {
        groups.append({ presentation_timestamp, freed_bytes });
        if (freed_bytes >= wanted_bytes)
            return IterationDecision::Break;
        return IterationDecision::Continue;
    });
    return groups;
}

size_t SourceBufferDemuxer::take_earliest_groups(Media::Track const& track, size_t group_count)
{
    MutexLocker locker { m_mutex };
    VERIFY(!m_reached_end_of_stream);
    auto& data = track_data(track);

    size_t bytes = 0;
    while (group_count > 0) {
        VERIFY(!data.runs.is_empty());
        auto const& frames = data.runs.first().frames;
        size_t frame_count = 0;
        while (group_count > 0 && frame_count < frames.size()) {
            frame_count++;
            while (frame_count < frames.size() && !frames[frame_count].is_keyframe())
                frame_count++;
            group_count--;
        }
        bytes += erase_frames_and_dependants(data, 0, 0, frame_count);
    }
    restore_run_order(data);
    buffered_data_changed_while_locked();
    return bytes;
}

Vector<SourceBufferDemuxer::LatestEvictableFrame> SourceBufferDemuxer::latest_evictable_frames(Media::Track const& track, AK::Duration current_time, size_t wanted_bytes) const
{
    MutexLocker locker { m_mutex };
    auto const& data = track_data(track);

    Vector<LatestEvictableFrame> evictable_frames;
    size_t bytes = 0;
    for (size_t run_index = data.runs.size(); run_index-- > 0;) {
        if (run_ends_at_last_appended_frame(data, run_index))
            break;
        auto const& frames = data.runs[run_index].frames;
        for (size_t frame_index = frames.size(); frame_index-- > 0;) {
            auto const& frame = frames[frame_index];
            if (!is_frame_evictable(data, frame, current_time))
                return evictable_frames;
            auto byte_count = buffered_byte_count(frame);
            evictable_frames.append({ frame.presentation_timestamp(), frame.decode_timestamp(), byte_count });
            bytes += byte_count;
            if (bytes >= wanted_bytes)
                return evictable_frames;
        }
    }
    return evictable_frames;
}

size_t SourceBufferDemuxer::take_latest_frames(Media::Track const& track, size_t frame_count)
{
    MutexLocker locker { m_mutex };
    VERIFY(!m_reached_end_of_stream);
    auto& data = track_data(track);

    size_t bytes = 0;
    while (frame_count > 0) {
        VERIFY(!data.runs.is_empty());
        auto run_index = data.runs.size() - 1;
        auto run_frame_count = data.runs[run_index].frames.size();
        auto count = min(frame_count, run_frame_count);
        bytes += erase_frames_and_dependants(data, run_index, run_frame_count - count, count);
        frame_count -= count;
    }
    restore_run_order(data);
    buffered_data_changed_while_locked();
    return bytes;
}

void SourceBufferDemuxer::set_reached_end_of_stream()
{
    MutexLocker locker { m_mutex };
    m_reached_end_of_stream = true;
    buffered_data_changed_while_locked();
}

void SourceBufferDemuxer::clear_reached_end_of_stream()
{
    MutexLocker locker { m_mutex };
    m_reached_end_of_stream = false;
    for (auto& data : m_tracks) {
        if (!data->delivered_end_of_stream)
            continue;
        data->delivered_end_of_stream = false;
        m_invalidated_ranges.add_range(data->highest_delivered_presentation_end.value_or(AK::Duration::zero()), AK::Duration::max());
    }
    buffered_data_changed_while_locked();
}

Media::DecoderErrorOr<void> SourceBufferDemuxer::create_context_for_track(Media::Track const&)
{
    return {};
}

Media::DecoderErrorOr<Vector<Media::Track>> SourceBufferDemuxer::get_tracks_for_type(Media::TrackType type)
{
    Vector<Media::Track> tracks;
    for (auto const& data : m_tracks) {
        if (data->track.type() == type)
            tracks.append(data->track);
    }
    return tracks;
}

Media::DecoderErrorOr<Optional<Media::Track>> SourceBufferDemuxer::get_preferred_track_for_type(Media::TrackType type)
{
    for (auto const& data : m_tracks) {
        if (data->track.type() == type)
            return Optional<Media::Track> { data->track };
    }
    return Optional<Media::Track> {};
}

void SourceBufferDemuxer::count_frame_duration(TrackData& data, AK::Duration duration)
{
    if (duration > data.maximum_frame_duration) {
        data.maximum_frame_duration = duration;
        data.frames_at_maximum_duration = 1;
        return;
    }
    if (duration == data.maximum_frame_duration)
        data.frames_at_maximum_duration++;
}

void SourceBufferDemuxer::decrement_frames_with_maximum_duration(TrackData& data, size_t count)
{
    VERIFY(data.frames_at_maximum_duration >= count);
    data.frames_at_maximum_duration -= count;

    if (data.frames_at_maximum_duration == 0) {
        data.maximum_frame_duration = AK::Duration::zero();
        for (auto const& run : data.runs) {
            for (auto const& frame : run.frames)
                count_frame_duration(data, frame.duration());
        }
    }
}

Optional<size_t> SourceBufferDemuxer::find_run_to_play_from_while_locked(TrackData const& data, AK::Duration timestamp) const
{
    if (data.runs.is_empty())
        return {};

    for (size_t index = 0; index < data.runs.size(); index++) {
        auto const& run = data.runs[index];

        if (run.presentation_start > timestamp) {
            // A hole no wider than the permitted gap size plays from the preceding run first.
            if (index > 0 && run.presentation_start - data.runs[index - 1].presentation_end <= maximum_time_range_gap(data))
                return index - 1;
            // Resolve seeks to the following run if they are within the permitted gap size.
            if (run.presentation_start - timestamp <= maximum_time_range_gap(data))
                return index;
            if (index == 0 && is_within_start_gap_allowance(timestamp, run.presentation_start))
                return index;
            return {};
        }

        if (timestamp < run.presentation_end)
            return index;
    }

    if (m_reached_end_of_stream)
        return data.runs.size() - 1;
    return {};
}

Optional<ReadonlyBytes> SourceBufferDemuxer::codec_configuration_at_position(TrackData const& data, size_t run_index, size_t frame_index)
{
    auto const& run = data.runs[run_index];
    VERIFY(frame_index < run.frames.size());
    return codec_configuration_after_frame_prefix(run, frame_index + 1);
}

bool SourceBufferDemuxer::may_read_frame_while_locked(TrackData const& data, Media::CodedFrame const& frame) const
{
    if (!contributes_to_buffered_ranges(data) || !data.read_anchor.has_value())
        return true;

    // Frames up to the anchor are reordered frames, or the ones that a seek's target is decoded from.
    auto anchor = data.read_anchor.value();
    if (frame.presentation_timestamp() <= anchor)
        return true;

    // A read may only move forward within the buffered range that it is already in. A seek target shortly before a
    // range plays from that range, as the track's own seeks do.
    auto range = m_buffered_ranges.range_at_or_after(anchor);
    if (!range.has_value())
        return false;
    if (range->start > anchor + maximum_time_range_gap(data) && !is_within_start_gap_allowance(anchor, m_buffered_ranges[0].start))
        return false;
    return frame.presentation_timestamp() < range->end;
}

Media::DecoderErrorOr<Media::CodedFrame> SourceBufferDemuxer::get_next_sample_for_track(Media::Track const& track)
{
    MutexLocker locker { m_mutex };
    auto& data = track_data(track);

    auto continue_into_next_run_while_locked = [&] {
        if (data.current_run + 1 >= data.runs.size())
            return false;
        auto const& current_run = data.runs[data.current_run];
        auto const& next_run = data.runs[data.current_run + 1];
        auto gap = next_run.presentation_start - current_run.presentation_end;
        if (gap > maximum_time_range_gap(data))
            return false;

        data.current_run++;
        data.current_frame = 0;
        note_cursor_jumped(data);

        return true;
    };

    bool notified_blocked = false;
    Optional<Media::DecoderError> error;
    while (true) {
        bool has_frame_at_cursor = false;
        if (data.cursor_continuity == CursorContinuity::NeedsReanchoring) {
            auto anchor = data.cursor_presentation_timestamp.value_or(AK::Duration::zero());
            if (move_cursor_to_presentation_time_while_locked(data, anchor))
                continue;
        } else if (data.current_run < data.runs.size() && data.current_frame < data.runs[data.current_run].frames.size()) {
            has_frame_at_cursor = true;
            if (may_read_frame_while_locked(data, data.runs[data.current_run].frames[data.current_frame]))
                break;
        } else if (continue_into_next_run_while_locked()) {
            continue;
        }
        if (data.aborted.load()) {
            error = Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Read aborted"sv);
            break;
        }
        // Once the stream has ended, a frame outside the buffered ranges will never become readable.
        if (m_reached_end_of_stream && (has_frame_at_cursor || data.current_run + 1 >= data.runs.size())) {
            error = Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "End of stream"sv);
            data.delivered_end_of_stream = true;
            break;
        }
        if (!notified_blocked) {
            notified_blocked = true;
            if (data.read_blocked_change_handler)
                data.read_blocked_change_handler(Media::ReadBlocked::Yes);
        }
        m_data_changed.wait();
    }

    if (notified_blocked && data.read_blocked_change_handler)
        data.read_blocked_change_handler(Media::ReadBlocked::No);

    if (error.has_value())
        return error.release_value();

    auto const& stored_frame = data.runs[data.current_run].frames[data.current_frame];
    if (!data.read_anchor.has_value() || stored_frame.presentation_timestamp() > data.read_anchor.value())
        data.read_anchor = stored_frame.presentation_timestamp();

    if (data.cursor_continuity == CursorContinuity::Jumped) {
        data.cursor_continuity = CursorContinuity::Continuous;
        auto configuration = codec_configuration_at_position(data, data.current_run, data.current_frame);
        auto configuration_is_new = configuration.has_value() && (!data.last_delivered_codec_configuration.has_value() || *configuration != data.last_delivered_codec_configuration->span());
        if (configuration_is_new)
            data.last_delivered_codec_configuration = MUST(FixedArray<u8>::create(*configuration));

        // The first frame of a run always includes a codec configuration, but we only want to emit it if it differs
        // from the config at the end of the last run.
        if (configuration_is_new || stored_frame.new_codec_configuration().has_value()) {
            Optional<FixedArray<u8>> new_codec_configuration;
            if (configuration_is_new)
                new_codec_configuration = MUST(data.last_delivered_codec_configuration->clone());
            Media::CodedFrame frame {
                stored_frame.codec_id(),
                stored_frame.presentation_timestamp(),
                stored_frame.decode_timestamp(),
                stored_frame.duration(),
                stored_frame.flags(),
                MUST(FixedArray<u8>::create(stored_frame.data())),
                move(new_codec_configuration),
            };
            data.cursor_presentation_timestamp = frame.presentation_timestamp();
            note_delivered_frame(data, frame);
            data.current_frame++;
            return frame;
        }
    } else if (auto configuration = stored_frame.new_codec_configuration(); configuration.has_value()) {
        data.last_delivered_codec_configuration = MUST(FixedArray<u8>::create(*configuration));
    }

    data.cursor_presentation_timestamp = stored_frame.presentation_timestamp();
    note_delivered_frame(data, stored_frame);
    data.current_frame++;
    return stored_frame;
}

AK::Duration SourceBufferDemuxer::select_fast_seek_target_for_track(Media::Track const& track, AK::Duration target, Media::SeekMode mode)
{
    MutexLocker locker { m_mutex };

    Optional<AK::Duration> best_timestamp;
    for (auto const& run : track_data(track).runs) {
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

bool SourceBufferDemuxer::move_cursor_to_presentation_time_while_locked(TrackData& data, AK::Duration timestamp)
{
    auto run_index = find_run_to_play_from_while_locked(data, timestamp);
    if (!run_index.has_value())
        return false;

    auto const& run = data.runs[run_index.value()];
    VERIFY(run.frames.first().is_keyframe());
    size_t keyframe_index = 0;
    for (size_t index = 1; index < run.frames.size(); index++) {
        auto const& frame = run.frames[index];
        if (!frame.is_keyframe() || frame.presentation_timestamp() > timestamp)
            continue;
        if (frame.presentation_timestamp() >= run.frames[keyframe_index].presentation_timestamp())
            keyframe_index = index;
    }

    data.current_run = run_index.value();
    data.current_frame = keyframe_index;
    data.cursor_presentation_timestamp = run.frames[data.current_frame].presentation_timestamp();
    data.cursor_continuity = CursorContinuity::Jumped;
    return true;
}

Media::DecoderErrorOr<Media::DemuxerSeekResult> SourceBufferDemuxer::seek_to_most_recent_keyframe(Media::Track const& track, AK::Duration timestamp, Media::DemuxerSeekOptions options)
{
    MutexLocker locker { m_mutex };
    auto& data = track_data(track);

    // Forget what the consumer was sent, so that the configuration in effect is delivered again.
    if (has_flag(options, Media::DemuxerSeekOptions::NeedCodecConfiguration))
        data.last_delivered_codec_configuration = {};

    while (true) {
        if (data.aborted.load())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Seek aborted"sv);

        if (move_cursor_to_presentation_time_while_locked(data, timestamp)) {
            data.read_anchor = timestamp;
            data.highest_delivered_presentation_end.clear();
            data.delivered_end_of_stream = false;
            m_data_changed.broadcast();
            return Media::DemuxerSeekResult::MovedPosition;
        }

        m_data_changed.wait();
    }
}

Media::DecoderErrorOr<AK::Duration> SourceBufferDemuxer::duration_of_track(Media::Track const&)
{
    return AK::Duration::zero();
}

Media::DecoderErrorOr<AK::Duration> SourceBufferDemuxer::total_duration()
{
    MutexLocker locker { m_mutex };
    return highest_end_time_while_locked();
}

Media::DemuxerScanState const& SourceBufferDemuxer::scan_state() const
{
    return m_scan_state;
}

void SourceBufferDemuxer::set_scan_state_change_handler(ScanStateChangeHandler handler)
{
    m_scan_state_change_handler = move(handler);
    MutexLocker locker { m_mutex };
    m_scan_state_change_handler_event_loop = &Core::EventLoop::current();
    // Deliver any state that was built up before a home event loop existed.
    queue_scan_state_change_dispatch_while_locked();
}

void SourceBufferDemuxer::queue_scan_state_change_dispatch_while_locked()
{
    if (m_scan_state_change_dispatch_pending)
        return;
    if (m_scan_state_change_handler_event_loop == nullptr)
        return;
    m_scan_state_change_dispatch_pending = true;
    m_scan_state_change_handler_event_loop->deferred_invoke([self = NonnullRefPtr(*this)] {
        Media::DemuxerScanState scan_state;
        Media::TimeRanges invalidated_ranges;
        {
            MutexLocker locker { self->m_mutex };
            self->m_scan_state_change_dispatch_pending = false;
            scan_state = self->scan_state_while_locked();
            invalidated_ranges = exchange(self->m_invalidated_ranges, {});
        }
        self->m_scan_state = move(scan_state);
        if (self->m_scan_state_change_handler)
            self->m_scan_state_change_handler(invalidated_ranges);
    });
}

void SourceBufferDemuxer::set_blocking_reads_aborted_for_track(Media::Track const& track)
{
    track_data(track).aborted.store(true);
    MutexLocker locker { m_mutex };
    m_data_changed.broadcast();
}

void SourceBufferDemuxer::reset_blocking_reads_aborted_for_track(Media::Track const& track)
{
    track_data(track).aborted.store(false);
}

void SourceBufferDemuxer::set_read_blocked_change_handler_for_track(Media::Track const& track, Media::ReadBlockedChangeHandler handler)
{
    MutexLocker locker { m_mutex };
    track_data(track).read_blocked_change_handler = move(handler);
}

}
