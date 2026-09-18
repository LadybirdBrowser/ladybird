/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Containers/MP3/MP3Demuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>

static NonnullRefPtr<Media::Demuxer> create_demuxer(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    return MUST(Media::MP3::MP3Demuxer::from_stream(stream));
}

static void expect_durations_within(AK::Duration left, AK::Duration right, AK::Duration tolerance)
{
    EXPECT(left - right < tolerance);
    EXPECT(right - left < tolerance);
}

static Media::Track audio_track(Media::Demuxer& demuxer)
{
    auto track = MUST(demuxer.get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    MUST(demuxer.create_context_for_track(*track));
    return *track;
}

TEST_CASE(demuxer_describes_the_only_track_of_the_stream)
{
    auto demuxer = create_demuxer("buffered-ranges/tone.mp3"sv);

    EXPECT(MUST(demuxer->get_tracks_for_type(Media::TrackType::Video)).is_empty());

    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.sample_rate(), 48000u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.channel_count(), 2);
    EXPECT_EQ(tracks[0].parsed_codec().codec_id(), Media::CodecID::MP3);

    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_milliseconds(8'040));
}

TEST_CASE(demuxer_reads_every_frame_after_the_stream_info_frame)
{
    auto demuxer = create_demuxer("buffered-ranges/tone.mp3"sv);
    auto track = audio_track(*demuxer);

    size_t frame_count = 0;
    auto expected_timestamp = AK::Duration::zero();
    while (true) {
        auto frame = demuxer->get_next_sample_for_track(track);
        if (frame.is_error()) {
            EXPECT_EQ(frame.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        EXPECT_EQ(frame.value().presentation_timestamp(), expected_timestamp);
        EXPECT_EQ(frame.value().data().size(), 384u);
        EXPECT(frame.value().is_keyframe());
        expected_timestamp += frame.value().duration();
        frame_count++;
    }

    // The Info frame describing the stream is not one of the 335 frames it counts.
    EXPECT_EQ(frame_count, 335u);
    EXPECT_EQ(expected_timestamp, AK::Duration::from_milliseconds(8'040));

    // The duration is what the frames of the stream add up to, because it also anchors the
    // interpolation that gives a time to every byte a scan has not reached. Trimming the samples an
    // encoder padded the stream with would take 40ms off this stream, and has to change both at
    // once or neither.
    expect_durations_within(expected_timestamp, MUST(demuxer->total_duration()), AK::Duration::from_milliseconds(1));
}

TEST_CASE(demuxer_seeks_to_the_frame_holding_a_timestamp)
{
    auto demuxer = create_demuxer("buffered-ranges/tone.mp3"sv);
    auto track = audio_track(*demuxer);

    auto target = AK::Duration::from_milliseconds(4'000);
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, target)), Media::DemuxerSeekResult::MovedPosition);

    auto frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(frame.presentation_timestamp() <= target);
    EXPECT(frame.presentation_timestamp() + frame.duration() > target);

    // Seeking back to the start returns the first frame of the stream.
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::zero())), Media::DemuxerSeekResult::MovedPosition);
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::zero());
}

TEST_CASE(demuxer_reads_a_stream_that_describes_itself_with_nothing_but_frames)
{
    auto demuxer = create_demuxer("bbb_without_stream_info.mp3"sv);
    auto track = audio_track(*demuxer);

    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks[0].audio_data().sample_specification.sample_rate(), 44100u);

    size_t frame_count = 0;
    auto summed_duration = AK::Duration::zero();
    while (true) {
        auto frame = demuxer->get_next_sample_for_track(track);
        if (frame.is_error())
            break;
        summed_duration += frame.value().duration();
        frame_count++;
    }

    EXPECT_EQ(frame_count, 385u);

    // Without a frame count to read, the duration is extrapolated from the bitrate of the frames at
    // the start of the stream, so it only approaches the 10.057 seconds they add up to.
    expect_durations_within(summed_duration, MUST(demuxer->total_duration()), AK::Duration::from_milliseconds(10));
}

TEST_CASE(demuxer_reads_the_frames_that_resume_after_damage)
{
    // 128 kbps 48 kHz stereo Layer III frames, which each occupy 384 bytes.
    auto append_frames = [](Vector<u8>& bytes, size_t count) {
        for (size_t index = 0; index < count; index++) {
            bytes.append(0xFF);
            bytes.append(0xFB);
            bytes.append(0x94);
            bytes.append(0x00);
            for (size_t padding = 4; padding < 384; padding++)
                bytes.append(0);
        }
    };

    Vector<u8> bytes;
    append_frames(bytes, 2);
    bytes.resize(bytes.size() + 500);
    append_frames(bytes, 3);

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(ByteBuffer::copy(bytes.span())));
    auto demuxer = MUST(Media::MP3::MP3Demuxer::from_stream(stream));
    auto track = audio_track(*demuxer);

    size_t frame_count = 0;
    while (!demuxer->get_next_sample_for_track(track).is_error())
        frame_count++;
    EXPECT_EQ(frame_count, 5u);
}

TEST_CASE(demuxer_declines_a_stream_that_holds_no_frames)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(ByteBuffer::create_zeroed(4096)));
    auto demuxer = Media::MP3::MP3Demuxer::from_stream(stream);
    EXPECT(demuxer.is_error());
    EXPECT_EQ(demuxer.error().category(), Media::DecoderErrorCategory::UnrecognizedFormat);
}
