/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromClient.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>

namespace Compositor {

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint>(*this, move(transport), 1)
{
}

void ConnectionFromClient::die()
{
    Core::EventLoop::current().quit(0);
}

Messages::CompositorControlServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

Messages::CompositorControlServer::ConnectWebContentResponse ConnectionFromClient::connect_web_content()
{
    auto paired_transport = MUST(IPC::Transport::create_paired());
    auto web_content_connection_id = m_next_web_content_connection_id++;
    auto connection = ConnectionFromWebContent::construct(move(paired_transport.local), web_content_connection_id);
    connection->on_death = [this](auto& dead) {
        m_web_content_connections.remove(dead.client_id());
    };
    m_web_content_connections.set(web_content_connection_id, move(connection));
    async_did_connect_web_content(web_content_connection_id);
    return { move(paired_transport.remote_handle), web_content_connection_id };
}

}
