/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Containers/ADTS/ADTSDemuxer.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>

static constexpr size_t FRAME_BYTE_SIZE = 256;

// 44100 Hz stereo AAC-LC frames carrying no CRC and one block each.
static void append_frames(Vector<u8>& bytes, size_t count)
{
    for (size_t index = 0; index < count; index++) {
        bytes.append(0xFF);
        bytes.append(0xF1);
        bytes.append(0x50);
        bytes.append(static_cast<u8>(0x80 | ((FRAME_BYTE_SIZE >> 11) & 3)));
        bytes.append(static_cast<u8>((FRAME_BYTE_SIZE >> 3) & 0xFF));
        bytes.append(static_cast<u8>(((FRAME_BYTE_SIZE & 7) << 5) | 0x1F));
        bytes.append(0xFC);
        for (size_t padding = Media::ADTS::FrameHeader::SIZE; padding < FRAME_BYTE_SIZE; padding++)
            bytes.append(0);
    }
}

static NonnullRefPtr<Media::Demuxer> create_demuxer(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    return MUST(Media::ADTS::ADTSDemuxer::from_stream(stream));
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

TEST_CASE(demuxer_reads_a_stream_of_nothing_but_frames)
{
    // Nine ADTS AAC-LC frames containing 48 kHz stereo silence, as an encoder wrote them. The first
    // frame carries the fill element naming the encoder, so the audio is not all that a frame holds.
    // clang-format off
    static constexpr auto raw_aac_data = to_array<u8>({
        0xff, 0xf1, 0x4c, 0x80, 0x03, 0xdf, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61,
        0x76, 0x63, 0x36, 0x31, 0x2e, 0x31, 0x39, 0x2e, 0x31, 0x30, 0x31, 0x00,
        0x42, 0x20, 0x08, 0xc1, 0x18, 0x38, 0xff, 0xf1, 0x4c, 0x80, 0x01, 0xbf,
        0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1, 0x4c, 0x80, 0x01,
        0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1, 0x4c, 0x80,
        0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1, 0x4c,
        0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1,
        0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff,
        0xf1, 0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c,
        0xff, 0xf1, 0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c,
        0x1c, 0xff, 0xf1, 0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60,
        0x8c, 0x1c,
    });
    // clang-format on

    auto stream = Media::IncrementallyPopulatedStream::create_from_data(raw_aac_data);
    auto demuxer = MUST(Media::ADTS::ADTSDemuxer::from_stream(stream));

    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.sample_rate(), 48000u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.channel_count(), 2);

    auto track = audio_track(*demuxer);
    size_t frame_count = 0;
    while (!demuxer->get_next_sample_for_track(track).is_error())
        frame_count++;
    EXPECT_EQ(frame_count, 9u);

    // Every frame of the stream was walked, so its length is known rather than extrapolated.
    EXPECT_EQ(MUST(demuxer->total_duration()), AK::Duration::from_time_units(9 * 1024, 1, 48000));
}

TEST_CASE(demuxer_describes_the_only_track_of_the_stream)
{
    auto demuxer = create_demuxer("bbb.adts"sv);

    EXPECT(MUST(demuxer->get_tracks_for_type(Media::TrackType::Video)).is_empty());

    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.sample_rate(), 44100u);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.channel_count(), 2);
    EXPECT_EQ(tracks[0].parsed_codec().codec_id(), Media::CodecID::AAC);
}

TEST_CASE(demuxer_reads_every_frame_of_the_stream)
{
    auto demuxer = create_demuxer("bbb.adts"sv);
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
        EXPECT(frame.value().is_keyframe());
        expected_timestamp += frame.value().duration();
        frame_count++;
    }

    EXPECT_EQ(frame_count, 219u);

    // Each frame is rounded to the clock a timestamp is kept on, so the timestamps of a stream add
    // up to its length rather than landing on it exactly.
    expect_durations_within(expected_timestamp, AK::Duration::from_time_units(219 * 1024, 1, 44100), AK::Duration::from_microseconds(1));
}

TEST_CASE(demuxer_hands_the_decoder_the_configuration_that_the_headers_imply)
{
    auto demuxer = create_demuxer("bbb.adts"sv);
    auto track = audio_track(*demuxer);

    auto frame = MUST(demuxer->get_next_sample_for_track(track));
    auto configuration = frame.new_codec_configuration();
    EXPECT(configuration.has_value());

    // An Audio Specific Config of object type 2, sampling frequency index 4 and channel
    // configuration 2, which is what a 44100 Hz stereo AAC-LC stream would carry in a container
    // that describes its codec.
    EXPECT_EQ(configuration->size(), 2u);
    EXPECT_EQ((*configuration)[0], 0x12);
    EXPECT_EQ((*configuration)[1], 0x10);

    // Only the frame that begins a decode sequence carries it.
    auto second = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(!second.new_codec_configuration().has_value());
}

TEST_CASE(demuxer_carries_the_audio_without_the_framing_around_it)
{
    Vector<u8> bytes;
    append_frames(bytes, 4);

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(ByteBuffer::copy(bytes.span())));
    auto demuxer = MUST(Media::ADTS::ADTSDemuxer::from_stream(stream));
    auto track = audio_track(*demuxer);

    auto frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(frame.data().size(), FRAME_BYTE_SIZE - Media::ADTS::FrameHeader::SIZE);
}

TEST_CASE(demuxer_seeks_to_the_frame_holding_a_timestamp)
{
    auto demuxer = create_demuxer("bbb.adts"sv);
    auto track = audio_track(*demuxer);

    auto target = AK::Duration::from_milliseconds(2'500);
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, target)), Media::DemuxerSeekResult::MovedPosition);

    auto frame = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(frame.presentation_timestamp() <= target);
    EXPECT(frame.presentation_timestamp() + frame.duration() > target);

    // Seeking back to the start returns the first frame of the stream.
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::zero())), Media::DemuxerSeekResult::MovedPosition);
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::zero());
}

TEST_CASE(demuxer_reads_the_frames_that_resume_after_damage)
{
    Vector<u8> bytes;
    append_frames(bytes, 2);
    bytes.resize(bytes.size() + 500);
    append_frames(bytes, 3);

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(ByteBuffer::copy(bytes.span())));
    auto demuxer = MUST(Media::ADTS::ADTSDemuxer::from_stream(stream));
    auto track = audio_track(*demuxer);

    size_t frame_count = 0;
    while (!demuxer->get_next_sample_for_track(track).is_error())
        frame_count++;
    EXPECT_EQ(frame_count, 5u);
}

TEST_CASE(demuxer_declines_a_stream_that_holds_no_frames)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(ByteBuffer::create_zeroed(4096)));
    auto demuxer = Media::ADTS::ADTSDemuxer::from_stream(stream);
    EXPECT(demuxer.is_error());
    EXPECT_EQ(demuxer.error().category(), Media::DecoderErrorCategory::UnrecognizedFormat);
}

TEST_CASE(demuxer_declines_a_stream_of_mpeg_audio_frames)
{
    // MPEG audio frames carry a layer, which ADTS requires to be absent, so the two formats do not
    // answer for one another however alike their sync codes look.
    Vector<u8> bytes;
    for (size_t index = 0; index < 8; index++) {
        bytes.append(0xFF);
        bytes.append(0xFB);
        bytes.append(0x94);
        bytes.append(0x00);
        for (size_t padding = 4; padding < 384; padding++)
            bytes.append(0);
    }

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(ByteBuffer::copy(bytes.span())));
    EXPECT(Media::ADTS::ADTSDemuxer::from_stream(stream).is_error());
}

TEST_CASE(the_registry_hands_a_stream_of_adts_frames_to_this_demuxer)
{
    auto file = MUST(Core::File::open("bbb.adts"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));

    // No other demuxer recognizes a stream that is nothing but ADTS frames, so reaching this one is
    // what makes the format playable at all.
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].parsed_codec().codec_id(), Media::CodecID::AAC);
    EXPECT_EQ(tracks[0].audio_data().sample_specification.sample_rate(), 44100u);
}
