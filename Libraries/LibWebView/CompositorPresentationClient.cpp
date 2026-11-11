/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CompositorPresentationClient.h>

#include <LibCore/EventLoop.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

CompositorPresentationClient::CompositorPresentationClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorPresentationClientEndpoint, CompositorPresentationServerEndpoint>(*this, move(transport))
{
}

void CompositorPresentationClient::die()
{
    fail_pending_frame_snapshots();

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

void CompositorPresentationClient::did_present_frame_in_remote_layer(Web::CompositorContextId context_id, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, double device_pixel_ratio)
{
    if (auto* page = page_for_compositor_context_id(context_id))
        page->did_present_frame_in_remote_layer(content_rect, damage_rect, device_pixel_ratio);
}

void CompositorPresentationClient::take_frame_snapshot(Web::CompositorContextId context_id, Optional<Gfx::IntSize> maximum_size, FrameSnapshotCallback callback)
{
    auto request_id = m_next_frame_snapshot_request_id++;
    m_pending_frame_snapshots.set(request_id, move(callback));
    async_take_frame_snapshot(context_id, request_id, maximum_size);
}

void CompositorPresentationClient::fail_pending_frame_snapshots()
{
    auto pending_frame_snapshots = move(m_pending_frame_snapshots);
    for (auto& [_, callback] : pending_frame_snapshots)
        callback({});
}

void CompositorPresentationClient::did_take_frame_snapshot(u64 request_id, Optional<Gfx::ShareableBitmap> snapshot)
{
    if (auto callback = m_pending_frame_snapshots.take(request_id); callback.has_value())
        (*callback)(move(snapshot));
}

void CompositorPresentationClient::did_hold_synchronized_resize_frame(u64 remote_layer_id, u64 token)
{
    if (auto* view = ViewImplementation::find_view_showing_remote_layer(remote_layer_id))
        view->synchronized_resize_frame_held({}, token);
}

void CompositorPresentationClient::did_commit_synchronized_resize(u64 remote_layer_id, u64 token)
{
    if (auto* view = ViewImplementation::find_view_showing_remote_layer(remote_layer_id))
        view->synchronized_resize_committed({}, token);
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

void CompositorPresentationClient::did_dispatch_input_event(Web::CompositorContextId context_id, u64 event_id, bool performed_default_action)
{
    if (auto* page = page_for_compositor_context_id(context_id))
        page->did_dispatch_input_event_through_compositor(event_id, performed_default_action);
}

}
