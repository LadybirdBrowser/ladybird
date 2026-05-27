/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Compositor/CompositorState.h>
#include <Compositor/CompositorWebContentClientEndpoint.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Compositor {

class ConnectionFromWebContent final
    : public IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>
    , public CompositorStateWebContentClient {
    C_OBJECT(ConnectionFromWebContent);

public:
    virtual ~ConnectionFromWebContent() override = default;
    void notify_compositor_lost();
    void set_on_death(Function<void(ConnectionFromWebContent&)> handler) { m_on_death = move(handler); }

private:
    explicit ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport>, NonnullRefPtr<CompositorState>, int client_id);

    virtual void die() override;

    virtual void set_presentation_mode(Web::Compositor::CompositorContextId, Web::Compositor::PresentationMode) override;
    virtual void destroy_context(Web::Compositor::CompositorContextId) override;
    virtual void update_display_list(Web::Compositor::CompositorContextId, NonnullRefPtr<Web::Painting::DisplayList>, Web::Painting::AccumulatedVisualContextTree, Web::Painting::DisplayListResourceTransaction, Web::Painting::ScrollStateSnapshot) override;
    virtual void update_scroll_state(Web::Compositor::CompositorContextId, Web::Painting::ScrollStateSnapshot) override;
    virtual void update_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>) override;
    virtual void clear_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId) override;
    virtual void update_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId, Gfx::SharedImage) override;
    virtual void clear_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId) override;
    virtual void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId, u64 generation) override;
    virtual Messages::CompositorWebContentServer::AsyncScrollByResponse async_scroll_by(Web::Compositor::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking) override;
    virtual Messages::CompositorWebContentServer::ShouldDeferMainThreadPresentForAsyncScrollResponse should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId) override;
    virtual Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdatesResponse take_pending_async_scroll_updates(Web::Compositor::CompositorContextId) override;
    virtual void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress) override;
    virtual void present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect viewport_rect) override;
    virtual void request_screenshot(Web::Compositor::CompositorContextId, Web::Compositor::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap) override;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const&) override;
    virtual void request_rendering_update() override;
    void verify_context_is_owned_by_this_connection(Web::Compositor::CompositorContextId);

    NonnullRefPtr<CompositorState> m_compositor_state;
    Function<void(ConnectionFromWebContent&)> m_on_death;
};

}
