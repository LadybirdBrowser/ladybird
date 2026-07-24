/*
 * Copyright (c) 2023-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibMedia/Audio/NullPlaybackStream.h>
#include <LibTest/TestSuite.h>

TEST_CASE(default_playback_stream_can_be_created_and_suspended)
{
    Core::EventLoop event_loop;

    Atomic<u32> request_count { 0 };
    RefPtr<Audio::PlaybackStream> stream;
    bool created = false;
    Audio::PlaybackStream::create_platform_or_null(Audio::OutputState::Suspended, 10, [&](Span<float> buffer) -> ReadonlySpan<float> {
            buffer.fill(0);
            request_count.fetch_add(1);
            return buffer; })
        ->when_resolved([&](auto& created_stream) {
            stream = created_stream;
            created = true;
        })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    event_loop.spin_until([&] { return created; });
    EXPECT(stream->sample_specification().is_valid());

    Atomic<bool> resumed { false };
    stream->resume()
        ->when_resolved([&](auto) { resumed.store(true); })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    auto poll_timer = Core::Timer::create_repeating(1, [] { });
    poll_timer->start();
    event_loop.spin_until([&] { return resumed.load() && request_count.load() > 0; });

    Atomic<bool> suspended { false };
    stream->discard_buffer_and_suspend()
        ->when_resolved([&] { suspended.store(true); })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    event_loop.spin_until([&] { return suspended.load(); });
    poll_timer->stop();
}

TEST_CASE(null_playback_stream_pulls_and_tracks_time)
{
    Core::EventLoop event_loop;

    Atomic<u32> request_count { 0 };
    auto stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 10, [&](Span<float> buffer) -> ReadonlySpan<float> {
        request_count.fetch_add(1);
        return buffer;
    });

    EXPECT_EQ(request_count.load(), 0u);
    EXPECT_EQ(stream->sample_specification().sample_rate(), 44100u);
    EXPECT_EQ(stream->sample_specification().channel_map(), Audio::ChannelMap::stereo());

    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    auto poll_timer = Core::Timer::create_repeating(1, [] { });
    poll_timer->start();
    event_loop.spin_until([&] { return request_count.load() > 0; });
    event_loop.spin_until([&] { return stream->total_time_played() > AK::Duration::zero(); });
    poll_timer->stop();

    stream->discard_buffer_and_suspend()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    bool discarded = false;
    auto timer = Core::Timer::create_single_shot(20, [&] { discarded = true; });
    timer->start();
    event_loop.spin_until([&] { return discarded; });
    auto suspended_time = stream->total_time_played();

    bool checked_suspension = false;
    timer = Core::Timer::create_single_shot(20, [&] {
        EXPECT_EQ(stream->total_time_played(), suspended_time);
        checked_suspension = true;
    });
    timer->start();
    event_loop.spin_until([&] { return checked_suspension; });
}

TEST_CASE(null_playback_stream_recovers_from_underrun)
{
    Core::EventLoop event_loop;

    Atomic<u32> request_count { 0 };
    auto stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 10, [&](Span<float> buffer) -> ReadonlySpan<float> {
        if (request_count.fetch_add(1) == 0)
            return {};
        return buffer;
    });
    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    auto poll_timer = Core::Timer::create_repeating(1, [&] {
        stream->notify_data_available();
    });
    poll_timer->start();
    event_loop.spin_until([&] { return request_count.load() > 1; });
    poll_timer->stop();
}

TEST_CASE(null_playback_stream_resume_completes_pending_drain)
{
    Core::EventLoop event_loop;

    auto stream = Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, 1000, [](Span<float> buffer) -> ReadonlySpan<float> {
        return buffer;
    });
    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    auto poll_timer = Core::Timer::create_repeating(1, [] { });
    poll_timer->start();
    event_loop.spin_until([&] { return stream->total_time_played() > AK::Duration::zero(); });

    Atomic<bool> drained { false };
    stream->drain_buffer_and_suspend()
        ->when_resolved([&] { drained.store(true); })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    bool checked_drain = false;
    auto timer = Core::Timer::create_single_shot(50, [&] {
        EXPECT(drained.load());
        checked_drain = true;
    });
    timer->start();
    event_loop.spin_until([&] { return checked_drain; });
    poll_timer->stop();
}
