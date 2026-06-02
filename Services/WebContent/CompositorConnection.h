/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <Compositor/CompositorWebContentClientEndpoint.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ScrollState.h>

namespace WebContent {

class CompositorConnection final
    : public IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>
    , public CompositorWebContentClientEndpoint {
    C_OBJECT_ABSTRACT(CompositorConnection)

public:
    explicit CompositorConnection(NonnullOwnPtr<IPC::Transport>);

    void set_presentation_mode(Web::Compositor::CompositorContextId, Web::Compositor::PresentationMode const&);
    void destroy_context(Web::Compositor::CompositorContextId);
    void update_display_list(Web::Compositor::CompositorContextId, NonnullRefPtr<Web::Painting::DisplayList> const&, Web::Painting::AccumulatedVisualContextTree const&, Web::Painting::DisplayListResourceTransaction const&, Web::Painting::ScrollStateSnapshot const&);
    void update_visual_context_tree(Web::Compositor::CompositorContextId, Web::Painting::AccumulatedVisualContextTree const&);
    void update_scroll_state(Web::Compositor::CompositorContextId, Web::Painting::ScrollStateSnapshot const&);
    void update_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const> const&);
    void clear_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId);
    void update_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId, Gfx::SharedImage const&);
    void clear_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId);
    void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId, u64 generation);
    Web::Compositor::AsyncScrollEnqueueResult async_scroll_by(Web::Compositor::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking);
    bool should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId);
    Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::Compositor::CompositorContextId);
    void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize, Web::Compositor::WindowResizingInProgress);
    void present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect);
    void request_screenshot(Web::Compositor::CompositorContextId, NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&&);
    Function<void(u64 page_id, Web::MouseEvent)> on_mouse_event;

private:
    struct PendingScreenshot {
        NonnullRefPtr<Gfx::PaintingSurface> target_surface;
        NonnullRefPtr<Gfx::Bitmap> target_bitmap;

        Function<void()> callback;
    };

    virtual void die() override;

    virtual void mouse_event(u64 page_id, Web::MouseEvent) override;
    virtual void request_rendering_update() override;
    virtual void did_complete_screenshot(Web::Compositor::ScreenshotRequestId) override;
    virtual void did_fail_screenshot(Web::Compositor::ScreenshotRequestId) override;
    virtual void did_lose_compositor() override;

    bool can_send_message_to_compositor() const;
    Optional<PendingScreenshot> take_screenshot(Web::Compositor::ScreenshotRequestId);

    HashMap<Web::Compositor::ScreenshotRequestId, PendingScreenshot> m_screenshots;
    u64 m_next_screenshot_request_id { 1 };
    bool m_has_lost_compositor { false };
};

}
