/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
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

TEST_CASE(create_and_destroy_playback_stream)
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
    }

#if defined(HAVE_PULSEAUDIO)
    // The PulseAudio context is kept alive by the PlaybackStream's control thread, which blocks on
    // some operations, so it won't necessarily be destroyed immediately.
    auto wait_start = MonotonicTime::now_coarse();
    while (Audio::PulseAudioContext::is_connected()) {
        if (MonotonicTime::now_coarse() - wait_start > AK::Duration::from_milliseconds(100))
            VERIFY_NOT_REACHED();
    }
#endif
}
