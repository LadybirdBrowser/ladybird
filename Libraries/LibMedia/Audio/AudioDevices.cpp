/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioDevices.h>

namespace Media {

#if !defined(LIBMEDIA_AUDIO_DEVICE_ENUMERATION)

// FIXME: Implement device enumeration for the WASAPI (Windows) backend.
NonnullRefPtr<AudioDeviceEnumerationPromise> enumerate_platform_audio_devices()
{
    return AudioDeviceEnumerationPromise::resolved(AudioDeviceEnumeration {});
}

#endif

AudioDevices& AudioDevices::the()
{
    static AudioDevices& devices = *new AudioDevices;

    static bool did_initial_refresh = false;
    if (!did_initial_refresh) {
        did_initial_refresh = true;
        devices.refresh();
    }

    return devices;
}

void AudioDevices::refresh()
{
    if (m_refresh_in_progress)
        return;

    m_refresh_in_progress = true;
    auto promise = enumerate_platform_audio_devices();
    promise->when_resolved([this](AudioDeviceEnumeration& enumeration) {
        m_cached_input_devices = move(enumeration.inputs);
        m_cached_output_devices = move(enumeration.outputs);
        m_refresh_in_progress = false;
        m_has_completed_refresh = true;
        notify_listeners();
    });
    promise->when_rejected([this](Error& error) {
        warnln("Failed to enumerate audio devices: {}", error);
        m_refresh_in_progress = false;
        m_has_completed_refresh = true;
        notify_listeners();
    });
}

Vector<AudioDeviceInfo> AudioDevices::input_devices() const
{
    return m_cached_input_devices;
}

Vector<AudioDeviceInfo> AudioDevices::output_devices() const
{
    return m_cached_output_devices;
}

AudioDevices::ListenerId AudioDevices::add_devices_changed_listener(Function<void()> listener)
{
    ListenerId listener_id = m_next_listener_id++;
    m_listeners.set(listener_id, move(listener));
    return listener_id;
}

void AudioDevices::remove_devices_changed_listener(ListenerId listener_id)
{
    m_listeners.remove(listener_id);
}

void AudioDevices::notify_listeners()
{
    Vector<ListenerId> listener_ids;
    listener_ids.ensure_capacity(m_listeners.size());
    for (auto const& listener : m_listeners)
        listener_ids.append(listener.key);

    for (auto listener_id : listener_ids) {
        auto callback = m_listeners.get(listener_id);
        if (!callback.has_value())
            continue;
        callback.value()();
    }
}

}
