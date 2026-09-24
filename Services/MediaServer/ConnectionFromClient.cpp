/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <MediaServer/ConnectionFromClient.h>

namespace MediaServer {

static NeverDestroyed<HashMap<int, NonnullRefPtr<ConnectionFromClient>>> s_connections;
static int s_next_client_id { 1 };

static int allocate_client_id()
{
    VERIFY(s_next_client_id != NumericLimits<int>::max());
    return s_next_client_id++;
}

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, Role role)
    : IPC::ConnectionFromClient<MediaClientEndpoint, MediaServerEndpoint>(*this, move(transport), allocate_client_id())
    , m_role(role)
{
    s_connections->set(client_id(), *this);
}

void ConnectionFromClient::die()
{
    s_connections->remove(client_id());
    Core::Process::terminate_immediately(0);
}

Messages::MediaServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#else
    did_misbehave("Unexpected media server transport initialization");
    return 0;
#endif
}

ErrorOr<IPC::TransportHandle> ConnectionFromClient::create_renderer_connection()
{
    auto paired_transports = TRY(IPC::Transport::create_paired());
    auto handle = move(paired_transports.remote_handle);

    // The static connection map owns this connection until its peer disconnects.
    auto client = adopt_ref(*new ConnectionFromClient(move(paired_transports.local), Role::Renderer));

    return handle;
}

Messages::MediaServer::ConnectNewClientResponse ConnectionFromClient::connect_new_client()
{
    if (m_role != Role::Controller) {
        did_misbehave("Only the controller may connect a new media client");
        return OptionalNone {};
    }

    auto handle = create_renderer_connection();
    if (handle.is_error()) {
        dbgln("Failed to connect a media client: {}", handle.error());
        return OptionalNone {};
    }
    return handle.release_value();
}

}
