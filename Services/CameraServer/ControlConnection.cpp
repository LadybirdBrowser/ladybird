/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CameraServer/ControlConnection.h>
#include <CameraServer/Tabs.h>
#include <LibCore/Process.h>

namespace CameraServer {

ControlConnection::ControlConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<CameraServerControlClientEndpoint, CameraServerControlEndpoint>(*this, move(transport), 0)
{
}

ControlConnection::~ControlConnection() = default;

void ControlConnection::die()
{
    Core::Process::terminate_immediately(0);
}

Messages::CameraServerControl::ConnectNewClientResponse ControlConnection::connect_new_client(u64 tab_id)
{
    auto handle = Tabs::the().connect_client(tab_id);
    if (handle.is_error()) {
        dbgln("Failed to connect a camera client: {}", handle.error());
        return OptionalNone {};
    }
    return handle.release_value();
}

void ControlConnection::set_capture_permission(u64 tab_id, bool granted)
{
    Tabs::the().set_capture_permission(tab_id, granted);
}

void ControlConnection::set_preferred_device(ByteString device_id)
{
    Tabs::the().set_preferred_device(move(device_id));
}

void ControlConnection::capture_decided(u64 request_id, bool allowed)
{
    Tabs::the().capture_decided(request_id, allowed);
}

}
