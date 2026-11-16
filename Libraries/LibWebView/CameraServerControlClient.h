/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <CameraServer/CameraServerControlClientEndpoint.h>
#include <CameraServer/CameraServerControlEndpoint.h>
#include <LibCamera/DeviceInfo.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Export.h>

namespace WebView {

// The control plane of a renderer's CameraServer, spoken only by the Browser, which launched it.
class WEBVIEW_API CameraServerControlClient final
    : public IPC::ConnectionToServer<CameraServerControlClientEndpoint, CameraServerControlEndpoint>
    , public CameraServerControlClientEndpoint {
    C_OBJECT_ABSTRACT(CameraServerControlClient);

public:
    explicit CameraServerControlClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~CameraServerControlClient() override;

    // A client of the tab asks to capture from the camera for a page; the holder answers with capture_decided.
    Function<void(u64 tab_id, u64 request_id, ByteString const& device_id, u64 page_label)> on_capture_requested;
    Function<void(u64 tab_id, u64 page_label, bool capturing)> on_capture_activity_changed;
    // The cameras by name as the CameraServer last told of them, the default one marked.
    Vector<Camera::DeviceInfo> const& devices() const { return m_devices; }
    // All capture streams are lost with the server.
    Function<void()> on_death;

private:
    virtual void die() override;
    virtual void capture_requested(u64 tab_id, u64 request_id, ByteString device_id, u64 page_label) override;
    virtual void capture_activity_changed(u64 tab_id, u64 page_label, bool capturing) override;
    virtual void devices_changed(Vector<Camera::DeviceInfo> devices) override;

    Vector<Camera::DeviceInfo> m_devices;
};

}
