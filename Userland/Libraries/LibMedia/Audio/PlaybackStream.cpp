/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaybackStream.h"

#include <AK/Platform.h>
#include <LibCore/ThreadedPromise.h>

#if defined(HAVE_PULSEAUDIO)
#    include "PlaybackStreamPulseAudio.h"
#elif defined(AK_OS_MACOS)
#    include "PlaybackStreamAudioUnit.h"
#elif defined(AK_OS_ANDROID)
#    include "PlaybackStreamOboe.h"
#endif

namespace Audio {

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStream::create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32 target_latency_ms, AudioDataRequestCallback&& data_request_callback)
{
    VERIFY(data_request_callback);
    // Create the platform-specific implementation for this stream.
#if defined(HAVE_PULSEAUDIO)
    return PlaybackStreamPulseAudio::create(initial_output_state, sample_rate, channels, target_latency_ms, move(data_request_callback));
#elif defined(AK_OS_MACOS)
    return PlaybackStreamAudioUnit::create(initial_output_state, sample_rate, channels, target_latency_ms, move(data_request_callback));
#elif defined(AK_OS_ANDROID)
    return PlaybackStreamOboe::create(initial_output_state, sample_rate, channels, target_latency_ms, move(data_request_callback));
#else
    (void)initial_output_state, (void)sample_rate, (void)channels, (void)target_latency_ms;
    return Error::from_string_literal("Audio output is not available for this platform");
#endif
}

}
