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

    {
        auto stream_result = Audio::PlaybackStream::create(Audio::OutputState::Playing, 100, [](Audio::SampleSpecification) {}, [](Span<float> buffer) -> ReadonlySpan<float> { return buffer.trim(0); });
        if (stream_result.is_error())
            dbgln("Failed to create playback stream: {}", stream_result.error());
        EXPECT_EQ(!stream_result.is_error(), has_implementation);
        if (has_implementation)
            EXPECT_EQ(stream_result.value()->total_time_played(), AK::Duration::zero());
    }

#    if defined(HAVE_PULSEAUDIO)
    // The PulseAudio context is kept alive by the PlaybackStream's control thread, which blocks on
    // some operations, so it won't necessarily be destroyed immediately.
    auto wait_start = MonotonicTime::now_coarse();
    while (Audio::PulseAudioContext::is_connected()) {
        if (MonotonicTime::now_coarse() - wait_start > AK::Duration::from_milliseconds(100))
            VERIFY_NOT_REACHED();
    }
#    endif
}
#endif
