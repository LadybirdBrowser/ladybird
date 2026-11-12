/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <LibMedia/MediaSourceExtensions/SourceBufferDemuxer.h>
#include <LibTest/TestCase.h>
#include <Tests/LibMedia/TestMediaCommon.h>

static constexpr auto frame_storage = Media::MediaSourceExtensions::SourceBufferDemuxer::buffered_bytes_per_frame;

static Media::CodedFrame coded_frame_at(u64 seconds, Optional<ReadonlyBytes> new_codec_configuration = {}, Media::CodecID codec_id = Media::CodecID::H264)
{
    auto data = MUST(FixedArray<u8>::create(1));
    data[0] = static_cast<u8>(seconds);

    Optional<FixedArray<u8>> configuration;
    if (new_codec_configuration.has_value())
        configuration = MUST(FixedArray<u8>::create(*new_codec_configuration));

    auto timestamp = AK::Duration::from_seconds(seconds);
    return {
        codec_id,
        timestamp,
        timestamp,
        AK::Duration::from_seconds(1),
        Media::FrameFlags::Keyframe,
        move(data),
        move(configuration),
    };
}

// Frames at 60 fps whose timestamps are rounded to whole milliseconds while their duration isn't, as in a WebM stream,
// with a keyframe every 120 frames.
static AK::Duration rounded_frame_start(i64 index)
{
    return AK::Duration::from_milliseconds((index * 1000 + 30) / 60);
}

static constexpr auto rounded_frame_duration = AK::Duration::from_nanoseconds(16'666'667);

static Media::CodedFrame frame_with_rounded_timestamp(i64 index)
{
    auto data = MUST(FixedArray<u8>::create(1));
    auto timestamp = rounded_frame_start(index);
    auto flags = index % 120 == 0 ? Media::FrameFlags::Keyframe : Media::FrameFlags::None;
    return { Media::CodecID::VP9, timestamp, timestamp, rounded_frame_duration, flags, move(data) };
}

TEST_CASE(seek_provides_the_active_codec_configuration)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    constexpr Array changed_configuration { static_cast<u8>(2) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1, changed_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(10));
    demuxer->add_coded_frame(track, coded_frame_at(11));

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(11'500), Media::DemuxerSeekOptions::None));
    auto forward_sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(forward_sample.presentation_timestamp(), AK::Duration::from_seconds(11));
    EXPECT_EQ(forward_sample.new_codec_configuration(), changed_configuration.span());

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(500), Media::DemuxerSeekOptions::None));
    auto backward_sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(backward_sample.presentation_timestamp(), AK::Duration::zero());
    EXPECT_EQ(backward_sample.new_codec_configuration(), initial_configuration.span());
}

// Frames one second apart coalesce into a run, so a three second gap starts a new one that is still close
// enough for playback to continue across.
TEST_CASE(crossing_into_a_run_with_an_unchanged_configuration_does_not_resend_it)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1));
    demuxer->add_coded_frame(track, coded_frame_at(4));
    demuxer->add_coded_frame(track, coded_frame_at(5));

    auto first = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(first.presentation_timestamp(), AK::Duration::zero());
    EXPECT_EQ(first.new_codec_configuration(), initial_configuration.span());

    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(1));

    auto across_the_gap = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(across_the_gap.presentation_timestamp(), AK::Duration::from_seconds(4));
    EXPECT(!across_the_gap.new_codec_configuration().has_value());
}

TEST_CASE(crossing_into_a_run_with_a_changed_configuration_sends_it)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    constexpr Array changed_configuration { static_cast<u8>(2) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1));
    demuxer->add_coded_frame(track, coded_frame_at(4, changed_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(5));

    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).new_codec_configuration(), initial_configuration.span());
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(1));

    auto across_the_gap = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(across_the_gap.presentation_timestamp(), AK::Duration::from_seconds(4));
    EXPECT_EQ(across_the_gap.new_codec_configuration(), changed_configuration.span());
}

TEST_CASE(seeking_within_one_configuration_does_not_resend_it)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    for (u64 second = 1; second < 4; second++)
        demuxer->add_coded_frame(track, coded_frame_at(second));

    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).new_codec_configuration(), initial_configuration.span());

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(2'500), Media::DemuxerSeekOptions::None));
    auto after_seek = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(after_seek.presentation_timestamp(), AK::Duration::from_seconds(2));
    EXPECT(!after_seek.new_codec_configuration().has_value());
}

TEST_CASE(seeking_onto_a_run_head_does_not_resend_its_configuration)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1));
    demuxer->add_coded_frame(track, coded_frame_at(4));
    demuxer->add_coded_frame(track, coded_frame_at(5));

    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).new_codec_configuration(), initial_configuration.span());

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(4'500), Media::DemuxerSeekOptions::None));
    auto at_run_head = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(at_run_head.presentation_timestamp(), AK::Duration::from_seconds(4));
    EXPECT(!at_run_head.new_codec_configuration().has_value());
}

// A codec that needs no initialization data still announces where a decode sequence begins, so an empty
// configuration has to be told apart from none at all.
TEST_CASE(an_empty_configuration_is_delivered_rather_than_treated_as_absent)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, ReadonlyBytes {}));
    demuxer->add_coded_frame(track, coded_frame_at(1));

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::zero(), Media::DemuxerSeekOptions::None));
    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::zero());
    EXPECT_EQ(sample.new_codec_configuration(), ReadonlyBytes {});
}

// A consumer that discarded its decoder has no way to ask for the configuration in effect other than this.
TEST_CASE(seeking_with_need_codec_configuration_resends_an_unchanged_configuration)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    for (u64 second = 1; second < 4; second++)
        demuxer->add_coded_frame(track, coded_frame_at(second));

    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).new_codec_configuration(), initial_configuration.span());

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(2'500), Media::DemuxerSeekOptions::NeedCodecConfiguration));
    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(2));
    EXPECT_EQ(sample.new_codec_configuration(), initial_configuration.span());
}

TEST_CASE(evicting_the_first_frame_of_a_run_preserves_its_configuration)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    for (u64 second = 1; second < 3; second++)
        demuxer->add_coded_frame(track, coded_frame_at(second));

    demuxer->take_earliest_groups(track, 1);

    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(1));
    EXPECT_EQ(sample.new_codec_configuration(), initial_configuration.span());
}

TEST_CASE(buffered_bytes_include_the_codec_configurations_that_frames_carry)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    Array<u8, 100> configuration {};
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    // Each switch of configuration is charged, and so is the copy that starts a new run.
    demuxer->add_coded_frame(track, coded_frame_at(0, configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1));
    demuxer->add_coded_frame(track, coded_frame_at(2, configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(10));
    EXPECT_EQ(demuxer->total_bytes(track), 4 * (frame_storage + 1) + 300);

    // The next frame carries the evicted frame's configuration forward, so the configuration isn't freed.
    EXPECT_EQ(demuxer->take_earliest_groups(track, 1), frame_storage + 1);
    EXPECT_EQ(demuxer->total_bytes(track), 3 * (frame_storage + 1) + 300);

    EXPECT_EQ(demuxer->take_latest_frames(track, 1), frame_storage + 101);
    EXPECT_EQ(demuxer->take_earliest_groups(track, 1), frame_storage + 101);
    EXPECT_EQ(demuxer->take_earliest_groups(track, 1), frame_storage + 101);
    EXPECT_EQ(demuxer->total_bytes(track), 0u);
}

TEST_CASE(evictable_bytes_leave_out_the_configuration_that_eviction_moves_forward)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    Array<u8, 100> initial_configuration {};
    Array<u8, 50> changed_configuration {};
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1));
    demuxer->add_coded_frame(track, coded_frame_at(2, changed_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(3));
    demuxer->add_coded_frame(track, coded_frame_at(10));

    // Eviction stops at the frame at 3 seconds, which receives the changed configuration.
    auto current_time = AK::Duration::from_seconds(3);
    auto evictable_bytes = demuxer->evictable_bytes_when_taking_all_earliest_frames(track, current_time);
    EXPECT_EQ(evictable_bytes, 3 * (frame_storage + 1) + 100);

    auto evictable_groups = demuxer->earliest_evictable_groups(track, current_time, NumericLimits<size_t>::max());
    EXPECT_EQ(evictable_groups.size(), 3u);
    EXPECT_EQ(evictable_groups.last().bytes_freed_by_evicting_through_this_group, evictable_bytes);
    EXPECT_EQ(demuxer->take_earliest_groups(track, evictable_groups.size()), evictable_bytes);
}

// Removing the run that the cursor sits in leaves it with no position to jump from, so a later removal that
// would otherwise displace it must not discard that.
TEST_CASE(a_pending_reanchor_outranks_a_later_jump)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1));
    for (u64 second = 4; second < 7; second++)
        demuxer->add_coded_frame(track, coded_frame_at(second));

    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::zero());

    // The cursor's own run disappears, so it has to be reanchored to where playback had reached.
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::zero(), AK::Duration::from_seconds(2));
    demuxer->add_coded_frame(track, coded_frame_at(0));

    // A removal that displaces the cursor within the run it now indexes must not cancel that reanchoring.
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::from_seconds(4), AK::Duration::from_seconds(5));

    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::zero());
}

TEST_CASE(removing_a_configuration_change_preserves_it_for_the_remaining_run)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    constexpr Array changed_configuration { static_cast<u8>(2) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(1, changed_configuration.span()));
    demuxer->add_coded_frame(track, coded_frame_at(2));
    demuxer->add_coded_frame(track, coded_frame_at(3));

    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::from_seconds(1), AK::Duration::from_seconds(2));
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(2'500), Media::DemuxerSeekOptions::None));
    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(2));
    EXPECT_EQ(sample.new_codec_configuration(), changed_configuration.span());
}

// A track may only play through the times in which every audio and video track of the SourceBuffer has data.
TEST_CASE(a_track_stops_where_another_tracks_data_ends)
{
    Media::Track video_track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    Media::Track audio_track { Media::TrackType::Audio, 2, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { video_track, audio_track });

    for (u64 second = 0; second < 6; second++)
        demuxer->add_coded_frame(video_track, coded_frame_at(second));
    for (u64 second = 0; second < 3; second++)
        demuxer->add_coded_frame(audio_track, coded_frame_at(second, {}, Media::CodecID::Opus));

    auto buffered = demuxer->buffered_ranges();
    EXPECT_EQ(buffered.size(), 1u);
    EXPECT_EQ(buffered[0].end, AK::Duration::from_seconds(3));

    for (u64 second = 0; second < 3; second++)
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(video_track)).presentation_timestamp(), AK::Duration::from_seconds(second));

    // The next video frame lies past the audio data, so the read would wait; aborting it shows that it did not
    // deliver the frame.
    demuxer->set_blocking_reads_aborted_for_track(video_track);
    auto blocked_read = demuxer->get_next_sample_for_track(video_track);
    EXPECT(blocked_read.is_error());
    EXPECT_EQ(blocked_read.error().category(), Media::DecoderErrorCategory::Aborted);
    demuxer->reset_blocking_reads_aborted_for_track(video_track);

    demuxer->add_coded_frame(audio_track, coded_frame_at(3, {}, Media::CodecID::Opus));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(video_track)).presentation_timestamp(), AK::Duration::from_seconds(3));
}

TEST_CASE(track_buffer_ranges_are_published_once_the_change_batch_ends)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    {
        Media::MediaSourceExtensions::SourceBufferDemuxer::ChangeBatch change_batch { demuxer };
        for (u64 second : { 6u, 5u, 1u, 0u })
            demuxer->add_coded_frame(track, coded_frame_at(second));
        EXPECT(demuxer->track_buffer_ranges(track).is_empty());
        EXPECT(demuxer->buffered_ranges().is_empty());
    }

    Media::TimeRanges expected_ranges {
        { AK::Duration::from_seconds(0), AK::Duration::from_seconds(2) },
        { AK::Duration::from_seconds(5), AK::Duration::from_seconds(7) },
    };
    EXPECT_EQ(demuxer->track_buffer_ranges(track), expected_ranges);
    EXPECT_EQ(demuxer->buffered_ranges(), expected_ranges);
}

static Media::CodedFrame keyframe_at(u64 seconds, u64 duration_in_seconds)
{
    auto data = MUST(FixedArray<u8>::create(1));
    auto timestamp = AK::Duration::from_seconds(seconds);
    return { Media::CodecID::H264, timestamp, timestamp, AK::Duration::from_seconds(duration_in_seconds), Media::FrameFlags::Keyframe, move(data), {} };
}

static Media::TimeRanges::Range seconds_range(u64 start, u64 end)
{
    return { AK::Duration::from_seconds(start), AK::Duration::from_seconds(end) };
}

TEST_CASE(removing_a_frame_keeps_the_range_that_an_overlapping_frame_covers)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, keyframe_at(0, 2));
    demuxer->add_coded_frame(track, keyframe_at(1, 2));

    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::zero(), AK::Duration::from_seconds(1));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(1, 3) }));
}

TEST_CASE(a_longer_frame_bridges_the_gaps_before_it)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, keyframe_at(0, 1));
    demuxer->add_coded_frame(track, keyframe_at(5, 1));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(0, 1), seconds_range(5, 6) }));

    demuxer->add_coded_frame(track, keyframe_at(20, 3));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(0, 6), seconds_range(20, 23) }));
}

TEST_CASE(removing_the_longest_frame_stops_bridging_the_gaps_it_allowed)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, keyframe_at(0, 3));
    demuxer->add_coded_frame(track, keyframe_at(10, 1));
    demuxer->add_coded_frame(track, keyframe_at(15, 1));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(0, 3), seconds_range(10, 16) }));

    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::zero(), AK::Duration::from_seconds(3));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(10, 11), seconds_range(15, 16) }));
}

static Media::CodedFrame one_second_frame_at(u64 presentation_seconds, u64 decode_seconds, Media::FrameFlags flags)
{
    auto data = MUST(FixedArray<u8>::create(1));
    data[0] = static_cast<u8>(presentation_seconds);
    return { Media::CodecID::H264, AK::Duration::from_seconds(presentation_seconds), AK::Duration::from_seconds(decode_seconds), AK::Duration::from_seconds(1), flags, move(data), {} };
}

TEST_CASE(frames_sharing_a_decode_timestamp_share_one_copy_of_the_codec_configuration)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array configuration { static_cast<u8>(1), static_cast<u8>(2), static_cast<u8>(3), static_cast<u8>(4) };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (size_t index = 0; index < 4; index++) {
        auto data = MUST(FixedArray<u8>::create(1));
        Optional<FixedArray<u8>> new_configuration;
        if (index == 0)
            new_configuration = MUST(FixedArray<u8>::create(configuration.span()));
        demuxer->add_coded_frame(track, { Media::CodecID::H264, AK::Duration::zero(), AK::Duration::zero(), AK::Duration::zero(), Media::FrameFlags::Keyframe, move(data), move(new_configuration) });
    }
    EXPECT_EQ(demuxer->total_bytes(track), 4 * (frame_storage + 1) + configuration.size());
}

TEST_CASE(removing_the_last_appended_frame_drops_the_frames_that_would_follow_it)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, one_second_frame_at(0, 0, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, one_second_frame_at(1, 1, Media::FrameFlags::None));
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::from_seconds(1), AK::Duration::from_seconds(2));

    demuxer->add_coded_frame(track, one_second_frame_at(2, 2, Media::FrameFlags::None));
    EXPECT_EQ(demuxer->total_bytes(track), frame_storage + 1);
}

TEST_CASE(removing_an_earlier_run_keeps_appending_to_the_last_appended_run)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, one_second_frame_at(0, 0, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, one_second_frame_at(10, 10, Media::FrameFlags::Keyframe));
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::zero(), AK::Duration::from_seconds(1));

    demuxer->add_coded_frame(track, one_second_frame_at(11, 11, Media::FrameFlags::None));
    EXPECT_EQ(demuxer->total_bytes(track), 2 * (frame_storage + 1));
}

TEST_CASE(splitting_the_last_appended_run_keeps_appending_to_its_tail)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 seconds = 0; seconds < 4; seconds++)
        demuxer->add_coded_frame(track, one_second_frame_at(seconds, seconds, Media::FrameFlags::Keyframe));
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::from_seconds(1), AK::Duration::from_seconds(2));

    demuxer->add_coded_frame(track, one_second_frame_at(4, 4, Media::FrameFlags::None));
    EXPECT_EQ(demuxer->total_bytes(track), 4 * (frame_storage + 1));
}

TEST_CASE(a_frame_presenting_before_earlier_runs_keeps_the_runs_ordered)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 seconds : { 5, 6, 7 })
        demuxer->add_coded_frame(track, one_second_frame_at(seconds, seconds - 5, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, one_second_frame_at(20, 20, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, one_second_frame_at(0, 21, Media::FrameFlags::Keyframe));

    // Splitting the run at five seconds checks its order against the run that now starts at zero.
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::from_seconds(6), AK::Duration::from_seconds(7));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(0, 1), seconds_range(5, 8), seconds_range(20, 21) }));

    // A seek to the start finds the run that starts there, although it was appended after the run at five seconds.
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::zero(), Media::DemuxerSeekOptions::None)), Media::DemuxerSeekResult::MovedPosition);
}

TEST_CASE(splitting_a_run_whose_tail_presents_first_keeps_the_runs_ordered)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 seconds : { 5, 6, 0 })
        demuxer->add_coded_frame(track, one_second_frame_at(seconds, 0, Media::FrameFlags::Keyframe));

    // The frame at zero decodes after the removed frame, so it stays and starts a run before the one it split from.
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::from_seconds(6), AK::Duration::from_seconds(7));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(0, 1), seconds_range(5, 6) }));
}

TEST_CASE(removing_the_start_of_a_run_keeps_the_runs_ordered)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, one_second_frame_at(0, 0, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, one_second_frame_at(5, 1, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, one_second_frame_at(3, 10, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, one_second_frame_at(4, 11, Media::FrameFlags::Keyframe));

    // The first run now starts at five seconds, after the run at three seconds.
    demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::zero(), AK::Duration::from_seconds(1));

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(3), Media::DemuxerSeekOptions::None));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(3));
}

TEST_CASE(evicting_the_earliest_groups_continues_into_the_next_run)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 seconds : { 0, 1, 10, 11 })
        demuxer->add_coded_frame(track, one_second_frame_at(seconds, seconds, Media::FrameFlags::Keyframe));

    // The group being appended to is never evicted.
    auto groups = demuxer->earliest_evictable_groups(track, AK::Duration::from_seconds(20), NumericLimits<size_t>::max());
    EXPECT_EQ(groups.size(), 3u);
    EXPECT_EQ(groups.last().presentation_timestamp, AK::Duration::from_seconds(10));
    EXPECT_EQ(groups.last().bytes_freed_by_evicting_through_this_group, 3 * (frame_storage + 1));

    EXPECT_EQ(demuxer->take_earliest_groups(track, groups.size()), 3 * (frame_storage + 1));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(11, 12) }));
}

TEST_CASE(evicting_the_latest_frames_continues_into_the_previous_run)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    // Each pair of frames decodes before the last, so the earliest run is the one being appended to.
    for (u64 seconds : { 20, 21, 10, 11, 0, 1 })
        demuxer->add_coded_frame(track, one_second_frame_at(seconds, seconds, Media::FrameFlags::Keyframe));

    auto frames = demuxer->latest_evictable_frames(track, AK::Duration::zero(), 3 * (frame_storage + 1));
    EXPECT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames.last().presentation_timestamp, AK::Duration::from_seconds(11));

    EXPECT_EQ(demuxer->take_latest_frames(track, frames.size()), 3 * (frame_storage + 1));
    EXPECT_EQ(demuxer->track_buffer_ranges(track), Media::TimeRanges({ seconds_range(0, 2), seconds_range(10, 11) }));
}

static Media::CodedFrame tenth_of_a_second_frame_at(u64 milliseconds, Media::CodecID codec_id)
{
    auto data = MUST(FixedArray<u8>::create(1));
    auto timestamp = AK::Duration::from_milliseconds(milliseconds);
    return { codec_id, timestamp, timestamp, AK::Duration::from_milliseconds(100), Media::FrameFlags::Keyframe, move(data), {} };
}

TEST_CASE(a_seek_into_a_gap_after_the_first_range_does_not_play_across_it)
{
    Media::Track video_track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    Media::Track audio_track { Media::TrackType::Audio, 2, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { video_track, audio_track });

    for (u64 milliseconds = 0; milliseconds < 1'000; milliseconds += 100)
        demuxer->add_coded_frame(video_track, tenth_of_a_second_frame_at(milliseconds, Media::CodecID::H264));
    for (u64 milliseconds : { 0, 100, 800, 900 })
        demuxer->add_coded_frame(audio_track, tenth_of_a_second_frame_at(milliseconds, Media::CodecID::Opus));

    // The range after the gap starts within the allowance for playing from the start, but a gap between ranges is
    // not the start.
    auto buffered = demuxer->buffered_ranges();
    EXPECT_EQ(buffered.size(), 2u);
    EXPECT_EQ(buffered[1].start, AK::Duration::from_milliseconds(800));

    MUST(demuxer->seek_to_most_recent_keyframe(video_track, AK::Duration::from_milliseconds(400), Media::DemuxerSeekOptions::None));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(video_track)).presentation_timestamp(), AK::Duration::from_milliseconds(400));

    // The next video frame lies in the gap, so the read would wait; aborting it shows that it did not deliver the
    // frame.
    demuxer->set_blocking_reads_aborted_for_track(video_track);
    auto blocked_read = demuxer->get_next_sample_for_track(video_track);
    EXPECT(blocked_read.is_error());
    EXPECT_EQ(blocked_read.error().category(), Media::DecoderErrorCategory::Aborted);
}

TEST_CASE(the_end_of_the_stream_lets_the_longer_track_play_out)
{
    Media::Track video_track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    Media::Track audio_track { Media::TrackType::Audio, 2, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { video_track, audio_track });

    for (u64 second = 0; second < 6; second++)
        demuxer->add_coded_frame(video_track, coded_frame_at(second));
    for (u64 second = 0; second < 3; second++)
        demuxer->add_coded_frame(audio_track, coded_frame_at(second, {}, Media::CodecID::Opus));
    demuxer->set_reached_end_of_stream();

    auto buffered = demuxer->buffered_ranges();
    EXPECT_EQ(buffered.size(), 1u);
    EXPECT_EQ(buffered[0].end, AK::Duration::from_seconds(6));

    for (u64 second = 0; second < 6; second++)
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(video_track)).presentation_timestamp(), AK::Duration::from_seconds(second));
    auto end_of_stream = demuxer->get_next_sample_for_track(video_track);
    EXPECT(end_of_stream.is_error());
    EXPECT_EQ(end_of_stream.error().category(), Media::DecoderErrorCategory::EndOfStream);
}

// Frames whose first byte identifies the append they came from, so that replacements can be told apart.
static Media::CodedFrame frame_from_source_at(u8 source, u64 seconds, Media::FrameFlags flags = Media::FrameFlags::Keyframe)
{
    auto data = MUST(FixedArray<u8>::create(2));
    data[0] = source;
    data[1] = static_cast<u8>(seconds);

    auto timestamp = AK::Duration::from_seconds(seconds);
    return { Media::CodecID::H264, timestamp, timestamp, AK::Duration::from_seconds(1), flags, move(data), {} };
}

// Like coded frame processing, each appended frame first removes the frames it overlaps.
static void replace_with_frame(Media::MediaSourceExtensions::SourceBufferDemuxer& demuxer, Media::Track const& track, Media::CodedFrame frame)
{
    auto start = frame.presentation_timestamp();
    demuxer.remove_coded_frames_and_dependants_in_range(track, start, start + frame.duration());
    demuxer.add_coded_frame(track, move(frame));
}

static void expect_next_sample(Media::MediaSourceExtensions::SourceBufferDemuxer& demuxer, Media::Track const& track, u8 source, u64 seconds)
{
    auto sample = MUST(demuxer.get_next_sample_for_track(track));
    EXPECT_EQ(sample.data()[0], source);
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(seconds));
}

TEST_CASE(replacing_frames_at_the_cursor_continues_reading_from_the_replacement)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 second = 0; second < 10; second++)
        demuxer->add_coded_frame(track, frame_from_source_at('A', second));
    for (u64 second = 0; second < 3; second++)
        expect_next_sample(*demuxer, track, 'A', second);

    // The replacement continues the run before the cursor, so reads have to find it by time.
    for (u64 second = 2; second < 6; second++)
        replace_with_frame(*demuxer, track, frame_from_source_at('B', second));

    expect_next_sample(*demuxer, track, 'B', 2);
    expect_next_sample(*demuxer, track, 'B', 3);
}

TEST_CASE(replacing_a_frame_in_the_cursors_group_of_pictures_reads_the_group_again)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 second = 0; second < 9; second++)
        demuxer->add_coded_frame(track, frame_from_source_at('A', second, second % 3 == 0 ? Media::FrameFlags::Keyframe : Media::FrameFlags::None));
    expect_next_sample(*demuxer, track, 'A', 0);
    expect_next_sample(*demuxer, track, 'A', 1);

    replace_with_frame(*demuxer, track, frame_from_source_at('B', 2));

    expect_next_sample(*demuxer, track, 'A', 0);
    expect_next_sample(*demuxer, track, 'A', 1);
    expect_next_sample(*demuxer, track, 'B', 2);
    expect_next_sample(*demuxer, track, 'A', 3);
}

TEST_CASE(replacing_a_later_group_of_pictures_does_not_move_the_cursor)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 second = 0; second < 9; second++)
        demuxer->add_coded_frame(track, frame_from_source_at('A', second, second % 3 == 0 ? Media::FrameFlags::Keyframe : Media::FrameFlags::None));
    expect_next_sample(*demuxer, track, 'A', 0);
    expect_next_sample(*demuxer, track, 'A', 1);

    for (u64 second = 3; second < 6; second++)
        replace_with_frame(*demuxer, track, frame_from_source_at('B', second));

    expect_next_sample(*demuxer, track, 'A', 2);
    expect_next_sample(*demuxer, track, 'B', 3);
    expect_next_sample(*demuxer, track, 'B', 4);
    expect_next_sample(*demuxer, track, 'B', 5);
    expect_next_sample(*demuxer, track, 'A', 6);
}

static Media::TimeRanges ranges_reported_as_invalidated_after(Media::MediaSourceExtensions::SourceBufferDemuxer& demuxer, Function<void()> change)
{
    auto& loop = never_destroyed_event_loop();
    Media::TimeRanges reported;
    demuxer.set_scan_state_change_handler([&](Media::TimeRanges const& ranges) {
        for (auto const& range : ranges)
            reported.add_range(range.start, range.end);
    });
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    reported = {};

    change();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    demuxer.set_scan_state_change_handler(nullptr);
    return reported;
}

TEST_CASE(replacing_delivered_frames_reports_their_ranges_as_invalidated)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 second = 0; second < 10; second++)
        demuxer->add_coded_frame(track, frame_from_source_at('A', second));
    for (u64 second = 0; second < 5; second++)
        expect_next_sample(*demuxer, track, 'A', second);

    // The consumer may still hold these, though the cursor has moved past them.
    auto reported_behind_the_cursor = ranges_reported_as_invalidated_after(*demuxer, [&] {
        replace_with_frame(*demuxer, track, frame_from_source_at('B', 2));
    });
    EXPECT_EQ(reported_behind_the_cursor, Media::TimeRanges({ seconds_range(2, 3) }));

    auto reported_ahead_of_the_cursor = ranges_reported_as_invalidated_after(*demuxer, [&] {
        replace_with_frame(*demuxer, track, frame_from_source_at('B', 7));
    });
    EXPECT(reported_ahead_of_the_cursor.is_empty());
}

TEST_CASE(reordered_frames_delivered_earlier_are_reported_as_invalidated)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    // Decode order I0 P3 B1 B2, which presents in the order 0 1 2 3.
    auto frame = [](u64 presentation, u64 decode, Media::FrameFlags flags) {
        auto data = MUST(FixedArray<u8>::create(1));
        data[0] = static_cast<u8>(presentation);
        return Media::CodedFrame { Media::CodecID::H264, AK::Duration::from_seconds(presentation), AK::Duration::from_seconds(decode), AK::Duration::from_seconds(1), flags, move(data), {} };
    };
    demuxer->add_coded_frame(track, frame(0, 0, Media::FrameFlags::Keyframe));
    demuxer->add_coded_frame(track, frame(3, 1, Media::FrameFlags::None));
    demuxer->add_coded_frame(track, frame(1, 2, Media::FrameFlags::None));
    demuxer->add_coded_frame(track, frame(2, 3, Media::FrameFlags::None));
    for (u64 presentation : { 0u, 3u, 1u })
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(presentation));

    // The last delivered frame presents at 1, but the one presenting at 3 was delivered before it.
    auto reported = ranges_reported_as_invalidated_after(*demuxer, [&] {
        demuxer->remove_coded_frames_and_dependants_in_range(track, AK::Duration::from_seconds(3), AK::Duration::from_seconds(4));
    });
    EXPECT_EQ(reported, Media::TimeRanges({ seconds_range(1, 4) }));
}

TEST_CASE(a_seek_forgets_what_was_delivered)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (u64 second = 0; second < 10; second++)
        demuxer->add_coded_frame(track, frame_from_source_at('A', second));
    for (u64 second = 0; second < 5; second++)
        expect_next_sample(*demuxer, track, 'A', second);

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(8), Media::DemuxerSeekOptions::None));
    auto reported_after_seeking = ranges_reported_as_invalidated_after(*demuxer, [&] {
        replace_with_frame(*demuxer, track, frame_from_source_at('B', 2));
    });
    EXPECT(reported_after_seeking.is_empty());
}

TEST_CASE(retracting_a_delivered_end_of_stream_reports_what_follows_as_invalidated)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    demuxer->add_coded_frame(track, frame_from_source_at('A', 0));
    demuxer->set_reached_end_of_stream();
    expect_next_sample(*demuxer, track, 'A', 0);

    // Until a read reports the end, nothing that was delivered depends on it.
    auto reported_before_delivering_the_end = ranges_reported_as_invalidated_after(*demuxer, [&] {
        demuxer->clear_reached_end_of_stream();
    });
    EXPECT(reported_before_delivering_the_end.is_empty());

    demuxer->set_reached_end_of_stream();
    auto end_of_stream = demuxer->get_next_sample_for_track(track);
    EXPECT(end_of_stream.is_error() && end_of_stream.error().category() == Media::DecoderErrorCategory::EndOfStream);

    auto reported_after_delivering_the_end = ranges_reported_as_invalidated_after(*demuxer, [&] {
        demuxer->clear_reached_end_of_stream();
    });
    EXPECT_EQ(reported_after_delivering_the_end, Media::TimeRanges({ { AK::Duration::from_seconds(1), AK::Duration::max() } }));
}

TEST_CASE(buffered_ranges_bridge_the_rounding_gaps_between_frames)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (i64 index = 0; index < 600; index++)
        demuxer->add_coded_frame(track, frame_with_rounded_timestamp(index));

    auto end = rounded_frame_start(599) + rounded_frame_duration;
    EXPECT_EQ(demuxer->track_buffer_ranges(track), (Media::TimeRanges { { AK::Duration::zero(), end } }));
}

TEST_CASE(evicting_a_group_of_frames_starts_the_buffered_ranges_at_the_next_group)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    auto demuxer = make_ref_counted<Media::MediaSourceExtensions::SourceBufferDemuxer>(Vector { track });

    for (i64 index = 0; index < 600; index++)
        demuxer->add_coded_frame(track, frame_with_rounded_timestamp(index));
    demuxer->take_earliest_groups(track, 1);

    auto end = rounded_frame_start(599) + rounded_frame_duration;
    EXPECT_EQ(demuxer->track_buffer_ranges(track), (Media::TimeRanges { { rounded_frame_start(120), end } }));
}
