/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <Compositor/BackingStoreManager.h>
#include <Compositor/ViewportScrollbarController.h>
#include <LibCore/Forward.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Gfx {

class SkiaBackendContext;

}

namespace Web {

struct MouseEvent;
struct PinchEvent;

}

namespace Web::Painting {

class DisplayListPlayerSkia;

}

namespace Compositor {

class CompositorStateWebContentClient;
using CompositedContextResolver = Function<RefPtr<Gfx::PaintingSurface>(Web::Compositor::CompositorContextId)>;

class ContextState {
    AK_MAKE_NONCOPYABLE(ContextState);
    AK_MAKE_NONMOVABLE(ContextState);

public:
    struct AsyncScrollResult {
        Web::Compositor::AsyncScrollEnqueueResult enqueue_result;
        Optional<Gfx::IntRect> frame_to_present;
    };

    struct ContextUpdateResult {
        bool accepted { false };
        Optional<Gfx::IntRect> frame_to_present;
        bool should_request_rendering_update { false };
    };

    struct PreparedFrame {
        Gfx::PaintingSurface* rendered_surface { nullptr };
        i32 bitmap_id { 0 };
    };

    ContextState(Optional<u64> page_id, CompositorStateWebContentClient&, Web::Painting::CanvasSurfaceRegistry const&, bool async_scrolling_enabled);
    ~ContextState();

    bool is_owned_by(CompositorStateWebContentClient const&) const;
    void request_rendering_update();
    void dispatch_mouse_event_to_web_content(Web::MouseEvent const&);

    bool presents_to_client() const { return m_presents_to_client; }
    void stop_presenting_to_client();
    void did_stop_presenting_to_client_if_needed(bool was_presenting_to_client, bool will_present_to_client);

    void set_parent_context(Optional<Web::Compositor::CompositorContextId>);
    Optional<Web::Compositor::CompositorContextId> parent_context_id() const { return m_parent_context_id; }
    RefPtr<Gfx::PaintingSurface> latest_rendered_surface() const { return m_latest_rendered_surface; }

    void apply_display_list_resource_transaction(Web::Painting::DisplayListResourceTransaction&&);
    void install_display_list_update(
        NonnullRefPtr<Web::Painting::DisplayList>,
        Web::Painting::AccumulatedVisualContextTree,
        Web::Painting::ScrollStateSnapshot&&);
    void update_visual_context_tree(Web::Painting::AccumulatedVisualContextTree);
    void update_scroll_state(Web::Painting::ScrollStateSnapshot&&);
    void update_video_frame(Web::Painting::VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>);
    void clear_video_frame(Web::Painting::VideoFrameResourceId);

    void invalidate_wheel_event_listener_state(u64 generation);
    ContextUpdateResult handle_mouse_event(Web::MouseEvent const&);
    ContextUpdateResult handle_pinch_event(Web::PinchEvent const&);
    AsyncScrollResult async_scroll_by(
        Web::UniqueNodeID document_id,
        Gfx::FloatPoint position,
        Gfx::FloatPoint delta,
        Gfx::IntRect viewport_rect,
        Web::Compositor::AsyncScrollOperationTracking);
    ContextUpdateResult async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta);
    bool should_defer_main_thread_present_for_async_scroll() const;
    Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates();

    void viewport_size_updated(Gfx::IntSize, Web::Compositor::WindowResizingInProgress);
    bool should_shrink_backing_stores_after_resize() const;
    void schedule_backing_store_shrink(Function<void()>);
    void finish_window_resize();
    Optional<BackingStoreManager::Publication> resize_backing_stores_if_needed(RefPtr<Gfx::SkiaBackendContext> const&);

    bool set_display_metadata(Optional<u64> display_id, double refresh_rate);
    Optional<u64> display_id() const { return m_display_id; }
    double display_refresh_rate() const { return m_display_refresh_rate; }

    void queue_present_frame(Gfx::IntRect);
    void mark_pending_present_frame_scheduled();
    bool has_pending_present_frame_scheduled_on(Optional<u64> display_id) const;
    bool can_schedule_pending_present_frame_if_unblocked() const;
    Optional<Gfx::IntRect> take_pending_present_frame_if_unblocked();
    bool needs_synchronous_present_for_screenshot() const;
    Optional<Gfx::IntRect> current_frame_rect_to_present() const;
    Optional<PreparedFrame> prepare_frame(Web::Painting::DisplayListPlayerSkia&, Gfx::IntRect, CompositedContextResolver const*);
    void did_submit_prepared_frame(Gfx::IntRect);
    bool present_synchronously(Web::Painting::DisplayListPlayerSkia&, CompositedContextResolver const*);
    bool can_paint_screenshot(Gfx::ShareableBitmap&) const;
    void paint_screenshot(Web::Painting::DisplayListPlayerSkia&, Gfx::ShareableBitmap&, CompositedContextResolver const*);
    bool acknowledge_presented_bitmap(i32 bitmap_id);
    void did_finish_gpu_present(i32 bitmap_id);

private:
    struct VisualViewportScrollDelta {
        Web::Compositor::AsyncScrollOffset scroll_offset;
        Gfx::FloatPoint consumed_delta;
    };

    void stop_backing_store_shrink_timer();
    Web::Painting::AccumulatedVisualContextTree const& current_visual_context_tree() const;
    Optional<Gfx::FloatPoint> viewport_scroll_offset_from(Vector<Web::Compositor::AsyncScrollOffset> const&) const;
    Optional<float> visual_viewport_scale_for_compositing() const;
    Optional<VisualViewportScrollDelta> apply_visual_viewport_scroll_delta(Gfx::FloatPoint);
    Optional<Gfx::FloatPoint> reapply_pending_async_scroll_offsets(Vector<Web::Compositor::AsyncScrollOffset> const&);
    void store_pending_async_scroll_offsets(Vector<Web::Compositor::AsyncScrollOffset> const&, Optional<Web::Compositor::AsyncScrollOperationID> = {});
    Optional<Gfx::IntRect> apply_viewport_scrollbar_drag(ViewportScrollbarController::Drag const&);
    void rebuild_wheel_hit_test_targets();
    bool is_present_blocked() const;
    bool can_render_frame() const;
    Web::Painting::AccumulatedVisualContextTree const& visual_context_tree_for_compositing() const;
    void paint_current_display_list(Web::Painting::DisplayListPlayerSkia&, Gfx::PaintingSurface&, CompositedContextResolver const*);

    CompositorStateWebContentClient& m_web_content_client;
    Web::Painting::CanvasSurfaceRegistry const& m_canvas_surface_registry;
    Optional<u64> m_page_id;
    bool const m_async_scrolling_enabled { true };

    bool m_presents_to_client { false };
    Optional<Web::Compositor::CompositorContextId> m_parent_context_id;

    RefPtr<Web::Painting::DisplayList const> m_display_list;
    Optional<Web::Painting::AccumulatedVisualContextTree> m_visual_context_tree;
    mutable Optional<Web::Painting::AccumulatedVisualContextTree> m_visual_context_tree_for_compositing;
    Web::Painting::DisplayListResourceStorage m_display_list_resource_storage;
    Web::Painting::ScrollStateSnapshot m_scroll_state_snapshot;
    BackingStoreManager m_backing_store_manager;
    RefPtr<Gfx::PaintingSurface> m_latest_rendered_surface;

    Web::Compositor::AsyncScrollTree m_async_scroll_tree;
    ViewportScrollbarController m_viewport_scrollbar_controller;

    Vector<Web::Compositor::AsyncScrollOffset> m_pending_async_scroll_offsets;
    Vector<Web::Compositor::AsyncScrollOperationID> m_completed_async_scroll_operation_ids;
    Web::Compositor::AsyncScrollOperationID m_next_async_scroll_operation_id { 0 };
    Gfx::IntRect m_async_scrolling_viewport_rect;
    bool m_has_async_scrolling_state { false };
    bool m_can_accept_async_wheel_events { false };
    bool m_has_blocking_wheel_event_listeners { false };
    u64 m_wheel_event_listener_state_generation { 0 };
    Web::Compositor::WheelRoutingAdmission m_wheel_routing_admission { Web::Compositor::WheelRoutingAdmission::NoAsyncScrollingState };
    Optional<Web::Painting::TransformData> m_async_visual_viewport_transform;

    Gfx::IntSize m_viewport_size;
    Web::Compositor::WindowResizingInProgress m_window_resize_in_progress { Web::Compositor::WindowResizingInProgress::No };
    RefPtr<Core::Timer> m_backing_store_shrink_timer;
    Optional<u64> m_display_id;
    double m_display_refresh_rate { 60.0 };

    Optional<Gfx::IntRect> m_pending_present_frame;
    bool m_pending_present_frame_scheduled { false };
    Optional<Gfx::IntRect> m_presented_frame;
    Optional<i32> m_gpu_present_bitmap_id_awaiting_completion;
    Optional<i32> m_presented_bitmap_id_awaiting_ack;
};

}
