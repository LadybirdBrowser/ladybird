/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/CompositorPresentationClientEndpoint.h>
#include <Compositor/CompositorPresentationServerEndpoint.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImage.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The Compositor connection that receives the frames of the UI's pages and sends their input.
class WEBVIEW_API CompositorPresentationClient final
    : public IPC::ConnectionToServer<CompositorPresentationClientEndpoint, CompositorPresentationServerEndpoint>
    , public CompositorPresentationClientEndpoint {
    C_OBJECT_ABSTRACT(CompositorPresentationClient)

public:
    explicit CompositorPresentationClient(NonnullOwnPtr<IPC::Transport>);

    Function<void()> on_death;

private:
    virtual void die() override;

    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) override;
    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores) override;
    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) override;
    virtual void did_present_frame(Web::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio) override;
    virtual void did_consume_input_event(Web::CompositorContextId, u64 event_id) override;
    virtual void did_not_dispatch_input_event(Web::CompositorContextId, u64 event_id) override;
    virtual void did_dispatch_input_event(Web::CompositorContextId, u64 event_id, bool async_scroll_performed_default_action) override;
};

}
