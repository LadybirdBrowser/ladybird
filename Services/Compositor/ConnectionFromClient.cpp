/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/Math.h>
#include <Compositor/ConnectionFromClient.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibCompositing/FontServiceClient.h>
#include <LibCompositing/PausedDebuggerOverlay.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>

namespace Compositor {

static IDAllocator s_web_content_connection_ids;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
    : IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint>(*this, move(transport), 1)
    , m_compositor_state(CompositorState::create(move(skia_backend_context)))
{
    m_compositor_state->set_client(*this);
}

void ConnectionFromClient::die()
{
    for (auto& [id, connection] : m_web_content_connections)
        connection->notify_compositor_lost();
    Core::Process::terminate_immediately(0);
}

void ConnectionFromClient::did_allocate_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores)
{
    async_did_allocate_backing_stores(context_id, move(bitmap_ids), move(backing_stores));
}

void ConnectionFromClient::did_add_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores)
{
    async_did_add_backing_stores(context_id, move(bitmap_ids), move(backing_stores));
}

void ConnectionFromClient::did_retire_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids)
{
    async_did_retire_backing_stores(context_id, move(bitmap_ids));
}

void ConnectionFromClient::did_present_frame(Web::CompositorContextId context_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id)
{
    async_did_present_frame(context_id, content_rect, damage_rect, bitmap_id);
}

void ConnectionFromClient::did_consume_input_event(Web::CompositorContextId context_id, u64 event_id)
{
    async_did_consume_input_event(context_id, event_id);
}

void ConnectionFromClient::did_not_dispatch_input_event(Web::CompositorContextId context_id, u64 event_id)
{
    async_did_not_dispatch_input_event(context_id, event_id);
}

Messages::CompositorControlServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

void ConnectionFromClient::set_font_service(IPC::TransportHandle handle, IPC::File catalog, u64 catalog_size, u64 generation)
{
    MUST(Compositing::install_font_service(move(handle), move(catalog), catalog_size, generation));
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

void ConnectionFromClient::create_context(Web::CompositorContextId context_id, Optional<u64> page_id, i32 web_content_connection_id)
{
    auto* connection = web_content_connection(web_content_connection_id);
    if (!connection) {
        dbgln("Compositor: Ignoring context {} for WebContent connection {}, which is already gone", context_id, web_content_connection_id);
        return;
    }

    m_compositor_state->create_context(context_id, page_id, *connection);
}

void ConnectionFromClient::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    m_compositor_state->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

void ConnectionFromClient::set_paused_debugger_overlay(Web::CompositorContextId context_id, bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<u8> hovered_action_value)
{
    if (!isfinite(device_pixel_ratio) || device_pixel_ratio <= 0) {
        did_misbehave("Invalid device pixel ratio");
        return;
    }

    Optional<Compositing::PausedDebuggerOverlayAction> hovered_action;
    if (hovered_action_value.has_value()) {
        hovered_action = Compositing::paused_debugger_overlay_action_from_underlying(*hovered_action_value);
        if (!hovered_action.has_value()) {
            did_misbehave("Invalid paused debugger overlay action");
            return;
        }
    }
    m_compositor_state->set_paused_debugger_overlay(context_id, visible, device_pixel_ratio, move(font_family), hovered_action);
}

void ConnectionFromClient::set_display_metadata(Web::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    m_compositor_state->set_display_metadata(context_id, display_id, refresh_rate);
}

void ConnectionFromClient::set_context_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility)
{
    m_compositor_state->set_context_visibility(context_id, visibility);
}

void ConnectionFromClient::handle_and_dispatch_mouse_event(Web::CompositorContextId context_id, Web::MouseEvent event)
{
    m_compositor_state->handle_and_dispatch_mouse_event(context_id, move(event));
}

Messages::CompositorControlServer::HandleKeyEventResponse ConnectionFromClient::handle_key_event(Web::CompositorContextId context_id, Web::KeyEvent event)
{
    return m_compositor_state->handle_key_event(context_id, event);
}

Messages::CompositorControlServer::DispatchKeyEventToWebContentResponse ConnectionFromClient::dispatch_key_event_to_web_content(Web::CompositorContextId context_id, Web::KeyEvent event)
{
    return m_compositor_state->dispatch_key_event_to_web_content(context_id, event);
}

void ConnectionFromClient::handle_pinch_event(Web::CompositorContextId context_id, Web::PinchEvent event)
{
    m_compositor_state->handle_pinch_event(context_id, event);
}

void ConnectionFromClient::presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
{
    m_compositor_state->presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void ConnectionFromClient::set_client_gpu_presentation_capability(bool supported, u64 adapter_luid)
{
    m_compositor_state->set_client_gpu_presentation_capability(supported, adapter_luid);
}

void ConnectionFromClient::set_synthesizes_scroll_momentum(bool synthesizes_scroll_momentum)
{
    m_compositor_state->set_synthesizes_scroll_momentum(synthesizes_scroll_momentum);
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
