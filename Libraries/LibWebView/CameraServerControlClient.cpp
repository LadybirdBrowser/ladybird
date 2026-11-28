/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CameraServerControlClient.h>

namespace WebView {

CameraServerControlClient::CameraServerControlClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CameraServerControlClientEndpoint, CameraServerControlEndpoint>(*this, move(transport))
{
}

CameraServerControlClient::~CameraServerControlClient() = default;

void CameraServerControlClient::die()
{
    // The holder launches a new CameraServer when it next connects a client; the clients of this one hear about it from
    // their own connections.
    m_devices.clear();
    if (on_death)
        on_death();
}

void CameraServerControlClient::capture_requested(u64 tab_id, u64 request_id, ByteString device_id, u64 page_label)
{
    if (on_capture_requested)
        on_capture_requested(tab_id, request_id, device_id, page_label);
}

void CameraServerControlClient::devices_changed(Vector<Camera::DeviceInfo> devices)
{
    m_devices = move(devices);
}

void CameraServerControlClient::capture_activity_changed(u64 tab_id, u64 page_label, bool capturing)
{
    // Reports queued before the connection closed still arrive, from a CameraServer that is gone.
    if (!is_open())
        return;
    if (on_capture_activity_changed)
        on_capture_activity_changed(tab_id, page_label, capturing);
}

}
