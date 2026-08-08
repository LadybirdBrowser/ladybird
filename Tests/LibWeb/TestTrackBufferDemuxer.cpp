/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>

static Media::CodedFrame coded_frame_at(u64 seconds, Optional<ReadonlyBytes> new_codec_configuration = {})
{
    auto data = MUST(FixedArray<u8>::create(1));
    data[0] = static_cast<u8>(seconds);

    Optional<FixedArray<u8>> configuration;
    if (new_codec_configuration.has_value())
        configuration = MUST(FixedArray<u8>::create(*new_codec_configuration));

    auto timestamp = AK::Duration::from_seconds(seconds);
    return {
        Media::CodecID::H264,
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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(1, changed_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(10));
    demuxer->add_coded_frame(coded_frame_at(11));

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(1));
    demuxer->add_coded_frame(coded_frame_at(4));
    demuxer->add_coded_frame(coded_frame_at(5));

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(1));
    demuxer->add_coded_frame(coded_frame_at(4, changed_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(5));

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    for (u64 second = 1; second < 4; second++)
        demuxer->add_coded_frame(coded_frame_at(second));

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(1));
    demuxer->add_coded_frame(coded_frame_at(4));
    demuxer->add_coded_frame(coded_frame_at(5));

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, ReadonlyBytes {}));
    demuxer->add_coded_frame(coded_frame_at(1));

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    for (u64 second = 1; second < 4; second++)
        demuxer->add_coded_frame(coded_frame_at(second));

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    for (u64 second = 1; second < 3; second++)
        demuxer->add_coded_frame(coded_frame_at(second));

    demuxer->take_earliest_frame_and_dependants();

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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(1));
    for (u64 second = 4; second < 7; second++)
        demuxer->add_coded_frame(coded_frame_at(second));

    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::zero());

    // The cursor's own run disappears, so it has to be reanchored to where playback had reached.
    demuxer->remove_coded_frames_and_dependants_in_range(AK::Duration::zero(), AK::Duration::from_seconds(2));
    demuxer->add_coded_frame(coded_frame_at(0));

    // A removal that displaces the cursor within the run it now indexes must not cancel that reanchoring.
    demuxer->remove_coded_frames_and_dependants_in_range(AK::Duration::from_seconds(4), AK::Duration::from_seconds(5));

    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::zero());
}

TEST_CASE(removing_a_configuration_change_preserves_it_for_the_remaining_run)
{
    Media::Track track { Media::TrackType::Video, 1, Media::Track::Kind::Main, {}, {} };
    constexpr Array initial_configuration { static_cast<u8>(1) };
    constexpr Array changed_configuration { static_cast<u8>(2) };
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(track);

    demuxer->add_coded_frame(coded_frame_at(0, initial_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(1, changed_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(2));
    demuxer->add_coded_frame(coded_frame_at(3));

    demuxer->remove_coded_frames_and_dependants_in_range(AK::Duration::from_seconds(1), AK::Duration::from_seconds(2));
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(2'500), Media::DemuxerSeekOptions::None));
    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(2));
    EXPECT_EQ(sample.new_codec_configuration(), changed_configuration.span());
}
