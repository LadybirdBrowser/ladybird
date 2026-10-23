/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationVideoDecoder.h>

#include "TestMediaCommon.h"

namespace {

struct DemuxerAndVideoTrack {
    NonnullRefPtr<Media::Demuxer> demuxer;
    Media::Track track;
};

DemuxerAndVideoTrack create_demuxer_and_video_track(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));
    return { move(demuxer), track.release_value() };
}

// Windows N editions ship without the H.264 decoder, and HEVC needs an extension from the Store.
OwnPtr<Media::WindowsMediaFoundation::WindowsMediaFoundationVideoDecoder> create_decoder_if_available(Media::CodedFrame const& first_sample)
{
    auto decoder_or_error = Media::WindowsMediaFoundation::WindowsMediaFoundationVideoDecoder::try_create(first_sample.codec_id(), first_sample.new_codec_configuration().value_or({}));
    if (decoder_or_error.is_error()) {
        EXPECT_EQ(decoder_or_error.error().category(), Media::DecoderErrorCategory::NotImplemented);
        warnln("Media Foundation has no decoder for {}, skipping", first_sample.codec_id());
        return nullptr;
    }
    return decoder_or_error.release_value();
}

struct DecodeResult {
    size_t frame_count { 0 };
    RefPtr<Media::VideoFrame> first_frame;
};

DecodeResult decode_all_frames(StringView path, Function<void(Media::VideoFrame const&)> check_frame = {})
{
    auto [demuxer, track] = create_demuxer_and_video_track(path);
    auto first_sample = MUST(demuxer->get_next_sample_for_track(track));
    auto decoder = create_decoder_if_available(first_sample);
    if (!decoder)
        return {};

    DecodeResult result;
    auto last_timestamp = AK::Duration::min();
    auto take_decoded_frames = [&](Media::DecoderErrorCategory expected_end) {
        while (true) {
            auto frame_result = decoder->take_next_output(track.video_data().cicp);
            if (frame_result.is_error()) {
                EXPECT_EQ(frame_result.error().category(), expected_end);
                return;
            }
            auto frame = frame_result.release_value();
            EXPECT(last_timestamp < frame->timestamp());
            EXPECT(!frame->duration().is_zero());
            last_timestamp = frame->timestamp();
            if (check_frame)
                check_frame(*frame);
            if (!result.first_frame)
                result.first_frame = frame;
            result.frame_count++;
        }
    };

    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));
    take_decoded_frames(Media::DecoderErrorCategory::NeedsMoreInput);
    while (true) {
        auto sample_result = demuxer->get_next_sample_for_track(track);
        if (sample_result.is_error()) {
            EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        MUST(decoder->receive_coded_data(sample_result.value(), Media::DecodeIntent::Output));
        take_decoded_frames(Media::DecoderErrorCategory::NeedsMoreInput);
    }

    decoder->signal_end_of_stream();
    take_decoded_frames(Media::DecoderErrorCategory::EndOfStream);
    return result;
}

// A decoder that hands back an empty or misplaced chroma plane draws a flat green picture.
void expect_picture_has_detail(Media::VideoFrame const& frame)
{
    auto yuv_data = frame.yuv_data();
    VERIFY(yuv_data.has_value());
    auto component_size = frame.bit_depth() > 8 ? 2u : 1u;

    auto plane_varies = [&](ReadonlyBytes plane) {
        for (size_t i = component_size; i < plane.size(); i += component_size) {
            if (plane.slice(i, component_size) != plane.slice(0, component_size))
                return true;
        }
        return false;
    };
    EXPECT(plane_varies(yuv_data->y_data()));
    EXPECT(plane_varies(yuv_data->u_data()));
    EXPECT(plane_varies(yuv_data->v_data()));
}

}

TEST_CASE(avc_in_mp4_with_reordered_frames)
{
    auto result = decode_all_frames("./avc.mp4"sv);
    if (!result.first_frame)
        return;
    EXPECT_EQ(result.frame_count, 50u);
    expect_picture_has_detail(*result.first_frame);
}

TEST_CASE(avc_in_matroska)
{
    auto result = decode_all_frames("./avc_in_matroska.mkv"sv);
    if (!result.first_frame)
        return;
    EXPECT_EQ(result.frame_count, 50u);
}

TEST_CASE(hevc_10bit)
{
    auto result = decode_all_frames("./hevc_10bit.mp4"sv, [](Media::VideoFrame const& frame) {
        EXPECT_EQ(frame.bit_depth(), 10);
    });
    if (!result.first_frame)
        return;
    EXPECT(result.frame_count > 0);
    expect_picture_has_detail(*result.first_frame);

    // Ten significant bits must land in the low end of each sample.
    auto luma = result.first_frame->yuv_data()->y_data();
    for (size_t i = 0; i + 1 < luma.size(); i += 2)
        EXPECT((luma[i + 1] & 0xFC) == 0);
}

TEST_CASE(h264_reference_only_frames_are_decoded_but_not_produced)
{
    auto [demuxer, track] = create_demuxer_and_video_track("./avc.mp4"sv);
    auto first_sample = MUST(demuxer->get_next_sample_for_track(track));
    auto decoder = create_decoder_if_available(first_sample);
    if (!decoder)
        return;

    Vector<AK::Duration> presented_timestamps;
    auto drain = [&] {
        while (true) {
            auto decoded_frame_result = decoder->take_next_output(track.video_data().cicp);
            if (decoded_frame_result.is_error())
                return;
            presented_timestamps.append(decoded_frame_result.value()->timestamp());
        }
    };

    static constexpr size_t REFERENCE_ONLY_SAMPLE_COUNT = 8;
    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Reference));
    drain();
    for (size_t index = 1; index < REFERENCE_ONLY_SAMPLE_COUNT; index++) {
        auto sample = MUST(demuxer->get_next_sample_for_track(track));
        MUST(decoder->receive_coded_data(sample, Media::DecodeIntent::Reference));
        drain();
    }

    auto wanted_sample = MUST(demuxer->get_next_sample_for_track(track));
    auto wanted_timestamp = wanted_sample.presentation_timestamp();
    MUST(decoder->receive_coded_data(wanted_sample, Media::DecodeIntent::Output));
    decoder->signal_end_of_stream();
    drain();

    EXPECT_EQ(presented_timestamps.size(), 1u);
    if (!presented_timestamps.is_empty())
        EXPECT_EQ(presented_timestamps.first(), wanted_timestamp);
}

TEST_CASE(flush_restarts_decoding_from_a_keyframe)
{
    auto [demuxer, track] = create_demuxer_and_video_track("./avc.mp4"sv);
    auto first_sample = MUST(demuxer->get_next_sample_for_track(track));
    auto decoder = create_decoder_if_available(first_sample);
    if (!decoder)
        return;

    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));
    for (size_t index = 0; index < 10; index++)
        MUST(decoder->receive_coded_data(MUST(demuxer->get_next_sample_for_track(track)), Media::DecodeIntent::Output));
    decoder->flush();

    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));
    decoder->signal_end_of_stream();
    auto frame = MUST(decoder->take_next_output(track.video_data().cicp));
    EXPECT_EQ(frame->timestamp(), first_sample.presentation_timestamp());
    EXPECT_EQ(decoder->take_next_output(track.video_data().cicp).error().category(), Media::DecoderErrorCategory::EndOfStream);
}
