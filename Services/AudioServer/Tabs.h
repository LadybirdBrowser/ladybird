/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/WeakPtr.h>
#include <AudioServer/CaptureDevices.h>
#include <AudioServer/PlaybackStreamMixer.h>
#include <AudioServer/ServerConnection.h>
#include <LibAudio/AudioOutput.h>
#include <LibAudio/PlaybackGroupId.h>
#include <LibIPC/TransportHandle.h>

namespace AudioServer {

class ControlConnection;

// The tabs with connected client processes: each has a mixer, and the Browser's word on whether its pages hold the
// microphone permission, on which playback groups may play, and on which the user has muted.
class Tabs {
public:
    static Tabs& the();

    // Decides what every tab's device stream is; headless instances discard their mix into a null stream and have no
    // devices to capture from.
    void set_audio_output(Audio::AudioOutput audio_output) { m_audio_output = audio_output; }
    // The Browser rules on every capture request, and is asked over this connection.
    void set_control_connection(ControlConnection&);

    ErrorOr<IPC::TransportHandle> connect_client(u64 tab_id);
    // Whether the tab's pages hold the permission: its clients, present and future, learn the devices' names only with
    // it, and the capture streams they have are closed without it.
    void set_capture_permission(u64 tab_id, bool granted);
    void capture_decided(u64 request_id, bool allowed);
    // Whether the tab's streams of the group may play, and whether the user has muted them. Both hold for the tab's
    // clients, present and future, since a client can reconnect while the Browser's word stands.
    void set_playback_group_authorization(u64 tab_id, Audio::PlaybackGroupId, Audio::PlaybackGroupAuthorization);
    void set_playback_group_mute_state(u64 tab_id, Audio::PlaybackGroupId, Audio::PlaybackGroupMuteState);

private:
    struct Tab {
        NonnullRefPtr<Audio::PlaybackStreamMixer> mixer;
        HashMap<int, NonnullRefPtr<Audio::ServerConnection>> connections;
    };

    Tab& tab_for(u64 tab_id);
    Audio::CaptureDevices& capture_devices();
    void connection_died(u64 tab_id, int client_id);
    void request_capture_permission(u64 tab_id, ByteString const& device_id, Function<void(bool)> on_decided);

    HashMap<u64, Tab> m_tabs;
    HashTable<u64> m_tabs_with_capture_permission;
    HashMap<u64, HashTable<Audio::PlaybackGroupId>> m_authorized_playback_groups;
    HashMap<u64, HashTable<Audio::PlaybackGroupId>> m_muted_playback_groups;
    RefPtr<Audio::CaptureDevices> m_capture_devices;
    int m_next_client_id { 1 };
    Audio::AudioOutput m_audio_output { Audio::AudioOutput::Platform };

    WeakPtr<ControlConnection> m_control_connection;
    HashMap<u64, Function<void(bool)>> m_pending_capture_decisions;
    u64 m_next_capture_request_id { 1 };
};

}
