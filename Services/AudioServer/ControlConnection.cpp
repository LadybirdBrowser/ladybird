/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/ControlConnection.h>
#include <AudioServer/Tabs.h>
#include <LibCore/Process.h>

namespace AudioServer {

ControlConnection::ControlConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<AudioServerControlClientEndpoint, AudioServerControlEndpoint>(*this, move(transport), 0)
{
}

ControlConnection::~ControlConnection() = default;

void ControlConnection::die()
{
    Core::Process::terminate_immediately(0);
}

Messages::AudioServerControl::ConnectNewClientResponse ControlConnection::connect_new_client(u64 tab_id)
{
    auto handle = Tabs::the().connect_client(tab_id);
    if (handle.is_error()) {
        dbgln("Failed to connect an audio client: {}", handle.error());
        return OptionalNone {};
    }
    return handle.release_value();
}

void ControlConnection::set_capture_permission(u64 tab_id, bool granted)
{
    Tabs::the().set_capture_permission(tab_id, granted);
}

void ControlConnection::capture_decided(u64 request_id, bool allowed)
{
    Tabs::the().capture_decided(request_id, allowed);
}

void ControlConnection::authorize_playback_group(u64 tab_id, Audio::PlaybackGroupId group)
{
    Tabs::the().set_playback_group_authorization(tab_id, group, Audio::PlaybackGroupAuthorization::Authorized);
}

void ControlConnection::revoke_playback_group(u64 tab_id, Audio::PlaybackGroupId group)
{
    Tabs::the().set_playback_group_authorization(tab_id, group, Audio::PlaybackGroupAuthorization::Unauthorized);
}

void ControlConnection::mute_playback_group(u64 tab_id, Audio::PlaybackGroupId group)
{
    Tabs::the().set_playback_group_mute_state(tab_id, group, Audio::PlaybackGroupMuteState::Muted);
}

void ControlConnection::unmute_playback_group(u64 tab_id, Audio::PlaybackGroupId group)
{
    Tabs::the().set_playback_group_mute_state(tab_id, group, Audio::PlaybackGroupMuteState::Unmuted);
}

}
