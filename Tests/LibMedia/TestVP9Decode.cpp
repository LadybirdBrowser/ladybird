/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>

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

BENCHMARK_CASE(vp9_4k)
{
    decode_video("./vp9_4k.webm"sv, 2, make_decoder);
}

BENCHMARK_CASE(vp9_clamp_reference_mvs)
{
    decode_video("./vp9_clamp_reference_mvs.webm"sv, 92, make_decoder);
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
