/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/CompositorControlClientEndpoint.h>
#include <Compositor/CompositorControlServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API CompositorClient final
    : public IPC::ConnectionToServer<CompositorControlClientEndpoint, CompositorControlServerEndpoint>
    , public CompositorControlClientEndpoint {
    C_OBJECT_ABSTRACT(CompositorClient)

public:
    using InitTransport = Messages::CompositorControlServer::InitTransport;

    explicit CompositorClient(NonnullOwnPtr<IPC::Transport>);

    Function<void()> on_death;

private:
    virtual void die() override;

    virtual void did_connect_web_content(i32 web_content_connection_id) override;
};

}
