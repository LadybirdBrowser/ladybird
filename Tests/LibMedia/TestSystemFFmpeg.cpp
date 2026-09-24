/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/FFmpeg/SystemFFmpeg.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

#include "TestMediaCommon.h"

// The bundled libavcodec stands in for a system one; the loader treats it like any other.
static NonnullOwnPtr<Media::FFmpeg::SystemFFmpeg> load_bundled_library()
{
    return MUST(Media::FFmpeg::SystemFFmpeg::try_load(LADYBIRD_BUNDLED_LIBAVCODEC_PATH ""sv));
}

TEST_CASE(loaded_library_reports_its_major_and_decoders)
{
    auto library = load_bundled_library();
    EXPECT(library->major() >= 60u);
    EXPECT(library->has_decoder(Media::CodecID::VP9));
    EXPECT(!library->provides_decoder_missing_from_bundle());
}

TEST_CASE(a_missing_library_is_an_error)
{
    auto result = Media::FFmpeg::SystemFFmpeg::try_load("libavcodec-that-does-not-exist.so"sv);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::NotImplemented);
}

TEST_CASE(decodes_through_the_loaded_library)
{
    auto library = load_bundled_library();

    auto file = MUST(Core::File::open("./vp9_in_webm.webm"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).release_value();
    MUST(demuxer->create_context_for_track(track));

    auto first_sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(Media::FFmpeg::FFmpegVideoDecoder::capabilities(library->functions(), Media::ParsedCodec { first_sample.codec_id() }).has_value());
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(library->functions(), first_sample.codec_id(), first_sample.new_codec_configuration().value_or({})));
    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));

    size_t frame_count = 0;
    auto drain = [&] {
        while (true) {
            auto frame = decoder->take_next_output(track.video_data().cicp);
            if (frame.is_error())
                return frame.release_error();
            frame_count++;
        }
    };

    while (true) {
        auto sample = demuxer->get_next_sample_for_track(track);
        if (sample.is_error()) {
            EXPECT_EQ(sample.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        MUST(decoder->receive_coded_data(sample.release_value(), Media::DecodeIntent::Output));
        EXPECT_EQ(drain().category(), Media::DecoderErrorCategory::NeedsMoreInput);
    }
    decoder->signal_end_of_stream();
    EXPECT_EQ(drain().category(), Media::DecoderErrorCategory::EndOfStream);
    EXPECT(frame_count > 0);
}
