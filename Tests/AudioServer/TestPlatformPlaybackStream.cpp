/*
 * Copyright (c) 2023-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NeverDestroyed.h>
#include <AudioServer/PlatformAudio.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>

TEST_CASE(platform_playback_stream_can_be_created_and_suspended)
{
    static NeverDestroyed<Core::EventLoop> event_loop;

    Atomic<u32> request_count { 0 };
    RefPtr<Audio::PlaybackStream> stream;
    bool created = false;
    bool rejected = false;
    Audio::create_platform_playback_stream(Audio::OutputState::Suspended, 10, [&](Span<float> buffer, MonotonicTime) -> ReadonlySpan<float> {
        buffer.fill(0);
        request_count.fetch_add(1);
        return buffer;
    })->when_resolved([&](auto& created_stream) {
          stream = created_stream;
          created = true;
      }).when_rejected([&](Error const& error) {
        warnln("Skipping: no output device to open ({})", error);
        rejected = true;
    });

    event_loop->spin_until([&] { return created || rejected; });
    if (rejected)
        return;
    EXPECT(stream->sample_specification().is_valid());

    Atomic<bool> resumed { false };
    stream->resume()
        ->when_resolved([&] { resumed.store(true); })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });

    auto poll_timer = Core::Timer::create_repeating(1, [] { });
    poll_timer->start();
    event_loop->spin_until([&] { return resumed.load() && request_count.load() > 0; });

    Atomic<bool> suspended { false };
    stream->discard_buffer_and_suspend()
        ->when_resolved([&] { suspended.store(true); })
        .when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
    event_loop->spin_until([&] { return suspended.load(); });
    poll_timer->stop();
}
