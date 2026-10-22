/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CompositorPresentationClient.h>

#include <LibCore/EventLoop.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

CompositorPresentationClient::CompositorPresentationClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorPresentationClientEndpoint, CompositorPresentationServerEndpoint>(*this, move(transport))
{
}

void CompositorPresentationClient::die()
{
    if (auto callback = move(on_death)) {
        Core::deferred_invoke([callback = move(callback)]() mutable {
            callback();
        });
    }
}

void CompositorPresentationClient::did_allocate_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    auto web_content_client = WebContentClient::client_for_compositor_context_id(context_id);
    if (web_content_client.has_value()) {
        auto page_id = web_content_client->page_id_for_compositor_context_id(context_id);
        VERIFY(page_id.has_value());

        if (auto* page = web_content_client->page(*page_id)) {
            page->did_present_backing_stores(move(bitmap_ids), move(backing_stores));
            return;
        }
    }

    // The compositor reserves the first published buffer for the UI to install as its front buffer.
    if (!bitmap_ids.is_empty())
        async_presented_bitmap_ready_to_paint(context_id, bitmap_ids[0]);
}

void CompositorPresentationClient::did_present_frame(Web::CompositorContextId context_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio)
{
    auto web_content_client = WebContentClient::client_for_compositor_context_id(context_id);
    if (web_content_client.has_value()) {
        auto page_id = web_content_client->page_id_for_compositor_context_id(context_id);
        VERIFY(page_id.has_value());
        if (auto* page = web_content_client->page(*page_id)) {
            page->did_present_bitmap(content_rect, damage_rect, bitmap_id, device_pixel_ratio);
            return;
        }
    }

    async_presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

static WebContentPage* page_for_compositor_context_id(Web::CompositorContextId context_id)
{
    auto web_content_client = WebContentClient::client_for_compositor_context_id(context_id);
    if (!web_content_client.has_value())
        return nullptr;
    auto page_id = web_content_client->page_id_for_compositor_context_id(context_id);
    if (!page_id.has_value())
        return nullptr;
    return web_content_client->page(*page_id);
}

void CompositorPresentationClient::did_add_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    if (auto* page = page_for_compositor_context_id(context_id))
        page->did_add_backing_stores(move(bitmap_ids), move(backing_stores));
}

void CompositorPresentationClient::did_retire_backing_stores(Web::CompositorContextId context_id, Vector<i32> bitmap_ids)
{
    if (auto* page = page_for_compositor_context_id(context_id))
        page->did_retire_backing_stores(bitmap_ids);
}

void CompositorPresentationClient::did_consume_input_event(Web::CompositorContextId context_id, u64 event_id)
{
    if (auto* page = page_for_compositor_context_id(context_id))
        page->did_consume_input_event_in_compositor(event_id);
}

void CompositorPresentationClient::did_not_dispatch_input_event(Web::CompositorContextId context_id, u64 event_id)
{
    if (auto* page = page_for_compositor_context_id(context_id))
        page->did_not_dispatch_input_event_through_compositor(event_id);
}

void CompositorPresentationClient::did_dispatch_input_event(Web::CompositorContextId context_id, u64 event_id, bool async_scroll_performed_default_action)
{
    if (auto* page = page_for_compositor_context_id(context_id))
        page->did_dispatch_input_event_through_compositor(event_id, async_scroll_performed_default_action);
}

}
