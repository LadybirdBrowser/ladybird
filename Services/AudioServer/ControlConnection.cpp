/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/ControlConnection.h>
#include <AudioServer/Tabs.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>

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

Messages::AudioServerControl::InitTransportResponse ControlConnection::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#else
    did_misbehave("Unexpected audio server transport initialization");
    return 0;
#endif
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

}
