/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/IntegralMath.h>
#include <LibMedia/Containers/FrameScanTimeline.h>

namespace Media {

// Divides every sample rate MPEG audio and AAC can carry, so that the duration of a frame is a whole
// number of ticks whichever of them a stream was encoded at.
static constexpr u64 FRAME_DURATION_TIMEBASE = 28'224'000;

// A byte range of a stream whose frames have been walked, holding the time they were found to carry.
struct FrameScanTimeline::ScannedByteRange {
    size_t byte_start { 0 };
    AK::Duration time_start { AK::Duration::zero() };
    size_t last_scanned_byte { 0 };
    u64 duration_in_ticks { 0 };
};

struct FrameScanTimeline::ByteTimePoint {
    size_t byte { 0 };
    AK::Duration time { AK::Duration::zero() };
};

// The unwalked bytes between the end of walked frames and the next known point after them.
struct FrameScanTimeline::Gap {
    ByteTimePoint start;
    ByteTimePoint end;
};

struct FrameScanTimeline::FrameWalk {
    ByteTimePoint point;
    bool spans_target { false };
};

struct FrameScanTimeline::ByteRate {
    u64 ticks { 0 };
    u64 byte_count { 0 };
};

static u64 frame_duration_in_ticks(SampleDuration const& duration)
{
    return static_cast<u64>(duration.sample_count) * FRAME_DURATION_TIMEBASE / duration.sample_rate;
}

static AK::Duration duration_from_ticks(u64 ticks)
{
    return AK::Duration::from_time_units(static_cast<i64>(ticks), 1, FRAME_DURATION_TIMEBASE);
}

AK::Duration FrameScanTimeline::interpolate_timestamp_at_byte(size_t byte, ByteTimePoint const& left, ByteTimePoint const& right)
{
    if (byte <= left.byte)
        return left.time;
    if (byte >= right.byte)
        return right.time;
    if (right.time <= left.time)
        return left.time;

    auto time_span_in_ticks = static_cast<u64>((right.time - left.time).to_time_units(1, FRAME_DURATION_TIMEBASE));
    auto ticks = AK::multiply_divide(time_span_in_ticks, byte - left.byte, right.byte - left.byte);
    return left.time + duration_from_ticks(ticks);
}

// A timestamp at or past the right point lands on it rather than extrapolating beyond it.
size_t FrameScanTimeline::estimate_byte_for_timestamp(AK::Duration timestamp, ByteTimePoint const& left, ByteTimePoint const& right)
{
    if (timestamp >= right.time)
        return right.byte;
    VERIFY(timestamp >= left.time);

    auto estimated_byte = left.byte;
    if (right.time > left.time && right.byte > left.byte) {
        auto numerator = (timestamp - left.time).to_time_units(1, FRAME_DURATION_TIMEBASE);
        auto denominator = (right.time - left.time).to_time_units(1, FRAME_DURATION_TIMEBASE);
        if (denominator > 0) {
            auto byte_offset = AK::multiply_divide(right.byte - left.byte, static_cast<u64>(numerator), static_cast<u64>(denominator));
            estimated_byte = left.byte + static_cast<size_t>(byte_offset);
        }
    }
    return estimated_byte;
}

FrameScanTimeline::ByteTimePoint FrameScanTimeline::scanned_start(ScannedByteRange const& range)
{
    return { range.byte_start, range.time_start };
}

FrameScanTimeline::ByteTimePoint FrameScanTimeline::scanned_endpoint(ScannedByteRange const& range)
{
    return {
        range.last_scanned_byte,
        range.time_start + duration_from_ticks(range.duration_in_ticks),
    };
}

static Optional<MediaStream::ByteRange> find_containing_byte_range(Vector<MediaStream::ByteRange> const& byte_ranges, size_t byte_position)
{
    auto index = lower_bound_index(byte_ranges, byte_position, [](MediaStream::ByteRange const& range, size_t position) {
        return range.end <= position ? -1 : 1;
    });
    if (index >= byte_ranges.size() || byte_position < byte_ranges[index].start)
        return {};
    return byte_ranges[index];
}

FrameScanTimeline::FrameScanTimeline(NonnullOwnPtr<FrameScanSource> frames, size_t first_frame_position, AK::Duration duration, DurationSource duration_source, Optional<size_t> declared_byte_count)
    : m_frames(move(frames))
    , m_first_frame_position(first_frame_position)
    , m_duration_source(duration_source)
    , m_declared_byte_count(declared_byte_count)
    , m_declared_duration(duration)
    , m_duration(duration)
{
    m_scanned_ranges.append(origin_range());
}

FrameScanTimeline::~FrameScanTimeline() = default;

BufferedRangesScan FrameScanTimeline::buffered_time_ranges(NonnullRefPtr<MediaStreamCursor> const& cursor, Vector<MediaStream::ByteRange> const& byte_ranges, Optional<u64> file_size)
{
    MutexLocker locker { m_mutex };
    update_scanned_ranges(*cursor, byte_ranges, file_size);

    BufferedRangesScan scan;
    for (auto const& range : m_scanned_ranges) {
        scan.time_ranges.add_range(range.time_start, scanned_endpoint(range).time);
        if (range.duration_in_ticks > 0 && range.byte_start >= byte_ranges.last().start)
            scan.last_byte_range_has_samples = true;
    }
    return scan;
}

AK::Duration FrameScanTimeline::duration()
{
    MutexLocker locker { m_mutex };
    return m_duration;
}

DecoderErrorOr<SeekResult> FrameScanTimeline::seek_to_timestamp(NonnullRefPtr<MediaStreamCursor> const& scanning_cursor, NonnullRefPtr<MediaStreamCursor> const& seek_cursor, MediaStream& stream, AK::Duration timestamp)
{
    timestamp = max(timestamp, AK::Duration::zero());

    Gap gap;
    {
        MutexLocker locker { m_mutex };
        auto file_size = stream.expected_size();
        update_scanned_ranges(*scanning_cursor, stream.available_byte_ranges(), file_size);

        auto lookup = look_up_timestamp(*scanning_cursor, timestamp, file_size);
        if (auto const* landed = lookup.get_pointer<ByteTimePoint>())
            return seeked_position(*landed);
        gap = lookup.get<Gap>();
    }

    auto estimated_byte = estimate_byte_for_timestamp(timestamp, gap.start, gap.end);
    auto resynced_frame_byte = m_frames->find_boundary_at_or_before(*seek_cursor, estimated_byte, gap.start.byte, gap.end.byte).value_or(gap.start.byte);

    // The resync above has blocked until the data at the target is available. Now that the data is available, the
    // frames walked by the scan may have a different bitrate than we expected. Walk the frames from an established
    // point until the target to determine the exact byte representing the target timestamp with the new data present.
    Optional<ScannedByteRange> range;
    {
        MutexLocker locker { m_mutex };
        update_scanned_ranges(*scanning_cursor, stream.available_byte_ranges(), stream.expected_size());
        range = last_range_starting_at_or_before_byte(resynced_frame_byte);
    }
    if (!range.has_value())
        return seeked_position({ resynced_frame_byte, interpolate_timestamp_at_byte(resynced_frame_byte, gap.start, gap.end) });

    return seeked_position(walk_to_timestamp(*seek_cursor, walk_start_for(*range, timestamp), timestamp).point);
}

SeekedPosition FrameScanTimeline::seeked_position(ByteTimePoint const& point)
{
    return SeekedPosition { static_cast<i64>(point.byte), point.time };
}

FrameScanTimeline::ByteTimePoint FrameScanTimeline::origin_point() const
{
    return { m_first_frame_position, AK::Duration::zero() };
}

FrameScanTimeline::ScannedByteRange FrameScanTimeline::origin_range() const
{
    return {
        .byte_start = m_first_frame_position,
        .time_start = AK::Duration::zero(),
        .last_scanned_byte = m_first_frame_position,
        .duration_in_ticks = 0,
    };
}

void FrameScanTimeline::add_walked_frames(ByteRate& rate, ScannedByteRange const& range)
{
    rate.byte_count += range.last_scanned_byte - range.byte_start;
    rate.ticks += range.duration_in_ticks;
}

FrameScanTimeline::ByteRate FrameScanTimeline::walked_rate() const
{
    ByteRate rate;
    for (auto const& range : m_scanned_ranges)
        add_walked_frames(rate, range);
    return rate;
}

Optional<size_t> FrameScanTimeline::declared_byte_end(Optional<u64> file_size) const
{
    if (m_declared_byte_count.has_value())
        return m_first_frame_position + *m_declared_byte_count;
    if (file_size.has_value())
        return static_cast<size_t>(*file_size);
    return {};
}

FrameScanTimeline::ByteRate FrameScanTimeline::declared_rate(Optional<u64> file_size) const
{
    auto byte_end = declared_byte_end(file_size);
    if (!byte_end.has_value() || *byte_end <= m_first_frame_position)
        return {};
    return { static_cast<u64>(m_declared_duration.to_time_units(1, FRAME_DURATION_TIMEBASE)), *byte_end - m_first_frame_position };
}

// The rate the bytes of a gap are assumed to carry. Walked frames only inform it for a stream
// whose duration was estimated; a declared duration gives every byte the stream's average.
FrameScanTimeline::ByteRate FrameScanTimeline::rate_for_gap(ByteRate const& walked_before_gap, Optional<u64> file_size) const
{
    if (m_duration_source == DurationSource::Declared)
        return declared_rate(file_size);
    if (walked_before_gap.byte_count > 0)
        return walked_before_gap;
    if (auto rate = walked_rate(); rate.byte_count > 0)
        return rate;
    return declared_rate(file_size);
}

AK::Duration FrameScanTimeline::gap_duration(size_t byte_count, ByteRate const& rate)
{
    if (rate.byte_count == 0)
        return AK::Duration::zero();
    if (AK::multiply_divide_would_overflow(rate.ticks, byte_count, rate.byte_count))
        return AK::Duration::zero();
    auto ticks = AK::multiply_divide(rate.ticks, byte_count, rate.byte_count);
    return duration_from_ticks(ticks);
}

FrameScanTimeline::ByteTimePoint FrameScanTimeline::last_scanned_endpoint() const
{
    if (m_scanned_ranges.is_empty())
        return origin_point();
    return scanned_endpoint(m_scanned_ranges.last());
}

Optional<FrameScanTimeline::ByteTimePoint> FrameScanTimeline::end_anchor(Optional<u64> file_size) const
{
    if (!file_size.has_value())
        return {};
    auto last = last_scanned_endpoint();
    auto end_byte = max(static_cast<size_t>(*file_size), last.byte);
    return ByteTimePoint { end_byte, last.time + gap_duration(end_byte - last.byte, rate_for_gap(walked_rate(), file_size)) };
}

FrameScanTimeline::TimestampLookup FrameScanTimeline::look_up_timestamp(MediaStreamCursor& cursor, AK::Duration target, Optional<u64> file_size) const
{
    Optional<size_t> index;
    for (size_t i = 0; i < m_scanned_ranges.size(); i++) {
        if (m_scanned_ranges[i].time_start > target)
            break;
        index = i;
    }

    auto scanned_end = origin_point();
    if (index.has_value()) {
        auto walk = walk_to_timestamp(cursor, walk_start_for(m_scanned_ranges[*index], target), target);
        if (walk.spans_target)
            return walk.point;
        scanned_end = walk.point;
    }

    auto next_index = index.has_value() ? *index + 1 : 0;
    Optional<ByteTimePoint> gap_end;
    if (next_index < m_scanned_ranges.size())
        gap_end = scanned_start(m_scanned_ranges[next_index]);
    else
        gap_end = end_anchor(file_size);

    if (!gap_end.has_value() || scanned_end.byte >= gap_end->byte)
        return scanned_end;
    return Gap { scanned_end, *gap_end };
}

Optional<FrameScanTimeline::ScannedByteRange> FrameScanTimeline::last_range_starting_at_or_before_byte(size_t byte) const
{
    Optional<ScannedByteRange> found;
    for (auto const& range : m_scanned_ranges) {
        if (range.byte_start > byte)
            break;
        found = range;
    }
    return found;
}

FrameScanTimeline::ByteTimePoint FrameScanTimeline::walk_start_for(ScannedByteRange const& range, AK::Duration target)
{
    auto end = scanned_endpoint(range);
    if (target >= end.time)
        return end;
    return scanned_start(range);
}

FrameScanTimeline::FrameWalk FrameScanTimeline::walk_to_timestamp(MediaStreamCursor& cursor, ByteTimePoint const& start, AK::Duration target) const
{
    auto target_duration_in_ticks = (target - start.time).to_time_units(1, FRAME_DURATION_TIMEBASE);
    u64 duration_in_ticks = 0;
    auto position = start.byte;
    while (true) {
        auto frame = m_frames->next_frame(cursor, position);
        if (!frame.has_value())
            break;

        auto frame_ticks = frame_duration_in_ticks(frame->duration);
        if (static_cast<i64>(duration_in_ticks + frame_ticks) > target_duration_in_ticks)
            return { { frame->position, start.time + duration_from_ticks(duration_in_ticks) }, true };
        duration_in_ticks += frame_ticks;
        position = frame->end_position();
    }
    return { { position, start.time + duration_from_ticks(duration_in_ticks) }, false };
}

void FrameScanTimeline::scan_frames_into_range(MediaStreamCursor& cursor, ScannedByteRange& range, size_t limit)
{
    while (true) {
        auto frame = m_frames->next_frame(cursor, range.last_scanned_byte);
        if (!frame.has_value())
            break;
        if (frame->end_position() > limit) {
            range.last_scanned_byte = min(frame->position, limit);
            break;
        }
        range.duration_in_ticks += frame_duration_in_ticks(frame->duration);
        range.last_scanned_byte = frame->end_position();
    }
}

void FrameScanTimeline::update_scanned_ranges(MediaStreamCursor& cursor, Vector<MediaStream::ByteRange> const& byte_ranges, Optional<u64> file_size)
{
    drop_ranges_whose_bytes_are_gone(byte_ranges);
    create_ranges_for_unscanned_bytes(cursor, byte_ranges);
    walk_ranges(cursor, byte_ranges);
    derive_range_times(file_size);
    m_duration = max(m_duration, reported_duration(file_size));
}

void FrameScanTimeline::drop_ranges_whose_bytes_are_gone(Vector<MediaStream::ByteRange> const& byte_ranges)
{
    for (size_t i = 0; i < m_scanned_ranges.size();) {
        auto& range = m_scanned_ranges[i];
        auto byte_range = find_containing_byte_range(byte_ranges, range.byte_start);
        if (!byte_range.has_value()) {
            m_scanned_ranges.remove(i);
            continue;
        }
        if (range.last_scanned_byte > byte_range->end) {
            range.last_scanned_byte = range.byte_start;
            range.duration_in_ticks = 0;
        }
        i++;
    }
}

// A range that walks up to the next one takes over its frames, so that contiguous bytes are
// always one range.
void FrameScanTimeline::walk_ranges(MediaStreamCursor& cursor, Vector<MediaStream::ByteRange> const& byte_ranges)
{
    for (size_t i = 0; i < m_scanned_ranges.size(); i++) {
        auto byte_range = find_containing_byte_range(byte_ranges, m_scanned_ranges[i].byte_start);
        VERIFY(byte_range.has_value());

        while (true) {
            auto& range = m_scanned_ranges[i];
            auto next_index = i + 1;
            auto limit = byte_range->end;
            if (next_index < m_scanned_ranges.size())
                limit = min(limit, m_scanned_ranges[next_index].byte_start);
            scan_frames_into_range(cursor, range, limit);

            if (next_index >= m_scanned_ranges.size() || range.last_scanned_byte < m_scanned_ranges[next_index].byte_start)
                break;

            auto const& absorbed = m_scanned_ranges[next_index];
            range.duration_in_ticks += absorbed.duration_in_ticks;
            range.last_scanned_byte = absorbed.last_scanned_byte;
            m_scanned_ranges.remove(next_index);
        }
    }
}

// Bytes ahead of the first range within them can arrive too, since a resync reads back from its target.
void FrameScanTimeline::create_ranges_for_unscanned_bytes(MediaStreamCursor& cursor, Vector<MediaStream::ByteRange> const& byte_ranges)
{
    for (auto const& byte_range : byte_ranges) {
        auto first_range_index = lower_bound_index(m_scanned_ranges, byte_range.start, [](ScannedByteRange const& range, size_t position) {
            return range.byte_start < position ? -1 : 1;
        });

        auto search_start = max(byte_range.start, m_first_frame_position);
        auto search_end = byte_range.end;
        if (first_range_index < m_scanned_ranges.size())
            search_end = min(search_end, m_scanned_ranges[first_range_index].byte_start);
        if (search_start >= search_end)
            continue;

        auto frame_boundary = m_frames->find_boundary_at_or_after(cursor, search_start, search_end);
        if (!frame_boundary.has_value())
            continue;

        ScannedByteRange range {
            .byte_start = *frame_boundary,
            .time_start = AK::Duration::zero(),
            .last_scanned_byte = *frame_boundary,
            .duration_in_ticks = 0,
        };
        m_scanned_ranges.insert(first_range_index, range);
    }
}

void FrameScanTimeline::derive_range_times(Optional<u64> file_size)
{
    auto previous_end = origin_point();
    ByteRate walked_before;
    for (auto& range : m_scanned_ranges) {
        range.time_start = previous_end.time + gap_duration(range.byte_start - previous_end.byte, rate_for_gap(walked_before, file_size));
        previous_end = scanned_endpoint(range);
        add_walked_frames(walked_before, range);
    }
}

// A declared duration is trusted over any estimate of the bytes it describes, and only raised
// by frames walked past it or by bytes that lie beyond them.
AK::Duration FrameScanTimeline::reported_duration(Optional<u64> file_size) const
{
    auto last = last_scanned_endpoint();
    if (m_duration_source == DurationSource::Estimated) {
        auto anchor = end_anchor(file_size);
        return anchor.has_value() ? anchor->time : last.time;
    }

    auto duration = max(m_declared_duration, last.time);
    if (!file_size.has_value())
        return duration;

    auto described_byte_end = max(declared_byte_end(file_size).value(), last.byte);
    if (*file_size <= described_byte_end)
        return duration;
    return duration + gap_duration(static_cast<size_t>(*file_size) - described_byte_end, declared_rate(file_size));
}

}
