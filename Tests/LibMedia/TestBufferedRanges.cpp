/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>

struct SplitPoint {
    size_t byte;
    AK::Duration range_end_before_byte;
    AK::Duration range_start_at_byte;

    SplitPoint(size_t byte, AK::Duration range_end_before_byte, AK::Duration range_start_at_byte)
        : byte(byte)
        , range_end_before_byte(range_end_before_byte)
        , range_start_at_byte(range_start_at_byte)
    {
    }

    SplitPoint(size_t byte, i64 timestamp_prior, i64 timestamp_delta, u32 timebase)
        : SplitPoint(byte, AK::Duration::from_time_units(timestamp_prior, 1, timebase), AK::Duration::from_time_units(timestamp_prior + timestamp_delta, 1, timebase))
    {
    }
};

struct Fixture {
    StringView path;
    SplitPoint first_split;
    SplitPoint second_split;
};

static ByteBuffer read_fixture(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(file->read_until_eof());
}

static NonnullRefPtr<Media::FFmpeg::FFmpegDemuxer> create_demuxer(NonnullRefPtr<Media::IncrementallyPopulatedStream> const& stream)
{
    return MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));
}

static NonnullRefPtr<Media::IncrementallyPopulatedStream> create_complete_stream(ByteBuffer const& data)
{
    return Media::IncrementallyPopulatedStream::create_from_buffer(data);
}

static NonnullRefPtr<Media::IncrementallyPopulatedStream> create_disjoint_stream(ByteBuffer const& data, SplitPoint const& first_split, SplitPoint const& second_split)
{
    VERIFY(first_split.byte < second_split.byte);
    VERIFY(second_split.byte < data.size());

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->add_chunk_at(0, data.bytes().trim(first_split.byte));
    stream->add_chunk_at(second_split.byte, data.bytes().slice(second_split.byte));
    stream->close();
    return stream;
}

static void expect_valid_ranges(Media::TimeRanges const& ranges)
{
    EXPECT(!ranges.is_empty());

    auto previous_end = AK::Duration::min();
    for (auto const& range : ranges) {
        EXPECT(range.start >= AK::Duration::zero());
        EXPECT(range.end > range.start);
        EXPECT(range.start >= previous_end);
        previous_end = range.end;
    }
}

[[maybe_unused]] static bool nearly_greater_than_or_equal(AK::Duration lhs, AK::Duration rhs)
{
    return rhs - lhs <= AK::Duration::from_microseconds(5);
}

static void expect_complete_file_is_fully_buffered(Fixture const& fixture)
{
    dbgln("-- '{}' fully buffered --", fixture.path);

    auto data = read_fixture(fixture.path);
    auto demuxer = create_demuxer(create_complete_stream(data));
    auto duration = MUST(demuxer->total_duration());
    auto ranges = demuxer->buffered_time_ranges();

    expect_valid_ranges(ranges);
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, AK::Duration::zero());
    EXPECT(ranges[0].end >= duration - AK::Duration::from_microseconds(5));
}

static void expect_disjoint_byte_ranges_produce_disjoint_time_ranges(Fixture const& fixture)
{
    dbgln("-- '{}' disjointed --", fixture.path);

    auto data = read_fixture(fixture.path);
    auto demuxer = create_demuxer(create_disjoint_stream(data, fixture.first_split, fixture.second_split));
    auto ranges = demuxer->buffered_time_ranges();

    expect_valid_ranges(ranges);
    EXPECT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].start, AK::Duration::zero());
    EXPECT_EQ(ranges[0].end, fixture.first_split.range_end_before_byte);
    EXPECT_EQ(ranges[1].start, fixture.second_split.range_start_at_byte);
    EXPECT(ranges[0].end < ranges[ranges.size() - 1].start);
}

static void expect_removing_inside_of_complete_file_produces_disjoint_time_ranges(Fixture const& fixture)
{
    dbgln("-- '{}' removed inside --", fixture.path);

    auto data = read_fixture(fixture.path);
    auto stream = create_complete_stream(data);
    auto demuxer = create_demuxer(stream);
    (void)demuxer->buffered_time_ranges();
    stream->remove_byte_range(fixture.first_split.byte, fixture.second_split.byte);
    auto ranges = demuxer->buffered_time_ranges();

    expect_valid_ranges(ranges);
    EXPECT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].start, AK::Duration::zero());
    EXPECT_EQ(ranges[0].end, fixture.first_split.range_end_before_byte);
    EXPECT_EQ(ranges[1].start, fixture.second_split.range_start_at_byte);
    EXPECT(ranges[0].end < ranges[ranges.size() - 1].start);
}

static void expect_removing_ends_of_complete_file_produces_middle_time_range(Fixture const& fixture)
{
    dbgln("-- '{}' removed ends --", fixture.path);

    auto data = read_fixture(fixture.path);
    auto stream = create_complete_stream(data);
    auto demuxer = create_demuxer(stream);
    (void)demuxer->buffered_time_ranges();
    stream->remove_byte_range(fixture.second_split.byte, data.size());
    stream->remove_byte_range(0, fixture.first_split.byte);
    auto ranges = demuxer->buffered_time_ranges();

    expect_valid_ranges(ranges);
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, fixture.first_split.range_start_at_byte);
    EXPECT_EQ(ranges[0].end, fixture.second_split.range_end_before_byte);
}

static Array fixtures {
    Fixture {
        "buffered-ranges/tone.wav"sv,
        { 256 * KiB, (256 * KiB) - 78, 0, 192000 },
        { 1024 * KiB, (1024 * KiB) - 78, 0, 192000 },
    },
    Fixture {
        "buffered-ranges/tone.mp3"sv,
        { 32 * KiB, 28449792, 338688, 14112000 },
        { 72 * KiB, 64350720, 338688, 14112000 },
    },
    Fixture {
        "buffered-ranges/tone.flac"sv,
        { 32 * KiB, 87552, 0, 48000 },
        { 68 * KiB, 221184, 0, 48000 },
    },
    Fixture {
        "buffered-ranges/tone_opus.ogg"sv,
        { 96 * KiB, 432000, 48000, 48000 },
        { 160 * KiB, 768000, 48000, 48000 },
    },
    Fixture {
        "buffered-ranges/tone_flac.ogg"sv,
        { 96 * KiB, 322560, 46080, 44100 },
        { 160 * KiB, 552960, 46080, 44100 },
    },
    Fixture {
        "buffered-ranges/tone_vorbis.ogg"sv,
        { 256 * KiB, 3333120, 45056, 44100 },
        { 320 * KiB, 4189184, 45056, 44100 },
    },
};

TEST_CASE(complete_files_are_fully_buffered)
{
    for (auto const& fixture : fixtures)
        expect_complete_file_is_fully_buffered(fixture);
}

TEST_CASE(disjoint_byte_ranges_produce_disjoint_time_ranges)
{
    for (auto const& fixture : fixtures)
        expect_disjoint_byte_ranges_produce_disjoint_time_ranges(fixture);
}

TEST_CASE(removing_inside_of_complete_file_produces_disjoint_time_ranges)
{
    for (auto const& fixture : fixtures)
        expect_removing_inside_of_complete_file_produces_disjoint_time_ranges(fixture);
}

TEST_CASE(removing_ends_of_complete_file_produces_middle_time_range)
{
    for (auto const& fixture : fixtures)
        expect_removing_ends_of_complete_file_produces_middle_time_range(fixture);
}
