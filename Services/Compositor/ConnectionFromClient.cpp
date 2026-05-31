/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <Compositor/ConnectionFromClient.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>

namespace Compositor {

static IDAllocator s_web_content_connection_ids;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool async_scrolling_enabled)
    : IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint>(*this, move(transport), 1)
    , m_compositor_state(CompositorState::create(move(skia_backend_context), async_scrolling_enabled))
{
    m_compositor_state->set_client(*this);
}

void ConnectionFromClient::die()
{
    for (auto& [id, connection] : m_web_content_connections)
        connection->notify_compositor_lost();
    Core::EventLoop::current().quit(0);
}

void ConnectionFromClient::did_allocate_backing_stores(Web::Compositor::CompositorContextId context_id, i32 front_bitmap_id, Gfx::SharedImage&& front_backing_store, i32 back_bitmap_id, Gfx::SharedImage&& back_backing_store)
{
    async_did_allocate_backing_stores(context_id, front_bitmap_id, move(front_backing_store), back_bitmap_id, move(back_backing_store));
}

void ConnectionFromClient::did_present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect content_rect, i32 bitmap_id)
{
    async_did_present_frame(context_id, content_rect, bitmap_id);
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
    auto web_content_connection_id = s_web_content_connection_ids.allocate();
    auto connection = ConnectionFromWebContent::construct(move(paired_transport.local), m_compositor_state, web_content_connection_id);
    connection->set_on_death([this](ConnectionFromWebContent& dead) {
        auto client_id = dead.client_id();
        m_web_content_connections.remove(client_id);
        s_web_content_connection_ids.deallocate(client_id);
    });
    m_web_content_connections.set(web_content_connection_id, move(connection));
    return { move(paired_transport.remote_handle), web_content_connection_id };
}

void ConnectionFromClient::create_context(Web::Compositor::CompositorContextId context_id, Optional<u64> page_id, i32 web_content_connection_id)
{
    auto* connection = web_content_connection(web_content_connection_id);
    VERIFY(connection);
    m_compositor_state->create_context(context_id, page_id, *connection);
}

void ConnectionFromClient::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    m_compositor_state->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

void ConnectionFromClient::set_display_metadata(Web::Compositor::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    m_compositor_state->set_display_metadata(context_id, display_id, refresh_rate);
}

Messages::CompositorControlServer::HandleMouseEventResponse ConnectionFromClient::handle_mouse_event(Web::Compositor::CompositorContextId context_id, Web::MouseEvent event)
{
    return m_compositor_state->handle_mouse_event(context_id, event);
}

Messages::CompositorControlServer::DispatchMouseEventToWebContentResponse ConnectionFromClient::dispatch_mouse_event_to_web_content(Web::Compositor::CompositorContextId context_id, Web::MouseEvent event)
{
    return m_compositor_state->dispatch_mouse_event_to_web_content(context_id, event);
}

Messages::CompositorControlServer::AsyncScrollByResponse ConnectionFromClient::async_scroll_by(Web::Compositor::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
{
    return m_compositor_state->async_scroll_by(context_id, position, delta_in_device_pixels);
}

void ConnectionFromClient::presented_bitmap_ready_to_paint(Web::Compositor::CompositorContextId context_id, i32 bitmap_id)
{
    m_compositor_state->presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void ConnectionFromClient::crash()
{
    warnln("Crashing Compositor process by request from Browser");
    VERIFY_NOT_REACHED();
}

ConnectionFromWebContent* ConnectionFromClient::web_content_connection(i32 web_content_connection_id)
{
    auto it = m_web_content_connections.find(web_content_connection_id);
    if (it == m_web_content_connections.end())
        return nullptr;
    return it->value.ptr();
}

}
