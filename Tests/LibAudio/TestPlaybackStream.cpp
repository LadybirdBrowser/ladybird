/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/WeakPtr.h>
#include <LibAudio/PlaybackStream.h>
#include <LibCore/EventLoop.h>
#include <LibTest/TestSuite.h>
#include <unistd.h>

#if defined(HAVE_PULSEAUDIO)
#    include <LibAudio/PulseAudioWrappers.h>
#endif

// FIXME: CI doesn't run an AudioServer currently. Creating one in /etc/SystemServer.ini does not
//        allow this test to pass since CI runs in a Shell that will setsid() if it finds that the
//        current session ID is 0, and AudioServer's socket address depends on the current sid.
//        If we can fix that, this test can run on CI.
//        https://github.com/SerenityOS/serenity/issues/20538
#define STREAM_TEST TEST_CASE

STREAM_TEST(create_and_destroy_playback_stream)
{
    Core::EventLoop event_loop;

    bool has_implementation = false;
#if defined(HAVE_PULSEAUDIO) || defined(AK_OS_MACOS)
    has_implementation = true;
#endif

    {
        auto stream_result = Audio::PlaybackStream::create(Audio::OutputState::Playing, 44100, 2, 100, [](Bytes buffer, Audio::PcmSampleFormat format, size_t sample_count) -> ReadonlyBytes {
            VERIFY(format == Audio::PcmSampleFormat::Float32);
            FixedMemoryStream writing_stream { buffer };

            for (size_t i = 0; i < sample_count; i++) {
                MUST(writing_stream.write_value(0.0f));
                MUST(writing_stream.write_value(0.0f));
            }

            return buffer.trim(writing_stream.offset());
        });
        EXPECT_EQ(!stream_result.is_error(), has_implementation);
        usleep(10000);
    }

#if defined(HAVE_PULSEAUDIO)
    VERIFY(!Audio::PulseAudioContext::weak_instance());
#endif
}
