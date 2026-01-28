/*
 * Copyright (c) 2023-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibTest/TestCase.h>

#include <LibMedia/Containers/Matroska/Reader.h>

TEST_CASE(master_elements_containing_crc32)
{
    auto file = MUST(Core::File::open("./master_elements_containing_crc32.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    EXPECT_EQ(video_track, 1u);

    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), video_track));
    MUST(iterator.next_block());
    MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_seconds(7)));
    MUST(iterator.next_block());
}

TEST_CASE(seek_in_multi_frame_blocks)
{
    auto file = MUST(Core::File::open("./test-webm-xiph-lacing.mka"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream->create_cursor()));
    auto optional_track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    EXPECT(optional_track.has_value());
    auto track = optional_track.release_value();
    MUST(demuxer->create_context_for_track(track, stream->create_cursor()));

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

TEST_CASE(block_group)
{
    auto file = MUST(Core::File::open("./test-matroska-block-group.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    EXPECT_EQ(video_track, 1u);

    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), video_track));

    auto first_block = MUST(iterator.next_block());
    EXPECT(first_block.duration().has_value());
    EXPECT_EQ(first_block.duration()->to_milliseconds(), 33);

    auto second_block = MUST(iterator.next_block());
    EXPECT_EQ(second_block.timestamp().to_milliseconds(), 33);
    EXPECT(second_block.only_keyframes());
}
