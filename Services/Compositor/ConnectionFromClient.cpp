/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/Math.h>
#include <Compositor/ConnectionFromClient.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/SharedFontProvider.h>
#include <LibIPC/Transport.h>
#include <LibWebView/PausedDebuggerOverlay.h>

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
    Core::Process::terminate_immediately(0);
}

void ConnectionFromClient::did_allocate_backing_stores(Web::Compositor::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores)
{
    async_did_allocate_backing_stores(context_id, move(bitmap_ids), move(backing_stores));
}

void ConnectionFromClient::did_present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id)
{
    async_did_present_frame(context_id, content_rect, damage_rect, bitmap_id);
}

Messages::CompositorControlServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

void ConnectionFromClient::set_font_service_transport(IPC::TransportHandle handle)
{
    auto transport = handle.create_transport();
    if (transport.is_error()) {
        dbgln("Compositor: Unable to create font service transport: {}", transport.error());
        return;
    }

    m_font_client = FontClient::construct(transport.release_value());
#ifdef AK_OS_WINDOWS
    auto response = m_font_client->send_sync_but_allow_failure<FontClient::InitTransport>(Core::System::getpid());
    if (!response) {
        dbgln("Compositor: Unable to initialize font service transport");
        m_font_client = nullptr;
        return;
    }
    m_font_client->transport().set_peer_pid(response->peer_pid());
#endif
}

void ConnectionFromClient::set_font_catalog(IPC::File file, u64 size, u64 generation)
{
    if (m_font_provider) {
        if (auto result = m_font_provider->replace_catalog(move(file), size, generation); result.is_error())
            dbgln("Compositor: Unable to replace font catalog: {}", result.error());
        return;
    }

    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.open_font = [this](u64 requested_generation, u64 face_id) {
        if (!m_font_client)
            return Gfx::BrokeredFont {};
        auto response = m_font_client->send_sync_but_allow_failure<Messages::CompositorFontServer::OpenSystemFont>(requested_generation, face_id);
        if (!response || response->format() > to_underlying(Gfx::FontFileFormat::WOFF))
            return Gfx::BrokeredFont {};
        return Gfx::BrokeredFont {
            .face_id = response->matched_face_id(),
            .ttc_index = response->ttc_index(),
            .format = static_cast<Gfx::FontFileFormat>(response->format()),
            .file = response->take_file(),
        };
    };
    callbacks.match_font = [this](String const& family, u16 weight, u16 width, u8 slope) {
        if (!m_font_client)
            return Gfx::BrokeredFont {};
        auto response = m_font_client->send_sync_but_allow_failure<Messages::CompositorFontServer::MatchSystemFont>(family, weight, width, slope);
        if (!response || response->format() > to_underlying(Gfx::FontFileFormat::WOFF))
            return Gfx::BrokeredFont {};
        return Gfx::BrokeredFont {
            .face_id = response->face_id(),
            .ttc_index = response->ttc_index(),
            .format = static_cast<Gfx::FontFileFormat>(response->format()),
            .file = response->take_file(),
        };
    };
    callbacks.match_font_for_code_point = [this](u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji) {
        if (!m_font_client)
            return Gfx::BrokeredFont {};
        auto response = m_font_client->send_sync_but_allow_failure<Messages::CompositorFontServer::MatchSystemFontForCodePoint>(code_point, weight, width, slope, prefer_color_emoji);
        if (!response || response->format() > to_underlying(Gfx::FontFileFormat::WOFF))
            return Gfx::BrokeredFont {};
        return Gfx::BrokeredFont {
            .face_id = response->face_id(),
            .ttc_index = response->ttc_index(),
            .format = static_cast<Gfx::FontFileFormat>(response->format()),
            .file = response->take_file(),
        };
    };
    callbacks.resolve_generic_family = [this](String const& family, u16 weight, u8 slope) -> Optional<FlyString> {
        if (!m_font_client)
            return {};
        auto response = m_font_client->send_sync_but_allow_failure<Messages::CompositorFontServer::ResolveGenericFont>(family, weight, slope);
        if (!response)
            return {};
        auto resolved_family = response->take_resolved_family();
        if (!resolved_family.has_value())
            return {};
        return FlyString { resolved_family.release_value() };
    };

    auto provider = Gfx::SharedFontProvider::create_from_catalog_file_or_empty(move(file), size, generation, move(callbacks));
    if (provider.is_error()) {
        dbgln("Compositor: Unable to install fallback font catalog: {}", provider.error());
        return;
    }
    m_font_provider = provider.value().ptr();
    Gfx::FontDatabase::the().install_system_font_provider(provider.release_value());
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
    if (!connection) {
        dbgln("Compositor: Ignoring context {} for WebContent connection {}, which is already gone", context_id, web_content_connection_id);
        return;
    }

    m_compositor_state->create_context(context_id, page_id, *connection);
}

void ConnectionFromClient::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    m_compositor_state->viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

void ConnectionFromClient::set_paused_debugger_overlay(Web::Compositor::CompositorContextId context_id, bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<u8> hovered_action_value)
{
    if (!isfinite(device_pixel_ratio) || device_pixel_ratio <= 0) {
        did_misbehave("Invalid device pixel ratio");
        return;
    }

    Optional<WebView::PausedDebuggerOverlayAction> hovered_action;
    if (hovered_action_value.has_value()) {
        hovered_action = WebView::paused_debugger_overlay_action_from_underlying(*hovered_action_value);
        if (!hovered_action.has_value()) {
            did_misbehave("Invalid paused debugger overlay action");
            return;
        }
    }
    m_compositor_state->set_paused_debugger_overlay(context_id, visible, device_pixel_ratio, move(font_family), hovered_action);
}

void ConnectionFromClient::set_display_metadata(Web::Compositor::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    m_compositor_state->set_display_metadata(context_id, display_id, refresh_rate);
}

void ConnectionFromClient::set_context_visibility(Web::Compositor::CompositorContextId context_id, Web::Compositor::ContextVisibility visibility)
{
    m_compositor_state->set_context_visibility(context_id, visibility);
}

Messages::CompositorControlServer::HandleMouseEventResponse ConnectionFromClient::handle_mouse_event(Web::Compositor::CompositorContextId context_id, Web::MouseEvent event)
{
    return m_compositor_state->handle_mouse_event(context_id, event);
}

Messages::CompositorControlServer::DispatchMouseEventToWebContentResponse ConnectionFromClient::dispatch_mouse_event_to_web_content(Web::Compositor::CompositorContextId context_id, Web::MouseEvent event)
{
    return m_compositor_state->dispatch_mouse_event_to_web_content(context_id, event);
}

Messages::CompositorControlServer::HandlePinchEventResponse ConnectionFromClient::handle_pinch_event(Web::Compositor::CompositorContextId context_id, Web::PinchEvent event)
{
    return m_compositor_state->handle_pinch_event(context_id, event);
}

Messages::CompositorControlServer::AsyncScrollByResponse ConnectionFromClient::async_scroll_by(Web::Compositor::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Web::Compositor::SnapContainerHandling snap_container_handling)
{
    return m_compositor_state->async_scroll_by(context_id, position, delta_in_device_pixels, snap_container_handling);
}

void ConnectionFromClient::presented_bitmap_ready_to_paint(Web::Compositor::CompositorContextId context_id, i32 bitmap_id)
{
    m_compositor_state->presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void ConnectionFromClient::set_client_gpu_presentation_capability(bool supported, u64 adapter_luid)
{
    m_compositor_state->set_client_gpu_presentation_capability(supported, adapter_luid);
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
