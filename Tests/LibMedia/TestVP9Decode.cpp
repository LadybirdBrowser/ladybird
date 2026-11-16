/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>

#include "TestMediaCommon.h"

static NonnullOwnPtr<Media::VideoDecoder> make_decoder(Media::Matroska::TrackEntry const& track)
{
    return MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(Media::CodecID::VP9, track.codec_private_data()));
}

TEST_CASE(webm_in_vp9)
{
    decode_video("./vp9_in_webm.webm"sv, 25, make_decoder);
}

TEST_CASE(vp9_oob_blocks)
{
    decode_video("./vp9_oob_blocks.webm"sv, 240, make_decoder);
}

TEST_CASE(input_after_the_end_of_stream_is_rejected_until_flushed)
{
    auto file = MUST(Core::File::open("./vp9_in_webm.webm"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto first_frame = MUST(demuxer->get_next_sample_for_track(*track));
    auto second_frame = MUST(demuxer->get_next_sample_for_track(*track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(first_frame.codec_id(), *first_frame.new_codec_configuration()));

    MUST(decoder->receive_coded_data(first_frame, Media::DecodeIntent::Output));
    decoder->signal_end_of_stream();

    auto result = decoder->receive_coded_data(second_frame, Media::DecodeIntent::Output);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::EndOfStream);

    decoder->flush();
    MUST(decoder->receive_coded_data(first_frame, Media::DecodeIntent::Output));
}

// The file was encoded from hevc_hdr10_plus_in_sei.mkv, so its frames carry the same HDR10+, in BlockAdditions.
TEST_CASE(frames_carry_their_own_hdr10_plus_scene_peaks_from_block_additions)
{
    ScenePeakChecker scene_peaks { hevc_hdr10_plus_in_sei_scene_peaks };
    auto file = MUST(Core::File::open("./vp9_hdr10_plus_in_block_additions.webm"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto first_frame = MUST(demuxer->get_next_sample_for_track(*track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(first_frame.codec_id(), *first_frame.new_codec_configuration()));
    MUST(decoder->receive_coded_data(first_frame, Media::DecodeIntent::Output));

    size_t frame_count = 0;
    while (true) {
        auto frame_result = decoder->take_next_output(track->video_data().color_metadata);
        if (!frame_result.is_error()) {
            auto const& frame = *frame_result.value();
            scene_peaks.check(frame.timestamp(), frame.color_metadata().maximum_scene_luminance);
            frame_count++;
            continue;
        }
        if (frame_result.error().category() == Media::DecoderErrorCategory::EndOfStream)
            break;
        if (frame_result.error().category() != Media::DecoderErrorCategory::NeedsMoreInput) {
            FAIL(ByteString::formatted("Decoding failed: {}", frame_result.error().description()));
            return;
        }

        auto sample_result = demuxer->get_next_sample_for_track(*track);
        if (!sample_result.is_error()) {
            MUST(decoder->receive_coded_data(sample_result.value(), Media::DecodeIntent::Output));
            continue;
        }
        if (sample_result.error().category() != Media::DecoderErrorCategory::EndOfStream) {
            FAIL(ByteString::formatted("Demuxing failed: {}", sample_result.error().description()));
            return;
        }
        decoder->signal_end_of_stream();
    }
    EXPECT_EQ(frame_count, hevc_hdr10_plus_in_sei_scene_peaks.size());
    scene_peaks.expect_every_frame_checked();
}

struct OutputDelay {
    size_t frames_in_before_first_out { 0 };
    bool every_later_frame_came_out_at_once { true };
};

static OutputDelay measure_output_delay(StringView path, Media::DecoderLatency latency)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    OutputDelay delay;
    OwnPtr<Media::VideoDecoder> decoder;
    size_t frames_in = 0;
    size_t frames_out = 0;
    for (size_t i = 0; i < 20; i++) {
        auto frame = demuxer->get_next_sample_for_track(*track);
        if (frame.is_error())
            break;
        if (!decoder)
            decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(frame.value().codec_id(), *frame.value().new_codec_configuration(), latency));
        MUST(decoder->receive_coded_data(frame.value(), Media::DecodeIntent::Output));
        frames_in++;
        size_t came_out = 0;
        while (!decoder->take_next_output({}).is_error())
            came_out++;
        if (frames_out == 0 && came_out > 0)
            delay.frames_in_before_first_out = frames_in;
        else if (frames_out > 0 && came_out == 0)
            delay.every_later_frame_came_out_at_once = false;
        frames_out += came_out;
    }
    return delay;
}

TEST_CASE(a_low_latency_decoder_gives_each_frame_before_it_takes_the_next)
{
    auto delay = measure_output_delay("./vp9_in_webm.webm"sv, Media::DecoderLatency::Low);
    EXPECT_EQ(delay.frames_in_before_first_out, 1u);
    EXPECT(delay.every_later_frame_came_out_at_once);
}
