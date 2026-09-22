/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <Compositor/CompositorControlClientEndpoint.h>
#include <Compositor/CompositorControlServerEndpoint.h>
#include <Compositor/CompositorState.h>
#include <Compositor/FontClient.h>
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>

namespace Gfx {

class SharedFontProvider;

}

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

    virtual void did_allocate_backing_stores(Compositing::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) override;
    virtual void did_present_frame(Compositing::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id) override;

    virtual Messages::CompositorControlServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual void set_font_service_transport(IPC::TransportHandle) override;
    virtual void set_font_catalog(IPC::File, u64 size, u64 generation) override;
    virtual Messages::CompositorControlServer::ConnectWebContentResponse connect_web_content() override;
    virtual void create_context(Compositing::CompositorContextId, Optional<u64> page_id, i32 web_content_connection_id) override;
    virtual void viewport_size_updated(Compositing::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress) override;
    virtual void set_paused_debugger_overlay(Compositing::CompositorContextId, bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<u8> hovered_action) override;
    virtual void set_display_metadata(Compositing::CompositorContextId, Optional<u64>, double) override;
    virtual void set_context_visibility(Compositing::CompositorContextId, Compositing::ContextVisibility) override;
    virtual Messages::CompositorControlServer::HandleMouseEventResponse handle_mouse_event(Compositing::CompositorContextId, Compositing::MouseEvent) override;
    virtual Messages::CompositorControlServer::DispatchMouseEventToWebContentResponse dispatch_mouse_event_to_web_content(Compositing::CompositorContextId, Compositing::MouseEvent) override;
    virtual Messages::CompositorControlServer::HandlePinchEventResponse handle_pinch_event(Compositing::CompositorContextId, Compositing::PinchEvent) override;
    virtual Messages::CompositorControlServer::HandleKeyEventResponse handle_key_event(Compositing::CompositorContextId, Compositing::KeyEvent) override;
    virtual Messages::CompositorControlServer::DispatchKeyEventToWebContentResponse dispatch_key_event_to_web_content(Compositing::CompositorContextId, Compositing::KeyEvent) override;
    virtual Messages::CompositorControlServer::AsyncScrollByResponse async_scroll_by(Compositing::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers) override;
    virtual void presented_bitmap_ready_to_paint(Compositing::CompositorContextId, i32 bitmap_id) override;
    virtual void set_client_gpu_presentation_capability(bool supported, u64 adapter_luid) override;
    virtual void crash() override;

    ConnectionFromWebContent* web_content_connection(i32 web_content_connection_id);

    HashMap<i32, NonnullRefPtr<ConnectionFromWebContent>> m_web_content_connections;
    NonnullRefPtr<CompositorState> m_compositor_state;
    Gfx::SharedFontProvider* m_font_provider { nullptr };
    RefPtr<FontClient> m_font_client;
};

}
