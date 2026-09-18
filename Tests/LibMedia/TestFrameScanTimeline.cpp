/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibMedia/Containers/FrameScanTimeline.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>

// A stream of fixed-size frames, so that a byte position and a timestamp convert to each other by
// arithmetic the test can state outright. Only reading a frame consults the stream, so what a scan
// can reach is decided by which bytes have arrived, the way it is for a real container.
namespace {

constexpr size_t FRAME_BYTE_SIZE = 100;
constexpr size_t FRAME_COUNT = 1000;
constexpr size_t FILE_BYTE_SIZE = FRAME_BYTE_SIZE * FRAME_COUNT;

// Frames within these bytes carry twice the time for the same size, so that a test can give a
// stream a rate that differs from what the rest of it would predict.
struct DoubledFrameBytes {
    size_t start { 0 };
    size_t end { 0 };
};
Optional<DoubledFrameBytes> s_doubled_frame_bytes;

// Bytes within these hold no frames, the way a tag can sit between two concatenated streams or after
// the audio. Reading a frame among them finds the first one after them, as a resynchronizing reader
// does once it has read through them.
struct NonFrameBytes {
    size_t start { 0 };
    size_t end { 0 };

    bool contains(size_t position) const { return position >= start && position < end; }
};
Optional<NonFrameBytes> s_non_frame_bytes;

bool is_non_frame_byte(size_t position)
{
    return s_non_frame_bytes.has_value() && s_non_frame_bytes->contains(position);
}

// Runs while a seek resolves its frame boundary, standing in for the blocking read that brings the
// bytes around the target into the stream, and for the scan thread running in the meantime.
Function<void(size_t target_byte)> s_on_resync;

// Runs when the seek cursor reads a frame that has not arrived, standing in for a blocking read
// waiting for its bytes.
Media::MediaStreamCursor* s_blocking_cursor { nullptr };
Function<void(size_t position)> s_on_blocked_read;

// What a real resync reads: a chunk back from the target and the frames that verify a boundary.
constexpr size_t RESYNC_BYTES_BEFORE_TARGET = 4096;
constexpr size_t RESYNC_BYTES_AFTER_TARGET = 400;

class FixedSizeFrames final : public Media::FrameScanSource {
public:
    virtual Optional<Frame> next_frame(Media::MediaStreamCursor& cursor, size_t position) const override
    {
        auto bytes_are_readable = [&](size_t start, size_t end) {
            if (cursor.seek(start, AK::SeekMode::SetPosition).is_error())
                return false;
            auto data = cursor.read_bytes(end - start);
            return !data.is_error() && data.value().size() == end - start;
        };
        auto whole_frame_is_readable = [&] { return bytes_are_readable(position, position + FRAME_BYTE_SIZE); };

        if (is_non_frame_byte(position)) {
            if (!bytes_are_readable(position, s_non_frame_bytes->end))
                return {};
            position = s_non_frame_bytes->end;
        }

        auto readable = whole_frame_is_readable();
        if (!readable && &cursor == s_blocking_cursor && s_on_blocked_read) {
            s_on_blocked_read(position);
            readable = whole_frame_is_readable();
        }
        if (!readable)
            return {};

        auto sample_count = 1152u;
        if (s_doubled_frame_bytes.has_value() && position >= s_doubled_frame_bytes->start && position < s_doubled_frame_bytes->end)
            sample_count *= 2;
        return Frame { position, FRAME_BYTE_SIZE, Media::SampleDuration { sample_count, 48000 } };
    }

    virtual Optional<size_t> find_boundary_at_or_after(Media::MediaStreamCursor&, size_t start_byte, size_t upper_bound) const override
    {
        auto boundary = (start_byte + FRAME_BYTE_SIZE - 1) / FRAME_BYTE_SIZE * FRAME_BYTE_SIZE;
        if (is_non_frame_byte(boundary))
            boundary = s_non_frame_bytes->end;
        if (boundary + FRAME_BYTE_SIZE > upper_bound)
            return {};
        return boundary;
    }

    virtual Optional<size_t> find_boundary_at_or_before(Media::MediaStreamCursor&, size_t target_byte, size_t lower_bound, size_t upper_bound) const override
    {
        if (s_on_resync)
            s_on_resync(target_byte);

        auto boundary = min(target_byte, upper_bound) / FRAME_BYTE_SIZE * FRAME_BYTE_SIZE;
        while (boundary + FRAME_BYTE_SIZE > upper_bound && boundary >= FRAME_BYTE_SIZE)
            boundary -= FRAME_BYTE_SIZE;
        if (is_non_frame_byte(boundary))
            boundary = s_non_frame_bytes->start - FRAME_BYTE_SIZE;
        if (boundary < lower_bound)
            return {};
        return boundary;
    }
};

struct Fixture {
    Fixture(AK::Duration duration, Media::DurationSource duration_source = Media::DurationSource::Estimated, Optional<size_t> declared_byte_count = {}, size_t file_byte_size = FILE_BYTE_SIZE)
        : file_byte_size(file_byte_size)
        , stream(Media::IncrementallyPopulatedStream::create_empty())
        , scan_cursor(stream->create_cursor())
        , seek_cursor(stream->create_cursor())
        , timeline(make<FixedSizeFrames>(), 0, duration, duration_source, declared_byte_count)
    {
        stream->set_expected_size(file_byte_size);
        scan_cursor->set_is_blocking(false);
        seek_cursor->set_is_blocking(false);

        s_blocking_cursor = seek_cursor.ptr();
        s_on_resync = [this](size_t target_byte) {
            arrive(target_byte - min(target_byte, RESYNC_BYTES_BEFORE_TARGET), min(target_byte + RESYNC_BYTES_AFTER_TARGET, this->file_byte_size));
        };
        s_on_blocked_read = [this](size_t position) {
            if (position + FRAME_BYTE_SIZE <= this->file_byte_size)
                arrive(position, position + FRAME_BYTE_SIZE);
        };
    }

    ~Fixture()
    {
        s_blocking_cursor = nullptr;
        s_on_resync = nullptr;
        s_on_blocked_read = nullptr;
    }

    void arrive(size_t start, size_t end)
    {
        auto bytes = MUST(ByteBuffer::create_zeroed(end - start));
        stream->add_chunk_at(start, bytes.bytes());
    }

    Media::TimeRanges scan()
    {
        return timeline.buffered_time_ranges(scan_cursor, stream->available_byte_ranges(), file_byte_size).time_ranges;
    }

    Media::SeekedPosition seek_to(AK::Duration timestamp)
    {
        auto result = MUST(timeline.seek_to_timestamp(scan_cursor, seek_cursor, *stream, timestamp));
        auto const* seeked = result.get_pointer<Media::SeekedPosition>();
        VERIFY(seeked != nullptr);
        return *seeked;
    }

    size_t file_byte_size { FILE_BYTE_SIZE };
    NonnullRefPtr<Media::IncrementallyPopulatedStream> stream;
    NonnullRefPtr<Media::MediaStreamCursor> scan_cursor;
    NonnullRefPtr<Media::MediaStreamCursor> seek_cursor;
    Media::FrameScanTimeline timeline;
};

void expect_range(Media::TimeRanges const& ranges, size_t index, i64 start_milliseconds, i64 end_milliseconds)
{
    EXPECT(index < ranges.size());
    if (index >= ranges.size())
        return;
    EXPECT_EQ(ranges[index].start, AK::Duration::from_milliseconds(start_milliseconds));
    EXPECT_EQ(ranges[index].end, AK::Duration::from_milliseconds(end_milliseconds));
}

void expect_same_range(Media::TimeRanges const& ranges, size_t index, Media::TimeRanges const& expected_ranges, size_t expected_index)
{
    EXPECT(index < ranges.size() && expected_index < expected_ranges.size());
    if (index >= ranges.size() || expected_index >= expected_ranges.size())
        return;
    EXPECT_EQ(ranges[index].start, expected_ranges[expected_index].start);
    EXPECT_EQ(ranges[index].end, expected_ranges[expected_index].end);
}

}

TEST_CASE(a_range_the_scan_creates_is_placed_at_the_rate_walked_before_it)
{
    // The stream is 24 seconds long, but only 2 were estimated from its first frames.
    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.arrive(50'000, 60'000);

    // The 10000 walked bytes carry 2.4 seconds, so the 40000 bytes of the gap are given 9.6.
    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);
    expect_range(ranges, 0, 0, 2'400);
    expect_range(ranges, 1, 12'000, 14'400);
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(24'000));
}

TEST_CASE(a_range_keeps_its_start_while_it_grows)
{
    // The second half of the stream carries twice the time per byte, so that growing the second
    // range would move it if what it is worth were measured against itself.
    s_doubled_frame_bytes = DoubledFrameBytes { 50'000, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.arrive(50'000, 60'000);
    auto before_growth = fixture.scan();
    expect_range(before_growth, 1, 12'000, 16'800);

    fixture.arrive(60'000, 70'000);
    auto after_growth = fixture.scan();
    EXPECT_EQ(after_growth.size(), 2u);
    expect_range(after_growth, 1, 12'000, 21'600);
}

TEST_CASE(a_duration_already_reported_is_never_lowered)
{
    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.arrive(50'000, 60'000);
    fixture.scan();

    auto reported = fixture.timeline.duration();
    EXPECT(reported > AK::Duration::zero());

    fixture.stream->remove_byte_range(50'000, 60'000);
    fixture.scan();
    EXPECT_EQ(fixture.timeline.duration(), reported);
}

TEST_CASE(a_seek_into_a_gap_lands_between_the_ranges_around_it)
{
    s_doubled_frame_bytes = DoubledFrameBytes { 50'000, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.arrive(50'000, 60'000);
    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(8'000));
    EXPECT(seeked.timestamp > ranges[0].end);
    EXPECT(seeked.timestamp < ranges[1].start);
}

TEST_CASE(a_seek_into_a_gap_hours_long_lands_on_the_line_through_it)
{
    // Ten hours of frames, whose byte and tick spans multiply past what 64 bits can hold.
    Fixture fixture { AK::Duration::from_milliseconds(2'000), Media::DurationSource::Estimated, {}, 150'000'000 };
    fixture.arrive(0, 10'000);
    fixture.scan();
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_seconds(36'000));

    auto seeked = fixture.seek_to(AK::Duration::from_seconds(18'000));
    EXPECT_EQ(seeked.byte_position, 75'000'000);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_seconds(18'000));
}

TEST_CASE(a_seek_lands_on_the_frame_the_arriving_bytes_put_under_its_target)
{
    s_doubled_frame_bytes = DoubledFrameBytes { 50'000, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(24'000) };
    fixture.arrive(0, 60'000);
    fixture.scan();

    // Resolving the boundary reads backwards from its estimate, so bytes arrive from before the
    // target. The range they form starts on the line, and its frames carry more than the line gave
    // them, so the target falls a frame or two before the estimated byte.
    s_on_resync = [&fixture](size_t target_byte) { fixture.arrive(target_byte - 300, FILE_BYTE_SIZE); };

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(22'800));
    EXPECT_EQ(seeked.byte_position, 81'300);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(22'784));

    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);
    expect_range(ranges, 1, 22'736, 31'760);
}

TEST_CASE(seeking_twice_to_the_same_timestamp_lands_on_the_same_byte)
{
    s_doubled_frame_bytes = DoubledFrameBytes { 50'000, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(24'000) };
    fixture.arrive(0, 60'000);
    fixture.scan();

    s_on_resync = [&fixture](size_t target_byte) { fixture.arrive(target_byte - 300, FILE_BYTE_SIZE); };

    // A seek is dispatched on both mouse down and mouse up, so the same target has to resolve to
    // the same byte even though the first one moved what the timeline knows.
    auto target = AK::Duration::from_milliseconds(22'800);
    auto first = fixture.seek_to(target);
    fixture.scan();

    auto second = fixture.seek_to(target);
    EXPECT_EQ(second.byte_position, first.byte_position);
    EXPECT_EQ(second.timestamp, first.timestamp);
}

TEST_CASE(seeking_forward_into_a_gap_moves_no_other_range)
{
    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.arrive(50'000, 60'000);
    fixture.scan();

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(20'000));
    EXPECT_EQ(seeked.byte_position, 83'300);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(19'992));

    fixture.arrive(83'300, 90'000);
    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 3u);
    expect_range(ranges, 0, 0, 2'400);
    expect_range(ranges, 1, 12'000, 14'400);
    expect_range(ranges, 2, 19'032, 21'600);
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(24'000));

    seeked = fixture.seek_to(AK::Duration::from_milliseconds(8'000));
    EXPECT_EQ(seeked.byte_position, 33'300);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(7'992));

    fixture.arrive(33'300, 40'000);
    ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 4u);
    expect_range(ranges, 0, 0, 2'400);
    expect_range(ranges, 1, 7'032, 9'600);
    expect_range(ranges, 2, 12'000, 14'400);
    expect_range(ranges, 3, 19'032, 21'600);
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(24'000));
}

TEST_CASE(seeking_backward_into_a_gap_moves_no_other_range)
{
    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();

    // Buffer from near the end to the end of the stream, then seek back before it.
    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(20'000));
    fixture.arrive(static_cast<size_t>(seeked.byte_position), FILE_BYTE_SIZE);
    auto before_seek = fixture.scan();
    EXPECT_EQ(before_seek.size(), 2u);
    expect_range(before_seek, 1, 19'032, 24'000);
    auto duration_before_seek = fixture.timeline.duration();

    seeked = fixture.seek_to(AK::Duration::from_milliseconds(10'000));
    EXPECT(seeked.timestamp > before_seek[0].end);
    EXPECT(seeked.timestamp < before_seek[1].start);

    fixture.arrive(static_cast<size_t>(seeked.byte_position), static_cast<size_t>(seeked.byte_position) + 5'000);
    auto after_seek = fixture.scan();
    EXPECT_EQ(after_seek.size(), 3u);
    expect_same_range(after_seek, 0, before_seek, 0);
    expect_same_range(after_seek, 2, before_seek, 1);
    EXPECT_EQ(fixture.timeline.duration(), duration_before_seek);
}

TEST_CASE(a_seek_lands_on_the_walked_frame_when_the_range_behind_reaches_it)
{
    // The bytes the resync waits for run from the end of the playing range up to the target, so
    // that range walks through the target and places it by its frames rather than by the line.
    s_doubled_frame_bytes = DoubledFrameBytes { 11'000, 20'000 };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();

    s_on_resync = [&fixture](size_t) { fixture.arrive(10'000, 20'000); };

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(3'000));
    EXPECT_EQ(seeked.byte_position, 11'700);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(2'976));

    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 1u);
    expect_range(ranges, 0, 0, 6'960);

    auto again = fixture.seek_to(AK::Duration::from_milliseconds(3'000));
    EXPECT_EQ(again.byte_position, seeked.byte_position);
    EXPECT_EQ(again.timestamp, seeked.timestamp);
}

TEST_CASE(a_seek_landing_just_before_a_range_forms_a_range_up_to_it)
{
    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(20'000));
    fixture.arrive(static_cast<size_t>(seeked.byte_position), 90'000);
    auto ranges = fixture.scan();
    expect_range(ranges, 1, 19'032, 21'600);

    // The resync reads back from a target just before that range, so its bytes join the range's
    // without a range of their own. They get one, and the seek lands within it rather than reading
    // the whole gap from the first range.
    seeked = fixture.seek_to(AK::Duration::from_milliseconds(19'000));
    EXPECT_EQ(seeked.byte_position, 79'100);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(18'984));

    ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);
    expect_range(ranges, 0, 0, 2'400);
    expect_range(ranges, 1, 18'024, 21'600);
}

TEST_CASE(a_seek_waits_for_the_frame_spanning_its_target)
{
    // The walked frames carry twice the time of the gap's, so the line places the target earlier in
    // the gap than its frames do, and the bytes the resync brought in end before the frame spanning
    // it. The seek reads on until it reaches that frame, and a later seek finds the same one.
    s_doubled_frame_bytes = DoubledFrameBytes { 0, 60'000 };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 60'000);
    fixture.scan();

    s_on_resync = [&fixture](size_t target_byte) { fixture.arrive(target_byte - 300, target_byte + 100); };
    s_on_blocked_read = [&fixture](size_t position) { fixture.arrive(position, position + FRAME_BYTE_SIZE); };

    auto target = AK::Duration::from_milliseconds(40'000);
    auto first = fixture.seek_to(target);
    EXPECT_EQ(first.byte_position, 83'500);
    EXPECT_EQ(first.timestamp, AK::Duration::from_milliseconds(39'984));

    fixture.scan();
    auto second = fixture.seek_to(target);
    EXPECT_EQ(second.byte_position, first.byte_position);
    EXPECT_EQ(second.timestamp, first.timestamp);
}

TEST_CASE(bytes_arriving_behind_a_seek_are_still_walked)
{
    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(20'000));
    fixture.arrive(static_cast<size_t>(seeked.byte_position), 90'000);

    // Bytes still in flight for the range that was playing arrive after the seek.
    fixture.arrive(10'000, 20'000);
    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);
    expect_range(ranges, 0, 0, 4'800);
    expect_range(ranges, 1, 19'032, 21'600);
}

TEST_CASE(an_estimated_duration_extrapolates_the_tail_from_walked_frames)
{
    s_doubled_frame_bytes = DoubledFrameBytes { 0, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(48'000));
}

TEST_CASE(an_estimated_anchor_can_drop_below_the_reported_duration)
{
    // The first frames carry twice the time of the rest, so the anchor is placed at 48 seconds
    // before the rest of the stream shows it belongs at 39.
    s_doubled_frame_bytes = DoubledFrameBytes { 0, 10'000 };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(48'000));

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(30'000));
    EXPECT_EQ(seeked.byte_position, 66'500);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(30'000));

    fixture.arrive(62'500, FILE_BYTE_SIZE);
    auto ranges = fixture.scan();
    expect_range(ranges, 1, 28'080, 38'040);
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(48'000));

    // The reported duration runs past every byte of the stream, so a seek there lands on its end.
    seeked = fixture.seek_to(AK::Duration::from_milliseconds(45'000));
    EXPECT_EQ(seeked.byte_position, static_cast<i64>(FILE_BYTE_SIZE));
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(38'040));
}

TEST_CASE(a_seek_among_the_bytes_after_the_last_frame_lands_where_the_frames_end)
{
    // The anchor gives the bytes after the last frame time, but no frame can be found among them, so
    // the seek walks on from the last frame it knows of and finds nothing more.
    s_non_frame_bytes = NonFrameBytes { FILE_BYTE_SIZE - 200, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_non_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, FILE_BYTE_SIZE);
    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 1u);
    expect_range(ranges, 0, 0, 23'952);
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(24'000));

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(23'980));
    EXPECT_EQ(seeked.byte_position, 99'800);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(23'952));

    seeked = fixture.seek_to(fixture.timeline.duration());
    EXPECT_EQ(seeked.byte_position, 99'800);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(23'952));
}

TEST_CASE(a_range_reaches_the_next_across_bytes_that_hold_no_frames)
{
    // Once the bytes between the two ranges arrive, the first walks through the ones holding no frames
    // to the second, which moves back by the time the gap had given them.
    s_non_frame_bytes = NonFrameBytes { 40'000, 50'000 };
    ScopeGuard restore_stream_shape { [] { s_non_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.arrive(45'000, 60'000);
    auto ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);
    expect_range(ranges, 0, 0, 2'400);
    expect_range(ranges, 1, 12'000, 14'400);

    fixture.arrive(10'000, 45'000);
    ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 1u);
    expect_range(ranges, 0, 0, 12'000);
}

TEST_CASE(a_declared_duration_is_never_extrapolated)
{
    s_doubled_frame_bytes = DoubledFrameBytes { 0, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(24'000), Media::DurationSource::Declared };
    fixture.arrive(0, 10'000);
    fixture.scan();
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(24'000));

    // A seek past the declared duration lands at the end of the stream.
    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(30'000));
    EXPECT_EQ(seeked.byte_position, static_cast<i64>(FILE_BYTE_SIZE));
}

TEST_CASE(a_declared_duration_rises_to_walked_frames_that_pass_it)
{
    s_doubled_frame_bytes = DoubledFrameBytes { 0, FILE_BYTE_SIZE };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000), Media::DurationSource::Declared };
    fixture.arrive(0, 10'000);
    fixture.scan();
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(4'800));
}

TEST_CASE(a_declared_duration_describes_only_the_bytes_it_declares)
{
    // The first frames carry twice the declared average, so a rate walked from them would misplace
    // the gap, and the declared duration covers only the first half of the file.
    s_doubled_frame_bytes = DoubledFrameBytes { 0, 10'000 };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(12'000), Media::DurationSource::Declared, 50'000 };
    fixture.arrive(0, 10'000);
    fixture.scan();

    // The bytes past the declared stream are given the declared average as well.
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(24'000));

    auto seeked = fixture.seek_to(AK::Duration::from_milliseconds(6'000));
    EXPECT_EQ(seeked.byte_position, 15'000);
    EXPECT_EQ(seeked.timestamp, AK::Duration::from_milliseconds(6'000));
}

TEST_CASE(a_range_growing_into_the_next_absorbs_it)
{
    // The first frames carry twice the time of the rest, so the gap after them is given more
    // time than its bytes turn out to carry, and the range past it drifts back as they are walked.
    s_doubled_frame_bytes = DoubledFrameBytes { 0, 10'000 };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();
    fixture.arrive(50'000, 60'000);
    auto ranges = fixture.scan();
    expect_range(ranges, 0, 0, 4'800);
    expect_range(ranges, 1, 24'000, 26'400);

    fixture.arrive(10'000, 30'000);
    ranges = fixture.scan();
    expect_range(ranges, 0, 0, 9'600);
    expect_range(ranges, 1, 16'000, 18'400);

    fixture.arrive(30'000, 50'000);
    ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 1u);
    expect_range(ranges, 0, 0, 16'800);
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(48'000));
}

TEST_CASE(a_range_ahead_drifts_with_the_rate_the_range_behind_it_walks)
{
    // The bytes right after the first range carry twice the time the rate before them predicted.
    s_doubled_frame_bytes = DoubledFrameBytes { 10'000, 20'000 };
    ScopeGuard restore_stream_shape { [] { s_doubled_frame_bytes = {}; } };

    Fixture fixture { AK::Duration::from_milliseconds(2'000) };
    fixture.arrive(0, 10'000);
    fixture.scan();
    fixture.arrive(50'000, 60'000);
    auto ranges = fixture.scan();
    expect_range(ranges, 1, 12'000, 14'400);

    fixture.arrive(10'000, 20'000);
    ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);
    expect_range(ranges, 0, 0, 7'200);
    expect_range(ranges, 1, 18'000, 20'400);

    fixture.arrive(20'000, 40'000);
    ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 2u);
    expect_range(ranges, 0, 0, 12'000);
    expect_range(ranges, 1, 15'000, 17'400);

    fixture.arrive(40'000, 50'000);
    ranges = fixture.scan();
    EXPECT_EQ(ranges.size(), 1u);
    expect_range(ranges, 0, 0, 16'800);

    // The duration reached 33.2 seconds while the range ahead sat furthest out, and stays there.
    EXPECT_EQ(fixture.timeline.duration(), AK::Duration::from_milliseconds(33'200));
}

TEST_CASE(a_declared_rate_too_large_for_the_timeline_gives_its_gaps_no_time)
{
    // A stream declaring 95 years of media within a single byte would scale every gap past the range the
    // timeline can hold, so the gaps are given no time rather than overflowing.
    Fixture fixture { AK::Duration::from_seconds(3'000'000'000), Media::DurationSource::Declared, 1 };
    fixture.arrive(0, FRAME_BYTE_SIZE * 4);
    fixture.scan();

    auto seeked = fixture.seek_to(AK::Duration::from_seconds(1));
    EXPECT(seeked.byte_position >= 0);
    EXPECT(static_cast<size_t>(seeked.byte_position) <= FILE_BYTE_SIZE);
}
