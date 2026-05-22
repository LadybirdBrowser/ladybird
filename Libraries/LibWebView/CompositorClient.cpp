/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CompositorClient.h>

#include <LibCore/EventLoop.h>

namespace WebView {

CompositorClient::CompositorClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorControlClientEndpoint, CompositorControlServerEndpoint>(*this, move(transport))
{
}

void CompositorClient::die()
{
    if (auto callback = move(on_death)) {
        Core::deferred_invoke([callback = move(callback)]() mutable {
            callback();
        });
    }
}

void CompositorClient::did_connect_web_content(i32)
{
}

}
