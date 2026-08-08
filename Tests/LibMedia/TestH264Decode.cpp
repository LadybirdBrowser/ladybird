/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FixedArray.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerRegistry.h>
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

struct DemuxerAndVideoTrack {
    NonnullRefPtr<Media::Demuxer> demuxer;
    Media::Track track;
};

static DemuxerAndVideoTrack create_demuxer_and_video_track(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));
    return { move(demuxer), track.release_value() };
}

TEST_CASE(h264_configuration_change)
{
    auto [initial_demuxer, initial_track] = create_demuxer_and_video_track("./avc.mp4"sv);
    auto initial_sample = MUST(initial_demuxer->get_next_sample_for_track(initial_track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(initial_sample.codec_id(), initial_sample.new_codec_configuration().value()));

    auto [new_demuxer, new_track] = create_demuxer_and_video_track("./vfr.mkv"sv);

    bool decoded_frame = false;
    while (!decoded_frame) {
        auto sample = MUST(new_demuxer->get_next_sample_for_track(new_track));
        MUST(decoder->receive_coded_data(sample));

        while (true) {
            auto metadata_result = decoder->peek_next_output(new_track.video_data().cicp);
            if (metadata_result.is_error()) {
                EXPECT_EQ(metadata_result.error().category(), Media::DecoderErrorCategory::NeedsMoreInput);
                break;
            }
            auto metadata = metadata_result.release_value();

            EXPECT_EQ(metadata.size, Gfx::IntSize(1280, 720));
            auto plane_sizes = MUST(Gfx::YUVData::plane_sizes(metadata.size, metadata.bit_depth, metadata.subsampling));
            auto storage = MUST(FixedArray<u8>::create(plane_sizes.total));
            auto yuv_data = MUST(Gfx::YUVData::create(metadata.size, metadata.bit_depth, metadata.subsampling, metadata.cicp,
                storage.span().slice(0, plane_sizes.y),
                storage.span().slice(plane_sizes.y, plane_sizes.u),
                storage.span().slice(plane_sizes.y + plane_sizes.u, plane_sizes.v)));
            MUST(decoder->take_next_output_into(yuv_data));
            decoded_frame = true;
        }
    }
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

    auto first_sample = MUST(demuxer->get_next_sample_for_track(track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(first_sample.codec_id(), first_sample.new_codec_configuration().value()));
    MUST(decoder->receive_coded_data(first_sample));

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

        MUST(decoder->receive_coded_data(sample));
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
