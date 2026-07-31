/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>

static Media::CodedFrame coded_frame_at(u64 seconds, ReadonlyBytes new_codec_configuration = {})
{
    auto data = MUST(FixedArray<u8>::create(1));
    data[0] = static_cast<u8>(seconds);

    FixedArray<u8> configuration;
    if (!new_codec_configuration.is_empty())
        configuration = MUST(FixedArray<u8>::create(new_codec_configuration));

    auto timestamp = AK::Duration::from_seconds(seconds);
    return {
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
    auto demuxer = make_ref_counted<Web::MediaSourceExtensions::TrackBufferDemuxer>(
        track,
        Media::CodecID::H264,
        MUST(ByteBuffer::copy(initial_configuration.span())));

    demuxer->add_coded_frame(coded_frame_at(0));
    demuxer->add_coded_frame(coded_frame_at(1, changed_configuration.span()));
    demuxer->add_coded_frame(coded_frame_at(2));
    demuxer->add_coded_frame(coded_frame_at(3));

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(3'500), Media::DemuxerSeekOptions::None));
    auto forward_sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(forward_sample.presentation_timestamp(), AK::Duration::from_seconds(3));
    EXPECT_EQ(forward_sample.new_codec_configuration(), changed_configuration.span());

    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(500), Media::DemuxerSeekOptions::None));
    auto backward_sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(backward_sample.presentation_timestamp(), AK::Duration::zero());
    EXPECT_EQ(backward_sample.new_codec_configuration(), initial_configuration.span());
}
