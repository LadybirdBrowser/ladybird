/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CompositorClient.h>

#include <LibCore/EventLoop.h>
#include <LibWebView/WebContentClient.h>

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

void CompositorClient::did_allocate_backing_stores(Web::Compositor::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    auto web_content_client = WebContentClient::client_for_compositor_context_id(context_id);
    if (!web_content_client.has_value())
        return;

    auto page_id = web_content_client->page_id_for_compositor_context_id(context_id);
    VERIFY(page_id.has_value());

    web_content_client->did_present_backing_stores(*page_id, move(bitmap_ids), move(backing_stores));
}

void CompositorClient::did_present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id)
{
    auto web_content_client = WebContentClient::client_for_compositor_context_id(context_id);
    if (web_content_client.has_value()) {
        auto page_id = web_content_client->page_id_for_compositor_context_id(context_id);
        VERIFY(page_id.has_value());
        web_content_client->did_present_bitmap(*page_id, content_rect, damage_rect, bitmap_id);
        return;
    }

    async_presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

}
