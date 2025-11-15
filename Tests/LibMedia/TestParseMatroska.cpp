/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibTest/TestCase.h>

#include <LibMedia/Containers/Matroska/Reader.h>

TEST_CASE(master_elements_containing_crc32)
{
    auto matroska_reader = MUST(Media::Matroska::Reader::from_file("master_elements_containing_crc32.mkv"sv));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    VERIFY(video_track == 1);

    auto iterator = MUST(matroska_reader.create_sample_iterator(video_track));
    MUST(iterator.next_block());
    MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_seconds(7)));
    MUST(iterator.next_block());
}

TEST_CASE(seek_in_multi_frame_blocks)
{
    auto demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_file("test-webm-xiph-lacing.mka"sv));
    auto optional_track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    EXPECT(optional_track.has_value());
    auto track = optional_track.release_value();

    auto initial_coded_frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(initial_coded_frame.timestamp() <= AK::Duration::zero());

    auto forward_seek_time = AK::Duration::from_seconds(5);
    MUST(demuxer->seek_to_most_recent_keyframe(track, forward_seek_time, Media::DemuxerSeekOptions::None));
    auto coded_frame_after_forward_seek = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(coded_frame_after_forward_seek.timestamp() > AK::Duration::zero());
    EXPECT(coded_frame_after_forward_seek.timestamp() <= forward_seek_time);

    auto backward_seek_time = AK::Duration::from_seconds(2);
    MUST(demuxer->seek_to_most_recent_keyframe(track, backward_seek_time, Media::DemuxerSeekOptions::None));
    auto coded_frame_after_backward_seek = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(coded_frame_after_backward_seek.timestamp() > AK::Duration::zero());
    EXPECT(coded_frame_after_backward_seek.timestamp() <= backward_seek_time);
}
