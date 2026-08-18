/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>

static Media::CodedFrame keyframe_at(AK::Duration timestamp)
{
    return Media::CodedFrame(
        timestamp,
        AK::Duration::from_milliseconds(40),
        Media::FrameFlags::Keyframe,
        {},
        Media::CodedVideoFrameData(Optional<AK::Duration> { timestamp }));
}

TEST_CASE(seek_in_unbuffered_gap_after_end_of_stream)
{
    Media::Track track(Media::TrackType::Video, 0, Media::Track::Kind::Main, {}, {});
    auto demuxer = adopt_ref(*new Web::MediaSourceExtensions::TrackBufferDemuxer(track, Media::CodecID::H264, {}));
    demuxer->add_coded_frame(keyframe_at(AK::Duration::zero()));
    demuxer->add_coded_frame(keyframe_at(AK::Duration::from_seconds(3)));
    demuxer->set_reached_end_of_stream();

    auto seek_result = demuxer->seek_to_most_recent_keyframe(
        track, AK::Duration::from_seconds(2), Media::DemuxerSeekOptions::None);
    EXPECT(seek_result.is_error());
    EXPECT_EQ(seek_result.error().category(), Media::DecoderErrorCategory::EndOfStream);

    auto sample_result = demuxer->get_next_sample_for_track(track);
    EXPECT(sample_result.is_error());
    EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
}
