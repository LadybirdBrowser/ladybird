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

    demuxer->take_earliest_frame_and_dependants(track);

    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(1));
    EXPECT_EQ(sample.new_codec_configuration(), initial_configuration.span());
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

static Media::TimeRanges::Range seconds_range(u64 start, u64 end)
{
    return { AK::Duration::from_seconds(start), AK::Duration::from_seconds(end) };
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
