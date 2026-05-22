/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/BackingStoreManager.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
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

    enum class ContextOwnerCheckResult {
        OwnedByClient,
        ContextUnavailable,
        ConflictingOwner,
    };

    void set_client(CompositorStateClient&);
    ContextOwnerCheckResult check_context_owner(Web::Compositor::CompositorContextId, CompositorStateWebContentClient&);
    void destroy_contexts_for_web_content_client(CompositorStateWebContentClient&);

    void create_context(Web::Compositor::CompositorContextId, Optional<u64> page_id, Web::Compositor::PagePresentationRegistration, CompositorStateWebContentClient&);
    void destroy_context(Web::Compositor::CompositorContextId);

    void set_presentation_mode(Web::Compositor::CompositorContextId, Web::Compositor::PresentationMode);
    void stop_presenting_to_client(Web::Compositor::CompositorContextId);
    void update_display_list(Web::Compositor::CompositorContextId, NonnullRefPtr<Web::Painting::DisplayList>, Web::Painting::DisplayListResourceTransaction&&, Web::Painting::ScrollStateSnapshot&&);
    void update_scroll_state(Web::Compositor::CompositorContextId, Web::Painting::ScrollStateSnapshot&&);
    void update_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>);
    void clear_video_frame(Web::Compositor::CompositorContextId, Web::Painting::VideoFrameResourceId);
    void update_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId, Gfx::SharedImage&&);
    void clear_compositor_surface(Web::Compositor::CompositorContextId, Web::Painting::CompositorSurfaceId);
    void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId, u64 generation);
    bool handle_mouse_event(Web::Compositor::CompositorContextId, Web::MouseEvent const&);
    void dispatch_mouse_event_to_web_content(Web::Compositor::CompositorContextId, Web::MouseEvent const&);
    Web::Compositor::AsyncScrollEnqueueResult async_scroll_by(Web::Compositor::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking);
    bool async_scroll_by(Web::Compositor::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta);
    void present_deferred_async_scroll_frame(Web::Compositor::CompositorContextId);
    bool should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId) const;
    Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::Compositor::CompositorContextId);
    void viewport_size_updated(Web::Compositor::CompositorContextId, Gfx::IntSize, bool is_top_level_traversable, Web::Compositor::WindowResizingInProgress);
    void present_frame(Web::Compositor::CompositorContextId, Gfx::IntRect);
    bool request_screenshot(Web::Compositor::CompositorContextId, Gfx::ShareableBitmap&);
    void presented_bitmap_ready_to_paint(Web::Compositor::CompositorContextId, i32 bitmap_id);

private:
    CompositorState(RefPtr<Gfx::SkiaBackendContext>, bool async_scrolling_enabled);

    struct ViewportScrollbarDrag {
        size_t scrollbar_index { 0 };
        float primary_position { 0 };
        float thumb_grab_position { 0 };
    };

    struct ContextState {
        struct PublishedSurface {
            Web::Compositor::CompositorContextId parent_context_id;
            Web::Painting::CompositorSurfaceId surface_id;
        };

        bool is_registered { false };
        CompositorStateWebContentClient* web_content_client { nullptr };
        Optional<u64> page_id;
        Web::Compositor::PagePresentationRegistration page_presentation_registration { Web::Compositor::PagePresentationRegistration::No };

        bool presents_to_client { false };
        Web::Compositor::PresentationMode presentation_mode { Empty {} };
        Optional<PublishedSurface> published_surface;
        HashMap<Web::Painting::CompositorSurfaceId, Web::Compositor::CompositorContextId> child_contexts_by_surface_id;

        RefPtr<Web::Painting::DisplayList> display_list;
        Web::Painting::DisplayListResourceStorage display_list_resource_storage;
        Web::Painting::ScrollStateSnapshot scroll_state_snapshot;
        Web::Compositor::BackingStoreManager backing_store_manager;

        Web::Compositor::AsyncScrollTree async_scroll_tree;
        Vector<Web::Compositor::ViewportScrollbar> viewport_scrollbars;
        Optional<size_t> hovered_viewport_scrollbar_index;
        Optional<size_t> captured_viewport_scrollbar_index;
        float viewport_scrollbar_thumb_grab_position { 0 };

        Vector<Web::Compositor::AsyncScrollOffset> pending_async_scroll_offsets;
        Vector<Web::Compositor::AsyncScrollOperationID> completed_async_scroll_operation_ids;
        Web::Compositor::AsyncScrollOperationID next_async_scroll_operation_id { 0 };
        Gfx::IntRect async_scrolling_viewport_rect;
        bool has_async_scrolling_state { false };
        bool can_accept_async_wheel_events { false };
        u64 wheel_event_listener_state_generation { 0 };
        Web::Compositor::WheelRoutingAdmission wheel_routing_admission { Web::Compositor::WheelRoutingAdmission::NoAsyncScrollingState };

        Gfx::IntSize viewport_size;
        bool is_top_level_traversable { false };
        Web::Compositor::WindowResizingInProgress window_resize_in_progress { Web::Compositor::WindowResizingInProgress::No };

        Optional<Gfx::IntRect> pending_present_frame;
        Optional<Gfx::IntRect> presented_frame;
        Optional<i32> presented_bitmap_id_awaiting_ack;
        bool has_deferred_async_scroll_present { false };
        Gfx::IntRect deferred_async_scroll_present_viewport_rect;
    };

    ContextState* context_if_present(Web::Compositor::CompositorContextId);
    ContextState const* context_if_present(Web::Compositor::CompositorContextId) const;
    void detach_from_parent_surface(Web::Compositor::CompositorContextId, ContextState&);
    void remove_child_surface(ContextState&, Web::Compositor::CompositorContextId parent_context_id, Web::Painting::CompositorSurfaceId);
    void install_display_list_update(ContextState&, NonnullRefPtr<Web::Painting::DisplayList>, Web::Painting::ScrollStateSnapshot&&);
    Optional<Gfx::FloatPoint> viewport_scroll_offset_from(ContextState&, Vector<Web::Compositor::AsyncScrollOffset> const&) const;
    Optional<Gfx::FloatPoint> reapply_pending_async_scroll_offsets(ContextState&, Vector<Web::Compositor::AsyncScrollOffset> const&);
    void store_pending_async_scroll_offsets(ContextState&, Vector<Web::Compositor::AsyncScrollOffset> const&, Optional<Web::Compositor::AsyncScrollOperationID> = {});
    Optional<size_t> hit_test_viewport_scrollbar(ContextState&, Gfx::FloatPoint position) const;
    Optional<ViewportScrollbarDrag> begin_viewport_scrollbar_drag(ContextState&, Gfx::FloatPoint position);
    Optional<ViewportScrollbarDrag> captured_viewport_scrollbar_drag(ContextState&, Gfx::FloatPoint position);
    Optional<ViewportScrollbarDrag> release_captured_viewport_scrollbar_drag(ContextState&, Gfx::FloatPoint position);
    void set_hovered_viewport_scrollbar(Web::Compositor::CompositorContextId, ContextState&, Optional<size_t>);
    bool apply_viewport_scrollbar_drag(Web::Compositor::CompositorContextId, ContextState&, size_t scrollbar_index, float primary_position, float thumb_grab_position);
    void present_viewport_scrollbar_overlay(Web::Compositor::CompositorContextId, ContextState&);
    bool paint_viewport_scrollbar_overlay(ContextState&, Gfx::PaintingSurface&);
    void resize_backing_stores_if_needed(Web::Compositor::CompositorContextId, ContextState&);
    void present_current_frame(Web::Compositor::CompositorContextId, ContextState&);
    void publish_to_parent_surface(ContextState&, Web::Compositor::PublishToCompositorSurface const&);
    void present_frame(Web::Compositor::CompositorContextId, ContextState&, Gfx::IntRect);
    void publish_backing_stores(Web::Compositor::CompositorContextId, ContextState&, Web::Compositor::BackingStoreManager::Publication&&);
    bool present_frame_to_client(Web::Compositor::CompositorContextId, ContextState&, Gfx::IntRect const&, i32 bitmap_id);

    HashMap<Web::Compositor::CompositorContextId, OwnPtr<ContextState>> m_contexts;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    OwnPtr<Web::Painting::DisplayListPlayerSkia> m_display_list_player;
    CompositorStateClient* m_client { nullptr };
    bool m_async_scrolling_enabled { true };
};

}
