/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/SessionManagement.h>
#include <LibIPC/Connection.h>

namespace IPC {

template<typename ClientEndpoint, typename ServerEndpoint>
class ConnectionToServer : public IPC::Connection<ClientEndpoint, ServerEndpoint>
    , public ClientEndpoint::Stub
    , public ServerEndpoint::template Proxy<ClientEndpoint> {
public:
    using ClientStub = typename ClientEndpoint::Stub;
    using IPCProxy = typename ServerEndpoint::template Proxy<ClientEndpoint>;

    ConnectionToServer(ClientStub& local_endpoint, Transport transport)
        : Connection<ClientEndpoint, ServerEndpoint>(local_endpoint, move(transport))
        , ServerEndpoint::template Proxy<ClientEndpoint>(*this, {})
    {
    }

    virtual void die() override
    {
        // Override this function if you don't want your app to exit if it loses the connection.
        exit(0);
    }
};

}
