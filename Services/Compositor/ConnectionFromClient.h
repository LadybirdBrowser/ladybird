/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <Compositor/CompositorControlClientEndpoint.h>
#include <Compositor/CompositorControlServerEndpoint.h>
#include <Compositor/Forward.h>
#include <LibIPC/ConnectionFromClient.h>

namespace Compositor {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint> {
    C_OBJECT(ConnectionFromClient)

public:
    virtual void die() override;

private:
    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::CompositorControlServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::CompositorControlServer::ConnectWebContentResponse connect_web_content() override;

    i32 m_next_web_content_connection_id { 1 };
    HashMap<i32, NonnullRefPtr<ConnectionFromWebContent>> m_web_content_connections;
};

}
