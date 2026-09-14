/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Sinks/AudioPullSink.h>
#include <LibTest/TestCase.h>
#include <unistd.h>

#include "TestMediaCommon.h"

static constexpr size_t QUANTUM_FRAME_COUNT = 128;

template<typename Condition>
static bool pump_until(Core::EventLoop& loop, Condition condition)
{
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(5);
    while (!condition()) {
        if (MonotonicTime::now_coarse() >= deadline)
            return false;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    return true;
}

// A mono sink at 8 kHz fed with the given number of frames, waited on until the first block has been queued.
static NonnullRefPtr<Media::AudioPullSink> create_mono_sink_with_frames(Core::EventLoop& loop, ScriptedAudioProducer& producer, size_t frame_count)
{
    auto sink = MUST(Media::AudioPullSink::try_create(8'000));
    auto have_data = make<Atomic<bool>>(false);
    sink->set_state_change_handler([have_data = have_data.ptr()](Media::PipelineStatus status) {
        if (status == Media::PipelineStatus::HaveData)
            have_data->store(true);
    });
    MUST(sink->set_channel_map(Audio::ChannelMap::mono()));
    MUST(sink->connect_input(producer));
    producer.append_block(0, frame_count);
    producer.wake();
    VERIFY(pump_until(loop, [&] { return have_data->load(); }));
    sink->set_state_change_handler(nullptr);
    return sink;
}

TEST_CASE(render_delivers_frames_only_for_the_current_channel_map)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto sink = TRY_OR_FAIL(Media::AudioPullSink::try_create(8'000));
    Vector<Media::PipelineStatus> statuses;
    sink->set_state_change_handler([&](Media::PipelineStatus status) { statuses.append(status); });
    TRY_OR_FAIL(sink->set_channel_map(Audio::ChannelMap::stereo()));
    TRY_OR_FAIL(sink->connect_input(producer));

    for (i64 first_frame_index = 0; first_frame_index < 16'000; first_frame_index += 2'000)
        producer->append_block(first_frame_index, 2'000);
    producer->wake();
    EXPECT(pump_until(loop, [&] { return statuses.contains_slow(Media::PipelineStatus::HaveData); }));

    Array<float, QUANTUM_FRAME_COUNT> left;
    Array<float, QUANTUM_FRAME_COUNT> right;
    Array<Span<float>, 2> stereo_channels { left.span(), right.span() };
    sink->resume();
    EXPECT_EQ(sink->render(stereo_channels, 0), QUANTUM_FRAME_COUNT);
    EXPECT_EQ(sink->channel_count(), 2u);
    EXPECT_EQ(left[1], 1.0f);
    EXPECT_EQ(right[1], 1.0f);

    statuses.clear();
    TRY_OR_FAIL(sink->set_channel_map(Audio::ChannelMap::mono()));
    for (i64 first_frame_index = 0; first_frame_index < 16'000; first_frame_index += 4'000)
        producer->append_block(first_frame_index, 4'000);
    producer->wake();
    EXPECT(pump_until(loop, [&] { return statuses.contains_slow(Media::PipelineStatus::HaveData); }));

    Array<Span<float>, 1> mono_channels { left.span() };
    EXPECT_EQ(sink->render(mono_channels, 0), QUANTUM_FRAME_COUNT);
    EXPECT_EQ(sink->channel_count(), 1u);

    // A renderer still sized for the previous channel map is given nothing rather than misinterleaved samples.
    EXPECT_EQ(sink->render(stereo_channels, 0), 0u);
}

TEST_CASE(muting_playback_output_does_not_mute_pull_sink)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto sink = TRY_OR_FAIL(Media::AudioPullSink::try_create(8'000));
    TRY_OR_FAIL(sink->set_channel_map(Audio::ChannelMap::mono()));
    TRY_OR_FAIL(sink->connect_input(producer));

    auto manager = Media::PlaybackManager::create();
    manager->set_audio_pull_sink(sink, true);
    manager->set_volume(0.5);
    manager->set_audio_output_muted(true);

    Atomic<bool> have_data { false };
    sink->set_state_change_handler([&](Media::PipelineStatus status) {
        if (status == Media::PipelineStatus::HaveData)
            have_data.store(true);
    });
    producer->append_block(0, QUANTUM_FRAME_COUNT);
    producer->wake();
    EXPECT(pump_until(loop, [&] { return have_data.load(); }));

    Array<float, QUANTUM_FRAME_COUNT> samples;
    Array<Span<float>, 1> channels { samples.span() };
    sink->resume();
    EXPECT_EQ(sink->render(channels, 0), QUANTUM_FRAME_COUNT);
    EXPECT_EQ(samples[1], 0.5f);
}

TEST_CASE(clock_follows_the_frames_that_were_rendered)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto sink = create_mono_sink_with_frames(loop, producer, 1'024);
    auto clock = sink->time_reader();

    Array<float, QUANTUM_FRAME_COUNT> samples;
    Array<Span<float>, 1> channels { samples.span() };
    sink->resume();
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(sink->render(channels, 0), QUANTUM_FRAME_COUNT);
    sink->pause();

    // 512 frames at 8 kHz were rendered within microseconds, so the clock reads the audio position, not the time spent.
    auto time_after_rendering = clock.current_time();
    EXPECT(time_after_rendering >= AK::Duration::from_milliseconds(64));
    EXPECT(time_after_rendering < AK::Duration::from_milliseconds(76));

    // Pausing with audio still queued ahead must not let the clock run into it.
    auto time_at_pause = clock.current_time();
    usleep(20'000);
    EXPECT(clock.current_time() == time_at_pause);
    sink->resume();

    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(sink->render(channels, 0), QUANTUM_FRAME_COUNT);
    sink->pause();
    auto time_after_draining = clock.current_time();
    EXPECT(time_after_draining >= AK::Duration::from_milliseconds(128));
    EXPECT(time_after_draining < AK::Duration::from_milliseconds(140));

    // Reads that find the queue empty hold the clock at the end of what was heard.
    sink->resume();
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(sink->render(channels, 0), 0u);
    sink->pause();
    auto time_while_starved = clock.current_time();
    EXPECT(time_while_starved >= time_after_draining);
    EXPECT(time_while_starved < AK::Duration::from_milliseconds(140));
}

TEST_CASE(clock_reports_the_position_being_heard)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto sink = create_mono_sink_with_frames(loop, producer, 1'024);
    auto clock = sink->time_reader();

    Array<float, QUANTUM_FRAME_COUNT> samples;
    Array<Span<float>, 1> channels { samples.span() };
    sink->resume();

    // The renderer is 64 frames ahead of the device, so only the first 64 rendered frames are audible yet.
    EXPECT_EQ(sink->render(channels, 64), QUANTUM_FRAME_COUNT);
    sink->pause();
    auto time = clock.current_time();
    EXPECT(time >= AK::Duration::from_milliseconds(8));
    EXPECT(time < AK::Duration::from_milliseconds(12));
}

TEST_CASE(output_latency_does_not_move_the_clock_before_a_seek_target)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto sink = TRY_OR_FAIL(Media::AudioPullSink::try_create(8'000));
    Atomic<bool> have_data { false };
    sink->set_state_change_handler([&](Media::PipelineStatus status) {
        if (status == Media::PipelineStatus::HaveData)
            have_data.store(true);
    });
    TRY_OR_FAIL(sink->set_channel_map(Audio::ChannelMap::mono()));
    TRY_OR_FAIL(sink->connect_input(producer));

    auto seek_target = AK::Duration::from_seconds(1);
    sink->seek(seek_target);
    producer->append_block(8'000, QUANTUM_FRAME_COUNT);
    producer->wake();
    EXPECT(pump_until(loop, [&] { return have_data.load(); }));

    Array<float, QUANTUM_FRAME_COUNT> samples;
    Array<Span<float>, 1> channels { samples.span() };
    sink->resume();
    EXPECT_EQ(sink->render(channels, QUANTUM_FRAME_COUNT * 2), QUANTUM_FRAME_COUNT);
    sink->pause();
    EXPECT_EQ(sink->time_reader().current_time(), seek_target);
}

TEST_CASE(clock_runs_on_wall_time_while_the_renderer_is_not_pulling)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto sink = create_mono_sink_with_frames(loop, producer, QUANTUM_FRAME_COUNT);
    auto clock = sink->time_reader();

    Array<float, QUANTUM_FRAME_COUNT> samples;
    Array<Span<float>, 1> channels { samples.span() };
    sink->resume();
    EXPECT_EQ(sink->render(channels, 0), QUANTUM_FRAME_COUNT);

    // With nothing queued ahead the clock would otherwise stay at the end of the rendered audio.
    sink->set_ticking(false);
    auto time_when_pulling_stopped = clock.current_time();
    usleep(30'000);
    auto elapsed = clock.current_time() - time_when_pulling_stopped;
    EXPECT(elapsed >= AK::Duration::from_milliseconds(25));
    EXPECT(elapsed < AK::Duration::from_milliseconds(100));

    sink->pause();
    auto time_at_pause = clock.current_time();
    usleep(20'000);
    EXPECT(clock.current_time() == time_at_pause);
    sink->resume();

    // Pulling again picks up from wherever the wall clock got to, discarding the audio queued before that position.
    sink->set_ticking(true);
    auto time_when_pulling_resumed = clock.current_time();
    EXPECT(time_when_pulling_resumed >= time_when_pulling_stopped + AK::Duration::from_milliseconds(25));
    producer->append_block(0, 4'096);
    producer->wake();
    EXPECT(pump_until(loop, [&] { return sink->render(channels, 0) == QUANTUM_FRAME_COUNT; }));
    EXPECT(samples[0] > 300.0f);
}
