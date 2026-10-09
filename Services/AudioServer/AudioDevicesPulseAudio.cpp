/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AudioServer/PlatformAudio.h>
#include <AudioServer/PulseAudioWrappers.h>
#include <LibAudio/AudioDevices.h>
#include <LibCore/EventLoop.h>
#include <LibThreading/ThreadPool.h>

namespace Audio {

static ErrorOr<AudioDeviceEnumeration> enumerate_pulse_audio_devices()
{
    AudioDeviceEnumeration enumeration;
    auto context = TRY(Audio::PulseAudioContext::the());
    TRY(context->enumerate_audio_devices(enumeration.inputs, enumeration.outputs));
    return enumeration;
}

struct AudioDeviceWatch {
    Core::EventLoop* event_loop { nullptr };
    AudioDeviceListCallback on_device_list;
    // The subscription dies with the context, which is otherwise released along with the last stream.
    RefPtr<PulseAudioContext> context;
};

// The watch lasts for the rest of the process.
static NeverDestroyed<AudioDeviceWatch> s_audio_device_watch;

// Listing blocks on round trips to the daemon, so it happens on the pool and reports back on the watching loop.
static void report_devices_from_the_pool()
{
    Threading::ThreadPool::the().submit([] {
        auto enumeration = enumerate_pulse_audio_devices();
        s_audio_device_watch->event_loop->deferred_invoke([enumeration = move(enumeration)]() mutable {
            s_audio_device_watch->on_device_list(move(enumeration));
        });
    });
}

void watch_platform_audio_devices(AudioDeviceListCallback on_device_list)
{
    // Without an event loop there is nobody to tell of changes, so this is a one-off listing.
    if (!Core::EventLoop::is_running()) {
        on_device_list(enumerate_pulse_audio_devices());
        return;
    }

    VERIFY(s_audio_device_watch->event_loop == nullptr);
    s_audio_device_watch->event_loop = &Core::EventLoop::current();
    s_audio_device_watch->on_device_list = move(on_device_list);

    // Connecting to the daemon blocks too.
    Threading::ThreadPool::the().submit([] {
        auto context = Audio::PulseAudioContext::the();
        if (!context.is_error()) {
            if (auto result = context.value()->watch_devices([] { report_devices_from_the_pool(); }); result.is_error())
                warnln("Unable to watch PulseAudio devices for changes: {}", result.error());
            else
                s_audio_device_watch->context = context.value();
        }
        auto enumeration = enumerate_pulse_audio_devices();
        s_audio_device_watch->event_loop->deferred_invoke([enumeration = move(enumeration)]() mutable {
            s_audio_device_watch->on_device_list(move(enumeration));
        });
    });
}

}
