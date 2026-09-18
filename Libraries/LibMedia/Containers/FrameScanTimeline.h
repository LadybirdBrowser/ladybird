/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibMedia/Containers/ContainerNavigator.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

struct SampleDuration {
    u32 sample_count { 0 };
    u32 sample_rate { 0 };
};

// The frames of a container format, as everything a timeline needs of them.
class FrameScanSource {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct Frame {
        size_t position { 0 };
        size_t byte_size { 0 };
        SampleDuration duration;

        size_t end_position() const { return position + byte_size; }
    };

    virtual ~FrameScanSource() = default;

    // Reads the frame at the position, skipping over bytes that head no frame.
    virtual Optional<Frame> next_frame(MediaStreamCursor&, size_t position) const = 0;

    virtual Optional<size_t> find_boundary_at_or_after(MediaStreamCursor&, size_t start_byte, size_t upper_bound) const = 0;
    virtual Optional<size_t> find_boundary_at_or_before(MediaStreamCursor&, size_t target_byte, size_t lower_bound, size_t upper_bound) const = 0;
};

enum class DurationSource : u8 {
    Declared,
    Estimated,
};

// Maps byte positions onto timestamps by walking the frames of a stream. Between walked ranges, time runs at a rate
// the bytes are assumed to carry: the rate of the frames walked before the gap, or the average a declared duration
// gives the whole stream. Every range's start is derived from the ranges before it, so a range only moves when one
// of those grows, and the one being appended to never does.
class MEDIA_API FrameScanTimeline {
public:
    AK_ALLOC_WITH_KMALLOC;

    FrameScanTimeline(NonnullOwnPtr<FrameScanSource>, size_t first_frame_position, AK::Duration duration, DurationSource, Optional<size_t> declared_byte_count);
    ~FrameScanTimeline();

    BufferedRangesScan buffered_time_ranges(NonnullRefPtr<MediaStreamCursor> const& cursor, Vector<MediaStream::ByteRange> const& byte_ranges, Optional<u64> file_size);
    AK::Duration duration();
    DecoderErrorOr<SeekResult> seek_to_timestamp(NonnullRefPtr<MediaStreamCursor> const& scanning_cursor, NonnullRefPtr<MediaStreamCursor> const& seek_cursor, MediaStream&, AK::Duration timestamp);

private:
    struct ScannedByteRange;
    struct ByteTimePoint;
    struct Gap;
    struct FrameWalk;
    struct ByteRate;

    // Either the point a seek lands on, or the gap it has to resync within.
    using TimestampLookup = Variant<ByteTimePoint, Gap>;

    static AK::Duration interpolate_timestamp_at_byte(size_t byte, ByteTimePoint const& left, ByteTimePoint const& right);
    static size_t estimate_byte_for_timestamp(AK::Duration timestamp, ByteTimePoint const& left, ByteTimePoint const& right);
    static ByteTimePoint scanned_start(ScannedByteRange const&);
    static ByteTimePoint scanned_endpoint(ScannedByteRange const&);
    static SeekedPosition seeked_position(ByteTimePoint const&);
    static void add_walked_frames(ByteRate&, ScannedByteRange const&);
    static AK::Duration gap_duration(size_t byte_count, ByteRate const&);
    static ByteTimePoint walk_start_for(ScannedByteRange const&, AK::Duration target);

    ByteTimePoint origin_point() const;
    ScannedByteRange origin_range() const;
    ByteRate walked_rate() const;
    Optional<size_t> declared_byte_end(Optional<u64> file_size) const;
    ByteRate declared_rate(Optional<u64> file_size) const;
    ByteRate rate_for_gap(ByteRate const& walked_before_gap, Optional<u64> file_size) const;
    ByteTimePoint last_scanned_endpoint() const;
    Optional<ByteTimePoint> end_anchor(Optional<u64> file_size) const;
    TimestampLookup look_up_timestamp(MediaStreamCursor&, AK::Duration target, Optional<u64> file_size) const;
    Optional<ScannedByteRange> last_range_starting_at_or_before_byte(size_t byte) const;
    FrameWalk walk_to_timestamp(MediaStreamCursor&, ByteTimePoint const& start, AK::Duration target) const;
    void scan_frames_into_range(MediaStreamCursor&, ScannedByteRange&, size_t limit);
    void update_scanned_ranges(MediaStreamCursor&, Vector<MediaStream::ByteRange> const&, Optional<u64> file_size);
    void drop_ranges_whose_bytes_are_gone(Vector<MediaStream::ByteRange> const&);
    void walk_ranges(MediaStreamCursor&, Vector<MediaStream::ByteRange> const&);
    void create_ranges_for_unscanned_bytes(MediaStreamCursor&, Vector<MediaStream::ByteRange> const&);
    void derive_range_times(Optional<u64> file_size);
    AK::Duration reported_duration(Optional<u64> file_size) const;

    NonnullOwnPtr<FrameScanSource> m_frames;

    size_t m_first_frame_position { 0 };
    DurationSource m_duration_source { DurationSource::Estimated };
    Optional<size_t> m_declared_byte_count;
    AK::Duration m_declared_duration;
    AK::Duration m_duration;

    Mutex m_mutex;
    Vector<ScannedByteRange> m_scanned_ranges;
};

}
