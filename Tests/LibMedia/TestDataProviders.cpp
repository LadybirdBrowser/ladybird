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
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>
#include <LibTest/TestCase.h>

// The following tests attempt to reproduce a race condition in AudioDataProvider and VideoDataProvider
// where rapidly transitioning through states None -> Suspended -> Exit can cause the decoder thread to
// continue with a null decoder.

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

static constexpr size_t iterations = 100;

TEST_CASE(audio_provider_suspend_then_exit)
{
    Core::EventLoop loop;

    for (size_t i = 0; i < iterations; i++) {
        auto stream = load_test_file("test-webm-xiph-lacing.mka"sv);
        auto demuxer = create_demuxer(stream);
        auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
        VERIFY(track.has_value());

        auto provider = TRY_OR_FAIL(Media::AudioDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track.release_value()));

        provider->suspend();
        MUST(Core::System::sleep_ms(1));
    }
}

TEST_CASE(video_provider_suspend_then_exit)
{
    Core::EventLoop loop;

    for (size_t i = 0; i < iterations; i++) {
        auto stream = load_test_file("vp9_in_webm.webm"sv);
        auto demuxer = create_demuxer(stream);
        auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
        VERIFY(track.has_value());

        auto provider = TRY_OR_FAIL(Media::VideoDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track.release_value()));

        provider->suspend();
        MUST(Core::System::sleep_ms(1));
    }
}

TEST_CASE(audio_provider_start_suspend_then_exit)
{
    Core::EventLoop loop;

    for (size_t i = 0; i < iterations; i++) {
        auto stream = load_test_file("test-webm-xiph-lacing.mka"sv);
        auto demuxer = create_demuxer(stream);
        auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
        VERIFY(track.has_value());

        auto provider = TRY_OR_FAIL(Media::AudioDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track.release_value()));

        provider->start();
        MUST(Core::System::sleep_ms(1));
        provider->suspend();
        MUST(Core::System::sleep_ms(1));
    }
}

TEST_CASE(video_provider_start_suspend_then_exit)
{
    Core::EventLoop loop;

    for (size_t i = 0; i < iterations; i++) {
        auto stream = load_test_file("vp9_in_webm.webm"sv);
        auto demuxer = create_demuxer(stream);
        auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
        VERIFY(track.has_value());

        auto provider = TRY_OR_FAIL(Media::VideoDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track.release_value()));

        provider->start();
        MUST(Core::System::sleep_ms(1));
        provider->suspend();
        MUST(Core::System::sleep_ms(1));
    }
}

TEST_CASE(audio_provider_underspecified_5_1_channel_map)
{
    Core::EventLoop loop;

    auto stream = load_test_file("WAV/tone_44100_5_1_underspecified.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());

    auto provider = TRY_OR_FAIL(Media::AudioDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track.release_value()));

    provider->start();

    auto time_limit = AK::Duration::from_seconds(1);
    auto start_time = MonotonicTime::now_coarse();

    while (true) {
        auto block = provider->retrieve_block();
        if (!block.is_empty()) {
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
