/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CompositorHostBase.h>
#include <WebWorker/ConnectionFromClient.h>
#include <WebWorker/WebWorkerCompositorHost.h>

namespace WebWorker {

class WebWorkerCompositorHost final : public WebView::CompositorHostBase {
public:
    explicit WebWorkerCompositorHost(ConnectionFromClient& client)
        : m_client(client)
    {
    }

private:
    virtual WebView::CompositorConnection* compositor_connection() const override
    {
        return m_client.compositor_process_connection();
    }

    ConnectionFromClient& m_client;
};

NonnullOwnPtr<Web::Compositor::CompositorHost> create_web_worker_compositor_host(ConnectionFromClient& client)
{
    return make<WebWorkerCompositorHost>(client);
}

}
