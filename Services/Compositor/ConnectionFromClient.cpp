/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/Math.h>
#include <Compositor/ConnectionFromClient.h>
#include <Compositor/ConnectionFromPresentation.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibCompositing/FontServiceClient.h>
#include <LibCompositing/UIOverlays.h>
#include <LibCore/Process.h>
#include <LibIPC/Transport.h>

namespace Compositor {

static IDAllocator s_web_content_connection_ids;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
    : IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint>(*this, move(transport), 1)
    , m_compositor_state(CompositorState::create(move(skia_backend_context)))
{
}

void ConnectionFromClient::die()
{
    for (auto& [id, connection] : m_web_content_connections)
        connection->notify_compositor_lost();
    Core::Process::terminate_immediately(0);
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

void ConnectionFromClient::connect_render_clock(i32 web_content_connection_id, IPC::TransportHandle handle)
{
    if (auto connection = m_web_content_connections.get(web_content_connection_id); connection.has_value())
        (*connection)->install_render_clock_channel(move(handle));
}

void ConnectionFromClient::connect_video_presentation(i32 web_content_connection_id, IPC::TransportHandle handle)
{
    if (auto connection = m_web_content_connections.get(web_content_connection_id); connection.has_value())
        (*connection)->install_video_presentation_channel(move(handle));
}

Messages::CompositorControlServer::CreatePresentationConnectionResponse ConnectionFromClient::create_presentation_connection(bool synthesizes_scroll_momentum, bool scrolls_elastically, Optional<u64> gpu_presentation_adapter_luid)
{
    auto paired_transport = MUST(IPC::Transport::create_paired());
    auto presentation_id = m_next_presentation_id++;
    // NB: The capabilities come with the connection, so the first backing stores of a context bound to it match them.
    auto capabilities = CompositorState::PresentationCapabilities {
        .synthesizes_scroll_momentum = synthesizes_scroll_momentum,
        .scrolls_elastically = scrolls_elastically,
        .gpu_presentation_adapter_luid = gpu_presentation_adapter_luid,
    };
    auto connection = ConnectionFromPresentation::construct(move(paired_transport.local), m_compositor_state, capabilities);
    connection->set_on_death([this, presentation_id] {
        m_presentation_connections.remove(presentation_id);
    });
    m_presentation_connections.set(presentation_id, move(connection));
    return { move(paired_transport.remote_handle), presentation_id };
}

void ConnectionFromClient::bind_presentation(Web::CompositorContextId context_id, u64 presentation_id)
{
    auto connection = m_presentation_connections.get(presentation_id);
    if (!connection.has_value()) {
        dbgln("Compositor: Ignoring binding of context {} to presentation {}, which is already gone", context_id, presentation_id);
        return;
    }
    m_compositor_state->bind_presentation(context_id, **connection);
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

void ConnectionFromClient::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!isfinite(device_pixel_ratio) || device_pixel_ratio <= 0) {
        did_misbehave("Invalid device pixel ratio");
        return;
    }
    m_compositor_state->viewport_size_updated(context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
}

void ConnectionFromClient::set_ui_overlays(Web::CompositorContextId context_id, Compositing::UIOverlays overlays)
{
    m_compositor_state->set_ui_overlays(context_id, move(overlays));
}

void ConnectionFromClient::set_display_metadata(Web::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    m_compositor_state->set_display_metadata(context_id, display_id, refresh_rate);
}

void ConnectionFromClient::set_context_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility)
{
    m_compositor_state->set_context_visibility(context_id, visibility);
}

void ConnectionFromClient::set_context_presents_from_cpu_memory(Web::CompositorContextId context_id, bool presents_from_cpu_memory)
{
    m_compositor_state->set_context_presents_from_cpu_memory(context_id, presents_from_cpu_memory);
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
