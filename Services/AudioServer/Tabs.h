/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/WeakPtr.h>
#include <LibIPC/TransportHandle.h>
#include <LibMedia/Audio/CaptureDevices.h>
#include <LibMedia/Audio/PlaybackStreamMixer.h>
#include <LibMedia/Audio/ServerConnection.h>
#include <LibMedia/AudioOutput.h>

namespace AudioServer {

class ControlConnection;

// The tabs with connected client processes: each has a mixer, and the Browser is asked before any of them captures.
class Tabs {
public:
    static Tabs& the();

    // Decides what every tab's device stream is; headless instances discard their mix into a null stream and have no
    // devices to capture from.
    void set_audio_output(Media::AudioOutput audio_output) { m_audio_output = audio_output; }
    // The Browser rules on every capture request, and is asked over this connection.
    void set_control_connection(ControlConnection&);

    ErrorOr<IPC::TransportHandle> connect_client(u64 tab_id);
    void capture_decided(u64 request_id, bool allowed);

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
    RefPtr<Audio::CaptureDevices> m_capture_devices;
    int m_next_client_id { 1 };
    Media::AudioOutput m_audio_output { Media::AudioOutput::Platform };

    WeakPtr<ControlConnection> m_control_connection;
    HashMap<u64, Function<void(bool)>> m_pending_capture_decisions;
    u64 m_next_capture_request_id { 1 };
};

}
