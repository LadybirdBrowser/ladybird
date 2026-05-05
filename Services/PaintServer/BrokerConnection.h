/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/SharedImagePayload.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibPaintServer/ToBrokerFromPaintServerEndpoint.h>
#include <LibPaintServer/ToPaintServerFromBrokerEndpoint.h>
#include <LibPaintServer/Types.h>

namespace PaintServer {

class Server;

class BrokerConnection final
    : public IPC::ConnectionFromClient<ToBrokerFromPaintServerEndpoint, ToPaintServerFromBrokerEndpoint> {
    C_OBJECT(BrokerConnection);

public:
    ~BrokerConnection() override = default;

    ConnectionID connection_id() const { return static_cast<ConnectionID>(client_id()); }
    pid_t client_pid() const { return m_client_pid; }

    void die() override;

private:
    BrokerConnection(Server&, NonnullOwnPtr<IPC::Transport>);

    void hello(RequestID request_id, ClientKind client_kind, int client_pid) override;

    void create_presentation_surface(SurfaceID surface_id, Gfx::IntSize size) override;
    void resize_presentation_surface(SurfaceID surface_id, Gfx::IntSize size) override;
    void set_surface_visible(SurfaceID surface_id, bool visible) override;
    void destroy_presentation_surface(SurfaceID surface_id) override;

    void register_presentation_buffers(SurfaceID surface_id, Vector<ImageID> image_ids, Vector<Gfx::SharedImagePayload> image_payloads, Gfx::IntSize size) override;

    void attach_render_client(IPC::TransportHandle handle) override;

    void did_present_or_released(SurfaceID surface_id, FrameID present_id) override;

    Server& m_server;
    pid_t m_client_pid { -1 };
};

}
