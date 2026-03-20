/*
 * Copyright (c) 2023-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/ReadonlyBytesCursor.h>
#include <LibTest/TestCase.h>

#include <LibMedia/Containers/Matroska/Reader.h>

static Media::Matroska::Streamer streamer_from_bytes(ReadonlyBytes bytes)
{
    return Media::Matroska::Streamer(make_ref_counted<Media::ReadonlyBytesCursor>(bytes));
}

TEST_CASE(streamer_read_element_id)
{
    // 1-byte ID: 0xEC (Void)
    auto streamer = streamer_from_bytes("\xEC"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_element_id()), 0xECu);

    // 2-byte ID: 0x42 0x86 (EBMLVersion)
    streamer = streamer_from_bytes("\x42\x86"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_element_id()), 0x4286u);

    // 4-byte ID: 0x1A 0x45 0xDF 0xA3 (EBML)
    streamer = streamer_from_bytes("\x1A\x45\xDF\xA3"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_element_id()), 0x1A45DFA3u);

    // Invalid: leading byte 0x00 (no width bit set)
    streamer = streamer_from_bytes("\x00"sv.bytes());
    EXPECT(streamer.read_element_id().is_error());
}

TEST_CASE(streamer_read_element_size)
{
    // 1-byte size: 0x85 = size 5
    auto streamer = streamer_from_bytes("\x85"sv.bytes());
    auto size = MUST(streamer.read_element_size());
    EXPECT(size.has_value());
    EXPECT_EQ(size.value(), 5u);

    // 1-byte size: 0x80 = size 0
    streamer = streamer_from_bytes("\x80"sv.bytes());
    size = MUST(streamer.read_element_size());
    EXPECT(size.has_value());
    EXPECT_EQ(size.value(), 0u);

    // 2-byte size: 0x40 0x02 = size 2
    streamer = streamer_from_bytes("\x40\x02"sv.bytes());
    size = MUST(streamer.read_element_size());
    EXPECT(size.has_value());
    EXPECT_EQ(size.value(), 2u);

    // 1-byte unknown size: 0xFF
    streamer = streamer_from_bytes("\xFF"sv.bytes());
    size = MUST(streamer.read_element_size());
    EXPECT(!size.has_value());

    // 2-byte unknown size: 0x7F 0xFF
    streamer = streamer_from_bytes("\x7F\xFF"sv.bytes());
    size = MUST(streamer.read_element_size());
    EXPECT(!size.has_value());

    // 4-byte unknown size: 0x1F 0xFF 0xFF 0xFF
    streamer = streamer_from_bytes("\x1F\xFF\xFF\xFF"sv.bytes());
    size = MUST(streamer.read_element_size());
    EXPECT(!size.has_value());

    // 8-byte unknown size: 0x01 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
    streamer = streamer_from_bytes("\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF"sv.bytes());
    size = MUST(streamer.read_element_size());
    EXPECT(!size.has_value());
}

TEST_CASE(streamer_read_variable_size_integer)
{
    // 1-byte VINT: 7 data bits
    // 0x80 → data=0
    auto streamer = streamer_from_bytes("\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 0u);

    // 0x81 → data=1
    streamer = streamer_from_bytes("\x81"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 1u);

    // 0xFE → data=126
    streamer = streamer_from_bytes("\xFE"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 126u);

    // 0xFF → data=127 (max for 1-byte)
    streamer = streamer_from_bytes("\xFF"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 127u);

    // 2-byte VINT: 14 data bits
    // 0x40 0x00 → data=0
    streamer = streamer_from_bytes("\x40\x00"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 0u);

    // 0x40 0x80 → data=128
    streamer = streamer_from_bytes("\x40\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 128u);

    // 0x7F 0xFF → data=16383 (max for 2-byte)
    streamer = streamer_from_bytes("\x7F\xFF"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 16383u);

    // 4-byte VINT: 28 data bits
    // 0x10 0x00 0x01 0x00 → data=256
    streamer = streamer_from_bytes("\x10\x00\x01\x00"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_integer()), 256u);

    // Invalid: leading byte 0x00
    streamer = streamer_from_bytes("\x00"sv.bytes());
    EXPECT(streamer.read_variable_size_integer().is_error());
}

TEST_CASE(streamer_read_variable_size_signed_integer)
{
    // 1-byte signed VINT: 7 data bits, bias = 2^6 - 1 = 63
    // 0xBF → data=63 → 63-63 = 0
    auto streamer = streamer_from_bytes("\xBF"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_signed_integer()), 0);

    // 0x80 → data=0 → 0-63 = -63
    streamer = streamer_from_bytes("\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_signed_integer()), -63);

    // 0xFF → data=127 → 127-63 = 64
    streamer = streamer_from_bytes("\xFF"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_signed_integer()), 64);

    // 2-byte signed VINT: 14 data bits, bias = 2^13 - 1 = 8191
    // 0x60 0x00 → data=8191 → 8191-8191 = 0
    streamer = streamer_from_bytes("\x5F\xFF"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_signed_integer()), 0);

    // 0x40 0x00 → data=0 → 0-8191 = -8191
    streamer = streamer_from_bytes("\x40\x00"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_signed_integer()), -8191);

    // 0x7F 0xFE → data=16382 → 16382-8191 = 8191
    streamer = streamer_from_bytes("\x7F\xFE"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_variable_size_signed_integer()), 8191);
}

TEST_CASE(streamer_read_u64)
{
    // Zero-length integer = 0
    auto streamer = streamer_from_bytes("\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_u64()), 0u);

    // 1-byte integer: size=1, value=42
    streamer = streamer_from_bytes("\x81\x2A"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_u64()), 42u);

    // 2-byte integer: size=2, value=0x0100 = 256
    streamer = streamer_from_bytes("\x82\x01\x00"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_u64()), 256u);

    // 1-byte integer: size=1, value=0xFF = 255
    streamer = streamer_from_bytes("\x81\xFF"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_u64()), 255u);
}

TEST_CASE(streamer_read_i64)
{
    // Zero-length signed integer = 0
    auto streamer = streamer_from_bytes("\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_i64()), 0);

    // 1-byte signed: size=1, value=0x01 = 1
    streamer = streamer_from_bytes("\x81\x01"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_i64()), 1);

    // 1-byte signed: size=1, value=0xFF = -1 (sign extended)
    streamer = streamer_from_bytes("\x81\xFF"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_i64()), -1);

    // 1-byte signed: size=1, value=0x80 = -128
    streamer = streamer_from_bytes("\x81\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_i64()), -128);

    // 1-byte signed: size=1, value=0x7F = 127
    streamer = streamer_from_bytes("\x81\x7F"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_i64()), 127);

    // 2-byte signed: size=2, value=0xFF80 = -128
    streamer = streamer_from_bytes("\x82\xFF\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_i64()), -128);

    // 2-byte signed: size=2, value=0x0080 = 128
    streamer = streamer_from_bytes("\x82\x00\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_i64()), 128);
}

TEST_CASE(streamer_read_float)
{
    // Zero-length float = 0.0
    auto streamer = streamer_from_bytes("\x80"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_float()), 0.0);

    // 4-byte float: size=4, IEEE 754 1.0f = 0x3F800000
    streamer = streamer_from_bytes("\x84\x3F\x80\x00\x00"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_float()), 1.0);

    // 8-byte double: size=8, IEEE 754 1.0 = 0x3FF0000000000000
    streamer = streamer_from_bytes("\x88\x3F\xF0\x00\x00\x00\x00\x00\x00"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_float()), 1.0);

    // 4-byte float: size=4, IEEE 754 -1.0f = 0xBF800000
    streamer = streamer_from_bytes("\x84\xBF\x80\x00\x00"sv.bytes());
    EXPECT_EQ(MUST(streamer.read_float()), -1.0);

    // Invalid float size (3 bytes)
    streamer = streamer_from_bytes("\x83\x00\x00\x00"sv.bytes());
    EXPECT(streamer.read_float().is_error());
}

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
    EXPECT_EQ(second_block.timestamp().value().to_milliseconds(), 33);
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
    EXPECT_EQ(block1.timestamp().value().to_milliseconds(), 0);
    EXPECT(block1.only_keyframes());
    EXPECT_EQ(block1.lacing(), Media::Matroska::Block::Lacing::FixedSize);
    auto frames1 = MUST(iterator.get_frames(block1));
    EXPECT_EQ(frames1.size(), 4u);
    for (auto const& frame : frames1)
        EXPECT_EQ(frame.size(), 4u);

    // Block 2: 2 frames × 8 bytes
    auto block2 = MUST(iterator.next_block());
    EXPECT_EQ(block2.timestamp().value().to_milliseconds(), 33);
    EXPECT_EQ(block2.lacing(), Media::Matroska::Block::Lacing::FixedSize);
    auto frames2 = MUST(iterator.get_frames(block2));
    EXPECT_EQ(frames2.size(), 2u);
    for (auto const& frame : frames2)
        EXPECT_EQ(frame.size(), 8u);

    // Block 3: 3 frames × 1 byte
    auto block3 = MUST(iterator.next_block());
    EXPECT_EQ(block3.timestamp().value().to_milliseconds(), 66);
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
        EXPECT_EQ(first_block.timestamp().value().to_milliseconds(), 0);
        EXPECT(first_block.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(150)));
        auto block_after_forward_seek = MUST(iterator.next_block());
        EXPECT_EQ(block_after_forward_seek.timestamp().value().to_milliseconds(), 100);
        EXPECT(block_after_forward_seek.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(220)));
        auto block_at_200 = MUST(iterator.next_block());
        EXPECT_EQ(block_at_200.timestamp().value().to_milliseconds(), 200);
        EXPECT(block_at_200.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(50)));
        auto block_at_0 = MUST(iterator.next_block());
        EXPECT_EQ(block_at_0.timestamp().value().to_milliseconds(), 0);
        EXPECT(block_at_0.only_keyframes());

        iterator = MUST(matroska_reader.seek_to_random_access_point(iterator, AK::Duration::from_milliseconds(100)));
        auto block_exact_100 = MUST(iterator.next_block());
        EXPECT_EQ(block_exact_100.timestamp().value().to_milliseconds(), 100);
        EXPECT(block_exact_100.only_keyframes());
    }
}

TEST_CASE(opus_frame_duration)
{
    auto file = MUST(Core::File::open("./vp9_in_webm.webm"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));

    u64 audio_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Audio, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        audio_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    EXPECT_NE(audio_track, 0u);

    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), audio_track));

    // WebM files typically don't specify a default frame duration. However, we parse the Opus frame header
    // to get the duration, so the reader should return durations of 20ms.
    auto expected_duration = AK::Duration::from_milliseconds(20);
    for (int i = 0; i < 5; i++) {
        auto block = MUST(iterator.next_block());
        VERIFY(block.duration().has_value());
        EXPECT_EQ(block.duration().value(), expected_duration);
    }
}
