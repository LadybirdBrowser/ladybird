/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <CameraServer/CameraServerControlClientEndpoint.h>
#include <CameraServer/CameraServerControlEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>

namespace CameraServer {

// The control plane of the CameraServer, spoken only by the Browser over the initial socket. It brokers one connection
// per client process; the server exits when the Browser drops it, which it does with the renderer it serves.
class ControlConnection final
    : public IPC::ConnectionFromClient<CameraServerControlClientEndpoint, CameraServerControlEndpoint> {
    C_OBJECT(ControlConnection);

public:
    virtual ~ControlConnection() override;

    virtual void die() override;

private:
    explicit ControlConnection(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::CameraServerControl::ConnectNewClientResponse connect_new_client(u64 tab_id) override;
    virtual void set_capture_permission(u64 tab_id, bool granted) override;
    virtual void set_preferred_device(ByteString device_id) override;
    virtual void capture_decided(u64 request_id, bool allowed) override;
};

}
