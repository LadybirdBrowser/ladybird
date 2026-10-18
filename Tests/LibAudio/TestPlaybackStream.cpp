/*
 * Copyright (c) 2023-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <LibAudio/NullPlaybackStream.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestSuite.h>

static Core::EventLoop& never_destroyed_event_loop()
{
    static NeverDestroyed<Core::EventLoop> s_event_loop;
    return *s_event_loop;
}

TEST_CASE(null_playback_stream_completes_interleaved_controls_in_order)
{
    Mutex mutex;
    ConditionVariable condition { mutex };
    Vector<u32> completions;
    auto record_completion = [&](u32 index) {
        MutexLocker locker(mutex);
        completions.append(index);
        condition.signal();
    };

    RefPtr<Audio::PlaybackStream> stream;
    stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 10, [&](Span<float>, MonotonicTime) -> ReadonlySpan<float> {
        // Queue all controls from the data callback so the output thread cannot resolve any of them until the batch is ready.
        stream->drain_buffer_and_suspend()
            ->when_resolved([&] { record_completion(0); })
            .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
        stream->resume()
            ->when_resolved([&] { record_completion(1); })
            .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
        stream->set_volume(0.5)
            ->when_resolved([&] { record_completion(2); })
            .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
        stream->discard_buffer_and_suspend()
            ->when_resolved([&] { record_completion(3); })
            .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
        stream->resume()
            ->when_resolved([&] { record_completion(4); })
            .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
        stream->discard_buffer_and_suspend()
            ->when_resolved([&] { record_completion(5); })
            .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
        return {};
    });
    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    MutexLocker locker(mutex);
    condition.wait_while([&] { return completions.size() < 6; });
    for (u32 index = 0; index < completions.size(); ++index)
        EXPECT_EQ(completions[index], index);
}

TEST_CASE(null_playback_stream_pulls_only_while_playing)
{
    auto& event_loop = never_destroyed_event_loop();

    Atomic<u32> request_count { 0 };
    auto stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 10, [&](Span<float> buffer, MonotonicTime) -> ReadonlySpan<float> {
        request_count.fetch_add(1);
        return buffer;
    });

    EXPECT_EQ(request_count.load(), 0u);
    EXPECT_EQ(stream->sample_specification().sample_rate(), 44100u);
    EXPECT_EQ(stream->sample_specification().channel_map(), Audio::ChannelMap::stereo());

    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    auto poll_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [] { });
    poll_timer->start();
    event_loop.spin_until([&] { return request_count.load() > 0; });

    Atomic<bool> suspended { false };
    stream->discard_buffer_and_suspend()
        ->when_resolved([&] { suspended.store(true); })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    event_loop.spin_until([&] { return suspended.load(); });
    poll_timer->stop();
    auto request_count_when_suspended = request_count.load();

    bool checked_suspension = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(20), [&] {
        EXPECT_EQ(request_count.load(), request_count_when_suspended);
        checked_suspension = true;
    });
    timer->start();
    event_loop.spin_until([&] { return checked_suspension; });
}

TEST_CASE(null_playback_stream_dates_each_buffer_by_what_it_holds)
{
    auto& event_loop = never_destroyed_event_loop();

    auto resumed_at = MonotonicTime::now();
    Atomic<u32> request_count { 0 };
    Atomic<bool> every_buffer_dated_by_what_it_holds { true };
    // Only the output thread reads and writes this.
    auto previous_buffer_ends_at = resumed_at;
    auto stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 100, [&](Span<float> buffer, MonotonicTime buffer_starts_playing_at) -> ReadonlySpan<float> {
        // Each buffer starts where the one before it ends, or later after an underrun, and never more than the target
        // after it was requested, however late the output thread runs.
        auto now = MonotonicTime::now();
        if (buffer_starts_playing_at < previous_buffer_ends_at - AK::Duration::from_milliseconds(1) || buffer_starts_playing_at > now + AK::Duration::from_milliseconds(100))
            every_buffer_dated_by_what_it_holds.store(false);
        previous_buffer_ends_at = buffer_starts_playing_at + AK::Duration::from_time_units(buffer.size() / 2, 1, 44100);
        request_count.fetch_add(1);
        return buffer;
    });

    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    auto poll_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [] { });
    poll_timer->start();
    event_loop.spin_until([&] { return request_count.load() > 2; });
    EXPECT(every_buffer_dated_by_what_it_holds.load());
    poll_timer->stop();
    stream->discard_buffer_and_suspend()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
}

TEST_CASE(null_playback_stream_recovers_from_underrun)
{
    auto& event_loop = never_destroyed_event_loop();

    Atomic<u32> request_count { 0 };
    auto stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 10, [&](Span<float> buffer, MonotonicTime) -> ReadonlySpan<float> {
        if (request_count.fetch_add(1) == 0)
            return {};
        return buffer;
    });
    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    auto poll_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [&] {
        stream->notify_data_available();
    });
    poll_timer->start();
    event_loop.spin_until([&] { return request_count.load() > 1; });
    poll_timer->stop();
}

TEST_CASE(null_playback_stream_resume_completes_pending_drain)
{
    auto& event_loop = never_destroyed_event_loop();

    Atomic<u32> request_count { 0 };
    auto stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 1000, [&](Span<float> buffer, MonotonicTime) -> ReadonlySpan<float> {
        request_count.fetch_add(1);
        return buffer;
    });
    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    auto poll_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(1), [] { });
    poll_timer->start();
    event_loop.spin_until([&] { return request_count.load() > 0; });

    Atomic<bool> drained { false };
    stream->drain_buffer_and_suspend()
        ->when_resolved([&] { drained.store(true); })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    bool checked_drain = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(50), [&] {
        EXPECT(drained.load());
        checked_drain = true;
    });
    timer->start();
    event_loop.spin_until([&] { return checked_drain; });
    poll_timer->stop();
}
