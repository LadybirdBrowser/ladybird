/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/SpscAudioFrameRing.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio {

extern "C" RustFFI::FfiSharedCppObject web_audio_analyser_create_ring(RustFFI::AudioFrameRingCallbacks*);
RustFFI::FfiSharedCppObject web_audio_analyser_create_ring(RustFFI::AudioFrameRingCallbacks* callbacks)
{
    auto* ring = new Audio::SpscAudioFrameRing(32768, 1);
    *callbacks = {
        [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->channel_count(); },
        [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_available(); },
        [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_free(); },
        [](void* ring, Span<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_pop(samples); },
        [](void* ring, ReadonlySpan<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_push(samples); },
    };
    return { ring, [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->ref(); }, [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->unref(); } };
}

}
