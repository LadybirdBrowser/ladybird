/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibWebView/CompositorHostBase.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/WebContentCompositorHost.h>

namespace WebContent {

class WebContentCompositorHost final : public WebView::CompositorHostBase {
public:
    explicit WebContentCompositorHost(ConnectionFromClient& client)
        : m_client(client)
    {
    }

private:
    virtual WebView::CompositorConnection* compositor_connection() const override
    {
        return m_client.compositor_process_connection();
    }

    virtual void context_was_destroyed(Web::Compositor::CompositorContextId context_id) override
    {
        m_client.did_destroy_compositor_context(context_id);
    }

    ConnectionFromClient& m_client;
};

NonnullOwnPtr<Web::Compositor::CompositorHost> create_web_content_compositor_host(ConnectionFromClient& client)
{
    return make<WebContentCompositorHost>(client);
}

}
