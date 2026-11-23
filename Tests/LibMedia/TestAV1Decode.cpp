/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>

#include "TestMediaCommon.h"

// The file's mastering display and content light levels are only in metadata OBUs, not in its Matroska Colour element.
TEST_CASE(frames_carry_the_high_dynamic_range_metadata_in_their_bitstream)
{
    auto file = MUST(Core::File::open("./av1_hdr_metadata_in_webm.webm"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    EXPECT(!track->video_data().color_metadata.mastering_display_color_volume.has_value());
    EXPECT(!track->video_data().color_metadata.content_light_level_information.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto first_frame = MUST(demuxer->get_next_sample_for_track(*track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(first_frame.codec_id(), first_frame.new_codec_configuration().value_or({})));
    MUST(decoder->receive_coded_data(first_frame, Media::DecodeIntent::Output));
    decoder->signal_end_of_stream();
    auto frame = MUST(decoder->take_next_output({}));
    auto const& color_metadata = frame->color_metadata();

    EXPECT(color_metadata.content_light_level_information.has_value());
    if (color_metadata.content_light_level_information.has_value()) {
        EXPECT_EQ(color_metadata.content_light_level_information->maximum_content_light_level, 1005);
        EXPECT_EQ(color_metadata.content_light_level_information->maximum_frame_average_light_level, 315);
    }

    EXPECT(color_metadata.mastering_display_color_volume.has_value());
    if (!color_metadata.mastering_display_color_volume.has_value())
        return;
    auto const& volume = *color_metadata.mastering_display_color_volume;
    // AV1 codes chromaticities in units of 2^-16, and the minimum luminance in units of 2^-14 cd/m².
    static constexpr double chromaticity_error = 1.0 / (1 << 16);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.red.x, 0.708, chromaticity_error);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.red.y, 0.292, chromaticity_error);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.green.x, 0.170, chromaticity_error);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.green.y, 0.797, chromaticity_error);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.blue.x, 0.131, chromaticity_error);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.blue.y, 0.046, chromaticity_error);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.white_point.x, 0.3127, chromaticity_error);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.white_point.y, 0.3290, chromaticity_error);
    EXPECT_APPROXIMATE(volume.maximum_luminance, 1000.0);
    EXPECT_APPROXIMATE_WITH_ERROR(volume.minimum_luminance, 0.005, 1.0 / (1 << 14));
}

TEST_CASE(a_flush_forgets_the_high_dynamic_range_metadata_of_earlier_frames)
{
    // clang-format off
    auto metadata_obus = to_array<u8>({
        0x2a, 0x06, // metadata
        0x01, 0x03, 0xed, 0x01, 0x3b, // HDR_CLL: 1005 and 315
        0x80, // trailing bits
    });
    // clang-format on
    auto temporal_delimiter = to_array<u8>({ 0x12, 0x00 });
    auto coded_frame = [](ReadonlyBytes data, i64 milliseconds) {
        auto timestamp = AK::Duration::from_milliseconds(milliseconds);
        return Media::CodedFrame(Media::CodecID::AV1, timestamp, timestamp, AK::Duration::from_milliseconds(33), Media::FrameFlags::Keyframe, MUST(FixedArray<u8>::create(data)));
    };

    Media::FFmpeg::VideoFramesAwaitingOutput frames { Media::CodecID::AV1, {} };
    auto key = MUST(frames.add(coded_frame(metadata_obus, 0), Media::DecodeIntent::Output));
    EXPECT(frames.take(static_cast<i64>(key))->high_dynamic_range_metadata.content_light_level_information.has_value());

    key = MUST(frames.add(coded_frame(temporal_delimiter, 33), Media::DecodeIntent::Output));
    EXPECT(frames.take(static_cast<i64>(key))->high_dynamic_range_metadata.content_light_level_information.has_value());

    frames.flush();
    key = MUST(frames.add(coded_frame(temporal_delimiter, 0), Media::DecodeIntent::Output));
    EXPECT(!frames.take(static_cast<i64>(key))->high_dynamic_range_metadata.content_light_level_information.has_value());
}

static NonnullRefPtr<Media::VideoFrame> first_frame_of(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto first_frame = MUST(demuxer->get_next_sample_for_track(*track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(first_frame.codec_id(), first_frame.new_codec_configuration().value_or({})));
    MUST(decoder->receive_coded_data(first_frame, Media::DecodeIntent::Output));
    decoder->signal_end_of_stream();
    return MUST(decoder->take_next_output({}));
}

TEST_CASE(a_picture_without_chroma_is_given_planes_of_neutral_chroma)
{
    auto frame = first_frame_of("./av1_gray_8_bit.webm"sv);
    EXPECT_EQ(frame->bit_depth(), 8);
    EXPECT_EQ(frame->subsampling(), Media::Subsampling::yuv420());
    auto yuv_data = frame->yuv_data().release_value();
    EXPECT(all_of(yuv_data.u_data(), [](u8 sample) { return sample == 128; }));
    EXPECT(all_of(yuv_data.v_data(), [](u8 sample) { return sample == 128; }));
    EXPECT(any_of(yuv_data.y_data(), [&](u8 sample) { return sample != yuv_data.y_data()[0]; }));

    frame = first_frame_of("./av1_gray_12_bit.webm"sv);
    EXPECT_EQ(frame->bit_depth(), 12);
    EXPECT_EQ(frame->subsampling(), Media::Subsampling::yuv420());
    yuv_data = frame->yuv_data().release_value();
    EXPECT(all_of(yuv_data.u_data().reinterpret<u16 const>(), [](u16 sample) { return sample == 2048; }));
    EXPECT(all_of(yuv_data.v_data().reinterpret<u16 const>(), [](u16 sample) { return sample == 2048; }));
}
