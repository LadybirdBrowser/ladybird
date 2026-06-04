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
#include <LibTest/TestCase.h>

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
    Core::EventLoop loop;

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
