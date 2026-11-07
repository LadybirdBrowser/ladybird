/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/CompositorHost.h>
#include <WebContent/Worker/ConnectionFromClient.h>
#include <WebContent/Worker/WebWorkerCompositorHost.h>

namespace WebWorker {

class WebWorkerCompositorHost final : public Web::Compositor::CompositorHost {
public:
    explicit WebWorkerCompositorHost(ConnectionFromClient& client)
        : m_client(client)
    {
    }

private:
    virtual Web::Compositor::CompositorConnection* compositor_connection() const override
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
