/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AudioServer/AudioServerControlClientEndpoint.h>
#include <AudioServer/AudioServerControlEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>

namespace AudioServer {

// The control plane of the AudioServer, spoken only by the Browser over the initial socket. It brokers one playback
// connection per client process; the server exits when the Browser drops it, which it does with the renderer it serves.
class ControlConnection final
    : public IPC::ConnectionFromClient<AudioServerControlClientEndpoint, AudioServerControlEndpoint> {
    C_OBJECT(ControlConnection);

public:
    virtual ~ControlConnection() override;

    virtual void die() override;

private:
    explicit ControlConnection(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::AudioServerControl::ConnectNewClientResponse connect_new_client(u64 tab_id) override;
    virtual void set_capture_permission(u64 tab_id, bool granted) override;
    virtual void capture_decided(u64 request_id, bool allowed) override;
    virtual void authorize_playback_group(u64 tab_id, Audio::PlaybackGroupId group) override;
    virtual void revoke_playback_group(u64 tab_id, Audio::PlaybackGroupId group) override;
    virtual void mute_playback_group(u64 tab_id, Audio::PlaybackGroupId group) override;
    virtual void unmute_playback_group(u64 tab_id, Audio::PlaybackGroupId group) override;
};

}
