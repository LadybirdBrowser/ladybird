/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/AudioDevices.h>
#include <LibMedia/Audio/PulseAudioWrappers.h>
#include <LibThreading/Thread.h>

namespace Media {

static ErrorOr<AudioDeviceEnumeration> enumerate_pulse_audio_devices()
{
    AudioDeviceEnumeration enumeration;
    auto context = TRY(Audio::PulseAudioContext::the());
    TRY(context->enumerate_audio_devices(enumeration.inputs, enumeration.outputs));
    return enumeration;
}

NonnullRefPtr<AudioDeviceEnumerationPromise> enumerate_platform_audio_devices()
{
    auto promise = AudioDeviceEnumerationPromise::construct();

    if (!Core::EventLoop::is_running()) {
        auto enumeration_or_error = enumerate_pulse_audio_devices();
        if (enumeration_or_error.is_error())
            promise->reject(enumeration_or_error.release_error());
        else
            promise->resolve(enumeration_or_error.release_value());
        return promise;
    }

    auto& main_thread_event_loop = Core::EventLoop::current();
    auto thread_or_error = Threading::Thread::try_create("AudioDevEnum"sv, [&main_thread_event_loop, promise]() mutable {
        auto enumeration_or_error = enumerate_pulse_audio_devices();
        main_thread_event_loop.deferred_invoke([promise, enumeration_or_error = move(enumeration_or_error)]() mutable {
            if (enumeration_or_error.is_error())
                promise->reject(enumeration_or_error.release_error());
            else
                promise->resolve(enumeration_or_error.release_value());
        });
        return 0;
    });
    if (thread_or_error.is_error()) {
        promise->reject(thread_or_error.release_error());
        return promise;
    }
    auto thread = thread_or_error.release_value();
    thread->start();
    thread->detach();
    return promise;
}

}
