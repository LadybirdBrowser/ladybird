/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DoublyLinkedList.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <Compositor/ContextState.h>
#include <Compositor/VSyncScheduler.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web {

struct MouseEvent;

}

namespace Compositor {

class CompositorStateClient {
public:
    virtual ~CompositorStateClient() = default;

    virtual void did_allocate_backing_stores(Web::Compositor::CompositorContextId, i32 front_bitmap_id, Gfx::SharedImage&& front_backing_store, i32 back_bitmap_id, Gfx::SharedImage&& back_backing_store) = 0;
    virtual void did_present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect content_rect, i32 bitmap_id) = 0;
};

class CompositorStateWebContentClient {
public:
    virtual ~CompositorStateWebContentClient() = default;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const&) = 0;
    virtual void request_rendering_update() = 0;
};

class CompositorState final : public RefCounted<CompositorState> {
public:
    static NonnullRefPtr<CompositorState> create(RefPtr<Gfx::SkiaBackendContext>, bool async_scrolling_enabled);
    ~CompositorState();

    enum class ContextOwnerCheckResult {
        OwnedByClient,
        ContextUnavailable,
        ConflictingOwner,
    };

    void set_client(CompositorStateClient&);
    ContextOwnerCheckResult check_context_owner(Web::Compositor::CompositorContextId, CompositorStateWebContentClient&);
    void destroy_contexts_for_web_content_client(CompositorStateWebContentClient&);

    void create_context(Web::Compositor::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&);
    void destroy_context(Web::Compositor::CompositorContextId);

    void set_presentation_mode(Web::Compositor::CompositorContextId, Web::Compositor::PresentationMode);
    void stop_presenting_to_client(Web::Compositor::CompositorContextId);
    void update_display_list(Web::Compositor::CompositorContextId, NonnullRefPtr<Web::Painting::DisplayList>, Web::Painting::AccumulatedVisualContextTree, Web::Painting::DisplayListResourceTransaction&&, Web::Painting::ScrollStateSnapshot&&);
    void update_scroll_state(Web::Compositor::CompositorContextId, Web::Painting::ScrollStateSnapshot&&);
    void update_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>);
    void clear_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId);
    void update_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId, Gfx::SharedImage&&);
    void clear_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId);
    void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId, u64 generation);
    bool handle_mouse_event(Web::Compositor::CompositorContextId, Web::MouseEvent const&);
    bool dispatch_mouse_event_to_web_content(Web::Compositor::CompositorContextId, Web::MouseEvent const&);
    Web::Compositor::AsyncScrollEnqueueResult async_scroll_by(Web::Compositor::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking);
    bool async_scroll_by(Web::Compositor::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta);
    bool should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId) const;
    Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::Compositor::CompositorContextId);
    void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize, Web::Compositor::WindowResizingInProgress);
    void set_display_metadata(Web::Compositor::CompositorContextId, Optional<u64> display_id, double refresh_rate);
    void present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect);
    bool request_screenshot(Web::Compositor::CompositorContextId, Gfx::ShareableBitmap&);
    void presented_bitmap_ready_to_paint(Web::Compositor::CompositorContextId, i32 bitmap_id);

private:
    CompositorState(RefPtr<Gfx::SkiaBackendContext>, bool async_scrolling_enabled);

    struct PendingAsyncPresent {
        PendingAsyncPresent(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect, i32 bitmap_id)
            : context_id(context_id)
            , viewport_rect(viewport_rect)
            , bitmap_id(bitmap_id)
        {
        }

        Web::Compositor::CompositorContextId context_id;
        Gfx::IntRect viewport_rect;
        i32 bitmap_id { 0 };
        bool was_cancelled { false };
    };

    ContextState* context_if_present(Web::Compositor::CompositorContextId);
    ContextState const* context_if_present(Web::Compositor::CompositorContextId) const;
    void detach_from_parent_surface(Web::Compositor::CompositorContextId, ContextState&);
    void remove_child_surface(ContextState&, Web::Compositor::CompositorContextId parent_context_id, Web::Painting::CompositorSurfaceId);
    void schedule_backing_store_shrink(Web::Compositor::CompositorContextId, ContextState&);
    void shrink_backing_stores_after_resize(Web::Compositor::CompositorContextId);
    void resize_backing_stores_if_needed(Web::Compositor::CompositorContextId, ContextState&);
    void present_current_frame(Web::Compositor::CompositorContextId, ContextState&);
    void publish_to_parent_surface(ContextState&, Web::Compositor::PublishToCompositorSurface const&);
    bool apply_context_update_result(
        Web::Compositor::CompositorContextId,
        ContextState&,
        ContextState::ContextUpdateResult const&);
    void present_frame(Web::Compositor::CompositorContextId, ContextState&, Gfx::IntRect);
    void schedule_present_frame(Web::Compositor::CompositorContextId, ContextState&, Gfx::IntRect);
    void schedule_pending_present_frame_on_vsync(Web::Compositor::CompositorContextId, ContextState&);
    void schedule_pending_present_frame_if_unblocked(Web::Compositor::CompositorContextId, ContextState&);
    VSyncScheduler& vsync_scheduler_for_display(Optional<u64> display_id);
    void present_pending_frames_on_vsync(Optional<u64> display_id);
    void flush_descendant_surfaces_for_screenshot(Web::Compositor::CompositorContextId);
    bool present_subtree_for_screenshot(Web::Compositor::CompositorContextId);
    void present_context_synchronously(ContextState&);
    void publish_backing_stores(Web::Compositor::CompositorContextId, ContextState&, BackingStoreManager::Publication&&);
    void did_finish_async_present(PendingAsyncPresent&);
    void cancel_pending_async_presents_for_context(Web::Compositor::CompositorContextId);
    void schedule_gpu_completion_check();
    void check_gpu_completions();

    HashMap<Web::Compositor::CompositorContextId, OwnPtr<ContextState>> m_contexts;
    DoublyLinkedList<PendingAsyncPresent> m_pending_async_presents;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    OwnPtr<Web::Painting::DisplayListPlayerSkia> m_display_list_player;
    HashMap<Optional<u64>, OwnPtr<VSyncScheduler>> m_vsync_schedulers_by_display;
    RefPtr<Core::Timer> m_gpu_completion_timer;
    CompositorStateClient* m_client { nullptr };
    bool m_async_scrolling_enabled { true };
};

}
