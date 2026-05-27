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
#include <Compositor/CompositorState.h>
#include <Compositor/Forward.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Compositor {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint>
    , public CompositorStateClient {
    C_OBJECT(ConnectionFromClient);

public:
    virtual ~ConnectionFromClient() override = default;

private:
    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, RefPtr<Gfx::SkiaBackendContext>, bool async_scrolling_enabled);

    virtual void die() override;

    virtual void did_allocate_backing_stores(Web::Compositor::CompositorContextId, i32 front_bitmap_id, Gfx::SharedImage&& front_backing_store, i32 back_bitmap_id, Gfx::SharedImage&& back_backing_store) override;
    virtual void did_present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect content_rect, i32 bitmap_id) override;

    virtual Messages::CompositorControlServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::CompositorControlServer::ConnectWebContentResponse connect_web_content() override;
    virtual void create_context(Web::Compositor::CompositorContextId, Optional<u64> page_id, Web::Compositor::PagePresentationRegistration, i32 web_content_connection_id) override;
    virtual void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize, Web::Compositor::WindowResizingInProgress) override;
    virtual Messages::CompositorControlServer::HandleMouseEventResponse handle_mouse_event(Web::Compositor::CompositorContextId, Web::MouseEvent) override;
    virtual Messages::CompositorControlServer::DispatchMouseEventToWebContentResponse dispatch_mouse_event_to_web_content(Web::Compositor::CompositorContextId, Web::MouseEvent) override;
    virtual Messages::CompositorControlServer::AsyncScrollByResponse async_scroll_by(Web::Compositor::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels) override;
    virtual void presented_bitmap_ready_to_paint(Web::Compositor::CompositorContextId, i32 bitmap_id) override;

    ConnectionFromWebContent* web_content_connection(i32 web_content_connection_id);

    HashMap<i32, NonnullRefPtr<ConnectionFromWebContent>> m_web_content_connections;
    NonnullRefPtr<CompositorState> m_compositor_state;
};

}
