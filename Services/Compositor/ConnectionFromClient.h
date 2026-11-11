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
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>

namespace Compositor {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<CompositorControlClientEndpoint, CompositorControlServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    virtual ~ConnectionFromClient() override = default;

private:
    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, RefPtr<Gfx::SkiaBackendContext>, bool scrollbars_always_shown);

    virtual void die() override;

    virtual void set_font_service(IPC::TransportHandle, IPC::File catalog, u64 catalog_size, u64 generation) override;
    virtual Messages::CompositorControlServer::ConnectWebContentResponse connect_web_content() override;
    virtual void connect_render_clock(i32 web_content_connection_id, IPC::TransportHandle) override;
    virtual void connect_video_presentation(i32 web_content_connection_id, IPC::TransportHandle) override;
    virtual Messages::CompositorControlServer::CreatePresentationConnectionResponse create_presentation_connection(bool synthesizes_scroll_momentum, bool scrolls_elastically, Optional<u64> gpu_presentation_adapter_luid) override;
    virtual void bind_presentation(Web::CompositorContextId, u64 presentation_id) override;
    virtual Messages::CompositorControlServer::CreateRemoteLayerResponse create_remote_layer(u64 presentation_id, u64 remote_layer_id) override;
    virtual void destroy_remote_layer(u64 remote_layer_id) override;
    virtual void show_context_in_remote_layer(u64 remote_layer_id, Optional<Web::CompositorContextId>) override;
    virtual void set_remote_layer_fit(u64 remote_layer_id, Optional<Gfx::IntSize> fit_within) override;
    virtual void begin_synchronized_resize(u64 remote_layer_id, u64 token) override { m_compositor_state->begin_synchronized_resize(remote_layer_id, token); }
    virtual void await_synchronized_resize_frame(u64 remote_layer_id, u64 token, Gfx::IntSize size) override { m_compositor_state->await_synchronized_resize_frame(remote_layer_id, token, size); }
    virtual void commit_synchronized_resize(u64 remote_layer_id, u64 token) override { m_compositor_state->commit_synchronized_resize(remote_layer_id, token); }
    virtual void create_context(Web::CompositorContextId, Optional<u64> page_id, i32 web_content_connection_id) override;
    virtual void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress) override;
    virtual void set_ui_overlays(Web::CompositorContextId, Compositing::UIOverlays) override;
    virtual void set_display_metadata(Web::CompositorContextId, Optional<u64>, double) override;
    virtual void set_context_visibility(Web::CompositorContextId, Compositing::ContextVisibility) override;
    virtual void set_context_presents_from_cpu_memory(Web::CompositorContextId, bool) override;
    virtual void crash() override;

    ConnectionFromWebContent* web_content_connection(i32 web_content_connection_id);

    HashMap<i32, NonnullRefPtr<ConnectionFromWebContent>> m_web_content_connections;
    NonnullRefPtr<CompositorState> m_compositor_state;
    HashMap<u64, NonnullRefPtr<ConnectionFromPresentation>> m_presentation_connections;
    u64 m_next_presentation_id { 1 };
};

}
