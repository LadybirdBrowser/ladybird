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
    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle_raw(IPC::RawMessage&) override;
    virtual void create_context(Web::CompositorContextId, Optional<u64> page_id, i32 web_content_connection_id) override;
    virtual void set_context_visibility(Web::CompositorContextId, Compositing::ContextVisibility) override;
    virtual void crash() override;

    ConnectionFromWebContent* web_content_connection(i32 web_content_connection_id);

    HashMap<i32, NonnullRefPtr<ConnectionFromWebContent>> m_web_content_connections;
    NonnullRefPtr<CompositorState> m_compositor_state;
    HashMap<u64, NonnullRefPtr<ConnectionFromPresentation>> m_presentation_connections;
};

}
