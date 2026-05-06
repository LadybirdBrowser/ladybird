/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/BrokerConnection.h>
#include <PaintServer/Server.h>

namespace PaintServer {

static IDAllocator s_connection_ids;
static HashMap<ConnectionID, RefPtr<BrokerConnection>> s_connections;

BrokerConnection::BrokerConnection(Server& server, NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ToBrokerFromPaintServerEndpoint, ToPaintServerFromBrokerEndpoint>(*this, move(transport), s_connection_ids.allocate())
    , m_server(server)
{
    s_connections.set(connection_id(), *this);

    async_server_epoch_changed(m_server.server_epoch());

    async_capabilities(0);
}

void BrokerConnection::die()
{
    s_connections.remove(connection_id());
    m_server.clear_broker_connection(*this);
}

void BrokerConnection::hello(RequestID request_id, ClientKind client_kind, int client_pid)
{
    (void)client_kind;
    if (client_pid > 0)
        m_client_pid = client_pid;

    async_did_hello(request_id, getpid(), m_server.server_epoch(), 0);
}

void BrokerConnection::create_presentation_surface(SurfaceID surface_id, Gfx::IntSize size)
{
    (void)m_server.set_presentation_surface(surface_id, size, false);
}

void BrokerConnection::resize_presentation_surface(SurfaceID surface_id, Gfx::IntSize size)
{
    (void)m_server.set_presentation_surface(surface_id, size, true);
}

void BrokerConnection::set_surface_visible(SurfaceID surface_id, bool visible)
{
    m_server.set_presentation_surface_visible(surface_id, visible);
}

void BrokerConnection::destroy_presentation_surface(SurfaceID surface_id)
{
    m_server.destroy_presentation_surface(surface_id);
}

void BrokerConnection::register_presentation_buffers(SurfaceID surface_id, Vector<ImageID> image_ids, Vector<Gfx::SharedImagePayload> image_payloads, Gfx::IntSize size)
{
    m_server.register_presentation_buffers(surface_id, move(image_ids), move(image_payloads), size);
}

void BrokerConnection::attach_render_client(IPC::TransportHandle handle)
{
    auto transport = handle.create_transport();
    if (transport.is_error()) {
        dbgln("PaintServer::BrokerConnection: failed to create render client transport: {}", transport.error());
        return;
    }

    (void)m_server.connect_render_client(transport.release_value());
}

void BrokerConnection::did_present_or_released(SurfaceID surface_id, FrameID present_id)
{
    m_server.did_present_or_released(surface_id, present_id);
}

}
