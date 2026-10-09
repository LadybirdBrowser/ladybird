/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/AudioDevices.h>
#include <LibAudio/ClientConnection.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Audio {

AudioDevices& AudioDevices::the()
{
    static AudioDevices& devices = *new AudioDevices;
    devices.ensure_watching();
    return devices;
}

void AudioDevices::ensure_watching()
{
    // The AudioServer reports its own devices; a process with no server to reach has none.
    if (m_watching || !Audio::ClientConnection::has_transport_factory())
        return;
    m_watching = true;

    auto connection = Audio::ClientConnection::acquire();
    if (connection.is_error()) {
        // Listeners hear of the failure first, since they may well come back here looking for the list.
        report_device_list(connection.release_error());
        m_watching = false;
        return;
    }
    connection.value()->watch_devices([this](ErrorOr<AudioDeviceEnumeration> enumeration) {
        if (enumeration.is_error()) {
            m_watching = false;
            return;
        }
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

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Audio::AudioDeviceInfo const& device)
{
    TRY(encoder.encode(device.dom_device_id));
    TRY(encoder.encode(device.label));
    TRY(encoder.encode(device.group_id));
    TRY(encoder.encode(device.sample_rate_hz));
    TRY(encoder.encode(device.channel_count));
    TRY(encoder.encode(device.is_default));
    return {};
}

template<>
ErrorOr<Audio::AudioDeviceInfo> decode(Decoder& decoder)
{
    Audio::AudioDeviceInfo device;
    device.dom_device_id = TRY(decoder.decode<ByteString>());
    device.label = TRY(decoder.decode<ByteString>());
    device.group_id = TRY(decoder.decode<ByteString>());
    device.sample_rate_hz = TRY(decoder.decode<u32>());
    device.channel_count = TRY(decoder.decode<u32>());
    device.is_default = TRY(decoder.decode<bool>());
    return device;
}

}
