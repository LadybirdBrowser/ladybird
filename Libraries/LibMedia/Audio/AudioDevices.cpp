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

void AudioDevices::refresh()
{
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
