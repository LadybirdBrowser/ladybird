/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioDevices.h>

namespace Media {

#if !defined(LIBMEDIA_AUDIO_DEVICE_ENUMERATION)

// FIXME: Implement device enumeration for the WASAPI (Windows) backend.
void watch_platform_audio_devices(AudioDeviceListCallback on_device_list)
{
    on_device_list(AudioDeviceEnumeration {});
}

#endif

AudioDevices& AudioDevices::the()
{
    static AudioDevices& devices = *new AudioDevices;
    devices.ensure_watching();
    return devices;
}

void AudioDevices::ensure_watching()
{
    if (m_watching)
        return;
    m_watching = true;
    watch_platform_audio_devices([this](ErrorOr<AudioDeviceEnumeration> enumeration) {
        report_device_list(move(enumeration));
    });
}

void AudioDevices::report_device_list(ErrorOr<AudioDeviceEnumeration> enumeration)
{
    if (enumeration.is_error()) {
        // The last list stands until the source manages a new one.
        warnln("Failed to enumerate audio devices: {}", enumeration.error());
    } else {
        m_cached_input_devices = move(enumeration.value().inputs);
        m_cached_output_devices = move(enumeration.value().outputs);
    }
    m_has_device_list = true;
    notify_listeners();
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
