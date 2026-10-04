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
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>

namespace Compositor {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint>
    , public CompositorStateClient {
    C_OBJECT(ConnectionFromClient);

public:
    virtual ~ConnectionFromClient() override = default;

private:
    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, RefPtr<Gfx::SkiaBackendContext>);

    virtual void die() override;

    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) override;
    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) override;
    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) override;
    virtual void did_present_frame(Web::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id) override;
    virtual void did_consume_input_event(Web::CompositorContextId, u64 event_id) override;
    virtual void did_not_dispatch_input_event(Web::CompositorContextId, u64 event_id) override;

    virtual Messages::CompositorControlServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual void set_font_service(IPC::TransportHandle, IPC::File catalog, u64 catalog_size, u64 generation) override;
    virtual Messages::CompositorControlServer::ConnectWebContentResponse connect_web_content() override;
    virtual void create_context(Web::CompositorContextId, Optional<u64> page_id, i32 web_content_connection_id) override;
    virtual void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress) override;
    virtual void set_paused_debugger_overlay(Web::CompositorContextId, bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<u8> hovered_action) override;
    virtual void set_display_metadata(Web::CompositorContextId, Optional<u64>, double) override;
    virtual void set_context_visibility(Web::CompositorContextId, Compositing::ContextVisibility) override;
    virtual void handle_and_dispatch_mouse_event(Web::CompositorContextId, Web::MouseEvent) override;
    virtual void handle_pinch_event(Web::CompositorContextId, Web::PinchEvent) override;
    virtual Messages::CompositorControlServer::HandleKeyEventResponse handle_key_event(Web::CompositorContextId, Web::KeyEvent) override;
    virtual Messages::CompositorControlServer::DispatchKeyEventToWebContentResponse dispatch_key_event_to_web_content(Web::CompositorContextId, Web::KeyEvent) override;
    virtual void presented_bitmap_ready_to_paint(Web::CompositorContextId, i32 bitmap_id) override;
    virtual void set_client_gpu_presentation_capability(bool supported, u64 adapter_luid) override;
    virtual void set_synthesizes_scroll_momentum(bool) override;
    virtual void crash() override;

    ConnectionFromWebContent* web_content_connection(i32 web_content_connection_id);

    HashMap<i32, NonnullRefPtr<ConnectionFromWebContent>> m_web_content_connections;
    NonnullRefPtr<CompositorState> m_compositor_state;
};

}
