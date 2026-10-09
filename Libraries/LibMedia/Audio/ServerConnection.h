/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibMedia/Audio/AudioClientEndpoint.h>
#include <LibMedia/Audio/AudioDevices.h>
#include <LibMedia/Audio/AudioServerEndpoint.h>
#include <LibMedia/Audio/PlaybackStreamMixer.h>
#include <LibMedia/Export.h>

namespace Audio {

// The server end of one client process's audio connection; each stream becomes a client of the tab's mixer.
class MEDIA_API ServerConnection final
    : public IPC::ConnectionFromClient<AudioClientEndpoint, AudioServerEndpoint> {
    C_OBJECT(ServerConnection);

public:
    // What the client is told about devices; a headless server has none to tell of.
    enum class DeviceEnumeration : u8 {
        Platform,
        None,
    };

    virtual ~ServerConnection() override;

    // Runs when the client disconnects, after its streams have left the mixer.
    Function<void()> on_death;

private:
    ServerConnection(NonnullOwnPtr<IPC::Transport>, int client_id, NonnullRefPtr<PlaybackStreamMixer>, DeviceEnumeration);

    virtual void die() override;
    void remove_streams_from_mixer();
    void stop_listening_for_device_changes();

    virtual Messages::AudioServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual void create_stream(u64 stream_id) override;
    virtual void destroy_stream(u64 stream_id) override;
    virtual void attach_stream_ring(u64 stream_id, SharedAudioFrameRing ring) override;
    virtual void resume_stream(u64 stream_id, u64 request_id) override;
    virtual void drain_stream(u64 stream_id, u64 request_id) override;
    virtual void discard_stream(u64 stream_id, u64 request_id) override;
    virtual void set_stream_volume(u64 stream_id, float volume) override;
    virtual void watch_devices() override;

    void devices_changed();
    void send_device_list();

    struct StreamState {
        MixerClientId mixer_client_id { 0 };
        bool has_ring { false };
        float volume { 1 };
    };
    StreamState* find_stream(u64 stream_id, StringView operation);

    NonnullRefPtr<PlaybackStreamMixer> m_mixer;
    HashMap<u64, StreamState> m_streams;

    DeviceEnumeration m_device_enumeration { DeviceEnumeration::None };
    Optional<Media::AudioDevices::ListenerId> m_devices_changed_listener_id;
    bool m_client_watches_devices { false };
};

}
