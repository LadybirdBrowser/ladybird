/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibTest/TestCase.h>

TEST_CASE(seek_past_eos)
{
    auto file = MUST(Core::File::open("./vfr.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream));

    auto optional_track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    EXPECT(optional_track.has_value());
    auto track = optional_track.release_value();
    MUST(demuxer->create_context_for_track(track));

    AK::Duration last_timestamp;
    while (true) {
        auto sample_result = demuxer->get_next_sample_for_track(track);
        if (sample_result.is_error()) {
            EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        last_timestamp = sample_result.release_value().timestamp();
    }
    EXPECT_EQ(last_timestamp, AK::Duration::from_milliseconds(30126));

    auto seek_time = AK::Duration::from_milliseconds(31000);
    MUST(demuxer->seek_to_most_recent_keyframe(track, seek_time, Media::DemuxerSeekOptions::None));
    auto sample_after_seek = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample_after_seek.timestamp(), AK::Duration::zero());
}
