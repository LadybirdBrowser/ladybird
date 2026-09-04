/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
        MUST(decoder->receive_coded_data(sample, Media::DecodeIntent::Output));

        while (true) {
            auto decoded_frame_result = decoder->take_next_output(new_track.video_data().cicp);
            if (decoded_frame_result.is_error()) {
                EXPECT_EQ(decoded_frame_result.error().category(), Media::DecoderErrorCategory::NeedsMoreInput);
                break;
            }
            EXPECT_EQ(decoded_frame_result.value()->size(), Gfx::Size<u32>(1280, 720));
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
    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));

    size_t frame_count = 0;
    auto last_timestamp = AK::Duration::min();

    auto take_decoded_frame = [&]() -> Media::DecoderErrorOr<void> {
        auto decoded_frame = TRY(decoder->take_next_output(track.video_data().cicp));

        EXPECT(last_timestamp <= decoded_frame->timestamp());
        EXPECT(!decoded_frame->duration().is_zero());
        last_timestamp = decoded_frame->timestamp();
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

        MUST(decoder->receive_coded_data(sample, Media::DecodeIntent::Output));
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

TEST_CASE(h264_reference_only_frames_are_decoded_but_not_produced)
{
    auto [demuxer, track] = create_demuxer_and_video_track("./avc.mp4"sv);

    auto first_sample = MUST(demuxer->get_next_sample_for_track(track));
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(first_sample.codec_id(), first_sample.new_codec_configuration().value()));

    Vector<AK::Duration> presented_timestamps;
    auto drain = [&] {
        while (true) {
            auto decoded_frame_result = decoder->take_next_output(track.video_data().cicp);
            if (decoded_frame_result.is_error())
                return;
            presented_timestamps.append(decoded_frame_result.value()->timestamp());
        }
    };

    // Only the last of these frames is wanted for display; the rest are decoded so it has something to reference.
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
    EXPECT_EQ(presented_timestamps.first(), wanted_timestamp);
}
