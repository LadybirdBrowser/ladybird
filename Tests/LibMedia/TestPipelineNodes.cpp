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
#include <LibMedia/MediaClock.h>
#include <LibMedia/MonotonicMediaClock.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Processors/AudioTimeStretchProcessor.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFramePool.h>
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

template<typename Condition>
static bool pump_until(Core::EventLoop& loop, Condition condition)
{
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(10);
    while (MonotonicTime::now_coarse() < deadline) {
        if (condition())
            return true;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    return false;
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
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            auto const& block = *output.block;
            EXPECT(!block.is_empty());
            EXPECT_EQ(block.channel_count(), 6);
            EXPECT_EQ(block.sample_specification().channel_map(), Audio::ChannelMap::surround_5_1());
            producer->consume();
            return;
        }
        if (MonotonicTime::now_coarse() - start_time >= time_limit)
            break;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    FAIL("Decoding timed out.");
}

TEST_CASE(audio_mixer_seek_during_playback_rewakes_consumer)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_5_1_underspecified.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));

    auto mixer = TRY_OR_FAIL(Media::AudioMixer::try_create());
    TRY_OR_FAIL(mixer->set_output_sample_specification(tracks[0].audio_data().sample_specification));
    TRY_OR_FAIL(mixer->connect_input(producer));

    bool consumer_woken = false;
    mixer->set_wake_handler([&] { consumer_woken = true; });

    mixer->start();

    // Play until the mixer is producing data, mirroring steady playback (its wake-forwarding state is
    // now latched as it would be then).
    EXPECT(pump_until(loop, [&] { return mixer->peek().status == Media::PipelineStatus::HaveData; }));
    mixer->consume();

    // Seek mid-playback. The consumer is now waiting to be woken (it is not polling anymore), so the
    // mixer must forward the producer's post-seek wake downstream or the seek never resolves.
    consumer_woken = false;
    mixer->seek(AK::Duration::zero());

    EXPECT(pump_until(loop, [&] { return consumer_woken; }));
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
            if (auto output = producer->peek(); output.frame != nullptr) {
                producer->consume();
                return output.frame;
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

TEST_CASE(video_producer_seeks_past_the_end_resolve_within_queued_data)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("vp9_oob_blocks.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0]));
    producer->start();

    auto pump_until = [&](auto condition) {
        auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(5);
        while (MonotonicTime::now_coarse() < deadline) {
            if (condition())
                return true;
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return false;
    };

    auto past_end = AK::Duration::from_seconds(100);
    producer->seek(past_end);
    EXPECT(pump_until([&] { return producer->peek().status == Media::PipelineStatus::HaveData; }));

    // Give the decode loop time to observe the demuxer's end of stream behind the queued final frame.
    auto fill_until = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(250);
    while (MonotonicTime::now_coarse() < fill_until)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    // Nothing exists beyond the queued final frame for the demuxer to re-emit, so a further seek past
    // the end must resolve with the queued frame instead of discarding it.
    producer->seek(past_end + AK::Duration::from_seconds(1));
    auto output = producer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::HaveData);
    EXPECT(output.frame != nullptr);
    producer->consume();
    EXPECT(pump_until([&] { return producer->peek().status == Media::PipelineStatus::EndOfStream; }));

    // With the final frame consumed, later seeks past the end resolve immediately at end of stream.
    producer->seek(past_end + AK::Duration::from_seconds(2));
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::EndOfStream);
}

static constexpr AK::Duration SHORT_AUTO_SUSPEND_TIMEOUT = AK::Duration::from_milliseconds(50);

static void sleep_past_the_idle_timeout()
{
    MUST(Core::System::sleep_ms(static_cast<u32>(SHORT_AUTO_SUSPEND_TIMEOUT.to_milliseconds() * 4)));
}

TEST_CASE(video_producer_auto_suspends_and_resumes_only_from_a_seek)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));
    producer->start();

    EXPECT(pump_until(loop, [&] { return producer->peek().status == Media::PipelineStatus::HaveData; }));

    // Left idle, the producer suspends. The checking peeks are spaced wider than the idle timeout
    // so they cannot defer it indefinitely.
    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // Peeking and consuming must not resume a suspended producer.
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);
        producer->consume();
    }
    sleep_past_the_idle_timeout();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);

    // A seek resumes decoding and wakes the waiting consumer.
    bool woken = false;
    producer->set_wake_handler([&] { woken = true; });
    producer->seek(AK::Duration::zero());
    EXPECT(pump_until(loop, [&] { return woken && producer->peek().status == Media::PipelineStatus::HaveData; }));
}

TEST_CASE(video_producer_suspends_while_frames_are_held_and_wakes_the_consumer)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));
    producer->start();

    // Hold every frame pool slot so the decoder parks waiting for a free slot, as it would behind
    // a paused sink holding frames downstream.
    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    EXPECT(pump_until(loop, [&] {
        auto output = producer->peek();
        if (output.frame != nullptr) {
            held_frames.append(output.frame.release_nonnull());
            producer->consume();
        }
        return held_frames.size() == Media::VideoFramePool::MAX_SLOT_COUNT;
    }));

    // The queue is drained, so the last observed status is a waiting one and entering suspension
    // must dispatch a wake.
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Pending);
    // Run any wake dispatch still queued from draining the frames above, so the wait below only
    // observes the wake dispatched by entering suspension.
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    bool woken = false;
    producer->set_wake_handler([&] { woken = true; });
    EXPECT(pump_until(loop, [&] { return woken; }));
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);
}

TEST_CASE(audio_producer_auto_suspends_and_resumes_only_from_a_seek)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));
    producer->start();

    EXPECT(pump_until(loop, [&] { return producer->peek().status == Media::PipelineStatus::HaveData; }));

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // A seek resumes decoding and wakes the waiting consumer.
    bool woken = false;
    producer->set_wake_handler([&] { woken = true; });
    producer->seek(AK::Duration::zero());
    EXPECT(pump_until(loop, [&] { return woken && producer->peek().status == Media::PipelineStatus::HaveData; }));
}

TEST_CASE(audio_mixer_resumes_a_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto mixer = TRY_OR_FAIL(Media::AudioMixer::try_create());
    TRY_OR_FAIL(mixer->set_output_sample_specification(tracks[0].audio_data().sample_specification));
    TRY_OR_FAIL(mixer->connect_input(producer));
    mixer->start();

    EXPECT(pump_until(loop, [&] { return mixer->peek().status == Media::PipelineStatus::HaveData; }));
    mixer->consume();

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // Pulling from the mixer again demand-seeks the suspended input and mixing resumes.
    EXPECT(pump_until(loop, [&] { return mixer->peek().status == Media::PipelineStatus::HaveData; }));
}

TEST_CASE(audio_time_stretch_processor_resumes_a_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto stretcher = TRY_OR_FAIL(Media::AudioTimeStretchProcessor::try_create());
    TRY_OR_FAIL(stretcher->set_output_sample_specification(tracks[0].audio_data().sample_specification));
    TRY_OR_FAIL(stretcher->connect_input(producer));
    stretcher->start();

    EXPECT(pump_until(loop, [&] { return stretcher->peek().status == Media::PipelineStatus::HaveData; }));
    stretcher->consume();

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // Pulling from the stretcher again demand-seeks the suspended input and stretching resumes.
    EXPECT(pump_until(loop, [&] { return stretcher->peek().status == Media::PipelineStatus::HaveData; }));
}

TEST_CASE(displaying_video_sink_reports_a_suspended_input_while_unticked)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto clock = TRY_OR_FAIL(Media::MonotonicMediaClock::try_create());
    auto sink = TRY_OR_FAIL(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Optional<Media::PipelineStatus> last_dispatched_status;
    sink->set_state_change_handler([&](Media::PipelineStatus status) { last_dispatched_status = status; });
    TRY_OR_FAIL(sink->connect_input(producer));

    EXPECT(pump_until(loop, [&] {
        (void)sink->update(MonotonicTime::now());
        return sink->current_frame() != nullptr;
    }));

    // With no further ticks, the suspension wake is the sink's only signal, and it must forward
    // the status to its user.
    EXPECT(pump_until(loop, [&] { return last_dispatched_status == Media::PipelineStatus::Suspended; }));
}

TEST_CASE(displaying_video_sink_demand_seeks_a_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto clock = TRY_OR_FAIL(Media::MonotonicMediaClock::try_create());
    auto sink = TRY_OR_FAIL(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    TRY_OR_FAIL(sink->connect_input(producer));

    // Display the first frame, then leave the sink unticked until the producer suspends.
    EXPECT(pump_until(loop, [&] {
        (void)sink->update(MonotonicTime::now());
        return sink->current_frame() != nullptr;
    }));
    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // Ticks while the displayed frame still covers the paused clock leave the input suspended.
    (void)sink->update(MonotonicTime::now());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);

    // Once the clock demands a time past the cached frames, the next tick seeks the input to
    // resume decoding at the current position.
    auto initial_frame = sink->current_frame();
    clock->seek(AK::Duration::from_seconds(1));
    EXPECT(pump_until(loop, [&] {
        (void)sink->update(MonotonicTime::now());
        return sink->current_frame() != nullptr && sink->current_frame() != initial_frame;
    }));
    EXPECT(sink->current_frame()->timestamp() > initial_frame->timestamp());
}
