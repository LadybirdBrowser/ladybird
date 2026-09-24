/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>
#include <MediaServer/Forward.h>
#include <MediaServer/MediaClientEndpoint.h>
#include <MediaServer/MediaServerEndpoint.h>

namespace MediaServer {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<MediaClientEndpoint, MediaServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    // The Browser holds the controller connection and brokers the one renderer connection. The server serves that
    // one renderer, so it exits when either connection goes away.
    enum class Role {
        Controller,
        Renderer,
    };

    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, Role);
    ~ConnectionFromClient() override = default;

    virtual void die() override;

private:
    virtual Messages::MediaServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::MediaServer::ConnectNewClientResponse connect_new_client() override;

    static ErrorOr<IPC::TransportHandle> create_renderer_connection();

    Role m_role { Role::Renderer };
};

}
