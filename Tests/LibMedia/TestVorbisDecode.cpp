/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <Tests/LibMedia/TestMediaCommon.h>

TEST_SETUP
{
    Media::install_video_decoders({ Media::hardware_video_decoders(), Media::ffmpeg_video_decoders() });
    Media::install_audio_decoders({ Media::audio_decoders() });
}

TEST_CASE(44_1Khz_stereo)
{
    decode_audio("vorbis/44_1Khz_stereo.ogg"sv, 44100, 2, 352800);
}

TEST_CASE(input_after_the_end_of_stream_is_rejected_until_flushed)
{
    auto file = MUST(Core::File::open("vorbis/44_1Khz_stereo.ogg"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());
    auto const& track = tracks[0];
    MUST(demuxer->create_context_for_track(track));

    auto first_frame = MUST(demuxer->get_next_sample_for_track(track));
    auto second_frame = MUST(demuxer->get_next_sample_for_track(track));
    auto decoder = MUST(Media::FFmpeg::FFmpegAudioDecoder::try_create(first_frame.codec_id(), track.audio_data().sample_specification, *first_frame.new_codec_configuration()));

    MUST(decoder->receive_coded_data(first_frame));
    decoder->signal_end_of_stream();

    auto result = decoder->receive_coded_data(second_frame);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::EndOfStream);

    decoder->flush();
    MUST(decoder->receive_coded_data(first_frame));
}
