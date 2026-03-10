/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioDevices.h>

namespace Media {

AudioDevices& AudioDevices::the()
{
    static AudioDevices devices;
    return devices;
}

void AudioDevices::attach_to_audio_server_client(NonnullRefPtr<AudioServer::SessionClientOfAudioServer> client)
{
    if (m_audio_server_client.ptr() == client.ptr())
        return;

    auto existing_devices_changed_handler = move(client->on_devices_changed);
    client->on_devices_changed = [existing_devices_changed_handler = move(existing_devices_changed_handler)]() mutable {
        AudioDevices::the().refresh();
        if (existing_devices_changed_handler)
            existing_devices_changed_handler();
    };

    m_audio_server_client = move(client);
    refresh();
}

void AudioDevices::refresh()
{
    if (!m_audio_server_client)
        return;

    auto result = m_audio_server_client->get_devices(
        [this](Vector<AudioServer::DeviceInfo> const& devices) {
            Vector<AudioDeviceInfo> input_devices;
            Vector<AudioDeviceInfo> output_devices;
            input_devices.ensure_capacity(devices.size());
            output_devices.ensure_capacity(devices.size());

            for (auto const& device : devices) {
                if (device.type == AudioServer::DeviceInfo::Type::Input) {
                    input_devices.append(device);
                    continue;
                }

                if (device.type == AudioServer::DeviceInfo::Type::Output)
                    output_devices.append(device);
            }

            m_cached_input_devices = move(input_devices);
            m_cached_output_devices = move(output_devices);
            notify_listeners();
        },
        [](ByteString const&) {
        });
    if (result.is_error())
        return;
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
