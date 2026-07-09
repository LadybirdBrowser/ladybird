/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

#include "TestMediaCommon.h"

static NonnullOwnPtr<Media::VideoDecoder> make_decoder(Media::Matroska::TrackEntry const& track)
{
    return MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(Media::CodecID::H264, track.codec_private_data()));
}

TEST_CASE(avc_in_matroska)
{
    decode_video("./avc_in_matroska.mkv"sv, 50, make_decoder);
}

TEST_CASE(avc_in_mp4_with_reordered_frames)
{
    auto file = MUST(Core::File::open("./avc.mp4"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));
    auto optional_track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(optional_track.has_value());
    auto track = optional_track.release_value();
    MUST(demuxer->create_context_for_track(track));

    auto codec_id = MUST(demuxer->get_codec_id_for_track(track));
    auto codec_initialization_data = MUST(demuxer->get_codec_initialization_data_for_track(track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data));

    size_t frame_count = 0;
    auto last_timestamp = AK::Duration::min();

    auto process_decoded_frames = [&] {
        while (true) {
            auto frame_result = decoder->get_decoded_frame(track.video_data().cicp);
            if (frame_result.is_error()) {
                if (frame_result.error().category() == Media::DecoderErrorCategory::NeedsMoreInput)
                    return;
                VERIFY_NOT_REACHED();
            }

            auto frame = frame_result.release_value();
            EXPECT(last_timestamp <= frame->timestamp());
            EXPECT(!frame->duration().is_zero());
            last_timestamp = frame->timestamp();
            ++frame_count;
        }
    };

    while (true) {
        auto sample_result = demuxer->get_next_sample_for_track(track);
        if (sample_result.is_error()) {
            EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }

        auto sample = sample_result.release_value();
        EXPECT(!sample.duration().is_zero());

        MUST(decoder->receive_coded_data(sample.timestamp(), sample.duration(), sample.data()));
        process_decoded_frames();
    }

    decoder->signal_end_of_stream();
    while (true) {
        auto frame_result = decoder->get_decoded_frame(track.video_data().cicp);
        if (frame_result.is_error()) {
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }

        auto frame = frame_result.release_value();
        EXPECT(last_timestamp <= frame->timestamp());
        EXPECT(!frame->duration().is_zero());
        last_timestamp = frame->timestamp();
        ++frame_count;
    }

    EXPECT_EQ(frame_count, 50u);
}
