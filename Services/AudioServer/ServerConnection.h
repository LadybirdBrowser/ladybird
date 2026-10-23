/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AudioServer/CaptureDevices.h>
#include <AudioServer/PlaybackStreamMixer.h>
#include <LibAudio/AudioClientEndpoint.h>
#include <LibAudio/AudioDevices.h>
#include <LibAudio/AudioServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>

namespace Audio {

// The server end of one client process's audio connection; each stream becomes a client of the tab's mixer.
class ServerConnection final
    : public IPC::ConnectionFromClient<AudioClientEndpoint, AudioServerEndpoint> {
    C_OBJECT(ServerConnection);

public:
    // What the client is told about devices; a headless server has none to tell of.
    enum class DeviceEnumeration : u8 {
        Platform,
        None,
    };

    // Asks the Browser whether the client's tab may capture from the device; the answer comes once, on this thread.
    using CapturePermissionRequest = Function<void(ByteString const& device_id, Function<void(bool allowed)> on_decided)>;

    virtual ~ServerConnection() override;

    // Runs when the client disconnects, after its streams have left the mixer.
    Function<void()> on_death;

    // Whether the Browser says the client's tab holds the microphone permission. Without it the client learns of the
    // devices by their existence alone, and the capture streams it has are closed.
    void set_capture_permitted(bool);

private:
    ServerConnection(NonnullOwnPtr<IPC::Transport>, int client_id, NonnullRefPtr<PlaybackStreamMixer>, DeviceEnumeration, NonnullRefPtr<CaptureDevices>, CapturePermissionRequest);

    virtual void die() override;
    void remove_streams_from_mixer();
    void remove_record_streams();
    void stop_listening_for_device_changes();

    virtual void declare_session(u64 session_id, PlaybackGroupId group) override;
    virtual void rebind_session(u64 session_id, PlaybackGroupId group) override;
    virtual void destroy_session(u64 session_id) override;
    virtual void create_stream(u64 stream_id, u64 session_id) override;
    virtual void destroy_stream(u64 stream_id) override;
    virtual void attach_stream_ring(u64 stream_id, SharedAudioFrameRing ring) override;
    virtual void resume_stream(u64 stream_id, u64 request_id) override;
    virtual void drain_stream(u64 stream_id, u64 request_id) override;
    virtual void discard_stream(u64 stream_id, u64 request_id) override;
    virtual void set_stream_volume(u64 stream_id, float volume) override;
    virtual void watch_devices() override;
    virtual void create_record_stream(u64 stream_id, ByteString device_id) override;
    virtual void destroy_record_stream(u64 stream_id) override;

    void devices_changed();
    void send_device_list();
    void capture_permission_decided(u64 stream_id, ByteString const& device_id, bool allowed);
    void capture_device_ready(u64 stream_id, CaptureSubscriberId, ErrorOr<SampleSpecification> const&);

    struct StreamState {
        MixerClientId mixer_client_id { 0 };
        u64 session_id { 0 };
        bool has_ring { false };
        float volume { 1 };
    };
    StreamState* find_stream(u64 stream_id, StringView operation);
    // The streams of a destroyed session belong to no group, and play into nothing.
    void move_streams_of_session_to(u64 session_id, Optional<PlaybackGroupId>);

    // No subscriber while the Browser is being asked.
    struct RecordStreamState {
        Optional<CaptureSubscriberId> subscriber_id;
    };

    NonnullRefPtr<PlaybackStreamMixer> m_mixer;
    HashMap<u64, PlaybackGroupId> m_session_groups;
    HashMap<u64, StreamState> m_streams;

    NonnullRefPtr<CaptureDevices> m_capture_devices;
    CapturePermissionRequest m_request_capture_permission;
    HashMap<u64, RecordStreamState> m_record_streams;
    bool m_capture_permitted { false };

    DeviceEnumeration m_device_enumeration { DeviceEnumeration::None };
    Optional<AudioDevices::ListenerId> m_devices_changed_listener_id;
    bool m_client_watches_devices { false };
};

}
