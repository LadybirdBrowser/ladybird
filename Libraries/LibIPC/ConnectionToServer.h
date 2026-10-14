/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Connection.h>

namespace IPC {

template<typename ClientEndpoint, typename ServerEndpoint, typename LocalStub = typename ClientEndpoint::Stub>
class ConnectionToServer : public IPC::Connection<ClientEndpoint, ServerEndpoint>
    , public LocalStub
    , public ServerEndpoint::Proxy {
public:
    using IPCProxy = typename ServerEndpoint::Proxy;

    ConnectionToServer(LocalStub& local_endpoint, NonnullOwnPtr<Transport> transport)
        : Connection<ClientEndpoint, ServerEndpoint>(local_endpoint, move(transport))
        , ServerEndpoint::Proxy(*this, {})
    {
    }

    virtual void die() override
    {
        // Override this function if you don't want your app to exit if it loses the connection.
        exit(0);
    }
};

}
