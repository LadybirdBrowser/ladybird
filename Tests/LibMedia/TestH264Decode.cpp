/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FixedArray.h>
#include <LibGfx/YUVData.h>
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

    auto take_decoded_frame = [&]() -> Media::DecoderErrorOr<void> {
        auto metadata = TRY(decoder->peek_next_output(track.video_data().cicp));

        auto plane_sizes = MUST(Gfx::YUVData::plane_sizes(metadata.size, metadata.bit_depth, metadata.subsampling));
        auto storage = MUST(FixedArray<u8>::create(plane_sizes.total));
        auto yuv_data = MUST(Gfx::YUVData::create(metadata.size, metadata.bit_depth, metadata.subsampling, metadata.cicp,
            storage.span().slice(0, plane_sizes.y),
            storage.span().slice(plane_sizes.y, plane_sizes.u),
            storage.span().slice(plane_sizes.y + plane_sizes.u, plane_sizes.v)));
        MUST(decoder->take_next_output_into(yuv_data));

        EXPECT(last_timestamp <= metadata.timestamp);
        EXPECT(!metadata.duration.is_zero());
        last_timestamp = metadata.timestamp;
        ++frame_count;
        return {};
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
        while (true) {
            auto frame_result = take_decoded_frame();
            if (frame_result.is_error()) {
                EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::NeedsMoreInput);
                break;
            }
        }
    }

    decoder->signal_end_of_stream();
    while (true) {
        auto frame_result = take_decoded_frame();
        if (frame_result.is_error()) {
            EXPECT_EQ(frame_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
    }

    EXPECT_EQ(frame_count, 50u);
}
