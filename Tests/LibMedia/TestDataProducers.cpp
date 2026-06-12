/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/MediaTimeProvider.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/VideoFrame.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

static NonnullRefPtr<Media::IncrementallyPopulatedStream> load_test_file(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
}

static NonnullRefPtr<Media::Demuxer> create_demuxer(NonnullRefPtr<Media::IncrementallyPopulatedStream> const& stream)
{
    auto matroska_result = Media::Matroska::MatroskaDemuxer::from_stream(stream);
    if (!matroska_result.is_error())
        return matroska_result.release_value();
    return MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));
}

TEST_CASE(audio_producer_underspecified_5_1_channel_map)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_5_1_underspecified.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));

    producer->start();

    auto time_limit = AK::Duration::from_seconds(1);
    auto start_time = MonotonicTime::now_coarse();

    while (true) {
        Media::AudioBlock block;
        auto status = producer->status();
        if (status == Media::PipelineStatus::HaveData)
            producer->pull(block);
        if (status == Media::PipelineStatus::HaveData) {
            EXPECT(!block.is_empty());
            EXPECT_EQ(block.channel_count(), 6);
            EXPECT_EQ(block.sample_specification().channel_map(), Audio::ChannelMap::surround_5_1());
            return;
        }
        if (MonotonicTime::now_coarse() - start_time >= time_limit)
            break;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    FAIL("Decoding timed out.");
}

TEST_CASE(video_producer_seeks_while_frames_are_held)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("vp9_oob_blocks.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0]));
    producer->start();

    auto pull_next_frame = [&]() -> RefPtr<Media::VideoFrame> {
        auto time_limit = AK::Duration::from_seconds(10);
        auto start_time = MonotonicTime::now_coarse();
        while (MonotonicTime::now_coarse() - start_time < time_limit) {
            auto status = producer->status();
            if (status == Media::PipelineStatus::MovedPosition || status == Media::PipelineStatus::HaveData) {
                RefPtr<Media::VideoFrame> frame;
                producer->pull(frame);
                if (frame != nullptr)
                    return frame;
            }
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return nullptr;
    };

    // Hold a displaying sink's worth of frames plus a lagging media element reference, so the
    // seek below must decode towards its target while these frame pool slots stay occupied.
    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    for (int i = 0; i < 3; i++) {
        auto frame = pull_next_frame();
        if (frame == nullptr)
            FAIL("Timed out pulling the initial frames");
        held_frames.append(frame.release_nonnull());
    }

    // Give the producer time to fill its decode-ahead queue behind the held frames.
    auto fill_until = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(250);
    while (MonotonicTime::now_coarse() < fill_until)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    producer->seek(AK::Duration::from_seconds(2));

    if (pull_next_frame() == nullptr)
        FAIL("The seek did not resolve; the producer is likely starved of frame pool slots");
}
