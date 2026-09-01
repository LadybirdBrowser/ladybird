/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/CompositorControlClientEndpoint.h>
#include <Compositor/CompositorControlServerEndpoint.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImage.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWeb/Compositor/Types.h>
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

    virtual void did_allocate_backing_stores(Web::Compositor::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) override;
    virtual void did_present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id) override;
};

}
