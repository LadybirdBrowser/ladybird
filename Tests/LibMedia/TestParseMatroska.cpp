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
    auto demuxer = MUST(Media::Matroska::MatroskaDemuxer::from_stream(stream));
    auto optional_track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    EXPECT(optional_track.has_value());
    auto track = optional_track.release_value();
    MUST(demuxer->create_context_for_track(track));

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

TEST_CASE(fixed_size_lacing)
{
    auto file = MUST(Core::File::open("./test-matroska-fixed-size-lacing.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    EXPECT_EQ(video_track, 1u);

    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), video_track));

    // Block 1: 4 frames × 4 bytes
    auto block1 = MUST(iterator.next_block());
    EXPECT_EQ(block1.timestamp().to_milliseconds(), 0);
    EXPECT(block1.only_keyframes());
    EXPECT_EQ(block1.lacing(), Media::Matroska::Block::Lacing::FixedSize);
    auto frames1 = MUST(iterator.get_frames(block1));
    EXPECT_EQ(frames1.size(), 4u);
    for (auto const& frame : frames1)
        EXPECT_EQ(frame.size(), 4u);

    // Block 2: 2 frames × 8 bytes
    auto block2 = MUST(iterator.next_block());
    EXPECT_EQ(block2.timestamp().to_milliseconds(), 33);
    EXPECT_EQ(block2.lacing(), Media::Matroska::Block::Lacing::FixedSize);
    auto frames2 = MUST(iterator.get_frames(block2));
    EXPECT_EQ(frames2.size(), 2u);
    for (auto const& frame : frames2)
        EXPECT_EQ(frame.size(), 8u);

    // Block 3: 3 frames × 1 byte
    auto block3 = MUST(iterator.next_block());
    EXPECT_EQ(block3.timestamp().to_milliseconds(), 66);
    EXPECT_EQ(block3.lacing(), Media::Matroska::Block::Lacing::FixedSize);
    auto frames3 = MUST(iterator.get_frames(block3));
    EXPECT_EQ(frames3.size(), 3u);
    for (auto const& frame : frames3)
        EXPECT_EQ(frame.size(), 1u);
}

TEST_CASE(fixed_size_lacing_invalid)
{
    auto file = MUST(Core::File::open("./test-matroska-fixed-size-lacing-invalid.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    EXPECT_EQ(video_track, 1u);

    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), video_track));

    auto block = MUST(iterator.next_block());
    EXPECT_EQ(block.lacing(), Media::Matroska::Block::Lacing::FixedSize);
    auto frames_or_error = iterator.get_frames(block);
    EXPECT(frames_or_error.is_error());
}

TEST_CASE(ebml_lacing)
{
    auto file = MUST(Core::File::open("./test-matroska-ebml-lacing.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    EXPECT_EQ(video_track, 1u);

    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), video_track));

    auto block1 = MUST(iterator.next_block());
    EXPECT_EQ(block1.lacing(), Media::Matroska::Block::Lacing::EBML);
    auto frames1 = MUST(iterator.get_frames(block1));
    EXPECT_EQ(frames1.size(), 2u);
    EXPECT_EQ(frames1[0].size(), 4u);
    EXPECT_EQ(frames1[1].size(), 4u);

    auto block2 = MUST(iterator.next_block());
    auto frames2 = MUST(iterator.get_frames(block2));
    EXPECT_EQ(frames2.size(), 3u);
    EXPECT_EQ(frames2[0].size(), 2u);
    EXPECT_EQ(frames2[1].size(), 4u);
    EXPECT_EQ(frames2[2].size(), 6u);

    auto block3 = MUST(iterator.next_block());
    auto frames3 = MUST(iterator.get_frames(block3));
    EXPECT_EQ(frames3.size(), 3u);
    EXPECT_EQ(frames3[0].size(), 6u);
    EXPECT_EQ(frames3[1].size(), 4u);
    EXPECT_EQ(frames3[2].size(), 2u);

    auto block4 = MUST(iterator.next_block());
    auto frames4 = MUST(iterator.get_frames(block4));
    EXPECT_EQ(frames4.size(), 4u);
    EXPECT_EQ(frames4[0].size(), 4u);
    EXPECT_EQ(frames4[1].size(), 6u);
    EXPECT_EQ(frames4[2].size(), 3u);
    EXPECT_EQ(frames4[3].size(), 5u);

    auto block5 = MUST(iterator.next_block());
    auto frames5 = MUST(iterator.get_frames(block5));
    EXPECT_EQ(frames5.size(), 5u);
    for (auto const& frame : frames5)
        EXPECT_EQ(frame.size(), 3u);

    auto block6 = MUST(iterator.next_block());
    auto frames6 = MUST(iterator.get_frames(block6));
    EXPECT_EQ(frames6.size(), 2u);
    EXPECT_EQ(frames6[0].size(), 1u);
    EXPECT_EQ(frames6[1].size(), 10u);

    auto block7 = MUST(iterator.next_block());
    auto frames7 = MUST(iterator.get_frames(block7));
    EXPECT_EQ(frames7.size(), 3u);
    EXPECT_EQ(frames7[0].size(), 10u);
    EXPECT_EQ(frames7[1].size(), 1u);
    EXPECT_EQ(frames7[2].size(), 8u);
}

TEST_CASE(seeking)
{
    auto test_files = {
        "./test-matroska-seeking.mkv"sv,
        "./test-matroska-seeking-without-cues.mkv"sv,
    };

    for (auto test_file : test_files) {
        auto file = MUST(Core::File::open(test_file, Core::File::OpenMode::Read));
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
        EXPECT_EQ(first_block.timestamp().to_milliseconds(), 0);
        EXPECT(first_block.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(150)));
        auto block_after_forward_seek = MUST(iterator.next_block());
        EXPECT_EQ(block_after_forward_seek.timestamp().to_milliseconds(), 100);
        EXPECT(block_after_forward_seek.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(220)));
        auto block_at_200 = MUST(iterator.next_block());
        EXPECT_EQ(block_at_200.timestamp().to_milliseconds(), 200);
        EXPECT(block_at_200.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(50)));
        auto block_at_0 = MUST(iterator.next_block());
        EXPECT_EQ(block_at_0.timestamp().to_milliseconds(), 0);
        EXPECT(block_at_0.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(100)));
        auto block_exact_100 = MUST(iterator.next_block());
        EXPECT_EQ(block_exact_100.timestamp().to_milliseconds(), 100);
        EXPECT(block_exact_100.only_keyframes());
    }
}
