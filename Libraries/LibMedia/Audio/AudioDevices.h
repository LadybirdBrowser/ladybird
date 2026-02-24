/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibMedia/Export.h>

namespace Media {

using AudioDeviceHandle = AudioServer::DeviceHandle;
using AudioDeviceInfo = AudioServer::DeviceInfo;

class MEDIA_API AudioDevices {
public:
    static AudioDevices& the();

    void attach_to_audio_server_client(NonnullRefPtr<AudioServer::SessionClientOfAudioServer>);
    void refresh();

    Vector<AudioDeviceInfo> input_devices() const;
    Vector<AudioDeviceInfo> output_devices() const;

    using ListenerId = u64;
    ListenerId add_devices_changed_listener(Function<void()>);
    void remove_devices_changed_listener(ListenerId);

private:
    void notify_listeners();

    RefPtr<AudioServer::SessionClientOfAudioServer> m_audio_server_client;
    Vector<AudioDeviceInfo> m_cached_input_devices;
    Vector<AudioDeviceInfo> m_cached_output_devices;

    ListenerId m_next_listener_id { 1 };
    HashMap<ListenerId, Function<void()>> m_listeners;
};

}
