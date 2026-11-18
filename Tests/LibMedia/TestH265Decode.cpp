/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>

#include "TestMediaCommon.h"

// H.265 is left out of the bundled FFmpeg, so these run against whatever system libavcodec decodes it.
static Media::FFmpeg::SystemFFmpeg const* h265_library()
{
    Media::FFmpeg::SystemFFmpeg::load();
    auto const* library = Media::FFmpeg::SystemFFmpeg::the();
    if (library == nullptr || !library->has_decoder(Media::CodecID::H265)) {
        warnln("No system FFmpeg decodes H.265, skipping");
        return nullptr;
    }
    return library;
}

static size_t for_each_frame(Media::FFmpeg::SystemFFmpeg const& library, StringView path, Function<void(Media::VideoFrame const&)> callback)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto first_sample = MUST(demuxer->get_next_sample_for_track(*track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(library.functions(), Media::CodecID::H265, first_sample.new_codec_configuration().value_or({})));
    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));

    size_t frame_count = 0;
    while (true) {
        auto frame_result = decoder->take_next_output(track->video_data().color_metadata);
        if (!frame_result.is_error()) {
            callback(*frame_result.value());
            frame_count++;
            continue;
        }
        if (frame_result.error().category() == Media::DecoderErrorCategory::EndOfStream)
            return frame_count;
        if (frame_result.error().category() != Media::DecoderErrorCategory::NeedsMoreInput) {
            FAIL(ByteString::formatted("Decoding failed: {}", frame_result.error().description()));
            return frame_count;
        }

        auto sample_result = demuxer->get_next_sample_for_track(*track);
        if (!sample_result.is_error()) {
            MUST(decoder->receive_coded_data(sample_result.value(), Media::DecodeIntent::Output));
            continue;
        }
        if (sample_result.error().category() != Media::DecoderErrorCategory::EndOfStream) {
            FAIL(ByteString::formatted("Demuxing failed: {}", sample_result.error().description()));
            return frame_count;
        }
        decoder->signal_end_of_stream();
    }
}

// The file's mastering display and content light levels are only in its SEI, not in its sample entry.
TEST_CASE(h265_frames_carry_the_high_dynamic_range_metadata_in_their_bitstream)
{
    auto const* library = h265_library();
    if (library == nullptr)
        return;

    auto frame_count = for_each_frame(*library, "./hevc_hdr_metadata_in_sei.mp4"sv, [](Media::VideoFrame const& frame) {
        auto const& color_metadata = frame.color_metadata();
        EXPECT(color_metadata.mastering_display_color_volume.has_value());
        if (color_metadata.mastering_display_color_volume.has_value()) {
            EXPECT_APPROXIMATE(color_metadata.mastering_display_color_volume->maximum_luminance, 1000.0);
            EXPECT_APPROXIMATE(color_metadata.mastering_display_color_volume->red.x, 0.68);
        }
        EXPECT(color_metadata.content_light_level_information.has_value());
        if (color_metadata.content_light_level_information.has_value()) {
            EXPECT_EQ(color_metadata.content_light_level_information->maximum_content_light_level, 1000);
            EXPECT_EQ(color_metadata.content_light_level_information->maximum_frame_average_light_level, 400);
        }
    });
    EXPECT_EQ(frame_count, 6u);
}

TEST_CASE(h265_frames_carry_their_own_hdr10_plus_scene_peaks)
{
    ScenePeakChecker scene_peaks { hevc_hdr10_plus_in_sei_scene_peaks };
    auto const* library = h265_library();
    if (library == nullptr)
        return;

    auto frame_count = for_each_frame(*library, "./hevc_hdr10_plus_in_sei.mkv"sv, [&](Media::VideoFrame const& frame) {
        scene_peaks.check(frame.timestamp(), frame.color_metadata().maximum_scene_luminance);
    });
    EXPECT_EQ(frame_count, 6u);
    scene_peaks.expect_every_frame_checked();
}

TEST_CASE(h265_frames_carry_their_own_dolby_vision_scene_peaks)
{
    ScenePeakChecker scene_peaks { hevc_dolby_vision_rpus_scene_peaks };
    auto const* library = h265_library();
    if (library == nullptr)
        return;

    auto frame_count = for_each_frame(*library, "./hevc_dolby_vision_rpus.mkv"sv, [&](Media::VideoFrame const& frame) {
        scene_peaks.check(frame.timestamp(), frame.color_metadata().maximum_scene_luminance);
    });
    EXPECT_EQ(frame_count, 6u);
    scene_peaks.expect_every_frame_checked();
}
