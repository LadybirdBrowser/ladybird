/*
 * Copyright (c) 2023-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/WeakPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibTest/TestSuite.h>

#if defined(HAVE_PULSEAUDIO)
#    include <LibMedia/Audio/PulseAudioWrappers.h>
#endif

// We are unable to create a playback stream on windows without an audio output device, so this would fail in CI
#if !defined(AK_OS_WINDOWS)

TEST_CASE(create_and_destroy_playback_stream)
{
    Core::EventLoop event_loop;

    bool has_implementation = false;
#    if defined(HAVE_PULSEAUDIO) || defined(AK_OS_MACOS)
    has_implementation = true;
#    endif

    bool done = false;

    Audio::PlaybackStream::create(Audio::OutputState::Playing, 100, [](Span<float> buffer) -> ReadonlySpan<float> { return buffer.trim(0); })
        ->when_resolved([&](auto& stream) {
            if (!has_implementation)
                VERIFY_NOT_REACHED();

            EXPECT_EQ(stream->total_time_played(), AK::Duration::zero());

            for (int i = 0; i < 5; i++) {
                stream->resume()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
                stream->drain_buffer_and_suspend()->when_rejected([](Error const&) { VERIFY_NOT_REACHED(); });
            }

            done = true;
        })
        .when_rejected([&](auto& error) {
            if (has_implementation) {
                dbgln("Failed to create playback stream: {}", error);
                VERIFY_NOT_REACHED();
            }

            done = true;
        });

    event_loop.spin_until([&] { return done; });

#    if defined(HAVE_PULSEAUDIO)
    // The PulseAudio context is kept alive by the PlaybackStream's control thread, which blocks on
    // some operations, so it won't necessarily be destroyed immediately.
    auto wait_start = MonotonicTime::now_coarse();
    while (Audio::PulseAudioContext::is_connected()) {
        if (MonotonicTime::now_coarse() - wait_start > AK::Duration::from_milliseconds(1000))
            VERIFY_NOT_REACHED();
    }
#    endif
}
#endif
